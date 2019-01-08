//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef CSTEAMNETWORKINGSOCKETS_H
#define CSTEAMNETWORKINGSOCKETS_H
#pragma once

#include <steamnetworkingsockets/isteamnetworkingsockets.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h> // FIXME - could use a more narrow header
#include <steam/isteamnetworkingsocketsserialized.h>
#endif
#include "steamnetworkingsockets_connections.h"

class CMsgSteamDatagramP2PRendezvous;
struct SteamDatagramHostedAddress;

namespace SteamNetworkingSocketsLib {

class CSteamNetworkConnectionP2PSDR;
class CSteamNetworkListenSocketP2P;

#pragma pack(push,1)
struct P2PMessageHeader
{
	uint8 m_nFlags;
	int m_nChannel;
};
#pragma pack(pop)
COMPILE_TIME_ASSERT( sizeof(P2PMessageHeader) == 5 );

/////////////////////////////////////////////////////////////////////////////
//
// Hook Steam callbacks in a hacked way to global callbacks
//
/////////////////////////////////////////////////////////////////////////////

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
// Ug, we have to hack in her because we aren't linking with steam_api.lib
class CSteamNetworkingSocketsCallbackBase : public CCallbackBase
{
public:
	~CSteamNetworkingSocketsCallbackBase() { Unregister(); }
	virtual void Run( void *pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall ) { Assert( false ); }
	void Register( bool bGameServer );
	void Unregister();
};

template<typename ArgType>
class CSteamNetworkingSocketsCallbackBaseT : public CSteamNetworkingSocketsCallbackBase
{
public:
	virtual int GetCallbackSizeBytes() { return sizeof(ArgType); }
protected:
	CSteamNetworkingSocketsCallbackBaseT() : CSteamNetworkingSocketsCallbackBase() { m_iCallback = ArgType::k_iCallback; }
};

template<typename ArgType>
class CSteamNetworkingSocketsCallback : public CSteamNetworkingSocketsCallbackBaseT<ArgType>
{
public:
	virtual void OnCallback( ArgType *pvParam ) = 0;

protected:
	virtual void Run( void *pvParam ) OVERRIDE { OnCallback( (ArgType*)pvParam ); }
};

class CSteamNetworkingSocketsCallResultBase : public CCallbackBase
{
public:
	~CSteamNetworkingSocketsCallResultBase() { Cancel(); }
	virtual void Run( void *pvParam ) OVERRIDE { Assert( false ); }
	void Set( SteamAPICall_t hSteamAPICall );
	void Cancel();
protected:
	CSteamNetworkingSocketsCallResultBase() { m_hAPICall = k_uAPICallInvalid; }
	SteamAPICall_t m_hAPICall;
};

template<typename T>
class CSteamNetworkingSocketsCallResult : public CSteamNetworkingSocketsCallResultBase
{
public:
	virtual int GetCallbackSizeBytes() OVERRIDE { return sizeof(T); }

