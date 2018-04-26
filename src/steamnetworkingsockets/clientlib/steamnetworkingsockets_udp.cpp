//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_udp.h"
#include "steamnetworkingconfig.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

const int k_cbSteamNetworkingMinPaddedPacketSize = 512;

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

#pragma pack( push, 1 )

/// A protobuf-encoded message that is padded to ensure a minimum length
struct UDPPaddedMessageHdr
{
	uint8 m_nMsgID;
	uint16 m_nMsgLength;
};

struct UDPDataMsgHdr
{
	enum
	{
		kFlag_ProtobufBlob  = 0x01, // Protobuf-encoded message is inline (CMsgSteamSockets_UDP_Stats)
	};

	uint8 m_unMsgFlags;
	uint32 m_unToConnectionID; // Recipient's portion of the connection ID
	uint16 m_unSeqNum;

	// [optional, if flags&kFlag_ProtobufBlob]  varint-encoded protobuf blob size, followed by blob
	// Data frame(s)
	// End of packet
};
#pragma pack( pop )

//
// FIXME - legacy stuff.  A very early version of the framing
//         within the end-to-end data payload.
//

const uint8 k_nDataFrameFlags_Size_Mask         = 0xc0;
const uint8 k_nDataFrameFlags_Size_RestOfPacket = 0x00;
//const uint8 k_nDataFrameFlags_Size_8bits        = 0x40;
//const uint8 k_nDataFrameFlags_Size_8bitsAdd256  = 0x80;
//const uint8 k_nDataFrameFlags_Size_16bits       = 0xc0;

const uint8 k_nDataFrameFlags_FrameType_Mask        = 0x3c;
const uint8 k_nDataFrameFlags_FrameType_Unreliable  = 0x00;
//const uint8 k_nDataFrameFlags_FrameType_Reliable    = 0x10;
//const uint8 k_nDataFrameFlags_FrameType_ReliableAck = 0x20;

const uint8 k_nDataFrameFlags_Channel_Mask        = 0x03;
const uint8 k_nDataFrameFlags_Channel_Same        = 0x00;
const uint8 k_nDataFrameFlags_Channel_8bit        = 0x02;
//const uint8 k_nDataFrameFlags_Channel_16bit       = 0x03;

const int k_nMaxRecentLocalConnectionIDs = 256;
static CUtlVectorFixed<uint16,k_nMaxRecentLocalConnectionIDs> s_vecRecentLocalConnectionIDs;

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

void ReallyReportBadPacket( const netadr_t &adrFrom, const char *pszMsgType, const char *pszFmt, ... )
{
	char buf[ 2048 ];
	va_list ap;
	va_start( ap, pszFmt );
	V_vsprintf_safe( buf, pszFmt, ap );
	va_end( ap );
	V_StripTrailingWhitespaceASCII( buf );

	if ( !pszMsgType || !pszMsgType[0] )
		pszMsgType = "message";

	SpewMsg( "Ignored bad %s from %s.  %s\n", pszMsgType, CUtlNetAdrRender( adrFrom ).String(), buf );
}

#define ReportBadPacketFrom( adrFrom, pszMsgType, /* fmt */ ... ) \
	( BCheckRateLimitReportBadPacket( usecNow ) ? ReallyReportBadPacket( adrFrom, pszMsgType, __VA_ARGS__ ) : (void)0 )

#define ReportBadPacket( pszMsgType, /* fmt */ ... ) \
	ReportBadPacketFrom( adrFrom, pszMsgType, __VA_ARGS__ )


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
// CSteamNetworkListenSocketStandard - basic IPv4 packet handling
//
/////////////////////////////////////////////////////////////////////////////

void CSteamNetworkListenSocketStandard::ReceivedIPv4FromUnknownHost( const void *pvPkt, int cbPkt, const netadr_t &adrFrom, CSteamNetworkListenSocketStandard *pSock )
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
		pSock->ReceivedIPv4_ChallengeRequest( msg, adrFrom, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectRequest )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_ConnectRequest, msg )
		pSock->ReceivedIPv4_ConnectRequest( msg, adrFrom, cbPkt, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectionClosed )
	{
		ParsePaddedPacket( pvPkt, cbPkt, CMsgSteamSockets_UDP_ConnectionClosed, msg )
		pSock->ReceivedIPv4_ConnectionClosed( msg, adrFrom, usecNow );
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

uint64 CSteamNetworkListenSocketStandard::GenerateChallenge( uint16 nTime, uint32 nIP ) const
{
	uint32 data[2] = { nTime, nIP };
	uint64 nChallenge = siphash( (const uint8_t *)data, sizeof(data), m_argbChallengeSecret );
	return ( nChallenge & 0xffffffffffff0000ull ) | nTime;
}

inline uint16 GetChallengeTime( SteamNetworkingMicroseconds usecNow )
{
	return uint16( usecNow >> 20 );
}

void CSteamNetworkListenSocketStandard::ReceivedIPv4_ChallengeRequest( const CMsgSteamSockets_UDP_ChallengeRequest &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow )
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
	uint64 nChallenge = GenerateChallenge( nTime, adrFrom.GetIP() );

	// Send them a reply
	CMsgSteamSockets_UDP_ChallengeReply msgReply;
	msgReply.set_connection_id( msg.connection_id() );
	msgReply.set_challenge( nChallenge );
	msgReply.set_your_timestamp( msg.my_timestamp() );
	msgReply.set_protocol_version( k_nCurrentProtocolVersion );
	SendMsgIPv4( k_ESteamNetworkingUDPMsg_ChallengeReply, msgReply, adrFrom );
}

