//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_p2p_webrtc.h"
#include "steamnetworkingsockets_udp.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

#pragma pack( push, 1 )
struct WebRTCDataMsgHdr
{
	enum
	{
		kFlag_ProtobufBlob  = 0x01, // Protobuf-encoded message is inline (CMsgSteamSockets_UDP_Stats)
	};

	uint8 m_unMsgFlags;
	uint16 m_unSeqNum;

	// [optional, if flags&kFlag_ProtobufBlob]  varint-encoded protobuf blob size, followed by blob
	// Data frame(s)
	// End of packet
};
#pragma pack( pop )

/////////////////////////////////////////////////////////////////////////////
//
// CConnectionTransportP2PSDR
//
/////////////////////////////////////////////////////////////////////////////

CConnectionTransportP2PWebRTC::CConnectionTransportP2PWebRTC( CSteamNetworkConnectionP2P &connection )
: CConnectionTransport( connection )
, m_pWebRTCSession( nullptr )
, m_eWebRTCSessionState( k_EWebRTCSessionStateNew )
{
}

CConnectionTransportP2PWebRTC::~CConnectionTransportP2PWebRTC()
{
	Assert( !m_pWebRTCSession );
}

void CConnectionTransportP2PWebRTC::TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const
{
}

void CConnectionTransportP2PWebRTC::GetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow )
{
	// FIXME Need to indicate whether we are relayed or were able to pierce NAT
	CConnectionTransport::GetDetailedConnectionStatus( stats, usecNow );
}

void CConnectionTransportP2PWebRTC::TransportFreeResources()
{
	if ( m_pWebRTCSession )
	{
		m_pWebRTCSession->Release();
		m_pWebRTCSession = nullptr;
	}

	if ( m_eWebRTCSessionState == k_EWebRTCSessionStateConnecting || m_eWebRTCSessionState == k_EWebRTCSessionStateConnected )
		m_eWebRTCSessionState = k_EWebRTCSessionStateClosed;

	CConnectionTransport::TransportFreeResources();
}

void CConnectionTransportP2PWebRTC::SendStatsMsg( EStatsReplyRequest eReplyRequested, SteamNetworkingMicroseconds usecNow, const char *pszReason )
{
	UDPSendPacketContext_t ctx( usecNow, pszReason );
	ctx.Populate( sizeof(WebRTCDataMsgHdr), eReplyRequested, m_connection );

	// Send a data packet (maybe containing ordinary data), with this piggy backed on top of it
	m_connection.SNP_SendPacket( ctx );
}

bool CConnectionTransportP2PWebRTC::SendDataPacket( SteamNetworkingMicroseconds usecNow )
{
	if ( !m_pWebRTCSession )
	{
		Assert( false );
		return false;
	}


	// Populate context struct with any stats we want/need to send, and how much space we need to reserve for it
	UDPSendPacketContext_t ctx( usecNow, "data" );
	ctx.Populate( sizeof(WebRTCDataMsgHdr), k_EStatsReplyRequest_NothingToSend, m_connection );

	// Send a packet
	return m_connection.SNP_SendPacket( ctx );
}

int CConnectionTransportP2PWebRTC::SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctxBase )
{
	if ( !m_pWebRTCSession )
	{
		Assert( false );
		return 0;
	}

	UDPSendPacketContext_t &ctx = static_cast<UDPSendPacketContext_t &>( ctxBase );

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	WebRTCDataMsgHdr *hdr = (WebRTCDataMsgHdr *)pkt;
	hdr->m_unMsgFlags = 0x80;
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

	ctx.Trim( cbHdrOutSpaceRemaining);
	if ( ctx.Serialize( p ) )
	{
		// Update bookkeeping with the stuff we are actually sending
		TrackSentStats( ctx.msg, true, ctx.m_usecNow );

		// Mark header with the flag
		hdr->m_unMsgFlags |= hdr->kFlag_ProtobufBlob;
	}

	// !FIXME! Time since previous, for jitter measurement?

	// And now append the payload
	memcpy( p, pChunk, cbChunk );
	p += cbChunk;
	int cbSend = p - pkt;
	Assert( cbSend <= sizeof(pkt) ); // Bug in the code above.  We should never "overflow" the packet.  (Ignoring the fact that we using a gather-based send.  The data could be tiny with a large header for piggy-backed stats.)

	// !FIXME! Should we track data payload separately?  Maybe we ought to track
	// *messages* instead of packets.

	// Send it
	if ( !m_pWebRTCSession->BSendData( pkt, cbSend ) )
		return -1;
	return cbSend;
}