	virtual void OnCallback( T *param, bool bIOFailure ) = 0;
protected:
	CSteamNetworkingSocketsCallResult() : CSteamNetworkingSocketsCallResultBase() { m_iCallback = T::k_iCallback; }
	virtual void Run( void *pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall ) OVERRIDE
	{
		if ( hSteamAPICall == m_hAPICall )
		{
			m_hAPICall = k_uAPICallInvalid; // caller unregisters for us
			OnCallback( (T*)pvParam, bIOFailure );
		}
	}
};
#endif

/////////////////////////////////////////////////////////////////////////////
//
// Steam API interfaces
//
/////////////////////////////////////////////////////////////////////////////

class CSteamNetworkingSockets : public ISteamNetworkingSockets
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
	, private CSteamNetworkingSocketsCallback<SteamServersConnected_t>
	, private CSteamNetworkingSocketsCallback<SteamServerConnectFailure_t>
	, private CSteamNetworkingSocketsCallback<SteamServersDisconnected_t>
	, private CSteamNetworkingSocketsCallResult<SteamNetworkingSocketsCert_t>
	, private CSteamNetworkingSocketsCallback<SteamNetworkingSocketsRecvP2PRendezvous_t>
	, private CSteamNetworkingSocketsCallback<SteamNetworkingSocketsRecvP2PFailure_t>
	, private CSteamNetworkingSocketsCallback<SteamNetworkingSocketsConfigUpdated_t>
#endif
{
public:
	CSteamNetworkingSockets( bool bGameServer );

	const bool m_bGameServer;
	AppId_t m_nAppID;

	const SteamNetworkingIdentity &InternalGetIdentity();

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
	ISteamUtils *m_pSteamUtils;
	ISteamNetworkingSocketsSerialized *m_pSteamNetworkingSocketsSerialized;

	enum ELogonStatus
	{
		k_ELogonStatus_InitialConnecting, // we're not logged on yet, but we're making our initial connection
		k_ELogonStatus_Connected,
		k_ELogonStatus_Disconnected,
	};
	inline ELogonStatus GetLogonStatus() { return m_eLogonStatus; }
	CUtlHashMap<int,CSteamNetworkListenSocketP2P *,std::equal_to<int>,std::hash<int>> m_mapListenSocketsByVirtualPort;
#endif

	CMsgSteamDatagramCertificateSigned m_msgSignedCert;
	CMsgSteamDatagramCertificate m_msgCert;
	CECSigningPrivateKey m_keyPrivateKey;
	bool BCertHasIdentity() const;

	bool BHasAnyConnections() const;
	bool BHasAnyListenSockets() const;
	bool BInitted() const { return m_bInitted; }

	void Kill();

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
	bool BInitNonSteam( const SteamNetworkingIdentity *pIdentity, SteamDatagramErrMsg &errMsg );
#else
	bool BInit( ISteamClient *pClient, HSteamUser hSteamUser, HSteamPipe hSteamPipe, SteamDatagramErrMsg &errMsg );
	void AsyncCertRequest();
	bool SetCertificate( const void *pCert, int cbCert, void *pPrivateKey, int cbPrivateKey, SteamDatagramErrMsg &errMsg );

	bool BSDRClientInit( SteamDatagramErrMsg &errMsg );
	void SDRClientKill();
	const CachedRelayAuthTicket *FindRelayAuthTicketForServerPtr( const SteamNetworkingIdentity &identityGameServer, int nVirtualPort );

	void SendP2PConnectionFailure( CSteamID steamIDRemote, uint32 nConnectionIDDest, uint32 nReason, const char *pszReason );
	void SendP2PNoConnection( CSteamID steamIDRemote, uint32 nConnectionIDDest );
	void SendP2PRendezvous( CSteamID steamIDRemote, uint32 nConnectionIDSrc, const CMsgSteamDatagramP2PRendezvous &msg, const char *pszDebugReason );
#endif

	template <typename T>
	void QueueCallback( const T& x )
	{
		InternalQueueCallback( T::k_iCallback, sizeof(T), &x );
	}

	// Implements ISteamNetworkingSockets
	virtual HSteamListenSocket CreateListenSocketIP( const SteamNetworkingIPAddr &localAddress ) OVERRIDE;
	virtual HSteamNetConnection ConnectByIPAddress( const SteamNetworkingIPAddr &adress ) OVERRIDE;
	virtual EResult AcceptConnection( HSteamNetConnection hConn ) OVERRIDE;
	virtual bool CloseConnection( HSteamNetConnection hConn, int nReason, const char *pszDebug, bool bEnableLinger ) OVERRIDE;
	virtual bool CloseListenSocket( HSteamListenSocket hSocket ) OVERRIDE;
	virtual bool SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData ) OVERRIDE;
	virtual int64 GetConnectionUserData( HSteamNetConnection hPeer ) OVERRIDE;
	virtual void SetConnectionName( HSteamNetConnection hPeer, const char *pszName ) OVERRIDE;
	virtual bool GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen ) OVERRIDE;
	virtual EResult SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType ) OVERRIDE;
	virtual EResult FlushMessagesOnConnection( HSteamNetConnection hConn ) OVERRIDE;
	virtual int ReceiveMessagesOnConnection( HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) OVERRIDE;
	virtual int ReceiveMessagesOnListenSocket( HSteamListenSocket hSocket, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) OVERRIDE;
	virtual bool GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo ) OVERRIDE;
	virtual bool GetQuickConnectionStatus( HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats ) OVERRIDE;
	virtual int GetDetailedConnectionStatus( HSteamNetConnection hConn, char *pszBuf, int cbBuf ) OVERRIDE;
	virtual bool GetListenSocketAddress( HSteamListenSocket hSocket, SteamNetworkingIPAddr *pAddress ) OVERRIDE;
	virtual bool CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 ) OVERRIDE;
	virtual bool GetConnectionDebugText( HSteamNetConnection hConn, char *pszOut, int nOutCCH ) OVERRIDE;
	virtual int32 GetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue ) OVERRIDE;
	virtual bool SetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue, int32 nValue ) OVERRIDE;
	virtual const char *GetConfigurationValueName( ESteamNetworkingConfigurationValue eConfigValue ) OVERRIDE;
	virtual int32 GetConfigurationString( ESteamNetworkingConfigurationString eConfigString, char *pDest, int32 destSize ) OVERRIDE;
	virtual bool SetConfigurationString( ESteamNetworkingConfigurationString eConfigString, const char *pString ) OVERRIDE;
	virtual const char *GetConfigurationStringName( ESteamNetworkingConfigurationString eConfigString ) OVERRIDE;
	virtual int32 GetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue ) OVERRIDE;
	virtual bool SetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue, int32 nValue ) OVERRIDE;
	virtual void RunCallbacks( ISteamNetworkingSocketsCallbacks *pCallbacks ) OVERRIDE;
	virtual bool GetIdentity( SteamNetworkingIdentity *pIdentity ) OVERRIDE;

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
	virtual HSteamListenSocket CreateListenSocketP2P( int nVirtualPort ) OVERRIDE;
	virtual HSteamNetConnection ConnectP2P( const SteamNetworkingIdentity &identityRemote, int nVirtualPort ) OVERRIDE;
	virtual bool ReceivedRelayAuthTicket( const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket ) OVERRIDE;
	virtual int FindRelayAuthTicketForServer( const SteamNetworkingIdentity &identityGameServer, int nVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket ) OVERRIDE;
	virtual HSteamNetConnection ConnectToHostedDedicatedServer( const SteamNetworkingIdentity &identityTarget, int nVirtualPort ) OVERRIDE;
	virtual uint16 GetHostedDedicatedServerPort() OVERRIDE;
	virtual SteamNetworkingPOPID GetHostedDedicatedServerPOPID() OVERRIDE;
	virtual bool GetHostedDedicatedServerAddress( SteamDatagramHostedAddress *pRouting ) OVERRIDE;
	virtual HSteamListenSocket CreateHostedDedicatedServerListenSocket( int nVirtualPort ) OVERRIDE;
