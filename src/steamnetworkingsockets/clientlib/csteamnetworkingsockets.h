//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef CSTEAMNETWORKINGSOCKETS_H
#define CSTEAMNETWORKINGSOCKETS_H
#pragma once

#include <time.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#if defined( STEAMNETWORKINGSOCKETS_STEAMCLIENT ) || defined( STEAMNETWORKINGSOCKETS_STREAMINGCLIENT )
	#include "../../common/steam/iclientnetworkingsockets.h"
	#include "../../common/steam/iclientnetworkingutils.h"
	#define ICLIENTNETWORKING_OVERRIDE override
#else
	typedef ISteamNetworkingSockets IClientNetworkingSockets;
	typedef ISteamNetworkingUtils IClientNetworkingUtils;
	#define ICLIENTNETWORKING_OVERRIDE
#endif

#include "steamnetworkingsockets_connections.h"

namespace SteamNetworkingSocketsLib {

class CSteamNetworkingUtils;
class CSteamNetworkListenSocketP2P;

/////////////////////////////////////////////////////////////////////////////
//
// Steam API interfaces
//
/////////////////////////////////////////////////////////////////////////////

class CSteamNetworkingSockets : public IClientNetworkingSockets
{
public:
	STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW
	CSteamNetworkingSockets( CSteamNetworkingUtils *pSteamNetworkingUtils );

	CSteamNetworkingUtils *const m_pSteamNetworkingUtils;
	CMsgSteamDatagramCertificateSigned m_msgSignedCert;
	CMsgSteamDatagramCertificate m_msgCert;
	CECSigningPrivateKey m_keyPrivateKey;
	bool BCertHasIdentity() const;
	virtual bool SetCertificateAndPrivateKey( const void *pCert, int cbCert, void *pPrivateKey, int cbPrivateKey );

	bool BHasAnyConnections() const;
	bool BHasAnyListenSockets() const;
	bool BInitted() const { return m_bHaveLowLevelRef; }

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
	bool BInitGameNetworkingSockets( const SteamNetworkingIdentity *pIdentity, SteamDatagramErrMsg &errMsg );
	void CacheIdentity() { m_identity.SetLocalHost(); }
#else
	virtual void CacheIdentity() = 0;
#endif

	/// Perform cleanup and self-destruct.  Use this instead of
	/// calling operator delete.  This solves some complications
	/// due to calling virtual functions from within destructor.
	void Destroy();
	virtual void FreeResources();

	const SteamNetworkingIdentity &InternalGetIdentity()
	{
		if ( m_identity.IsInvalid() )
			CacheIdentity();
		return m_identity;
	}

	template <typename T>
	void QueueCallback( const T& x, void *fnRegisteredFunctionPtr )
	{
		InternalQueueCallback( T::k_iCallback, sizeof(T), &x, fnRegisteredFunctionPtr );
	}

	// Implements ISteamNetworkingSockets
	virtual HSteamListenSocket CreateListenSocketIP( const SteamNetworkingIPAddr &localAddress, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override;
	virtual HSteamNetConnection ConnectByIPAddress( const SteamNetworkingIPAddr &adress, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override;
	virtual HSteamListenSocket CreateListenSocketP2P( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override;
	virtual HSteamNetConnection ConnectP2P( const SteamNetworkingIdentity &identityRemote, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override;
	virtual EResult AcceptConnection( HSteamNetConnection hConn ) override;
	virtual bool CloseConnection( HSteamNetConnection hConn, int nReason, const char *pszDebug, bool bEnableLinger ) override;
	virtual bool CloseListenSocket( HSteamListenSocket hSocket ) override;
	virtual bool SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData ) override;
	virtual int64 GetConnectionUserData( HSteamNetConnection hPeer ) override;
	virtual void SetConnectionName( HSteamNetConnection hPeer, const char *pszName ) override;
	virtual bool GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen ) override;
	virtual EResult SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber ) override;
	virtual void SendMessages( int nMessages, SteamNetworkingMessage_t *const *pMessages, int64 *pOutMessageNumberOrResult ) override;
	virtual EResult FlushMessagesOnConnection( HSteamNetConnection hConn ) override;
	virtual int ReceiveMessagesOnConnection( HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) override;
	virtual bool GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo ) override;
	virtual bool GetQuickConnectionStatus( HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats ) override;
	virtual int GetDetailedConnectionStatus( HSteamNetConnection hConn, char *pszBuf, int cbBuf ) override;
	virtual bool GetListenSocketAddress( HSteamListenSocket hSocket, SteamNetworkingIPAddr *pAddress ) override;
	virtual bool CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 ) override;
	virtual bool GetIdentity( SteamNetworkingIdentity *pIdentity ) override;

