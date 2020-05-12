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

class CMsgSteamSockets_UDP_ICEPingCheck;

namespace SteamNetworkingSocketsLib {

class CSteamNetworkConnectionP2P;
struct UDPSendPacketContext_t;

/// Transport for peer-to-peer connection using WebRTC
class CConnectionTransportP2PICE final
: public CConnectionTransportUDPBase
, public IThinker
, private IICESessionDelegate
{
public:
	CConnectionTransportP2PICE( CSteamNetworkConnectionP2P &connection );
	virtual ~CConnectionTransportP2PICE();

	inline CSteamNetworkConnectionP2P &Connection() const { return *assert_cast< CSteamNetworkConnectionP2P *>( &m_connection ); }
	inline ISteamNetworkingConnectionCustomSignaling *Signaling() const { return Connection().m_pSignaling; }

	void Init();

	// CConnectionTransport overrides
	virtual void TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const override;
	virtual void GetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow ) override;
	virtual void TransportFreeResources() override;
	virtual void TransportConnectionStateChanged( ESteamNetworkingConnectionState eOldState ) override;
	virtual bool BCanSendEndToEndData() const override;

	// IThinker
	virtual void Think( SteamNetworkingMicroseconds usecNow ) override;

	/// Fill in SDR-specific fields to signal
	void PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow );
	void RecvRendezvous( const CMsgICERendezvous &msg, SteamNetworkingMicroseconds usecNow );

	inline int LogLevel_P2PRendezvous() const { return m_connection.m_connectionConfig.m_LogLevel_P2PRendezvous.Get(); }

	const char *m_pszNeedToSendSignalReason;
	SteamNetworkingMicroseconds m_usecSendSignalDeadline;
	uint32 m_nLastSendRendesvousMessageID;
	uint32 m_nLastRecvRendesvousMessageID;

	void NotifyConnectionFailed( int nReasonCode, const char *pszReason );
	void QueueSelfDestruct();
	void ScheduleSendSignal( const char *pszReason );
	void QueueSignalMessage( CMsgICERendezvous_Message &&msg, const char *pszDebug );

	// In certain circumstances we may need to buffer packets
	std::mutex m_mutexPacketQueue;
	CUtlBuffer m_bufPacketQueue;

	// Some basic stats tracking about ping times.  Currently these only track the pings
	// explicitly sent at this layer.  Ideally we would hook into the SNP code, because
	// almost every data packet we send contains ping-related information.
	PingTrackerBasic m_ping;
	SteamNetworkingMicroseconds m_usecTimeLastRecv;
	SteamNetworkingMicroseconds m_usecInFlightReplyTimeout;
	int m_nReplyTimeoutsSinceLastRecv;
	int m_nTotalPingsSent;

	/// True if we need to take aggressive action to confirm
	/// end-to-end connectivity.  This will be the case when
	/// doing initial route finding, or if we aren't sure about
	/// end-to-end connectivity because we lost all of our
	/// sessions, etc.  Once we get some data packets, we set
	/// this flag to false.
	bool m_bNeedToConfirmEndToEndConnectivity;

private:
	IICESession *m_pICESession;

	struct OutboundMessage
	{
		uint32 m_nID;
		SteamNetworkingMicroseconds m_usecRTO; // Retry timeout
		CMsgICERendezvous_Message m_msg;
	};
	std::vector< OutboundMessage > m_vecUnackedOutboundMessages; // outbound messages that have not been acked

	// Implements IICESessionDelegate
	virtual void Log( IICESessionDelegate::ELogPriority ePriority, const char *pszMessageFormat, ... ) override;
	virtual void OnData( const void *pData, size_t nSize ) override;
	virtual void OnIceCandidateAdded( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate ) override;
	virtual void OnWritableStateChanged() override;

	void DrainPacketQueue( SteamNetworkingMicroseconds usecNow );
	void ProcessPacket( const uint8_t *pData, int cbPkt, SteamNetworkingMicroseconds usecNow );
	void Received_PingCheck( const CMsgSteamSockets_UDP_ICEPingCheck &msg, SteamNetworkingMicroseconds usecNow );

	// Implements CConnectionTransportUDPBase
	virtual bool SendPacket( const void *pkt, int cbPkt ) override;
	virtual bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal ) override;

	// FIXME - Need to figure out when this will be called
	void TrackSentPingRequest( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply );
};

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#endif // STEAMNETWORKINGSOCKETS_P2P_ICE_H
