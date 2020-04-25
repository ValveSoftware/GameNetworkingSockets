//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_P2P_WEBRTC_H
#define STEAMNETWORKINGSOCKETS_P2P_WEBRTC_H
#pragma once

#include "steamnetworkingsockets_p2p.h"
#include <mutex>

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#include "../../external/steamwebrtc/ice_session.h"

extern "C" CreateICESession_t g_SteamNetworkingSockets_CreateICESessionFunc;

class CMsgSteamSockets_UDP_Stats;
class CMsgSteamSockets_WebRTC_ConnectionClosed;
class CMsgSteamSockets_WebRTC_PingCheck;

namespace SteamNetworkingSocketsLib {

class CSteamNetworkConnectionP2P;
struct UDPSendPacketContext_t;

/// Transport for peer-to-peer connection using WebRTC
class CConnectionTransportP2PICE
: public CConnectionTransport
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
	virtual bool SendDataPacket( SteamNetworkingMicroseconds usecNow ) override;
	virtual void SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason ) override;
	virtual int SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctx ) override;

	// IThinker
	virtual void Think( SteamNetworkingMicroseconds usecNow ) override;

	/// Fill in SDR-specific fields to signal
	void PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow );
	void RecvRendezvous( const CMsgWebRTCRendezvous &msg, SteamNetworkingMicroseconds usecNow );

	inline int LogLevel_P2PRendezvous() const { return m_connection.m_connectionConfig.m_LogLevel_P2PRendezvous.Get(); }

	std::vector<std::string> m_vecStunServers;

	const char *m_pszNeedToSendSignalReason;
	SteamNetworkingMicroseconds m_usecSendSignalDeadline;
	bool m_bNeedToAckRemoteCandidates;
	uint32 m_nLocalCandidatesRevision;
	uint32 m_nRemoteCandidatesRevision;

	void NotifyConnectionFailed( int nReasonCode, const char *pszReason );
	void QueueSelfDestruct();
	void ScheduleSendSignal( const char *pszReason );

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

	struct LocalCandidate
	{
		uint32 m_nRevision;
		CMsgWebRTCRendezvous_Candidate candidate;
		SteamNetworkingMicroseconds m_usecRTO; // Retry timeout
	};
	std::vector< LocalCandidate > m_vecLocalUnAckedCandidates;

	// Implements IICESessionDelegate
	virtual void Log( IICESessionDelegate::ELogPriority ePriority, const char *pszMessageFormat, ... );
	virtual EICERole GetRole() override;
	virtual int GetNumStunServers() override;
	virtual const char *GetStunServer( int iIndex ) override;
	virtual void OnData( const void *pData, size_t nSize ) override;
	virtual void OnIceCandidateAdded( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate ) override;
	virtual void OnWritableStateChanged() override;

	// FIXME
	//virtual int GetNumTurnServers() override;
	//virtual const char *GetTurnServer( int iIndex ) { return nullptr; }
	//virtual const char *GetTurnServerUsername() { return nullptr; }
	//virtual const char *GetTurnServerPassword() { return nullptr; }

	void DrainPacketQueue( SteamNetworkingMicroseconds usecNow );
	void ProcessPacket( const uint8_t *pData, int cbPkt, SteamNetworkingMicroseconds usecNow );
	void Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectionClosed( const CMsgSteamSockets_WebRTC_ConnectionClosed &msg, SteamNetworkingMicroseconds usecNow );
	void Received_PingCheck( const CMsgSteamSockets_WebRTC_PingCheck &msg, SteamNetworkingMicroseconds usecNow );

	void SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg );

	void SendConnectionClosed();

	/// Process stats message, either inline or standalone
	void RecvStats( const CMsgSteamSockets_UDP_Stats &msgStatsIn, bool bInline, SteamNetworkingMicroseconds usecNow );
	void SendStatsMsg( EStatsReplyRequest eReplyRequested, SteamNetworkingMicroseconds usecNow, const char *pszReason );
	void TrackSentStats( const CMsgSteamSockets_UDP_Stats &msgStatsOut, bool bInline, SteamNetworkingMicroseconds usecNow );
	void TrackSentPingRequest( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply );
};

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#endif // STEAMNETWORKINGSOCKETS_P2P_WEBRTC_H
