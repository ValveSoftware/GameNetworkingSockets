//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_P2P_H
#define STEAMNETWORKINGSOCKETS_P2P_H
#pragma once

#include "steamnetworkingsockets_connections.h"
#include "csteamnetworkingsockets.h"

class CMsgSteamDatagramConnectRequest;

namespace SteamNetworkingSocketsLib {

/// Special disconnection reason code that is used in signals
/// to indicate "no connection"
const uint32 k_ESteamNetConnectionEnd_Internal_P2PNoConnection = 9999;

// If we are the "controlled" agent, add this penalty to routes
// other than the one that are not the one the controlling agent
// has selected
constexpr int k_nRoutePenaltyNotNominated = 100;
constexpr int k_nRoutePenaltyNeedToConfirmConnectivity = 10000;
constexpr int k_nRoutePenaltyNotLan = 10; // Any route that appears to be a LAN route gets a bonus.  (Actually, all others are penalized)
constexpr int k_nRoutePenaltyNotSelectedOverride = 4000;

// Values for P2PTRansportOverride config value
constexpr int k_nP2P_TransportOverride_None = 0;
constexpr int k_nP2P_TransportOverride_SDR = 1;
constexpr int k_nP2P_TransportOverride_ICE = 2;

constexpr int k_nICECloseCode_Local_NotCompiled = k_ESteamNetConnectionEnd_Local_Max;
constexpr int k_nICECloseCode_Local_UserNotEnabled = k_ESteamNetConnectionEnd_Local_Max-1;
constexpr int k_nICECloseCode_Local_FailedInit = k_ESteamNetConnectionEnd_Local_Max-2;
constexpr int k_nICECloseCode_Remote_NotEnabled = k_ESteamNetConnectionEnd_Remote_Max;

// A really terrible ping score, but one that we can do some math with without overflowing
constexpr int k_nRouteScoreHuge = INT_MAX/8;


struct SteamNetworkingMessagesSession;
class CSteamNetworkingMessages;
class CConnectionTransportP2PSDR;
class CConnectionTransportP2PICE;

//-----------------------------------------------------------------------------
/// Listen socket for peer-to-peer connections relayed through through SDR network
/// We can only do this on platforms where this is some sort of "default" signaling
/// mechanism

class CSteamNetworkListenSocketP2P : public CSteamNetworkListenSocketBase
{
public:
	CSteamNetworkListenSocketP2P( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );

	/// Setup
	bool BInit( int nVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg );

private:
	virtual ~CSteamNetworkListenSocketP2P(); // hidden destructor, don't call directly.  Use Destroy()

	/// The "virtual port" of the server for relay connections.
	int m_nVirtualPort;
};

/// Mixin base class for different P2P transports.
class CConnectionTransportP2PBase
{
public:
	// Virtual base classes.  (We don't directly derive, since we are a mixin,
	// but all classes that derive from us will derive from these base classes.)
	CConnectionTransport *const m_pSelfAsConnectionTransport;
	IThinker *const m_pSelfAsThinker;

	const char *const m_pszP2PTransportDebugName;

	/// True if we need to take aggressive action to confirm
	/// end-to-end connectivity.  This will be the case when
	/// doing initial route finding, or if we aren't sure about
	/// end-to-end connectivity because we lost all of our
	/// sessions, etc.  Once we get some data packets, we set
	/// this flag to false.
	bool m_bNeedToConfirmEndToEndConnectivity;

	// Some basic stats tracking about ping times.  Currently these only track the pings
	// explicitly sent at this layer.  Ideally we would hook into the SNP code, because
	// almost every data packet we send contains ping-related information.
	PingTrackerForRouteSelection m_pingEndToEnd;
	SteamNetworkingMicroseconds m_usecEndToEndInFlightReplyTimeout;
	int m_nReplyTimeoutsSinceLastRecv;
	int m_nTotalPingsSent;

	struct P2PRouteQualityMetrics
	{
		// Scores based only on ping times.
		int m_nScoreCurrent;
		int m_nScoreMin;
		int m_nScoreMax;

		// Sum of all penalties
		int m_nTotalPenalty;

		// Number of recent valid ping collection intervals.
		// (See PingTrackerForRouteSelection)
		int m_nBucketsValid;

