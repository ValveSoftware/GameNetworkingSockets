//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_p2p_ice.h"
#include "steamnetworkingsockets_udp.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

extern "C" {
CreateICESession_t g_SteamNetworkingSockets_CreateICESessionFunc = nullptr;
}

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// CConnectionTransportP2PSDR
//
/////////////////////////////////////////////////////////////////////////////

CConnectionTransportP2PICE::CConnectionTransportP2PICE( CSteamNetworkConnectionP2P &connection )
: CConnectionTransportUDPBase( connection )
, CConnectionTransportP2PBase( "ICE", this, this )
, m_pICESession( nullptr )
{
	//m_eCurrentRouteLocalCandidateType = k_EICECandidate_Invalid;
	//m_eCurrentRouteRemoteCandidateType = k_EICECandidate_Invalid;
	m_eCurrentRouteKind = k_ESteamNetTransport_Unknown;
	m_currentRouteRemoteAddress.Clear();
}

CConnectionTransportP2PICE::~CConnectionTransportP2PICE()
{
	Assert( !m_pICESession );
}

void CConnectionTransportP2PICE::TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const
{
	CConnectionTransport::TransportPopulateConnectionInfo( info );

	info.m_addrRemote = m_currentRouteRemoteAddress;
	info.m_eTransportKind = m_eCurrentRouteKind;

	// If we thought the route was local, but ping time is too high, then clear local flag.
	// (E.g. VPN)
	if ( info.m_eTransportKind == k_ESteamNetTransport_UDPProbablyLocal )
	{
		int nPingMin, nPingMax;
		m_pingEndToEnd.GetPingRangeFromRecentBuckets( nPingMin, nPingMax, SteamNetworkingSockets_GetLocalTimestamp() );
		if ( nPingMin >= k_nMinPingTimeLocalTolerance )
			info.m_eTransportKind = k_ESteamNetTransport_UDP;
	}
}

void CConnectionTransportP2PICE::GetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow )
{
	// FIXME Need to indicate whether we are relayed or were able to pierce NAT
	CConnectionTransport::GetDetailedConnectionStatus( stats, usecNow );
}

// Base-64 encode the least significant 30 bits.
// Returns a 5-character base-64 string
static std::string Base64EncodeLower30Bits( uint32 nNum )
{
	static const char szBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	char result[6] = {
		szBase64Chars[ ( nNum >> 24 ) & 63 ],
		szBase64Chars[ ( nNum >> 18 ) & 63 ],
		szBase64Chars[ ( nNum >> 12 ) & 63 ],
		szBase64Chars[ ( nNum >>  6 ) & 63 ],
		szBase64Chars[ ( nNum       ) & 63 ],
		'\0'
	};
	return std::string( result );
}

void CConnectionTransportP2PICE::TransportFreeResources()
{
	if ( m_pICESession )
	{
		m_pICESession->Destroy();
		m_pICESession = nullptr;
	}
	ClearNextThinkTime();
	find_and_remove_element( Connection().m_vecAvailableTransports, this );

	CConnectionTransport::TransportFreeResources();
}

