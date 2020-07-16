//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Implements a store of CA certificates, e.g. certificates that are not
// assigned to a particular identity.  Also contains functions for checking
// the chain of trust.

#ifndef STEAMNETWORKINGSOCKETS_CERTSTORE_H
#define STEAMNETWORKINGSOCKETS_CERTSTORE_H
#pragma once

#include <ostream>
#include "steamnetworkingsockets_internal.h"

// Use a hardcoded root CA key?  It's just a key, not the cert here, because there are no additional relevant
// details that the cert might specify.  We assume any such cert would be self-signed and not have any
// restrictions, and we also act as if it doesn't expire.
#if !defined( STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY ) && !defined( STEAMNETWORKINGSOCKETS_ALLOW_DYNAMIC_SELFSIGNED_CERTS )

	// Master Valve Key
	#define STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJrsoE4XUc5iaNVpACyh4fobLbwm02tOo6AIOtNygpuE ID18220590129359924542"
#endif

// Internal stuff goes in a private namespace
namespace SteamNetworkingSocketsLib {

/// Certificates are granted limited authority.  A CertAuthParameter is a
/// list items of items of a certain type (AppID, PopID, etc) that are
/// authorized.  The concept of "none" and "all" are also possible to
/// represent.
///
/// Internally, this is represented using a simple sorted array.
/// "All" is represented as a list with a single special "invalid" item.
template <typename T, T kInvalidItem = T(-1)>
struct CertAuthParameter
{
	/// Set the list to authorize nothing.
	inline void SetEmpty() { m_vecItems.clear();  }
	inline bool IsEmpty() const { return m_vecItems.empty(); }

	/// Set the list to be "all items".
	inline void SetAll() { m_vecItems.clear(); m_vecItems.push_back( kInvalidItem );}
	inline bool IsAll() const { return m_vecItems.size() == 1 && m_vecItems[0] == kInvalidItem; }

	/// Return true if the item is in the list.  (Or if we are set to "all".)
	bool HasItem( T x ) const
	{
		Assert( x != kInvalidItem );
		if ( IsAll() )
			return true;
		return std::binary_search( m_vecItems.begin(), m_vecItems.end(), x );
	}

	/// Set this list to be the intersection of the two lists
	void SetIntersection( const CertAuthParameter<T,kInvalidItem> &a, const CertAuthParameter<T,kInvalidItem> &b );
	void Setup( const T *pItems, int n );
	void Print( std::ostream &out, void (*ItemPrint)( std::ostream &out, const T &x ) ) const;

private:

	// Usually very few items here, use a static list; overflow
	// to heap
	vstd::small_vector<T,8> m_vecItems;
};

/// Describe the rights that a cert is authorized to grant,
/// and its expiry.  This is also used to describe the authority
/// granted by a *chain* of certs --- it is the intersection
/// of all the certs on the chain.  (E.g. a cert may claim certain
/// rights, but those assertions are not valid if the signing
/// key does not have rights to grant them).
struct CertAuthScope
{
	CertAuthParameter<SteamNetworkingPOPID> m_pops;
	CertAuthParameter<AppId_t> m_apps;
	time_t m_timeExpiry = 0;

	void SetAll()
	{
		m_pops.SetAll();
		m_apps.SetAll();
		m_timeExpiry = std::numeric_limits<time_t>::max();
	}

	void SetEmpty()
	{
		m_pops.SetEmpty();
		m_apps.SetEmpty();
		m_timeExpiry = 0;
	}

	// Return true if we don't grant authorization to anything
	bool IsEmpty() const
	{
		return m_timeExpiry == 0 || m_apps.IsEmpty() || m_pops.IsEmpty();
	}

	void SetIntersection( const CertAuthScope &a, const CertAuthScope &b )
	{
		m_pops.SetIntersection( a.m_pops, b.m_pops );
		m_apps.SetIntersection( a.m_apps, b.m_apps );
		m_timeExpiry = std::min( a.m_timeExpiry, b.m_timeExpiry );
	}

	void Print( std::ostream &out, const char *pszIndent ) const;
};

/// Nuke all certs and start over from scratch
extern void CertStore_Reset();

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

/// Print all of the certs in the cert storef
extern void CertStore_Print( std::ostream &out );

/// Check the cert store, asserting if any keys are not trusted
extern void CertStore_Check();

/// Steam memory validation
#ifdef DBGFLAG_VALIDATE
extern void CertStore_ValidateStatics( CValidator &validator );
#endif

}

#endif // STEAMNETWORKINGSOCKETS_CERTSTORE_H