void CSteamNetworkListenSocketStandard::ReceivedIPv4_ConnectRequest( const CMsgSteamSockets_UDP_ConnectRequest &msg, const netadr_t &adrFrom, int cbPkt, SteamNetworkingMicroseconds usecNow )
{

	// Make sure challenge was generated relatively recently
	uint16 nTimeThen = uint32( msg.challenge() );
	uint16 nElapsed = GetChallengeTime( usecNow ) - nTimeThen;
	if ( nElapsed > GetChallengeTime( 4*k_nMillion ) )
	{
		ReportBadPacket( "ConnectRequest", "Challenge too old." );
		return;
	}

	// Assuming we sent them this time value, re-create the challenge we would have sent them.
	if ( GenerateChallenge( nTimeThen, adrFrom.GetIP() ) != msg.challenge() )
	{
		ReportBadPacket( "ConnectRequest", "Incorrect challenge.  Could be spoofed." );
		return;
	}

	// Check that the Steam ID is valid.  We'll authenticate this using their cert later
	CSteamID steamID( uint64( msg.client_steam_id() ) );
	if ( !steamID.IsValid() && steamdatagram_ip_allow_connections_without_auth == 0 )
	{
		ReportBadPacket( "ConnectRequest", "Invalid SteamID %llu.", steamID.ConvertToUint64() );
		return;
	}
	uint32 unClientConnectionID = msg.client_connection_id();
	if ( unClientConnectionID == 0 )
	{
		ReportBadPacket( "ConnectRequest", "Missing connection ID" );
		return;
	}

	// Does this connection already exist?  (At a different address?)
	int h = m_mapChildConnections.Find( ChildConnectionKey_t( steamID, unClientConnectionID ) );
	if ( h != m_mapChildConnections.InvalidIndex() )
	{
		CSteamNetworkConnectionBase *pOldConn = m_mapChildConnections[ h ];
		Assert( pOldConn->m_steamIDRemote == steamID );
		Assert( pOldConn->GetRemoteAddr() != adrFrom ); // or else why didn't we already map it directly to them!

		// NOTE: We cannot just destroy the object.  The API semantics
		// are that all connections, once accepted and made visible
		// to the API, must be closed by the application.
		ReportBadPacket( "ConnectRequest", "Rejecting connection request from %s at %s, connection ID %u.  That steamID/ConnectionID pair already has a connection from %s\n",
			steamID.Render(), CUtlNetAdrRender( adrFrom ).String(), unClientConnectionID, CUtlNetAdrRender( pOldConn->GetRemoteAddr() ).String()
		);

		CMsgSteamSockets_UDP_ConnectionClosed msgReply;
		msgReply.set_to_connection_id( unClientConnectionID );
		msgReply.set_reason_code( k_ESteamNetConnectionEnd_Misc_Generic );
		msgReply.set_debug( "A connection with that ID already exists." );
		SendPaddedMsgIPv4( k_ESteamNetworkingUDPMsg_ConnectionClosed, msgReply, adrFrom );
	}

	CSteamNetworkConnectionIPv4 *pConn = new CSteamNetworkConnectionIPv4( m_pSteamNetworkingSocketsInterface );

	// OK, they have completed the handshake.  Accept the connection.
	uint32 nPeerProtocolVersion = msg.has_protocol_version() ? msg.protocol_version() : 1;
	SteamDatagramErrMsg errMsg;
	if ( !pConn->BBeginAccept( this, adrFrom, m_pSockIPV4Connections, steamID, unClientConnectionID, nPeerProtocolVersion, msg.cert(), msg.crypt(), errMsg ) )
	{
		SpewWarning( "Failed to accept connection from %s.  %s\n", CUtlNetAdrRender( adrFrom ).String(), errMsg );
		pConn->Destroy();
		return;
	}

	pConn->m_statsEndToEnd.TrackRecvPacket( cbPkt, usecNow );

	// Did they send us a ping estimate?
	if ( msg.has_ping_est_ms() )
		pConn->m_statsEndToEnd.m_ping.ReceivedPing( msg.ping_est_ms(), usecNow );

	// Save of timestamp that we will use to reply to them when the application
	// decides to accept the connection
	if ( msg.has_my_timestamp() )
	{
		pConn->m_ulHandshakeRemoteTimestamp = msg.my_timestamp();
		pConn->m_usecWhenReceivedHandshakeRemoteTimestamp = usecNow;
	}
}

void CSteamNetworkListenSocketStandard::ReceivedIPv4_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow )
{
	// Send an ack.  Note that we require the inbound message to be padded
	// to a minimum size, and this reply is tiny, so we are not at a risk of
	// being used for reflection, even though the source address could be spoofed.
	CMsgSteamSockets_UDP_NoConnection msgReply;
	if ( msg.from_connection_id() )
		msgReply.set_to_connection_id( msg.from_connection_id() );
	if ( msg.to_connection_id() )
		msgReply.set_from_connection_id( msg.to_connection_id() );
	SendMsgIPv4( k_ESteamNetworkingUDPMsg_NoConnection, msgReply, adrFrom );
}

void CSteamNetworkListenSocketStandard::SendMsgIPv4( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t &adrTo )
{
	if ( !m_pSockIPV4Connections )
	{
		Assert( false );
		return;
	}

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	pkt[0] = nMsgID;
	int cbPkt = msg.ByteSize()+1;
	if ( cbPkt > sizeof(pkt) )
	{
		AssertMsg3( false, "Msg type %d is %d bytes, larger than MTU of %d bytes", int( nMsgID ), cbPkt, (int)sizeof(pkt) );
		return;
	}
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt+1 );
	Assert( cbPkt == pEnd - pkt );

	// Send the reply
	m_pSockIPV4Connections->BSendRawPacket( pkt, cbPkt, adrTo );
}

void CSteamNetworkListenSocketStandard::SendPaddedMsgIPv4( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t adrTo )
{

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	memset( pkt, 0, sizeof(pkt) ); // don't send random bits from our process memory over the wire!
	UDPPaddedMessageHdr *hdr = (UDPPaddedMessageHdr *)pkt;
	int nMsgLength = msg.ByteSize();
	hdr->m_nMsgID = nMsgID;
	hdr->m_nMsgLength = LittleWord( uint16( nMsgLength ) );
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt + sizeof(*hdr) );
	int cbPkt = pEnd - pkt;
	Assert( cbPkt == int( sizeof(*hdr) + nMsgLength ) );
	cbPkt = MAX( cbPkt, k_cbSteamNetworkingMinPaddedPacketSize );

	m_pSockIPV4Connections->BSendRawPacket( pkt, cbPkt, adrTo );
}

/////////////////////////////////////////////////////////////////////////////
//
// IP connections
//
/////////////////////////////////////////////////////////////////////////////

struct IPv4InlineStatsContext_t
{
	CMsgSteamSockets_UDP_Stats msg;
	int cbSize;
	//char data[ k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend ];
};

