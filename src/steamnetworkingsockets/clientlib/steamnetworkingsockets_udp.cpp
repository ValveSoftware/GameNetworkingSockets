//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_udp.h"
#include "csteamnetworkingsockets.h"
#include "crypto.h"

#ifdef _WINDOWS
	#include <iphlpapi.h>
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

// Try to guess if the route the specified address is probably "local".
// This is difficult to do in general.  We want something that mostly works.
//
// False positives: VPNs and IPv6 addresses that appear to be nearby but are not.
// False negatives: We can't always tell if a route is local.
bool IsRouteToAddressProbablyLocal( netadr_t addr )
{

	// Assume that if we are able to send to any "reserved" route, that is is local.
	// Note that this will be true for VPNs, too!
	if ( addr.IsReservedAdr() )
		return true;

	// But other cases might also be local routes.  E.g. two boxes with public IPs.
	// Convert to sockaddr struct so we can ask the operating system
	addr.SetPort(0);
	sockaddr_storage sockaddrDest;
	addr.ToSockadr( &sockaddrDest );

	#ifdef _WINDOWS

		//
		// These functions were added with Vista, so load dynamically
		// in case
		//

		typedef
		DWORD
		(WINAPI *FnGetBestInterfaceEx)(
			struct sockaddr *pDestAddr,
			PDWORD           pdwBestIfIndex
			);
		typedef 
		NETIO_STATUS
		(NETIOAPI_API_*FnGetBestRoute2)(
			NET_LUID *InterfaceLuid,
			NET_IFINDEX InterfaceIndex,
			CONST SOCKADDR_INET *SourceAddress,
			CONST SOCKADDR_INET *DestinationAddress,
			ULONG AddressSortOptions,
			PMIB_IPFORWARD_ROW2 BestRoute,
			SOCKADDR_INET *BestSourceAddress
			);

		static HMODULE hModule = LoadLibraryA( "Iphlpapi.dll" );
		static FnGetBestInterfaceEx pGetBestInterfaceEx = hModule ? (FnGetBestInterfaceEx)GetProcAddress( hModule, "GetBestInterfaceEx" ) : nullptr;
		static FnGetBestRoute2 pGetBestRoute2 = hModule ? (FnGetBestRoute2)GetProcAddress( hModule, "GetBestRoute2" ) : nullptr;;
		if ( !pGetBestInterfaceEx || !pGetBestRoute2 )
			return false;

		NET_IFINDEX dwBestIfIndex;
		DWORD r = (*pGetBestInterfaceEx)( (sockaddr *)&sockaddrDest, &dwBestIfIndex );
		if ( r != NO_ERROR )
		{
			AssertMsg2( false, "GetBestInterfaceEx failed with result %d for address '%s'", r, CUtlNetAdrRender( addr ).String() );
			return false;
		}

		MIB_IPFORWARD_ROW2 bestRoute;
		SOCKADDR_INET bestSourceAddress;
		r = (*pGetBestRoute2)(
			nullptr, // InterfaceLuid
			dwBestIfIndex, // InterfaceIndex
			nullptr, // SourceAddress
			(SOCKADDR_INET *)&sockaddrDest, // DestinationAddress
			0, // AddressSortOptions
			&bestRoute, // BestRoute
			&bestSourceAddress // BestSourceAddress
		);
		if ( r != NO_ERROR )
		{
			AssertMsg2( false, "GetBestRoute2 failed with result %d for address '%s'", r, CUtlNetAdrRender( addr ).String() );
			return false;
		}
		if ( bestRoute.Protocol == MIB_IPPROTO_LOCAL )
			return true;
		netadr_t nextHop;
		if ( !nextHop.SetFromSockadr( &bestRoute.NextHop ) )
		{
			AssertMsg( false, "GetBestRoute2 returned invalid next hop address" );
			return false;
		}

		nextHop.SetPort( 0 );

		// https://docs.microsoft.com/en-us/windows/win32/api/netioapi/ns-netioapi-mib_ipforward_row2:
		//   For a remote route, the IP address of the next system or gateway en route.
		//   If the route is to a local loopback address or an IP address on the local
		//   link, the next hop is unspecified (all zeros). For a local loopback route,
		//   this member should be an IPv4 address of 0.0.0.0 for an IPv4 route entry
		//   or an IPv6 address address of 0::0 for an IPv6 route entry.
		if ( !nextHop.HasIP() )
			return true;
		if ( nextHop == addr )
			return true;

		// If final destination is on the same IPv6/56 prefix, then assume
		// it's a local route.  This is an arbitrary prefix size to use,
		// but it's a compromise.  We think that /64 probably has too
		// many false negatives, but /48 has have too many false positives.
		if ( addr.GetType() == k_EIPTypeV6 )
		{
			if ( nextHop.GetType() == k_EIPTypeV6 )
			{
				if ( memcmp( addr.GetIPV6Bytes(), nextHop.GetIPV6Bytes(), 7 ) == 0 )
					return true;
			}
			netadr_t netdrBestSource;
			if ( netdrBestSource.SetFromSockadr( &bestSourceAddress ) && netdrBestSource.GetType() == k_EIPTypeV6 )
			{
				if ( memcmp( addr.GetIPV6Bytes(), netdrBestSource.GetIPV6Bytes(), 7 ) == 0 )
					return true;
			}
		}

	#else
		// FIXME - Writeme
	#endif

	// Nope
	return false;
}

/////////////////////////////////////////////////////////////////////////////
//
// Packet parsing / handling utils
//
/////////////////////////////////////////////////////////////////////////////

bool BCheckRateLimitReportBadPacket( SteamNetworkingMicroseconds usecNow )
{
	static SteamNetworkingMicroseconds s_usecLastReport;
	if ( s_usecLastReport + k_nMillion*2 > usecNow )
		return false;
	s_usecLastReport = usecNow;
	return true;
}

void ReallyReportBadUDPPacket( const char *pszFrom, const char *pszMsgType, const char *pszFmt, ... )
{
	char buf[ 2048 ];
	va_list ap;
	va_start( ap, pszFmt );
	V_vsprintf_safe( buf, pszFmt, ap );
	va_end( ap );
	V_StripTrailingWhitespaceASCII( buf );

	if ( !pszMsgType || !pszMsgType[0] )
		pszMsgType = "message";

	SpewMsg( "[%s] Ignored bad %s.  %s\n", pszMsgType, pszFrom, buf );
}

#define ReportBadPacket( pszMsgType, /* fmt */ ... ) \
	ReportBadUDPPacketFrom( CUtlNetAdrRender( adrFrom ).String(), pszMsgType, __VA_ARGS__ )


