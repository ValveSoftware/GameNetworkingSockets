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
#include <openssl/err.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/pem.h>
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

void CCrypto::Init()
{
	OneTimeCryptoInitOpenSSL();
}

void DispatchOpenSSLErrors( const char * contextMessage )
{
	char buf[1024];
	while ( unsigned long e = ERR_get_error() )
	{
		ERR_error_string_n( e, buf, sizeof( buf ) );
		buf[ sizeof(buf) - 1 ] = '\0';
//SDR_PUBLIC		DMsg( SPEW_CRYPTO, 2, "OpenSSL error (%s): %s\n", contextMessage, buf );
	}
}

// AESNI support
#if defined(_M_IX86) || defined (_M_X64) || defined(__i386__) || defined(__x86_64__)
#define ENABLE_AESNI_INSTRINSIC_PATH 1
#include <emmintrin.h>
// stupid: can't use wmmintrin.h on gcc/clang, requires AESNI codegen to be globally enabled!
#if defined(__clang__) || defined(__GNUC__)
static FORCEINLINE __attribute__((gnu_inline)) __m128i _mm_aesenc_si128( __m128i a, __m128i b ) { asm( "aesenc %1, %0" : "+x"(a) : "x"(b) ); return a; }
static FORCEINLINE __attribute__((gnu_inline)) __m128i _mm_aesenclast_si128( __m128i a, __m128i b ) { asm( "aesenclast %1, %0" : "+x"(a) : "x"(b) ); return a; }
static FORCEINLINE __attribute__((gnu_inline)) __m128i _mm_aesdec_si128( __m128i a, __m128i b ) { asm( "aesdec %1, %0" : "+x"(a) : "x"(b) ); return a; }
static FORCEINLINE __attribute__((gnu_inline)) __m128i _mm_aesdeclast_si128( __m128i a, __m128i b ) { asm( "aesdeclast %1, %0" : "+x"(a) : "x"(b) ); return a; }
#else
#include <wmmintrin.h>
#endif
#endif

// OpenSSL AES_KEY structure details are marked "internal", sometimes big-endian
// even on x86/x64 depending on platform+compiler+options used to build OpenSSL
static bool BExtractAESRoundKeys( const AES_KEY *key, bool bDecrypt, uint32 (&rgRoundKeysAsU32)[15*4], int *pnRounds )
{
	// 0 = untested, 1 = no endian swap, 2 = endian swap, -1 = UNKNOWN KEY SETUP
	static int8 s_nDetectPlatformKeyFormat[2];
	if ( s_nDetectPlatformKeyFormat[0] == 0 )
	{
		unsigned char rgchTest[32] = { };
		AES_KEY keyTest;

		AES_set_encrypt_key( rgchTest, 256, &keyTest );
		DbgAssert( keyTest.rounds == 14 && (keyTest.rd_key[59] == 0x856370CBu || keyTest.rd_key[59] == 0xCB706385u) );
		if ( keyTest.rounds == 14 && keyTest.rd_key[59] == 0x856370CBu )
			s_nDetectPlatformKeyFormat[0] = 1;
		else if ( keyTest.rounds == 14 && keyTest.rd_key[59] == 0xCB706385u )
			s_nDetectPlatformKeyFormat[0] = 2;
		else
			s_nDetectPlatformKeyFormat[0] = -1;

		AES_set_decrypt_key( rgchTest, 256, &keyTest );
		DbgAssert( keyTest.rounds == 14 && ( keyTest.rd_key[0] == 0x170AF810u || keyTest.rd_key[0] == 0x10F80A17u ) );
		if ( keyTest.rounds == 14 && keyTest.rd_key[0] == 0x170AF810u )
			s_nDetectPlatformKeyFormat[1] = 1;
		else if ( keyTest.rounds == 14 && keyTest.rd_key[0] == 0x10F80A17u )
			s_nDetectPlatformKeyFormat[1] = 2;
		else
			s_nDetectPlatformKeyFormat[1] = -1;
	}

	if ( ( key->rounds != 10 && key->rounds != 14 ) || s_nDetectPlatformKeyFormat[bDecrypt] <= 0 )
		return false;

	*pnRounds = key->rounds;
	int nWords = (key->rounds + 1)*4;

	if ( s_nDetectPlatformKeyFormat[bDecrypt] == 1 )
	{
		for ( int i = 0; i < nWords; ++i )
			rgRoundKeysAsU32[i] = key->rd_key[i];
	}
	else
	{
		for ( int i = 0; i < nWords; ++i )
			rgRoundKeysAsU32[i] = DWordSwap( key->rd_key[i] );
	}
	return true;
}