CSteamNetworkConnectionIPv4::CSteamNetworkConnectionIPv4( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkConnectionBase( pSteamNetworkingSocketsInterface )
{
	m_pSocket = nullptr;
}

CSteamNetworkConnectionIPv4::~CSteamNetworkConnectionIPv4()
{
	AssertMsg( !m_pSocket, "Connection not destroyed properly" );
}

void CSteamNetworkConnectionIPv4::FreeResources()
{
	if ( m_pSocket )
	{
		m_pSocket->Close();
		m_pSocket = nullptr;
	}

	// Base class cleanup
	CSteamNetworkConnectionBase::FreeResources();
}

int CSteamNetworkConnectionIPv4::SendEncryptedDataChunk( const void *pChunk, int cbChunk, SteamNetworkingMicroseconds usecNow, void *pConnectionContext )
{
	if ( !m_pSocket )
	{
		Assert( false );
		return 0;
	}

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	UDPDataMsgHdr *hdr = (UDPDataMsgHdr *)pkt;
	hdr->m_unMsgFlags = 0x80;
	Assert( m_unConnectionIDRemote != 0 );
	hdr->m_unToConnectionID = LittleDWord( m_unConnectionIDRemote );
	hdr->m_unSeqNum = LittleWord( m_statsEndToEnd.GetNextSendSequenceNumber( usecNow ) );

	byte *p = (byte*)( hdr + 1 );

	// Check how much bigger we could grow the header
	// and still fit in a packet
	int cbHdrOutSpaceRemaining = pkt + sizeof(pkt) - p - cbChunk;
	if ( cbHdrOutSpaceRemaining < 0 )
	{
		AssertMsg( false, "MTU / header size problem!" );
		return 0;
	}

	// Assume no inline blob
	CMsgSteamSockets_UDP_Stats *pStatsMsg = nullptr;
	int cbStatsMsg = 0;
	int cbHdrSpaceNeeded = 0;

	// Did we initiate this packet?
	if ( pConnectionContext )
	{
		IPv4InlineStatsContext_t &context = *(IPv4InlineStatsContext_t *)pConnectionContext;
		pStatsMsg = &context.msg;
		cbStatsMsg = context.cbSize;
		cbHdrSpaceNeeded = cbStatsMsg + 1;
		if ( cbStatsMsg >= 0x80 )
			++cbHdrSpaceNeeded;
		if ( cbHdrOutSpaceRemaining < cbHdrSpaceNeeded )
		{
			AssertMsg( false, "We didn't make enough room for CMsgSteamSockets_UDP_Stats!" );
			return 0;
		}
	}
	else if ( cbHdrOutSpaceRemaining >= 4 )
	{

		// We didn't specifically request any inline stats, this is an ordinary data packet.
		// Do we need to send tracer ping request or connection stats?

		// What sorts of ack should we request?
		uint32 nFlags = 0;
		if ( m_statsEndToEnd.BNeedToSendPingImmediate( usecNow ) )
		{
			// Connection problem.  Ping aggressively until we figure it out
			nFlags = CMsgSteamSockets_UDP_Stats::ACK_REQUEST_E2E | CMsgSteamSockets_UDP_Stats::ACK_REQUEST_IMMEDIATE;
		}
		else if ( m_statsEndToEnd.BReadyToSendTracerPing( usecNow ) || m_statsEndToEnd.BNeedToSendKeepalive( usecNow ) )
			nFlags |= CMsgSteamSockets_UDP_Stats::ACK_REQUEST_E2E;

		// Check if we should send connection stats inline.
		bool bTrySendEndToEndStats = m_statsEndToEnd.BReadyToSendStats( usecNow );

		// Do we actually want to send anything in the protobuf blob at all?
		// The goal is that we should only do this every couple of seconds or so.
		if ( bTrySendEndToEndStats || nFlags != 0 || m_statsEndToEnd.m_nPendingOutgoingAcks > 0 )
		{
			// Populate a message with everything we'd like to send
			static CMsgSteamSockets_UDP_Stats msgStatsOut;
			pStatsMsg = &msgStatsOut;
			msgStatsOut.Clear();
			if ( bTrySendEndToEndStats )
				m_statsEndToEnd.PopulateMessage( *msgStatsOut.mutable_stats(), usecNow );

			// Acks
			// FIXME - Eventually we'd like to put a single ack in the header, since these will
			// be reasonably common
			PutEndToEndAcksIntoMessage( msgStatsOut, m_statsEndToEnd, usecNow );

			// We'll try to fit what we can.  If we try to serialize a message
			// and it won't fit, we'll remove some stuff and see if that fits.
			for (;;)
			{

				// Slam flags based on what we are actually going to send.  Don't send flags
				// if they are implied by the stats we are sending.
				uint32 nImpliedFlags = 0;
				if ( msgStatsOut.has_stats() ) nImpliedFlags |= msgStatsOut.ACK_REQUEST_E2E;
				if ( nFlags != nImpliedFlags )
					msgStatsOut.set_flags( nFlags );
				else
					msgStatsOut.clear_flags();

				// Cache size, check how big it would be
				cbStatsMsg = msgStatsOut.ByteSize();

				// Include varint-encoded message size.
				// Note that if the size requires 3 bytes varint encoded, it won't fit in
				// a packet anyway, so we don't need to handle that case.  But it is totally
				// possible that we might want to send more than 128 bytes of stats, and have
				// an opportunity to send it all in the same packet.
				cbHdrSpaceNeeded = cbStatsMsg + 1;
				if ( cbStatsMsg >= 0x80 )
					++cbHdrSpaceNeeded;

				// Will it fit inline with this data packet?
				if ( cbHdrSpaceNeeded <= cbHdrOutSpaceRemaining )
					break;

				// Rats.  We want to send some stuff, but it won't fit.
				// Strip off stuff, in no particular order.

				if ( msgStatsOut.has_stats() )
				{
					Assert( bTrySendEndToEndStats );
					if ( msgStatsOut.stats().has_instantaneous() && msgStatsOut.stats().has_lifetime() )
					{
						// Trying to send both - clear instantaneous
						msgStatsOut.mutable_stats()->clear_instantaneous();
					}
					else
					{
						// Trying to send just one or the other.  Clear the whole container.
						msgStatsOut.clear_stats();
						bTrySendEndToEndStats = false;
					}
					continue;
				}
				Assert( !bTrySendEndToEndStats );

				// FIXME - we could try to send without acks.

				// Nothing left to clear!?  We shouldn't get here!
				AssertMsg( false, "Serialized stats message still won't fit, ever after clearing everything?" );
				cbStatsMsg = -1;
				break;
			}
		}
	}

	// Did we actually end up sending anything?
	if ( cbStatsMsg > 0 && pStatsMsg )
	{

		// Serialize the stats size, var-int encoded
		byte *pStatsOut = SerializeVarInt( (byte*)p, uint32( cbStatsMsg ) );

		// Serialize the actual message
		pStatsOut = pStatsMsg->SerializeWithCachedSizesToArray( pStatsOut );

		// Make sure we wrote the number of bytes we expected
		if ( pStatsOut != p + cbHdrSpaceNeeded )
		{
			// ABORT!
			AssertMsg( false, "Size mismatch after serializing connection quality stats" );
		}
		else
		{

			// Update bookkeeping with the stuff we are actually sending
			TrackSentStats( *pStatsMsg, true, usecNow );

			// Mark header with the flag
			hdr->m_unMsgFlags |= hdr->kFlag_ProtobufBlob;

			// Advance pointer
			p = pStatsOut;
		}
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
	SendPacketGather( 2, gather, cbSend );
	return cbSend;
}

bool CSteamNetworkConnectionIPv4::BInitConnect( const netadr_t &netadrRemote, SteamDatagramErrMsg &errMsg )
{
	AssertMsg( !m_pSocket, "Trying to connect when we already have a socket?" );

	// We're initiating a connection, not being accepted on a listen socket
	Assert( !m_pParentListenSocket );

	// For now we're just assuming each connection will gets its own socket,
	// on an ephemeral port.  Later we could add a setting to enable
	// sharing of the socket.
	m_pSocket = OpenUDPSocketBoundToHost( 0, 0, netadrRemote, CRecvPacketCallback( PacketReceived, this ), errMsg );
	if ( !m_pSocket )
		return false;

	// We use SteamID validity to denote when our connection has been accepted,
	// so it's important that it be cleared
	Assert( m_steamIDRemote.GetEAccountType() == k_EAccountTypeInvalid );
	m_steamIDRemote.Clear();

	// Let base class do some common initialization
	uint32 nPeerProtocolVersion = 0; // don't know yet
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	if ( !CSteamNetworkConnectionBase::BInitConnection( nPeerProtocolVersion, usecNow, errMsg ) )
	{
		m_pSocket->Close();
		m_pSocket = nullptr;
		return false;
	}

	// We should know our own identity, unless the app has said it's OK to go without this.
	if ( !m_steamIDLocal.IsValid() && steamdatagram_ip_allow_connections_without_auth == 0 )
	{
		V_strcpy_safe( errMsg, "Unable to determine local SteamID.  Not logged into Steam?" );
		return false;
	}

	// Start the connection state machine, and send the first request packet.
	CheckConnectionStateAndSetNextThinkTime( usecNow );

	return true;
}

bool CSteamNetworkConnectionIPv4::BCanSendEndToEndConnectRequest() const
{
	return m_pSocket != nullptr;
}

bool CSteamNetworkConnectionIPv4::BCanSendEndToEndData() const
{
	return m_pSocket != nullptr;
}

void CSteamNetworkConnectionIPv4::SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow )
{
	Assert( !m_pParentListenSocket );
	Assert( GetState() == k_ESteamNetworkingConnectionState_Connecting ); // Why else would we be doing this?
	Assert( m_unConnectionIDLocal );

	CMsgSteamSockets_UDP_ChallengeRequest msg;
	msg.set_connection_id( m_unConnectionIDLocal );
	//msg.set_client_steam_id( m_steamIDLocal.ConvertToUint64() );
	msg.set_my_timestamp( usecNow );
	msg.set_protocol_version( k_nCurrentProtocolVersion );

	// Send it, with padding
	SendPaddedMsg( k_ESteamNetworkingUDPMsg_ChallengeRequest, msg );

	// They are supposed to reply with a timestamps, from which we can estimate the ping.
	// So this counts as a ping request
	m_statsEndToEnd.TrackSentPingRequest( usecNow, false );
}

void CSteamNetworkConnectionIPv4::SendEndToEndPing( bool bUrgent, SteamNetworkingMicroseconds usecNow )
{
	SendStatsMsg( bUrgent ? k_EStatsReplyRequest_Immediate : k_EStatsReplyRequest_DelayedOK, usecNow );
}

void CSteamNetworkConnectionIPv4::ThinkConnection( SteamNetworkingMicroseconds usecNow )
{

	// Check if we have stats we need to flush out
	if ( BStateIsConnectedForWirePurposes() )
	{

		// Do we need to send something immediately, for any reason?
		if (
			m_statsEndToEnd.BNeedToSendStatsOrAcks( usecNow )
			|| m_statsEndToEnd.BNeedToSendPingImmediate( usecNow )
		) {
			SendStatsMsg( k_EStatsReplyRequest_None, usecNow );

			// Make sure that took care of what we needed!

			Assert( !m_statsEndToEnd.BNeedToSendStatsOrAcks( usecNow ) );
			Assert( !m_statsEndToEnd.BNeedToSendPingImmediate( usecNow ) );
		}
	}
}

bool CSteamNetworkConnectionIPv4::BBeginAccept(
	CSteamNetworkListenSocketStandard *pParent,
	const netadr_t &adrFrom,
	CSharedSocket *pSharedSock,
	CSteamID steamID,
	uint32 unConnectionIDRemote,
	uint32 nPeerProtocolVersion,
	const CMsgSteamDatagramCertificateSigned &msgCert,
	const CMsgSteamDatagramSessionCryptInfoSigned &msgCryptSessionInfo,
	SteamDatagramErrMsg &errMsg
)
{
	AssertMsg( !m_pSocket, "Trying to accept when we already have a socket?" );

	// Get an interface just to talk just to this guy
	m_pSocket = pSharedSock->AddRemoteHost( adrFrom, CRecvPacketCallback( PacketReceived, this ) );
	if ( !m_pSocket )
	{
		V_strcpy_safe( errMsg, "Unable to create a bound socket on the shared socket." );
		return false;
	}

	m_steamIDRemote = steamID;
	m_unConnectionIDRemote = unConnectionIDRemote;
	m_netAdrRemote = adrFrom;
	pParent->AddChildConnection( this );

	// Let base class do some common initialization
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	if ( !CSteamNetworkConnectionBase::BInitConnection( nPeerProtocolVersion, usecNow, errMsg ) )
	{
		m_pSocket->Close();
		m_pSocket = nullptr;
		return false;
	}

	// Process crypto handshake now
	if ( !BRecvCryptoHandshake( msgCert, msgCryptSessionInfo, true ) )
	{
		m_pSocket->Close();
		m_pSocket = nullptr;
		return false;
	}

	// OK
	return true;
}

EResult CSteamNetworkConnectionIPv4::APIAcceptConnection()
{
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Send the message
	SendConnectOK( usecNow );

	// We are fully connected
	ConnectionState_Connected( usecNow );

	// OK
	return k_EResultOK;
}

void CSteamNetworkConnectionIPv4::SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg )
{

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	pkt[0] = nMsgID;
	int cbPkt = msg.ByteSize()+1;
	if ( cbPkt > sizeof(pkt) )
	{
		AssertMsg3( false, "Msg type %d is %d bytes, larger than MTU of %d bytes", int( nMsgID ), cbPkt, (int)sizeof(pkt) );
		return;
	}
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt+1 );
	Assert( cbPkt == pEnd - pkt );

	SendPacket( pkt, cbPkt );
}

