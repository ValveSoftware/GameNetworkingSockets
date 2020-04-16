//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_p2p_webrtc.h"
#include "steamnetworkingsockets_udp.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC

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
, m_bWaitingOnOffer( false )
, m_bWaitingOnAnswer( false )
, m_bNeedToSendAnswer( false )
, m_pszNeedToSendSignalReason( nullptr )
, m_usecSendSignalDeadline( INT64_MAX )
, m_nRemoteCandidatesRevision( 0 )
, m_nLocalCandidatesRevision( 0 )
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

void CConnectionTransportP2PWebRTC::Init()
{
	{
		CUtlVectorAutoPurge<char *> tempStunServers;
		V_AllocAndSplitString( m_connection.m_connectionConfig.m_P2P_STUN_ServerList.Get().c_str(), ",", tempStunServers );
		for ( const char *pszAddress: tempStunServers )
		{
			std::string server;

			// Add prefix, unless they already supplied it
			if ( V_strnicmp( pszAddress, "stun:", 5 ) != 0 )
				server = "stun:";
			server.append( pszAddress );

			m_vecStunServers.push_back( std::move( server ) );
		}
	}

	m_pWebRTCSession = ::CreateWebRTCSession( this );
	if ( !m_pWebRTCSession )
	{
		NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "CreateWebRTCSession failed" );
		return;
	}

	if ( !m_pWebRTCSession->BAddDataChannel( false ) )
	{
		NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "BAddDataChannel failed" );
		return;
	}

	// Fetch the state, make sure we're OK
	m_eWebRTCSessionState = m_pWebRTCSession->GetState();
	if ( m_eWebRTCSessionState != k_EWebRTCSessionStateConnecting && m_eWebRTCSessionState != k_EWebRTCSessionStateNew )
	{
		char errMsg[ 256 ];
		V_sprintf_safe( errMsg, "WebRTC session state is %d", m_eWebRTCSessionState );
		NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, errMsg );
		return;
	}

	// If we are accepting a connection, then create the offer
	if ( m_connection.m_bConnectionInitiatedRemotely )
	{
		if ( !m_pWebRTCSession->BCreateOffer() )
		{
			NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "BCreateOffer failed" );
			return;
		}
		SpewType( LogLevel_P2PRendezvous(), "[%s] Creating offer\n", ConnectionDescription() );
	}
	else
	{
		m_bWaitingOnOffer = true;
	}

}

void CConnectionTransportP2PWebRTC::PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	m_pszNeedToSendSignalReason = nullptr;
	m_usecSendSignalDeadline = INT64_MAX;

	CMsgWebRTCRendezvous *pMsgWebRTC = msg.mutable_webrtc();

	if ( !m_local_offer.empty() )
		pMsgWebRTC->set_offer( m_local_offer );
	if ( !m_local_answer.empty() && m_bNeedToSendAnswer )
	{
		m_bNeedToSendAnswer = false;
		pMsgWebRTC->set_answer( m_local_answer );
	}

	// Any un-acked candidates that we are ready to (re)try
	for ( LocalCandidate &s: m_vecLocalUnAckedCandidates )
	{

		// Not yet ready to retry sending?
		if ( !pMsgWebRTC->has_first_candidate_revision() )
		{
			if ( s.m_usecRTO > usecNow )
				continue; // We've sent.  Don't give up yet.

			// Start sending from this guy forward
			pMsgWebRTC->set_first_candidate_revision( s.m_nRevision );
		}

		*pMsgWebRTC->add_candidates() = s.candidate;

		s.m_usecRTO = usecNow + k_nMillion/2; // Reset RTO

		// If we have a ton of candidates, don't send
		// too many in a single message
		if ( pMsgWebRTC->candidates_size() > 10 )
			break;
	}

	// Go ahead and always ack, even if we don't need to, because this is small
	if ( m_nRemoteCandidatesRevision > 0 )
		pMsgWebRTC->set_ack_candidates_revision( m_nRemoteCandidatesRevision );

}

