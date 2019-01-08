//====== Copyright Valve Corporation, All rights reserved. ====================

#include <steamnetworkingsockets/steamnetworkingsockets_flat.h>
#include <steamnetworkingsockets/steamnetworkingsockets.h>

extern "C" {

STEAMNETWORKINGSOCKETS_INTERFACE HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP( intptr_t instancePtr, const SteamNetworkingIPAddr *pAddress )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CreateListenSocketIP( *pAddress );
}

STEAMNETWORKINGSOCKETS_INTERFACE HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress( intptr_t instancePtr, const SteamNetworkingIPAddr *pAddress )
{
	return ((ISteamNetworkingSockets*)instancePtr)->ConnectByIPAddress( *pAddress );
}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
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

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress( intptr_t instancePtr, HSteamListenSocket hSocket, SteamNetworkingIPAddr *pAddress )
{
	return ((ISteamNetworkingSockets*)instancePtr)->GetListenSocketAddress( hSocket, pAddress );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamAPI_ISteamNetworkingSockets_CreateSocketPair( intptr_t instancePtr, HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 )
{
	return ((ISteamNetworkingSockets*)instancePtr)->CreateSocketPair( pOutConnection1, pOutConnection2, bUseNetworkLoopback, pIdentity1, pIdentity2 );
}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

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

}
