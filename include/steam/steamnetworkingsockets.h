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
// On failure, false is returned, and a non-localized diagnostic message is returned.
STEAMNETWORKINGSOCKETS_INTERFACE bool GameNetworkingSockets_Init( const SteamNetworkingIdentity *pIdentity, SteamNetworkingErrMsg &errMsg );

// Close all connections and listen sockets and free all resources
STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_Kill();

/// Custom memory allocation methods.  If you call this, you MUST call it exactly once,
/// before calling any other API function.  *Most* allocations will pass through these,
/// especially all allocations that are per-connection.  A few allocations
/// might still go to the default CRT malloc and operator new.
/// To use this, you must compile the library with STEAMNETWORKINGSOCKETS_ENABLE_MEM_OVERRIDE
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetCustomMemoryAllocator(
	void* (*pfn_malloc)( size_t s ),
	void (*pfn_free)( void *p ),
	void* (*pfn_realloc)( void *p, size_t s )
);


//
// Statistics about the global lock.
//
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockWaitWarningThreshold( SteamNetworkingMicroseconds usecThreshold );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockAcquiredCallback( void (*callback)( const char *tags, SteamNetworkingMicroseconds usecWaited ) );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockHeldCallback( void (*callback)( const char *tags, SteamNetworkingMicroseconds usecWaited ) );

/// Called from the service thread at initialization time.
/// Use this to customize its priority / affinity, etc
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetServiceThreadInitCallback( void (*callback)() );

}

#endif // STEAMNETWORKINGSOCKETS_H