#define ParseProtobufBody( pvMsg, cbMsg, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	if ( !msgVar.ParseFromArray( pvMsg, cbMsg ) ) \
	{ \
		ReportBadPacket( # CMsgCls, "Protobuf parse failed." ); \
		return; \
	}

#define ParsePaddedPacket( pvPkt, cbPkt, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	{ \
		if ( cbPkt < k_cbSteamNetworkingMinPaddedPacketSize ) \
		{ \
			ReportBadPacket( # CMsgCls, "Packet is %d bytes, must be padded to at least %d bytes.", cbPkt, k_cbSteamNetworkingMinPaddedPacketSize ); \
			return; \
		} \
		const UDPPaddedMessageHdr *hdr = static_cast< const UDPPaddedMessageHdr * >( pvPkt ); \
		int nMsgLength = LittleWord( hdr->m_nMsgLength ); \
		if ( nMsgLength <= 0 || int(nMsgLength+sizeof(UDPPaddedMessageHdr)) > cbPkt ) \
		{ \
			ReportBadPacket( # CMsgCls, "Invalid encoded message length %d.  Packet is %d bytes.", nMsgLength, cbPkt ); \
			return; \
		} \
		if ( !msgVar.ParseFromArray( hdr+1, nMsgLength ) ) \
		{ \
			ReportBadPacket( # CMsgCls, "Protobuf parse failed." ); \
			return; \
		} \
	}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketDirectUDP
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkListenSocketDirectUDP::CSteamNetworkListenSocketDirectUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkListenSocketBase( pSteamNetworkingSocketsInterface )
{
	m_pSock = nullptr;
}

CSteamNetworkListenSocketDirectUDP::~CSteamNetworkListenSocketDirectUDP()
{
	// Clean up socket, if any
	if ( m_pSock )
	{
		delete m_pSock;
		m_pSock = nullptr;
	}
}

bool CSteamNetworkListenSocketDirectUDP::BInit( const SteamNetworkingIPAddr &localAddr, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	Assert( m_pSock == nullptr );

	if ( localAddr.m_port == 0 )
	{
		V_strcpy_safe( errMsg, "Must specify local port." );
		return false;
	}

	// Set options, add us to the global table
	if ( !BInitListenSocketCommon( nOptions, pOptions, errMsg ) )
		return false;

	// Might we need to authenticate?
	int IP_AllowWithoutAuth = m_connectionConfig.m_IP_AllowWithoutAuth.Get();
	if ( IP_AllowWithoutAuth < 2 )
	{
		m_pSteamNetworkingSocketsInterface->AuthenticationNeeded();

		// If we know for sure that this can't ever work, then go ahead and fail now.
		#ifndef STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT
			if ( IP_AllowWithoutAuth == 0 )
			{
				V_strcpy_safe( errMsg, "No cert authority, must set IP_AllowWithoutAuth" );
				return false;
			}
		#endif
	}

	m_pSock = new CSharedSocket;
	if ( !m_pSock->BInit( localAddr, CRecvPacketCallback( ReceivedFromUnknownHost, this ), errMsg ) )
	{
		delete m_pSock;
		m_pSock = nullptr;
		return false;
	}

	CCrypto::GenerateRandomBlock( m_argbChallengeSecret, sizeof(m_argbChallengeSecret) );

	return true;
}

bool CSteamNetworkListenSocketDirectUDP::APIGetAddress( SteamNetworkingIPAddr *pAddress )
{
	if ( !m_pSock )
	{
		Assert( false );
		return false;
	}

	const SteamNetworkingIPAddr *pBoundAddr = m_pSock->GetBoundAddr();
	if ( !pBoundAddr )
		return false;
	if ( pAddress )
		*pAddress = *pBoundAddr;
	return true;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketUDP packet handling
//
/////////////////////////////////////////////////////////////////////////////

void CSteamNetworkListenSocketDirectUDP::ReceivedFromUnknownHost( const void *pvPkt, int cbPkt, const netadr_t &adrFrom, CSteamNetworkListenSocketDirectUDP *pSock )
{
	const uint8 *pPkt = static_cast<const uint8 *>( pvPkt );

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	if ( cbPkt < 5 )
	{
		ReportBadPacket( "packet", "%d byte packet is too small", cbPkt );
		return;
	}

	if ( *pPkt & 0x80 )
	{
		if ( *(uint32*)pPkt == 0xffffffff )
		{
			// Source engine connectionless packet (LAN discovery, etc).
			// Just ignore it, and don't even spew.
		}
		else
		{
			// A stray data packet.  Just ignore it.
			//
			// When clients are able to actually establish a connection, after that connection
			// is over we will use the FinWait state to close down the connection gracefully.
			// But since we don't have that connection in our table anymore, either this guy
			// never had a connection, or else we believe he knows that the connection was closed,
			// or the FinWait state has timed out.
			ReportBadPacket( "Data", "Stray data packet from host with no connection.  Ignoring." );
		}
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ChallengeRequest )
	{
		ParsePaddedPacket( pvPkt, cbPkt, CMsgSteamSockets_UDP_ChallengeRequest, msg )
		pSock->Received_ChallengeRequest( msg, adrFrom, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectRequest )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_ConnectRequest, msg )
		pSock->Received_ConnectRequest( msg, adrFrom, cbPkt, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectionClosed )
	{
		ParsePaddedPacket( pvPkt, cbPkt, CMsgSteamSockets_UDP_ConnectionClosed, msg )
		pSock->Received_ConnectionClosed( msg, adrFrom, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_NoConnection )
	{
		// They don't think there's a connection on this address.
		// We agree -- connection ID doesn't matter.  Nothing else to do.
	}
	else
	{
		// Any other lead byte is bogus
		//
		// Note in particular that these packet types should be ignored:
		//
		// k_ESteamNetworkingUDPMsg_ChallengeReply
		// k_ESteamNetworkingUDPMsg_ConnectOK
		//
		// We are not initiating connections, so we shouldn't ever get
		// those sorts of replies.

		ReportBadPacket( "packet", "Invalid lead byte 0x%02x", *pPkt );
	}
}

uint64 CSteamNetworkListenSocketDirectUDP::GenerateChallenge( uint16 nTime, const netadr_t &adr ) const
{
	#pragma pack(push,1)
	struct
	{
		uint16 nTime;
		uint16 nPort;
		uint8 ipv6[16];
	} data;
	#pragma pack(pop)
	data.nTime = nTime;
	data.nPort = adr.GetPort();
	adr.GetIPV6( data.ipv6 );
	uint64 nChallenge = siphash( (const uint8_t *)&data, sizeof(data), m_argbChallengeSecret );
	return ( nChallenge & 0xffffffffffff0000ull ) | nTime;
}

inline uint16 GetChallengeTime( SteamNetworkingMicroseconds usecNow )
{
	return uint16( usecNow >> 20 );
}

void CSteamNetworkListenSocketDirectUDP::Received_ChallengeRequest( const CMsgSteamSockets_UDP_ChallengeRequest &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow )
{
	if ( msg.connection_id() == 0 )
	{
		ReportBadPacket( "ChallengeRequest", "Missing connection_id." );
		return;
	}
	//CSteamID steamIDClient( uint64( msg.client_steam_id() ) );
	//if ( !steamIDClient.IsValid() )
	//{
	//	ReportBadPacket( "ChallengeRequest", "Missing/invalid SteamID.", cbPkt );
	//	return;
	//}

	// Get time value of challenge
	uint16 nTime = GetChallengeTime( usecNow );

	// Generate a challenge
	uint64 nChallenge = GenerateChallenge( nTime, adrFrom );

	// Send them a reply
	CMsgSteamSockets_UDP_ChallengeReply msgReply;
	msgReply.set_connection_id( msg.connection_id() );
	msgReply.set_challenge( nChallenge );
	msgReply.set_your_timestamp( msg.my_timestamp() );
	msgReply.set_protocol_version( k_nCurrentProtocolVersion );
	SendMsg( k_ESteamNetworkingUDPMsg_ChallengeReply, msgReply, adrFrom );
}

void CSteamNetworkListenSocketDirectUDP::Received_ConnectRequest( const CMsgSteamSockets_UDP_ConnectRequest &msg, const netadr_t &adrFrom, int cbPkt, SteamNetworkingMicroseconds usecNow )
{
	SteamDatagramErrMsg errMsg;

	// Make sure challenge was generated relatively recently
	uint16 nTimeThen = uint32( msg.challenge() );
	uint16 nElapsed = GetChallengeTime( usecNow ) - nTimeThen;
	if ( nElapsed > GetChallengeTime( 4*k_nMillion ) )
	{
		ReportBadPacket( "ConnectRequest", "Challenge too old." );
		return;
	}

	// Assuming we sent them this time value, re-create the challenge we would have sent them.
	if ( GenerateChallenge( nTimeThen, adrFrom ) != msg.challenge() )
	{
		ReportBadPacket( "ConnectRequest", "Incorrect challenge.  Could be spoofed." );
		return;
	}

	uint32 unClientConnectionID = msg.client_connection_id();
	if ( unClientConnectionID == 0 )
	{
		ReportBadPacket( "ConnectRequest", "Missing connection ID" );
		return;
	}

	// Parse out identity from the cert
	SteamNetworkingIdentity identityRemote;
	bool bIdentityInCert = true;
	{
		// !SPEED! We are deserializing the cert here,
		// and then we are going to do it again below.
		// Should refactor to fix this.
		int r = SteamNetworkingIdentityFromSignedCert( identityRemote, msg.cert(), errMsg );
		if ( r < 0 )
		{
			ReportBadPacket( "ConnectRequest", "Bad identity in cert.  %s", errMsg );
			return;
		}
		if ( r == 0 )
		{
			// No identity in the cert.  Check if they put it directly in the connect message
			bIdentityInCert = false;
			r = SteamNetworkingIdentityFromProtobuf( identityRemote, msg, identity_string, legacy_identity_binary, legacy_client_steam_id, errMsg );
			if ( r < 0 )
			{
				ReportBadPacket( "ConnectRequest", "Bad identity.  %s", errMsg );
				return;
			}
			if ( r == 0 )
			{
				// If no identity was presented, it's the same as them saying they are "localhost"
				identityRemote.SetLocalHost();
			}
		}
	}
	Assert( !identityRemote.IsInvalid() );

	// Check if they are using an IP address as an identity (possibly the anonymous "localhost" identity)
	if ( identityRemote.m_eType == k_ESteamNetworkingIdentityType_IPAddress )
	{
		SteamNetworkingIPAddr addr;
		adrFrom.GetIPV6( addr.m_ipv6 );
		addr.m_port = adrFrom.GetPort();

		if ( identityRemote.IsLocalHost() )
		{
			if ( m_connectionConfig.m_IP_AllowWithoutAuth.Get() == 0 )
			{
				// Should we send an explicit rejection here?
				ReportBadPacket( "ConnectRequest", "Unauthenticated connections not allowed." );
				return;
			}

			// Set their identity to their real address (including port)
			identityRemote.SetIPAddr( addr );
		}
		else
		{
			// FIXME - Should the address be required to match?
			// If we are behind NAT, it won't.
			//if ( memcmp( addr.m_ipv6, identityRemote.m_ip.m_ipv6, sizeof(addr.m_ipv6) ) != 0
			//	|| ( identityRemote.m_ip.m_port != 0 && identityRemote.m_ip.m_port != addr.m_port ) ) // Allow 0 port in the identity to mean "any port"
			//{
			//	ReportBadPacket( "ConnectRequest", "Identity in request is %s, but packet is coming from %s." );
			//	return;
			//}

			// It's not really clear what the use case is here for
			// requesting a specific IP address as your identity,
			// and not using localhost.  If they have a cert, assume it's
			// meaningful.  Remember: the cert could be unsigned!  That
			// is a separate issue which will be handled later, whether
			// we want to allow that.
			if ( !bIdentityInCert )
			{
				// Should we send an explicit rejection here?
				ReportBadPacket( "ConnectRequest", "Cannot use specific IP address." );
				return;
			}
		}
	}

	// Does this connection already exist?  (At a different address?)
	int h = m_mapChildConnections.Find( RemoteConnectionKey_t{ identityRemote, unClientConnectionID } );
	if ( h != m_mapChildConnections.InvalidIndex() )
	{
		CSteamNetworkConnectionBase *pOldConn = m_mapChildConnections[ h ];
		Assert( pOldConn->m_identityRemote == identityRemote );

		// NOTE: We cannot just destroy the object.  The API semantics
		// are that all connections, once accepted and made visible
		// to the API, must be closed by the application.
		ReportBadPacket( "ConnectRequest", "Rejecting connection request from %s at %s, connection ID %u.  That steamID/ConnectionID pair already has a connection [%s]\n",
			SteamNetworkingIdentityRender( identityRemote ).c_str(), CUtlNetAdrRender( adrFrom ).String(), unClientConnectionID, pOldConn->GetDescription()
		);

		CMsgSteamSockets_UDP_ConnectionClosed msgReply;
		msgReply.set_to_connection_id( unClientConnectionID );
		msgReply.set_reason_code( k_ESteamNetConnectionEnd_Misc_Generic );
		msgReply.set_debug( "A connection with that ID already exists." );
		SendPaddedMsg( k_ESteamNetworkingUDPMsg_ConnectionClosed, msgReply, adrFrom );
		return;
	}

	CSteamNetworkConnectionUDP *pConn = new CSteamNetworkConnectionUDP( m_pSteamNetworkingSocketsInterface );

	// OK, they have completed the handshake.  Accept the connection.
	if ( !pConn->BBeginAccept( this, adrFrom, m_pSock, identityRemote, unClientConnectionID, msg.cert(), msg.crypt(), errMsg ) )
	{
		SpewWarning( "Failed to accept connection from %s.  %s\n", CUtlNetAdrRender( adrFrom ).String(), errMsg );
		pConn->ConnectionDestroySelfNow();
		return;
	}

	pConn->m_statsEndToEnd.TrackRecvPacket( cbPkt, usecNow );

	// Did they send us a ping estimate?
	if ( msg.has_ping_est_ms() )
	{
		if ( msg.ping_est_ms() > 1500 )
		{
			SpewWarning( "[%s] Ignoring really large ping estimate %u in connect request", pConn->GetDescription(), msg.has_ping_est_ms() );
		}
		else
		{
			pConn->m_statsEndToEnd.m_ping.ReceivedPing( msg.ping_est_ms(), usecNow );
		}
	}

	// Save of timestamp that we will use to reply to them when the application
	// decides to accept the connection
	if ( msg.has_my_timestamp() )
	{
		pConn->m_ulHandshakeRemoteTimestamp = msg.my_timestamp();
		pConn->m_usecWhenReceivedHandshakeRemoteTimestamp = usecNow;
	}
}

void CSteamNetworkListenSocketDirectUDP::Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow )
{
	// Send an ack.  Note that we require the inbound message to be padded
	// to a minimum size, and this reply is tiny, so we are not at a risk of
	// being used for reflection, even though the source address could be spoofed.
	CMsgSteamSockets_UDP_NoConnection msgReply;
	if ( msg.from_connection_id() )
		msgReply.set_to_connection_id( msg.from_connection_id() );
	if ( msg.to_connection_id() )
		msgReply.set_from_connection_id( msg.to_connection_id() );
	SendMsg( k_ESteamNetworkingUDPMsg_NoConnection, msgReply, adrFrom );
}

void CSteamNetworkListenSocketDirectUDP::SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t &adrTo )
{
	if ( !m_pSock )
	{
		Assert( false );
		return;
	}

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	pkt[0] = nMsgID;
	int cbPkt = ProtoMsgByteSize( msg )+1;
	if ( cbPkt > sizeof(pkt) )
	{
		AssertMsg3( false, "Msg type %d is %d bytes, larger than MTU of %d bytes", int( nMsgID ), int( cbPkt ), (int)sizeof(pkt) );
		return;
	}
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt+1 );
	Assert( cbPkt == pEnd - pkt );

	// Send the reply
	m_pSock->BSendRawPacket( pkt, cbPkt, adrTo );
}

void CSteamNetworkListenSocketDirectUDP::SendPaddedMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t adrTo )
{

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	memset( pkt, 0, sizeof(pkt) ); // don't send random bits from our process memory over the wire!
	UDPPaddedMessageHdr *hdr = (UDPPaddedMessageHdr *)pkt;
	int nMsgLength = ProtoMsgByteSize( msg );
	hdr->m_nMsgID = nMsgID;
	hdr->m_nMsgLength = LittleWord( uint16( nMsgLength ) );
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt + sizeof(*hdr) );
	int cbPkt = pEnd - pkt;
	Assert( cbPkt == int( sizeof(*hdr) + nMsgLength ) );
	cbPkt = MAX( cbPkt, k_cbSteamNetworkingMinPaddedPacketSize );

	m_pSock->BSendRawPacket( pkt, cbPkt, adrTo );
}

