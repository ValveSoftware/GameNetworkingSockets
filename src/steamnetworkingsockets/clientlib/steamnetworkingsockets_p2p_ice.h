//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_P2P_ICE_H
#define STEAMNETWORKINGSOCKETS_P2P_ICE_H
#pragma once

#include "steamnetworkingsockets_p2p.h"
#include "steamnetworkingsockets_udp.h"
#include <mutex>

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#include "../../external/steamwebrtc/ice_session.h"

extern "C" CreateICESession_t g_SteamNetworkingSockets_CreateICESessionFunc;

namespace SteamNetworkingSocketsLib {

constexpr int k_nMinPingTimeLocalTolerance = 5;

class CSteamNetworkConnectionP2P;
struct UDPSendPacketContext_t;

/// Transport for peer-to-peer connection using WebRTC
class CConnectionTransportP2PICE final
: public CConnectionTransportUDPBase
, public CConnectionTransportP2PBase
, private IICESessionDelegate
{
public:
	CConnectionTransportP2PICE( CSteamNetworkConnectionP2P &connection );
	virtual ~CConnectionTransportP2PICE();

	inline CSteamNetworkConnectionP2P &Connection() const { return *assert_cast< CSteamNetworkConnectionP2P *>( &m_connection ); }
	inline ISteamNetworkingConnectionSignaling *Signaling() const { return Connection().m_pSignaling; }

	void Init();

	// CConnectionTransport overrides
	virtual void TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const override;
	virtual void GetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow ) override;
	virtual void TransportFreeResources() override;
	virtual bool BCanSendEndToEndData() const override;

	// CConnectionTransportP2PBase
	virtual void P2PTransportUpdateRouteMetrics( SteamNetworkingMicroseconds usecNow ) override;
	virtual void P2PTransportThink( SteamNetworkingMicroseconds usecNow ) override;

	/// Fill in SDR-specific fields to signal
	void PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow );
	void RecvRendezvous( const CMsgICERendezvous &msg, SteamNetworkingMicroseconds usecNow );

	inline int LogLevel_P2PRendezvous() const { return m_connection.m_connectionConfig.m_LogLevel_P2PRendezvous.Get(); }

	// In certain circumstances we may need to buffer packets
	std::mutex m_mutexPacketQueue;
	CUtlBuffer m_bufPacketQueue;

	//EICECandidateType m_eCurrentRouteLocalCandidateType;
	//EICECandidateType m_eCurrentRouteRemoteCandidateType;
	SteamNetworkingIPAddr m_currentRouteRemoteAddress;
	ESteamNetTransportKind m_eCurrentRouteKind;
	int m_nAllowedCandidateTypes; // k_EICECandidate_xxx

private:
	IICESession *m_pICESession;

	// Implements IICESessionDelegate
	virtual void Log( IICESessionDelegate::ELogPriority ePriority, const char *pszMessageFormat, ... ) override;
	virtual void OnData( const void *pData, size_t nSize ) override;
	virtual void OnLocalCandidateGathered( EICECandidateType eType, const char *pszCandidate ) override;
	virtual void OnWritableStateChanged() override;
	virtual void OnRouteChanged() override;

	void RouteOrWritableStateChanged();
	void UpdateRoute();

	void DrainPacketQueue( SteamNetworkingMicroseconds usecNow );
	void ProcessPacket( const uint8_t *pData, int cbPkt, SteamNetworkingMicroseconds usecNow );

	// Implements CConnectionTransportUDPBase
	virtual bool SendPacket( const void *pkt, int cbPkt ) override;
	virtual bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal ) override;
	virtual void TrackSentStats( UDPSendPacketContext_t &ctx ) override;
	virtual void RecvValidUDPDataPacket( UDPRecvPacketContext_t &ctx ) override;
};

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#endif // STEAMNETWORKINGSOCKETS_P2P_ICE_H
