//====== Copyright Valve Corporation, All rights reserved. ====================
//
// High level interface to GameNetworkingSockets library.
//
//=============================================================================

#ifndef STEAMNETWORKINGSOCKETS_H
#define STEAMNETWORKINGSOCKETS_H
#ifdef _WIN32
#pragma once
#endif

#include "isteamnetworkingsockets.h"

extern "C" {

// Initialize the library.  Optionally, you can set an initial identity for the default
// interface that is returned by SteamNetworkingSockets().
//
// On failure, false is returned, and a non-localized diagtnostic message is returned.
STEAMNETWORKINGSOCKETS_INTERFACE bool GameNetworkingSockets_Init( const SteamNetworkingIdentity *pIdentity, SteamNetworkingErrMsg &errMsg );

// Close all connections and listen sockets and free all resources
STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_Kill();

//
// Statistics about the global lock.
//
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockWaitWarningThreshold( SteamNetworkingMicroseconds usecThreshold );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockAcquiredCallback( void (*callback)( SteamNetworkingMicroseconds usecWaited ) );

}

/// Callback dispatch mechanism.  Override this and then use
/// ISteamNetworkingSockets::RunCallbacks.
class ISteamNetworkingSocketsCallbacks
{
public:
	inline ISteamNetworkingSocketsCallbacks() {}
	virtual void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t * ) = 0;
protected:
	inline ~ISteamNetworkingSocketsCallbacks() {}
};

#endif // STEAMNETWORKINGSOCKETS_H
