//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Plain C interface to SteamNetworkingSockets
//
//=============================================================================

#ifndef STEAMNETWORKINGSOCKETS_FLAT
#define STEAMNETWORKINGSOCKETS_FLAT
#pragma once

#include <stdint.h>
#include "isteamnetworkingsockets.h"

extern "C" {

STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamNetworkingSockets_CreateListenSocket( ISteamNetworkingSockets *pInterface, int nSteamConnectVirtualPort, uint32 nIP, uint16 nPort );

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamNetworkingSockets_ConnectBySteamID( ISteamNetworkingSockets *pInterface, CSteamID steamIDTarget, int nVirtualPort );
#endif

STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamNetworkingSockets_ConnectByIPv4Address( ISteamNetworkingSockets *pInterface, uint32 nIP, uint16 nPort );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamNetworkingSockets_AcceptConnection( HSteamNetConnection hConn );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_CloseConnection( HSteamNetConnection hPeer, int nReason, const char *pszDebug, bool bEnableLinger );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_CloseListenSocket( HSteamListenSocket hSocket, const char *pszNotifyRemoteReason );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData );
STEAMNETWORKINGSOCKETS_INTERFACE int64 SteamNetworkingSockets_GetConnectionUserData( HSteamNetConnection hPeer );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetConnectionName( HSteamNetConnection hPeer, const char *pszName );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamNetworkingSockets_SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType );
STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamNetworkingSockets_FlushMessagesOnConnection( HSteamNetConnection hConn );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamNetworkingSockets_ReceiveMessagesOnConnection( HSteamNetConnection hConn, ISteamNetworkingMessage **ppOutMessages, int nMaxMessages ); 
STEAMNETWORKINGSOCKETS_INTERFACE int SteamNetworkingSockets_ReceiveMessagesOnListenSocket( HSteamListenSocket hSocket, ISteamNetworkingMessage **ppOutMessages, int nMaxMessages ); 
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_GetQuickConnectionStatus( HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamNetworkingSockets_GetDetailedConnectionStatus( HSteamNetConnection hConn, char *pszBuf, int cbBuf );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_GetListenSocketInfo( HSteamListenSocket hSocket, uint32 *pnIP, uint16 *pnPort );

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_CreateSocketPair( ISteamNetworkingSockets *pInterface, HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback );

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_ReceivedRelayAuthTicket( ISteamNetworkingSockets *pInterface, const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket );
STEAMNETWORKINGSOCKETS_INTERFACE int SteamNetworkingSockets_FindRelayAuthTicketForServer( ISteamNetworkingSockets *pInterface, CSteamID steamID, int nVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket );
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamNetworkingSockets_ConnectToHostedDedicatedServer( ISteamNetworkingSockets *pInterface, CSteamID steamIDTarget, int nVirtualPort );
STEAMNETWORKINGSOCKETS_INTERFACE uint16 SteamNetworkingSockets_GetHostedDedicatedServerListenPort();
STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamNetworkingSockets_CreateHostedDedicatedServerListenSocket( int nVirtualPort );

#endif // #ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_GetConnectionDebugText( HSteamNetConnection hConn, char *pOut, int nOutCCH );
STEAMNETWORKINGSOCKETS_INTERFACE int32 SteamNetworkingSockets_GetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_SetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue, int32 nValue );
STEAMNETWORKINGSOCKETS_INTERFACE const char *SteamNetworkingSockets_GetConfigurationValueName( ESteamNetworkingConfigurationValue eConfigValue );
STEAMNETWORKINGSOCKETS_INTERFACE int32 SteamNetworkingSockets_GetConfigurationString( ESteamNetworkingConfigurationString eConfigString, char *pDest, int32 destSize );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_SetConfigurationString( ESteamNetworkingConfigurationString eConfigString, const char *pString );
STEAMNETWORKINGSOCKETS_INTERFACE const char *SteamNetworkingSockets_GetConfigurationStringName( ESteamNetworkingConfigurationString eConfigString );
STEAMNETWORKINGSOCKETS_INTERFACE int32 SteamNetworkingSockets_GetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_SetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue, int32 nValue );

typedef void (*FSteamNetConnectionStatusChangedCallback)( SteamNetConnectionStatusChangedCallback_t *pInfo, intptr_t context );

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_RunConnectionStatusChangedCallbacks( ISteamNetworkingSockets *pInterface, FSteamNetConnectionStatusChangedCallback callback, intptr_t context );

}

#endif // ISTEAMNETWORKINGSOCKETS
