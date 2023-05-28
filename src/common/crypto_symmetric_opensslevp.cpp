//========= Copyright Valve LLC, All rights reserved. ========================

#include <tier0/dbg.h>
#include "crypto.h"

#include "tier0/memdbgoff.h"
#include <openssl/evp.h>
#include "tier0/memdbgon.h"

extern void OneTimeCryptoInitOpenSSL();

SymmetricCryptContextBase::SymmetricCryptContextBase()
{
	m_ctx = nullptr;
	m_cbIV = 0;
	m_cbTag = 0;
}

void SymmetricCryptContextBase::Wipe()
{
	if ( m_ctx )
	{
		EVP_CIPHER_CTX *ctx = (EVP_CIPHER_CTX*)m_ctx;
		#if OPENSSL_VERSION_NUMBER < 0x10100000
			EVP_CIPHER_CTX_cleanup( ctx );
			delete ctx;
		#else
			EVP_CIPHER_CTX_free( ctx );
		#endif
		m_ctx = nullptr;
	}
	m_cbIV = 0;
	m_cbTag = 0;
}

bool AES_GCM_CipherContext::InitCipher( const void *pKey, size_t cbKey, size_t cbIV, size_t cbTag, bool bEncrypt )
{
	EVP_CIPHER_CTX *ctx = (EVP_CIPHER_CTX*)m_ctx;
	if ( ctx )
	{
		#if OPENSSL_VERSION_NUMBER < 0x10100000
			EVP_CIPHER_CTX_cleanup( ctx );
			EVP_CIPHER_CTX_init( ctx );
		#else
			EVP_CIPHER_CTX_reset( ctx );
		#endif
	}
	else
	{
		#if OPENSSL_VERSION_NUMBER < 0x10100000
			ctx = new EVP_CIPHER_CTX;
			if ( !ctx )
				return false;
			EVP_CIPHER_CTX_init( ctx );
		#else
			ctx = EVP_CIPHER_CTX_new();
			if ( !ctx )
				return false;
		#endif
		m_ctx = ctx;
	}

	// Select the cipher based on the size of the key
	const EVP_CIPHER *cipher = nullptr;
	switch ( cbKey )
	{
		case 128/8: cipher = EVP_aes_128_gcm(); break;
		case 192/8: cipher = EVP_aes_192_gcm(); break;
		case 256/8: cipher = EVP_aes_256_gcm(); break;
	}
	if ( cipher == nullptr )
	{
		AssertMsg( false, "Invalid AES-GCM key size" );
		Wipe();
		return false;
	}

	// Setup for encryption setting the key
	if ( EVP_CipherInit_ex( ctx, cipher, nullptr, (const uint8*)pKey, nullptr, bEncrypt ? 1 : 0 ) != 1 )
	{
		Wipe();
		return false;
	}

	// Set IV length
	if ( EVP_CIPHER_CTX_ctrl( ctx, EVP_CTRL_GCM_SET_IVLEN, (int)cbIV, NULL) != 1 )
	{
		AssertMsg( false, "Bad IV size" );
		Wipe();
		return false;
	}

	// Remember parameters
	m_cbIV = (uint32)cbIV;
	m_cbTag = (uint32)cbTag;
	return true;
}

