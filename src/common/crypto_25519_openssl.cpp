//========= Copyright Valve LLC, All rights reserved. ========================
#include "crypto.h"
#include "crypto_25519.h"
#include <tier0/dbg.h>

#ifdef STEAMNETWORKINGSOCKETS_CRYPTO_25519_OPENSSL

#include <openssl/evp.h>

#if OPENSSL_VERSION_NUMBER < 0x10101000
// https://www.openssl.org/docs/man1.1.1/man3/EVP_PKEY_get_raw_private_key.html
#error "Raw access to 25519 keys requires OpenSSL 1.1.1"
#endif

CEC25519KeyBase::~CEC25519KeyBase()
{
	Wipe();
}

bool CEC25519KeyBase::IsValid() const
{
	return m_evp_pkey != nullptr;
}

uint32 CEC25519KeyBase::GetRawData( void *pData ) const
{
	EVP_PKEY *pkey = (EVP_PKEY*)m_evp_pkey;
	if ( !pkey )
		return 0;

	// All 25519 keys are the same size
	if ( !pData )
		return 32;

	// Using a switch here instead of overriding virtual functions
	// seems kind of messy, but given the other crypto providers,
	// it's probably the simplest, cleanest thing.
	size_t len = 32;
	switch ( m_eKeyType )
	{
		case k_ECryptoKeyTypeSigningPublic:
		case k_ECryptoKeyTypeKeyExchangePublic:
			if ( EVP_PKEY_get_raw_public_key( pkey, (unsigned char *)pData, &len ) != 1 )
			{
				AssertMsg( false, "EVP_PKEY_get_raw_public_key failed?" );
				return 0;
			}
			break;

		case k_ECryptoKeyTypeSigningPrivate:
		case k_ECryptoKeyTypeKeyExchangePrivate:
			if ( EVP_PKEY_get_raw_private_key( pkey, (unsigned char *)pData, &len ) != 1 )
			{
				AssertMsg( false, "EVP_PKEY_get_raw_private_key failed?" );
				return 0;
			}
			break;

		default:
			AssertMsg( false, "Invalid 25519 key type" );
			return 0;
	}

	if ( len != 32 )
	{
		AssertMsg1( false, "unexpected raw key size %d", (int)len );
		return 0;
	}
	return 32;
}

void CEC25519KeyBase::Wipe()
{
	// We should never be using the raw buffer
	Assert( CCryptoKeyBase_RawBuffer::m_pData == nullptr );
	Assert( CCryptoKeyBase_RawBuffer::m_cbData == 0 );

	EVP_PKEY_free( (EVP_PKEY*)m_evp_pkey );
	m_evp_pkey = nullptr;
}

bool CEC25519KeyBase::SetRawData( const void *pData, size_t cbData )
{
	Wipe();

	EVP_PKEY *pkey = nullptr;
	switch ( m_eKeyType )
	{
		case k_ECryptoKeyTypeSigningPublic:
			pkey = EVP_PKEY_new_raw_public_key( EVP_PKEY_ED25519, nullptr, (const unsigned char *)pData, cbData );
			break;

		case k_ECryptoKeyTypeSigningPrivate:
			pkey = EVP_PKEY_new_raw_private_key( EVP_PKEY_ED25519, nullptr, (const unsigned char *)pData, cbData );
			break;

		case k_ECryptoKeyTypeKeyExchangePublic:
			pkey = EVP_PKEY_new_raw_public_key( EVP_PKEY_X25519, nullptr, (const unsigned char *)pData, cbData );
			break;

		case k_ECryptoKeyTypeKeyExchangePrivate:
			pkey = EVP_PKEY_new_raw_private_key( EVP_PKEY_X25519, nullptr, (const unsigned char *)pData, cbData );
			break;
	}
	if ( pkey == nullptr )
	{
		AssertMsg1( false, "EVP_PKEY_new_raw_xxx_key failed for key type %d", (int)m_eKeyType );
		return false;
	}

	// Success
	m_evp_pkey = pkey;
	return true;
}

