//====== Copyright Valve Corporation, All rights reserved. ====================

// Ug, I didn't know ostream used exceptions.  Isn't there a decent
// stream implementation that won't bring in 10000000000 dependencies?
#ifdef _MSC_VER
#pragma warning( disable: 4530 )
#endif

#include <time.h>
#include <ostream>
#include <memory>
#include <crypto.h>
#include <crypto_25519.h>
#include "steamnetworkingsockets_certstore.h"
#include <tier1/utlhashmap.h>

#ifdef DBGFLAG_VALIDATE
	#include <tier0/validator_std.h>
#endif

// Must be the last include
#include <tier0/memdbgon.h>

#define MsgOrOK( s ) ( (s).empty() ? "OK" : (s).c_str() )

namespace SteamNetworkingSocketsLib {

template <typename T, T kInvalidItem >
void CertAuthParameter<T,kInvalidItem>::SetIntersection( const CertAuthParameter<T,kInvalidItem> &a, const CertAuthParameter<T,kInvalidItem> &b )
{
	if ( a.IsAll() )
	{
		m_vecItems = b.m_vecItems;
		return;
	}
	if ( b.IsAll() )
	{
		m_vecItems = a.m_vecItems;
		return;
	}
	m_vecItems.clear();
	m_vecItems.reserve( std::min( a.m_vecItems.size(), b.m_vecItems.size() ) );

	#define INC(x) ++it##x; if ( it##x == x.m_vecItems.end() ) break; Assert( *it##x > v##x );

	// Scan lists in parallel, taking advantage of the fact that they should be sorted
	auto ita = a.m_vecItems.begin();
	auto itb = b.m_vecItems.begin();
	for (;;)
	{
		const T &va = *ita;
		const T &vb = *itb;
		if ( va < vb )
		{
			INC(a)
		}
		else if ( vb < va )
		{
			INC(b)
		}
		else
		{
			m_vecItems.push_back( va );
			INC(a)
			INC(b)
		}
	}

	#undef INC
}

template <typename T, T kInvalidItem >
void CertAuthParameter<T,kInvalidItem>::Setup( const T *pItems, int n )
{
	m_vecItems.assign( pItems, pItems+n);

	// Sort, so that intersection can be computed efficiently
	std::sort( m_vecItems.begin(), m_vecItems.end() );

	// Remove duplicates.  We assume both that duplicates are rare
	// and lists are small, so that O(n^2) is OK here. 
	for ( int i = len(m_vecItems)-1 ; i > 1 ; --i )
	{
		if ( m_vecItems[i-1] == m_vecItems[i] )
			erase_at( m_vecItems, i );
	}
}

template <typename T, T kInvalidItem >
void CertAuthParameter<T,kInvalidItem>::Print( std::ostream &out, void (*ItemPrint)( std::ostream &out, const T &x ) ) const
{
	if ( IsEmpty() )
	{
		out << "(none)";
	}
	else if ( IsAll() )
	{
		out << "(any)";
	}
	else
	{
		for ( int i = 0 ; i < len( m_vecItems ) ; ++i )
		{
			if ( i > 0 )
				out << ',';
			(*ItemPrint)( out, m_vecItems[i] );
		}
	}
}

void CertAuthScope::Print( std::ostream &out, const char *pszIndent ) const
{
	auto PrintAppID = []( std::ostream &o, const AppId_t &x )
	{
		o << x;
	};
	auto PrintPOPID = []( std::ostream &o, const SteamNetworkingPOPID &x )
	{
		o << SteamNetworkingPOPIDRender( x ).c_str();
	};

	out << pszIndent << "AppIDs . . : "; m_apps.Print( out, PrintAppID ); out << std::endl;
	out << pszIndent << "POPs . . . : "; m_pops.Print( out, PrintPOPID ); out << std::endl;
	out << pszIndent << "Expires. . : " << ctime( &m_timeExpiry );
}

enum ETrust
{
	k_ETrust_Revoked = -3,
	k_ETrust_NotTrusted = -2,
	k_ETrust_UnknownWorking = -1,
	k_ETrust_Unknown = 0,
	k_ETrust_Trusted = 1,
	k_ETrust_Hardcoded = 2,
};

// List of certs presented for this public key.
// We only actually ever use one, and it's in the first slot.
// But on some occasions we may have more than one cert for
// a key (e.g. key rotation)
struct Cert
{
	ETrust m_eTrust = k_ETrust_Unknown;
	std::string m_status_msg; // If it's not trusted, why?
	std::string m_signed_data;
	uint64 m_ca_key_id = 0;
	std::string m_signature;
	CertAuthScope m_authScope;
	time_t m_timeCreated;

