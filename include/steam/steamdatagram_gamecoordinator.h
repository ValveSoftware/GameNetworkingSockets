//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Utilities that are useful to central/backend/matchmaking servers
// to interface with the Steam datagram relay network
//
//=============================================================================

#ifndef STEAMDATAGRAM_TICKETGEN_H
#define STEAMDATAGRAM_TICKETGEN_H
#ifdef _WIN32
#pragma once
#endif

// Import some common stuff that is useful by both the client
// and the backend ticket-generating authority.
#include "steamdatagram_tickets.h"

#if defined( STEAMDATAGRAM_GAMECOORDINATOR_FOREXPORT )
	#define STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE DLL_EXPORT
#elif defined( STEAMNETWORKINGSOCKETS_STATIC_LINK )
	#define STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE extern "C"
#else
	#define STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE DLL_IMPORT
#endif

///
/// Steam datagram relay ticket and certificate generation
///
/// "Tickets" are used to grant clients access to the relay network, to talk to a
/// dedicated server running in a known data center that is connected to the
/// relay network.
///
/// "Certificates" are used for end-to-end encryption/authentication, and to access the
/// relay network for P2P.  A client only needs one certificate (we set the expiry to about
/// 24 hours), but they will need a new ticket for each dedicated server they connect to.
/// For simplicity, you may choose to just always generate a certificate any time you
/// generate a ticket.  On Steam, certificates are handled automatically and you will
/// not need to deal with them.  At the time of this writing, you need to issue certificates
/// for your non-Steam players.  (In the future, Steam may provide a certificate service,
/// although if you are using this library you may prefer to issue the certificates yourself,
/// to reduce the number of services that must be operational for clients to be able to
/// connect.)
///

/// Structure used to return a blob of data
struct SteamDatagramSignedBlob
{
	int m_sz;
	uint8 m_blob[ 2048 ];

	/// Get as std::string.  (Declared as a template so that we don't have to
	/// include <string>, if you don't use this.)  This is useful for interacting
	/// with google protobuf.
	template <typename T> void GetAsStdString( T *str ) const
	{
		str->assign( this->m_blob, this->m_sz );
	}
};

/// Initialize the game coordinator library.
///
/// bInsecureDevMode
///		This MUST be false when handling production traffic.  However, when
///		handling test traffic, it can be used to ignore almost all authentication
///		problems.  Any ignored errors will generate debug output warnings.
///
/// fnDebugOutput
///		Set a function to be called when the library produces diagnostics.
///		Most APIs return error messages directly and will not use this mechanism.
///		However, it is still highly recommended to install a handler and log all output.
///		The output will be sparse, you should not have to filter it.  Any output with a
///		type <= k_ESteamNetworkingSocketsDebugOutputType_Error is probably an indication
///		of a relatively serious problem and worth generating an alert and ringing a pager.
STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE void SteamDatagram_GameCoordinator_Init( bool bInsecureDevMode, FSteamNetworkingSocketsDebugOutput fnDebugOutput );

/// Return the URL to use to fetch the network config.  This URL will point to
/// a WebAPI endpoint that is configured for maximum availability.  (For example,
/// it will continue to function even during Steam server maintenance.)  However,
/// if you fail to fetch the data for whatever reason, using stale data from a
/// previously successful download is OK.
///
/// If you have more than one AppID, that's usually OK, as the relevant configuration
/// information will be the same for every app.  Please get in touch with Valve
/// if you have multiple apps and they need to use different keys or have a different
/// set of SDR relays.
///
/// Returns a pointer to a static (not-thread-local) buffer.
STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE const char *SteamDatagram_GameCoordinator_GetNetworkConfigURL( AppId_t nAppID );

/// Set the network config.  This has public keys and revocation lists.
/// You MUST call this if you wish to generate tickets or process
/// hosted server logins.
///
/// The data you pass will be a JSON blob that you should download from
/// the URL returned by SteamDatagram_GameCoordinator_GetNetworkConfigURL.
///
/// You should refresh the network configuration periodically, because keys
/// get rotated and certificates renewed.  Once every 24 hours is probably
/// sufficient in practice, but since this operation is relatively cheap
/// and no more engineering effort is required to refresh it more frequently,
/// an interval of 1 hour is recommended.  Remember that on the first attempt,
/// if you fail to fetch the data for any reason, using data from a previous
/// fetch is OK.  (If you fail on subsequent attempts, just ignoring the failure
/// is OK.)
STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE bool SteamDatagram_GameCoordinator_SetNetworkConfig( const void *pData, size_t cbData, SteamNetworkingErrMsg &errMsg );

