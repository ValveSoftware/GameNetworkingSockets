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

struct SteamNetworkingMessagesSession;
class CSteamNetworkingMessages;
class CConnectionTransportP2PSDR;

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
	virtual void ThinkConnection( SteamNetworkingMicroseconds usecNow );
	virtual void DestroyTransport() override;
	virtual bool BConnectionCanSendEndToEndConnectRequest() const override;
	virtual void ConnectionSendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow ) override;
	virtual CSteamNetworkConnectionP2P *AsSteamNetworkConnectionP2P() override;

	void SendConnectOKSignal( SteamNetworkingMicroseconds usecNow );
	void SendConnectionClosedSignal( SteamNetworkingMicroseconds usecNow );
	void SendNoConnectionSignal( SteamNetworkingMicroseconds usecNow );

	/// Given a partially-completed CMsgSteamNetworkingP2PRendezvous, finish filling out
	/// the required fields, and send it to the peer via the signaling mechanism
	void SetRendezvousCommonFieldsAndSendSignal( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow, const char *pszDebugReason );

	void ProcessSignal_ConnectOK( const CMsgSteamNetworkingP2PRendezvous_ConnectOK &msgConnectOK, SteamNetworkingMicroseconds usecNow );

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
	// FIXME - WebRTC transport for STUN / TURN
	// FIXME - UDP transport for LAN discovery, so P2P works without any signaling

	inline int LogLevel_P2PRendezvous() const { return m_connectionConfig.m_LogLevel_P2PRendezvous.Get(); }

private:
	virtual ~CSteamNetworkConnectionP2P(); // hidden destructor, don't call directly.  Use ConnectionDestroySelfNow

	/// Shared init
	bool BInitP2PConnectionCommon( SteamNetworkingMicroseconds usecNow, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg );
};

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_P2P_H