		inline void SetInvalid()
		{
			m_nScoreCurrent = k_nRouteScoreHuge;
			m_nScoreMin = k_nRouteScoreHuge;
			m_nScoreMax = k_nRouteScoreHuge;
			m_nTotalPenalty = 0;
			m_nBucketsValid = 0;
		}

	};
	P2PRouteQualityMetrics m_routeMetrics;

	void P2PTransportTrackRecvEndToEndPacket( SteamNetworkingMicroseconds usecNow )
	{
		m_usecEndToEndInFlightReplyTimeout = 0;
		m_nReplyTimeoutsSinceLastRecv = 0;
	}
	void P2PTransportTrackSentEndToEndPingRequest( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply );
	void P2PTransportThink( SteamNetworkingMicroseconds usecNow );
	void P2PTransportEndToEndConnectivityConfirmed( SteamNetworkingMicroseconds usecNow );
	void P2PTransportEndToEndConnectivityNotConfirmed( SteamNetworkingMicroseconds usecNow );

	// Populate m_routeMetrics.  If we're not really available, then the metrics should be set to a huge score
	virtual void P2PTransportUpdateRouteMetrics( SteamNetworkingMicroseconds usecNow ) = 0;

protected:
	CConnectionTransportP2PBase( const char *pszDebugName, CConnectionTransport *pSelfBase, IThinker *pSelfThinker );

	// Shortcut to get connection and upcast
	CSteamNetworkConnectionP2P &Connection() const;
};

/// A peer-to-peer connection that can use different types of underlying transport
class CSteamNetworkConnectionP2P final : public CSteamNetworkConnectionBase
{
public:
	CSteamNetworkConnectionP2P( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );

	/// Start connecting to a remote peer at the specified virtual port
	bool BInitConnect( ISteamNetworkingConnectionCustomSignaling *pSignaling, const SteamNetworkingIdentity *pIdentityRemote, int nVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg );

	/// Begin accepting a P2P connection
	bool BBeginAccept(
		const CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest,
		SteamDatagramErrMsg &errMsg,
		SteamNetworkingMicroseconds usecNow
	);

	// CSteamNetworkConnectionBase overrides
	virtual void FreeResources() override;
	virtual EResult AcceptConnection( SteamNetworkingMicroseconds usecNow ) override;
	virtual void GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const override;
	virtual void ThinkConnection( SteamNetworkingMicroseconds usecNow ) override;
	virtual SteamNetworkingMicroseconds ThinkConnection_ClientConnecting( SteamNetworkingMicroseconds usecNow ) override;
	virtual void DestroyTransport() override;
	virtual CSteamNetworkConnectionP2P *AsSteamNetworkConnectionP2P() override;
	virtual void ConnectionStateChanged( ESteamNetworkingConnectionState eOldState ) override;
	virtual void ProcessSNPPing( int msPing, RecvPacketContext_t &ctx ) override;

	void SendConnectOKSignal( SteamNetworkingMicroseconds usecNow );
	void SendConnectionClosedSignal( SteamNetworkingMicroseconds usecNow );
	void SendNoConnectionSignal( SteamNetworkingMicroseconds usecNow );

	void ScheduleSendSignal( const char *pszReason );
	void QueueSignalReliableMessage( CMsgSteamNetworkingP2PRendezvous_ReliableMessage &&msg, const char *pszDebug );

	/// Given a partially-completed CMsgSteamNetworkingP2PRendezvous, finish filling out
	/// the required fields, and send it to the peer via the signaling mechanism
	void SetRendezvousCommonFieldsAndSendSignal( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow, const char *pszDebugReason );

	bool ProcessSignal( const CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow );
	void ProcessSignal_ConnectOK( const CMsgSteamNetworkingP2PRendezvous_ConnectOK &msgConnectOK, SteamNetworkingMicroseconds usecNow );

	// Return true if we are the "controlling" peer, in the ICE sense of the term.
	// That is, the agent who will primarily make the route decisions, with the
	// controlled agent accepting whatever routing decisions are made, when possible.
	inline bool IsControllingAgent() const
	{
		// For now, the "server" will always be the controlling agent.
		// This is the opposite of the ICE convention, but we had some
		// reasons for the initial use case to do it this way.  We can
		// plumb through role negotiation if we need to change this.
		return m_bConnectionInitiatedRemotely;
	}

	/// What virtual port are we requesting?
	int m_nRemoteVirtualPort;

	/// Handle to our entry in g_mapIncomingP2PConnections, or -1 if we're not in the map
	int m_idxMapIncomingP2PConnections;

