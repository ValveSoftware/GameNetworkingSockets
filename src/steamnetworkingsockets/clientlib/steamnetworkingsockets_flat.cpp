//====== Copyright Valve Corporation, All rights reserved. ====================

#include <steamnetworkingsockets/steamnetworkingsockets_flat.h>
#include <steamnetworkingsockets/isteamnetworkingsockets.h>

extern "C" {

STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateListenSocket( intptr_t instancePtr, int nSteamConnectVirtualPort, uint32 nIP, uint16 nPort )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CreateListenSocket( nSteamConnectVirtualPort, nIP, nPort );
}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectBySteamID( intptr_t instancePtr, CSteamID steamIDTarget, int nVirtualPort )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ConnectBySteamID( steamIDTarget, nVirtualPort );
}
#endif

STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectByIPv4Address( intptr_t instancePtr, uint32 nIP, uint16 nPort )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ConnectByIPv4Address( nIP, nPort ); 
}

STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_AcceptConnection( intptr_t instancePtr, HSteamNetConnection hConn )
{
	return ((ISteamNetworkingSockets*)instancePtr)->AcceptConnection( hConn );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CloseConnection( intptr_t instancePtr, HSteamNetConnection hPeer, int nReason, const char *pszDebug, bool bEnableLinger )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CloseConnection( hPeer, nReason, pszDebug, bEnableLinger );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CloseListenSocket( intptr_t instancePtr, HSteamListenSocket hSocket, const char *pszNotifyRemoteReason )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CloseListenSocket( hSocket, pszNotifyRemoteReason );
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

STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamAPI_ISteamNetworkingSockets_SendMessageToConnection( intptr_t instancePtr, HSteamNetConnection hConn, const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType )
{
	return ((ISteamNetworkingSockets*)instancePtr)->SendMessageToConnection( hConn, pData, cbData, eSendType );
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

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetListenSocketInfo( intptr_t instancePtr, HSteamListenSocket hSocket, uint32 *pnIP, uint16 *pnPort )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetListenSocketInfo( hSocket, pnIP, pnPort );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CreateSocketPair( intptr_t instancePtr, HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CreateSocketPair( pOutConnection1, pOutConnection2, bUseNetworkLoopback );
}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket( intptr_t instancePtr, const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ReceivedRelayAuthTicket( pvTicket, cbTicket, pOutParsedTicket );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer( intptr_t instancePtr, CSteamID steamID, int nVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket )
{
	return ((ISteamNetworkingSockets*)instancePtr)->FindRelayAuthTicketForServer( steamID, nVirtualPort, pOutParsedTicket );
}

STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer( intptr_t instancePtr, CSteamID steamIDTarget, int nVirtualPort )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ConnectToHostedDedicatedServer( steamIDTarget, nVirtualPort );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerInfo( intptr_t instancePtr, SteamDatagramServiceNetID *pRouting, SteamNetworkingPOPID *pPopID )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetHostedDedicatedServerInfo( pRouting, pPopID );
}

STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket( intptr_t instancePtr, int nVirtualPort )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CreateHostedDedicatedServerListenSocket( nVirtualPort );
}

#endif // #ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetConnectionDebugText( intptr_t instancePtr, HSteamNetConnection hConn, char *pOut, int nOutCCH )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetConnectionDebugText( hConn, pOut, nOutCCH );
}

STEAMNETWORKINGSOCKETS_INTERFACE int32 SteamAPI_ISteamNetworkingSockets_GetConfigurationValue( intptr_t instancePtr, ESteamNetworkingConfigurationValue eConfigValue )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetConfigurationValue( eConfigValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_SetConfigurationValue( intptr_t instancePtr, ESteamNetworkingConfigurationValue eConfigValue, int32 nValue )
{
	return ((ISteamNetworkingSockets*)instancePtr)->SetConfigurationValue( eConfigValue, nValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE const char *SteamAPI_ISteamNetworkingSockets_GetConfigurationValueName( intptr_t instancePtr, ESteamNetworkingConfigurationValue eConfigValue )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetConfigurationValueName( eConfigValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE int32 SteamAPI_ISteamNetworkingSockets_GetConfigurationString( intptr_t instancePtr, ESteamNetworkingConfigurationString eConfigString, char *pDest, int32 destSize )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetConfigurationString( eConfigString, pDest, destSize );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_SetConfigurationString( intptr_t instancePtr, ESteamNetworkingConfigurationString eConfigString, const char *pString )
{
	return ((ISteamNetworkingSockets*)instancePtr)->SetConfigurationString( eConfigString, pString );
}

STEAMNETWORKINGSOCKETS_INTERFACE const char *SteamAPI_ISteamNetworkingSockets_GetConfigurationStringName( intptr_t instancePtr, ESteamNetworkingConfigurationString eConfigString )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetConfigurationStringName( eConfigString );
}

STEAMNETWORKINGSOCKETS_INTERFACE int32 SteamAPI_ISteamNetworkingSockets_GetConnectionConfigurationValue( intptr_t instancePtr, HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetConnectionConfigurationValue( hConn, eConfigValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_SetConnectionConfigurationValue( intptr_t instancePtr, HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue, int32 nValue )
{
	return ((ISteamNetworkingSockets*)instancePtr)->SetConnectionConfigurationValue( hConn, eConfigValue, nValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamAPI_ISteamNetworkingSockets_RunConnectionStatusChangedCallbacks( intptr_t instancePtr, FSteamNetConnectionStatusChangedCallback callback, intptr_t context )
{
	struct CallbackAdapter : ISteamNetworkingSocketsCallbacks
	{
		CallbackAdapter( FSteamNetConnectionStatusChangedCallback callback, intptr_t context )
		: m_callback( callback ), m_context( context ) {}

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

}