void CSteamNetworkConnectionIPv4::SendPaddedMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg )
{

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	V_memset( pkt, 0, sizeof(pkt) ); // don't send random bits from our process memory over the wire!
	UDPPaddedMessageHdr *hdr = (UDPPaddedMessageHdr *)pkt;
	int nMsgLength = msg.ByteSize();
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

void CSteamNetworkConnectionIPv4::SendPacket( const void *pkt, int cbPkt )
{
	iovec temp;
	temp.iov_base = const_cast<void*>( pkt );
	temp.iov_len = cbPkt;
	SendPacketGather( 1, &temp, cbPkt );
}

void CSteamNetworkConnectionIPv4::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )
{
	// Safety
	if ( !m_pSocket )
	{
		AssertMsg( false, "Attemt to send packet, but socket has been closed!" );
		return;
	}

	// Update stats
	m_statsEndToEnd.TrackSentPacket( cbSendTotal );

	// Hand over to operating system
	m_pSocket->BSendRawPacketGather( nChunks, pChunks );
}

void CSteamNetworkConnectionIPv4::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	CSteamNetworkConnectionBase::ConnectionStateChanged( eOldState );

	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for IPv4
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

#define ReportBadPacketIPv4( pszMsgType, /* fmt */ ... ) \
	ReportBadPacketFrom( m_pSocket->GetRemoteHostAddr(), pszMsgType, __VA_ARGS__ )

