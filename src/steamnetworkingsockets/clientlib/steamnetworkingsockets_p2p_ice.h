//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_P2P_ICE_H
#define STEAMNETWORKINGSOCKETS_P2P_ICE_H
#pragma once

#include "steamnetworkingsockets_p2p.h"
#include "steamnetworkingsockets_udp.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#include "ice_client_types.h"

namespace SteamNetworkingSocketsLib {

constexpr int k_nMinPingTimeLocalTolerance = 5;

class CSteamNetworkConnectionP2P;
struct UDPSendPacketContext_t;

/// Base transport for peer-to-peer connection using ICE
class CConnectionTransportP2PICE
: public CConnectionTransportUDPBase
, public CConnectionTransportP2PBase
, public CTaskTarget // Should we promote this to a base class?
{
public:
	virtual ~CConnectionTransportP2PICE();

	inline CSteamNetworkConnectionP2P &Connection() const { return *assert_cast< CSteamNetworkConnectionP2P *>( &m_connection ); }
	inline ISteamNetworkingConnectionSignaling *Signaling() const { return Connection().m_pSignaling; }

	// CConnectionTransport overrides
	virtual void TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const override;
	virtual void GetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow ) override;

	// CConnectionTransportP2PBase
	virtual void P2PTransportUpdateRouteMetrics( SteamNetworkingMicroseconds usecNow ) override;

	/// Fill in SDR-specific fields to signal
	void PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow );
	virtual void RecvRendezvous( const CMsgICERendezvous &msg, SteamNetworkingMicroseconds usecNow ) = 0;

	inline int LogLevel_P2PRendezvous() const { return m_connection.m_connectionConfig.LogLevel_P2PRendezvous.Get(); }

	void LocalCandidateGathered( EICECandidateType eType, CMsgICECandidate &&msgCandidate );

	//EICECandidateType m_eCurrentRouteLocalCandidateType;
	//EICECandidateType m_eCurrentRouteRemoteCandidateType;
	SteamNetworkingIPAddr m_currentRouteRemoteAddress;
	ESteamNetTransportKind m_eCurrentRouteKind;

protected:
	CConnectionTransportP2PICE( CSteamNetworkConnectionP2P &connection );

	void ProcessPacket( const uint8_t *pData, int cbPkt, SteamNetworkingMicroseconds usecNow );

	// Implements CConnectionTransportUDPBase
	virtual bool SendPacket( const void *pkt, int cbPkt ) override = 0;
	virtual bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal ) override = 0;
	virtual void TrackSentStats( UDPSendPacketContext_t &ctx ) override;
	virtual void RecvValidUDPDataPacket( UDPRecvPacketContext_t &ctx ) override;
};

extern std::string Base64EncodeLower30Bits( uint32 nNum );

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#endif // STEAMNETWORKINGSOCKETS_P2P_ICE_H
