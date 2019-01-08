//====== Copyright Valve Corporation, All rights reserved. ====================
//
// High level control of the GameNetworkingSockets library.
//
//=============================================================================

#ifndef GAMENETWORKINGSOCKETS
#define GAMENETWORKINGSOCKETS
#ifdef _WIN32
#pragma once
#endif

#include "isteamnetworkingsockets.h"

extern "C" {

/// Initialize the library, setting the identity of the default interface.  (The one
/// returned by SteamNetworkingSockets() ).  If you specify null, a default identity
/// of "localhost" is used.  You will be able to make loopback connections, and
/// UDP connections if authentication for UDP is disabled.  (It is by default.)
STEAMNETWORKINGSOCKETS_INTERFACE bool GameNetworkingSockets_Init( const SteamNetworkingIdentity *pIdentity, SteamDatagramErrMsg &errMsg );

/// Shutdown library
STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_Kill();

/// Set debug output hook
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetDebugOutputFunction( /* ESteamNetworkingSocketsDebugOutputType */ int eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc );

}

/// Callback dispatch mechanism.
/// You'll override this guy and hook any callbacks you are interested in,
/// and then use ISteamNetworkingSockets::RunCallbacks.  In Steam code, this is not used.
/// You register for the callbacks you want using the normal SteamWorks callback
/// mechanisms, which are dispatched along with other Steamworks callbacks when you call
/// SteamAPI_RunCallbacks and SteamGameServer_RunCallbacks.
class ISteamNetworkingSocketsCallbacks
{
public:
	inline ISteamNetworkingSocketsCallbacks() {}
	virtual void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t * ) {}
	//virtual void OnP2PSessionRequest( P2PSessionRequest_t * ) {}
	//virtual void OnP2PSessionConnectFail( P2PSessionConnectFail_t * ) {}
protected:
	inline ~ISteamNetworkingSocketsCallbacks() {}
};

#endif // GAMENETWORKINGSOCKETS