	virtual HSteamNetPollGroup CreatePollGroup() override;
	virtual bool DestroyPollGroup( HSteamNetPollGroup hPollGroup ) override;
	virtual bool SetConnectionPollGroup( HSteamNetConnection hConn, HSteamNetPollGroup hPollGroup ) override;
	virtual int ReceiveMessagesOnPollGroup( HSteamNetPollGroup hPollGroup, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) override; 
	virtual HSteamNetConnection ConnectP2PCustomSignaling( ISteamNetworkingConnectionSignaling *pSignaling, const SteamNetworkingIdentity *pPeerIdentity, int nVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override;
	virtual bool ReceivedP2PCustomSignal( const void *pMsg, int cbMsg, ISteamNetworkingSignalingRecvContext *pContext ) override;
	virtual int GetP2P_Transport_ICE_Enable( const SteamNetworkingIdentity &identityRemote );

	virtual bool GetCertificateRequest( int *pcbBlob, void *pBlob, SteamNetworkingErrMsg &errMsg ) override;
	virtual bool SetCertificate( const void *pCertificate, int cbCertificate, SteamNetworkingErrMsg &errMsg ) override;

#ifdef STEAMNETWORKINGSOCKETS_STEAMCLIENT
	virtual int ReceiveMessagesOnListenSocketLegacyPollGroup( HSteamListenSocket hSocket, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) override;
#endif

	virtual void RunCallbacks() override;

	/// Configuration options that will apply to all connections on this interface
	ConnectionConfig m_connectionConfig;

	/// List of existing CSteamNetworkingSockets instances.  This is used, for example,
	/// if we want to initiate a P2P connection to a local identity, we can instead
	/// use a loopback connection.
	static std::vector<CSteamNetworkingSockets *> s_vecSteamNetworkingSocketsInstances;

	// P2P listen sockets
	CUtlHashMap<int,CSteamNetworkListenSocketP2P *,std::equal_to<int>,std::hash<int>> m_mapListenSocketsByVirtualPort;
	CSteamNetworkListenSocketP2P *InternalCreateListenSocketP2P( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions );

	//
	// Authentication
	//

#ifdef STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT
	virtual bool BCertRequestInFlight() = 0;

	ScheduledMethodThinker<CSteamNetworkingSockets> m_scheduleCheckRenewCert;

	/// Platform-specific code to actually obtain a cert
	virtual void BeginFetchCertAsync() = 0;
#else
	inline bool BCertRequestInFlight() { return false; }
#endif

	/// Called in any situation where we need to be able to authenticate, or anticipate
	/// needing to be able to do so soon.  If we don't have one right now, we will begin
	/// taking action to obtain one
	virtual void CheckAuthenticationPrerequisites( SteamNetworkingMicroseconds usecNow );
	void AuthenticationNeeded() { CheckAuthenticationPrerequisites( SteamNetworkingSockets_GetLocalTimestamp() ); }

	virtual ESteamNetworkingAvailability InitAuthentication() override final;
	virtual ESteamNetworkingAvailability GetAuthenticationStatus( SteamNetAuthenticationStatus_t *pAuthStatus ) override final;
	int GetSecondsUntilCertExpiry() const;

	//
	// Default signaling
	//

	CSteamNetworkConnectionBase *InternalConnectP2PDefaultSignaling( const SteamNetworkingIdentity &identityRemote, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions );
	CSteamNetworkingMessages *GetSteamNetworkingMessages();
	CSteamNetworkingMessages *m_pSteamNetworkingMessages;

// Stubs if SDR not enabled
#ifndef STEAMNETWORKINGSOCKETS_ENABLE_SDR
	virtual int FindRelayAuthTicketForServer( const SteamNetworkingIdentity &identityGameServer, int nRemoteVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket ) override { return 0; }
	virtual HSteamNetConnection ConnectToHostedDedicatedServer( const SteamNetworkingIdentity &identityTarget, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override { return k_HSteamNetConnection_Invalid; }
	virtual uint16 GetHostedDedicatedServerPort() override { return 0; }
	virtual SteamNetworkingPOPID GetHostedDedicatedServerPOPID() override { return 0; }
	virtual EResult GetHostedDedicatedServerAddress( SteamDatagramHostedAddress *pRouting ) override { return k_EResultFail; }
	virtual HSteamListenSocket CreateHostedDedicatedServerListenSocket( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions ) override { return k_HSteamNetConnection_Invalid; }
	virtual bool ReceivedRelayAuthTicket( const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket ) override { return false; }
	virtual EResult GetGameCoordinatorServerLogin( SteamDatagramGameCoordinatorServerLogin *pLogin, int *pcbSignedBlob, void *pBlob ) override { return k_EResultFail; }
#endif

protected:

	/// Overall authentication status.  Depends on the status of our cert, and the ability
	/// to obtain the CA certs (from the network config)
	SteamNetAuthenticationStatus_t m_AuthenticationStatus;

	/// Set new status, dispatch callbacks if it actually changed
	void SetAuthenticationStatus( const SteamNetAuthenticationStatus_t &newStatus );

	/// Current status of our attempt to get a certificate
	bool m_bEverTriedToGetCert;
	bool m_bEverGotCert;
	SteamNetAuthenticationStatus_t m_CertStatus;

	/// Set cert status, and then update m_AuthenticationStatus and
	/// dispatch any callbacks as needed
	void SetCertStatus( ESteamNetworkingAvailability eAvail, const char *pszFmt, ... );
#ifdef STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT
	void AsyncCertRequestFinished();
	void CertRequestFailed( ESteamNetworkingAvailability eCertAvail, ESteamNetConnectionEnd nConnectionEndReason, const char *pszMsg );
#endif

	/// Figure out the current authentication status.  And if it has changed, send out callbacks
	virtual void DeduceAuthenticationStatus();

	void KillConnections();

	SteamNetworkingIdentity m_identity;

	struct QueuedCallback
	{
		int nCallback;
		void *fnCallback;
		char data[ sizeof(SteamNetConnectionStatusChangedCallback_t) ]; // whatever the biggest callback struct we have is
	};
	std_vector<QueuedCallback> m_vecPendingCallbacks;
	virtual void InternalQueueCallback( int nCallback, int cbCallback, const void *pvCallback, void *fnRegisteredFunctionPtr );

	bool m_bHaveLowLevelRef;
	bool BInitLowLevel( SteamNetworkingErrMsg &errMsg );

	CSteamNetworkConnectionBase *InternalConnectP2P( ISteamNetworkingConnectionSignaling *pSignaling, const SteamNetworkingIdentity *pPeerIdentity, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions );
	bool InternalReceivedP2PSignal( const void *pMsg, int cbMsg, ISteamNetworkingSignalingRecvContext *pContext, bool bDefaultPlatformSignaling );

	// Protected - use Destroy()
	virtual ~CSteamNetworkingSockets();
};

class CSteamNetworkingUtils : public IClientNetworkingUtils
{
public:
	STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW
	virtual ~CSteamNetworkingUtils();

	virtual SteamNetworkingMessage_t *AllocateMessage( int cbAllocateBuffer ) override;

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

	// Stubs if SDR not enabled
#ifndef STEAMNETWORKINGSOCKETS_ENABLE_SDR
	virtual ESteamNetworkingAvailability GetRelayNetworkStatus( SteamRelayNetworkStatus_t *pDetails ) override
	{
		if ( pDetails )
		{
			memset( pDetails, 0, sizeof(*pDetails) );
			pDetails->m_eAvail = k_ESteamNetworkingAvailability_CannotTry;
			pDetails->m_eAvailAnyRelay = k_ESteamNetworkingAvailability_CannotTry;
			pDetails->m_eAvailNetworkConfig = k_ESteamNetworkingAvailability_CannotTry;
		}
		return k_ESteamNetworkingAvailability_CannotTry;
	}
	virtual bool CheckPingDataUpToDate( float flMaxAgeSeconds ) override { return false; }
	virtual float GetLocalPingLocation( SteamNetworkPingLocation_t &result ) override { return -1.0f; }
	virtual int EstimatePingTimeBetweenTwoLocations( const SteamNetworkPingLocation_t &location1, const SteamNetworkPingLocation_t &location2 ) override { return k_nSteamNetworkingPing_Unknown; }
	virtual int EstimatePingTimeFromLocalHost( const SteamNetworkPingLocation_t &remoteLocation ) override { return k_nSteamNetworkingPing_Unknown; }
	virtual void ConvertPingLocationToString( const SteamNetworkPingLocation_t &location, char *pszBuf, int cchBufSize ) override { if ( pszBuf ) *pszBuf = '\0'; }
	virtual bool ParsePingLocationString( const char *pszString, SteamNetworkPingLocation_t &result ) override { return false; }
	virtual int GetPingToDataCenter( SteamNetworkingPOPID popID, SteamNetworkingPOPID *pViaRelayPoP ) override { return k_nSteamNetworkingPing_Unknown; }
	virtual int GetDirectPingToPOP( SteamNetworkingPOPID popID ) override { return k_nSteamNetworkingPing_Unknown; }
	virtual int GetPOPCount() override { return 0; }
	virtual int GetPOPList( SteamNetworkingPOPID *list, int nListSz ) override { return 0; }
#endif

protected:
	AppId_t m_nAppID = 0;
};

} // namespace SteamNetworkingSocketsLib

#endif // CSTEAMNETWORKINGSOCKETS_H
