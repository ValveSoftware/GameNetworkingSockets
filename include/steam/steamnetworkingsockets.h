//====== Copyright Valve Corporation, All rights reserved. ====================
//
// API for standalone library.  (Not the opensource code, or Steamworks SDK.)
//
//=============================================================================

#ifndef STEAMNETWORKINGSOCKETS
#define STEAMNETWORKINGSOCKETS
#pragma once

#include <stdarg.h>

#include "isteamnetworkingsockets.h"
struct SteamRelayNetworkStatus_t;

#ifndef STEAMNETWORKINGSOCKETS_STANDALONELIB
	#error "Shouldn't be including this!"
#endif

// Call before initializing the library, to set the AppID and universe.
STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagram_SetAppID( AppId_t nAppID );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagram_SetUniverse( bool bChina = false, EUniverse eUniverse = k_EUniversePublic );

/// Set an environment variable.  This is useful if you cannot set a real environment
/// variable for whatever reason.  If a variable is set, it will take priority over the
/// real environment var.  You MUST call this before calling any Init functions.
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetEnvironmentVariable( const char *pszName, const char *pszVal );

/// Initialize client interface
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamDatagramClient_Init( SteamNetworkingErrMsg &errMsg );

/// Initialize gameserver interface
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamDatagramServer_Init( SteamNetworkingErrMsg &errMsg );

/// Shutdown the client interface
STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagramClient_Kill();

/// Shutdown the game server interface
STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagramServer_Kill();

/// Manual polling mode.  You should call this before initialize the lib.
/// This will prevent the library from opening up its own service thread,
/// allowing you to pump sockets and stuff from your own thread.
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetManualPollMode( bool bFlag );

/// If you call SteamNetworkingSockets_SetManualPollMode, then you need to
/// call this frequently.  Any time spent between calls is essentially
/// guaranteed to delay time-sensitive processing, so whatever you are
/// doing, make it quick.  If you pass a nonzero wait time, then this
/// function will sleep efficiently, waiting for incoming packets,
/// up to the maximum time you specify.  It may return prematurely
/// if packets arrive earlier than your timeout.
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_Poll( int msMaxWaitTime );

/// Get URL to use to download the network config.  Use this *after* calling SteamDatagramClient_Init.
/// Download this file and pass the contents to SteamDatagram_SetNetworkConfig
/// This is normally only needed when running on PC, but without Steam support.
STEAMNETWORKINGSOCKETS_INTERFACE const char *SteamDatagram_GetNetworkConfigURL();

/// Set the network config.  Returns false if there is a problem, such as the
/// data being corrupted.  Note that this will will also fail for dedicated
/// servers, if SDR_POPID is set, but the value is not in the network configuration
/// being supplied.  (This indicates a configuration problem that will prevent
/// connections to the server over SDR.)
/// 
/// You can use this if you are fetching the network configuration directly
/// or your game coordinator is distributing the network config to your
/// clients.  You can call this multiple times.  If the supplied data is
/// newer than any currently installed configuration, then the library will
/// apply the new configuration.  If the supplied data is not newer than any
/// configuration data that has already been installed, then the data supplied
/// data is ignored, and true is returned.
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamDatagram_SetNetworkConfig( const void *pData, size_t cbData, SteamNetworkingErrMsg &errMsg );

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

/// Set a custom handler to be called before formatting is performed.
/// The handler must be non-NULL!  If you use this, don't use
/// ISteamNetworkingUtils::SetDebugOutputFunction
///
/// eDetailLevel - verbosity for most output.  (Some config vals can be used to adjust
///		detail for specific systems.)
/// 
/// Arguments to the callback:
/// - bFmt - if false, then pMsg should be used as-is, and the argument list must be ignored
/// - pstrFile/nLine - MIGHT BE NULL/0!!  (Will only be non-NULL on asserts!)
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetPreFormatDebugOutputHandler(
	ESteamNetworkingSocketsDebugOutputType eDetailLevel,
	void (*pfn_Handler)( ESteamNetworkingSocketsDebugOutputType eType, bool bFmt, const char* pstrFile, int nLine, const char *pMsg, va_list ap )
);

/// The default spew handler function will do the formatting and invoke the callback.
/// Set using ISteamNetworkingUtils::SetDebugOutputFunction.
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_DefaultPreFormatDebugOutputHandler( ESteamNetworkingSocketsDebugOutputType eType, bool bFmt, const char* pstrFile, int nLine, const char *pMsg, va_list ap );

//
// Statistics about the global lock.
//
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockWaitWarningThreshold( SteamNetworkingMicroseconds usecThreshold );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockAcquiredCallback( void (*callback)( const char *tags, SteamNetworkingMicroseconds usecWaited ) );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockHeldCallback( void (*callback)( const char *tags, SteamNetworkingMicroseconds usecWaited ) );

/// Called from the service thread at initialization time.
/// Use this to customize its priority / affinity, etc
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetServiceThreadInitCallback( void (*callback)() );