void CConnectionTransportP2PICE::Init()
{
	if ( !g_SteamNetworkingSockets_CreateICESessionFunc )
	{
		Connection().ICEFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "CreateICESession factory not set" );
		return;
	}

	SteamDatagramTransportLock::SetLongLockWarningThresholdMS( "CConnectionTransportP2PICE::Init", 50 );

	ICESessionConfig cfg;

	// Generate local ufrag and password
	std::string sUfragLocal = Base64EncodeLower30Bits( ConnectionIDLocal() );
	uint32 nPwdFrag;
	CCrypto::GenerateRandomBlock( &nPwdFrag, sizeof(nPwdFrag) );
	std::string sPwdFragLocal = Base64EncodeLower30Bits( nPwdFrag );
	cfg.m_pszLocalUserFrag = sUfragLocal.c_str();
	cfg.m_pszLocalPwd = sPwdFragLocal.c_str();

	// Set role
	cfg.m_eRole = Connection().IsControllingAgent() ? k_EICERole_Controlling : k_EICERole_Controlled;

	// Get the stun server list
	std::vector<std::string> vecStunServers;
	std::vector<const char *> vecStunServersPsz;
	if ( m_connection.m_connectionConfig.m_P2P_Transport_ICE_Enable.Get() & k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Public )
	{
		SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Using STUN server list: %s\n", ConnectionDescription(), m_connection.m_connectionConfig.m_P2P_STUN_ServerList.Get().c_str() );
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

				vecStunServers.push_back( std::move( server ) );
				vecStunServersPsz.push_back( vecStunServers.rbegin()->c_str() );
			}
		}
	}
	else
	{
		SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Not using STUN servers as per P2P_Transport_ICE_Enable\n", ConnectionDescription() );
	}
	cfg.m_nStunServers = len( vecStunServersPsz );
	cfg.m_pStunServers = vecStunServersPsz.data();

	// Create the session
	m_pICESession = (*g_SteamNetworkingSockets_CreateICESessionFunc)( cfg, this, ICESESSION_INTERFACE_VERSION );
	if ( !m_pICESession )
	{
		Connection().ICEFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "CreateICESession failed" );
		return;
	}

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ETW
		m_pICESession->SetWriteEvent_setsockopt( ETW_webrtc_setsockopt );
		m_pICESession->SetWriteEvent_send( ETW_webrtc_send );
		m_pICESession->SetWriteEvent_sendto( ETW_webrtc_sendto );
	#endif

	// Queue a message to inform peer about our auth credentials.  It should
	// go out in the first signal.
	{
		CMsgSteamNetworkingP2PRendezvous_ReliableMessage msg;
		*msg.mutable_ice()->mutable_auth()->mutable_pwd_frag() = std::move( sPwdFragLocal );
		Connection().QueueSignalReliableMessage( std::move( msg ), "Initial ICE auth" );
	}
}

void CConnectionTransportP2PICE::PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	msg.set_ice_enabled( true );
}

void CConnectionTransportP2PICE::RecvRendezvous( const CMsgICERendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	// Safety
	if ( !m_pICESession )
	{
		Connection().ICEFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "No IICESession?" );
		return;
	}

	if ( msg.has_add_candidate() )
	{
		const CMsgICERendezvous_Candidate &c = msg.add_candidate();
		EICECandidateType eType = m_pICESession->AddRemoteIceCandidate( c.candidate().c_str() );
		if ( eType != k_EICECandidate_Invalid )
		{
			SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Processed remote Ice Candidate '%s' (type %d)\n", ConnectionDescription(), c.candidate().c_str(), eType );
			Connection().m_msgICESessionSummary.set_remote_candidate_types( Connection().m_msgICESessionSummary.remote_candidate_types() | eType );
		}
		else
		{
			SpewWarning( "[%s] Ignoring candidate %s\n", ConnectionDescription(), c.ShortDebugString().c_str() );
		}
	}

	if ( msg.has_auth() )
	{
		std::string sUfragRemote = Base64EncodeLower30Bits( ConnectionIDRemote() );
		const char *pszPwdFrag = msg.auth().pwd_frag().c_str();
		SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Set remote auth to %s / %s\n", ConnectionDescription(), sUfragRemote.c_str(), pszPwdFrag );
		m_pICESession->SetRemoteAuth( sUfragRemote.c_str(), pszPwdFrag );
	}
}

void CConnectionTransportP2PICE::Think( SteamNetworkingMicroseconds usecNow )
{
	// Are we dead?
	if ( !m_pICESession || Connection().m_pTransportICEPendingDelete )
	{
		Connection().CheckCleanupICE();
		// We could be deleted here!
		return;
	}

	P2PTransportThink( usecNow );
}