bool AES_GCM_EncryptContext::Encrypt(
	const void *pPlaintextData, size_t cbPlaintextData,
	const void *pIV,
	void *pEncryptedDataAndTag, uint32 *pcbEncryptedDataAndTag,
	const void *pAdditionalAuthenticationData, size_t cbAuthenticationData // Optional additional authentication data.  Not encrypted, but will be included in the tag, so it can be authenticated.
) {
	EVP_CIPHER_CTX *ctx = (EVP_CIPHER_CTX*)m_ctx;
	if ( !ctx )
	{
		AssertMsg( false, "Not initialized!" );
		*pcbEncryptedDataAndTag = 0;
		return false;
	}

	// Calculate size of encrypted data.  Note that GCM does not use padding.
	uint32 cbEncryptedWithoutTag = (uint32)cbPlaintextData;
	uint32 cbEncryptedTotal = cbEncryptedWithoutTag + m_cbTag;

	// Make sure their buffer is big enough
	if ( cbEncryptedTotal > *pcbEncryptedDataAndTag )
	{
		AssertMsg( false, "Buffer isn't big enough to hold padded+encrypted data and tag" );
		return false;
	}

	// This function really shouldn't fail unless we have a bug,
	// so people might not check the return value.  So make sure
	// if we do fail, they don't think anything was encrypted.
	*pcbEncryptedDataAndTag = 0;

	// Set IV
	VerifyFatal( EVP_EncryptInit_ex( ctx, nullptr, nullptr, nullptr, (const uint8*)pIV ) == 1 );

	int nBytesWritten;

	// AAD, if any
	if ( cbAuthenticationData > 0 && pAdditionalAuthenticationData )
	{
		VerifyFatal( EVP_EncryptUpdate( ctx, nullptr, &nBytesWritten, (const uint8*)pAdditionalAuthenticationData, (int)cbAuthenticationData ) == 1 );
	}
	else
	{
		Assert( cbAuthenticationData == 0 );
	}

	// Now the actual plaintext to be encrypted
	uint8 *pOut = (uint8 *)pEncryptedDataAndTag;
	VerifyFatal( EVP_EncryptUpdate( ctx, pOut, &nBytesWritten, (const uint8*)pPlaintextData, (int)cbPlaintextData ) == 1 );
	pOut += nBytesWritten;

	// Finish up
	VerifyFatal( EVP_EncryptFinal_ex( ctx, pOut, &nBytesWritten ) == 1 );
	pOut += nBytesWritten;

	// Make sure that we have the expected number of encrypted bytes at this point
	VerifyFatal( (uint8 *)pEncryptedDataAndTag + cbEncryptedWithoutTag == pOut );

	// Append the tag
	if ( EVP_CIPHER_CTX_ctrl( ctx, EVP_CTRL_GCM_GET_TAG, (int)m_cbTag, pOut ) != 1 )
	{
		AssertMsg( false, "Bad tag size" );
		return false;
	}

	// Give the caller back the size of everything
	*pcbEncryptedDataAndTag = cbEncryptedTotal;

	// Success.
	return true;
}