	bool Setup( const CMsgSteamDatagramCertificateSigned &msgCertSigned, CECSigningPublicKey &outPublicKey, SteamNetworkingErrMsg &errMsg )
	{
		m_signed_data = msgCertSigned.cert();
		m_signature = msgCertSigned.ca_signature();
		m_ca_key_id = msgCertSigned.ca_key_id();

		if ( m_signed_data.empty() )
		{
			V_strcpy_safe( errMsg, "No data" );
			return false;
		}
		if ( m_signature.length() != sizeof(CryptoSignature_t) )
		{
			V_strcpy_safe( errMsg, "Invalid signature length" );
			return false;
		}

		CMsgSteamDatagramCertificate msgCert;
		if ( !msgCert.ParseFromString( m_signed_data ) )
		{
			V_strcpy_safe( errMsg, "Cert failed protobuf parse" );
			return false;
		}

		// We don't store certs bound to a particular identity in the cert store
		if ( msgCert.has_identity_string() || msgCert.has_legacy_identity_binary() || msgCert.has_legacy_steam_id() )
		{
			V_strcpy_safe( errMsg, "Cert is bound to particular identity; doesn't go in the cert store" );
			return false;
		}

		if ( msgCert.key_type() != CMsgSteamDatagramCertificate_EKeyType_ED25519 )
		{
			V_strcpy_safe( errMsg, "Only ED25519 public key supported" );
			return false;
		}
		if ( !outPublicKey.SetRawDataWithoutWipingInput( msgCert.key_data().c_str(), msgCert.key_data().length() ) )
		{
			V_strcpy_safe( errMsg, "Invalid public key" );
			return false;
		}

		m_timeCreated = msgCert.time_created();
		m_authScope.m_timeExpiry = msgCert.time_expiry();
		if ( m_authScope.m_timeExpiry <= 0 )
		{
			V_strcpy_safe( errMsg, "Cert has no expiry" );
			return false;
		}

		if ( msgCert.gameserver_datacenter_ids_size() > 0 )
		{
			m_authScope.m_pops.Setup( msgCert.gameserver_datacenter_ids().data(), msgCert.gameserver_datacenter_ids_size() );
		}
		else
		{
			m_authScope.m_pops.SetAll();
		}

		if ( msgCert.app_ids_size() > 0 )
		{
			m_authScope.m_apps.Setup( msgCert.app_ids().data(), msgCert.app_ids_size() );
		}
		else
		{
			m_authScope.m_apps.SetAll();
		}

		return true;
	}

	void Print( std::ostream &out, const char *pszIndent ) const
	{
		out << pszIndent << "Cert signed by CA " << (unsigned long long)m_ca_key_id << "  " << MsgOrOK(m_status_msg) << std::endl;
		out << pszIndent << "Created " << ctime( &m_timeCreated );
		m_authScope.Print( out, ( std::string( pszIndent ) + "  " ).c_str() );
	}

};

struct PublicKey
{
	ETrust m_eTrust = k_ETrust_Unknown;
	CECSigningPublicKey m_keyPublic;
	std::string m_status_msg; // If it's not trusted, why?
	std_vector<Cert> m_vecCerts;
	CertAuthScope m_effectiveAuthScope;
	int m_idxNewestValidCert = -1;

	uint64 CalculateKeyID() const { Assert( m_keyPublic.IsValid() ); return CalculatePublicKeyID( m_keyPublic ); }

	inline bool IsTrusted() const
	{
		if ( m_eTrust >= k_ETrust_Trusted )
			return true;
		Assert( m_eTrust <= k_ETrust_NotTrusted );
		Assert( !m_status_msg.empty() ); // We should nkow the reason for any key we don't trust
		return false;
	}