#endif

protected:

	bool m_bInitted;

	void InternalQueueCallback( int nCallback, int cbCallback, const void *pvCallback );

	SteamNetworkingIdentity m_identity;

	struct QueuedCallback
	{
		int nCallback;
		char data[ sizeof(SteamNetConnectionStatusChangedCallback_t) ]; // whatever the biggest callback struct we have is
	};
	std::vector<QueuedCallback> m_vecPendingCallbacks;

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

	virtual void OnCallback( SteamServersConnected_t *param ) OVERRIDE;
	virtual void OnCallback( SteamServerConnectFailure_t *param ) OVERRIDE;
	virtual void OnCallback( SteamServersDisconnected_t *param ) OVERRIDE;
	virtual void OnCallback( SteamNetworkingSocketsCert_t *param, bool bIOFailure ) OVERRIDE;
	virtual void OnCallback( SteamNetworkingSocketsRecvP2PRendezvous_t *param ) OVERRIDE;
	virtual void OnCallback( SteamNetworkingSocketsRecvP2PFailure_t *param ) OVERRIDE;
	virtual void OnCallback( SteamNetworkingSocketsConfigUpdated_t *param ) OVERRIDE;
	void AsyncCertRequestFinished();
	void CertRequestFailed( ESteamNetConnectionEnd nConnectionEndReason, const char *pszMsg );
	void FetchNetworkConfig();
	void FetchNetworkConfigUsingSteamInterface();

	// Recent steam datagram tickets we have received.  We also keep a copy
	// on the other side of the pipe, in the steam process.  These are here
	// for fast access.
	CUtlVectorFixed<CachedRelayAuthTicket,10> m_vecRelayAuthAuthTickets;
	void LoadRelayAuthTicketCache();
	const SteamDatagramRelayAuthTicket *AddRelayAuthTicketToCache( const void *pvTicket, int cbTicket, bool bAddToSteamProcessCache );

	ELogonStatus m_eLogonStatus;
	bool m_bSDRClientInitted;
#endif // STEAMNETWORKINGSOCKETS_OPENSOURCE
};
extern CSteamNetworkingSockets g_SteamNetworkingSocketsUser;

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
extern CSteamNetworkingSockets g_SteamNetworkingSocketsGameServer;
#endif

} // namespace SteamNetworkingSocketsLib

#endif // CSTEAMNETWORKINGSOCKETS_H
