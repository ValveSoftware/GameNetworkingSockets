//====== Copyright Valve Corporation, All rights reserved. ====================
//
// "Flat" interface to SteamNetworkingSockets.
//
// Designed to match the auto-generated flat interface in the Steamworks SDK
// (for better or worse...)  It uses plain C linkage, but it is C++ code, and
// is not intended to compile as C code.
//
//=============================================================================

#ifndef STEAMNETWORKINGSOCKETS_FLAT
#define STEAMNETWORKINGSOCKETS_FLAT
#pragma once

#include "steamnetworkingtypes.h"
#include "isteamnetworkingsockets.h"
#include "isteamnetworkingutils.h"
class ISteamNetworkingConnectionSignaling;

typedef uint64 uint64_steamid; // Used when passing or returning CSteamID

// ISteamNetworkingSockets
STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamAPI_SteamNetworkingSockets_v009();
STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP( ISteamNetworkingSockets* self, const SteamNetworkingIPAddr & localAddress, int nOptions, const SteamNetworkingConfigValue_t * pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress( ISteamNetworkingSockets* self, const SteamNetworkingIPAddr & address, int nOptions, const SteamNetworkingConfigValue_t * pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P( ISteamNetworkingSockets* self, int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t * pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectP2P( ISteamNetworkingSockets* self, const SteamNetworkingIdentity & identityRemote, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t * pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_AcceptConnection( ISteamNetworkingSockets* self, HSteamNetConnection hConn );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CloseConnection( ISteamNetworkingSockets* self, HSteamNetConnection hPeer, int nReason, const char * pszDebug, bool bEnableLinger );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CloseListenSocket( ISteamNetworkingSockets* self, HSteamListenSocket hSocket );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_SetConnectionUserData( ISteamNetworkingSockets* self, HSteamNetConnection hPeer, int64 nUserData );
STEAMNETWORKINGSOCKETS_INTERFACE int64 SteamAPI_ISteamNetworkingSockets_GetConnectionUserData( ISteamNetworkingSockets* self, HSteamNetConnection hPeer );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingSockets_SetConnectionName( ISteamNetworkingSockets* self, HSteamNetConnection hPeer, const char * pszName );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetConnectionName( ISteamNetworkingSockets* self, HSteamNetConnection hPeer, char * pszName, int nMaxLen );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_SendMessageToConnection( ISteamNetworkingSockets* self, HSteamNetConnection hConn, const void * pData, uint32 cbData, int nSendFlags, int64 * pOutMessageNumber );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingSockets_SendMessages( ISteamNetworkingSockets* self, int nMessages, SteamNetworkingMessage_t *const * pMessages, int64 * pOutMessageNumberOrResult );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection( ISteamNetworkingSockets* self, HSteamNetConnection hConn );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection( ISteamNetworkingSockets* self, HSteamNetConnection hConn, SteamNetworkingMessage_t ** ppOutMessages, int nMaxMessages );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetConnectionInfo( ISteamNetworkingSockets* self, HSteamNetConnection hConn, SteamNetConnectionInfo_t * pInfo );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus( ISteamNetworkingSockets *self, HSteamNetConnection hConn, SteamNetConnectionRealTimeStatus_t *pStats, int nLanes, SteamNetConnectionRealTimeLaneStatus_t *pLanes );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus( ISteamNetworkingSockets* self, HSteamNetConnection hConn, char * pszBuf, int cbBuf );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress( ISteamNetworkingSockets* self, HSteamListenSocket hSocket, SteamNetworkingIPAddr * address );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CreateSocketPair( ISteamNetworkingSockets* self, HSteamNetConnection * pOutConnection1, HSteamNetConnection * pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity * pIdentity1, const SteamNetworkingIdentity * pIdentity2 );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes(ISteamNetworkingSockets *self, HSteamNetConnection hConn, int nNumLanes, const int *pLanePriorities, const uint16 *pLaneWeights );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetIdentity( ISteamNetworkingSockets* self, SteamNetworkingIdentity * pIdentity );
STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingAvailability SteamAPI_ISteamNetworkingSockets_InitAuthentication( ISteamNetworkingSockets* self );
STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingAvailability SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus( ISteamNetworkingSockets* self, SteamNetAuthenticationStatus_t * pDetails );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetPollGroup SteamAPI_ISteamNetworkingSockets_CreatePollGroup( ISteamNetworkingSockets* self );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_DestroyPollGroup( ISteamNetworkingSockets* self, HSteamNetPollGroup hPollGroup );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup( ISteamNetworkingSockets* self, HSteamNetConnection hConn, HSteamNetPollGroup hPollGroup );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup( ISteamNetworkingSockets* self, HSteamNetPollGroup hPollGroup, SteamNetworkingMessage_t ** ppOutMessages, int nMaxMessages );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket( ISteamNetworkingSockets* self, const void * pvTicket, int cbTicket, SteamDatagramRelayAuthTicket * pOutParsedTicket );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer( ISteamNetworkingSockets* self, const SteamNetworkingIdentity & identityGameServer, int nRemoteVirtualPort, SteamDatagramRelayAuthTicket * pOutParsedTicket );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer( ISteamNetworkingSockets* self, const SteamNetworkingIdentity & identityTarget, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t * pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE uint16 SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort( ISteamNetworkingSockets* self );
STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingPOPID SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID( ISteamNetworkingSockets* self );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress( ISteamNetworkingSockets* self, SteamDatagramHostedAddress * pRouting );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket( ISteamNetworkingSockets* self, int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t * pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin( ISteamNetworkingSockets* self, SteamDatagramGameCoordinatorServerLogin * pLoginInfo, int * pcbSignedBlob, void * pBlob );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling( ISteamNetworkingSockets* self, ISteamNetworkingConnectionSignaling * pSignaling, const SteamNetworkingIdentity * pPeerIdentity, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t * pOptions );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal( ISteamNetworkingSockets* self, const void * pMsg, int cbMsg, ISteamNetworkingSignalingRecvContext * pContext );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetCertificateRequest( ISteamNetworkingSockets* self, int * pcbBlob, void * pBlob, SteamNetworkingErrMsg & errMsg );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_SetCertificate( ISteamNetworkingSockets* self, const void * pCertificate, int cbCertificate, SteamNetworkingErrMsg & errMsg );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingSockets_RunCallbacks( ISteamNetworkingSockets* self );

// ISteamNetworkingUtils
STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingUtils *SteamAPI_SteamNetworkingUtils_v003();
STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingMessage_t * SteamAPI_ISteamNetworkingUtils_AllocateMessage( ISteamNetworkingUtils* self, int cbAllocateBuffer );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess( ISteamNetworkingUtils* self );
STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingAvailability SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus( ISteamNetworkingUtils* self, SteamRelayNetworkStatus_t * pDetails );
STEAMNETWORKINGSOCKETS_INTERFACE float SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation( ISteamNetworkingUtils* self, SteamNetworkPingLocation_t & result );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations( ISteamNetworkingUtils* self, const SteamNetworkPingLocation_t & location1, const SteamNetworkPingLocation_t & location2 );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost( ISteamNetworkingUtils* self, const SteamNetworkPingLocation_t & remoteLocation );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString( ISteamNetworkingUtils* self, const SteamNetworkPingLocation_t & location, char * pszBuf, int cchBufSize );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_ParsePingLocationString( ISteamNetworkingUtils* self, const char * pszString, SteamNetworkPingLocation_t & result );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate( ISteamNetworkingUtils* self, float flMaxAgeSeconds );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter( ISteamNetworkingUtils* self, SteamNetworkingPOPID popID, SteamNetworkingPOPID * pViaRelayPoP );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP( ISteamNetworkingUtils* self, SteamNetworkingPOPID popID );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetPOPCount( ISteamNetworkingUtils* self );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingUtils_GetPOPList( ISteamNetworkingUtils* self, SteamNetworkingPOPID * list, int nListSz );
STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingMicroseconds SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp( ISteamNetworkingUtils* self );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction( ISteamNetworkingUtils* self, ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32( ISteamNetworkingUtils* self, ESteamNetworkingConfigValue eValue, int32 val );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat( ISteamNetworkingUtils* self, ESteamNetworkingConfigValue eValue, float val );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString( ISteamNetworkingUtils* self, ESteamNetworkingConfigValue eValue, const char * val );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr( ISteamNetworkingUtils* self, ESteamNetworkingConfigValue eValue, void * val );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32( ISteamNetworkingUtils* self, HSteamNetConnection hConn, ESteamNetworkingConfigValue eValue, int32 val );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat( ISteamNetworkingUtils* self, HSteamNetConnection hConn, ESteamNetworkingConfigValue eValue, float val );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString( ISteamNetworkingUtils* self, HSteamNetConnection hConn, ESteamNetworkingConfigValue eValue, const char * val );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged( ISteamNetworkingUtils* self, FnSteamNetConnectionStatusChanged fnCallback );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged( ISteamNetworkingUtils* self, FnSteamNetAuthenticationStatusChanged fnCallback );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged( ISteamNetworkingUtils* self, FnSteamRelayNetworkStatusChanged fnCallback );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetConfigValue( ISteamNetworkingUtils* self, ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj, ESteamNetworkingConfigDataType eDataType, const void * pArg );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct( ISteamNetworkingUtils* self, const SteamNetworkingConfigValue_t & opt, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj );
STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingGetConfigValueResult SteamAPI_ISteamNetworkingUtils_GetConfigValue( ISteamNetworkingUtils* self, ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj, ESteamNetworkingConfigDataType * pOutDataType, void * pResult, size_t * cbResult );
STEAMNETWORKINGSOCKETS_INTERFACE const char * SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo( ISteamNetworkingUtils* self, ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigDataType * pOutDataType, ESteamNetworkingConfigScope * pOutScope );
STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingConfigValue SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues( ISteamNetworkingUtils* self, ESteamNetworkingConfigValue eCurrent, bool bEnumerateDevVars );

// SteamNetworkingIPAddr
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_Clear( SteamNetworkingIPAddr* self );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros( SteamNetworkingIPAddr* self );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_SetIPv6( SteamNetworkingIPAddr* self, const uint8 * ipv6, uint16 nPort );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_SetIPv4( SteamNetworkingIPAddr* self, uint32 nIP, uint16 nPort );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_IsIPv4( SteamNetworkingIPAddr* self );
STEAMNETWORKINGSOCKETS_INTERFACE uint32 SteamAPI_SteamNetworkingIPAddr_GetIPv4( SteamNetworkingIPAddr* self );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost( SteamNetworkingIPAddr* self, uint16 nPort );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_IsLocalHost( SteamNetworkingIPAddr* self );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_IsEqualTo( SteamNetworkingIPAddr* self, const SteamNetworkingIPAddr & x );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIPAddr_ToString( const SteamNetworkingIPAddr* self, char *buf, size_t cbBuf, bool bWithPort );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIPAddr_ParseString( SteamNetworkingIPAddr* self, const char *pszStr );

// SteamNetworkingIdentity
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_Clear( SteamNetworkingIdentity* self );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_IsInvalid( SteamNetworkingIdentity* self );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_SetSteamID( SteamNetworkingIdentity* self, uint64_steamid steamID );
STEAMNETWORKINGSOCKETS_INTERFACE uint64_steamid SteamAPI_SteamNetworkingIdentity_GetSteamID( SteamNetworkingIdentity* self );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_SetSteamID64( SteamNetworkingIdentity* self, uint64 steamID );
STEAMNETWORKINGSOCKETS_INTERFACE uint64 SteamAPI_SteamNetworkingIdentity_GetSteamID64( SteamNetworkingIdentity* self );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_SetIPAddr( SteamNetworkingIdentity* self, const SteamNetworkingIPAddr & addr );
STEAMNETWORKINGSOCKETS_INTERFACE const SteamNetworkingIPAddr * SteamAPI_SteamNetworkingIdentity_GetIPAddr( SteamNetworkingIdentity* self );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_SetLocalHost( SteamNetworkingIdentity* self );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_IsLocalHost( SteamNetworkingIdentity* self );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_SetGenericString( SteamNetworkingIdentity* self, const char * pszString );
STEAMNETWORKINGSOCKETS_INTERFACE const char * SteamAPI_SteamNetworkingIdentity_GetGenericString( SteamNetworkingIdentity* self );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_SetGenericBytes( SteamNetworkingIdentity* self, const void * data, uint32 cbLen );
STEAMNETWORKINGSOCKETS_INTERFACE const uint8 * SteamAPI_SteamNetworkingIdentity_GetGenericBytes( SteamNetworkingIdentity* self, int & cbLen );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_IsEqualTo( SteamNetworkingIdentity* self, const SteamNetworkingIdentity & x );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingIdentity_ToString( const SteamNetworkingIdentity* self, char *buf, size_t cbBuf );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_SteamNetworkingIdentity_ParseString( SteamNetworkingIdentity* self, size_t sizeofIdentity, const char *pszStr );

// SteamNetworkingMessage_t
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamNetworkingMessage_t_Release( SteamNetworkingMessage_t* self );

// SteamDatagramHostedAddress
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamDatagramHostedAddress_Clear( SteamDatagramHostedAddress* self );
STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingPOPID SteamAPI_SteamDatagramHostedAddress_GetPopID( SteamDatagramHostedAddress* self );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_SteamDatagramHostedAddress_SetDevAddress( SteamDatagramHostedAddress* self, uint32 nIP, uint16 nPort, SteamNetworkingPOPID popid );
#endif

//
// Special flat functions to make it easier to work with custom signaling
//

typedef bool (*FSteamNetworkingSocketsCustomSignaling_SendSignal)( void *ctx, HSteamNetConnection hConn, const SteamNetConnectionInfo_t &info, const void *pMsg, int cbMsg );
typedef void (*FSteamNetworkingSocketsCustomSignaling_Release)( void *ctx );

/// Create an ISteamNetworkingConnectionSignaling object from plain C primitives.
STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingConnectionSignaling *SteamAPI_ISteamNetworkingSockets_CreateCustomSignaling(
	void *ctx, //< pointer to something useful you understand.  Will be passed to your callbacks.
	FSteamNetworkingSocketsCustomSignaling_SendSignal fnSendSignal, //< Callback to send a signal.  See ISteamNetworkingConnectionSignaling::SendSignal
	FSteamNetworkingSocketsCustomSignaling_Release fnRelease //< callback to do any cleanup.  See ISteamNetworkingConnectionSignaling::Release.  You can pass NULL if you don't need to do any cleanup.
);

typedef ISteamNetworkingConnectionSignaling * (*FSteamNetworkingCustomSignalingRecvContext_OnConnectRequest)( void *ctx, HSteamNetConnection hConn, const SteamNetworkingIdentity &identityPeer, int nLocalVirtualPort );
typedef void (*FSteamNetworkingCustomSignalingRecvContext_SendRejectionSignal)( void *ctx, const SteamNetworkingIdentity &identityPeer, const void *pMsg, int cbMsg );

/// Same as SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal, but using plain C primitives.
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal2(
	ISteamNetworkingSockets* self, const void * pMsg, int cbMsg, //< Same as SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal
	void *ctx, //< pointer to something useful you understand.  Will be passed to your callbacks.
	FSteamNetworkingCustomSignalingRecvContext_OnConnectRequest fnOnConnectRequest, //< callback for sending a signal.  Required.  See ISteamNetworkingSignalingRecvContext::OnConnectRequest
	FSteamNetworkingCustomSignalingRecvContext_SendRejectionSignal fnSendRejectionSignal //< callback when we wish to actively reject the connection.  Optional, pass NULL if you don't need this.  See ISteamNetworkingSignalingRecvContext::SendRejectionSignal
);

#endif // STEAMNETWORKINGSOCKETS_FLAT