/////////////////////////////////////////////////////////////////////////////
//
// CConnectionTransportUDPBase
//
/////////////////////////////////////////////////////////////////////////////

CConnectionTransportUDPBase::CConnectionTransportUDPBase( CSteamNetworkConnectionBase &connection )
: CConnectionTransport( connection )
{
}

bool CConnectionTransportUDPBase::SendDataPacket( SteamNetworkingMicroseconds usecNow )
{
	// Populate context struct with any stats we want/need to send, and how much space we need to reserve for it
	UDPSendPacketContext_t ctx( usecNow, "data" );
	ctx.Populate( sizeof(UDPDataMsgHdr), k_EStatsReplyRequest_NothingToSend, this );

	// Send a packet
	return m_connection.SNP_SendPacket( this, ctx );
}

int CConnectionTransportUDPBase::SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctxBase )
{
	UDPSendPacketContext_t &ctx = static_cast<UDPSendPacketContext_t &>( ctxBase );

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	UDPDataMsgHdr *hdr = (UDPDataMsgHdr *)pkt;
	hdr->m_unMsgFlags = 0x80;
	Assert( m_connection.m_unConnectionIDRemote != 0 );
	hdr->m_unToConnectionID = LittleDWord( m_connection.m_unConnectionIDRemote );
	hdr->m_unSeqNum = LittleWord( m_connection.m_statsEndToEnd.ConsumeSendPacketNumberAndGetWireFmt( ctx.m_usecNow ) );

	byte *p = (byte*)( hdr + 1 );

	// Check how much bigger we could grow the header
	// and still fit in a packet
	int cbHdrOutSpaceRemaining = pkt + sizeof(pkt) - p - cbChunk;
	if ( cbHdrOutSpaceRemaining < 0 )
	{
		AssertMsg( false, "MTU / header size problem!" );
		return 0;
	}

	// Try to trim stuff from blob, if it won't fit
	ctx.Trim( cbHdrOutSpaceRemaining );

	if ( ctx.Serialize( p ) )
	{
		// Update bookkeeping with the stuff we are actually sending
		TrackSentStats( ctx );

		// Mark header with the flag
		hdr->m_unMsgFlags |= hdr->kFlag_ProtobufBlob;
	}

	// !FIXME! Time since previous, for jitter measurement?

	// Use gather-based send.  This saves one memcpy of every payload
	iovec gather[2];
	gather[0].iov_base = pkt;
	gather[0].iov_len = p - pkt;
	gather[1].iov_base = const_cast<void*>( pChunk );
	gather[1].iov_len = cbChunk;

	int cbSend = gather[0].iov_len + gather[1].iov_len;
	Assert( cbSend <= sizeof(pkt) ); // Bug in the code above.  We should never "overflow" the packet.  (Ignoring the fact that we using a gather-based send.  The data could be tiny with a large header for piggy-backed stats.)

	// !FIXME! Should we track data payload separately?  Maybe we ought to track
	// *messages* instead of packets.

	// Send it
	if ( SendPacketGather( 2, gather, cbSend ) )
		return cbSend;
	return 0;
}

std::string DescribeStatsContents( const CMsgSteamSockets_UDP_Stats &msg )
{
	std::string sWhat;
	if ( msg.flags() & msg.ACK_REQUEST_E2E )
		sWhat += " request_ack";
	if ( msg.flags() & msg.ACK_REQUEST_IMMEDIATE )
		sWhat += " request_ack_immediate";
	if ( msg.flags() & msg.NOT_PRIMARY_TRANSPORT_E2E )
		sWhat += " backup_transport";
	if ( msg.stats().has_lifetime() )
		sWhat += " stats.life";
	if ( msg.stats().has_instantaneous() )
		sWhat += " stats.rate";
	return sWhat;
}

