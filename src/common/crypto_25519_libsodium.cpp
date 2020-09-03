#include "crypto.h"
#include "crypto_25519.h"

#include <tier0/dbg.h>

#ifdef STEAMNETWORKINGSOCKETS_CRYPTO_25519_LIBSODIUM

#include <sodium.h>

CEC25519KeyBase::~CEC25519KeyBase()
{
	Wipe();
}

bool CEC25519KeyBase::IsValid() const
{
	return CCryptoKeyBase_RawBuffer::IsValid();
}

uint32 CEC25519KeyBase::GetRawData( void *pData ) const
{
	return CCryptoKeyBase_RawBuffer::GetRawData( pData );
}

void CEC25519KeyBase::Wipe()
{
	CCryptoKeyBase_RawBuffer::Wipe();
}

bool CEC25519KeyBase::SetRawData( const void *pData, size_t cbData )
{
	if ( cbData != 32 )
		return false;
	return CCryptoKeyBase_RawBuffer::SetRawData( pData, cbData );
}

bool CCrypto::PerformKeyExchange( const CECKeyExchangePrivateKey &localPrivateKey, const CECKeyExchangePublicKey &remotePublicKey, SHA256Digest_t *pSharedSecretOut )
{
	Assert( localPrivateKey.IsValid() );
	Assert( remotePublicKey.IsValid() );

	if ( !localPrivateKey.IsValid() || !remotePublicKey.IsValid() )
	{
		// Fail securely - generate something that won't be the same on both sides!
		GenerateRandomBlock( *pSharedSecretOut, sizeof( SHA256Digest_t ) );
		return false;
	}

	uint8 bufSharedSecret[32];
	uint8 bufLocalPrivate[32];
	uint8 bufRemotePublic[32];

	localPrivateKey.GetRawData(bufLocalPrivate);
	remotePublicKey.GetRawData(bufRemotePublic);

	const int nResult = crypto_scalarmult_curve25519(bufSharedSecret, bufLocalPrivate, bufRemotePublic);

	SecureZeroMemory( bufLocalPrivate, 32 );
	SecureZeroMemory( bufRemotePublic, 32 );

	if(nResult != 0)
	{
		return false;
	}

	GenerateSHA256Digest( bufSharedSecret, sizeof(bufSharedSecret), pSharedSecretOut );
	SecureZeroMemory( bufSharedSecret, 32 );

	return true;
}

void CECSigningPrivateKey::GenerateSignature( const void *pData, size_t cbData, CryptoSignature_t *pSignatureOut ) const
{
	if ( !IsValid() )
	{
		AssertMsg( false, "Key not initialized, cannot generate signature" );
		sodium_memzero( pSignatureOut, sizeof( CryptoSignature_t ) );
		return;
	}

	// libsodium secret key is concatenation of:
	// seed (i.e. what everyone else calls the secret key)
	// public key

	uint8 bufSodiumSecret[crypto_sign_ed25519_SECRETKEYBYTES];

	Assert( CCryptoKeyBase_RawBuffer::GetRawDataSize() == 32 );
	Assert( sizeof(m_publicKey) == 32 );
	Assert( crypto_sign_ed25519_SECRETKEYBYTES == 64 );

	memcpy(bufSodiumSecret, CCryptoKeyBase_RawBuffer::GetRawDataPtr(), 32 );
	memcpy(bufSodiumSecret + 32, m_publicKey, sizeof(m_publicKey));

	crypto_sign_ed25519_detached(*pSignatureOut, nullptr, static_cast<const unsigned char*>( pData ), cbData, bufSodiumSecret );
	sodium_memzero(bufSodiumSecret, sizeof(bufSodiumSecret) );
}

bool CECSigningPublicKey::VerifySignature( const void *pData, size_t cbData, const CryptoSignature_t &signature ) const
{
	if ( !IsValid() )
	{
		AssertMsg( false, "Key not initialized, cannot verify signature" );
		return false;
	}

	return crypto_sign_ed25519_verify_detached( signature, static_cast<const unsigned char*>( pData ), cbData, CCryptoKeyBase_RawBuffer::GetRawDataPtr() ) == 0;
}

bool CEC25519PrivateKeyBase::CachePublicKey()
{
	// Need to convert the private key into a public key here
	// then store in m_publicKey
	if ( !IsValid() )
	{
		return false;
	}

	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate )
	{
		// Get public key from secret key
		AssertMsg( sizeof(m_publicKey) == crypto_scalarmult_curve25519_bytes(), "Public key size mismatch." );
		AssertMsg( CCryptoKeyBase_RawBuffer::GetRawDataSize() == crypto_scalarmult_curve25519_scalarbytes(), "Private key size mismatch." );

		crypto_scalarmult_curve25519_base( m_publicKey, CCryptoKeyBase_RawBuffer::GetRawDataPtr() );
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
	{
		// Convert ed25519 private signing key to ed25519 public key
		// Note that what everyone else calls the private key, libsodium calls the seed
		AssertMsg( sizeof(m_publicKey) == crypto_sign_ed25519_publickeybytes(), "Public key size mismatch." );
		AssertMsg( CCryptoKeyBase_RawBuffer::GetRawDataSize() == crypto_sign_ed25519_seedbytes(), "Private key size mismatch." );

		unsigned char h[crypto_hash_sha512_BYTES];

		crypto_sign_ed25519_seed_keypair( m_publicKey, h, static_cast<const unsigned char*>( CCryptoKeyBase_RawBuffer::GetRawDataPtr() ) );

		sodium_memzero(h, sizeof(h));
	}
	else
	{
		Assert( false );
		return false;
	}

	return true;
}

#endif