void CSteamNetworkConnectionIPv4::PacketReceived( const void *pvPkt, int cbPkt, const netadr_t &adrFrom, CSteamNetworkConnectionIPv4 *pSelf )
{
	const uint8 *pPkt = static_cast<const uint8 *>( pvPkt );

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	pSelf->m_statsEndToEnd.TrackRecvPacket( cbPkt, usecNow ); // FIXME - We really shouldn't do this until we know it is valid and hasn't been spoofed!

	if ( cbPkt < 5 )
	{
		ReportBadPacket( "packet", "%d byte packet is too small", cbPkt );
		return;
	}

	if ( *pPkt & 0x80 )
	{
		pSelf->Received_Data( pPkt, cbPkt, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ChallengeReply )
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

std::string DescribeStatsContents( const CMsgSteamSockets_UDP_Stats &msg )
{
	std::string sWhat;
	if ( msg.flags() & msg.ACK_REQUEST_E2E )
		sWhat += " request_ack";
	if ( msg.flags() & msg.ACK_REQUEST_IMMEDIATE )
		sWhat += " request_ack_immediate";
	if ( msg.stats().has_lifetime() )
		sWhat += " stats.life";
	if ( msg.stats().has_instantaneous() )
		sWhat += " stats.rate";
	if ( msg.ack_e2e_size() > 0 )
		sWhat += " ack";
	return sWhat;
}

void CSteamNetworkConnectionIPv4::RecvStats( const CMsgSteamSockets_UDP_Stats &msgStatsIn, bool bInline, SteamNetworkingMicroseconds usecNow )
{

	// Connection quality stats?
	if ( msgStatsIn.has_stats() )
		m_statsEndToEnd.ProcessMessage( msgStatsIn.stats(), usecNow );

	// Receive acks, if any
	m_statsEndToEnd.RecvPackedAcks( msgStatsIn.ack_e2e(), usecNow );

	// Spew appropriately
	if ( g_eSteamDatagramDebugOutputDetailLevel >= k_ESteamNetworkingSocketsDebugOutputType_Verbose )
	{
		std::string sWhat = DescribeStatsContents( msgStatsIn );
		SpewVerbose( "Recvd %s stats from %s @ %s:%s\n",
			bInline ? "inline" : "standalone",
			m_steamIDRemote.Render(),
			CUtlNetAdrRender( m_pSocket->GetRemoteHostAddr() ).String(),
			sWhat.c_str()
		);
	}

	// Check if we need to reply, either now or later
	if ( BStateIsConnectedForWirePurposes() )
	{

		// Check for queuing outgoing acks
		bool bImmediate = ( msgStatsIn.flags() & msgStatsIn.ACK_REQUEST_IMMEDIATE ) != 0;
		if ( ( msgStatsIn.flags() & msgStatsIn.ACK_REQUEST_E2E ) || msgStatsIn.has_stats() )
		{
			m_statsEndToEnd.QueueOutgoingAck( msgStatsIn.seq_num(), bImmediate, usecNow );
		}

		// Do we need to send an immediate reply?
		if (
			m_statsEndToEnd.BNeedToSendPingImmediate( usecNow )
			|| m_statsEndToEnd.BNeedToSendStatsOrAcks( usecNow )
		) {
			// Send a stats message
			SendStatsMsg( k_EStatsReplyRequest_None, usecNow );
		}
	}
}

void CSteamNetworkConnectionIPv4::TrackSentStats( const CMsgSteamSockets_UDP_Stats &msgStatsOut, bool bInline, SteamNetworkingMicroseconds usecNow )
{

	// What effective flags will be received?
	uint32 nSentFlags = msgStatsOut.flags();
	if ( msgStatsOut.has_stats() )
		nSentFlags |= msgStatsOut.ACK_REQUEST_E2E;
	if ( nSentFlags & msgStatsOut.ACK_REQUEST_E2E )
	{
		bool bAllowDelayedReply = ( nSentFlags & msgStatsOut.ACK_REQUEST_IMMEDIATE ) == 0;

		// Record that we sent stats and are waiting for peer to ack
		if ( msgStatsOut.has_stats() )
		{
			m_statsEndToEnd.TrackSentStats( msgStatsOut.stats(), usecNow, bAllowDelayedReply );
		}
		else if ( ( nSentFlags & msgStatsOut.ACK_REQUEST_E2E ) )
		{
			m_statsEndToEnd.TrackSentMessageExpectingSeqNumAck( usecNow, bAllowDelayedReply );
		}
	}

	// Did we send any acks?
	m_statsEndToEnd.TrackSentPackedAcks( msgStatsOut.ack_e2e() );

	// Spew appropriately
	if ( g_eSteamDatagramDebugOutputDetailLevel >= k_ESteamNetworkingSocketsDebugOutputType_Verbose )
	{
		std::string sWhat = DescribeStatsContents( msgStatsOut );
		SpewVerbose( "Sent %s stats to %s @ %s:%s\n",
			bInline ? "inline" : "standalone",
			m_steamIDRemote.Render(),
			CUtlNetAdrRender( m_pSocket->GetRemoteHostAddr() ).String(),
			sWhat.c_str()
		);
	}
}

void CSteamNetworkConnectionIPv4::SendStatsMsg( EStatsReplyRequest eReplyRequested, SteamNetworkingMicroseconds usecNow )
{
	IPv4InlineStatsContext_t context;
	CMsgSteamSockets_UDP_Stats &msg = context.msg;

//	if ( m_unConnectionIDRemote )
//		msg.set_to_connection_id( m_unConnectionIDRemote );
//	msg.set_from_connection_id( m_unConnectionIDLocal );
//	msg.set_seq_num( m_statsEndToEnd.GetNextSendSequenceNumber( usecNow ) );

	// What flags should we set?
	uint32 nFlags = 0;
	if ( eReplyRequested == k_EStatsReplyRequest_Immediate || m_statsEndToEnd.BNeedToSendPingImmediate( usecNow ) )
		nFlags |= msg.ACK_REQUEST_E2E | msg.ACK_REQUEST_IMMEDIATE;
	else if ( eReplyRequested == k_EStatsReplyRequest_DelayedOK || m_statsEndToEnd.BNeedToSendKeepalive( usecNow ) || m_statsEndToEnd.BReadyToSendTracerPing( usecNow ) )
		nFlags |= msg.ACK_REQUEST_E2E;

	// Need to send any connection stats stats?
	if ( m_statsEndToEnd.BReadyToSendStats( usecNow ) )
		m_statsEndToEnd.PopulateMessage( *msg.mutable_stats(), usecNow );

	// Any pending acks?
	PutEndToEndAcksIntoMessage( msg, m_statsEndToEnd, usecNow );

	// Always set flags into message, even if they can be implied by the presence of stats
	msg.set_flags( nFlags );

	context.cbSize = msg.ByteSize();
	if ( context.cbSize > k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend )
	{
		AssertMsg1( false, "Serialized CMsgSteamSockets_UDP_Stats is %d bytes!", context.cbSize );
		return;
	}

	// Ask SNP to send a packet, with this data piggybacked on.
	// FIXME we can probably do better that this, although this is
	// not too bad.
	int cbMaxEncryptedPayload = k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend - context.cbSize;
	SNP_SendPacket( usecNow, cbMaxEncryptedPayload, &context );
}

void CSteamNetworkConnectionIPv4::Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow )
{

	if ( cbPkt < sizeof(UDPDataMsgHdr) )
	{
		ReportBadPacketIPv4( "DataPacket", "Packet of size %d is too small.", cbPkt );
		return;
	}

	// Check cookie
	const UDPDataMsgHdr *hdr = (const UDPDataMsgHdr *)pPkt;
	if ( LittleDWord( hdr->m_unToConnectionID ) != m_unConnectionIDLocal )
	{

		// Wrong session.  It could be an old session, or it could be spoofed.
		ReportBadPacketIPv4( "DataPacket", "Incorrect connection ID" );
		if ( BCheckGlobalSpamReplyRateLimit( usecNow ) )
		{
			SendNoConnection( LittleDWord( hdr->m_unToConnectionID ), 0 );
		}
		return;
	}
	uint16 nWirePktNumber = LittleWord( hdr->m_unSeqNum );

	// Check state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for IPv4
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			return;

		case k_ESteamNetworkingConnectionState_Linger:
			// FIXME: What should we do here?  We are half-closed here, so this
			// data is definitely going to be ignored.  Do we need to communicate
			// that state to the remote host somehow?
			return;

		case k_ESteamNetworkingConnectionState_Connecting:
			// Ignore it.  We don't have the SteamID of whoever is on the other end yet,
			// their encryption keys, etc.  The most likely cause is that a server sent
			// a ConnectOK, which dropped.  So they think we're connected but we don't
			// have everything yet.
			return;

		case k_ESteamNetworkingConnectionState_Connected:

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
			ReportBadPacketIPv4( "DataPacket", "Failed to varint decode size of stats blob" );
			return;
		}
		if ( pIn + cbStatsMsgIn > pPktEnd )
		{
			ReportBadPacketIPv4( "DataPacket", "stats message size doesn't make sense.  Stats message size %d, packet size %d", cbStatsMsgIn, cbPkt );
			return;
		}

		if ( !msgStats.ParseFromArray( pIn, cbStatsMsgIn ) )
		{
			ReportBadPacketIPv4( "DataPacket", "protobuf failed to parse inline stats message" );
			return;
		}

		// Shove sequence number so we know what acks to pend, etc
		msgStats.set_seq_num( nWirePktNumber );
		pMsgStatsIn = &msgStats;

		// Advance pointer
		pIn += cbStatsMsgIn;
	}

	if ( RecvDataChunk( nWirePktNumber, pIn, pPktEnd - pIn, cbPkt, 0, usecNow ) )
	{

		// Process the stats, if any
		if ( pMsgStatsIn )
			RecvStats( *pMsgStatsIn, true, usecNow );
	}
}