void CConnectionTransportUDPBase::RecvStats( const CMsgSteamSockets_UDP_Stats &msgStatsIn, SteamNetworkingMicroseconds usecNow )
{

	// Connection quality stats?
	if ( msgStatsIn.has_stats() )
		m_connection.m_statsEndToEnd.ProcessMessage( msgStatsIn.stats(), usecNow );

	// Spew appropriately
	SpewVerbose( "[%s] Recv UDP stats:%s\n",
		ConnectionDescription(),
		DescribeStatsContents( msgStatsIn ).c_str()
	);

	// Check if we need to reply, either now or later
	if ( m_connection.BStateIsActive() )
	{

		// Check for queuing outgoing acks
		if ( ( msgStatsIn.flags() & msgStatsIn.ACK_REQUEST_E2E ) || msgStatsIn.has_stats() )
		{
			bool bImmediate = ( msgStatsIn.flags() & msgStatsIn.ACK_REQUEST_IMMEDIATE ) != 0;
			m_connection.QueueEndToEndAck( bImmediate, usecNow );

			// Check if need to send an immediately reply, either because they
			// requested it, or because we are not the currently selected transport,
			// and we need to need to make sure the reply goes out using us
			if ( bImmediate || m_connection.m_pTransport != this )
			{
				SendEndToEndStatsMsg( k_EStatsReplyRequest_NothingToSend, usecNow, "AckStats" );
			}
		}
	}
}

void CConnectionTransportUDPBase::TrackSentStats( UDPSendPacketContext_t &ctx )
{

	// What effective flags will be received?
	bool bAllowDelayedReply = ( ctx.msg.flags() & ctx.msg.ACK_REQUEST_IMMEDIATE ) == 0;

	// Record that we sent stats and are waiting for peer to ack
	if ( ctx.msg.has_stats() )
	{
		m_connection.m_statsEndToEnd.TrackSentStats( ctx.msg.stats(), ctx.m_usecNow, bAllowDelayedReply );
	}
	else if ( ctx.msg.flags() & ctx.msg.ACK_REQUEST_E2E )
	{
		m_connection.m_statsEndToEnd.TrackSentMessageExpectingSeqNumAck( ctx.m_usecNow, bAllowDelayedReply );
	}

	// Spew appropriately
	SpewVerbose( "[%s] Sent UDP stats (%s):%s\n",
		ConnectionDescription(),
		ctx.m_pszReason,
		DescribeStatsContents( ctx.msg ).c_str()
	);
}

void CConnectionTransportUDPBase::Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow )
{

	if ( cbPkt < sizeof(UDPDataMsgHdr) )
	{
		ReportBadUDPPacketFromConnectionPeer( "DataPacket", "Packet of size %d is too small.", cbPkt );
		return;
	}

	// Check cookie
	const UDPDataMsgHdr *hdr = (const UDPDataMsgHdr *)pPkt;
	if ( LittleDWord( hdr->m_unToConnectionID ) != ConnectionIDLocal() )
	{

		// Wrong session.  It could be an old session, or it could be spoofed.
		ReportBadUDPPacketFromConnectionPeer( "DataPacket", "Incorrect connection ID" );
		if ( BCheckGlobalSpamReplyRateLimit( usecNow ) )
		{
			SendNoConnection( LittleDWord( hdr->m_unToConnectionID ), 0 );
		}
		return;
	}
	uint16 nWirePktNumber = LittleWord( hdr->m_unSeqNum );

	// Check state
	switch ( ConnectionState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			return;

		case k_ESteamNetworkingConnectionState_Connecting:
			// Ignore it.  We don't have the SteamID of whoever is on the other end yet,
			// their encryption keys, etc.  The most likely cause is that a server sent
			// a ConnectOK, which dropped.  So they think we're connected but we don't
			// have everything yet.
			return;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for raw UDP, but might be used for derived class

			// We'll process the chunk
			break;
	}

	const uint8 *pIn = pPkt + sizeof(*hdr);
	const uint8 *pPktEnd = pPkt + cbPkt;

	// Inline stats?
	static CMsgSteamSockets_UDP_Stats msgStats;
	CMsgSteamSockets_UDP_Stats *pMsgStatsIn = nullptr;
	uint32 cbStatsMsgIn = 0;
	if ( hdr->m_unMsgFlags & hdr->kFlag_ProtobufBlob )
	{
		//Msg_Verbose( "Received inline stats from %s", server.m_szName );

		pIn = DeserializeVarInt( pIn, pPktEnd, cbStatsMsgIn );
		if ( pIn == NULL )
		{
			ReportBadUDPPacketFromConnectionPeer( "DataPacket", "Failed to varint decode size of stats blob" );
			return;
		}
		if ( cbStatsMsgIn > (uint32)(pPktEnd - pIn) )
		{
			ReportBadUDPPacketFromConnectionPeer( "DataPacket", "stats message size doesn't make sense.  Stats message size %u, packet size %d", cbStatsMsgIn, cbPkt );
			return;
		}

		if ( !msgStats.ParseFromArray( pIn, cbStatsMsgIn ) )
		{
			ReportBadUDPPacketFromConnectionPeer( "DataPacket", "protobuf failed to parse inline stats message" );
			return;
		}

		// Shove sequence number so we know what acks to pend, etc
		pMsgStatsIn = &msgStats;

		// Advance pointer
		pIn += cbStatsMsgIn;
	}

	const void *pChunk = pIn;
	int cbChunk = pPktEnd - pIn;

	// Decrypt it, and check packet number
	UDPRecvPacketContext_t ctx;
	ctx.m_usecNow = usecNow;
	ctx.m_pTransport = this;
	ctx.m_pStatsIn = pMsgStatsIn;
	if ( !m_connection.DecryptDataChunk( nWirePktNumber, cbPkt, pChunk, cbChunk, ctx ) )
		return;

	// This is a valid packet.  P2P connections might want to make a note of this
	RecvValidUDPDataPacket( ctx );

	// Process plaintext
	int usecTimeSinceLast = 0; // FIXME - should we plumb this through so we can measure jitter?
	if ( !m_connection.ProcessPlainTextDataChunk( usecTimeSinceLast, ctx ) )
		return;

	// Process the stats, if any
	if ( pMsgStatsIn )
		RecvStats( *pMsgStatsIn, usecNow );
}

void CConnectionTransportUDPBase::RecvValidUDPDataPacket( UDPRecvPacketContext_t &ctx )
{
	// Base class doesn't care
}

void CConnectionTransportUDPBase::SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason )
{
	UDPSendPacketContext_t ctx( usecNow, pszReason );
	ctx.Populate( sizeof(UDPDataMsgHdr), eRequest, this );

	// Send a data packet (maybe containing ordinary data), with this piggy backed on top of it
	m_connection.SNP_SendPacket( this, ctx );
}

void CConnectionTransportUDPBase::SendConnectionClosedOrNoConnection()
{
	if ( ConnectionWireState() == k_ESteamNetworkingConnectionState_ClosedByPeer )
	{
		SendNoConnection( ConnectionIDLocal(), ConnectionIDRemote() );
	}
	else
	{
		CMsgSteamSockets_UDP_ConnectionClosed msg;
		msg.set_from_connection_id( ConnectionIDLocal() );

		if ( ConnectionIDRemote() )
			msg.set_to_connection_id( ConnectionIDRemote() );

		msg.set_reason_code( m_connection.m_eEndReason );
		if ( m_connection.m_szEndDebug[0] )
			msg.set_debug( m_connection.m_szEndDebug );
		SendPaddedMsg( k_ESteamNetworkingUDPMsg_ConnectionClosed, msg );
	}
}

void CConnectionTransportUDPBase::SendNoConnection( uint32 unFromConnectionID, uint32 unToConnectionID )
{
	CMsgSteamSockets_UDP_NoConnection msg;
	if ( unFromConnectionID == 0 && unToConnectionID == 0 )
	{
		AssertMsg( false, "Can't send NoConnection, we need at least one of from/to connection ID!" );
		return;
	}
	if ( unFromConnectionID )
		msg.set_from_connection_id( unFromConnectionID );
	if ( unToConnectionID )
		msg.set_to_connection_id( unToConnectionID );
	SendMsg( k_ESteamNetworkingUDPMsg_NoConnection, msg );
}

void CConnectionTransportUDPBase::SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg )
{

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	pkt[0] = nMsgID;
	int cbPkt = ProtoMsgByteSize( msg )+1;
	if ( cbPkt > sizeof(pkt) )
	{
		AssertMsg3( false, "Msg type %d is %d bytes, larger than MTU of %d bytes", int( nMsgID ), int( cbPkt ), (int)sizeof(pkt) );
		return;
	}
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt+1 );
	Assert( cbPkt == pEnd - pkt );

	SendPacket( pkt, cbPkt );
}

void CConnectionTransportUDPBase::SendPaddedMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg )
{

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	V_memset( pkt, 0, sizeof(pkt) ); // don't send random bits from our process memory over the wire!
	UDPPaddedMessageHdr *hdr = (UDPPaddedMessageHdr *)pkt;
	int nMsgLength = ProtoMsgByteSize( msg );
	if ( nMsgLength + sizeof(*hdr) > k_cbSteamNetworkingSocketsMaxUDPMsgLen )
	{
		AssertMsg3( false, "Msg type %d is %d bytes, larger than MTU of %d bytes", int( nMsgID ), int( nMsgLength + sizeof(*hdr) ), (int)sizeof(pkt) );
		return;
	}
	hdr->m_nMsgID = nMsgID;
	hdr->m_nMsgLength = LittleWord( uint16( nMsgLength ) );
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt + sizeof(*hdr) );
	int cbPkt = pEnd - pkt;
	Assert( cbPkt == int( sizeof(*hdr) + nMsgLength ) );
	cbPkt = MAX( cbPkt, k_cbSteamNetworkingMinPaddedPacketSize );

	SendPacket( pkt, cbPkt );
}