	/// How to send signals to the remote host for this
	ISteamNetworkingConnectionCustomSignaling *m_pSignaling;

	//
	// Different transports
	//
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		CConnectionTransportP2PSDR *m_pTransportP2PSDR;
	#endif
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

		// ICE transport that we are using, if any
		CConnectionTransportP2PICE *m_pTransportICE;

		// If ICE transport needs to self-destruct, we move it here, and clear
		// m_pTransportICE.  Then it will be deleted at a safe time.
		CConnectionTransportP2PICE *m_pTransportICEPendingDelete;

		// Failure reason for ICE, if any.  (0 if no failure yet.)
		int m_nICECloseCode;
		char m_szICECloseMsg[ k_cchSteamNetworkingMaxConnectionCloseReason ];

		// When we receive a connection from peer, we need to wait for the app
		// to accept it.  During that time we may need to pend any ICE messages
		std::vector<CMsgICERendezvous> m_vecPendingICEMessages;

		CMsgSteamNetworkingSocketsICESessionSummary m_msgICESessionSummary;

		void ICEFailed( int nReasonCode, const char *pszReason );
		void QueueDestroyICE();
	#else
		static constexpr int m_nICECloseCode = k_nICECloseCode_Local_NotCompiled;
	#endif

	/// Sometimes it's nice to have all existing options in a list
	vstd::small_vector< CConnectionTransportP2PBase *, 3 > m_vecAvailableTransports;

	/// Currently selected transport.
	/// Always the same as m_pTransport, but as CConnectionTransportP2PBase
	CConnectionTransportP2PBase *m_pCurrentTransportP2P;

	/// Which transport does it look like our peer is using?
	CConnectionTransportP2PBase *m_pPeerSelectedTransport;
	void SetPeerSelectedTransport( CConnectionTransportP2PBase *pPeerSelectedTransport )
	{
		if ( m_pPeerSelectedTransport != pPeerSelectedTransport )
		{
			m_pPeerSelectedTransport = pPeerSelectedTransport;
			PeerSelectedTransportChanged();
		}
	}

	// Check if user permissions for the remote host are allowed, then
	// create ICE.  Also, if the connection was initiated remotely,
	// we will create an offer
	void CheckInitICE();

	// Check if we pended ICE deletion, then do so now
	void CheckCleanupICE();

	//
	// Transport evaluation and selection
	//

	SteamNetworkingMicroseconds m_usecNextEvaluateTransport;

	/// True if we should be "sticky" to the current transport.
	/// When major state changes happen, we clear this flag
	/// and evaluate from scratch with no stickiness
	bool m_bTransportSticky;

	void ThinkSelectTransport( SteamNetworkingMicroseconds usecNow );
	void TransportEndToEndConnectivityChanged( CConnectionTransportP2PBase *pTransportP2P, SteamNetworkingMicroseconds usecNow );
	void SelectTransport( CConnectionTransportP2PBase *pTransport, SteamNetworkingMicroseconds usecNow );

	// FIXME - UDP transport for LAN discovery, so P2P works without any signaling

	inline int LogLevel_P2PRendezvous() const { return m_connectionConfig.m_LogLevel_P2PRendezvous.Get(); }

private:
	virtual ~CSteamNetworkConnectionP2P(); // hidden destructor, don't call directly.  Use ConnectionDestroySelfNow

	/// Shared init
	bool BInitP2PConnectionCommon( SteamNetworkingMicroseconds usecNow, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg );

	struct OutboundMessage
	{
		uint32 m_nID;
		int m_cbSerialized;
		SteamNetworkingMicroseconds m_usecRTO; // Retry timeout
		CMsgSteamNetworkingP2PRendezvous_ReliableMessage m_msg;
	};
	std::vector< OutboundMessage > m_vecUnackedOutboundMessages; // outbound messages that have not been acked

	const char *m_pszNeedToSendSignalReason;
	SteamNetworkingMicroseconds m_usecSendSignalDeadline;
	uint32 m_nLastSendRendesvousMessageID;
	uint32 m_nLastRecvRendesvousMessageID;

	// Really destroy ICE now
	void DestroyICENow();

	void PeerSelectedTransportChanged();
};

inline CSteamNetworkConnectionP2P &CConnectionTransportP2PBase::Connection() const
{
	return *assert_cast<CSteamNetworkConnectionP2P *>( &m_pSelfAsConnectionTransport->m_connection );
}

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_P2P_H