void CSteamNetworkConnectionIPv4::Received_ChallengeReply( const CMsgSteamSockets_UDP_ChallengeReply &msg, SteamNetworkingMicroseconds usecNow )
{
	// We should only be getting this if we are the "client"
	if ( m_pParentListenSocket )
	{
		ReportBadPacketIPv4( "ChallengeReply", "Shouldn't be receiving this unless on accepted connections, only connections initiated locally." );
		return;
	}

	// Ignore if we're not trying to connect
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting )
		return;

	// Check session ID to make sure they aren't spoofing.
	if ( msg.connection_id() != m_unConnectionIDLocal )
	{
		ReportBadPacketIPv4( "ChallengeReply", "Incorrect connection ID.  Message is stale or could be spoofed, ignoring." );
		return;
	}
	if ( msg.protocol_version() < k_nMinRequiredProtocolVersion )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Generic, "Peer is running old software and needs to be udpated" );
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
			m_statsEndToEnd.m_ping.ReceivedPing( nPing, usecNow );
		}
	}

	// Make sure we have the crypt info that we need
	if ( !m_msgSignedCertLocal.has_cert() || !m_msgSignedCryptLocal.has_info() )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Tried to connect request, but crypt not ready" );
		return;
	}

	// Remember protocol version.  They should send it again in the connect OK, but we have a valid value now,
	// so we might as well save it
	m_statsEndToEnd.m_nPeerProtocolVersion = msg.protocol_version();

	// Reply with the challenge data and our SteamID
	CMsgSteamSockets_UDP_ConnectRequest msgConnectRequest;
	msgConnectRequest.set_client_connection_id( m_unConnectionIDLocal );
	msgConnectRequest.set_challenge( msg.challenge() );
	msgConnectRequest.set_client_steam_id( m_steamIDLocal.ConvertToUint64() );
	msgConnectRequest.set_my_timestamp( usecNow );
	if ( m_statsEndToEnd.m_ping.m_nSmoothedPing >= 0 )
		msgConnectRequest.set_ping_est_ms( m_statsEndToEnd.m_ping.m_nSmoothedPing );
	*msgConnectRequest.mutable_cert() = m_msgSignedCertLocal;
	*msgConnectRequest.mutable_crypt() = m_msgSignedCryptLocal;
	msgConnectRequest.set_protocol_version( k_nCurrentProtocolVersion );
	SendMsg( k_ESteamNetworkingUDPMsg_ConnectRequest, msgConnectRequest );

	// Reset timeout/retry for this reply.  But if it fails, we'll start
	// the whole handshake over again.  It keeps the code simpler, and the
	// challenge value has a relatively short expiry anyway.
	m_usecWhenSentConnectRequest = usecNow;
	EnsureMinThinkTime( usecNow + k_usecConnectRetryInterval );

	// They are supposed to reply with a timestamps, from which we can estimate the ping.
	// So this counts as a ping request
	m_statsEndToEnd.TrackSentPingRequest( usecNow, false );
}