void CConnectionTransportP2PICE::P2PTransportUpdateRouteMetrics( SteamNetworkingMicroseconds usecNow )
{
	if ( !BCanSendEndToEndData() || m_pingEndToEnd.m_nSmoothedPing < 0 )
	{
		m_routeMetrics.SetInvalid();
		return;
	}

	int nPingMin, nPingMax;
	m_routeMetrics.m_nBucketsValid = m_pingEndToEnd.GetPingRangeFromRecentBuckets( nPingMin, nPingMax, usecNow );
	m_routeMetrics.m_nTotalPenalty = 0;

	// Set ping as the score
	m_routeMetrics.m_nScoreCurrent = m_pingEndToEnd.m_nSmoothedPing;
	m_routeMetrics.m_nScoreMin = nPingMin;
	m_routeMetrics.m_nScoreMax = nPingMax;

	// Local route?
	if ( nPingMin < k_nMinPingTimeLocalTolerance && m_eCurrentRouteKind == k_ESteamNetTransport_UDPProbablyLocal )
	{

		// Whoo whoo!  Probably NAT punched LAN

	}
	else
	{
		// Update score based on the fraction that we are going over the Internet,
		// instead of dedicated backbone links.  (E.g. all of it)
		// This should match CalculateRoutePingScorein the SDR code
		m_routeMetrics.m_nScoreCurrent += m_pingEndToEnd.m_nSmoothedPing/10;
		m_routeMetrics.m_nScoreMin += nPingMin/10;
		m_routeMetrics.m_nScoreMax += nPingMax/10;

		// And add a penalty that everybody who is not LAN uses
		m_routeMetrics.m_nTotalPenalty += k_nRoutePenaltyNotLan;
	}

	// Debug penalty
	m_routeMetrics.m_nTotalPenalty += m_connection.m_connectionConfig.m_P2P_Transport_ICE_Penalty.Get();
}

#define ParseProtobufBody( pvMsg, cbMsg, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	if ( !msgVar.ParseFromArray( pvMsg, cbMsg ) ) \
	{ \
		ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Protobuf parse failed." ); \
		return; \
	}

#define ParsePaddedPacket( pvPkt, cbPkt, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	{ \
		if ( cbPkt < k_cbSteamNetworkingMinPaddedPacketSize ) \
		{ \
			ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Packet is %d bytes, must be padded to at least %d bytes.", cbPkt, k_cbSteamNetworkingMinPaddedPacketSize ); \
			return; \
		} \
		const UDPPaddedMessageHdr *hdr =  (const UDPPaddedMessageHdr *)( pvPkt ); \
		int nMsgLength = LittleWord( hdr->m_nMsgLength ); \
		if ( nMsgLength <= 0 || int(nMsgLength+sizeof(UDPPaddedMessageHdr)) > cbPkt ) \
		{ \
			ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Invalid encoded message length %d.  Packet is %d bytes.", nMsgLength, cbPkt ); \
			return; \
		} \
		if ( !msgVar.ParseFromArray( hdr+1, nMsgLength ) ) \
		{ \
			ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Protobuf parse failed." ); \
			return; \
		} \
	}

void CConnectionTransportP2PICE::ProcessPacket( const uint8_t *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow )
{
	Assert( cbPkt >= 1 ); // Caller should have checked this
	ETW_ICEProcessPacket( m_connection.m_hConnectionSelf, cbPkt );

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
		ParsePaddedPacket( pPkt, cbPkt, CMsgSteamSockets_UDP_ConnectionClosed, msg )
		Received_ConnectionClosed( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_NoConnection )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_NoConnection, msg )
		Received_NoConnection( msg, usecNow );
	}
	else
	{
		ReportBadUDPPacketFromConnectionPeer( "packet", "Lead byte 0x%02x not a known message ID", *pPkt );
	}
}

bool CConnectionTransportP2PICE::SendPacket( const void *pkt, int cbPkt )
{
	if ( !m_pICESession )
		return false;

	ETW_ICESendPacket( m_connection.m_hConnectionSelf, cbPkt );
	if ( !m_pICESession->BSendData( pkt, cbPkt ) )
		return false;

	// Update stats
	m_connection.m_statsEndToEnd.TrackSentPacket( cbPkt );
	return true;
}

