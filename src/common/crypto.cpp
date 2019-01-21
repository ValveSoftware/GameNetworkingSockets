//========= Copyright Valve LLC, All rights reserved. ========================


// Note: not using precompiled headers! This file is included directly by
// several different projects and may include Crypto++ headers depending
// on compile-time defines, which in turn pulls in other odd dependencies

#if defined(_WIN32)
#ifdef __MINGW32__
// x86intrin.h gets included by MinGW's winnt.h, so defining this avoids
// redefinition of AES-NI intrinsics below
#define _X86INTRIN_H_INCLUDED
#endif
#include "winlite.h"
#elif defined(POSIX)
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <tier0/vprof.h>
#include <tier1/utlmemory.h>
#include "crypto.h"

#include "tier0/memdbgoff.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include "tier0/memdbgon.h"

#include "opensslwrapper.h"

void OneTimeCryptoInitOpenSSL()
{
	static bool once;
	if ( !once )
	{
		once = true; // Not thread-safe
		COpenSSLWrapper::Initialize();
		atexit( &COpenSSLWrapper::Shutdown );
	}
}

// Allocate a EVP_CIPHER_CTX, and clean it up securely on scope exit
// using RAII
struct EVP_CIPHER_CTX_safe
{
//	#if OPENSSL_API_LEVEL < 2
//
//		// Nice, we can allocate on the stack, super simple and fast
//		EVP_CIPHER_CTX_safe() { EVP_CIPHER_CTX_init( &ctx ); }
//		~EVP_CIPHER_CTX_safe() { EVP_CIPHER_CTX_cleanup( &ctx ); }
//		inline EVP_CIPHER_CTX *Ptr() { return &ctx; } 
//		EVP_CIPHER_CTX ctx;
//	#else

		// Ug, we have to go through a generic allocator!  What the heck
		// guys, we need a way to do this efficiently!  I don't want to be
		// doing heap allocations just to encrypt 1000 bytes!  Do they
		// expect you to use a thread-local allocation and reuse it?
		EVP_CIPHER_CTX_safe() { ctx = EVP_CIPHER_CTX_new(); }
		~EVP_CIPHER_CTX_safe() { EVP_CIPHER_CTX_free( ctx ); }
		inline EVP_CIPHER_CTX *Ptr() { return ctx; } 
		EVP_CIPHER_CTX *ctx;
//	#endif
};

void CCrypto::Init()
{
	OneTimeCryptoInitOpenSSL();
}

template < typename CTXType, void(*CleanupFunc)(CTXType)>
class EVPCTXPointer
{
public:
	CTXType ctx;

	EVPCTXPointer() { this->ctx = NULL; }
	EVPCTXPointer(CTXType ctx) { this->ctx = ctx; }
	~EVPCTXPointer() { CleanupFunc(ctx); }
};