bool CCrypto::PerformKeyExchange( const CECKeyExchangePrivateKey &localPrivateKey, const CECKeyExchangePublicKey &remotePublicKey, SHA256Digest_t *pSharedSecretOut )
{
	// Check if caller didn't provide valid keys
	EVP_PKEY *pkey = (EVP_PKEY*)localPrivateKey.evp_pkey();
	EVP_PKEY *peerkey = (EVP_PKEY*)remotePublicKey.evp_pkey();
	if ( !pkey || !peerkey )
	{
		AssertMsg( false, "Cannot perform key exchange, keys not valid" );
		GenerateRandomBlock( pSharedSecretOut, sizeof(*pSharedSecretOut) ); // In case caller isn't checking return value
		return false;
	}

	size_t skeylen = sizeof(*pSharedSecretOut);
	uint8 bufSharedSecret[32];

	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new( pkey, nullptr );

	// Unless we have a bug, all these errors "should never happen".
	VerifyFatal( ctx );
	VerifyFatal( EVP_PKEY_derive_init(ctx) == 1 );
	VerifyFatal( EVP_PKEY_derive_set_peer(ctx, peerkey) == 1 );
	VerifyFatal( EVP_PKEY_derive(ctx, bufSharedSecret, &skeylen ) == 1 );
	VerifyFatal( skeylen == sizeof(*pSharedSecretOut) );

	EVP_PKEY_CTX_free(ctx);

	GenerateSHA256Digest( bufSharedSecret, sizeof(bufSharedSecret), pSharedSecretOut );
	SecureZeroMemory( bufSharedSecret, 32 );

	return true;
}


void CECSigningPrivateKey::GenerateSignature( const void *pData, size_t cbData, CryptoSignature_t *pSignatureOut ) const
{
	EVP_PKEY *pkey = (EVP_PKEY*)m_evp_pkey;
	if ( !pkey )
	{
		AssertMsg( false, "Key not initialized, cannot generate signature" );
		memset( pSignatureOut, 0, sizeof( CryptoSignature_t ) );
		return;
	}

	EVP_MD_CTX *ctx = EVP_MD_CTX_create();
	VerifyFatal( ctx );
	VerifyFatal( EVP_DigestSignInit( ctx, nullptr, nullptr, nullptr, pkey ) == 1 );

	size_t siglen = sizeof(*pSignatureOut);
	VerifyFatal( EVP_DigestSign( ctx, (unsigned char *)pSignatureOut, &siglen, (const unsigned char *)pData, cbData ) == 1 );
	VerifyFatal( siglen == sizeof(*pSignatureOut) );

	EVP_MD_CTX_free(ctx);
}

bool CECSigningPublicKey::VerifySignature( const void *pData, size_t cbData, const CryptoSignature_t &signature ) const
{
	EVP_PKEY *pkey = (EVP_PKEY*)m_evp_pkey;
	if ( !pkey )
	{
		AssertMsg( false, "Key not initialized, cannot verify signature" );
		return false;
	}

	EVP_MD_CTX *ctx = EVP_MD_CTX_create();
	VerifyFatal( ctx );
	VerifyFatal( EVP_DigestVerifyInit( ctx, nullptr, nullptr, nullptr, pkey ) == 1 );

	int r = EVP_DigestVerify( ctx, (const unsigned char *)signature, sizeof(signature), (const unsigned char *)pData, cbData );

	EVP_MD_CTX_free(ctx);

	return r == 1;
}

bool CEC25519PrivateKeyBase::CachePublicKey()
{
	EVP_PKEY *pkey = (EVP_PKEY*)m_evp_pkey;
	if ( !pkey )
		return false;

	size_t len = 32;
	if ( !EVP_PKEY_get_raw_public_key( pkey, m_publicKey, &len ) )
	{
		AssertMsg( false, "EVP_PKEY_get_raw_public_key failed?!" );
		return false;
	}
	Assert( len == 32 );

	return true;
}

#endif // #ifdef GNS_CRYPTO_25519_OPENSSL