bool CConnectionTransportP2PICE::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )
{
	if ( nChunks == 1 )
	{
		Assert( (int)pChunks->iov_len == cbSendTotal );
		SendPacket( pChunks->iov_base, pChunks->iov_len );
		return false;
	}
	if ( cbSendTotal > k_cbSteamNetworkingSocketsMaxUDPMsgLen )
	{
		Assert( false );
		return false;
	}
	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	uint8 *p = pkt;
	while ( nChunks > 0 )
	{
		if ( p + pChunks->iov_len > pkt+cbSendTotal )
		{
			Assert( false );
			return false;
		}
		memcpy( p, pChunks->iov_base, pChunks->iov_len );
		p += pChunks->iov_len;
		--nChunks;
		++pChunks;
	}
	Assert( p == pkt+cbSendTotal );
	return SendPacket( pkt, p-pkt );
}

bool CConnectionTransportP2PICE::BCanSendEndToEndData() const
{
	if ( !m_pICESession )
		return false;
	if ( !m_pICESession->GetWritableState() )
		return false;
	return true;
}

void CConnectionTransportP2PICE::TrackSentStats( UDPSendPacketContext_t &ctx )
{
	CConnectionTransportUDPBase::TrackSentStats( ctx );

	// Does this count as a ping request?
	if ( ctx.msg.has_stats() || ( ctx.msg.flags() & ctx.msg.ACK_REQUEST_E2E ) )
	{
		bool bAllowDelayedReply = ( ctx.msg.flags() & ctx.msg.ACK_REQUEST_IMMEDIATE ) == 0;
		P2PTransportTrackSentEndToEndPingRequest( ctx.m_usecNow, bAllowDelayedReply );
	}
}

void CConnectionTransportP2PICE::RecvValidUDPDataPacket( UDPRecvPacketContext_t &ctx )
{
	if ( !ctx.m_pStatsIn || !( ctx.m_pStatsIn->flags() & ctx.m_pStatsIn->NOT_PRIMARY_TRANSPORT_E2E ) )
		Connection().SetPeerSelectedTransport( this );
	P2PTransportTrackRecvEndToEndPacket( ctx.m_usecNow );
	if ( m_bNeedToConfirmEndToEndConnectivity )
		P2PTransportEndToEndConnectivityConfirmed( ctx.m_usecNow );
}

void CConnectionTransportP2PICE::UpdateRoute()
{
	if ( !m_pICESession )
		return;

	// Clear ping data, it is no longer accurate
	m_pingEndToEnd.Reset();
	m_nTotalPingsSent = 0;

	IICESession::CandidateAddressString szRemoteAddress;
	EICECandidateType eLocalCandidate, eRemoteCandidate;
	if ( !m_pICESession->GetRoute( eLocalCandidate, eRemoteCandidate, szRemoteAddress ) )
	{
		SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE route is unkown\n", ConnectionDescription() );
		m_eCurrentRouteKind = k_ESteamNetTransport_Unknown;
		m_currentRouteRemoteAddress.Clear();
	}
	else
	{
		if ( !m_currentRouteRemoteAddress.ParseString( szRemoteAddress ) )
		{
			AssertMsg1( false, "IICESession::GetRoute returned invalid remote address '%s'!", szRemoteAddress );
			m_currentRouteRemoteAddress.Clear();
		}

		netadr_t netadrRemote;
		SteamNetworkingIPAddrToNetAdr( netadrRemote, m_currentRouteRemoteAddress );

		if ( ( eLocalCandidate | eRemoteCandidate ) & k_EICECandidate_Any_Relay )
		{
			m_eCurrentRouteKind = k_ESteamNetTransport_TURN;
			SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE route is via TURN to %s\n", ConnectionDescription(), szRemoteAddress );
		}
		else if ( netadrRemote.IsValid() && IsRouteToAddressProbablyLocal( netadrRemote ) )
		{
			m_eCurrentRouteKind = k_ESteamNetTransport_UDPProbablyLocal;
			SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE route proably local to %s (based on remote address)\n", ConnectionDescription(), szRemoteAddress );
		}
		else if ( ( eLocalCandidate & k_EICECandidate_Any_HostPrivate ) && ( eRemoteCandidate & k_EICECandidate_Any_HostPrivate ) )
		{
			m_eCurrentRouteKind = k_ESteamNetTransport_UDPProbablyLocal;
			SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE route is probably local to %s (based on candidate types both being private addresses)\n", ConnectionDescription(), szRemoteAddress );
		}
		else
		{
			m_eCurrentRouteKind = k_ESteamNetTransport_UDP;
			SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE route is public UDP to %s\n", ConnectionDescription(), szRemoteAddress );
		}
	}

	RouteOrWritableStateChanged();
}