void CConnectionTransportUDPBase::Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, SteamNetworkingMicroseconds usecNow )
{
	// Give them a reply to let them know we heard from them.  If it's the right connection ID,
	// then they probably aren't spoofing and it's critical that we give them an ack!
	//
	// If the wrong connection ID, then it could be an old connection so we'd like to send a reply
	// to let them know that they can stop telling us the connection is closed.
	// However, it could just be random garbage, so we need to protect ourselves from abuse,
	// so limit how many of these we send.
	bool bConnectionIDMatch =
		msg.to_connection_id() == ConnectionIDLocal()
		|| ( msg.to_connection_id() == 0 && msg.from_connection_id() && msg.from_connection_id() == m_connection.m_unConnectionIDRemote ); // they might not know our ID yet, if they are a client aborting the connection really early.
	if ( bConnectionIDMatch || BCheckGlobalSpamReplyRateLimit( usecNow ) )
	{
		// Send a reply, echoing exactly what they sent to us
		CMsgSteamSockets_UDP_NoConnection msgReply;
		if ( msg.to_connection_id() )
			msgReply.set_from_connection_id( msg.to_connection_id() );
		if ( msg.from_connection_id() )
			msgReply.set_to_connection_id( msg.from_connection_id() );
		SendMsg( k_ESteamNetworkingUDPMsg_NoConnection, msgReply );
	}

	// If incorrect connection ID, then that's all we'll do, since this packet actually
	// has nothing to do with current connection at all.
	if ( !bConnectionIDMatch )
		return;

	// Generic connection code will take it from here.
	m_connection.ConnectionState_ClosedByPeer( msg.reason_code(), msg.debug().c_str() );
}

void CConnectionTransportUDPBase::Received_NoConnection( const CMsgSteamSockets_UDP_NoConnection &msg, SteamNetworkingMicroseconds usecNow )
{
	// Make sure it's an ack of something we would have sent
	if ( msg.to_connection_id() != ConnectionIDLocal() || msg.from_connection_id() != m_connection.m_unConnectionIDRemote )
	{
		ReportBadUDPPacketFromConnectionPeer( "NoConnection", "Old/incorrect connection ID.  Message is for a stale connection, or is spoofed.  Ignoring." );
		return;
	}

	// Generic connection code will take it from here.
	// Closure failure code will only be used if this is news.
	// If we closed the connection (the usual case), it
	// will not be used.
	m_connection.ConnectionState_ClosedByPeer( k_ESteamNetConnectionEnd_Misc_PeerSentNoConnection, "Received unexpected 'no connection' from peer");
}

/////////////////////////////////////////////////////////////////////////////
//
// IP connections
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkConnectionUDP::CSteamNetworkConnectionUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkConnectionBase( pSteamNetworkingSocketsInterface )
{
}

CSteamNetworkConnectionUDP::~CSteamNetworkConnectionUDP()
{
}

CConnectionTransportUDP::CConnectionTransportUDP( CSteamNetworkConnectionUDP &connection )
: CConnectionTransportUDPBase( connection )
, m_pSocket( nullptr )
{
}

CConnectionTransportUDP::~CConnectionTransportUDP()
{
	Assert( !m_pSocket ); // Use TransportDestroySelfNow!
}

void CConnectionTransportUDP::TransportFreeResources()
{
	CConnectionTransport::TransportFreeResources();

	if ( m_pSocket )
	{
		m_pSocket->Close();
		m_pSocket = nullptr;
	}
}

void CSteamNetworkConnectionUDP::GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const
{
	char szAddr[ 64 ];
	if ( Transport() && Transport()->m_pSocket )
	{
		SteamNetworkingIPAddr adrRemote;
		NetAdrToSteamNetworkingIPAddr( adrRemote, Transport()->m_pSocket->GetRemoteHostAddr() );
		adrRemote.ToString( szAddr, sizeof(szAddr), true );
		if (
			m_identityRemote.IsLocalHost()
			|| ( m_identityRemote.m_eType == k_ESteamNetworkingIdentityType_IPAddress && adrRemote == m_identityRemote.m_ip )
		) {
			V_sprintf_safe( szDescription, "UDP %s", szAddr );
			return;
		}
	}
	else
	{
		V_strcpy_safe( szAddr, "???" );
	}

	SteamNetworkingIdentityRender sIdentity( m_identityRemote );

	V_sprintf_safe( szDescription, "UDP %s@%s", sIdentity.c_str(), szAddr );
}

void UDPSendPacketContext_t::Populate( size_t cbHdrtReserve, EStatsReplyRequest eReplyRequested, CConnectionTransportUDPBase *pTransport )
{
	CSteamNetworkConnectionBase &connection = pTransport->m_connection;
	LinkStatsTracker<LinkStatsTrackerEndToEnd> &statsEndToEnd = connection.m_statsEndToEnd;

	int nFlags = 0;
	if ( connection.m_pTransport != pTransport )
		nFlags |= msg.NOT_PRIMARY_TRANSPORT_E2E;

	// What effective flags should we send
	int nReadyToSendTracer = 0;
	if ( eReplyRequested == k_EStatsReplyRequest_Immediate || statsEndToEnd.BNeedToSendPingImmediate( m_usecNow ) )
		nFlags |= msg.ACK_REQUEST_E2E | msg.ACK_REQUEST_IMMEDIATE;
	else if ( eReplyRequested == k_EStatsReplyRequest_DelayedOK || statsEndToEnd.BNeedToSendKeepalive( m_usecNow ) )
		nFlags |= msg.ACK_REQUEST_E2E;
	else
	{
		nReadyToSendTracer = statsEndToEnd.ReadyToSendTracerPing( m_usecNow );
		if ( nReadyToSendTracer > 1 )
			nFlags |= msg.ACK_REQUEST_E2E;
	}

	m_nFlags = nFlags;

	// Need to send any connection stats stats?
	m_nStatsNeed = statsEndToEnd.GetStatsSendNeed( m_usecNow );
	if ( m_nStatsNeed & k_nSendStats_Due )
	{
		statsEndToEnd.PopulateMessage( m_nStatsNeed, *msg.mutable_stats(), m_usecNow );

		if ( nReadyToSendTracer > 0 )
			m_nFlags |= msg.ACK_REQUEST_E2E;
	}

	// Populate flags now, based on what is implied from what we HAVE to send
	SlamFlagsAndCalcSize();
	CalcMaxEncryptedPayloadSize( cbHdrtReserve, &connection );

	// Would we like to try to send some additional stats, if there is room?
	if ( m_nStatsNeed & k_nSendStats_Ready )
	{
		if ( nReadyToSendTracer > 0 )
			m_nFlags |= msg.ACK_REQUEST_E2E;
		statsEndToEnd.PopulateMessage( m_nStatsNeed & k_nSendStats_Ready, *msg.mutable_stats(), m_usecNow );
		SlamFlagsAndCalcSize();
	}
}

void UDPSendPacketContext_t::Trim( int cbHdrOutSpaceRemaining )
{
	while ( m_cbTotalSize > cbHdrOutSpaceRemaining )
	{
		if ( !msg.has_stats() )
		{
			// Nothing left to clear!?  We shouldn't get here!
			AssertMsg( false, "Serialized stats message still won't fit, ever after clearing everything?" );
			m_cbTotalSize = 0;
			break;
		}

		if ( m_nStatsNeed & k_nSendStats_Instantanous_Ready )
		{
			msg.mutable_stats()->clear_instantaneous();
			m_nStatsNeed &= ~k_nSendStats_Instantanous_Ready;
		}
		else if ( m_nStatsNeed & k_nSendStats_Lifetime_Ready )
		{
			msg.mutable_stats()->clear_lifetime();
			m_nStatsNeed &= ~k_nSendStats_Lifetime_Ready;
		}
		else
		{
			AssertMsg( false, "We didn't reserve enough space for stats!" );
			if ( m_nStatsNeed & k_nSendStats_Instantanous_Due )
			{
				msg.mutable_stats()->clear_instantaneous();
				m_nStatsNeed &= ~k_nSendStats_Instantanous_Due;
			}
			else
			{
				m_nStatsNeed = 0;
			}
		}

		if ( m_nStatsNeed == 0 )
			msg.clear_stats();

		SlamFlagsAndCalcSize();
	}
}

bool CConnectionTransportUDP::BConnect( const netadr_t &netadrRemote, SteamDatagramErrMsg &errMsg )
{

	// Create an actual OS socket.  We'll bind it to talk only to this host.
	// (Note: we might not actually "bind" it at the OS layer, but from our perpsective
	// it is bound.)
	//
	// For now we're just assuming each connection will gets its own socket,
	// on an ephemeral port.  Later we could add a setting to enable
	// sharing of the socket or binding to a particular local address.
	Assert( !m_pSocket );
	m_pSocket = OpenUDPSocketBoundToHost( netadrRemote, CRecvPacketCallback( PacketReceived, this ), errMsg );
	if ( !m_pSocket )
		return false;
	return true;
}

bool CConnectionTransportUDP::BAccept( CSharedSocket *pSharedSock, const netadr_t &netadrRemote, SteamDatagramErrMsg &errMsg )
{
	// Get an interface that is bound to talk to this address
	m_pSocket = pSharedSock->AddRemoteHost( netadrRemote, CRecvPacketCallback( PacketReceived, this ) );
	if ( !m_pSocket )
	{
		// This is really weird and shouldn't happen
		V_strcpy_safe( errMsg, "Unable to create a bound socket on the shared socket." );
		return false;
	}

	return true;
}