void CConnectionTransportP2PWebRTC::RecvStats( const CMsgSteamSockets_UDP_Stats &msgStatsIn, bool bInline, SteamNetworkingMicroseconds usecNow )
{

	// Connection quality stats?
	if ( msgStatsIn.has_stats() )
		m_connection.m_statsEndToEnd.ProcessMessage( msgStatsIn.stats(), usecNow );

	// Spew appropriately
	SpewVerbose( "[%s] Recv %s stats:%s\n",
		ConnectionDescription(),
		bInline ? "inline" : "standalone",
		DescribeStatsContents( msgStatsIn ).c_str()
	);

	// Check if we need to reply, either now or later
	if ( m_connection.BStateIsConnectedForWirePurposes() )
	{

		// Check for queuing outgoing acks
		bool bImmediate = ( msgStatsIn.flags() & msgStatsIn.ACK_REQUEST_IMMEDIATE ) != 0;
		if ( ( msgStatsIn.flags() & msgStatsIn.ACK_REQUEST_E2E ) || msgStatsIn.has_stats() )
		{
			m_connection.QueueEndToEndAck( bImmediate, usecNow );
		}

		// Do we need to send an immediate reply?
		const char *pszReason = m_connection.NeedToSendEndToEndStatsOrAcks( usecNow );
		if ( pszReason )
		{
			// Send a stats message
			SendStatsMsg( k_EStatsReplyRequest_NothingToSend, usecNow, pszReason );
		}
	}
}

void CConnectionTransportP2PWebRTC::TrackSentStats( const CMsgSteamSockets_UDP_Stats &msgStatsOut, bool bInline, SteamNetworkingMicroseconds usecNow )
{

	// What effective flags will be received?
	bool bAllowDelayedReply = ( msgStatsOut.flags() & msgStatsOut.ACK_REQUEST_IMMEDIATE ) == 0;

	// Record that we sent stats and are waiting for peer to ack
	if ( msgStatsOut.has_stats() )
	{
		m_connection.m_statsEndToEnd.TrackSentStats( msgStatsOut.stats(), usecNow, bAllowDelayedReply );
	}
	else if ( msgStatsOut.flags() & msgStatsOut.ACK_REQUEST_E2E )
	{
		m_connection.m_statsEndToEnd.TrackSentMessageExpectingSeqNumAck( usecNow, bAllowDelayedReply );
	}

	// Spew appropriately
	SpewVerbose( "[%s] Sent %s stats:%s\n",
		ConnectionDescription(),
		bInline ? "inline" : "standalone",
		DescribeStatsContents( msgStatsOut ).c_str()
	);
}

static void ReallyReportBadWebRTCPacket( CConnectionTransportP2PWebRTC *pTransport, const char *pszMsgType, const char *pszFmt, ... )
{
	char buf[ 2048 ];
	va_list ap;
	va_start( ap, pszFmt );
	V_vsprintf_safe( buf, pszFmt, ap );
	va_end( ap );
	V_StripTrailingWhitespaceASCII( buf );

	if ( !pszMsgType || !pszMsgType[0] )
		pszMsgType = "message";

	SpewMsg( "[%s] Ignored bad %s.  %s\n", pTransport->ConnectionDescription(), pszMsgType, buf );
}


#define ReportBadPacket( pszMsgType, /* fmt */ ... ) \
	( BCheckRateLimitReportBadPacket( usecNow ) ? ReallyReportBadWebRTCPacket( this, pszMsgType, __VA_ARGS__ ) : (void)0 )

#define ParseProtobufBody( pvMsg, cbMsg, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	if ( !msgVar.ParseFromArray( pvMsg, cbMsg ) ) \
	{ \
		ReportBadPacket( # CMsgCls, "Protobuf parse failed." ); \
		return; \
	}

void CConnectionTransportP2PWebRTC::OnData( const uint8_t *pPkt, size_t nSize )
{
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	int cbPkt = int( nSize );

	if ( cbPkt < 1 )
	{
		ReportBadPacket( "packet", "%d byte packet is too small", cbPkt );
		return;
	}

	// Data packet is the most common, check for it first.  Also, does stat tracking.
	if ( *pPkt & 0x80 )
	{
		Received_Data( pPkt, cbPkt, usecNow );
		return;
	}

	// Track stats for other packet types.
	m_connection.m_statsEndToEnd.TrackRecvPacket( cbPkt, usecNow );

	if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectionClosed )
	{
		ParseProtobufBody( pPkt, cbPkt, CMsgSteamSockets_UDP_ConnectionClosed, msg )
		Received_ConnectionClosed( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_NoConnection )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_NoConnection, msg )
		Received_NoConnection( msg, usecNow );
	}
	else
	{
		ReportBadPacket( "packet", "Lead byte 0x%02x not a known message ID", *pPkt );
	}
}

