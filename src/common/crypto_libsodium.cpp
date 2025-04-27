#include "crypto.h"

#ifdef VALVE_CRYPTO_LIBSODIUM

#include <tier0/vprof.h>
#include <tier0/dbg.h>

#include "tier0/memdbgoff.h"
#include <sodium/core.h>
#include <sodium/crypto_aead_aes256gcm.h>
#include <sodium/crypto_aead_chacha20poly1305.h>
#include <sodium/crypto_auth_hmacsha256.h>
#include <sodium/crypto_hash_sha256.h>
#include <sodium/randombytes.h>
#include <sodium/utils.h>
#include "tier0/memdbgon.h"

SymmetricCryptContextBase::SymmetricCryptContextBase()
	: m_ctx(nullptr), m_cbIV(0), m_cbTag(0)
{
}

void SymmetricCryptContextBase::Wipe()
{
	if ( m_ctx )
	{
		sodium_free(m_ctx);
		m_ctx = nullptr;
	}
	m_cbIV = 0;
	m_cbTag = 0;
}

bool AES_GCM_CipherContext::InitCipher( const void *pKey, size_t cbKey, size_t cbIV, size_t cbTag, bool bEncrypt )
{
	// Libsodium requires AES and CLMUL instructions for AES-GCM, available in
	// Intel "Westmere" and up. 90.41% of Steam users have this as of the
	// November 2019 survey.
	// Libsodium recommends ChaCha20-Poly1305 in software if you've not got AES support
	// in hardware.
	if ( crypto_aead_aes256gcm_is_available() != 1 )
	{
		AssertMsg( false, "No hardware AES support on this CPU." );
		return false;
	}
	if ( cbKey != crypto_aead_aes256gcm_KEYBYTES )
	{
		AssertMsg( false, "AES key sizes other than 256 are unsupported." );
		return false;
	}
	if ( cbIV != crypto_aead_aes256gcm_NPUBBYTES )
	{
		AssertMsg( false, "Nonce size is unsupported" );
		return false;
	}

	Wipe();

	m_ctx = sodium_malloc( sizeof(crypto_aead_aes256gcm_state) );

	if ( crypto_aead_aes256gcm_beforenm( static_cast<crypto_aead_aes256gcm_state*>( m_ctx ), static_cast<const unsigned char*>( pKey ) ) != 0 )
	{
		AssertMsg( false, "crypto_aead_aes256gcm_beforenm failed" ); // docs say this "should never happen"
		return false;
	}

	m_cbIV = cbIV;
	m_cbTag = crypto_aead_aes256gcm_ABYTES;
	COMPILE_TIME_ASSERT( crypto_aead_aes256gcm_ABYTES == 16 );

	return true;
}

bool AES_GCM_CipherContext::IsAvailable()
{
	// Libsodium requires AES and CLMUL instructions for AES-GCM, available in
	// Intel "Westmere" and up. 90.41% of Steam users have this as of the
	// November 2019 survey.
	// Libsodium recommends ChaCha20-Poly1305 in software if you've not got AES support
	// in hardware.
	return crypto_aead_aes256gcm_is_available() == 1;
}

bool AES_GCM_EncryptContext::Encrypt(
		const void *pPlaintextData, size_t cbPlaintextData,
		const void *pIV,
		void *pEncryptedDataAndTag, uint32 *pcbEncryptedDataAndTag,
		const void *pAdditionalAuthenticationData, size_t cbAuthenticationData
		)
{

	// Make sure caller's buffer is big enough to hold the result.
	if ( cbPlaintextData + crypto_aead_aes256gcm_ABYTES > *pcbEncryptedDataAndTag )
	{
		*pcbEncryptedDataAndTag = 0;
		return false;
	}

	unsigned long long cbEncryptedDataAndTag_longlong;
	if ( crypto_aead_aes256gcm_encrypt_afternm(
			static_cast<unsigned char*>( pEncryptedDataAndTag ), &cbEncryptedDataAndTag_longlong,
			static_cast<const unsigned char*>( pPlaintextData ), cbPlaintextData,
			static_cast<const unsigned char*>(pAdditionalAuthenticationData), cbAuthenticationData,
			nullptr,
			static_cast<const unsigned char*>( pIV ),
			static_cast<const crypto_aead_aes256gcm_state*>( m_ctx )
			) != 0
	) {
		AssertMsg( false, "crypto_aead_aes256gcm_encrypt_afternm failed" ); // docs say this "should never happen"
		*pcbEncryptedDataAndTag = 0;
		return false;
	}

	*pcbEncryptedDataAndTag = cbEncryptedDataAndTag_longlong;

	return true;
}

bool AES_GCM_DecryptContext::Decrypt(
		const void *pEncryptedDataAndTag, size_t cbEncryptedDataAndTag,
		const void *pIV,
		void *pPlaintextData, uint32 *pcbPlaintextData,
		const void *pAdditionalAuthenticationData, size_t cbAuthenticationData
		)
{
	// Make sure caller's buffer is big enough to hold the result
	if ( cbEncryptedDataAndTag > *pcbPlaintextData + crypto_aead_aes256gcm_ABYTES )
	{
		*pcbPlaintextData = 0;
		return false;
	}

	unsigned long long cbPlaintextData_longlong = 0;
	const int nDecryptResult = crypto_aead_aes256gcm_decrypt_afternm(
			static_cast<unsigned char*>( pPlaintextData ), &cbPlaintextData_longlong,
			nullptr,
			static_cast<const unsigned char*>( pEncryptedDataAndTag ), cbEncryptedDataAndTag,
			static_cast<const unsigned char*>( pAdditionalAuthenticationData ), cbAuthenticationData,
			static_cast<const unsigned char*>( pIV ), static_cast<const crypto_aead_aes256gcm_state*>( m_ctx )
			);

	*pcbPlaintextData = cbPlaintextData_longlong;

	return nDecryptResult == 0;
}