void CConnectionTransportP2PWebRTC::RecvRendezvous( const CMsgWebRTCRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	// Safety
	if ( !m_pWebRTCSession )
	{
		NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "No IWebRTCSession?" );
		return;
	}

	// Check for receiving the offer
	if ( msg.has_offer() )
	{
		// Make sure we send back an answer as soon as we have one
		m_bNeedToSendAnswer = true;

		// Did we already get our answer?
		if ( !m_local_answer.empty() )
		{
			// Retry send answer
			ScheduleSendSignal( "ReplyAnswer" );
		}
		else if ( m_bWaitingOnOffer )
		{
			m_bWaitingOnOffer = false;
			if ( !m_pWebRTCSession->BCreateAnswer( msg.offer().c_str() ) )
			{
				NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "BCreateAnswer failed" );
				return;
			}
		}
		else
		{
			// We're waiting on answer from local WebRTC system.  Keep waiting
		}
	}

	// Check for receiving the answer
	if ( msg.has_answer() )
	{

		// We've got our answer.  Make sure we stop sending the offer
		m_local_offer.clear();

		// Only process the answer once
		if ( m_bWaitingOnAnswer )
		{
			m_bWaitingOnAnswer = false;
			if ( !m_pWebRTCSession->BSetAnswer( msg.answer().c_str() ) )
			{
				NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "BSetAnswer failed" );
				return;
			}
		}
	}

	// Check if they are acking that they have received candidates
	if ( msg.has_ack_candidates_revision() )
	{

		// Remove any candidates from our list that are being acked
		while ( !m_vecLocalUnAckedCandidates.empty() && m_vecLocalUnAckedCandidates[0].m_nRevision <= msg.ack_candidates_revision() )
			erase_at( m_vecLocalUnAckedCandidates, 0 );

		// Check anything ready to retry now
		for ( const LocalCandidate &s: m_vecLocalUnAckedCandidates )
		{
			if ( s.m_usecRTO < usecNow )
			{
				ScheduleSendSignal( "SendCandidates" );
				break;
			}
		}
	}

	// Check if they sent candidate update.
	if ( msg.has_first_candidate_revision() )
	{

		// Send an ack, no matter what
		ScheduleSendSignal( "AckCandidatesRevision" );

		// Only process them if it was the next chunk we were expecting.
		if ( msg.first_candidate_revision() == m_nRemoteCandidatesRevision+1 )
		{

			// Take the update
			for ( const CMsgWebRTCRendezvous_Candidate &c: msg.candidates() )
			{
				if ( m_pWebRTCSession->BAddRemoteIceCandidate( c.sdpm_id().c_str(), c.sdpm_line_index(), c.candidate().c_str() ) )
				{
					SpewType( LogLevel_P2PRendezvous(), "[%s] Processed remote Ice Candidate %s\n", ConnectionDescription(), c.ShortDebugString().c_str() );
				}
				else
				{
					SpewWarning( "[%s] Ignoring candidate %s\n", ConnectionDescription(), c.ShortDebugString().c_str() );
				}
				++m_nRemoteCandidatesRevision;
			}
		}
	}
}

void CConnectionTransportP2PWebRTC::NotifyConnectionFailed( int nReasonCode, const char *pszReason )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	// Mark state as failed, if we didn't already set a more specific state
	if ( m_eWebRTCSessionState == k_EWebRTCSessionStateNew || m_eWebRTCSessionState == k_EWebRTCSessionStateConnecting || m_eWebRTCSessionState == k_EWebRTCSessionStateConnected )
		m_eWebRTCSessionState = k_EWebRTCSessionStateFailed;

	// Remember reason code, if we didn't already set one
	if ( Connection().m_nWebRTCCloseCode == 0 )
	{
		SpewType( LogLevel_P2PRendezvous(), "[%s] WebRTC failed %d %s\n", ConnectionDescription(), nReasonCode, pszReason );
		Connection().m_nWebRTCCloseCode = nReasonCode;
		V_strcpy_safe( Connection().m_szWebRTCCloseMsg, pszReason );
	}

	// Go ahead and free up our WebRTC session now, it is reference counted.
	if ( m_pWebRTCSession )
	{
		m_pWebRTCSession->Release();
		m_pWebRTCSession = nullptr;
	}

	// Queue us for deletion
	if ( Connection().m_pTransportP2PWebRTCPendingDelete )
	{
		// Already queued for delete
		Assert( Connection().m_pTransportP2PWebRTCPendingDelete == this );
	}
	else
	{
		Connection().m_pTransportP2PWebRTCPendingDelete = this;
		Assert( Connection().m_pTransportP2PWebRTC == this );
		Connection().m_pTransportP2PWebRTC = nullptr;
	}
	Connection().SetNextThinkTimeASAP();
}