// Zero-alloc HMAC implementation with memcpy-able local state;
// the OpenSSL and CryptoPP HMAC implmentations are less efficient
template < typename HashImpl >
class CHMACImplT
{
public:
	typedef typename HashImpl::Context_t HashContext_t;
	typedef typename HashImpl::Digest_t HashDigest_t;

	~CHMACImplT()
	{
		SecureZeroMemory( this, sizeof( *this ) );
	}

	void InitFrom( const CHMACImplT<HashImpl>& init )
	{
		V_memcpy( this, &init, sizeof( *this ) );
	}

	void Init( const void *pubKey, uint32 cubKey )
	{
		uint8 rgubKey[kBlockSizeBytes], rgubBuf[kBlockSizeBytes];
		memset( rgubKey, 0, sizeof( rgubKey ) );

		if ( cubKey > kBlockSizeBytes )
		{
			HashDigest_t digest;
			HashImpl::Init( &m_ctx1 );
			HashImpl::Update( &m_ctx1, (const uint8*)pubKey, cubKey );
			HashImpl::Final( digest, &m_ctx1 );
			COMPILE_TIME_ASSERT( sizeof( digest ) <= kBlockSizeBytes );
			V_memcpy( rgubKey, digest, sizeof( digest ) );
			SecureZeroMemory( digest, sizeof(digest) );
		}
		else
		{
			V_memcpy( rgubKey, pubKey, cubKey );
		}

		HashImpl::Init( &m_ctx1 );
		HashImpl::Init( &m_ctx2 );

		for ( int i = 0; i < kBlockSizeBytes; ++i )
			rgubBuf[i] = rgubKey[i] ^ 0x36;
		HashImpl::Update( &m_ctx1, rgubBuf, kBlockSizeBytes );

		for ( int i = 0; i < kBlockSizeBytes; ++i )
			rgubBuf[i] = rgubKey[i] ^ 0x5c;
		HashImpl::Update( &m_ctx2, rgubBuf, kBlockSizeBytes );

		SecureZeroMemory( rgubBuf, kBlockSizeBytes );
		SecureZeroMemory( rgubKey, kBlockSizeBytes );
	}

	void Update( const void *pubData, uint32 cubData )
	{
		HashImpl::Update( &m_ctx1, pubData, cubData );
	}

	void Final( HashDigest_t &out )
	{
		HashImpl::Final( out, &m_ctx1 );
		HashImpl::Update( &m_ctx2, out, sizeof( HashDigest_t ) );
		HashImpl::Final( out, &m_ctx2 );
	}

private:
	enum { kBlockSizeBytes = 64 };
	HashContext_t m_ctx1;
	HashContext_t m_ctx2;
};

struct HMACPolicy_MD5
{
	typedef MD5_CTX Context_t;
	typedef uint8 Digest_t[16];
	static void Init( Context_t *c ) { MD5_Init( c ); }
	static void Update( Context_t *c, const void *p, size_t n ) { MD5_Update( c, p, n ); }
	static void Final( Digest_t &out, Context_t *c ) { MD5_Final( out, c ); }
};
typedef CHMACImplT<HMACPolicy_MD5> CHMACMD5Impl;

struct HMACPolicy_SHA1
{
	typedef SHA_CTX Context_t;
	typedef uint8 Digest_t[20];
	static void Init( Context_t *c ) { SHA1_Init( c ); }
	static void Update( Context_t *c, const void *p, size_t n ) { SHA1_Update( c, p, n ); }
	static void Final( Digest_t &out, Context_t *c ) { SHA1_Final( out, c ); }
};
typedef CHMACImplT<HMACPolicy_SHA1> CHMACSHA1Impl;

