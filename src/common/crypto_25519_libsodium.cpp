//========= Copyright Valve LLC, All rights reserved. ========================

#include "crypto.h"

#ifdef USE_LIBSODIUM

#include <sodium.h>

//-----------------------------------------------------------------------------
// Purpose: Generate a curve25519 key pair for Diffie-Hellman secure key exchange
//-----------------------------------------------------------------------------
void CCrypto::GenerateKeyExchangeKeyPair( CECKeyExchangePublicKey *pPublicKey, CECKeyExchangePrivateKey *pPrivateKey )
{
	pPrivateKey->Wipe();
	pPublicKey->Wipe();

	uint8 rgubSecretData[32];
	GenerateRandomBlock( rgubSecretData, 32 );
	pPrivateKey->RebuildFromPrivateData( rgubSecretData );
	SecureZeroMemory( rgubSecretData, 32 );
	pPrivateKey->GetPublicKey( pPublicKey );
}

//-----------------------------------------------------------------------------
// Purpose: Generate a shared secret from two exchanged curve25519 keys
//-----------------------------------------------------------------------------
void CCrypto::PerformKeyExchange( const CECKeyExchangePrivateKey &localPrivateKey, const CECKeyExchangePublicKey &remotePublicKey, SHA256Digest_t *pSharedSecretOut )
{
	Assert( localPrivateKey.IsValid() );
	Assert( remotePublicKey.IsValid() );
	if ( !localPrivateKey.IsValid() || !remotePublicKey.IsValid() )
	{
		// Fail securely - generate something that won't be the same on both sides!
		GenerateRandomBlock( *pSharedSecretOut, sizeof( SHA256Digest_t ) );
		return;
	}

	uint8 bufSharedSecret[32];
	crypto_scalarmult_curve25519( bufSharedSecret, localPrivateKey.GetData() + 32, remotePublicKey.GetData() );
	GenerateSHA256Digest( bufSharedSecret, sizeof(bufSharedSecret), pSharedSecretOut );
	SecureZeroMemory( bufSharedSecret, 32 );
}


//-----------------------------------------------------------------------------
// Purpose: Generate an ed25519 key pair for public-key signature generation
//-----------------------------------------------------------------------------
void CCrypto::GenerateSigningKeyPair( CECSigningPublicKey *pPublicKey, CECSigningPrivateKey *pPrivateKey )
{
	pPrivateKey->Wipe();
	pPublicKey->Wipe();

	uint8 rgubSecretData[32];
	GenerateRandomBlock( rgubSecretData, 32 );
	pPrivateKey->RebuildFromPrivateData( rgubSecretData );
	SecureZeroMemory( rgubSecretData, 32 );
	pPrivateKey->GetPublicKey( pPublicKey );
}


//-----------------------------------------------------------------------------
// Purpose: Generate an ed25519 public-key signature
//-----------------------------------------------------------------------------
void CCrypto::GenerateSignature( const uint8 *pubData, uint32 cubData, const CECSigningPrivateKey &privateKey, CryptoSignature_t *pSignatureOut )
{
	Assert( privateKey.IsValid() );
	memset( pSignatureOut, 0, sizeof( CryptoSignature_t ) );
	if ( !privateKey.IsValid() )
	{
		return;
	}

	// HACK: We store <public><private>, libsodium stores it <private><public>
	uint8 swapped[64];
	memcpy(swapped, privateKey.GetData() + 32, 32);
	memcpy(swapped + 32, privateKey.GetData(), 32);

	uint64 cubSignature;
	crypto_sign_ed25519_detached( *pSignatureOut, &cubSignature, pubData, cubData, swapped );
}


//-----------------------------------------------------------------------------
// Purpose: Generate an ed25519 public-key signature
//-----------------------------------------------------------------------------
bool CCrypto::VerifySignature( const uint8 *pubData, uint32 cubData, const CECSigningPublicKey &publicKey, const CryptoSignature_t &signature )
{
	Assert( publicKey.IsValid() );
	return publicKey.IsValid() && crypto_sign_ed25519_verify_detached( signature, pubData, cubData, publicKey.GetData() ) == 0;
}

//-----------------------------------------------------------------------------
// Purpose: Generate a 25519 key pair (either x25519 or ed25519)
//-----------------------------------------------------------------------------
void CEC25519PrivateKeyBase::RebuildFromPrivateData( const uint8 privateKeyData[32] )
{
	union {
		uint8 bufComplete[64];
		struct {
			uint8 bufPublic[32];
			uint8 bufPrivate[32];
		} alias;
	};
	V_memset( alias.bufPublic, 0, 32 );
	V_memcpy( alias.bufPrivate, privateKeyData, 32 );
	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate )
	{
		crypto_scalarmult_curve25519_base( alias.bufPublic, alias.bufPrivate );
		this->Set( bufComplete, 64 );
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
	{
		// all bits are meaningful in the ed25519 scheme, which internally constructs
		// a curve-25519 private key by hashing all 32 bytes of private key material.
		uint8 scratch[64];
		crypto_sign_ed25519_seed_keypair( alias.bufPublic, scratch, alias.bufPrivate );
		this->Set( bufComplete, 64 );
	}
	else
	{
		this->Wipe();
	}
	SecureZeroMemory( bufComplete, 64 );
}

#endif
