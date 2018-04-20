//====== Copyright Valve Corporation, All rights reserved. ====================

#include <steamnetworkingsockets/steamnetworkingsockets_flat.h>

extern "C" {

STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamNetworkingSockets_CreateListenSocket( ISteamNetworkingSockets *pInterface, int nSteamConnectVirtualPort, uint32 nIP, uint16 nPort )
{
	return pInterface->CreateListenSocket( nSteamConnectVirtualPort, nIP, nPort );
}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamNetworkingSockets_ConnectBySteamID( ISteamNetworkingSockets *pInterface, CSteamID steamIDTarget, int nVirtualPort )
{
	return pInterface->ConnectBySteamID( steamIDTarget, nVirtualPort );
}
#endif

STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamNetworkingSockets_ConnectByIPv4Address( ISteamNetworkingSockets *pInterface, uint32 nIP, uint16 nPort )
{
	return pInterface->ConnectByIPv4Address( nIP, nPort ); 
}

STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamNetworkingSockets_AcceptConnection( HSteamNetConnection hConn )
{
	return SteamNetworkingSockets()->AcceptConnection( hConn );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_CloseConnection( HSteamNetConnection hPeer, int nReason, const char *pszDebug, bool bEnableLinger )
{
	return SteamNetworkingSockets()->CloseConnection( hPeer, nReason, pszDebug, bEnableLinger );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_CloseListenSocket( HSteamListenSocket hSocket, const char *pszNotifyRemoteReason )
{
	return SteamNetworkingSockets()->CloseListenSocket( hSocket, pszNotifyRemoteReason );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData )
{
	return SteamNetworkingSockets()->SetConnectionUserData( hPeer, nUserData );
}

STEAMNETWORKINGSOCKETS_INTERFACE int64 SteamNetworkingSockets_GetConnectionUserData( HSteamNetConnection hPeer )
{
	return SteamNetworkingSockets()->GetConnectionUserData( hPeer );
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetConnectionName( HSteamNetConnection hPeer, const char *pszName )
{
	return SteamNetworkingSockets()->SetConnectionName( hPeer, pszName );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen )
{
	return SteamNetworkingSockets()->GetConnectionName( hPeer, pszName, nMaxLen );
}

STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamNetworkingSockets_SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType )
{
	return SteamNetworkingSockets()->SendMessageToConnection( hConn, pData, cbData, eSendType );
}

STEAMNETWORKINGSOCKETS_INTERFACE EResult SteamNetworkingSockets_FlushMessagesOnConnection( HSteamNetConnection hConn )
{
	return SteamNetworkingSockets()->FlushMessagesOnConnection( hConn );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamNetworkingSockets_ReceiveMessagesOnConnection( HSteamNetConnection hConn, ISteamNetworkingMessage **ppOutMessages, int nMaxMessages )
{
	return SteamNetworkingSockets()->ReceiveMessagesOnConnection( hConn, ppOutMessages, nMaxMessages );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamNetworkingSockets_ReceiveMessagesOnListenSocket( HSteamListenSocket hSocket, ISteamNetworkingMessage **ppOutMessages, int nMaxMessages )
{
	return SteamNetworkingSockets()->ReceiveMessagesOnListenSocket( hSocket, ppOutMessages, nMaxMessages );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo )
{
	return SteamNetworkingSockets()->GetConnectionInfo( hConn, pInfo );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_GetQuickConnectionStatus( HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats )
{
	return SteamNetworkingSockets()->GetQuickConnectionStatus( hConn, pStats );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamNetworkingSockets_GetDetailedConnectionStatus( HSteamNetConnection hConn, char *pszBuf, int cbBuf )
{
	return SteamNetworkingSockets()->GetDetailedConnectionStatus( hConn, pszBuf, cbBuf );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_GetListenSocketInfo( HSteamListenSocket hSocket, uint32 *pnIP, uint16 *pnPort )
{
	return SteamNetworkingSockets()->GetListenSocketInfo( hSocket, pnIP, pnPort );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_CreateSocketPair( ISteamNetworkingSockets *pInterface, HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback )
{
	return pInterface->CreateSocketPair( pOutConnection1, pOutConnection2, bUseNetworkLoopback );
}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_ReceivedRelayAuthTicket( ISteamNetworkingSockets *pInterface, const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket )
{
	return pInterface->ReceivedRelayAuthTicket( pvTicket, cbTicket, pOutParsedTicket );
}

STEAMNETWORKINGSOCKETS_INTERFACE int SteamNetworkingSockets_FindRelayAuthTicketForServer( ISteamNetworkingSockets *pInterface, CSteamID steamID, int nVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket )
{
	return pInterface->FindRelayAuthTicketForServer( steamID, nVirtualPort, pOutParsedTicket );
}

STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamNetworkingSockets_ConnectToHostedDedicatedServer( ISteamNetworkingSockets *pInterface, CSteamID steamIDTarget, int nVirtualPort )
{
	return pInterface->ConnectToHostedDedicatedServer( steamIDTarget, nVirtualPort );
}

STEAMNETWORKINGSOCKETS_INTERFACE uint16 SteamNetworkingSockets_GetHostedDedicatedServerListenPort()
{
	return SteamNetworkingSocketsGameServer()->GetHostedDedicatedServerListenPort();
}

STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamNetworkingSockets_CreateHostedDedicatedServerListenSocket( int nVirtualPort )
{
	return SteamNetworkingSocketsGameServer()->CreateHostedDedicatedServerListenSocket( nVirtualPort );
}

#endif // #ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_GetConnectionDebugText( HSteamNetConnection hConn, char *pOut, int nOutCCH )
{
	return SteamNetworkingSockets()->GetConnectionDebugText( hConn, pOut, nOutCCH );
}

STEAMNETWORKINGSOCKETS_INTERFACE int32 SteamNetworkingSockets_GetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue )
{
	return SteamNetworkingSockets()->GetConfigurationValue( eConfigValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_SetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue, int32 nValue )
{
	return SteamNetworkingSockets()->SetConfigurationValue( eConfigValue, nValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE const char *SteamNetworkingSockets_GetConfigurationValueName( ESteamNetworkingConfigurationValue eConfigValue )
{
	return SteamNetworkingSockets()->GetConfigurationValueName( eConfigValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE int32 SteamNetworkingSockets_GetConfigurationString( ESteamNetworkingConfigurationString eConfigString, char *pDest, int32 destSize )
{
	return SteamNetworkingSockets()->GetConfigurationString( eConfigString, pDest, destSize );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_SetConfigurationString( ESteamNetworkingConfigurationString eConfigString, const char *pString )
{
	return SteamNetworkingSockets()->SetConfigurationString( eConfigString, pString );
}

STEAMNETWORKINGSOCKETS_INTERFACE const char *SteamNetworkingSockets_GetConfigurationStringName( ESteamNetworkingConfigurationString eConfigString )
{
	return SteamNetworkingSockets()->GetConfigurationStringName( eConfigString );
}

STEAMNETWORKINGSOCKETS_INTERFACE int32 SteamNetworkingSockets_GetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue )
{
	return SteamNetworkingSockets()->GetConnectionConfigurationValue( hConn, eConfigValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingSockets_SetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue, int32 nValue )
{
	return SteamNetworkingSockets()->SetConnectionConfigurationValue( hConn, eConfigValue, nValue );
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_RunConnectionStatusChangedCallbacks( ISteamNetworkingSockets *pInterface, FSteamNetConnectionStatusChangedCallback callback, intptr_t context )
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
	pInterface->RunCallbacks( &adapter );
}

}