void CConnectionTransportP2PWebRTC::ScheduleSendSignal( const char *pszReason )
{
	SteamNetworkingMicroseconds usecDeadline = SteamNetworkingSockets_GetLocalTimestamp() + 10*1000;
	if ( !m_pszNeedToSendSignalReason || m_usecSendSignalDeadline > usecDeadline )
	{
		m_pszNeedToSendSignalReason = pszReason;
		m_usecSendSignalDeadline = usecDeadline;
	}
	Connection().EnsureMinThinkTime( m_usecSendSignalDeadline );
}

void CConnectionTransportP2PWebRTC::CheckSendSignal( SteamNetworkingMicroseconds usecNow )
{
	if ( m_pWebRTCSession )
	{
		char hello[] = "hello";
		m_pWebRTCSession->BSendData( (const uint8_t *)hello, sizeof(hello) );
		m_connection.EnsureMinThinkTime( usecNow + k_nMillion/10 );
	}

	if ( usecNow < m_usecSendSignalDeadline )
	{
		if ( m_vecLocalUnAckedCandidates.empty() || m_vecLocalUnAckedCandidates[0].m_usecRTO > usecNow )
			return;
		m_pszNeedToSendSignalReason = "CandidateRTO";
	}

	// Send a signal
	CMsgSteamNetworkingP2PRendezvous msgRendezvous;
	Connection().SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, m_pszNeedToSendSignalReason );

	Assert( m_usecSendSignalDeadline > usecNow );

	SteamNetworkingMicroseconds usecNextSignal = m_usecSendSignalDeadline;
	if ( !m_vecLocalUnAckedCandidates.empty() && m_vecLocalUnAckedCandidates[0].m_usecRTO > 0 )
		usecNextSignal = std::min( usecNextSignal, m_vecLocalUnAckedCandidates[0].m_usecRTO );

	m_connection.EnsureMinThinkTime( usecNextSignal );
}

void CConnectionTransportP2PWebRTC::SendStatsMsg( EStatsReplyRequest eReplyRequested, SteamNetworkingMicroseconds usecNow, const char *pszReason )
{
	UDPSendPacketContext_t ctx( usecNow, pszReason );
	ctx.Populate( sizeof(WebRTCDataMsgHdr), eReplyRequested, m_connection );

	// Send a data packet (maybe containing ordinary data), with this piggy backed on top of it
	m_connection.SNP_SendPacket( this, ctx );
}