void CConnectionTransportP2PICE::RouteOrWritableStateChanged()
{

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Go ahead and add a ping sample from our RTT estimate if we don't have any other data
	if ( m_pingEndToEnd.m_nSmoothedPing < 0 )
	{
		int nPing = m_pICESession->GetPing();
		if ( nPing >= 0 )
			m_pingEndToEnd.ReceivedPing( nPing, usecNow );
		else
			P2PTransportEndToEndConnectivityNotConfirmed( usecNow );
	}

	Connection().TransportEndToEndConnectivityChanged( this, usecNow );

	CMsgSteamNetworkingSocketsICESessionSummary &ice_summary = Connection().m_msgICESessionSummary;
	if (
		ConnectionState() == k_ESteamNetworkingConnectionState_FindingRoute
		|| !ice_summary.has_initial_ping()
		|| !ice_summary.has_initial_route_kind()
	) {
		if ( m_pingEndToEnd.m_nSmoothedPing >= 0 )
			ice_summary.set_initial_ping( m_pingEndToEnd.m_nSmoothedPing );
		ice_summary.set_initial_route_kind( m_eCurrentRouteKind );
	}

	// We are writable, then send a ping immediately!
	if ( BCanSendEndToEndData() )
		SendEndToEndStatsMsg( k_EStatsReplyRequest_Immediate, usecNow, "ICEWritable" );
}

/////////////////////////////////////////////////////////////////////////////
//
// IICESessionDelegate handlers
//
// NOTE: These can be invoked from any thread,
// and we won't hold the lock
//
/////////////////////////////////////////////////////////////////////////////

class IConnectionTransportP2PICERunWithLock : public ISteamNetworkingSocketsRunWithLock
{
public:
	uint32 m_nConnectionIDLocal;

	virtual void RunTransportP2PICE( CConnectionTransportP2PICE *pTransport ) = 0;
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

		if ( !pConn->m_pTransportICE )
			return;

		RunTransportP2PICE( pConn->m_pTransportICE );
	}
};


void CConnectionTransportP2PICE::Log( IICESessionDelegate::ELogPriority ePriority, const char *pszMessageFormat, ... )
{
	ESteamNetworkingSocketsDebugOutputType eType;
	switch ( ePriority )
	{
		default:	
			AssertMsg1( false, "Unknown priority %d", ePriority );
			// FALLTHROUGH

		case IICESessionDelegate::k_ELogPriorityDebug: eType = k_ESteamNetworkingSocketsDebugOutputType_Debug; break;
		case IICESessionDelegate::k_ELogPriorityVerbose: eType = k_ESteamNetworkingSocketsDebugOutputType_Verbose; break;
		case IICESessionDelegate::k_ELogPriorityInfo: eType = k_ESteamNetworkingSocketsDebugOutputType_Msg; break;
		case IICESessionDelegate::k_ELogPriorityWarning: eType = k_ESteamNetworkingSocketsDebugOutputType_Warning; break;
		case IICESessionDelegate::k_ELogPriorityError: eType = k_ESteamNetworkingSocketsDebugOutputType_Error; break;
	}

	if ( eType > Connection().LogLevel_P2PRendezvous() )
		return;

	char buf[ 1024 ];
	va_list ap;
	va_start( ap, pszMessageFormat );
	V_vsprintf_safe( buf, pszMessageFormat, ap );
	va_end( ap );

	//ReallySpewType( eType, "[%s] ICE: %s", ConnectionDescription(), buf );
	ReallySpewType( eType, "ICE: %s", buf ); // FIXME would like to get the connection description, but that's not threadsafe
}

