//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Implements a store of CA certificates, e.g. certificates that are not
// assigned to a particular identity.  Also contains functions for checking
// the chain of trust.

#ifndef STEAMNETWORKINGSOCKETS_CERTSTORE_H
#define STEAMNETWORKINGSOCKETS_CERTSTORE_H
#pragma once

#include "steamnetworkingsockets_internal.h"


// Internal stuff goes in a private namespace
namespace SteamNetworkingSocketsLib {

struct CertAuthScope;

/// Add a cert to the store from a base-64 blob (the body of the PEM-like blob).  Returns false
/// only if there was a parse error.  DOES NOT check for expiry or validate any signatures, etc.
extern bool CertStore_AddCertFromBase64( const char *pszBase64, SteamNetworkingErrMsg &errMsg );

/// Adds a key revocation entry.
extern void CertStore_AddKeyRevocation( uint64 key_id );

/// Given a signed blob of data, locate the CA public key, make sure it is trusted,
/// and verify the signature.  Also checks that none of the certs in the chain
/// have expired at the current time (which you must supply).
///
/// If the signed data should be trusted, the scope of the authorization granted
/// to that public key is returned.  Otherwise, NULL is returned and errMsg is
/// populated.
extern const CertAuthScope *CertStore_CheckCASignature( const std::string &signed_data, uint64 nCAKeyID, const std::string &signature, time_t timeNow, SteamNetworkingErrMsg &errMsg );

/// Check a CA signature and chain of trust for a signed cert.
/// Also deserializes the cert and make sure it is not expired.
/// DOES NOT CHECK that the appid, pops, etc in the cert
/// are granted by the CA chain.  You need to do that!
extern const CertAuthScope *CertStore_CheckCert( const CMsgSteamDatagramCertificateSigned &msgCertSigned, CMsgSteamDatagramCertificate &outMsgCert, time_t timeNow, SteamNetworkingErrMsg &errMsg );

/// Check if a cert gives permission to access a certain app.
extern bool CheckCertAppID( const CMsgSteamDatagramCertificate &msgCert, const CertAuthScope *pCertAuthScope, AppId_t nAppID, SteamNetworkingErrMsg &errMsg );

/// Check if a cert gives permission to access a certain PoPID.
extern bool CheckCertPOPID( const CMsgSteamDatagramCertificate &msgCert, const CertAuthScope *pCertAuthScope, SteamNetworkingPOPID popID, SteamNetworkingErrMsg &errMsg );

/// Steam memory validation
#ifdef DBGFLAG_VALIDATE
extern void CertStore_ValidateStatics( CValidator &validator );
#endif

}

#endif // STEAMNETWORKINGSOCKETS_CERTSTORE_H
