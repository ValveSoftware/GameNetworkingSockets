//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef ICLIENTNETWORKINGSOCKETS_H
#define ICLIENTNETWORKINGSOCKETS_H
#pragma once

#include <steam/isteamnetworkingsockets.h>

/////////////////////////////////////////////////////////////////////////////
//
// IClientNetworkingSockets
//
// In Steam, this is a non-versioned interface used internally.  It only
// implements the latest version of ISteamNetworkingSockets, and we
// define adapters to convert users of old ISteamNetworkingSockets
// versions to be able to talk to this interface.
//
// Outside of Steam, this layer of version is not needed, and
// ISteamNetworkingSockets and IClientNetworkingSockets should
// be equivalent.  This layer shouldn't add any runtime cost in that case.
//
/////////////////////////////////////////////////////////////////////////////

#ifdef STEAMNETWORKINGSOCKETS_STEAM
	#define CLIENTNETWORKINGSOCKETS_OVERRIDE
#else
	#define CLIENTNETWORKINGSOCKETS_OVERRIDE override
#endif


class IClientNetworkingSockets
#ifndef STEAMNETWORKINGSOCKETS_STEAM
: public ISteamNetworkingSockets
#endif
{
public:
	virtual HSteamListenSocket CreateListenSocketIP( const SteamNetworkingIPAddr &localAddress ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual HSteamNetConnection ConnectByIPAddress( const SteamNetworkingIPAddr &address ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
	virtual HSteamListenSocket CreateListenSocketP2P( int nVirtualPort ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual HSteamNetConnection ConnectP2P( const SteamNetworkingIdentity &identityRemote, int nVirtualPort ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
#endif
	virtual EResult AcceptConnection( HSteamNetConnection hConn ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool CloseConnection( HSteamNetConnection hPeer, int nReason, const char *pszDebug, bool bEnableLinger ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool CloseListenSocket( HSteamListenSocket hSocket ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int64 GetConnectionUserData( HSteamNetConnection hPeer ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual void SetConnectionName( HSteamNetConnection hPeer, const char *pszName ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual EResult SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual EResult FlushMessagesOnConnection( HSteamNetConnection hConn ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int ReceiveMessagesOnConnection( HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0; 
	virtual int ReceiveMessagesOnListenSocket( HSteamListenSocket hSocket, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0; 
	virtual bool GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool GetQuickConnectionStatus( HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int GetDetailedConnectionStatus( HSteamNetConnection hConn, char *pszBuf, int cbBuf ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool GetListenSocketAddress( HSteamListenSocket hSocket, SteamNetworkingIPAddr *address ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool GetIdentity( SteamNetworkingIdentity *pIdentity ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
	virtual bool ReceivedRelayAuthTicket( const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int FindRelayAuthTicketForServer( const SteamNetworkingIdentity &identityGameServer, int nVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual HSteamNetConnection ConnectToHostedDedicatedServer( const SteamNetworkingIdentity &identityTarget, int nVirtualPort ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual uint16 GetHostedDedicatedServerPort() CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual SteamNetworkingPOPID GetHostedDedicatedServerPOPID() CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool GetHostedDedicatedServerAddress( SteamDatagramHostedAddress *pRouting ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual HSteamListenSocket CreateHostedDedicatedServerListenSocket( int nVirtualPort ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
#endif // #ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

	virtual bool GetConnectionDebugText( HSteamNetConnection hConn, char *pOut, int nOutCCH ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int32 GetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool SetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue, int32 nValue ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual const char *GetConfigurationValueName( ESteamNetworkingConfigurationValue eConfigValue ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int32 GetConfigurationString( ESteamNetworkingConfigurationString eConfigString, char *pDest, int32 destSize ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool SetConfigurationString( ESteamNetworkingConfigurationString eConfigString, const char *pString ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual const char *GetConfigurationStringName( ESteamNetworkingConfigurationString eConfigString ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int32 GetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool SetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue, int32 nValue ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
};

#endif // ICLIENTNETWORKINGSOCKETS_H