void CConnectionTransportP2PICE::OnLocalCandidateGathered( EICECandidateType eType, const char *pszCandidate )
{

	struct RunIceCandidateAdded : IConnectionTransportP2PICERunWithLock
	{
		EICECandidateType eType;
		CMsgSteamNetworkingP2PRendezvous_ReliableMessage msg;
		virtual void RunTransportP2PICE( CConnectionTransportP2PICE *pTransport )
		{
			CSteamNetworkConnectionP2P &conn = pTransport->Connection();
			CMsgSteamNetworkingSocketsICESessionSummary &sum = conn.m_msgICESessionSummary;
			sum.set_local_candidate_types( sum.local_candidate_types() | eType );
			pTransport->Connection().QueueSignalReliableMessage( std::move(msg), "LocalCandidateAdded" );
		}
	};

	RunIceCandidateAdded *pRun = new RunIceCandidateAdded;
	pRun->eType = eType;
	pRun->m_nConnectionIDLocal = m_connection.m_unConnectionIDLocal;
	CMsgICERendezvous_Candidate &c = *pRun->msg.mutable_ice()->mutable_add_candidate();
	c.set_candidate( pszCandidate );
	pRun->RunOrQueue( "ICE OnIceCandidateAdded" );
}

void CConnectionTransportP2PICE::DrainPacketQueue( SteamNetworkingMicroseconds usecNow )
{
	// Quickly swap into temp
	CUtlBuffer buf;
	m_mutexPacketQueue.lock();
	buf.Swap( m_bufPacketQueue );
	m_mutexPacketQueue.unlock();

	//SpewMsg( "CConnectionTransportP2PICE::DrainPacketQueue: %d bytes queued\n", buf.TellPut() );

	// Process all the queued packets
	uint8 *p = (uint8*)buf.Base();
	uint8 *end = p + buf.TellPut();
	while ( p < end )
	{
		if ( p+sizeof(int) > end )
		{
			Assert(false);
			break;
		}
		int cbPkt = *(int*)p;
		p += sizeof(int);
		if ( p + cbPkt > end )
		{
			// BUG!
			Assert(false);
			break;
		}
		ProcessPacket( p, cbPkt, usecNow );
		p += cbPkt;
	}
}

void CConnectionTransportP2PICE::OnWritableStateChanged()
{
	struct RunWritableStateChanged : IConnectionTransportP2PICERunWithLock
	{
		virtual void RunTransportP2PICE( CConnectionTransportP2PICE *pTransport )
		{
			// Are we writable right now?
			if ( pTransport->BCanSendEndToEndData() )
			{

				// Just spew
				SpewMsgGroup( pTransport->LogLevel_P2PRendezvous(), "[%s] ICE reports we are writable\n", pTransport->ConnectionDescription() );

				// Re-calculate some stuff if this is news
				if ( pTransport->m_bNeedToConfirmEndToEndConnectivity )
					pTransport->RouteOrWritableStateChanged();
			}
			else
			{

				// We're not writable.  Is this news to us?
				if ( !pTransport->m_bNeedToConfirmEndToEndConnectivity )
				{

					// We thought we were good.  Clear flag, we are in doubt
					SpewMsgGroup( pTransport->LogLevel_P2PRendezvous(), "[%s] ICE reports we are no longer writable\n", pTransport->ConnectionDescription() );
					pTransport->P2PTransportEndToEndConnectivityNotConfirmed( SteamNetworkingSockets_GetLocalTimestamp() );
				}
			}
		}
	};

	RunWritableStateChanged *pRun = new RunWritableStateChanged;
	pRun->m_nConnectionIDLocal = m_connection.m_unConnectionIDLocal;
	pRun->RunOrQueue( "ICE OnWritableStateChanged" );
}

void CConnectionTransportP2PICE::OnRouteChanged()
{
	struct RunRouteStateChanged : IConnectionTransportP2PICERunWithLock
	{
		virtual void RunTransportP2PICE( CConnectionTransportP2PICE *pTransport )
		{
			pTransport->UpdateRoute();
		}
	};

	RunRouteStateChanged *pRun = new RunRouteStateChanged;
	pRun->m_nConnectionIDLocal = m_connection.m_unConnectionIDLocal;
	pRun->RunOrQueue( "ICE OnRouteChanged" );
}

void CConnectionTransportP2PICE::OnData( const void *pPkt, size_t nSize )
{
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	const int cbPkt = int(nSize);

	if ( nSize < 1 )
	{
		ReportBadUDPPacketFromConnectionPeer( "packet", "Bad packet size: %d", cbPkt );
		return;
	}

	// See if we can process this packet (and anything queued before us)
	// immediately
	if ( SteamDatagramTransportLock::TryLock( "ICE Data", 0 ) )
	{
		// We can process the data now!
		//SpewMsg( "CConnectionTransportP2PICE::OnData %d bytes, process immediate\n", (int)nSize );

		// Check if queue is empty.  Note that no race conditions here.  We hold the lock,
		// which means we aren't messing with it in some other thread.  And we are in WebRTC's
		// callback, and we assume WebRTC will not call us from two threads at the same time.
		if ( m_bufPacketQueue.TellPut() > 0 )
		{
			DrainPacketQueue( usecNow );
			Assert( m_bufPacketQueue.TellPut() == 0 );
		}

		// And now process this packet
		ProcessPacket( (const uint8_t*)pPkt, cbPkt, usecNow );
		SteamDatagramTransportLock::Unlock();
		return;
	}

	// We're busy in the other thread.  We'll have to queue the data.
	// Grab the buffer lock
	m_mutexPacketQueue.lock();
	int nSaveTellPut = m_bufPacketQueue.TellPut();
	m_bufPacketQueue.PutInt( cbPkt );
	m_bufPacketQueue.Put( pPkt, cbPkt );
	m_mutexPacketQueue.unlock();

	// If the queue was empty,then we need to add a task to flush it
	// when we acquire the queue.  If it wasn't empty then a task is
	// already in the queue.  Or perhaps it was progress right now
	// in some other thread.  But if that were the case, we know that
	// it had not yet actually swapped the buffer out.  Because we had
	// the buffer lock when we checked if the queue was empty.
	if ( nSaveTellPut == 0 )
	{
		//SpewMsg( "CConnectionTransportP2PICE::OnData %d bytes, queued, added drain queue task\n", (int)nSize );
		struct RunDrainQueue : IConnectionTransportP2PICERunWithLock
		{
			virtual void RunTransportP2PICE( CConnectionTransportP2PICE *pTransport )
			{
				pTransport->DrainPacketQueue( SteamNetworkingSockets_GetLocalTimestamp() );
			}
		};

		RunDrainQueue *pRun = new RunDrainQueue;
		pRun->m_nConnectionIDLocal = m_connection.m_unConnectionIDLocal;

		// Queue it.  Don't use RunOrQueue.  We know we need to queue it,
		// since we already tried to grab the lock and failed.
		pRun->Queue( "ICE DrainQueue" );
	}
	else
	{
		if ( nSaveTellPut > 30000 )
		{
			SpewMsg( "CConnectionTransportP2PICE::OnData %d bytes, queued, %d previously queued LOCK PROBLEM!\n", (int)nSize, nSaveTellPut );
		}
		else
		{
			//SpewMsg( "CConnectionTransportP2PICE::OnData %d bytes, queued, %d previously queued\n", (int)nSize, nSaveTellPut );
		}
	}
}

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