static bool SymmetricEncryptHelper( const uint8 *pubPlaintextData, const uint32 cubPlaintextData_, 
									const uint8 *pIV, const uint32 cubIV,
									uint8 *pubEncryptedData, uint32 *pcubEncryptedData,
									const uint8 *pubKey, const uint32 cubKey, bool bWriteIV )
{
	uint32 cubPlaintextData = cubPlaintextData_;

	VPROF_BUDGET( "CCrypto::SymmetricEncrypt", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pubPlaintextData );
	Assert( cubPlaintextData );
	Assert( pIV );
	Assert( cubIV >= k_nSymmetricBlockSize );
	Assert( pubEncryptedData );
	Assert( pcubEncryptedData );
	Assert( *pcubEncryptedData );
	Assert( pubKey );
	Assert( k_nSymmetricKeyLen256 == cubKey || k_nSymmetricKeyLen128 == cubKey );

	uint32 cubEncryptedData = *pcubEncryptedData;	// remember how big the caller's buffer is

	// Output space required = IV block + encrypted data with padding
	int nPaddingLength = 16 - ( cubPlaintextData & 15 );
	uint32 cubTotalOutput = ( bWriteIV ? k_nSymmetricBlockSize : 0 ) + cubPlaintextData + nPaddingLength;
	Assert( cubEncryptedData >= cubTotalOutput );
	if ( cubEncryptedData < cubTotalOutput )
		return false;


	const EVP_CIPHER *cipher = NULL;
	switch(cubKey * 8) {
		case 128: cipher = EVP_aes_128_cbc(); break;
		case 256: cipher = EVP_aes_256_cbc(); break;
	}

	if (!cipher)
		return false;

	EVP_CIPHER_CTX_safe ctx;
	if (EVP_EncryptInit_ex( ctx.Ptr(), cipher, NULL, pubKey, pIV) != 1)
		return false;

	int ciphertext_len, len;

	if (EVP_EncryptUpdate(ctx.Ptr(), pubEncryptedData, &len, pubPlaintextData, cubPlaintextData) != 1)
		return false;
	ciphertext_len = len;

	if (EVP_EncryptFinal(ctx.Ptr(), pubEncryptedData + len, &len) != 1)
		return false;
	ciphertext_len += len;

	*pcubEncryptedData = ciphertext_len;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Encrypts the specified data with the specified key and IV.  Uses AES (Rijndael) symmetric
//			encryption.  The encrypted data may then be decrypted by calling SymmetricDecryptWithIV
//			with the same key and IV.
// Input:	pubPlaintextData -	Data to be encrypted
//			cubPlaintextData -	Size of data to be encrypted
//			pIV				 -	Pointer to initialization vector
//			cubIV			 -	Size of initialization vector
//			pubEncryptedData -  Pointer to buffer to receive encrypted data
//			pcubEncryptedData - Pointer to a variable that at time of call contains the size of
//								the receive buffer for encrypted data.  When the method returns, this will contain
//								the actual size of the encrypted data.
//			pubKey -			the key to encrypt the data with
//			cubKey -			Size of the key (must be k_nSymmetricKeyLen)
// Output:  true if successful, false if encryption failed
//-----------------------------------------------------------------------------

bool CCrypto::SymmetricEncryptWithIV( const uint8 * pubPlaintextData, uint32 cubPlaintextData,
	const uint8 * pIV, uint32 cubIV, uint8 * pubEncryptedData,
	uint32 * pcubEncryptedData, const uint8 * pubKey, uint32 cubKey )
{
	return SymmetricEncryptHelper( pubPlaintextData, cubPlaintextData, pIV, cubIV, pubEncryptedData, pcubEncryptedData, pubKey, cubKey, false /*no prepended IV*/ );
}


// Local helper to perform AES+CBC decryption using optimized OpenSSL AES routines
static bool BDecryptAESUsingOpenSSL( const uint8 *pubEncryptedData,
		uint32 cubEncryptedData, uint8 *pubPlaintextData, uint32 *pcubPlaintextData,
		const uint8 *pubKey, const uint32 cubKey, const uint8 *pIV,
		bool bVerifyPaddingBytes = true )
{
	COMPILE_TIME_ASSERT( k_nSymmetricBlockSize == 16 );

	// Block cipher encrypted text must be a multiple of the block size
	if ( cubEncryptedData % k_nSymmetricBlockSize != 0 )
		return false;

	// Enough input? Requirement is one padded final block
	if ( cubEncryptedData < k_nSymmetricBlockSize )
		return false;

	// Enough output space for all the full non-final blocks?
	if ( *pcubPlaintextData < cubEncryptedData - k_nSymmetricBlockSize )
		return false;

	const EVP_CIPHER *cipher = NULL;
	switch(cubKey * 8) {
		case 128: cipher = EVP_aes_128_cbc(); break;
		case 256: cipher = EVP_aes_256_cbc(); break;
	}

	if (!cipher)
		return false;

	EVP_CIPHER_CTX_safe ctx;
	if (EVP_DecryptInit_ex(ctx.Ptr(), cipher, NULL, pubKey, pIV) != 1)
		return false;

	int plaintext_len, len;

	if (EVP_DecryptUpdate(ctx.Ptr(), pubPlaintextData, &len, pubEncryptedData, cubEncryptedData) != 1)
		return false;
	plaintext_len = len;

	if (EVP_DecryptFinal(ctx.Ptr(), pubPlaintextData + plaintext_len, &len) != 1)
		return false;
	plaintext_len += len;

	*pcubPlaintextData = plaintext_len;

	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Decrypts the specified data with the specified key.  Uses AES (Rijndael) symmetric
//			decryption.  
// Input:	pubEncryptedData -	Data to be decrypted
//			cubEncryptedData -	Size of data to be decrypted
//			pIV -				Initialization vector. Byte array one block in size.
//			cubIV -				size of IV. This should be 16 (one block, 128 bits)
//			pubPlaintextData -  Pointer to buffer to receive decrypted data
//			pcubPlaintextData - Pointer to a variable that at time of call contains the size of
//								the receive buffer for decrypted data.  When the method returns, this will contain
//								the actual size of the decrypted data.
//			pubKey -			the key to decrypt the data with
//			cubKey -			Size of the key (must be k_nSymmetricKeyLen)
// Output:  true if successful, false if decryption failed
//-----------------------------------------------------------------------------
bool CCrypto::SymmetricDecryptWithIV( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
								const uint8 * pIV, uint32 cubIV,
								uint8 *pubPlaintextData, uint32 *pcubPlaintextData, 
								const uint8 *pubKey, const uint32 cubKey, bool bVerifyPaddingBytes )
{
	Assert( pubEncryptedData );
	Assert( cubEncryptedData);
	Assert( pIV );
	Assert( cubIV );
	Assert( pubPlaintextData );
	Assert( pcubPlaintextData );
	Assert( *pcubPlaintextData );
	Assert( pubKey );
	Assert( k_nSymmetricKeyLen256 == cubKey || k_nSymmetricKeyLen128 == cubKey );

	// IV input into CBC must be exactly one block size
	if ( cubIV != k_nSymmetricBlockSize )
		return false;

	return BDecryptAESUsingOpenSSL( pubEncryptedData, cubEncryptedData, pubPlaintextData, pcubPlaintextData, pubKey, cubKey, pIV, bVerifyPaddingBytes );
}

SymmetricCryptContextBase::SymmetricCryptContextBase()
{
	evp_cipher_ctx = nullptr;
	m_cbIV = 0;
	m_cbTag = 0;
}

void SymmetricCryptContextBase::Wipe()
{
	if ( evp_cipher_ctx )
	{
		EVP_CIPHER_CTX *ctx = (EVP_CIPHER_CTX*)evp_cipher_ctx;
		#if OPENSSL_VERSION_NUMBER < 0x10100000
			EVP_CIPHER_CTX_cleanup( ctx );
			delete ctx;
		#else
			EVP_CIPHER_CTX_free( ctx );
		#endif
		evp_cipher_ctx = nullptr;
	}
	m_cbIV = 0;
	m_cbTag = 0;
}

bool AES_GCM_CipherContext::InitCipher( const void *pKey, size_t cbKey, size_t cbIV, size_t cbTag, bool bEncrypt )
{
	EVP_CIPHER_CTX *ctx = (EVP_CIPHER_CTX*)evp_cipher_ctx;
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
		evp_cipher_ctx = ctx;
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
	EVP_CIPHER_CTX *ctx = (EVP_CIPHER_CTX*)evp_cipher_ctx;
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

	EVP_CIPHER_CTX *ctx = (EVP_CIPHER_CTX*)evp_cipher_ctx;
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

//-----------------------------------------------------------------------------
// Purpose: Generate a SHA256 hash
// Input:	pchInput -			Plaintext string of item to hash (null terminated)
//			pOutDigest -		Pointer to receive hashed digest output
//-----------------------------------------------------------------------------
void CCrypto::GenerateSHA256Digest( const uint8 *pubInput, const int cubInput, SHA256Digest_t *pOutDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateSHA256Digest", VPROF_BUDGETGROUP_ENCRYPTION );
	//Assert( pubInput );
	Assert( cubInput >= 0 );
	Assert( pOutDigest );

	EVPCTXPointer<EVP_MD_CTX *, EVP_MD_CTX_free> ctx(EVP_MD_CTX_create());

	unsigned int digest_len = sizeof(SHA256Digest_t);
	VerifyFatal(ctx.ctx != NULL);
	VerifyFatal(EVP_DigestInit_ex(ctx.ctx, EVP_sha256(), NULL) == 1);
	VerifyFatal(EVP_DigestUpdate(ctx.ctx, pubInput, cubInput) == 1);
	VerifyFatal(EVP_DigestFinal(ctx.ctx, *pOutDigest, &digest_len) == 1);
}

//-----------------------------------------------------------------------------
// Purpose: Generates a cryptographiacally random block of data fit for any use.
// NOTE: Function terminates process on failure rather than returning false!
//-----------------------------------------------------------------------------
void CCrypto::GenerateRandomBlock( void *pvDest, int cubDest )
{
	VPROF_BUDGET( "CCrypto::GenerateRandomBlock", VPROF_BUDGETGROUP_ENCRYPTION );
	AssertFatal( cubDest >= 0 );
	uint8 *pubDest = (uint8 *)pvDest;

#if defined(_WIN32)

	// NOTE: assume that this cannot fail. MS has baked this function name into
	// static CRT runtime libraries for years; changing the export would break 
	// millions of applications. Available from Windows XP onward. -henryg
	typedef BYTE ( NTAPI *PfnRtlGenRandom_t )( PVOID RandomBuffer, ULONG RandomBufferLength );
	static PfnRtlGenRandom_t s_pfnRtlGenRandom;
	if ( !s_pfnRtlGenRandom )
	{
		s_pfnRtlGenRandom = (PfnRtlGenRandom_t) (void*) GetProcAddress( LoadLibraryA( "advapi32.dll" ), "SystemFunction036" );
	}

	bool bRtlGenRandomOK = s_pfnRtlGenRandom && ( s_pfnRtlGenRandom( pubDest, (unsigned long)cubDest ) == TRUE );
	AssertFatal( bRtlGenRandomOK );

#elif defined(POSIX)

	// Reading from /dev/urandom is threadsafe, but possibly slow due to a kernel
	// spinlock or mutex protecting access to the internal PRNG state. In theory,
	// we could use bytes from /dev/urandom to initialize a user-space PRNG (like
	// OpenSSL does), but this introduces a security risk: if our process memory
	// is compromised and an attacker gains read access, the PRNG state could be
	// dumped and an attacker might be able to predict future or past outputs!
	// The risk of the kernel's internal PRNG state being exposed is much lower.
	// (We can revisit this if performance becomes an issue. -henryg 4/26/2016)
	static int s_dev_urandom_fd = open( "/dev/urandom", O_RDONLY | O_CLOEXEC );
	AssertFatal( s_dev_urandom_fd >= 0 );
	size_t remaining = (size_t)cubDest;
	while ( remaining )
	{
		ssize_t urandom_result = read( s_dev_urandom_fd, pubDest + cubDest - remaining, remaining );
		AssertFatal( urandom_result > 0 || ( urandom_result < 0 && errno == EINTR ) );
		if ( urandom_result > 0 )
		{
			remaining -= urandom_result;
		}
	}

#else

	OneTimeCryptoInitOpenSSL();
	AssertFatal( RAND_bytes( pubDest, cubDest ) > 0 );

#endif
}


//-----------------------------------------------------------------------------
// Purpose: Generate a keyed-hash MAC using SHA-256
//-----------------------------------------------------------------------------
void CCrypto::GenerateHMAC256( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHA256Digest_t *pOutputDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateHMAC256", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pubData );
	Assert( cubData > 0 );
	Assert( pubKey );
	Assert( cubKey > 0 );
	Assert( pOutputDigest );

	EVPCTXPointer<EVP_MD_CTX *, EVP_MD_CTX_free> mdctx(EVP_MD_CTX_create());
	EVPCTXPointer<EVP_PKEY *, EVP_PKEY_free> pkey(EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, pubKey, cubKey));
	const EVP_MD *digest = EVP_sha256();

	VerifyFatal(mdctx.ctx != NULL && pkey.ctx != NULL);
	VerifyFatal(EVP_DigestInit_ex(mdctx.ctx, digest, NULL) == 1);
	VerifyFatal(EVP_DigestSignInit(mdctx.ctx, NULL, digest, NULL, pkey.ctx) == 1);
	VerifyFatal(EVP_DigestSignUpdate(mdctx.ctx, pubData, cubData) == 1);

	size_t needed = sizeof(SHA256Digest_t);
	VerifyFatal(EVP_DigestSignFinal(mdctx.ctx, *pOutputDigest, &needed) == 1);
}