void CSteamNetworkConnectionIPv4::Received_ConnectOK( const CMsgSteamSockets_UDP_ConnectOK &msg, SteamNetworkingMicroseconds usecNow )
{
	// We should only be getting this if we are the "client"
	if ( m_pParentListenSocket )
	{
		ReportBadPacketIPv4( "ConnectOK", "Shouldn't be receiving this unless on accepted connections, only connections initiated locally." );
		return;
	}

	// Check connection ID to make sure they aren't spoofing and it's the same connection we think it is
	if ( msg.client_connection_id() != m_unConnectionIDLocal )
	{
		ReportBadPacketIPv4( "ConnectOK", "Incorrect connection ID.  Message is stale or could be spoofed, ignoring." );
		return;
	}

	// Who are they?  We'll authenticate their cert below
	CSteamID steamIDRemote( uint64( msg.server_steam_id() ) );
	if ( !steamIDRemote.IsValid() && steamdatagram_ip_allow_connections_without_auth == 0 )
	{
		ReportBadPacketIPv4( "ConnectOK", "Invalid server_steam_id." );
		return;
	}

	// Make sure they are still who we think they are
	if ( m_steamIDRemote.IsValid() && m_steamIDRemote != steamIDRemote )
	{
		ReportBadPacketIPv4( "ConnectOK", "server_steam_id doesn't match who we expect to be connecting to!" );
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
			m_statsEndToEnd.m_ping.ReceivedPing( nPing, usecNow );
		}
	}

	// Check state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for IPv4
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

	if ( msg.protocol_version() < k_nMinRequiredProtocolVersion )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Generic, "Peer is running old software and needs to be udpated" );
		return;
	}
	m_statsEndToEnd.m_nPeerProtocolVersion = msg.protocol_version();

	// New peers should send us their connection ID
	m_unConnectionIDRemote = msg.server_connection_id();
	if ( ( m_unConnectionIDRemote & 0xffff ) == 0 )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Didn't send valid connection ID" );
		return;
	}

	m_steamIDRemote = steamIDRemote;

	// Check the certs, save keys, etc
	if ( !BRecvCryptoHandshake( msg.cert(), msg.crypt(), false ) )
	{
		Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
		ReportBadPacketIPv4( "ConnectOK", "Failed crypto init.  %s", m_szEndDebug );
		return;
	}

	// Generic connection code will take it from here.
	ConnectionState_Connected( usecNow );
}

void CSteamNetworkConnectionIPv4::Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, SteamNetworkingMicroseconds usecNow )
{
	// Give them a reply to let them know we heard from them.  If it's the right connection ID,
	// then they probably aren't spoofing and it's critical that we give them an ack!
	//
	// If the wrong connection ID, then it could be an old connection so we'd like to send a reply
	// to let them know that they can stop telling us the connection is closed.
	// However, it could just be random garbage, so we need to protect ourselves from abuse,
	// so limit how many of these we send.
	bool bConnectionIDMatch =
		msg.to_connection_id() == m_unConnectionIDLocal
		|| ( msg.to_connection_id() == 0 && msg.from_connection_id() && msg.from_connection_id() == m_unConnectionIDRemote ); // they might not know our ID yet, if they are a client aborting the connection really early.
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
	ConnectionState_ClosedByPeer( msg.reason_code(), msg.debug().c_str() );
}

void CSteamNetworkConnectionIPv4::Received_NoConnection( const CMsgSteamSockets_UDP_NoConnection &msg, SteamNetworkingMicroseconds usecNow )
{
	// Make sure it's an ack of something we would have sent
	if ( msg.to_connection_id() != m_unConnectionIDLocal || msg.from_connection_id() != m_unConnectionIDRemote )
	{
		ReportBadPacketIPv4( "NoConnection", "Old/incorrect connection ID.  Message is for a stale connection, or is spoofed.  Ignoring." );
		return;
	}

	// Generic connection code will take it from here.
	ConnectionState_ClosedByPeer( 0, nullptr );
}

void CSteamNetworkConnectionIPv4::Received_ChallengeOrConnectRequest( const char *pszDebugPacketType, uint32 unPacketConnectionID, SteamNetworkingMicroseconds usecNow )
{
	// If wrong connection ID, then check for sending a generic reply and bail
	if ( unPacketConnectionID != m_unConnectionIDRemote )
	{
		ReportBadPacketIPv4( pszDebugPacketType, "Incorrect connection ID, when we do have a connection for this address.  Could be spoofed, ignoring." );
		// Let's not send a reply in this case
		//if ( BCheckGlobalSpamReplyRateLimit( usecNow ) )
		//	SendNoConnection( unPacketConnectionID );
		return;
	}

	// Check state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for IPv4
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
			if ( !m_pParentListenSocket )
			{
				// WAT?  We initiated this connection, so why are they requesting to connect?
				ReportBadPacketIPv4( pszDebugPacketType, "We are the 'client' who initiated the connection, so 'server' shouldn't be sending us this!" );
				return;
			}

			// This is totally legit and possible.  Our earlier reply might have dropped, and they are re-sending
			SendConnectOK( usecNow );
			return;
	}

}

