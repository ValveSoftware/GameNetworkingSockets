//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef CSTEAMNETWORKINGSOCKETS_H
#define CSTEAMNETWORKINGSOCKETS_H
#pragma once

#include <time.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifdef STEAMNETWORKINGSOCKETS_STEAMCLIENT
	#include <common/steam/iclientnetworkingsockets.h>
	#include <common/steam/iclientnetworkingutils.h>
#else
	typedef ISteamNetworkingSockets IClientNetworkingSockets;
	typedef ISteamNetworkingUtils IClientNetworkingUtils;
#endif

#include "steamnetworkingsockets_connections.h"

namespace SteamNetworkingSocketsLib {

class CSteamNetworkingUtils;

/////////////////////////////////////////////////////////////////////////////
//
// Steam API interfaces
//
/////////////////////////////////////////////////////////////////////////////

class CSteamNetworkingSockets : public IClientNetworkingSockets
{
public:
	CSteamNetworkingSockets( CSteamNetworkingUtils *pSteamNetworkingUtils );

	CSteamNetworkingUtils *const m_pSteamNetworkingUtils;
	CMsgSteamDatagramCertificateSigned m_msgSignedCert;
	CMsgSteamDatagramCertificate m_msgCert;
	CECSigningPrivateKey m_keyPrivateKey;
	bool BCertHasIdentity() const;
	virtual bool SetCertificateAndPrivateKey( const void *pCert, int cbCert, void *pPrivateKey, int cbPrivateKey, SteamDatagramErrMsg &errMsg );

	bool BHasAnyConnections() const;
	bool BHasAnyListenSockets() const;
	bool BInitted() const { return m_bHaveLowLevelRef; }

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
	bool BInitGameNetworkingSockets( const SteamNetworkingIdentity *pIdentity, SteamDatagramErrMsg &errMsg );
	void CacheIdentity() { m_identity.SetLocalHost(); }
	virtual ESteamNetworkingAvailability InitAuthentication() override;
	virtual ESteamNetworkingAvailability GetAuthenticationStatus( SteamNetAuthenticationStatus_t *pAuthStatus ) override;
#else
	virtual void AsyncCertRequest() = 0;
	virtual void CacheIdentity() = 0;
#endif

	/// Perform cleanup and self-destruct.  Use this instead of
	/// calling operator delete.  This solves some complications
	/// due to calling virtual functions from within destructor.
	virtual void Destroy();

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
	virtual HSteamListenSocket CreateListenSocketIP( const SteamNetworkingIPAddr &localAddress, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override;
	virtual HSteamNetConnection ConnectByIPAddress( const SteamNetworkingIPAddr &adress, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override;
	virtual EResult AcceptConnection( HSteamNetConnection hConn ) override;
	virtual bool CloseConnection( HSteamNetConnection hConn, int nReason, const char *pszDebug, bool bEnableLinger ) override;
	virtual bool CloseListenSocket( HSteamListenSocket hSocket ) override;
	virtual bool SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData ) override;
	virtual int64 GetConnectionUserData( HSteamNetConnection hPeer ) override;
	virtual void SetConnectionName( HSteamNetConnection hPeer, const char *pszName ) override;
	virtual bool GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen ) override;
	virtual EResult SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags ) override;
	virtual EResult FlushMessagesOnConnection( HSteamNetConnection hConn ) override;
	virtual int ReceiveMessagesOnConnection( HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) override;
	virtual int ReceiveMessagesOnListenSocket( HSteamListenSocket hSocket, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) override;
	virtual bool GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo ) override;
	virtual bool GetQuickConnectionStatus( HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats ) override;
	virtual int GetDetailedConnectionStatus( HSteamNetConnection hConn, char *pszBuf, int cbBuf ) override;
	virtual bool GetListenSocketAddress( HSteamListenSocket hSocket, SteamNetworkingIPAddr *pAddress ) override;
	virtual bool CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 ) override;
	virtual bool GetIdentity( SteamNetworkingIdentity *pIdentity ) override;

