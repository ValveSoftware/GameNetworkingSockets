//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_P2P_WEBRTC_H
#define STEAMNETWORKINGSOCKETS_P2P_WEBRTC_H
#pragma once

#include "steamnetworkingsockets_p2p.h"
#include <steamwebrtc.h>

class CMsgSteamSockets_UDP_Stats;
class CMsgSteamSockets_UDP_ConnectionClosed;
class CMsgSteamSockets_UDP_NoConnection;

namespace SteamNetworkingSocketsLib {

class CSteamNetworkConnectionP2P;
struct UDPSendPacketContext_t;

/// Transport for peer-to-peer connection using WebRTC
class CConnectionTransportP2PWebRTC
: public CConnectionTransport
, private IWebRTCSessionDelegate
{
public:
	CConnectionTransportP2PWebRTC( CSteamNetworkConnectionP2P &connection );
	virtual ~CConnectionTransportP2PWebRTC();

	inline CSteamNetworkConnectionP2P &Connection() const { return *assert_cast< CSteamNetworkConnectionP2P *>( &m_connection ); }
	inline ISteamNetworkingConnectionCustomSignaling *Signaling() const { return Connection().m_pSignaling; }

	// CConnectionTransport overrides
	virtual void TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const override;
	virtual void GetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow ) override;
	virtual void TransportFreeResources() override;
	virtual void ConnectionStateChanged( ESteamNetworkingConnectionState eOldState ) override;
	virtual bool BCanSendEndToEndData() const override;
	virtual bool SendDataPacket( SteamNetworkingMicroseconds usecNow ) override;
	virtual int SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctx ) override;

	/// Fill in SDR-specific fields to signal
	void PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow );

	inline int LogLevel_P2PRendezvous() const { return m_connection.m_connectionConfig.m_LogLevel_P2PRendezvous.Get(); }

	CUtlVectorAutoPurge<char *> m_vecStunServers;

private:
	IWebRTCSession *m_pWebRTCSession;
	EWebRTCSessionState m_eWebRTCSessionState;

	std::vector<CMsgSteamNetworkingP2PRendezvous_ICECandidate> m_vecICECandidates;

	// Implements IWebRTCSessionDelegate
	virtual void Log( IWebRTCSessionDelegate::ELogPriority ePriority, const char *pszMessageFormat, ... );
	virtual int GetNumStunServers() override;
	virtual const char *GetStunServer( int iIndex ) override;
	virtual void OnSessionStateChanged( EWebRTCSessionState eState ) override;
	virtual void OnOfferReady( bool bSuccess, const char *pszOffer ) override;
	virtual void OnAnswerReady( bool bSuccess, const char *pszAnswer ) override;
	virtual void OnData( const uint8_t *pData, size_t nSize ) override;

	//virtual int GetNumTurnServers() override;
	//virtual const char *GetTurnServer( int iIndex ) { return nullptr; }
	//virtual const char *GetTurnServerUsername() { return nullptr; }
	//virtual const char *GetTurnServerPassword() { return nullptr; }
	//virtual void OnIceCandidateAdded( const char *pszSDPMid, int nSDPMLineIndex, const char *pszCandidate ) { }
	//virtual void OnIceCandidatesComplete( const char *pszCandidates ) { }
	//virtual void OnSendPossible() { }

	void Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, SteamNetworkingMicroseconds usecNow );
	void Received_NoConnection( const CMsgSteamSockets_UDP_NoConnection &msg, SteamNetworkingMicroseconds usecNow );

	void SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg );

	void SendConnectionClosedOrNoConnection();
	void SendNoConnection();

	/// Process stats message, either inline or standalone
	void RecvStats( const CMsgSteamSockets_UDP_Stats &msgStatsIn, bool bInline, SteamNetworkingMicroseconds usecNow );
	void SendStatsMsg( EStatsReplyRequest eReplyRequested, SteamNetworkingMicroseconds usecNow, const char *pszReason );
	void TrackSentStats( const CMsgSteamSockets_UDP_Stats &msgStatsOut, bool bInline, SteamNetworkingMicroseconds usecNow );
};

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_P2P_WEBRTC_H