struct HMACPolicy_SHA256
{
	typedef SHA256_CTX Context_t;
	typedef uint8 Digest_t[32];
	static void Init( Context_t *c ) { SHA256_Init( c ); }
	static void Update( Context_t *c, const void *p, size_t n ) { SHA256_Update( c, p, n ); }
	static void Final( Digest_t &out, Context_t *c ) { SHA256_Final( out, c ); }
};
typedef CHMACImplT<HMACPolicy_SHA256> CHMACSHA256Impl;

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

	bool bRet = false;

	uint32 cubEncryptedData = *pcubEncryptedData;	// remember how big the caller's buffer is


	// Output space required = IV block + encrypted data with padding
	int nPaddingLength = 16 - ( cubPlaintextData & 15 );
	uint32 cubTotalOutput = ( bWriteIV ? k_nSymmetricBlockSize : 0 ) + cubPlaintextData + nPaddingLength;
	Assert( cubEncryptedData >= cubTotalOutput );
	if ( cubEncryptedData < cubTotalOutput )
		return false;

	uint nFullBlocks = cubPlaintextData / 16;

	AES_KEY key;
	if ( AES_set_encrypt_key( pubKey, cubKey * 8, &key ) < 0 )
		return false;

	// if overlapping but non-identical ranges, use temporary copy; we handle identical ranges
	CUtlMemory< uint8 > memTempCopy;
	if ( ( pubPlaintextData > pubEncryptedData && pubPlaintextData < pubEncryptedData + cubEncryptedData ) ||
		 ( pubEncryptedData > pubPlaintextData && pubEncryptedData < pubPlaintextData + cubPlaintextData ) )
	{
		memTempCopy.EnsureCapacity( cubPlaintextData );
		V_memcpy( memTempCopy.Base(), pubPlaintextData, cubPlaintextData );
		pubPlaintextData = memTempCopy.Base();
	}
	
	// We have to pipeline one block of plaintext because the output
	// of encryption has a one-block prefix of the encrypted IV, so
	// we are constantly writing one block ahead of the read position.

	union block_t {
		byte rgubData[16];
		uint64 u64[2];
		void Load( const void* p ) { V_memcpy( rgubData, p, 16 ); }
		void SetXor( const block_t& a, const block_t& b ) { u64[0] = a.u64[0] ^ b.u64[0]; u64[1] = a.u64[1] ^ b.u64[1]; }
		operator byte*() { return rgubData; }
		~block_t() { SecureZeroMemory( rgubData, 16 ); }
	};
	COMPILE_TIME_ASSERT( k_nSymmetricBlockSize == 16 && sizeof(block_t) == 16 );
	block_t blockNextPlaintext;
	block_t blockCBCThisBlock;
	block_t blockLastEncrypted;
	
	uint32 nNextReadSize = Min( cubPlaintextData, (uint32)k_nSymmetricBlockSize );
	V_memcpy( blockNextPlaintext, pubPlaintextData, nNextReadSize );
	pubPlaintextData += nNextReadSize;
	cubPlaintextData -= nNextReadSize;

	V_memcpy( blockLastEncrypted, pIV, k_nSymmetricBlockSize );

	if ( bWriteIV )
	{
		AES_encrypt( pIV, pubEncryptedData, &key );
		pubEncryptedData += k_nSymmetricBlockSize;
		cubEncryptedData -= k_nSymmetricBlockSize;
	}

