//========= Copyright Valve LLC, All rights reserved. ========================


// Note: not using precompiled headers! This file is included directly by
// several different projects and may include Crypto++ headers depending
// on compile-time defines, which in turn pulls in other odd dependencies

#include "crypto.h"

#if !defined(USE_LIBSODIUM)
extern "C" {
// external headers for curve25519 and ed25519 support, plus alternate 32-bit SSE2 versions
// (for x64, pure-C performance is on par with SSE2, so we don't compile the SSE2 versions)
#include "../external/ed25519-donna/ed25519.h"
#include "../external/curve25519-donna/curve25519.h"

#if defined( _M_IX86 ) || defined( __i386__ )
void curve25519_donna_sse2( curve25519_key mypublic, const curve25519_key secret, const curve25519_key basepoint );
void curve25519_donna_basepoint_sse2( curve25519_key mypublic, const curve25519_key secret );
void curved25519_scalarmult_basepoint_sse2( curved25519_key pk, const curved25519_key e );
void ed25519_publickey_sse2( const ed25519_secret_key sk, ed25519_public_key pk );
int ed25519_sign_open_sse2( const unsigned char *m, size_t mlen, const ed25519_public_key pk, const ed25519_signature RS );
void ed25519_sign_sse2( const unsigned char *m, size_t mlen, const ed25519_secret_key sk, const ed25519_public_key pk, ed25519_signature RS );

#ifdef OSX // We can assume SSE2 for all Intel macs running 32-bit code
#define CHOOSE_25519_IMPL( func ) func##_sse2
#else
#define CHOOSE_25519_IMPL( func ) ( GetCPUInformation().m_bSSE2 ? func##_sse2 : func )
#endif
#endif

#ifndef CHOOSE_25519_IMPL
#define CHOOSE_25519_IMPL( func ) func
#endif
}

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
	CHOOSE_25519_IMPL( curve25519_donna )( bufSharedSecret, localPrivateKey.GetData() + 32, remotePublicKey.GetData() );
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
	if ( !privateKey.IsValid() )
	{
		memset( pSignatureOut, 0, sizeof( CryptoSignature_t ) );
		return;
	}
	CHOOSE_25519_IMPL( ed25519_sign )( pubData, cubData, privateKey.GetData() + 32, privateKey.GetData(), *pSignatureOut );
}


//-----------------------------------------------------------------------------
// Purpose: Generate an ed25519 public-key signature
//-----------------------------------------------------------------------------
bool CCrypto::VerifySignature( const uint8 *pubData, uint32 cubData, const CECSigningPublicKey &publicKey, const CryptoSignature_t &signature )
{
	Assert( publicKey.IsValid() );
	return publicKey.IsValid() && CHOOSE_25519_IMPL( ed25519_sign_open )( pubData, cubData, publicKey.GetData(), signature ) == 0;
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
	V_memcpy( alias.bufPrivate, privateKeyData, 32 );
	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate )
	{
		// Ed25519 codebase provides a faster version of curve25519_donna_basepoint.
		//CHOOSE_25519_IMPL( curve25519_donna_basepoint )( alias.bufPublic, alias.bufPrivate );
		CHOOSE_25519_IMPL( curved25519_scalarmult_basepoint )(alias.bufPublic, alias.bufPrivate);
		this->Set( bufComplete, 64 );
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
	{
		// all bits are meaningful in the ed25519 scheme, which internally constructs
		// a curve-25519 private key by hashing all 32 bytes of private key material.
		CHOOSE_25519_IMPL( ed25519_publickey )( alias.bufPrivate, alias.bufPublic );
		this->Set( bufComplete, 64 );
	}
	else
	{
		this->Wipe();
	}
	SecureZeroMemory( bufComplete, 64 );
}

#endif
