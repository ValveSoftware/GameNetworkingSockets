//========= Copyright Valve LLC, All rights reserved. ========================
#include "gnsconfig.h"

#ifdef GNS_CRYPTO_25519_REF

#include "crypto.h"

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
// Purpose: Generate a shared secret from two exchanged curve25519 keys
//-----------------------------------------------------------------------------
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
	CHOOSE_25519_IMPL( curve25519_donna )( bufSharedSecret, bufLocalPrivate, bufRemotePublic );
	SecureZeroMemory( bufLocalPrivate, 32 );
	SecureZeroMemory( bufRemotePublic, 32 );
	GenerateSHA256Digest( bufSharedSecret, sizeof(bufSharedSecret), pSharedSecretOut );
	SecureZeroMemory( bufSharedSecret, 32 );

	return true;
}


void CECSigningPrivateKey::GenerateSignature( const void *pData, size_t cbData, CryptoSignature_t *pSignatureOut ) const
{
	if ( !IsValid() )
	{
		AssertMsg( false, "Key not initialized, cannot generate signature" );
		memset( pSignatureOut, 0, sizeof( CryptoSignature_t ) );
		return;
	}

	CHOOSE_25519_IMPL( ed25519_sign )( (const uint8 *)pData, cbData, m_pData, GetPublicKeyRawData(), *pSignatureOut );
}


bool CECSigningPublicKey::VerifySignature( const void *pData, size_t cbData, const CryptoSignature_t &signature ) const
{
	if ( !IsValid() )
	{
		AssertMsg( false, "Key not initialized, cannot verify signature" );
		return false;
	}

	bool ret = CHOOSE_25519_IMPL( ed25519_sign_open )( (const uint8 *)pData, cbData, m_pData, signature ) == 0;
	return ret;
}

bool CEC25519KeyBase::SetRawData( const void *pData, size_t cbData )
{
	if ( cbData != 32 )
		return false;
	return CCryptoKeyBase_RawBuffer::SetRawData( pData, cbData );
}

CEC25519KeyBase::~CEC25519KeyBase()
{
	Wipe();
}

void CEC25519KeyBase::Wipe()
{
	CCryptoKeyBase_RawBuffer::Wipe();
}

bool CEC25519KeyBase::IsValid() const
{
	return CCryptoKeyBase_RawBuffer::IsValid();
}

uint32 CEC25519KeyBase::GetRawData( void *pData ) const
{
	return CCryptoKeyBase_RawBuffer::GetRawData( pData );
}

bool CEC25519PrivateKeyBase::CachePublicKey()
{
	if ( !IsValid() )
		return false;

	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate )
	{
		// Ed25519 codebase provides a faster version of curve25519_donna_basepoint.
		//CHOOSE_25519_IMPL( curve25519_donna_basepoint )( alias.bufPublic, alias.bufPrivate );
		CHOOSE_25519_IMPL( curved25519_scalarmult_basepoint )( m_publicKey, CCryptoKeyBase_RawBuffer::GetRawDataPtr() );
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
	{
		// all bits are meaningful in the ed25519 scheme, which internally constructs
		// a curve-25519 private key by hashing all 32 bytes of private key material.
		CHOOSE_25519_IMPL( ed25519_publickey )( CCryptoKeyBase_RawBuffer::GetRawDataPtr(), m_publicKey );
	}
	else
	{
		Assert( false );
		return false;
	}

	return true;
}

#endif // #ifdef GNS_CRYPTO_25519_REF