/// Set the private key for your app, used to sign tickets or certs,
/// with an Ed25519 private key.  See: https://ed25519.cr.yp.to/
///
/// NOTE: The input buffer will be securely wiped to reduce the number
/// of copies of sensitive key material in memory.
///
/// You can generate an Ed25519 key using OpenSSH: ssh-keygen -t ed25519
/// Or with our cert tool: steamnetworkingsockets_certtool gen_keypair
///
/// The private key should be a PEM-like block of text
/// ("-----BEGIN OPENSSH PRIVATE KEY-----").
///
/// Private keys encrypted with a password are not supported.
///
/// In order for signatures using this key to be accepted by the relay network,
/// you need to send your public key to Valve.  This key should be on a single line
/// of text that begins with "ssh-ed25519".  (The format used in the .ssh/authorized_keys
/// file.)
///
/// It is highly recommended to call SteamDatagram_GameCoordinator_SetNetworkConfig before calling this,
/// so that the function can check your key against the live network config and see if it has
/// any problems (has been revoked, or is about to expire).  Any such problems will be generate
/// errors (probably urgent) or warnings (important but maybe not urgent) to the debug output
/// function, and you should pay attention to them.  Any such issues will *not* cause this
/// function to fail.
STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE bool SteamDatagram_SetPrivateKey_Ed25519( void *pvPrivateKey, size_t cbPrivateKey, SteamNetworkingErrMsg &errMsg );

/// Serialize the specified auth ticket and attach a signature.
/// Returns false if you did something stupid like forgot to load a key.
/// Will also fail if your ticket is too big.  (Probably because you
/// added too many extra fields.)
///
/// The resulting blob should be sent to the client, who will put it in
/// their ticket cache using ISteamNetworkingSockets::ReceivedRelayAuthTicket
///
/// Before using this, you must:
/// - Set the private key using SteamDatagram_SetPrivateKey_Ed25519.
/// - Set the network config using SteamDatagram_GameCoordinator_SetNetworkConfig.
STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE bool SteamDatagram_SerializeAndSignTicket( const SteamDatagramRelayAuthTicket &ticket, SteamDatagramSignedBlob &outBlob, SteamNetworkingErrMsg &errMsg );

/// Generate a cert for a user for your app and sign it using your private key.  This
/// is used on non-Steam platforms when *you* have authenticated a user (checking their
/// platform-specific authentication token).  It is not used on Steam, since Steam users
/// will obtain a cert through Steam.
///
/// You MUST only issue certificates to players that you have actually authenticated in
/// some way!  Do not write a generic service that just issues certs to anybody who asks.
/// If you write a bug or have a security hole, and your key is used to issue tickets or certs
/// inappropriately, we might need to revoke it to prevent disruption to other games.
///
/// Before using this, you must:
/// - Set the private key using SteamDatagram_SetPrivateKey_Ed25519.
STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE bool SteamDatagram_CreateCert(
	const SteamNetworkingIdentity &identity,
	uint32 nAppID,
	const void *pRequest, size_t cbRequest, // From ISteamNetworkingSockets::GetCertificateRequest
	SteamDatagramSignedBlob &outBlob,
	SteamNetworkingErrMsg &errMsg );


//
// Gameserver authentication with backend
//

/// Crack login blob and check signature.
///
/// **IMPORTANT**:
///
/// bAllowInsecureLoginToDevPOP
///		If true, then insecure logins are allowed to the "dev" PoP ID. (See
///		k_SteamDatagramPOPID_dev).  Logins claiming to be from any other PoP will require
///		a certificate and valid signatures.  In production, you MUST either use
///		bAllowInsecureLoginToDevPOP=false, or you must check the PoPID, and treat dev logins
///		as insecure!
/// 
///		Note that if you enabled insecure dev mode globally (see SteamDatagram_GameCoordinator_Init),
///		then almost all security errors are ignored and just generate a warning diagnostic.
///		This flag is used to carve out a narrow exception in production for certain servers
///		that you will authenticate through some other means.
///
/// You must call SteamDatagram_GameCoordinator_SetNetworkConfig before using this, so that the revocation
/// list can be checked.
STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE bool SteamDatagram_ParseHostedServerLogin( const void *pvBlob, size_t cbBlob, SteamDatagramGameCoordinatorServerLogin &outLogin, bool bAllowInsecureLoginToDevPOP, SteamNetworkingErrMsg &errMsg );

//
// Some ping-related tools that don't have anything to do with tickets.
// But it's something that a backend might find useful, so we're putting it in this library for now.
//

/// Parse location string.  Returns true on success
STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE bool SteamDatagram_ParsePingLocation( const char *pszString, SteamNetworkPingLocation_t &outLocation );

/// Estimate ping time between two locations.  Returns estimated RTT in ms,
/// or -1 if we couldn't make an estimate.
STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE int SteamDatagram_EstimatePingBetweenTwoLocations( const SteamNetworkPingLocation_t &location1, const SteamNetworkPingLocation_t &location2 );

/// You won't need this unless you work at Valve
STEAMDATAGRAM_GAMECOORDINATOR_INTERFACE void SteamDatagram_GameCoordinator_SetUniverse( EUniverse eUniverse );

#endif // STEAMDATAGRAM_TICKETGEN_H