#ifdef ENABLE_AESNI_INSTRINSIC_PATH
	// Use fast AESNI loop if possible - significantly faster than software AES
	uint32 roundKeysAsU32[15*4];
	int nRounds = 0;
	if ( GetCPUInformation().m_bAES && BExtractAESRoundKeys( &key, false, roundKeysAsU32, &nRounds ) )
	{
		__m128i workData = _mm_loadu_si128( (__m128i*)&blockLastEncrypted );
		for ( ; nFullBlocks > 0; --nFullBlocks )
		{
			workData = _mm_xor_si128( workData, _mm_loadu_si128( (__m128i*) &blockNextPlaintext ) );
			workData = _mm_xor_si128( workData, _mm_loadu_si128( (__m128i*)&roundKeysAsU32[0] ) );

			// Don't read past end of plaintext when pipelining one block ahead
			if ( cubPlaintextData >= k_nSymmetricBlockSize )
			{
				blockNextPlaintext.Load( pubPlaintextData );
				pubPlaintextData += k_nSymmetricBlockSize;
				cubPlaintextData -= k_nSymmetricBlockSize;
			}
			else
			{
				V_memcpy( blockNextPlaintext, pubPlaintextData, cubPlaintextData );
				cubPlaintextData = 0;
			}

			for ( int iRound = 1; iRound < nRounds; ++iRound )
				workData = _mm_aesenc_si128( workData, _mm_loadu_si128( (__m128i*)&roundKeysAsU32[4 * iRound] ) );
			workData = _mm_aesenclast_si128( workData, _mm_loadu_si128( (__m128i*)&roundKeysAsU32[4 * nRounds] ) );
			
			_mm_storeu_si128( (__m128i*) pubEncryptedData, workData );
			pubEncryptedData += k_nSymmetricBlockSize;
		}
		_mm_storeu_si128( (__m128i*) &blockLastEncrypted, workData );
		SecureZeroMemory( roundKeysAsU32, sizeof(roundKeysAsU32) );
	}
#endif

	for ( ; nFullBlocks > 0; --nFullBlocks )
	{
		// AES-CBC mode: XOR plaintext with preceding encrypted block (or raw IV)
		blockCBCThisBlock.SetXor( blockNextPlaintext, blockLastEncrypted );
		
		// Don't read past end of plaintext when pipelining one block ahead
		if ( cubPlaintextData >= k_nSymmetricBlockSize )
		{
			blockNextPlaintext.Load( pubPlaintextData );
			pubPlaintextData += k_nSymmetricBlockSize;
			cubPlaintextData -= k_nSymmetricBlockSize;
		}
		else
		{
			V_memcpy( blockNextPlaintext, pubPlaintextData, cubPlaintextData );
			cubPlaintextData = 0;
		}

		AES_encrypt( blockCBCThisBlock, blockLastEncrypted, &key );
		V_memcpy( pubEncryptedData, blockLastEncrypted, k_nSymmetricBlockSize );
		pubEncryptedData += k_nSymmetricBlockSize;
	}
	
	// final PKCS padded block - fill with byte values equal to number of padding bytes (may be all padding!)
	memset( (byte*)blockNextPlaintext + 16 - nPaddingLength, nPaddingLength, nPaddingLength );
	blockCBCThisBlock.SetXor( blockNextPlaintext, blockLastEncrypted );
	AES_encrypt( blockCBCThisBlock, pubEncryptedData, &key );

	SecureZeroMemory( &key, sizeof(key) );

	*pcubEncryptedData = cubTotalOutput;
	bRet = true;

	return bRet;
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
static bool BDecryptAESUsingOpenSSL( const uint8 *pubEncryptedData, uint32 cubEncryptedData, uint8 *pubPlaintextData, uint32 *pcubPlaintextData, AES_KEY *key, const uint8 *pIV, bool bVerifyPaddingBytes = true )
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

	uint8 rgubWorking[k_nSymmetricBlockSize];
	uint32 nDecrypted = 0;

	// We need to remember the last encrypted block to use as the IV for the next,
	// can't point back into pubEncryptedData because we may be operating in-place
	uint8 rgubLastEncrypted[k_nSymmetricBlockSize];

