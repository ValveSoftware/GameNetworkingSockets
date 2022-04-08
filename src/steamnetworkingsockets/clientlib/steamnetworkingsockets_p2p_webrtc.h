//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_P2P_WEBRTC_H
#define STEAMNETWORKINGSOCKETS_P2P_WEBRTC_H
#pragma once

#include "steamnetworkingsockets_p2p_ice.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC

#include "ice_client_types.h"
#include "../../external/steamwebrtc/ice_session.h"

class IICESession;
class IICESessionDelegate;
struct ICESessionConfig;
typedef IICESession *( *CreateICESession_t )( const ICESessionConfig &cfg, IICESessionDelegate *pDelegate, int nInterfaceVersion );
extern "C" CreateICESession_t g_SteamNetworkingSockets_CreateICESessionFunc;

namespace SteamNetworkingSocketsLib {

class CConnectionTransportP2PICE_WebRTC final
: public CConnectionTransportP2PICE
, private IICESessionDelegate
{
public:
	CConnectionTransportP2PICE_WebRTC( CSteamNetworkConnectionP2P &connection );
	virtual ~CConnectionTransportP2PICE_WebRTC();

	// In certain circumstances we may need to buffer packets
	ShortDurationLock m_mutexPacketQueue;
	CUtlBuffer m_bufPacketQueue;

	void Init( const ICESessionConfig &cfg );

private:
	IICESession *m_pICESession;

	void RouteOrWritableStateChanged();
	void UpdateRoute();

	void DrainPacketQueue( SteamNetworkingMicroseconds usecNow );

	// CConnectionTransport overrides
	virtual bool BCanSendEndToEndData() const override;
	virtual void TransportFreeResources() override;

	// CConnectionTransportP2PBase
	virtual void P2PTransportThink( SteamNetworkingMicroseconds usecNow ) override;

	// CConnectionTransportP2PICE overrides
	virtual void RecvRendezvous( const CMsgICERendezvous &msg, SteamNetworkingMicroseconds usecNow ) override;

	// CConnectionTransportUDPBase overrides
	virtual bool SendPacket( const void *pkt, int cbPkt ) override;
	virtual bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal ) override;

	// Implements IICESessionDelegate
	virtual void Log( IICESessionDelegate::ELogPriority ePriority, const char *pszMessageFormat, ... ) override;
	virtual void OnData( const void *pData, size_t nSize ) override;
	virtual void OnLocalCandidateGathered( EICECandidateType eType, const char *pszCandidate ) override;
	virtual void OnWritableStateChanged() override;
	virtual void OnRouteChanged() override;
};

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC

#endif // _H