void CSteamNetworkConnectionIPv4::SendConnectionClosedOrNoConnection()
{
	if ( GetState() == k_ESteamNetworkingConnectionState_ClosedByPeer )
	{
		SendNoConnection( m_unConnectionIDLocal, m_unConnectionIDRemote );
	}
	else
	{
		CMsgSteamSockets_UDP_ConnectionClosed msg;
		msg.set_from_connection_id( m_unConnectionIDLocal );

		if ( m_unConnectionIDRemote )
			msg.set_to_connection_id( m_unConnectionIDRemote );

		msg.set_reason_code( m_eEndReason );
		if ( m_szEndDebug[0] )
			msg.set_debug( m_szEndDebug );
		SendPaddedMsg( k_ESteamNetworkingUDPMsg_ConnectionClosed, msg );
	}
}

void CSteamNetworkConnectionIPv4::SendNoConnection( uint32 unFromConnectionID, uint32 unToConnectionID )
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

void CSteamNetworkConnectionIPv4::SendConnectOK( SteamNetworkingMicroseconds usecNow )
{
	Assert( m_unConnectionIDLocal );
	Assert( m_unConnectionIDRemote );
	Assert( m_pParentListenSocket );

	Assert( m_msgSignedCertLocal.has_cert() );
	Assert( m_msgSignedCryptLocal.has_info() );

	CMsgSteamSockets_UDP_ConnectOK msg;
	msg.set_client_connection_id( m_unConnectionIDRemote );
	msg.set_server_connection_id( m_unConnectionIDLocal );
	msg.set_server_steam_id( m_steamIDLocal.ConvertToUint64() );
	*msg.mutable_cert() = m_msgSignedCertLocal;
	*msg.mutable_crypt() = m_msgSignedCryptLocal;
	msg.set_protocol_version( k_nCurrentProtocolVersion );

	// Do we have a timestamp?
	if ( m_usecWhenReceivedHandshakeRemoteTimestamp )
	{
		SteamNetworkingMicroseconds usecElapsed = usecNow - m_usecWhenReceivedHandshakeRemoteTimestamp;
		Assert( usecElapsed >= 0 );
		if ( usecElapsed < 4*k_nMillion )
		{
			msg.set_your_timestamp( m_ulHandshakeRemoteTimestamp );
			msg.set_delay_time_usec( usecElapsed );
		}
		else
		{
			SpewWarning( "Discarding handshake timestamp that's %lldms old, not sending in ConnectOK\n", usecElapsed/1000 );
			m_usecWhenReceivedHandshakeRemoteTimestamp = 0;
		}
	}


	// Send it, with padding
	SendMsg( k_ESteamNetworkingUDPMsg_ConnectOK, msg );
}

/////////////////////////////////////////////////////////////////////////////
//
// Loopback connections
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkConnectionlocalhostLoopback::CSteamNetworkConnectionlocalhostLoopback( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkConnectionIPv4( pSteamNetworkingSocketsInterface )
{
}

bool CSteamNetworkConnectionlocalhostLoopback::BAllowRemoteUnsignedCert() { return true; }

void CSteamNetworkConnectionlocalhostLoopback::InitConnectionCrypto( SteamNetworkingMicroseconds usecNow )
{
	InitLocalCryptoWithUnsignedCert();
}

void CSteamNetworkConnectionlocalhostLoopback::PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState )
{
	// Don't post any callbacks for the initial transitions.
	if ( eNewAPIState == k_ESteamNetworkingConnectionState_Connected || eNewAPIState == k_ESteamNetworkingConnectionState_Connected )
		return;

	// But post callbacks for these guys
	CSteamNetworkConnectionIPv4::PostConnectionStateChangedCallback( eOldAPIState, eNewAPIState );
}

bool CSteamNetworkConnectionlocalhostLoopback::APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionlocalhostLoopback *pConn[2] )
{
	SteamDatagramErrMsg errMsg;

	pConn[1] = new CSteamNetworkConnectionlocalhostLoopback( pSteamNetworkingSocketsInterface );
	pConn[0] = new CSteamNetworkConnectionlocalhostLoopback( pSteamNetworkingSocketsInterface );
	if ( !pConn[0] || !pConn[1] )
	{
failed:
		delete pConn[0]; pConn[0] = nullptr;
		delete pConn[1]; pConn[1] = nullptr;
		return false;
	}

	IBoundUDPSocket *sock[2];
	if ( !CreateBoundSocketPair(
		CRecvPacketCallback( CSteamNetworkConnectionIPv4::PacketReceived, (CSteamNetworkConnectionIPv4*)pConn[0] ),
		CRecvPacketCallback( CSteamNetworkConnectionIPv4::PacketReceived, (CSteamNetworkConnectionIPv4*)pConn[1] ), sock, errMsg ) )
	{
		// Use assert here, because this really should only fail if we have some sort of bug
		AssertMsg1( false, "Failed to create UDP socekt pair.  %s", errMsg );
		goto failed;
	}

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Initialize both connections
	for ( int i = 0 ; i < 2 ; ++i )
	{
		pConn[i]->m_pSocket = sock[i];
		if ( !pConn[i]->BInitConnection( k_nCurrentProtocolVersion, usecNow, errMsg ) )
			goto failed;
	}

	// Tie the connections to each other, and mark them as connected
	for ( int i = 0 ; i < 2 ; ++i )
	{
		CSteamNetworkConnectionlocalhostLoopback *p = pConn[i];
		CSteamNetworkConnectionlocalhostLoopback *q = pConn[1-i];
		p->m_steamIDRemote = q->m_steamIDLocal;
		p->m_unConnectionIDRemote = q->m_unConnectionIDLocal;
		p->m_statsEndToEnd.m_usecTimeLastRecv = usecNow; // Act like we just now received something
		if ( !p->BRecvCryptoHandshake( q->m_msgSignedCertLocal, q->m_msgSignedCryptLocal, i==0 ) )
		{
			AssertMsg( false, "BRecvCryptoHandshake failed creating localhost socket pair" );
			goto failed;
		}
		p->ConnectionState_Connected( usecNow );
	}

	return true;
}

} // namespace SteamNetworkingSocketsLib