#ifdef ENABLE_AESNI_INSTRINSIC_PATH
	// 4-at-a-time AESNI instructions loop is 10-20x faster than software AES decryption
	uint32 roundKeysAsU32[15*4];
	int nRounds = 0;
	if ( GetCPUInformation().m_bAES && cubEncryptedData >= 80 && BExtractAESRoundKeys( key, true, roundKeysAsU32, &nRounds ) )
	{
		COMPILE_TIME_ASSERT( k_nSymmetricBlockSize * 4 == 64 );
		while ( nDecrypted + 63 < cubEncryptedData - k_nSymmetricBlockSize )
		{
			__m128i workData1 = _mm_loadu_si128( (__m128i*)( pubEncryptedData + nDecrypted ) );
			__m128i workData2 = _mm_loadu_si128( (__m128i*)( pubEncryptedData + nDecrypted + 16 ) );
			__m128i workData3 = _mm_loadu_si128( (__m128i*)( pubEncryptedData + nDecrypted + 32 ) );
			__m128i workData4 = _mm_loadu_si128( (__m128i*)( pubEncryptedData + nDecrypted + 48 ) );

			__m128i roundKey = _mm_loadu_si128( (__m128i*)&roundKeysAsU32[0] );
			workData1 = _mm_xor_si128( workData1, roundKey );
			workData2 = _mm_xor_si128( workData2, roundKey );
			workData3 = _mm_xor_si128( workData3, roundKey );
			workData4 = _mm_xor_si128( workData4, roundKey );
			for ( int iRound = 1; iRound < nRounds; ++iRound )
			{
				roundKey = _mm_loadu_si128( (__m128i*)&roundKeysAsU32[4 * iRound] );
				workData1 = _mm_aesdec_si128( workData1, roundKey );
				workData2 = _mm_aesdec_si128( workData2, roundKey );
				workData3 = _mm_aesdec_si128( workData3, roundKey );
				workData4 = _mm_aesdec_si128( workData4, roundKey );
			}
			roundKey = _mm_loadu_si128( (__m128i*)&roundKeysAsU32[4 * nRounds] );
			workData1 = _mm_aesdeclast_si128( workData1, roundKey );
			workData2 = _mm_aesdeclast_si128( workData2, roundKey );
			workData3 = _mm_aesdeclast_si128( workData3, roundKey );
			workData4 = _mm_aesdeclast_si128( workData4, roundKey );

			workData1 = _mm_xor_si128( workData1, _mm_loadu_si128( (__m128i*)( pIV ) ) );
			workData2 = _mm_xor_si128( workData2, _mm_loadu_si128( (__m128i*)( pubEncryptedData + nDecrypted ) ) );
			workData3 = _mm_xor_si128( workData3, _mm_loadu_si128( (__m128i*)( pubEncryptedData + nDecrypted + 16 ) ) );
			workData4 = _mm_xor_si128( workData4, _mm_loadu_si128( (__m128i*)( pubEncryptedData + nDecrypted + 32 ) ) );

			_mm_storeu_si128( (__m128i*)( pubPlaintextData + nDecrypted ), workData1 );
			_mm_storeu_si128( (__m128i*)( pubPlaintextData + nDecrypted + 16 ), workData2 );

			// Remember last encrypted block before clobbering if operating in-place
			_mm_storeu_si128( (__m128i*)rgubLastEncrypted, _mm_loadu_si128( (__m128i*)( pubEncryptedData + nDecrypted + 48 ) ) );

			_mm_storeu_si128( (__m128i*)( pubPlaintextData + nDecrypted + 32 ), workData3 );
			_mm_storeu_si128( (__m128i*)( pubPlaintextData + nDecrypted + 48 ), workData4 );

			pIV = rgubLastEncrypted;
			nDecrypted += 64;
		}
		SecureZeroMemory( roundKeysAsU32, sizeof( roundKeysAsU32 ) );
	}