bool CConnectionTransportUDP::CreateLoopbackPair( CConnectionTransportUDP *pTransport[2] )
{
	IBoundUDPSocket *sock[2];
	SteamNetworkingErrMsg errMsg;
	if ( !CreateBoundSocketPair(
		CRecvPacketCallback( PacketReceived, pTransport[0] ),
		CRecvPacketCallback( PacketReceived, pTransport[1] ), sock, errMsg ) )
	{
		// Assert, this really should only fail if we have some sort of bug
		AssertMsg1( false, "Failed to create UDP socket pair.  %s", errMsg );
		return false;
	}

	pTransport[0]->m_pSocket = sock[0];
	pTransport[1]->m_pSocket = sock[1];

	return true;
}

bool CSteamNetworkConnectionUDP::BInitConnect( const SteamNetworkingIPAddr &addressRemote, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	AssertMsg( !m_pTransport, "Trying to connect when we already have a socket?" );

	// We're initiating a connection, not being accepted on a listen socket
	Assert( !m_pParentListenSocket );
	Assert( !m_bConnectionInitiatedRemotely );

	netadr_t netadrRemote;
	SteamNetworkingIPAddrToNetAdr( netadrRemote, addressRemote );

	// We use identity validity to denote when our connection has been accepted,
	// so it's important that it be cleared.  (It should already be so.)
	Assert( m_identityRemote.IsInvalid() );
	m_identityRemote.Clear();

	// We should know our own identity, unless the app has said it's OK to go without this.
	if ( m_identityLocal.IsInvalid() ) // Specific identity hasn't already been set (by derived class, etc)
	{

		// Use identity from the interface, if we have one
		m_identityLocal = m_pSteamNetworkingSocketsInterface->InternalGetIdentity();
		if ( m_identityLocal.IsInvalid())
		{

			// We don't know who we are.  Should we attempt anonymous?
			if ( m_connectionConfig.m_IP_AllowWithoutAuth.Get() == 0 )
			{
				V_strcpy_safe( errMsg, "Unable to determine local identity, and auth required.  Not logged in?" );
				return false;
			}

			m_identityLocal.SetLocalHost();
		}
	}

	// Create transport.
	CConnectionTransportUDP *pTransport = new CConnectionTransportUDP( *this );
	if ( !pTransport->BConnect( netadrRemote, errMsg ) )
	{
		pTransport->TransportDestroySelfNow();
		return false;
	}
	m_pTransport = pTransport;

	// Let base class do some common initialization
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	if ( !CSteamNetworkConnectionBase::BInitConnection( usecNow, nOptions, pOptions, errMsg ) )
	{
		DestroyTransport();
		return false;
	}

	// Start the connection state machine
	return BConnectionState_Connecting( usecNow, errMsg );
}

bool CConnectionTransportUDP::BCanSendEndToEndConnectRequest() const
{
	return m_pSocket != nullptr;
}

bool CConnectionTransportUDP::BCanSendEndToEndData() const
{
	return m_pSocket != nullptr;
}

void CConnectionTransportUDP::SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow )
{
	Assert( !ListenSocket() );
	Assert( !m_connection.m_bConnectionInitiatedRemotely );
	Assert( ConnectionState() == k_ESteamNetworkingConnectionState_Connecting ); // Why else would we be doing this?
	Assert( ConnectionIDLocal() );

	CMsgSteamSockets_UDP_ChallengeRequest msg;
	msg.set_connection_id( ConnectionIDLocal() );
	//msg.set_client_steam_id( m_steamIDLocal.ConvertToUint64() );
	msg.set_my_timestamp( usecNow );
	msg.set_protocol_version( k_nCurrentProtocolVersion );

	// Send it, with padding
	SendPaddedMsg( k_ESteamNetworkingUDPMsg_ChallengeRequest, msg );

	// They are supposed to reply with a timestamps, from which we can estimate the ping.
	// So this counts as a ping request
	m_connection.m_statsEndToEnd.TrackSentPingRequest( usecNow, false );
}

bool CSteamNetworkConnectionUDP::BBeginAccept(
	CSteamNetworkListenSocketDirectUDP *pParent,
	const netadr_t &adrFrom,
	CSharedSocket *pSharedSock,
	const SteamNetworkingIdentity &identityRemote,
	uint32 unConnectionIDRemote,
	const CMsgSteamDatagramCertificateSigned &msgCert,
	const CMsgSteamDatagramSessionCryptInfoSigned &msgCryptSessionInfo,
	SteamDatagramErrMsg &errMsg
)
{
	AssertMsg( !m_pTransport, "Trying to accept when we already have transport?" );

	// Setup transport
	CConnectionTransportUDP *pTransport = new CConnectionTransportUDP( *this );
	if ( !pTransport->BAccept( pSharedSock, adrFrom, errMsg ) )
	{
		pTransport->TransportDestroySelfNow();
		return false;
	}
	m_pTransport = pTransport;

	m_identityRemote = identityRemote;

	// Caller should have ensured a valid identity
	Assert( !m_identityRemote.IsInvalid() );

	m_unConnectionIDRemote = unConnectionIDRemote;
	if ( !pParent->BAddChildConnection( this, errMsg ) )
		return false;

	// Let base class do some common initialization
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	if ( !CSteamNetworkConnectionBase::BInitConnection( usecNow, 0, nullptr, errMsg ) )
	{
		DestroyTransport();
		return false;
	}

	// Process crypto handshake now
	if ( !BRecvCryptoHandshake( msgCert, msgCryptSessionInfo, true ) )
	{
		DestroyTransport();
		Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
		V_sprintf_safe( errMsg, "Failed crypto init.  %s", m_szEndDebug );
		return false;
	}

	// Start the connection state machine
	return BConnectionState_Connecting( usecNow, errMsg );
}

EResult CSteamNetworkConnectionUDP::AcceptConnection( SteamNetworkingMicroseconds usecNow )
{
	if ( !Transport() )
	{
		AssertMsg( false, "Cannot acception UDP connection.  No transport?" );
		return k_EResultFail;
	}

	// Send the message
	Transport()->SendConnectOK( usecNow );

	// We are fully connected
	ConnectionState_Connected( usecNow );

	// OK
	return k_EResultOK;
}

bool CConnectionTransportUDP::SendPacket( const void *pkt, int cbPkt )
{
	iovec temp;
	temp.iov_base = const_cast<void*>( pkt );
	temp.iov_len = cbPkt;
	return SendPacketGather( 1, &temp, cbPkt );
}

bool CConnectionTransportUDP::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )
{
	// Safety
	if ( !m_pSocket )
	{
		AssertMsg( false, "Attemt to send packet, but socket has been closed!" );
		return false;
	}

	// Update stats
	m_connection.m_statsEndToEnd.TrackSentPacket( cbSendTotal );

	// Hand over to operating system
	return m_pSocket->BSendRawPacketGather( nChunks, pChunks );
}

void CConnectionTransportUDP::TransportConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	CConnectionTransport::TransportConnectionStateChanged( eOldState );

	switch ( ConnectionState() )
	{
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for raw UDP
		default:
			Assert( false );
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Dead:
			return;

		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			break;
	}
}

void CConnectionTransportUDP::TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const
{
	CConnectionTransport::TransportPopulateConnectionInfo( info );

	if ( m_pSocket )
	{
		const netadr_t &addr = m_pSocket->GetRemoteHostAddr();
		NetAdrToSteamNetworkingIPAddr( info.m_addrRemote, addr );
		if ( addr.IsLoopback() )
			info.m_eTransportKind = k_ESteamNetTransport_LocalHost;
		else if ( m_connection.m_statsEndToEnd.m_ping.m_nSmoothedPing <= 5 && IsRouteToAddressProbablyLocal( addr ) )
			info.m_eTransportKind = k_ESteamNetTransport_UDPProbablyLocal;
		else
			info.m_eTransportKind = k_ESteamNetTransport_UDP;
	}
}

void CConnectionTransportUDP::PacketReceived( const void *pvPkt, int cbPkt, const netadr_t &adrFrom, CConnectionTransportUDP *pSelf )
{
	const uint8 *pPkt = static_cast<const uint8 *>( pvPkt );

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	if ( cbPkt < 5 )
	{
		ReportBadPacket( "packet", "%d byte packet is too small", cbPkt );
		return;
	}

	// Data packet is the most common, check for it first.  Also, does stat tracking.
	if ( *pPkt & 0x80 )
	{
		pSelf->Received_Data( pPkt, cbPkt, usecNow );
		return;
	}

	// Track stats for other packet types.
	pSelf->m_connection.m_statsEndToEnd.TrackRecvPacket( cbPkt, usecNow );

	if ( *pPkt == k_ESteamNetworkingUDPMsg_ChallengeReply )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_ChallengeReply, msg )
		pSelf->Received_ChallengeReply( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectOK )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_ConnectOK, msg );
		pSelf->Received_ConnectOK( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectionClosed )
	{
		ParsePaddedPacket( pvPkt, cbPkt, CMsgSteamSockets_UDP_ConnectionClosed, msg )
		pSelf->Received_ConnectionClosed( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_NoConnection )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_NoConnection, msg )
		pSelf->Received_NoConnection( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ChallengeRequest )
	{
		ParsePaddedPacket( pvPkt, cbPkt, CMsgSteamSockets_UDP_ChallengeRequest, msg )
		pSelf->Received_ChallengeOrConnectRequest( "ChallengeRequest", msg.connection_id(), usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectRequest )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_ConnectRequest, msg )
		pSelf->Received_ChallengeOrConnectRequest( "ConnectRequest", msg.client_connection_id(), usecNow );
	}
	else
	{
		ReportBadPacket( "packet", "Lead byte 0x%02x not a known message ID", *pPkt );
	}
}

