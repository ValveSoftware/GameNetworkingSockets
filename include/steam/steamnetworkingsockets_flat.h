//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Plain C interface to SteamNetworkingSockets
//
// Designed to match the auto-generated flat interface in the Steamworks SDK
// (for better or worse...)
//
//=============================================================================

#ifndef STEAMNETWORKINGSOCKETS_FLAT
#define STEAMNETWORKINGSOCKETS_FLAT
#pragma once

#include <stdint.h>
#include "steamnetworkingtypes.h"

extern "C" {

STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP( intptr_t instancePtr, const SteamNetworkingIPAddr *pAddress, int nOptions, const SteamNetworkingConfigValue_t *pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress( intptr_t instancePtr, const SteamNetworkingIPAddr *pAddress, int nOptions, const SteamNetworkingConfigValue_t *pOptions );
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P( intptr_t instancePtr, int nVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectP2P( intptr_t instancePtr, const SteamNetworkingIdentity *pIdentity, int nVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions );
#endif
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_AcceptConnection( intptr_t instancePtr, HSteamNetConnection hConn );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CloseConnection( intptr_t instancePtr, HSteamNetConnection hPeer, int nReason, const char *pszDebug, bool bEnableLinger );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CloseListenSocket( intptr_t instancePtr, HSteamListenSocket hSocket );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_SetConnectionUserData( intptr_t instancePtr, HSteamNetConnection hPeer, int64 nUserData );
STEAMNETWORKINGSOCKETS_INTERFACE int64 SteamAPI_ISteamNetworkingSockets_GetConnectionUserData( intptr_t instancePtr, HSteamNetConnection hPeer );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingSockets_SetConnectionName( intptr_t instancePtr, HSteamNetConnection hPeer, const char *pszName );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetConnectionName( intptr_t instancePtr, HSteamNetConnection hPeer, char *pszName, int nMaxLen );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_SendMessageToConnection( intptr_t instancePtr, HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingSockets_SendMessages( intptr_t instancePtr, int nMessages, SteamNetworkingMessage_t *const *pMessages, int64 *pOutMessageNumberOrResult );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection( intptr_t instancePtr, HSteamNetConnection hConn );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection( intptr_t instancePtr, HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ); 
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetConnectionInfo( intptr_t instancePtr, HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetQuickConnectionStatus( intptr_t instancePtr, HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus( intptr_t instancePtr, HSteamNetConnection hConn, char *pszBuf, int cbBuf );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress( intptr_t instancePtr, HSteamListenSocket hSocket, SteamNetworkingIPAddr *pAddress );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CreateSocketPair( intptr_t instancePtr, HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetIdentity( intptr_t instancePtr, SteamNetworkingIdentity *pIdentity );

STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingAvailability SteamAPI_ISteamNetworkingSockets_InitAuthentication( intptr_t instancePtr );
STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingAvailability SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus( intptr_t instancePtr, SteamNetAuthenticationStatus_t *pDetails );

STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetPollGroup SteamAPI_ISteamNetworkingSockets_CreatePollGroup( intptr_t instancePtr );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_DestroyPollGroup( intptr_t instancePtr, HSteamNetPollGroup hPollGroup );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup( intptr_t instancePtr, HSteamNetConnection hConn, HSteamNetPollGroup hPollGroup );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup( intptr_t instancePtr, HSteamNetPollGroup hPollGroup, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages );

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket( intptr_t instancePtr, const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer( intptr_t instancePtr, const SteamNetworkingIdentity *pIdentityGameserver, int nVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer( intptr_t instancePtr, const SteamNetworkingIdentity *pIdentityTarget, int nVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE uint16 SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort( intptr_t instancePtr );
STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingPOPID SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID( intptr_t instancePtr );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress( intptr_t instancePtr, SteamDatagramHostedAddress *pRouting );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket( intptr_t instancePtr, int nVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin( intptr_t instancePtr, SteamDatagramGameCoordinatorServerLogin *pLoginInfo, int *pcbSignedBlob, void *pBlob );

#endif // #ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_Clear( SteamNetworkingIPAddr *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros( const SteamNetworkingIPAddr *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_SetIPv6( SteamNetworkingIPAddr *pThis, const uint8 *ipv6, uint16 nPort );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_SetIPv4( SteamNetworkingIPAddr *pThis, uint32 nIP, uint16 nPort );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_IsIPv4( const SteamNetworkingIPAddr *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE uint32 SteamAPI_SteamNetworkingIPAddr_GetIPv4( const SteamNetworkingIPAddr *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost( SteamNetworkingIPAddr *pThis, uint16 nPort );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_IsLocalHost( const SteamNetworkingIPAddr *pThis );
// Note: in steamnetworkingtypes.h:
// SteamAPI_SteamNetworkingIPAddr_ToString
// SteamAPI_SteamNetworkingIPAddr_ParseString

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_Clear( SteamNetworkingIdentity *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_IsInvalid( const SteamNetworkingIdentity *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_SetSteamID64( SteamNetworkingIdentity *pThis, uint64 steamID );
STEAMNETWORKINGSOCKETS_INTERFACE uint64 SteamAPI_SteamNetworkingIdentity_GetSteamID64( const SteamNetworkingIdentity *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_SetIPAddr( SteamNetworkingIdentity *pThis, const SteamNetworkingIPAddr *pAddr );
STEAMNETWORKINGSOCKETS_INTERFACE const SteamNetworkingIPAddr *SteamAPI_SteamNetworkingIdentity_GetIPAddr( SteamNetworkingIdentity *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_SetLocalHost( SteamNetworkingIdentity *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_IsLocalHost( const SteamNetworkingIdentity *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_SetGenericString( SteamNetworkingIdentity *pThis, const char *pszString );
STEAMNETWORKINGSOCKETS_INTERFACE const char *SteamAPI_SteamNetworkingIdentity_GetGenericString( const SteamNetworkingIdentity *pThis );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_SetGenericBytes( SteamNetworkingIdentity *pThis, const void *data, size_t cbLen );
STEAMNETWORKINGSOCKETS_INTERFACE const uint8 *SteamAPI_SteamNetworkingIdentity_GetGenericBytes( const SteamNetworkingIdentity *pThis, int *pOutLen );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_EqualTo( const SteamNetworkingIdentity *a, const SteamNetworkingIdentity *b );
// Note: in steamnetworkingtypes.h
// SteamAPI_SteamNetworkingIdentity_ToString
// SteamAPI_SteamNetworkingIdentity_ParseString

// Callback dispatch mechanism when using the lib in standalone mode.
#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
typedef void (*FSteamNetConnectionStatusChangedCallback)( SteamNetConnectionStatusChangedCallback_t *pInfo, intptr_t context );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingSockets_RunConnectionStatusChangedCallbacks ( intptr_t instancePtr, FSteamNetConnectionStatusChangedCallback callback, intptr_t context );
#endif

//
// ISteamNetworkingUtils
//

STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingMessage_t *SteamAPI_ISteamNetworkingUtils_AllocateMessage( intptr_t instancePtr, int cbAllocateBuffer );
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess( intptr_t instancePtr );
STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingAvailability SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus( intptr_t instancePtr, SteamRelayNetworkStatus_t *pDetails );
STEAMNETWORKINGSOCKETS_INTERFACE float SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation( intptr_t instancePtr, SteamNetworkPingLocation_t *result );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations( intptr_t instancePtr, const SteamNetworkPingLocation_t *location1, const SteamNetworkPingLocation_t *location2 );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost( intptr_t instancePtr, const SteamNetworkPingLocation_t *remoteLocation );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString( intptr_t instancePtr, const SteamNetworkPingLocation_t *location, char *pszBuf, int cchBufSize );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_ParsePingLocationString( intptr_t instancePtr, const char *pszString, SteamNetworkPingLocation_t *result );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate( intptr_t instancePtr, float flMaxAgeSeconds );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter( intptr_t instancePtr, SteamNetworkingPOPID popID, SteamNetworkingPOPID *pViaRelayPoP );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP( intptr_t instancePtr, SteamNetworkingPOPID popID );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetPOPCount( intptr_t instancePtr );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetPOPList( intptr_t instancePtr, SteamNetworkingPOPID *list, int nListSz );
#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR

STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingMicroseconds SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp( intptr_t instancePtr );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction( intptr_t instancePtr, ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetConfigValue( intptr_t instancePtr, ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj,
	ESteamNetworkingConfigDataType eDataType, const void *pValue );
STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingGetConfigValueResult SteamAPI_ISteamNetworkingUtils_GetConfigValue( intptr_t instancePtr, ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj,
	ESteamNetworkingConfigDataType *pOutDataType, void *pResult, size_t *cbResult );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo( intptr_t instancePtr, ESteamNetworkingConfigValue eValue, const char **pOutName, ESteamNetworkingConfigDataType *pOutDataType, ESteamNetworkingConfigScope *pOutScope, ESteamNetworkingConfigValue *pOutNextValue );
STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingConfigValue SteamAPI_ISteamNetworkingUtils_GetFirstConfigValue( intptr_t instancePtr );

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingMessage_t_Release( SteamNetworkingMessage_t *pIMsg );

}

#endif // ISTEAMNETWORKINGSOCKETS
