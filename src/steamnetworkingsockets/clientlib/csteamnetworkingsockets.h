//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef CSTEAMNETWORKINGSOCKETS_H
#define CSTEAMNETWORKINGSOCKETS_H
#pragma once

#include <steam/steamnetworkingsockets.h>

#ifdef STEAMNETWORKINGSOCKETS_STEAM
	#include <common/steam/iclientnetworkingsockets.h>
#else
	typedef ISteamNetworkingSockets IClientNetworkingSockets;
#endif

#include "steamnetworkingsockets_connections.h"

namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// Steam API interfaces
//
/////////////////////////////////////////////////////////////////////////////

class CSteamNetworkingSockets : public IClientNetworkingSockets
{
public:
	CSteamNetworkingSockets();
	virtual ~CSteamNetworkingSockets();

	bool m_bHaveLowLevelRef;
	AppId_t m_nAppID;

	CMsgSteamDatagramCertificateSigned m_msgSignedCert;
	CMsgSteamDatagramCertificate m_msgCert;
	CECSigningPrivateKey m_keyPrivateKey;
	bool BCertHasIdentity() const;
	bool SetCertificate( const void *pCert, int cbCert, void *pPrivateKey, int cbPrivateKey, SteamDatagramErrMsg &errMsg );

	bool BHasAnyConnections() const;
	bool BHasAnyListenSockets() const;
	bool BInitted() const { return m_bHaveLowLevelRef; }

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
	bool BInitGameNetworkingSockets( const SteamNetworkingIdentity *pIdentity, SteamDatagramErrMsg &errMsg );
	void Kill() { KillBase(); }
	void CacheIdentity() { m_identity.SetLocalHost(); }
#else
	virtual void AsyncCertRequest() = 0;
	virtual void CacheIdentity() = 0;
#endif

	const SteamNetworkingIdentity &InternalGetIdentity()
	{
		if ( m_identity.IsInvalid() )
			CacheIdentity();
		return m_identity;
	}

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
	virtual int32 GetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue ) OVERRIDE;
	virtual bool SetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue, int32 nValue ) OVERRIDE;
	virtual const char *GetConfigurationValueName( ESteamNetworkingConfigurationValue eConfigValue ) OVERRIDE;
	virtual int32 GetConfigurationString( ESteamNetworkingConfigurationString eConfigString, char *pDest, int32 destSize ) OVERRIDE;
	virtual bool SetConfigurationString( ESteamNetworkingConfigurationString eConfigString, const char *pString ) OVERRIDE;
	virtual const char *GetConfigurationStringName( ESteamNetworkingConfigurationString eConfigString ) OVERRIDE;
	virtual int32 GetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue ) OVERRIDE;
	virtual bool SetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue, int32 nValue ) OVERRIDE;
	virtual bool GetIdentity( SteamNetworkingIdentity *pIdentity ) OVERRIDE;

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
	virtual void RunCallbacks( ISteamNetworkingSocketsCallbacks *pCallbacks ) OVERRIDE;
#endif

protected:

	void KillBase();
	void KillConnections();

	static int s_nSteamNetworkingSocketsInitted;

	SteamNetworkingIdentity m_identity;

	virtual void InternalQueueCallback( int nCallback, int cbCallback, const void *pvCallback );
#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
	struct QueuedCallback
	{
		int nCallback;
		char data[ sizeof(SteamNetConnectionStatusChangedCallback_t) ]; // whatever the biggest callback struct we have is
	};
	std::vector<QueuedCallback> m_vecPendingCallbacks;
#endif
};

} // namespace SteamNetworkingSocketsLib

#endif // CSTEAMNETWORKINGSOCKETS_H