bool AES_GCM_DecryptContext::Decrypt(
	const void *pEncryptedDataAndTag, size_t cbEncryptedDataAndTag,
	const void *pIV,
	void *pPlaintextData, uint32 *pcbPlaintextData,
	const void *pAdditionalAuthenticationData, size_t cbAuthenticationData
) {

	EVP_CIPHER_CTX *ctx = (EVP_CIPHER_CTX*)m_ctx;
	if ( !ctx )
	{
		AssertMsg( false, "Not initialized!" );
		*pcbPlaintextData = 0;
		return false;
	}

	// Make sure buffer and tag sizes aren't totally bogus
	if ( m_cbTag > cbEncryptedDataAndTag )
	{
		AssertMsg( false, "Encrypted size doesn't make sense for tag size" );
		*pcbPlaintextData = 0;
		return false;
	}
	uint32 cbEncryptedDataWithoutTag = uint32( cbEncryptedDataAndTag - m_cbTag );

	// Make sure their buffer is big enough.  Remember that in GCM mode,
	// there is no padding, so if this fails, we indeed would have overflowed
	if ( cbEncryptedDataWithoutTag > *pcbPlaintextData )
	{
		AssertMsg( false, "Buffer might not be big enough to hold decrypted data" );
		*pcbPlaintextData = 0;
		return false;
	}

	// People really have to check the return value, but in case they
	// don't, make sure they don't think we decrypted any data
	*pcbPlaintextData = 0;

	// Set IV
	VerifyFatal( EVP_DecryptInit_ex( ctx, nullptr, nullptr, nullptr, (const uint8*)pIV ) == 1 );

	int nBytesWritten;

	// AAD, if any
	if ( cbAuthenticationData > 0 && pAdditionalAuthenticationData )
	{
		// I don't think it's actually possible to failed here, but
		// since the caller really must be checking the return value,
		// let's not make this fatal
		if ( EVP_DecryptUpdate( ctx, nullptr, &nBytesWritten, (const uint8*)pAdditionalAuthenticationData, (int)cbAuthenticationData ) != 1 )
		{
			AssertMsg( false, "EVP_DecryptUpdate failed?" );
			return false;
		}
	}
	else
	{
		Assert( cbAuthenticationData == 0 );
	}

	uint8 *pOut = (uint8 *)pPlaintextData;
	const uint8 *pIn = (const uint8 *)pEncryptedDataAndTag;

	// Now the actual ciphertext to be decrypted
	if ( EVP_DecryptUpdate( ctx, pOut, &nBytesWritten, pIn, (int)cbEncryptedDataWithoutTag ) != 1 )
		return false;
	pOut += nBytesWritten;
	pIn += cbEncryptedDataWithoutTag;

	// Set expected tag value
	if( EVP_CIPHER_CTX_ctrl( ctx, EVP_CTRL_GCM_SET_TAG, (int)m_cbTag, const_cast<uint8*>( pIn ) ) != 1)
	{
		AssertMsg( false, "Bad tag size" );
		return false;
	}

	// Finish up, and check tag
	if ( EVP_DecryptFinal_ex( ctx, pOut, &nBytesWritten ) <= 0 )
		return false; // data has been tamped with
	pOut += nBytesWritten;

	// Make sure we got back the size we expected, and return the size
	VerifyFatal( pOut == (uint8 *)pPlaintextData + cbEncryptedDataWithoutTag );
	*pcbPlaintextData = cbEncryptedDataWithoutTag;

	// Success.
	return true;
}


//
// !KLUDGE! This is not specific to OpenSSL, and I'd like to put it in crypto.cpp.
// But that generates linker errors for some reason.  Gah
//

//-----------------------------------------------------------------------------
bool CCrypto::SymmetricAuthEncryptWithIV(
	const void *pPlaintextData, size_t cbPlaintextData,
	const void *pIV, size_t cbIV,
	void *pEncryptedDataAndTag, uint32 *pcbEncryptedDataAndTag,
	const void *pKey, size_t cbKey,
	const void *pAdditionalAuthenticationData, size_t cbAuthenticationData,
	size_t cbTag
) {

	// Setup a context.  If you are going to be encrypting many buffers with the same parameters,
	// you should create a context and reuse it, to avoid this setup cost
	AES_GCM_EncryptContext ctx;
	if ( !ctx.Init( pKey, cbKey, cbIV, cbTag ) )
		return false;

	// Encrypt it, and cleanup
	return ctx.Encrypt( pPlaintextData, cbPlaintextData, pIV, pEncryptedDataAndTag, pcbEncryptedDataAndTag, pAdditionalAuthenticationData, cbAuthenticationData );
}

//-----------------------------------------------------------------------------
bool CCrypto::SymmetricAuthDecryptWithIV(
	const void *pEncryptedDataAndTag, size_t cbEncryptedDataAndTag,
	const void *pIV, size_t cbIV,
	void *pPlaintextData, uint32 *pcbPlaintextData,
	const void *pKey, size_t cbKey,
	const void *pAdditionalAuthenticationData, size_t cbAuthenticationData,
	size_t cbTag
) {
	// Setup a context.  If you are going to be decrypting many buffers with the same parameters,
	// you should create a context and reuse it, to avoid this setup cost
	AES_GCM_DecryptContext ctx;
	if ( !ctx.Init( pKey, cbKey, cbIV, cbTag ) )
		return false;

	// Decrypt it, and cleanup
	return ctx.Decrypt( pEncryptedDataAndTag, cbEncryptedDataAndTag, pIV, pPlaintextData, pcbPlaintextData, pAdditionalAuthenticationData, cbAuthenticationData );
}