// Struct used to return a buffer across a compilation boundary where
// different bits of code might not be using the same heap functions
struct SteamNetworkingSocketsBuffer_t
{
	void *m_pvData;
	uint32 m_cbData;
	void (*m_pfnFree)( void *p ); // how to free m_pvData
};

/// Callback used to load credentials from a "durable" cache.
/// pszSuggestedFilenameFragment will be a filename with no extension that
/// is specific to the current identity.  You should apply the correct
/// directory and extension of your choosing, and load up the data,
/// filling out the buffer.  If the load fails, set m_pvData=NULL
typedef void (*FnSteamDatagramClient_CredentialsDurableCacheLoad)( const char *pszSuggestedFilenameFragment, SteamNetworkingSocketsBuffer_t *pBuf );

/// Callback used to save credentials to a "durable" cache.
typedef void (*FnSteamDatagramClient_CredentialsDurableCacheSave)( const char *pszSuggestedFilenameFragment, const void *pvData, uint32 cbData );

/// Set callbacks used to load/save durable credentials.  These will be
/// called whenever our identity changes or we receive credentials.
STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagramClient_SetCredentialsDurableCacheCallbacks(
	FnSteamDatagramClient_CredentialsDurableCacheLoad pfnLoad,
	FnSteamDatagramClient_CredentialsDurableCacheSave pfnSave
);

// Special interface when using the standalone lib and running on Steam.
// Normally when running on Steam you will use the Steamworks SDK, so this
// is only for internal use or when you cannot use Steamworks for some reason.
//
// These functions are defined as macros to make sure no code is emitted
// that references these symbols unless you actually use these functions.
#if !defined(_XBOX_ONE) && !defined( _GAMING_XBOX_XBOXONE ) && !defined( _GAMING_XBOX_SCARLETT ) && !defined( NN_NINTENDO_SDK ) && !defined( __PROSPERO__ ) && !defined( __ORBIS__ )

#include <steam/steam_api.h>
#include <steam/steam_gameserver.h>

//inline bool SteamDatagramClient_InitSteam( SteamNetworkingErrMsg &errMsg );
#define SteamDatagramClient_InitSteam(errMsg) ( \
	SteamDatagram_Internal_SteamAPIKludge( &::SteamAPI_RegisterCallback, &::SteamAPI_UnregisterCallback, &::SteamAPI_RegisterCallResult, &::SteamAPI_UnregisterCallResult ), \
	SteamDatagramClient_InitSteam_InternalV1( errMsg, ::SteamInternal_CreateInterface, ::SteamAPI_GetHSteamUser(), ::SteamAPI_GetHSteamPipe() ) \
)

//inline bool SteamDatagramServer_InitSteam( SteamNetworkingErrMsg &errMsg );
#define SteamDatagramServer_InitSteam(errMsg) ( \
	SteamDatagram_Internal_SteamAPIKludge( &::SteamAPI_RegisterCallback, &::SteamAPI_UnregisterCallback, &::SteamAPI_RegisterCallResult, &::SteamAPI_UnregisterCallResult ), \
	SteamDatagramServer_InitSteam_InternalV1( errMsg, &SteamInternal_CreateInterface, ::SteamGameServer_GetHSteamUser(), ::SteamGameServer_GetHSteamPipe() ) \
)

/////////////////////////////////////////////////////////////////////////////
// Internal gross stuff you can ignore

typedef void * ( S_CALLTYPE *FSteamInternal_CreateInterface )( const char *);
typedef void ( S_CALLTYPE *FSteamAPI_RegisterCallback)( class CCallbackBase *pCallback, int iCallback );
typedef void ( S_CALLTYPE *FSteamAPI_UnregisterCallback)( class CCallbackBase *pCallback );
typedef void ( S_CALLTYPE *FSteamAPI_RegisterCallResult)( class CCallbackBase *pCallback, SteamAPICall_t hAPICall );
typedef void ( S_CALLTYPE *FSteamAPI_UnregisterCallResult)( class CCallbackBase *pCallback, SteamAPICall_t hAPICall );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagram_Internal_SteamAPIKludge( FSteamAPI_RegisterCallback fnRegisterCallback, FSteamAPI_UnregisterCallback fnUnregisterCallback, FSteamAPI_RegisterCallResult fnRegisterCallResult, FSteamAPI_UnregisterCallResult fnUnregisterCallResult );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamDatagramClient_InitSteam_InternalV1( SteamNetworkingErrMsg &errMsg, FSteamInternal_CreateInterface fnCreateInterface, HSteamUser hSteamUser, HSteamPipe hSteamPipe );
STEAMNETWORKINGSOCKETS_INTERFACE bool SteamDatagramServer_InitSteam_InternalV1( SteamNetworkingErrMsg &errMsg, FSteamInternal_CreateInterface fnCreateInterface, HSteamUser hSteamUser, HSteamPipe hSteamPipe );

#endif // #if a bunch of platforms where we know Steam cannot be running

#endif // ISTEAMNETWORKINGSOCKETS