void CConnectionTransportUDP::Received_ChallengeReply( const CMsgSteamSockets_UDP_ChallengeReply &msg, SteamNetworkingMicroseconds usecNow )
{
	// We should only be getting this if we are the "client"
	if ( ListenSocket() )
	{
		ReportBadUDPPacketFromConnectionPeer( "ChallengeReply", "Shouldn't be receiving this unless on accepted connections, only connections initiated locally." );
		return;
	}

	// Ignore if we're not trying to connect
	if ( ConnectionState() != k_ESteamNetworkingConnectionState_Connecting )
		return;

	// Check session ID to make sure they aren't spoofing.
	if ( msg.connection_id() != ConnectionIDLocal() )
	{
		ReportBadUDPPacketFromConnectionPeer( "ChallengeReply", "Incorrect connection ID.  Message is stale or could be spoofed, ignoring." );
		return;
	}
	if ( msg.protocol_version() < k_nMinRequiredProtocolVersion )
	{
		m_connection.ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Generic, "Peer is running old software and needs to be udpated" );
		return;
	}

	// Update ping, if they replied with the timestamp
	if ( msg.has_your_timestamp() )
	{
		SteamNetworkingMicroseconds usecElapsed = usecNow - (SteamNetworkingMicroseconds)msg.your_timestamp();
		if ( usecElapsed < 0 || usecElapsed > 2*k_nMillion )
		{
			SpewWarning( "Ignoring weird timestamp %llu in ChallengeReply, current time is %llu.\n", (unsigned long long)msg.your_timestamp(), usecNow );
		}
		else
		{
			int nPing = (usecElapsed + 500 ) / 1000;
			m_connection.m_statsEndToEnd.m_ping.ReceivedPing( nPing, usecNow );
		}
	}

	// Make sure we have the crypt info that we need
	if ( !m_connection.GetSignedCertLocal().has_cert() || !m_connection.GetSignedCryptLocal().has_info() )
	{
		m_connection.ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Tried to connect request, but crypt not ready" );
		return;
	}

	// Remember protocol version.  They must send it again in the connect OK, but we have a valid value now,
	// so we might as well save it
	m_connection.m_statsEndToEnd.m_nPeerProtocolVersion = msg.protocol_version();

	// Reply with the challenge data and our cert
	CMsgSteamSockets_UDP_ConnectRequest msgConnectRequest;
	msgConnectRequest.set_client_connection_id( ConnectionIDLocal() );
	msgConnectRequest.set_challenge( msg.challenge() );
	msgConnectRequest.set_my_timestamp( usecNow );
	if ( m_connection.m_statsEndToEnd.m_ping.m_nSmoothedPing >= 0 )
		msgConnectRequest.set_ping_est_ms( m_connection.m_statsEndToEnd.m_ping.m_nSmoothedPing );
	*msgConnectRequest.mutable_cert() = m_connection.GetSignedCertLocal();
	*msgConnectRequest.mutable_crypt() = m_connection.GetSignedCryptLocal();

	// If the cert is generic, then we need to specify our identity
	if ( !m_connection.BCertHasIdentity() )
	{
		SteamNetworkingIdentityToProtobuf( IdentityLocal(), msgConnectRequest, identity_string, legacy_identity_binary, legacy_client_steam_id );
	}
	else
	{
		// Identity is in the cert.  But for old peers, set legacy field, if we are a SteamID
		if ( IdentityLocal().GetSteamID64() )
			msgConnectRequest.set_legacy_client_steam_id( IdentityLocal().GetSteamID64() );
	}

	// Send it
	SendMsg( k_ESteamNetworkingUDPMsg_ConnectRequest, msgConnectRequest );

	// Update retry bookkeeping, etc
	m_connection.SentEndToEndConnectRequest( usecNow );

	// They are supposed to reply with a timestamps, from which we can estimate the ping.
	// So this counts as a ping request
	m_connection.m_statsEndToEnd.TrackSentPingRequest( usecNow, false );
}