#ifndef STEAMNETWORKINGSOCKETS_STEAM
	virtual bool GetCertificateRequest( int *pcbBlob, void *pBlob, SteamNetworkingErrMsg &errMsg ) override;
	virtual bool SetCertificate( const void *pCertificate, int cbCertificate, SteamNetworkingErrMsg &errMsg ) override;
#endif

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
	virtual void RunCallbacks( ISteamNetworkingSocketsCallbacks *pCallbacks ) override;
#endif

	/// Configuration options that will apply to all connections on this interface
	ConnectionConfig m_connectionConfig;

protected:

	void KillConnections();

	static int s_nSteamNetworkingSocketsInitted;

	SteamNetworkingIdentity m_identity;

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
	void InternalQueueCallback( int nCallback, int cbCallback, const void *pvCallback );
	struct QueuedCallback
	{
		int nCallback;
		char data[ sizeof(SteamNetConnectionStatusChangedCallback_t) ]; // whatever the biggest callback struct we have is
	};
	std::vector<QueuedCallback> m_vecPendingCallbacks;
#else
	virtual void InternalQueueCallback( int nCallback, int cbCallback, const void *pvCallback ) = 0;
#endif

	bool m_bHaveLowLevelRef;
	bool BInitLowLevel( SteamNetworkingErrMsg &errMsg )
	{
		if ( !m_bHaveLowLevelRef )
		{
			if ( !BSteamNetworkingSocketsLowLevelAddRef( errMsg) )
				return false;
			m_bHaveLowLevelRef = true;
		}
		return true;
	}

	// Protected - use Destroy()
	virtual ~CSteamNetworkingSockets();
};

class CSteamNetworkingUtils : public IClientNetworkingUtils
{
public:

	virtual SteamNetworkingMicroseconds GetLocalTimestamp() override;
	virtual void SetDebugOutputFunction( ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc ) override;

	virtual bool SetConfigValue( ESteamNetworkingConfigValue eValue,
		ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj,
		ESteamNetworkingConfigDataType eDataType, const void *pValue ) override;

	virtual ESteamNetworkingGetConfigValueResult GetConfigValue(
		ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType,
		intptr_t scopeObj, ESteamNetworkingConfigDataType *pOutDataType,
		void *pResult, size_t *cbResult ) override;

	virtual bool GetConfigValueInfo( ESteamNetworkingConfigValue eValue,
		const char **pOutName, ESteamNetworkingConfigDataType *pOutDataType,
		ESteamNetworkingConfigScope *pOutScope, ESteamNetworkingConfigValue *pOutNextValue ) override;

	virtual ESteamNetworkingConfigValue GetFirstConfigValue() override;

	virtual void SteamNetworkingIPAddr_ToString( const SteamNetworkingIPAddr &addr, char *buf, size_t cbBuf, bool bWithPort ) override;
	virtual bool SteamNetworkingIPAddr_ParseString( SteamNetworkingIPAddr *pAddr, const char *pszStr ) override;
	virtual void SteamNetworkingIdentity_ToString( const SteamNetworkingIdentity &identity, char *buf, size_t cbBuf ) override;
	virtual bool SteamNetworkingIdentity_ParseString( SteamNetworkingIdentity *pIdentity, const char *pszStr ) override;

	virtual AppId_t GetAppID();

	void SetAppID( AppId_t nAppID )
	{
		Assert( m_nAppID == 0 || m_nAppID == nAppID );
		m_nAppID = nAppID;
	}

	// Get current time of day, ideally from a source that
	// doesn't depend on the user setting their local clock properly
	virtual time_t GetTimeSecure();

protected:
	AppId_t m_nAppID = 0;
};

} // namespace SteamNetworkingSocketsLib

#endif // CSTEAMNETWORKINGSOCKETS_H