	#ifdef STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY
		void SlamHardcodedRootCA()
		{
			bool bOK = m_keyPublic.SetFromOpenSSHAuthorizedKeys( STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY, sizeof(STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY) );
			Assert( bOK );
			m_eTrust = k_ETrust_Hardcoded;
			m_effectiveAuthScope.SetAll();
		}
	#endif

	#ifdef DBGFLAG_VALIDATE
		void Validate( CValidator &validator, const char *pchName ) const
		{
			ValidateRecursive( m_keyPublic );
			ValidateRecursive( m_status_msg );
			ValidateRecursive( m_vecCerts );
			ValidateRecursive( m_effectiveAuthScope );
		}
	#endif
};
static CUtlHashMap<uint64,std::unique_ptr<PublicKey>,std::equal_to<uint64>,std::hash<uint64> > s_mapPublicKeys;
static bool s_bTrustValid = false;

static PublicKey *FindPublicKey( uint64 nKeyID )
{
	int idx = s_mapPublicKeys.Find( nKeyID );
	if ( idx == s_mapPublicKeys.InvalidIndex() )
		return nullptr;
	return s_mapPublicKeys[ idx ].get();
}

static void CertStore_OneTimeInit()
{
	#ifdef STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY
		if ( s_mapPublicKeys.Count() == 0 )
		{
			PublicKey *pKey = new PublicKey;
			pKey->SlamHardcodedRootCA();
			uint64 nKeyID = pKey->CalculateKeyID();

			// Make sure calculated ID matches what we expect!
			char checkID[64];
			V_sprintf_safe( checkID, "ID%llu", (unsigned long long)nKeyID );
			AssertFatal( V_stristr( STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY, checkID ) != NULL );

			s_mapPublicKeys.Insert( nKeyID, std::unique_ptr<PublicKey>( pKey ) );
		}
	#endif
}

void CertStore_Reset()
{
	s_mapPublicKeys.RemoveAll();
	s_bTrustValid = false;
}

void CertStore_AddKeyRevocation( uint64 key_id )
{
	PublicKey *pKey = FindPublicKey( key_id );
	if ( !pKey )
	{
		pKey = new PublicKey;
		pKey->m_eTrust = k_ETrust_Revoked;
		pKey->m_status_msg = "Revoked";
		s_mapPublicKeys.Insert( key_id, std::unique_ptr<PublicKey>( pKey ) );
		s_bTrustValid = false;
		return;
	}

	if ( pKey->m_eTrust == k_ETrust_Revoked )
		return;

	// What should we do if our hardcoded key ever shows up in a revocation list?
	// Probably just totally make all connections unable to connect, and force
	// people to update their software.  In reality it's probably a bad idea
	// for us to ever explicitly "revoke" root keys.  We should just remove them
	// from the dynamic list we are serving.
	AssertMsg( pKey->m_eTrust != k_ETrust_Hardcoded, "WARNING: Hardcoded trust key is in revocation list.  We won't be able to trust anything, ever!" );
	pKey->m_eTrust = k_ETrust_Revoked;
	pKey->m_status_msg = "Revoked";

	s_bTrustValid = false;
}

bool CertStore_AddCertFromBase64( const char *pszBase64, SteamNetworkingErrMsg &errMsg )
{
	CertStore_OneTimeInit();

	// Decode
	CMsgSteamDatagramCertificateSigned msgSignedCert;
	if ( !ParseCertFromBase64( pszBase64, V_strlen( pszBase64 ), msgSignedCert, errMsg ) )
		return false;

	CECSigningPublicKey publicKey;

	// Parse the basic properties of the cert without doing any auth checks
	Cert cert;
	if ( !cert.Setup( msgSignedCert, publicKey, errMsg ) )
		return false;

	uint64 key_id = CalculatePublicKeyID( publicKey );
	PublicKey *pKey = FindPublicKey( key_id );
	if ( pKey )
	{
		if ( pKey->m_keyPublic != publicKey )
		{
			if ( pKey->m_keyPublic.IsValid() )
			{
				V_sprintf_safe( errMsg, "Key collision on key ID %lld!?  Almost certainly a bug.", (unsigned long long)key_id );
				AssertMsg1( false, "%s", errMsg );
				return false;
			}

			// No key data, it was probably revoked.  We can continue on
			Assert( pKey->m_eTrust == k_ETrust_Revoked );
			pKey->m_keyPublic.CopyFrom( publicKey );
		}

		// Check if we already have this exact cert,
		// using the signature as as hash/fingerprint.
		for ( const Cert &c: pKey->m_vecCerts )
		{
			if ( cert.m_signature == c.m_signature )
			{
				Assert( cert.m_signed_data == c.m_signed_data );
				Assert( cert.m_ca_key_id == c.m_ca_key_id );
				Assert( cert.m_timeCreated == c.m_timeCreated );
				return true;
			}
		}
	}
	else
	{
		pKey = new PublicKey;
		pKey->m_keyPublic.CopyFrom( publicKey );
		s_mapPublicKeys.Insert( key_id, std::unique_ptr<PublicKey>( pKey ) );
	}

	// Add the cert
	pKey->m_vecCerts.emplace_back( std::move( cert ) );

	// Invalidate trust, recompute it next time we ask for it
	s_bTrustValid = false;

	// OK
	return true;
}

template< int kMaxSize = 1024  >
std::string V_sprintf_stdstring( const char *pszFmt, ... )
{
	char temp[kMaxSize];
	va_list ap;
	va_start( ap, pszFmt );
	V_vsprintf_safe( temp, pszFmt, ap );
	va_end( ap );
	return std::string( temp );
}

static void RecursiveEvaluateKeyTrust( PublicKey *pKey )
{

	// Make sure we didn't already make a definitive determination
	if ( pKey->m_eTrust != k_ETrust_Unknown )
	{
		Assert( pKey->m_eTrust != k_ETrust_UnknownWorking );
		return;
	}

	// Mark key as "working on it" so we can detect loops
	pKey->m_eTrust = k_ETrust_UnknownWorking;
	pKey->m_idxNewestValidCert = -1;

	// No certs?  How did we get here?
	if ( pKey->m_vecCerts.empty() )
	{
		Assert( false );
		pKey->m_eTrust = k_ETrust_NotTrusted;
		pKey->m_status_msg = "No certs?";
		return;
	}

	// Scan all certs, looking for the newest one that is valid
	for ( int i = 0 ; i < len( pKey->m_vecCerts ) ; ++i )
	{
		Cert &cert = pKey->m_vecCerts[ i ];
		Assert( !cert.m_signed_data.empty() );
		Assert( cert.m_signature.length() == sizeof(CryptoSignature_t) );

		// Cert with empty auth scope shouldn't parse
		Assert( !cert.m_authScope.IsEmpty() );

		// Assume failure
		cert.m_eTrust = k_ETrust_NotTrusted;
		cert.m_status_msg.clear();

		// Locate the public key that they are claiming signed this
		PublicKey *pSignerKey = FindPublicKey( cert.m_ca_key_id );
		if ( pSignerKey == nullptr )
		{
			cert.m_status_msg = V_sprintf_stdstring( "CA key %llu is not known", (unsigned long long)cert.m_ca_key_id );
			continue;
		}

		// Self-signed (root cert)?
		if ( pSignerKey == pKey )
		{
			#ifdef STEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY
				// If hardcoded root cert is in use, only trust the
				// one hardcoded root key.  (We've already tagged it
				// as trusted by hardcoded, so we don't get this far
				// for those keys).
				cert.m_status_msg = "Trusted root is hardcoded, cannot add more self-signed certs";
				continue;
			#else
				// Self signed is OK.
				cert.m_status_msg = "(Self-signed root)";
			#endif
		}
		else
		{

			// Recursively check that the other key is trusted.
			// Protect against a cycle
			if ( pSignerKey->m_eTrust == k_ETrust_UnknownWorking )
			{
				cert.m_status_msg = V_sprintf_stdstring( "Cycle detected in trust chain!  (Cert for key %llu, signed by CA key %llu)", pKey->CalculateKeyID(), (unsigned long long)cert.m_ca_key_id );
				continue;
			}

			RecursiveEvaluateKeyTrust( pSignerKey );
			Assert( pSignerKey->m_eTrust != k_ETrust_UnknownWorking ); // Should have made a determination!

			// Not trusted?
			if ( !pSignerKey->IsTrusted() )
			{
				cert.m_status_msg = V_sprintf_stdstring( "CA key %llu not trusted.  ", (unsigned long long)cert.m_ca_key_id ) + pSignerKey->m_status_msg.c_str();
				continue;
			}

			cert.m_status_msg.clear();
		}

		// If we get here, we trust the signing CA's public key
		// Check signature.  For self-signed certs this is just
		// basically busywork, but it's a nice double-check.)
		if ( !pSignerKey->m_keyPublic.VerifySignature( cert.m_signed_data.c_str(), cert.m_signed_data.length(), *(const CryptoSignature_t *)cert.m_signature.c_str() ) )
		{
			cert.m_status_msg = V_sprintf_stdstring( "Failed signature verification (against CA key %llu)", (unsigned long long)cert.m_ca_key_id );
			continue;
		}

		// Calculate effective auth scope, make sure it isn't empty
		CertAuthScope authScope;
		if ( pSignerKey == pKey )
		{
			authScope = cert.m_authScope;
		}
		else
		{
			authScope.SetIntersection( pSignerKey->m_effectiveAuthScope, cert.m_authScope );
		}

		if ( authScope.m_apps.IsEmpty() )
		{
			cert.m_status_msg = V_sprintf_stdstring( "All apps excluded by auth chain!" );
			continue;
		}
		if ( authScope.m_pops.IsEmpty() )
		{
			cert.m_status_msg = V_sprintf_stdstring( "All pops excluded by auth chain!" );
			continue;
		}
		Assert( authScope.m_timeExpiry > 0 );

		// OK, we're trusted.  Is this the best cert so far?
		if ( pKey->m_idxNewestValidCert < 0 || pKey->m_vecCerts[ pKey->m_idxNewestValidCert ].m_timeCreated < cert.m_timeCreated )
		{
			if ( pSignerKey == pKey )
			{
				pKey->m_effectiveAuthScope = cert.m_authScope;
			}
			else
			{
				pKey->m_effectiveAuthScope = std::move( authScope );
			}
			pKey->m_idxNewestValidCert = i;
		}
	}

	// Did we find a valid cert?
	if ( pKey->m_idxNewestValidCert < 0 )
	{
		pKey->m_eTrust = k_ETrust_NotTrusted;
		pKey->m_effectiveAuthScope.SetEmpty();
		const std::string &sFirstCertMsg = pKey->m_vecCerts[0].m_status_msg;
		Assert( !sFirstCertMsg.empty() );
		if ( pKey->m_vecCerts.size() == 1 )
		{
			pKey->m_status_msg = sFirstCertMsg;
		}
		else
		{
			pKey->m_status_msg = V_sprintf_stdstring( "None of %d certs trusted.  (E.g.: ", len( pKey->m_vecCerts ) )  + sFirstCertMsg + ")";
		}
		return;
	}

	// Trusted!
	pKey->m_eTrust = k_ETrust_Trusted;
	pKey->m_status_msg.clear();
	Assert( !pKey->m_effectiveAuthScope.IsEmpty() );
}

static void CertStore_EnsureTrustValid()
{
	CertStore_OneTimeInit();
	if ( s_bTrustValid )
		return;

	// Mark everything not in a "terminal" state as unknown
	for ( const std::unique_ptr<PublicKey> &pKey: s_mapPublicKeys.IterValues() )
	{
		if ( pKey->m_eTrust != k_ETrust_Revoked && pKey->m_eTrust != k_ETrust_Hardcoded )
			pKey->m_eTrust = k_ETrust_Unknown;
	}

	// Now scan all keys, and recursively calculate their trust
	for ( const std::unique_ptr<PublicKey> &pKey: s_mapPublicKeys.IterValues() )
	{
		RecursiveEvaluateKeyTrust( pKey.get() );
	}

	// Mark trust as having been calculated
	s_bTrustValid = true;
}

const CertAuthScope *CertStore_CheckCASignature( const std::string &signed_data, uint64 nCAKeyID, const std::string &signature, time_t timeNow, SteamNetworkingErrMsg &errMsg )
{
	CertStore_EnsureTrustValid();

	// Make sure they actually presented any data
	if ( signed_data.empty() )
	{
		V_strcpy_safe( errMsg, "No signed data" );
		return nullptr;
	}

	// Check that signature appears valid.
	if ( signature.empty() )
	{
		V_strcpy_safe( errMsg, "No signature" );
		return nullptr;
	}
	
	// Locate the cert
	if ( nCAKeyID == 0 )
	{
		V_strcpy_safe( errMsg, "Missing CA Key ID" );
		return nullptr;
	}
	PublicKey *pKey = FindPublicKey( nCAKeyID );
	if ( pKey == nullptr )
	{
		V_sprintf_safe( errMsg, "CA key %llu is not known to us", (unsigned long long)nCAKeyID );
		return nullptr;
	}

	// Check the status of the cert
	Assert( pKey->m_eTrust != k_ETrust_UnknownWorking && pKey->m_eTrust != k_ETrust_Unknown );
	if ( pKey->m_eTrust < k_ETrust_Trusted )
	{
		V_sprintf_safe( errMsg, "CA key %llu is not trusted.  %s", (unsigned long long)nCAKeyID, pKey->m_status_msg.c_str() );
		return nullptr;
	}

	// Is any part of the chain expired?
	if ( pKey->m_effectiveAuthScope.m_timeExpiry < timeNow )
	{
		V_sprintf_safe( errMsg, "CA key %llu (or an antecedent) expired %lld seconds ago!", (unsigned long long)nCAKeyID, (long long)( timeNow - pKey->m_effectiveAuthScope.m_timeExpiry ) );
		return nullptr;
	}

	// We only support one crypto method right now.
	if ( signature.length() != sizeof(CryptoSignature_t) )
	{
		V_strcpy_safe( errMsg, "Signature has invalid length" );
		return nullptr;
	}

	// Do the crypto work to check the signature
	if ( !pKey->m_keyPublic.VerifySignature( signed_data.c_str(), signed_data.length(), *(const CryptoSignature_t *)signature.c_str() ) )
	{
		V_strcpy_safe( errMsg, "Signature verification failed" );
		return nullptr;
	}

	return &pKey->m_effectiveAuthScope;
}

const CertAuthScope *CertStore_CheckCert( const CMsgSteamDatagramCertificateSigned &msgCertSigned, CMsgSteamDatagramCertificate &outMsgCert, time_t timeNow, SteamNetworkingErrMsg &errMsg )
{
	const CertAuthScope *pResult = CertStore_CheckCASignature( msgCertSigned.cert(), msgCertSigned.ca_key_id(), msgCertSigned.ca_signature(), timeNow, errMsg );
	if ( !pResult )
		return nullptr;
	if ( !outMsgCert.ParseFromString( msgCertSigned.cert() ) )
	{
		V_strcpy_safe( errMsg, "Cert failed protobuf parse" );
		return nullptr;
	}

	// Check expiry
	if ( (time_t)outMsgCert.time_expiry() < timeNow )
	{
		V_sprintf_safe( errMsg, "Cert expired %lld seconds ago", (long long)( timeNow - outMsgCert.time_expiry() ) );
		return nullptr;
	}

	return pResult;
}

bool CheckCertAppID( const CMsgSteamDatagramCertificate &msgCert, const CertAuthScope *pCACertAuthScope, AppId_t nAppID, SteamNetworkingErrMsg &errMsg )
{

	// Not bound to specific AppIDs?
	if ( msgCert.app_ids_size() == 0 )
	{
		if ( !pCACertAuthScope || pCACertAuthScope->m_apps.HasItem( nAppID ) )
			return true;
		V_sprintf_safe( errMsg, "Cert is not restricted by appid, by CA trust chain is, and does not authorize %u", nAppID );
		return true;
	}

	// Search cert for the one they are trying
	for ( AppId_t nCertAppID: msgCert.app_ids() )
	{
		if ( nCertAppID == nAppID )
		{
			if ( !pCACertAuthScope || pCACertAuthScope->m_apps.HasItem( nAppID ) )
				return true;
			V_sprintf_safe( errMsg, "Cert allows appid %u, but CA trust chain does not", nAppID );
			return false;
		}
	}

	// No good
	if ( msgCert.app_ids_size() == 1 )
	{
		V_sprintf_safe( errMsg, "Cert is not authorized for appid %u, only %u", nAppID, msgCert.app_ids(0) );
	}
	else
	{
		V_sprintf_safe( errMsg, "Cert is not authorized for appid %u, only %u (and %d more)", nAppID, msgCert.app_ids(0), msgCert.app_ids_size()-1 );
	}
	return false;
}

bool CheckCertPOPID( const CMsgSteamDatagramCertificate &msgCert, const CertAuthScope *pCACertAuthScope, SteamNetworkingPOPID popID, SteamNetworkingErrMsg &errMsg )
{

	// Not bound to specific PopIDs?
	if ( msgCert.gameserver_datacenter_ids_size() == 0 )
	{
		if ( !pCACertAuthScope || pCACertAuthScope->m_pops.HasItem( popID ) )
			return true;
		V_sprintf_safe( errMsg, "Cert is not restricted by POPID, by CA trust chain is, and does not authorize %s", SteamNetworkingPOPIDRender( popID ).c_str() );
		return true;
	}

	// Search cert for the one they are trying
	for ( SteamNetworkingPOPID certPOPID: msgCert.gameserver_datacenter_ids() )
	{
		if ( certPOPID == popID )
		{
			if ( !pCACertAuthScope || pCACertAuthScope->m_pops.HasItem( popID ) )
				return true;
			V_sprintf_safe( errMsg, "Cert allows POPID %s, but CA trust chain does not", SteamNetworkingPOPIDRender( popID ).c_str() );
			return false;
		}
	}

	// No good
	SteamNetworkingPOPIDRender firstAuthorizedPopID( msgCert.gameserver_datacenter_ids(0) );
	if ( msgCert.app_ids_size() == 1 )
	{
		V_sprintf_safe( errMsg, "Cert is not authorized for POPID %s, only %s", SteamNetworkingPOPIDRender( popID ).c_str(), firstAuthorizedPopID.c_str() );
	}
	else
	{
		V_sprintf_safe( errMsg, "Cert is not authorized for POPID %s, only %s (and %d more)", SteamNetworkingPOPIDRender( popID ).c_str(), firstAuthorizedPopID.c_str(), msgCert.gameserver_datacenter_ids_size()-1 );
	}
	return false;
}

void CertStore_Check()
{
	CertStore_EnsureTrustValid();

	for ( auto item: s_mapPublicKeys.IterItems() )
	{
		const std::unique_ptr<PublicKey> &pKey = item.Element();
		AssertMsg2( pKey->IsTrusted() || pKey->m_eTrust == k_ETrust_Revoked, "Key %llu not trusted: %s", (unsigned long long)item.Key(), pKey->m_status_msg.c_str() );
	}
}

void CertStore_Print( std::ostream &out )
{
	CertStore_EnsureTrustValid();

	for ( auto item: s_mapPublicKeys.IterItems() )
	{
		const std::unique_ptr<PublicKey> &pKey = item.Element();
		out << "Public key " << (unsigned long long)item.Key() << " " << MsgOrOK(pKey->m_status_msg) << std::endl;
		pKey->m_effectiveAuthScope.Print( out, "  " );
		if ( pKey->m_idxNewestValidCert >= 0 )
		{
			pKey->m_vecCerts[ pKey->m_idxNewestValidCert ].Print( out, "  " );
		}
		else if ( !pKey->m_vecCerts.empty() )
		{
			for ( const Cert &c: pKey->m_vecCerts )
			{
				c.Print( out, "  " );
			}
		}
		else
		{
			out << "  (No valid certs)" << std::endl;
		}
	}
}

#ifdef DBGFLAG_VALIDATE
void CertStore_ValidateStatics( CValidator &validator )
{
	ValidateRecursive( s_mapPublicKeys );
}
#endif

} // namespace SteamNetworkingSocketsLib