void CConnectionTransportP2PWebRTC::SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason )
{
	SendStatsMsg( eRequest, usecNow, pszReason );
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
	return m_connection.SNP_SendPacket( this, ctx );
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
	// FIXME This is terrible for perf, and doesn't work if we are being destroyed in another thread!
	SteamDatagramTransportLock scopeLock( "OnData" );

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

void CConnectionTransportP2PWebRTC::TransportConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	CConnectionTransport::TransportConnectionStateChanged( eOldState );

	switch ( ConnectionState() )
	{
		default:
			Assert( false );
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Dead:
			break;


		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			SendNoConnection();
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

/////////////////////////////////////////////////////////////////////////////
//
// IWebRTCSessionDelegate handlers
//
// NOTE: These can be invoked from any thread,
// and we won't hold the lock
//
/////////////////////////////////////////////////////////////////////////////

class IConnectionTransportP2PWebRTCRunWithLock : public ISteamNetworkingSocketsRunWithLock
{
public:
	uint32 m_nConnectionIDLocal;

	virtual void RunWebRTC( CConnectionTransportP2PWebRTC *pTransport ) = 0;
private:
	virtual void Run()
	{
		CSteamNetworkConnectionBase *pConnBase = FindConnectionByLocalID( m_nConnectionIDLocal );
		if ( !pConnBase )
			return;

		// FIXME RTTI!
		CSteamNetworkConnectionP2P *pConn = dynamic_cast<CSteamNetworkConnectionP2P *>( pConnBase );
		if ( !pConn )
			return;

		if ( !pConn->m_pTransportP2PWebRTC )
			return;

		RunWebRTC( pConn->m_pTransportP2PWebRTC );
	}
};


void CConnectionTransportP2PWebRTC::Log( IWebRTCSessionDelegate::ELogPriority ePriority, const char *pszMessageFormat, ... )
{
	ESteamNetworkingSocketsDebugOutputType eType;
	switch ( ePriority )
	{
		default:	
			AssertMsg1( false, "Unknown priority %d", ePriority );
		case IWebRTCSessionDelegate::k_ELogPriorityDebug: eType = k_ESteamNetworkingSocketsDebugOutputType_Debug; break;
		case IWebRTCSessionDelegate::k_ELogPriorityVerbose: eType = k_ESteamNetworkingSocketsDebugOutputType_Verbose; break;
		case IWebRTCSessionDelegate::k_ELogPriorityInfo: eType = k_ESteamNetworkingSocketsDebugOutputType_Msg; break;
		case IWebRTCSessionDelegate::k_ELogPriorityWarning: eType = k_ESteamNetworkingSocketsDebugOutputType_Warning; break;
		case IWebRTCSessionDelegate::k_ELogPriorityError: eType = k_ESteamNetworkingSocketsDebugOutputType_Error; break;
	}

	if ( eType > g_eSteamDatagramDebugOutputDetailLevel )
		return;

	// FIXME Warning!  This can be called from any thread

	char buf[ 1024 ];
	va_list ap;
	va_start( ap, pszMessageFormat );
	V_vsprintf_safe( buf, pszMessageFormat, ap );
	va_end( ap );

	//ReallySpewType( eType, "[%s] WebRTC: %s", ConnectionDescription(), buf );
	ReallySpewType( eType, "WebRTC: %s", buf ); // FIXME would like to get the connection description
}

int CConnectionTransportP2PWebRTC::GetNumStunServers()
{
	return len( m_vecStunServers );
}

const char *CConnectionTransportP2PWebRTC::GetStunServer( int iIndex )
{
	if ( iIndex < 0 || iIndex >= len( m_vecStunServers ) )
		return nullptr;
	return m_vecStunServers[ iIndex ].c_str();
}

void CConnectionTransportP2PWebRTC::OnSessionStateChanged( EWebRTCSessionState eState )
{
	struct RunOnSessionStateChanged : IConnectionTransportP2PWebRTCRunWithLock
	{
		virtual void RunWebRTC( CConnectionTransportP2PWebRTC *pTransport )
		{
			if ( !pTransport->m_pWebRTCSession )
				return;
			pTransport->m_eWebRTCSessionState = pTransport->m_pWebRTCSession->GetState();
			switch ( pTransport->m_eWebRTCSessionState )
			{
				case k_EWebRTCSessionStateConnecting:
				case k_EWebRTCSessionStateConnected:
					break;

				case k_EWebRTCSessionStateDisconnected:
					pTransport->NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_Timeout, "WebRTC disconnected" );
					break;

				case k_EWebRTCSessionStateFailed:
					pTransport->NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_Generic, "WebRTC failed" );
					break;

				case k_EWebRTCSessionStateClosed:
					pTransport->NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_Generic, "WebRTC closed" );
					break;
			}
		}
	};
	RunOnSessionStateChanged *pRun = new RunOnSessionStateChanged;
	pRun->m_nConnectionIDLocal = m_connection.m_unConnectionIDLocal;
	pRun->RunOrQueue( "WebRTC OnSessionStateChanged" );
}