void CConnectionTransportUDP::Received_ConnectOK( const CMsgSteamSockets_UDP_ConnectOK &msg, SteamNetworkingMicroseconds usecNow )
{
	SteamDatagramErrMsg errMsg;

	// We should only be getting this if we are the "client"
	if ( ListenSocket() )
	{
		ReportBadUDPPacketFromConnectionPeer( "ConnectOK", "Shouldn't be receiving this unless on accepted connections, only connections initiated locally." );
		return;
	}

	// Check connection ID to make sure they aren't spoofing and it's the same connection we think it is
	if ( msg.client_connection_id() != ConnectionIDLocal() )
	{
		ReportBadUDPPacketFromConnectionPeer( "ConnectOK", "Incorrect connection ID.  Message is stale or could be spoofed, ignoring." );
		return;
	}

	// Parse out identity from the cert
	SteamNetworkingIdentity identityRemote;
	bool bIdentityInCert = true;
	{
		// !SPEED! We are deserializing the cert here,
		// and then we are going to do it again below.
		// Should refactor to fix this.
		int r = SteamNetworkingIdentityFromSignedCert( identityRemote, msg.cert(), errMsg );
		if ( r < 0 )
		{
			ReportBadUDPPacketFromConnectionPeer( "ConnectRequest", "Bad identity in cert.  %s", errMsg );
			return;
		}
		if ( r == 0 )
		{
			// No identity in the cert.  Check if they put it directly in the connect message
			bIdentityInCert = false;
			r = SteamNetworkingIdentityFromProtobuf( identityRemote, msg, identity_string, legacy_identity_binary, legacy_server_steam_id, errMsg );
			if ( r < 0 )
			{
				ReportBadUDPPacketFromConnectionPeer( "ConnectRequest", "Bad identity.  %s", errMsg );
				return;
			}
			if ( r == 0 )
			{
				// If no identity was presented, it's the same as them saying they are "localhost"
				identityRemote.SetLocalHost();
			}
		}
	}
	Assert( !identityRemote.IsInvalid() );

	// Check if they are using an IP address as an identity (possibly the anonymous "localhost" identity)
	if ( identityRemote.m_eType == k_ESteamNetworkingIdentityType_IPAddress )
	{
		SteamNetworkingIPAddr addr;
		const netadr_t &adrFrom = m_pSocket->GetRemoteHostAddr();
		adrFrom.GetIPV6( addr.m_ipv6 );
		addr.m_port = adrFrom.GetPort();

		if ( identityRemote.IsLocalHost() )
		{
			if ( m_connection.m_connectionConfig.m_IP_AllowWithoutAuth.Get() == 0 )
			{
				// Should we send an explicit rejection here?
				ReportBadUDPPacketFromConnectionPeer( "ConnectOK", "Unauthenticated connections not allowed." );
				return;
			}

			// Set their identity to their real address (including port)
			identityRemote.SetIPAddr( addr );
		}
		else
		{
			// FIXME - Should the address be required to match?
			// If we are behind NAT, it won't.
			//if ( memcmp( addr.m_ipv6, identityRemote.m_ip.m_ipv6, sizeof(addr.m_ipv6) ) != 0
			//	|| ( identityRemote.m_ip.m_port != 0 && identityRemote.m_ip.m_port != addr.m_port ) ) // Allow 0 port in the identity to mean "any port"
			//{
			//	ReportBadPacket( "ConnectRequest", "Identity in request is %s, but packet is coming from %s." );
			//	return;
			//}

			// It's not really clear what the use case is here for
			// requesting a specific IP address as your identity,
			// and not using localhost.  If they have a cert, assume it's
			// meaningful.  Remember: the cert could be unsigned!  That
			// is a separate issue which will be handled later, whether
			// we want to allow that.
			if ( !bIdentityInCert )
			{
				// Should we send an explicit rejection here?
				ReportBadPacket( "ConnectOK", "Cannot use specific IP address." );
				return;
			}
		}
	}

	// Make sure they are still who we think they are
	if ( !m_connection.m_identityRemote.IsInvalid() && !( m_connection.m_identityRemote == identityRemote ) )
	{
		ReportBadUDPPacketFromConnectionPeer( "ConnectOK", "server_steam_id doesn't match who we expect to be connecting to!" );
		return;
	}

	// Update ping, if they replied a timestamp
	if ( msg.has_your_timestamp() )
	{
		SteamNetworkingMicroseconds usecElapsed = usecNow - (SteamNetworkingMicroseconds)msg.your_timestamp() - msg.delay_time_usec();
		if ( usecElapsed < 0 || usecElapsed > 2*k_nMillion )
		{
			SpewWarning( "Ignoring weird timestamp %llu in ConnectOK, current time is %llu, remote delay was %lld.\n", (unsigned long long)msg.your_timestamp(), usecNow, (long long)msg.delay_time_usec() );
		}
		else
		{
			int nPing = (usecElapsed + 500 ) / 1000;
			m_connection.m_statsEndToEnd.m_ping.ReceivedPing( nPing, usecNow );
		}
	}

	// Check state
	switch ( ConnectionState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for raw UDP
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			return;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connected:
			// We already know we were able to establish the connection.
			// Just ignore this packet
			return;

		case k_ESteamNetworkingConnectionState_Connecting:
			break;
	}

	// Connection ID
	m_connection.m_unConnectionIDRemote = msg.server_connection_id();
	if ( ( m_connection.m_unConnectionIDRemote & 0xffff ) == 0 )
	{
		m_connection.ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Didn't send valid connection ID" );
		return;
	}

	m_connection.m_identityRemote = identityRemote;

	// Check the certs, save keys, etc
	if ( !m_connection.BRecvCryptoHandshake( msg.cert(), msg.crypt(), false ) )
	{
		Assert( ConnectionState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
		ReportBadUDPPacketFromConnectionPeer( "ConnectOK", "Failed crypto init.  %s", m_connection.m_szEndDebug );
		return;
	}

	// Generic connection code will take it from here.
	m_connection.ConnectionState_Connected( usecNow );
}

void CConnectionTransportUDP::Received_ChallengeOrConnectRequest( const char *pszDebugPacketType, uint32 unPacketConnectionID, SteamNetworkingMicroseconds usecNow )
{
	// If wrong connection ID, then check for sending a generic reply and bail
	if ( unPacketConnectionID != m_connection.m_unConnectionIDRemote )
	{
		ReportBadUDPPacketFromConnectionPeer( pszDebugPacketType, "Incorrect connection ID, when we do have a connection for this address.  Could be spoofed, ignoring." );
		// Let's not send a reply in this case
		//if ( BCheckGlobalSpamReplyRateLimit( usecNow ) )
		//	SendNoConnection( unPacketConnectionID );
		return;
	}

	// Check state
	switch ( ConnectionState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for raw UDP
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			return;

		case k_ESteamNetworkingConnectionState_Connecting:
			// We're waiting on the application.  So we'll just have to ignore.
			break;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connected:
			if ( !ListenSocket() )
			{
				// WAT?  We initiated this connection, so why are they requesting to connect?
				ReportBadUDPPacketFromConnectionPeer( pszDebugPacketType, "We are the 'client' who initiated the connection, so 'server' shouldn't be sending us this!" );
				return;
			}

			// This is totally legit and possible.  Our earlier reply might have dropped, and they are re-sending
			SendConnectOK( usecNow );
			return;
	}

}

void CConnectionTransportUDP::SendConnectOK( SteamNetworkingMicroseconds usecNow )
{
	Assert( ConnectionIDLocal() );
	Assert( ConnectionIDRemote() );
	Assert( ListenSocket() );

	Assert( m_connection.GetSignedCertLocal().has_cert() );
	Assert( m_connection.GetSignedCryptLocal().has_info() );

	CMsgSteamSockets_UDP_ConnectOK msg;
	msg.set_client_connection_id( ConnectionIDRemote() );
	msg.set_server_connection_id( ConnectionIDLocal() );
	*msg.mutable_cert() = m_connection.GetSignedCertLocal();
	*msg.mutable_crypt() = m_connection.GetSignedCryptLocal();

	// If the cert is generic, then we need to specify our identity
	if ( !m_connection.BCertHasIdentity() )
	{
		SteamNetworkingIdentityToProtobuf( IdentityLocal(), msg, identity_string, legacy_identity_binary, legacy_server_steam_id );
	}
	else
	{
		// Identity is in the cert.  But for old peers, set legacy field, if we are a SteamID
		if ( IdentityLocal().GetSteamID64() )
			msg.set_legacy_server_steam_id( IdentityLocal().GetSteamID64() );
	}

	// Do we have a timestamp?
	if ( m_connection.m_usecWhenReceivedHandshakeRemoteTimestamp )
	{
		SteamNetworkingMicroseconds usecElapsed = usecNow - m_connection.m_usecWhenReceivedHandshakeRemoteTimestamp;
		Assert( usecElapsed >= 0 );
		if ( usecElapsed < 4*k_nMillion )
		{
			msg.set_your_timestamp( m_connection.m_ulHandshakeRemoteTimestamp );
			msg.set_delay_time_usec( usecElapsed );
		}
		else
		{
			SpewWarning( "Discarding handshake timestamp that's %lldms old, not sending in ConnectOK\n", usecElapsed/1000 );
			m_connection.m_usecWhenReceivedHandshakeRemoteTimestamp = 0;
		}
	}


	// Send it, with padding
	SendMsg( k_ESteamNetworkingUDPMsg_ConnectOK, msg );
}

EUnsignedCert CSteamNetworkConnectionUDP::AllowRemoteUnsignedCert()
{
	// NOTE: No special override for localhost.
	// Should we add a seperate convar for this?
	// For the CSteamNetworkConnectionlocalhostLoopback connection,
	// we know both ends are us.  but if they are just connecting to
	// 127.0.0.1, it's not clear that we should handle this any
	// differently from any other connection

	// Enabled by convar?
	int nAllow = m_connectionConfig.m_IP_AllowWithoutAuth.Get();
	if ( nAllow > 1 )
		return k_EUnsignedCert_Allow;
	if ( nAllow == 1 )
		return k_EUnsignedCert_AllowWarn;

	// Lock it down
	return k_EUnsignedCert_Disallow;
}

EUnsignedCert CSteamNetworkConnectionUDP::AllowLocalUnsignedCert()
{
	// Same logic actually applies for remote and local
	return AllowRemoteUnsignedCert();
}

/////////////////////////////////////////////////////////////////////////////
//
// Loopback connections
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkConnectionlocalhostLoopback::CSteamNetworkConnectionlocalhostLoopback( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, const SteamNetworkingIdentity &identity )
: CSteamNetworkConnectionUDP( pSteamNetworkingSocketsInterface )
{
	m_identityLocal = identity;
}

EUnsignedCert CSteamNetworkConnectionlocalhostLoopback::AllowRemoteUnsignedCert()
{
	return k_EUnsignedCert_Allow;
}

EUnsignedCert CSteamNetworkConnectionlocalhostLoopback::AllowLocalUnsignedCert()
{
	return k_EUnsignedCert_Allow;
}

bool CSteamNetworkConnectionlocalhostLoopback::APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionlocalhostLoopback *pConn[2], const SteamNetworkingIdentity pIdentity[2] )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	SteamDatagramErrMsg errMsg;

	pConn[1] = new CSteamNetworkConnectionlocalhostLoopback( pSteamNetworkingSocketsInterface, pIdentity[0] );
	pConn[0] = new CSteamNetworkConnectionlocalhostLoopback( pSteamNetworkingSocketsInterface, pIdentity[1] );
	if ( !pConn[0] || !pConn[1] )
	{
failed:
		pConn[0]->ConnectionDestroySelfNow(); pConn[0] = nullptr;
		pConn[1]->ConnectionDestroySelfNow(); pConn[1] = nullptr;
		return false;
	}

	// Don't post any state changes for these transitions.  We just want to immediately start in the
	// connected state
	++pConn[0]->m_nSupressStateChangeCallbacks;
	++pConn[1]->m_nSupressStateChangeCallbacks;

	CConnectionTransportUDP *pTransport[2] = {
		new CConnectionTransportUDP( *pConn[0] ),
		new CConnectionTransportUDP( *pConn[1] )
	};
	pConn[0]->m_pTransport = pTransport[0];
	pConn[1]->m_pTransport = pTransport[1];

	if ( !CConnectionTransportUDP::CreateLoopbackPair( pTransport ) )
		goto failed;

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Initialize both connections
	for ( int i = 0 ; i < 2 ; ++i )
	{
		if ( !pConn[i]->BInitConnection( usecNow, 0, nullptr, errMsg ) )
		{
			AssertMsg1( false, "CSteamNetworkConnectionlocalhostLoopback::BInitConnection failed.  %s", errMsg );
			goto failed;
		}
	}

	// Tie the connections to each other, and mark them as connected
	for ( int i = 0 ; i < 2 ; ++i )
	{
		CSteamNetworkConnectionlocalhostLoopback *p = pConn[i];
		CSteamNetworkConnectionlocalhostLoopback *q = pConn[1-i];
		p->m_identityRemote = q->m_identityLocal;
		p->m_unConnectionIDRemote = q->m_unConnectionIDLocal;
		p->m_statsEndToEnd.m_usecTimeLastRecv = usecNow; // Act like we just now received something
		if ( !p->BRecvCryptoHandshake( q->m_msgSignedCertLocal, q->m_msgSignedCryptLocal, i==0 ) )
		{
			AssertMsg( false, "BRecvCryptoHandshake failed creating localhost socket pair" );
			goto failed;
		}
		if ( !p->BConnectionState_Connecting( usecNow, errMsg ) )
		{
			AssertMsg( false, "BConnectionState_Connecting failed creating loopback pipe socket pair.  %s", errMsg );
			goto failed;
		}
		p->ConnectionState_Connected( usecNow );
	}

	// Any further state changes are legit
	pConn[0]->m_nSupressStateChangeCallbacks = 0;
	pConn[1]->m_nSupressStateChangeCallbacks = 0;

	return true;
}

} // namespace SteamNetworkingSocketsLib