/// This implementation uses the IETF variant of the ChaCha20-Poly1305 algorithm from libsodium.
/// For more information, please see https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305/ietf_chacha20-poly1305_construction
bool ChaCha20_Poly1305_CipherContext::InitCipher(const void* pKey, size_t cbKey, size_t cbIV, size_t cbTag, bool bEncrypt)
{
	if (cbKey != crypto_aead_chacha20poly1305_ietf_KEYBYTES)
	{
		AssertMsg(false, "ChaCha20-Poly1305-IETF key sizes other than %d are unsupported.", crypto_aead_chacha20poly1305_ietf_KEYBYTES);
		return false;
	}
	if (cbIV != crypto_aead_chacha20poly1305_ietf_NPUBBYTES)
	{
		AssertMsg(false, "Nonce size is unsupported");
		return false;
	}

	Wipe();

	if (pKey == nullptr)
	{
		AssertMsg(false, "Invalid secret key");
		return false;
	}

	m_ctx = sodium_malloc(cbKey);
	memcpy(m_ctx, pKey, cbKey);

	m_cbIV = cbIV;
	m_cbTag = crypto_aead_chacha20poly1305_ietf_ABYTES;
	COMPILE_TIME_ASSERT(crypto_aead_chacha20poly1305_ietf_ABYTES == 16);

	return true;
}

bool ChaCha20_Poly1305_CipherContext::IsAvailable()
{
	return true;
}

bool ChaCha20_Poly1305_EncryptContext::Encrypt(
		const void* pPlaintextData, size_t cbPlaintextData,
		const void* pIV,
		void* pEncryptedDataAndTag, uint32* pcbEncryptedDataAndTag,
		const void* pAdditionalAuthenticationData, size_t cbAuthenticationData
		)
{

	// Make sure caller's buffer is big enough to hold the result.
	if (cbPlaintextData + crypto_aead_chacha20poly1305_ietf_ABYTES > *pcbEncryptedDataAndTag)
	{
		*pcbEncryptedDataAndTag = 0;
		return false;
	}

	unsigned long long cbEncryptedDataAndTag_longlong;
	if (crypto_aead_chacha20poly1305_ietf_encrypt(
		static_cast<unsigned char*>(pEncryptedDataAndTag), &cbEncryptedDataAndTag_longlong,
		static_cast<const unsigned char*>(pPlaintextData), cbPlaintextData,
		static_cast<const unsigned char*>(pAdditionalAuthenticationData), cbAuthenticationData,
		nullptr,
		static_cast<const unsigned char*>(pIV),
		static_cast<const unsigned char*>(m_ctx)
	) != 0
		) {
		AssertMsg(false, "crypto_aead_chacha20poly1305_ietf_encrypt failed"); // docs say this "should never happen"
		*pcbEncryptedDataAndTag = 0;
		return false;
	}

	*pcbEncryptedDataAndTag = cbEncryptedDataAndTag_longlong;

	return true;
}

bool ChaCha20_Poly1305_DecryptContext::Decrypt(
		const void* pEncryptedDataAndTag, size_t cbEncryptedDataAndTag,
		const void* pIV,
		void* pPlaintextData, uint32* pcbPlaintextData,
		const void* pAdditionalAuthenticationData, size_t cbAuthenticationData
		)
{
	// Make sure caller's buffer is big enough to hold the result
	if (cbEncryptedDataAndTag > *pcbPlaintextData + crypto_aead_chacha20poly1305_ietf_ABYTES)
	{
		*pcbPlaintextData = 0;
		return false;
	}

	unsigned long long cbPlaintextData_longlong = 0;
	const int nDecryptResult = crypto_aead_chacha20poly1305_ietf_decrypt(
		static_cast<unsigned char*>(pPlaintextData), &cbPlaintextData_longlong,
		nullptr,
		static_cast<const unsigned char*>(pEncryptedDataAndTag), cbEncryptedDataAndTag,
		static_cast<const unsigned char*>(pAdditionalAuthenticationData), cbAuthenticationData,
		static_cast<const unsigned char*>(pIV), static_cast<const unsigned char*>(m_ctx)
	);

	*pcbPlaintextData = cbPlaintextData_longlong;

	return nDecryptResult == 0;
}

void CCrypto::Init()
{
	// sodium_init is safe to call multiple times from multiple threads
	// so no need to do anything clever here.
	if(sodium_init() < 0)
	{
		AssertMsg( false, "libsodium didn't init" );
	}
}

void CCrypto::GenerateRandomBlock( void *pubDest, int cubDest )
{
	VPROF_BUDGET( "CCrypto::GenerateRandomBlock", VPROF_BUDGETGROUP_ENCRYPTION );
	AssertFatal( cubDest >= 0 );

	randombytes_buf( pubDest, cubDest );
}

void CCrypto::GenerateSHA256Digest( const void *pData, size_t cbData, SHA256Digest_t *pOutputDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateSHA256Digest", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pData );
	Assert( pOutputDigest );

	crypto_hash_sha256( *pOutputDigest, static_cast<const unsigned char*>(pData), cbData );
}

void CCrypto::GenerateHMAC256( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHA256Digest_t *pOutputDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateHMAC256", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pubData );
	Assert( cubData > 0 );
	Assert( pubKey );
	Assert( cubKey > 0 );
	Assert( pOutputDigest );

	Assert( sizeof(*pOutputDigest) == crypto_auth_hmacsha256_BYTES );
	Assert( cubKey == crypto_auth_hmacsha256_KEYBYTES );

	crypto_auth_hmacsha256( *pOutputDigest, pubData, cubData, pubKey );
}

#endif