void CConnectionTransportP2PWebRTC::OnOfferReady( bool bSuccess, const char *pszOffer )
{
	struct RunOnOfferReady : IConnectionTransportP2PWebRTCRunWithLock
	{
		bool bSuccess;
		std::string offer;
		virtual void RunWebRTC( CConnectionTransportP2PWebRTC *pTransport )
		{
			if ( !bSuccess )
			{
				pTransport->NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "OnOfferReady failed" );
				return;
			}

			SpewType( pTransport->LogLevel_P2PRendezvous(), "[%s] WebRTC OnOfferReady %s\n", pTransport->ConnectionDescription(), offer.c_str() );

			pTransport->m_local_offer = std::move( offer );
			pTransport->m_bWaitingOnAnswer = true;
			pTransport->ScheduleSendSignal( "WebRTCOfferReady" );
		}
	};

	RunOnOfferReady *pRun = new RunOnOfferReady;
	pRun->m_nConnectionIDLocal = m_connection.m_unConnectionIDLocal;
	pRun->bSuccess = bSuccess;
	if ( bSuccess )
		pRun->offer = pszOffer;
	pRun->RunOrQueue( "WebRTC OnOfferReady" );
}

void CConnectionTransportP2PWebRTC::OnAnswerReady( bool bSuccess, const char *pszAnswer )
{
	struct RunOnAnswerReady : IConnectionTransportP2PWebRTCRunWithLock
	{
		bool bSuccess;
		std::string answer;
		virtual void RunWebRTC( CConnectionTransportP2PWebRTC *pTransport )
		{
			if ( !bSuccess )
			{
				pTransport->NotifyConnectionFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "OnAnswerReady failed" );
				return;
			}

			SpewType( pTransport->LogLevel_P2PRendezvous(), "[%s] WebRTC OnAnswerReady %s\n", pTransport->ConnectionDescription(), answer.c_str() );

			pTransport->m_local_answer = std::move( answer );
			pTransport->ScheduleSendSignal( "WebRTCAnswerReady" );
		}
	};

	RunOnAnswerReady *pRun = new RunOnAnswerReady;
	pRun->m_nConnectionIDLocal = m_connection.m_unConnectionIDLocal;
	pRun->bSuccess = bSuccess;
	if ( bSuccess )
		pRun->answer = pszAnswer;
	pRun->RunOrQueue( "WebRTC OnAnswerReady" );
}

void CConnectionTransportP2PWebRTC::OnIceCandidateAdded( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate )
{
	struct RunIceCandidateAdded : IConnectionTransportP2PWebRTCRunWithLock
	{
		CMsgWebRTCRendezvous_Candidate candidate;
		virtual void RunWebRTC( CConnectionTransportP2PWebRTC *pTransport )
		{
			SpewType( pTransport->LogLevel_P2PRendezvous(), "[%s] WebRTC OnIceCandidateAdded %s\n", pTransport->ConnectionDescription(), candidate.ShortDebugString().c_str() );

			pTransport->ScheduleSendSignal( "WebRTCCandidateAdded" );

			// Add to list of candidates that peer doesn't know about, and bump revision
			CConnectionTransportP2PWebRTC::LocalCandidate *c = push_back_get_ptr( pTransport->m_vecLocalUnAckedCandidates );
			c->m_nRevision = ++pTransport->m_nLocalCandidatesRevision;
			c->candidate = std::move( candidate );
			c->m_usecRTO = 0;
		}
	};

	RunIceCandidateAdded *pRun = new RunIceCandidateAdded;
	pRun->m_nConnectionIDLocal = m_connection.m_unConnectionIDLocal;
	pRun->candidate.set_sdpm_id( pszSDPMid );
	pRun->candidate.set_sdpm_line_index( nSDPMLineIndex );
	pRun->candidate.set_candidate( pszCandidate );
	pRun->RunOrQueue( "WebRTC OnIceCandidateAdded" );
}

void CConnectionTransportP2PWebRTC::OnIceCandidatesComplete( const char *pszCandidates )
{
	// FIXME not thread safe
	SpewType( LogLevel_P2PRendezvous(), "[%s] OnIceCandidatesComplete\n", ConnectionDescription() );

	char hello[] = "hello";
	m_pWebRTCSession->BSendData( (const uint8_t *)hello, sizeof(hello) );
}

void CConnectionTransportP2PWebRTC::OnSendPossible()
{
	// FIXME not thread safe
	SpewType( LogLevel_P2PRendezvous(), "[%s] OnSendPossible\n", ConnectionDescription() );

	char hello[] = "hello";
	m_pWebRTCSession->BSendData( (const uint8_t *)hello, sizeof(hello) );
}

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC
