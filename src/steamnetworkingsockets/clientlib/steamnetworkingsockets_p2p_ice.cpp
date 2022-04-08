//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_p2p_ice.h"
#include "steamnetworkingsockets_udp.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// CConnectionTransportP2PICE
//
/////////////////////////////////////////////////////////////////////////////

CConnectionTransportP2PICE::CConnectionTransportP2PICE( CSteamNetworkConnectionP2P &connection )
: CConnectionTransportUDPBase( connection )
, CConnectionTransportP2PBase( "ICE", this )
{
	m_nAllowedCandidateTypes = 0;
	m_eCurrentRouteKind = k_ESteamNetTransport_Unknown;
	m_currentRouteRemoteAddress.Clear();
}

CConnectionTransportP2PICE::~CConnectionTransportP2PICE()
{
}

void CConnectionTransportP2PICE::TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const
{
	CConnectionTransport::TransportPopulateConnectionInfo( info );

	info.m_addrRemote = m_currentRouteRemoteAddress;
	switch ( m_eCurrentRouteKind )
	{
		default:
		case k_ESteamNetTransport_SDRP2P:
		case k_ESteamNetTransport_Unknown:
			// Hm...
			Assert( false );
			break;

		case k_ESteamNetTransport_LocalHost:
			info.m_nFlags |= k_nSteamNetworkConnectionInfoFlags_Fast;
			break;

		case k_ESteamNetTransport_UDP:
			break;

		case k_ESteamNetTransport_UDPProbablyLocal:
		{
			int nPingMin, nPingMax;
			m_pingEndToEnd.GetPingRangeFromRecentBuckets( nPingMin, nPingMax, SteamNetworkingSockets_GetLocalTimestamp() );
			if ( nPingMin < k_nMinPingTimeLocalTolerance )
				info.m_nFlags |= k_nSteamNetworkConnectionInfoFlags_Fast;
			break;
		}

		case k_ESteamNetTransport_TURN:
			info.m_nFlags |= k_nSteamNetworkConnectionInfoFlags_Relayed;
			info.m_addrRemote.Clear();
			break;
	}
}

void CConnectionTransportP2PICE::GetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow )
{
	CConnectionTransportUDPBase::GetDetailedConnectionStatus( stats, usecNow );
	stats.m_eTransportKind = m_eCurrentRouteKind;
	if ( stats.m_eTransportKind == k_ESteamNetTransport_UDPProbablyLocal && !( stats.m_info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Fast ) )
		stats.m_eTransportKind = k_ESteamNetTransport_UDP;
}

// Base-64 encode the least significant 30 bits.
// Returns a 5-character base-64 string
std::string Base64EncodeLower30Bits( uint32 nNum )
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

void CConnectionTransportP2PICE::PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	msg.set_ice_enabled( true );
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

	// Check for recording the initial scoring data used to make the initial decision
	CMsgSteamNetworkingICESessionSummary &ice_summary = Connection().m_msgICESessionSummary;
	uint32 nScore = m_routeMetrics.m_nScoreCurrent + m_routeMetrics.m_nTotalPenalty;
	if (
		ConnectionState() == k_ESteamNetworkingConnectionState_FindingRoute
		|| !ice_summary.has_initial_ping()
		|| ( nScore < ice_summary.initial_score() && usecNow < Connection().m_usecWhenCreated + 15*k_nMillion )
	) {
		ice_summary.set_initial_score( nScore );
		ice_summary.set_initial_ping( m_pingEndToEnd.m_nSmoothedPing );
		ice_summary.set_initial_route_kind( m_eCurrentRouteKind );
	}

	if ( !ice_summary.has_best_score() || nScore < ice_summary.best_score() )
	{
		ice_summary.set_best_score( nScore );
		ice_summary.set_best_ping( m_pingEndToEnd.m_nSmoothedPing );
		ice_summary.set_best_route_kind( m_eCurrentRouteKind );
		ice_summary.set_best_time( ( usecNow - Connection().m_usecWhenCreated + 500*1000 ) / k_nMillion );
	}
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
	if ( m_bNeedToConfirmEndToEndConnectivity && BCanSendEndToEndData() )
		P2PTransportEndToEndConnectivityConfirmed( ctx.m_usecNow );
}

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
