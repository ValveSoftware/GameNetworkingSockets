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

/// Add a certificate to the trust store used to verify peer certificates.
/// Accepts a PEM-like blob ("STEAMDATAGRAM CERT") as emitted by the certificate
/// tool, or just the raw base64 body of one.  Call it once per certificate:
/// install your root CA cert, plus any intermediate certs needed to complete
/// the chain to certs that peers will present.  (Certs on the wire only carry
/// the ID of the CA key that signed them, not the chain itself, so the
/// verifying side must have the whole chain installed.)  The trust store is
/// global to the process and shared by all interfaces.  A self-signed cert
/// installed here becomes a trusted root, in addition to the hardcoded root
/// CA key, which remains trusted.  Do not install certs bound to a particular
/// identity (e.g. a server's own cert) -- only CA certs go in the trust store.
///
/// Note that installing certs does not, by itself, *require* peers to
/// authenticate.  To reject peers without a valid certificate, set
/// k_ESteamNetworkingConfig_IP_AllowWithoutAuth to 0.  Also, a certificate
/// proves that the peer's identity was certified by a trusted CA, but for
/// connections initiated by IP address the library has no expected identity
/// to compare against -- check SteamNetConnectionInfo_t::m_identityRemote
/// after connecting if you need to know *which* certified peer you are
/// talking to.
STEAMNETWORKINGSOCKETS_INTERFACE bool GameNetworkingSockets_AddTrustedCert( const char *pszCert, SteamNetworkingErrMsg &errMsg );

/// Set the App ID used for certificate validation.  Certificates are
/// authorized for particular App IDs, and both ends check certs against
/// their own App ID.  If you use certificate authentication, call this
/// before SetCertificate, on clients as well as servers.
STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_SetAppID( AppId_t nAppID );

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
