//========= Copyright Valve LLC, All rights reserved. ========================

#include "crypto.h"

#ifdef VALVE_CRYPTO_25519_OPENSSLEVP

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
	// FIXME
}

void CEC25519KeyBase::Wipe()
{
	// FIXME free m_evp_pkey
}

bool CEC25519KeyBase::SetRawData( const void *pData, size_t cbData )
{
	Wipe();

	// FIXME
}

//-----------------------------------------------------------------------------
// Purpose: Generate a curve25519 key pair for Diffie-Hellman secure key exchange
//-----------------------------------------------------------------------------
void CCrypto::GenerateKeyExchangeKeyPair( CECKeyExchangePublicKey *pPublicKey, CECKeyExchangePrivateKey *pPrivateKey )
{
	// FIXME
}

//-----------------------------------------------------------------------------
// Purpose: Generate a shared secret from two exchanged curve25519 keys
//-----------------------------------------------------------------------------
void CCrypto::PerformKeyExchange( const CECKeyExchangePrivateKey &localPrivateKey, const CECKeyExchangePublicKey &remotePublicKey, SHA256Digest_t *pSharedSecretOut )
{
	// FIXME
}


//-----------------------------------------------------------------------------
// Purpose: Generate an ed25519 key pair for public-key signature generation
//-----------------------------------------------------------------------------
void CCrypto::GenerateSigningKeyPair( CECSigningPublicKey *pPublicKey, CECSigningPrivateKey *pPrivateKey )
{
	// FIXME
}


//-----------------------------------------------------------------------------
// Purpose: Generate an ed25519 public-key signature
//-----------------------------------------------------------------------------
void CCrypto::GenerateSignature( const uint8 *pubData, uint32 cubData, const CECSigningPrivateKey &privateKey, CryptoSignature_t *pSignatureOut )
{
	// FIXME
}


//-----------------------------------------------------------------------------
// Purpose: Generate an ed25519 public-key signature
//-----------------------------------------------------------------------------
bool CCrypto::VerifySignature( const uint8 *pubData, uint32 cubData, const CECSigningPublicKey &publicKey, const CryptoSignature_t &signature )
{
	Assert( publicKey.IsValid() );
	return publicKey.IsValid() && CHOOSE_25519_IMPL( ed25519_sign_open )( pubData, cubData, publicKey.GetRawDataPtr(), signature ) == 0;
}

bool CEC25519KeyBase::SetRawData( const void *pData, size_t cbData )
{
	if ( cbData != 32 )
		return false;
	return CCryptoKeyBase_RawBuffer::SetRawBufferData( pData, cbData );
}

bool CEC25519PrivateKeyBase::CachePublicKey()
{
	if ( !IsValid() )
		return false;

	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate )
	{
		// FIXME
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
	{
		// FIXME
	}
	else
	{
		Assert( false );
		return false;
	}

	return true;
}

#endif // #ifdef VALVE_CRYPTO_25519_OPENSSLEVP

