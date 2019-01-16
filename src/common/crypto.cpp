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
#include "tier0/memdbgon.h"

#include "opensslwrapper.h"

typedef const EVP_CIPHER *(*EVP_cipherdef_t)(void);

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

void CCrypto::Init()
{
	OneTimeCryptoInitOpenSSL();
}

template < typename CTXType, void(*CleanupFunc)(CTXType)>
class EVPCTXPointer
{
public:
	CTXType ctx;

	EVPCTXPointer()
	{
		this->ctx = NULL;
	}

	EVPCTXPointer(CTXType ctx)
	{
		this->ctx = ctx;
	}

	~EVPCTXPointer()
	{
		CleanupFunc(ctx);
	}
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

	EVPCTXPointer<EVP_CIPHER_CTX *, EVP_CIPHER_CTX_free> ctx(EVP_CIPHER_CTX_new());

	if (!ctx.ctx)
		return false;

	EVP_cipherdef_t cipher = NULL;
	switch(cubKey * 8) {
		case 128: cipher = EVP_aes_128_cbc; break;
		case 256: cipher = EVP_aes_256_cbc; break;
	}

	if (!cipher)
		return false;

	if (EVP_EncryptInit_ex(ctx.ctx, cipher(), NULL, pubKey, pIV) != 1)
		return false;

	int ciphertext_len, len;

	if (EVP_EncryptUpdate(ctx.ctx, pubEncryptedData, &len, pubPlaintextData, cubPlaintextData) != 1)
		return false;
	ciphertext_len = len;

	if (EVP_EncryptFinal(ctx.ctx, pubEncryptedData + len, &len) != 1)
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

	EVPCTXPointer<EVP_CIPHER_CTX *, EVP_CIPHER_CTX_free> ctx(EVP_CIPHER_CTX_new());

	if (!ctx.ctx)
		return false;

	EVP_cipherdef_t cipher = NULL;
	switch(cubKey * 8) {
		case 128: cipher = EVP_aes_128_cbc; break;
		case 256: cipher = EVP_aes_256_cbc; break;
	}

	if (!cipher)
		return false;

	if (EVP_DecryptInit_ex(ctx.ctx, cipher(), NULL, pubKey, pIV) != 1)
		return false;

	int plaintext_len, len;

	if (EVP_DecryptUpdate(ctx.ctx, pubPlaintextData, &len, pubEncryptedData, cubEncryptedData) != 1)
		return false;
	plaintext_len = len;

	if (EVP_DecryptFinal(ctx.ctx, pubPlaintextData + plaintext_len, &len) != 1)
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