void CConnectionTransportP2PWebRTC::Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow )
{

	if ( cbPkt < sizeof(WebRTCDataMsgHdr) )
	{
		ReportBadPacket( "data", "Packet of size %d is too small.", cbPkt );
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
			// Ignore it.  We don't have the SteamID of whoever is on the other end yet,
			// their encryption keys, etc.  The most likely cause is that a server sent
			// a ConnectOK, which dropped.  So they think we're connected but we don't
			// have everything yet.
			return;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connected:

			// We'll process the chunk
			break;
	}

	// Check header
	const WebRTCDataMsgHdr *hdr = (const WebRTCDataMsgHdr *)pPkt;
	uint16 nWirePktNumber = LittleWord( hdr->m_unSeqNum );

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
			ReportBadPacket( "DataPacket", "Failed to varint decode size of stats blob" );
			return;
		}
		if ( pIn + cbStatsMsgIn > pPktEnd )
		{
			ReportBadPacket( "DataPacket", "stats message size doesn't make sense.  Stats message size %d, packet size %d", cbStatsMsgIn, cbPkt );
			return;
		}

		if ( !msgStats.ParseFromArray( pIn, cbStatsMsgIn ) )
		{
			ReportBadPacket( "DataPacket", "protobuf failed to parse inline stats message" );
			return;
		}

		// Shove sequence number so we know what acks to pend, etc
		msgStats.set_seq_num( nWirePktNumber );
		pMsgStatsIn = &msgStats;

		// Advance pointer
		pIn += cbStatsMsgIn;
	}

	const void *pChunk = pIn;
	int cbChunk = pPktEnd - pIn;

	// Decrypt it, and check packet number
	uint8 tempDecrypted[ k_cbSteamNetworkingSocketsMaxPlaintextPayloadRecv ];
	void *pDecrypted = tempDecrypted;
	uint32 cbDecrypted = sizeof(tempDecrypted);
	int64 nFullSequenceNumber = m_connection.DecryptDataChunk( nWirePktNumber, cbPkt, pChunk, cbChunk, pDecrypted, cbDecrypted, usecNow );
	if ( nFullSequenceNumber <= 0 )
		return;

	// Process plaintext
	if ( !m_connection.ProcessPlainTextDataChunk( nFullSequenceNumber, pDecrypted, cbDecrypted, 0, usecNow ) )
		return;

	// Process the stats, if any
	if ( pMsgStatsIn )
		RecvStats( *pMsgStatsIn, true, usecNow );
}

void CConnectionTransportP2PWebRTC::Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, SteamNetworkingMicroseconds usecNow )
{

	// We don't check the connection IDs, because we assume that WebRTC
	// has already done that sort of thing

	// Generic connection code will take it from here.
	m_connection.ConnectionState_ClosedByPeer( msg.reason_code(), msg.debug().c_str() );
}

void CConnectionTransportP2PWebRTC::Received_NoConnection( const CMsgSteamSockets_UDP_NoConnection &msg, SteamNetworkingMicroseconds usecNow )
{
	// We don't check the connection IDs, because we assume that WebRTC
	// has already done that sort of thing

	// Generic connection code will take it from here.
	m_connection.ConnectionState_ClosedByPeer( 0, nullptr );
}

void CConnectionTransportP2PWebRTC::SendConnectionClosedOrNoConnection()
{
	if ( ConnectionState() == k_ESteamNetworkingConnectionState_ClosedByPeer )
	{
		SendNoConnection();
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
		SendMsg( k_ESteamNetworkingUDPMsg_ConnectionClosed, msg );
	}
}

void CConnectionTransportP2PWebRTC::SendNoConnection()
{
	CMsgSteamSockets_UDP_NoConnection msg;
//	if ( unFromConnectionID == 0 && unToConnectionID == 0 )
//	{
//		AssertMsg( false, "Can't send NoConnection, we need at least one of from/to connection ID!" );
//		return;
//	}
//	if ( unFromConnectionID )
//		msg.set_from_connection_id( unFromConnectionID );
//	if ( unToConnectionID )
//		msg.set_to_connection_id( unToConnectionID );
	SendMsg( k_ESteamNetworkingUDPMsg_NoConnection, msg );
}

void CConnectionTransportP2PWebRTC::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	CConnectionTransport::ConnectionStateChanged( eOldState );

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

void CConnectionTransportP2PWebRTC::SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg )
{
	if ( !m_pWebRTCSession )
		return;

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

	m_pWebRTCSession->BSendData( pkt, cbPkt );
}

bool CConnectionTransportP2PWebRTC::BCanSendEndToEndData() const
{
	if ( !m_pWebRTCSession )
		return false;
	if ( m_eWebRTCSessionState != k_EWebRTCSessionStateConnected )
		return false;
	return true;
}

void CConnectionTransportP2PWebRTC::PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	for ( const CMsgSteamNetworkingP2PRendezvous_ICECandidate &c: m_vecICECandidates )
		*msg.add_ice_candidates() = c;
}

void CConnectionTransportP2PWebRTC::Log( IWebRTCSessionDelegate::ELogPriority ePriority, const char *pszMessageFormat, ... )
{
}

int CConnectionTransportP2PWebRTC::GetNumStunServers()
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();
	return m_vecStunServers.Count();
}

const char *CConnectionTransportP2PWebRTC::GetStunServer( int iIndex )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();
	if ( !m_vecStunServers.IsValidIndex( iIndex ) )
		return nullptr;
	return m_vecStunServers[ iIndex ];
}

void CConnectionTransportP2PWebRTC::OnSessionStateChanged( EWebRTCSessionState eState )
{
}

void CConnectionTransportP2PWebRTC::OnOfferReady( bool bSuccess, const char *pszOffer )
{
	// What do?
}

void CConnectionTransportP2PWebRTC::OnAnswerReady( bool bSuccess, const char *pszAnswer )
{
	// What do?
}

} // namespace SteamNetworkingSocketsLib
