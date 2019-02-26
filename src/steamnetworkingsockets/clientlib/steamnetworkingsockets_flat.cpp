//====== Copyright Valve Corporation, All rights reserved. ====================

#include <steam/steamnetworkingsockets_flat.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

extern "C" {

STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP( intptr_t instancePtr, const SteamNetworkingIPAddr *pAddress )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CreateListenSocketIP( *pAddress );
}

STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress( intptr_t instancePtr, const SteamNetworkingIPAddr *pAddress )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ConnectByIPAddress( *pAddress );
}

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P( intptr_t instancePtr, int nVirtualPort )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CreateListenSocketP2P( nVirtualPort );
}

STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectP2P( intptr_t instancePtr, const SteamNetworkingIdentity *pIdentity, int nVirtualPort )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ConnectP2P( *pIdentity, nVirtualPort );
}
#endif

STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_AcceptConnection( intptr_t instancePtr, HSteamNetConnection hConn )
{
	return ((ISteamNetworkingSockets*)instancePtr)->AcceptConnection( hConn );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CloseConnection( intptr_t instancePtr, HSteamNetConnection hPeer, int nReason, const char *pszDebug, bool bEnableLinger )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CloseConnection( hPeer, nReason, pszDebug, bEnableLinger );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CloseListenSocket( intptr_t instancePtr, HSteamListenSocket hSocket )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CloseListenSocket( hSocket );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_SetConnectionUserData( intptr_t instancePtr, HSteamNetConnection hPeer, int64 nUserData )
{
	return ((ISteamNetworkingSockets*)instancePtr)->SetConnectionUserData( hPeer, nUserData );
}

STEAMNETWORKINGSOCKETS_INTERFACE int64 SteamAPI_ISteamNetworkingSockets_GetConnectionUserData( intptr_t instancePtr, HSteamNetConnection hPeer )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetConnectionUserData( hPeer );
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingSockets_SetConnectionName( intptr_t instancePtr, HSteamNetConnection hPeer, const char *pszName )
{
	return ((ISteamNetworkingSockets*)instancePtr)->SetConnectionName( hPeer, pszName );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetConnectionName( intptr_t instancePtr, HSteamNetConnection hPeer, char *pszName, int nMaxLen )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetConnectionName( hPeer, pszName, nMaxLen );
}

STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_SendMessageToConnection( intptr_t instancePtr, HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags )
{
	return ((ISteamNetworkingSockets*)instancePtr)->SendMessageToConnection( hConn, pData, cbData, nSendFlags );
}

STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection( intptr_t instancePtr, HSteamNetConnection hConn )
{
	return ((ISteamNetworkingSockets*)instancePtr)->FlushMessagesOnConnection( hConn );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection( intptr_t instancePtr, HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ReceiveMessagesOnConnection( hConn, ppOutMessages, nMaxMessages );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnListenSocket( intptr_t instancePtr, HSteamListenSocket hSocket, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ReceiveMessagesOnListenSocket( hSocket, ppOutMessages, nMaxMessages );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetConnectionInfo( intptr_t instancePtr, HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetConnectionInfo( hConn, pInfo );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetQuickConnectionStatus( intptr_t instancePtr, HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetQuickConnectionStatus( hConn, pStats );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus( intptr_t instancePtr, HSteamNetConnection hConn, char *pszBuf, int cbBuf )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetDetailedConnectionStatus( hConn, pszBuf, cbBuf );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress( intptr_t instancePtr, HSteamListenSocket hSocket, SteamNetworkingIPAddr *pAddress )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetListenSocketAddress( hSocket, pAddress );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CreateSocketPair( intptr_t instancePtr, HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CreateSocketPair( pOutConnection1, pOutConnection2, bUseNetworkLoopback, pIdentity1, pIdentity2 );
}

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket( intptr_t instancePtr, const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ReceivedRelayAuthTicket( pvTicket, cbTicket, pOutParsedTicket );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer( intptr_t instancePtr, const SteamNetworkingIdentity *pIdentityGameserver, int nVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket )
{
	return ((ISteamNetworkingSockets*)instancePtr)->FindRelayAuthTicketForServer( *pIdentityGameserver, nVirtualPort, pOutParsedTicket );
}

STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer( intptr_t instancePtr, const SteamNetworkingIdentity *pIdentityTarget, int nVirtualPort )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ConnectToHostedDedicatedServer( *pIdentityTarget, nVirtualPort );
}

STEAMNETWORKINGSOCKETS_INTERFACE uint16 SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort( intptr_t instancePtr )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetHostedDedicatedServerPort();
}

STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingPOPID SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID( intptr_t instancePtr )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetHostedDedicatedServerPOPID();
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress( intptr_t instancePtr, SteamDatagramHostedAddress *pRouting )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetHostedDedicatedServerAddress( pRouting );
}

STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket( intptr_t instancePtr, int nVirtualPort )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CreateHostedDedicatedServerListenSocket( nVirtualPort );
}

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_Clear( SteamNetworkingIPAddr *pThis )
{
	pThis->Clear();
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros( const SteamNetworkingIPAddr *pThis )
{
	return pThis->IsIPv6AllZeros();
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_SetIPv6( SteamNetworkingIPAddr *pThis, const uint8 *ipv6, uint16 nPort )
{
	pThis->SetIPv6( ipv6, nPort );
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_SetIPv4( SteamNetworkingIPAddr *pThis, uint32 nIP, uint16 nPort )
{
	pThis->SetIPv4( nIP, nPort );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_IsIPv4( const SteamNetworkingIPAddr *pThis )
{
	return pThis->IsIPv4();
}

STEAMNETWORKINGSOCKETS_INTERFACE uint32 SteamAPI_SteamNetworkingIPAddr_GetIPv4( const SteamNetworkingIPAddr *pThis )
{
	return pThis->GetIPv4();
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost( SteamNetworkingIPAddr *pThis, uint16 nPort )
{
	pThis->SetIPv6LocalHost( nPort );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_IsLocalHost( const SteamNetworkingIPAddr *pThis )
{
	return pThis->IsLocalHost();
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_Clear( SteamNetworkingIdentity *pThis )
{
	pThis->Clear();
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_IsInvalid( const SteamNetworkingIdentity *pThis )
{
	return pThis->IsInvalid();
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_SetSteamID64( SteamNetworkingIdentity *pThis, uint64 steamID )
{
	pThis->SetSteamID64( steamID );
}

STEAMNETWORKINGSOCKETS_INTERFACE uint64 SteamAPI_SteamNetworkingIdentity_GetSteamID64( const SteamNetworkingIdentity *pThis )
{
	return pThis->GetSteamID64();
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_SetIPAddr( SteamNetworkingIdentity *pThis, const SteamNetworkingIPAddr *pAddr )
{
	pThis->SetIPAddr( *pAddr );
}

STEAMNETWORKINGSOCKETS_INTERFACE const SteamNetworkingIPAddr *SteamAPI_SteamNetworkingIdentity_GetIPAddr( SteamNetworkingIdentity *pThis )
{
	return pThis->GetIPAddr();
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_SetLocalHost( SteamNetworkingIdentity *pThis )
{
	pThis->SetLocalHost();
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_IsLocalHost( const SteamNetworkingIdentity *pThis )
{
	return pThis->IsLocalHost();
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_SetGenericString( SteamNetworkingIdentity *pThis, const char *pszString )
{
	return pThis->SetGenericString( pszString );
}

STEAMNETWORKINGSOCKETS_INTERFACE const char *SteamAPI_SteamNetworkingIdentity_GetGenericString( const SteamNetworkingIdentity *pThis )
{
	return pThis->GetGenericString();
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_SetGenericBytes( SteamNetworkingIdentity *pThis, const void *data, size_t cbLen )
{
	return pThis->SetGenericBytes( data, cbLen );
}

STEAMNETWORKINGSOCKETS_INTERFACE const uint8 *SteamAPI_SteamNetworkingIdentity_GetGenericBytes( const SteamNetworkingIdentity *pThis, int *pOutLen )
{
	return pThis->GetGenericBytes( *pOutLen );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_EqualTo( const SteamNetworkingIdentity *a, const SteamNetworkingIdentity *b )
{
	return (*a == *b);
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingSockets_RunConnectionStatusChangedCallbacks( intptr_t instancePtr, FSteamNetConnectionStatusChangedCallback callback, intptr_t context )
{
	struct CallbackAdapter : ISteamNetworkingSocketsCallbacks
	{
		CallbackAdapter( FSteamNetConnectionStatusChangedCallback _callback, intptr_t _context )
		: m_callback( _callback ), m_context( _context ) {}

		virtual void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *pInfo ) override
		{
			(*m_callback)( pInfo, m_context );
		}

		FSteamNetConnectionStatusChangedCallback m_callback;
		intptr_t m_context;
	};

	CallbackAdapter adapter( callback, context );
	((ISteamNetworkingSockets*)instancePtr)->RunCallbacks( &adapter );
}

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
STEAMNETWORKINGSOCKETS_INTERFACE float SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation( intptr_t instancePtr, SteamNetworkPingLocation_t *result )
{
	return ((ISteamNetworkingUtils*)instancePtr)->GetLocalPingLocation( *result );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations( intptr_t instancePtr, const SteamNetworkPingLocation_t *location1, const SteamNetworkPingLocation_t *location2 )
{
	return ((ISteamNetworkingUtils*)instancePtr)->EstimatePingTimeBetweenTwoLocations( *location1, *location2 );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost( intptr_t instancePtr, const SteamNetworkPingLocation_t *remoteLocation )
{
	return ((ISteamNetworkingUtils*)instancePtr)->EstimatePingTimeFromLocalHost( *remoteLocation );
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString( intptr_t instancePtr, const SteamNetworkPingLocation_t *location, char *pszBuf, int cchBufSize )
{
	((ISteamNetworkingUtils*)instancePtr)->ConvertPingLocationToString( *location, pszBuf, cchBufSize );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_ParsePingLocationString( intptr_t instancePtr, const char *pszString, SteamNetworkPingLocation_t *result )
{
	return ((ISteamNetworkingUtils*)instancePtr)->ParsePingLocationString( pszString, *result );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate( intptr_t instancePtr, float flMaxAgeSeconds )
{
	return ((ISteamNetworkingUtils*)instancePtr)->CheckPingDataUpToDate( flMaxAgeSeconds );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_IsPingMeasurementInProgress( intptr_t instancePtr )
{
	return ((ISteamNetworkingUtils*)instancePtr)->IsPingMeasurementInProgress();
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter( intptr_t instancePtr, SteamNetworkingPOPID popID, SteamNetworkingPOPID *pViaRelayPoP )
{
	return ((ISteamNetworkingUtils*)instancePtr)->GetPingToDataCenter( popID, pViaRelayPoP );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP( intptr_t instancePtr, SteamNetworkingPOPID popID )
{
	return ((ISteamNetworkingUtils*)instancePtr)->GetDirectPingToPOP( popID );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetPOPCount( intptr_t instancePtr )
{
	return ((ISteamNetworkingUtils*)instancePtr)->GetPOPCount();
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetPOPList( intptr_t instancePtr, SteamNetworkingPOPID *list, int nListSz )
{
	return ((ISteamNetworkingUtils*)instancePtr)->GetPOPList( list, nListSz );
}

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR

STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingMicroseconds SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp( intptr_t instancePtr )
{
	return ((ISteamNetworkingUtils*)instancePtr)->GetLocalTimestamp();
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction( intptr_t instancePtr, ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc )
{
	return ((ISteamNetworkingUtils*)instancePtr)->SetDebugOutputFunction( eDetailLevel, pfnFunc );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetConfigValue( intptr_t instancePtr, ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj,
	ESteamNetworkingConfigDataType eDataType, const void *pValue )
{
	return ((ISteamNetworkingUtils*)instancePtr)->SetConfigValue( eValue, eScopeType, scopeObj, eDataType, pValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingGetConfigValueResult SteamAPI_ISteamNetworkingUtils_GetConfigValue( intptr_t instancePtr, ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj,
	ESteamNetworkingConfigDataType *pOutDataType, void *pResult, size_t *cbResult )
{
	return ((ISteamNetworkingUtils*)instancePtr)->GetConfigValue( eValue, eScopeType, scopeObj, pOutDataType, pResult, cbResult );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo( intptr_t instancePtr, ESteamNetworkingConfigValue eValue, const char **pOutName, ESteamNetworkingConfigDataType *pOutDataType, ESteamNetworkingConfigScope *pOutScope, ESteamNetworkingConfigValue *pOutNextValue )
{
	return ((ISteamNetworkingUtils*)instancePtr)->GetConfigValueInfo( eValue, pOutName, pOutDataType, pOutScope, pOutNextValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingConfigValue SteamAPI_ISteamNetworkingUtils_GetFirstConfigValue( intptr_t instancePtr )
{
	return ((ISteamNetworkingUtils*)instancePtr)->GetFirstConfigValue();
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingMessage_t_Release( SteamNetworkingMessage_t *pIMsg )
{
	pIMsg->Release();
}

}