#endif

	// Decrypt blocks (or finish decryption of non-multiple-of-4 blocks in AESNI case) 
	while ( nDecrypted < cubEncryptedData - k_nSymmetricBlockSize )
	{
		AES_decrypt( pubEncryptedData + nDecrypted, rgubWorking, key );
#ifdef __arm__
		uint8 temp[ k_nSymmetricBlockSize ];
		for (int i = 0; i < k_nSymmetricBlockSize; ++i)
		{
			temp[i] = pIV[i] ^ rgubWorking[i];
		}

		// Remember last encrypted block before potentially clobbering if operating in-place
		memcpy( rgubLastEncrypted, pubEncryptedData + nDecrypted, k_nSymmetricBlockSize );
		memcpy( pubPlaintextData + nDecrypted, temp, k_nSymmetricBlockSize );
#else
		uint64 temp1 = ((UNALIGNED uint64*)pIV)[0] ^ ((UNALIGNED uint64*)rgubWorking)[0];
		uint64 temp2 = ((UNALIGNED uint64*)pIV)[1] ^ ((UNALIGNED uint64*)rgubWorking)[1];

		// Remember last encrypted block before potentially clobbering if operating in-place
		((UNALIGNED uint64*)rgubLastEncrypted)[0] = ((UNALIGNED uint64*)( pubEncryptedData + nDecrypted ))[0];
		((UNALIGNED uint64*)rgubLastEncrypted)[1] = ((UNALIGNED uint64*)( pubEncryptedData + nDecrypted ))[1];

		((UNALIGNED uint64*)( pubPlaintextData + nDecrypted ))[0] = temp1;
		((UNALIGNED uint64*)( pubPlaintextData + nDecrypted ))[1] = temp2;
#endif

		pIV = rgubLastEncrypted;
		nDecrypted += k_nSymmetricBlockSize;
	}

	// Process final block into rgubWorking for padding inspection
	Assert( nDecrypted == cubEncryptedData - k_nSymmetricBlockSize );
	AES_decrypt( pubEncryptedData + nDecrypted, rgubWorking, key );
	for ( int i = 0; i < k_nSymmetricBlockSize; ++i )
		rgubWorking[i] ^= pIV[i];
	
	// Get final block padding length and make sure it is backfilled properly (PKCS#5)
	uint8 pad = rgubWorking[ k_nSymmetricBlockSize - 1 ];
	if ( bVerifyPaddingBytes )
	{
		uint8 checkBits = ( pad - 1 ) & ~15;
		int32 shiftMask = 0x0001FFFE << ( ( pad - 1 ) & 15 );
		for ( int i = 0; i < k_nSymmetricBlockSize; ++i, shiftMask <<= 1 )
			checkBits |= ( ( rgubWorking[i] ^ pad ) & (uint8)( shiftMask >> 31 ) );
		if ( checkBits != 0 )
			return false;
	}
	
	pad = ( ( pad - 1 ) & 15 ) + 1;

    // Check that we have enough space for final bytes
	if ( *pcubPlaintextData < nDecrypted + k_nSymmetricBlockSize - pad )
		return false;

	// Write any non-pad bytes from rgubWorking to pubPlaintextData.
	// Prefer padding-blind method to avoid timing oracle, if we have output space
	if ( *pcubPlaintextData >= nDecrypted + k_nSymmetricBlockSize )
	{
		memcpy( pubPlaintextData + nDecrypted, rgubWorking, k_nSymmetricBlockSize );
		nDecrypted += k_nSymmetricBlockSize - pad;
	}
	else
	{
		for ( int i = 0; i < k_nSymmetricBlockSize - pad; ++i )
			pubPlaintextData[nDecrypted++] = rgubWorking[i];
	}

	// The old CryptoPP path zeros out the entire destination buffer, but that
	// behavior isn't documented or even expected. We'll just zero out one byte
	// in case anyone relies on string termination, but that zero isn't counted.
	if ( *pcubPlaintextData > nDecrypted )
		pubPlaintextData[nDecrypted] = 0;

	*pcubPlaintextData = nDecrypted;
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

	AES_KEY key;
	if ( AES_set_decrypt_key( pubKey, cubKey * 8, &key ) < 0 )
		return false;

	return BDecryptAESUsingOpenSSL( pubEncryptedData, cubEncryptedData, pubPlaintextData, pcubPlaintextData, &key, pIV, bVerifyPaddingBytes );
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

	SHA256_CTX c;
	VerifyFatal( SHA256_Init( &c ) );
	SHA256_Update( &c, pubInput, cubInput );
	SHA256_Final( *pOutDigest, &c );
	SecureZeroMemory( &c, sizeof(c) );
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

	CHMACSHA256Impl hmac;
	hmac.Init( pubKey, cubKey );
	hmac.Update( pubData, cubData );
	hmac.Final( *pOutputDigest );
}
