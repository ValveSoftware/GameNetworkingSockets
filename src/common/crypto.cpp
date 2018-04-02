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

// tier0
#include "tier0/vprof.h"
#include "vstdlib/strtools.h"
#include "tier1/utlmemory.h"
#include "tier1/utlbuffer.h"
//#include "globals.h" // for DMsg ??
#include "crypto.h"

#include "tier0/memdbgoff.h"
#include <openssl/err.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/pem.h>
#ifdef SDR_SUPPORT_RSA_TICKETS
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif
#include "tier0/memdbgon.h"

#include "opensslwrapper.h"


#ifdef ENABLE_CRYPTO_25519
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
#endif


#ifdef ENABLE_CRYPTO_SCRYPT
extern "C" {
#include "../external/scrypt-jane/scrypt-jane.h"
extern void* (*scrypt_malloc_VALVE)(size_t);
extern void (*scrypt_free_VALVE)(void *);
}
#endif

/// CUtlBuffer that will wipe upon destruction
//
/// WARNING: This is only intended for simple use cases where the caller
/// can easily pre-allocate.  For example, it won't wipe if the buffer needs
/// to be relocated as a result of realloc.  Or if you pas it to a function
/// via a CUtlBuffer&, and CUtlBuffer::Purge is invoked directly.  Etc.
class CAutoWipeBuffer : public CUtlBuffer
{
public:
	CAutoWipeBuffer() {}
	explicit CAutoWipeBuffer( int cbInit ) : CUtlBuffer( 0, cbInit, 0 ) {}
	~CAutoWipeBuffer() { Purge(); }

	void Clear()
	{
		SecureZeroMemory( Base(), SizeAllocated() );
		CUtlBuffer::Clear();
	}

	void Purge()
	{
		Clear();
		CUtlBuffer::Purge();
	}
};

static const char k_szOpenSSHPrivatKeyPEMHeader[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
static const char k_szOpenSSHPrivatKeyPEMFooter[] = "-----END OPENSSH PRIVATE KEY-----";

static const uint k_nRSAOAEPOverheadBytes = 42; // fixed-size overhead of OAEP padding scheme

static bool DecodeBase64ToBuf( const char *pszEncoded, uint32 cbEncoded, CAutoWipeBuffer &buf )
{
	uint32 cubDecodeSize = cbEncoded * 3 / 4 + 1;
	buf.EnsureCapacity( cubDecodeSize );
	if ( !CCrypto::Base64Decode( pszEncoded, cbEncoded, (uint8*)buf.Base(), &cubDecodeSize ) )
		return false;
	buf.SeekPut( CUtlBuffer::SEEK_HEAD, cubDecodeSize );
	return true;
}

static bool DecodePEMBody( const char *pszPem, uint32 cch, CAutoWipeBuffer &buf )
{
	const char *pszBody = CCrypto::LocatePEMBody( pszPem, &cch, nullptr );
	if ( !pszBody )
		return false;
	return DecodeBase64ToBuf( pszBody, cch, buf );
}

static bool BCheckAndEatBytes( CUtlBuffer &buf, const void *data, int sz )
{
	if ( buf.GetBytesRemaining() < sz )
		return false;
	if ( V_memcmp( buf.PeekGet(), data, sz ) != 0 )
		return false;
	buf.SeekGet( CUtlBuffer::SEEK_CURRENT, sz );
	return true;
}

static bool BOpenSSHGetUInt32( CUtlBuffer &buf, uint32 &result )
{
	uint32 temp;
	if ( !buf.Get( &temp, 4 ) )
		return false;
	result = BigDWord( temp );
	return true;
}

static void OpenSSHWriteUInt32( CUtlBuffer &buf, uint32 data )
{
	data = BigDWord( data );
	buf.Put( &data, sizeof(data) );
}

static const uint32 k_nBinarySSHEd25519KeyTypeIDLen = 15;

static bool BOpenSSHBinaryEd25519CheckAndEatKeyType( CUtlBuffer &buf )
{
	return BCheckAndEatBytes( buf, "\x00\x00\x00\x0bssh-ed25519", k_nBinarySSHEd25519KeyTypeIDLen );
}

static void OpenSSHBinaryEd25519WriteKeyType( CUtlBuffer &buf )
{
	buf.Put( "\x00\x00\x00\x0bssh-ed25519", k_nBinarySSHEd25519KeyTypeIDLen );
}

static bool BOpenSSHBinaryReadFixedSizeKey( CUtlBuffer &buf, void *pOut, uint32 cbExpectedSize )
{
	uint32 cbSize;
	if ( !BOpenSSHGetUInt32( buf, cbSize ) )
		return false;
	if ( cbSize != cbExpectedSize || buf.GetBytesRemaining() < (int)cbExpectedSize )
		return false;
	V_memcpy( pOut, buf.PeekGet(), cbExpectedSize );
	buf.SeekGet( CUtlBuffer::SEEK_CURRENT, cbExpectedSize );
	return true;
}

static void OpenSSHBinaryWriteFixedSizeKey( CUtlBuffer &buf, const void *pData, uint32 cbSize )
{
	OpenSSHWriteUInt32( buf, cbSize );
	buf.Put( pData, cbSize );
}

static bool BParseOpenSSHBinaryEd25519Private( CUtlBuffer &buf, uint8 *pKey )
{

	// OpenSSH source sshkey.c, sshkey_private_to_blob2():

	//	if ((r = sshbuf_put(encoded, AUTH_MAGIC, sizeof(AUTH_MAGIC))) != 0 ||
	//	    (r = sshbuf_put_cstring(encoded, ciphername)) != 0 ||
	//	    (r = sshbuf_put_cstring(encoded, kdfname)) != 0 ||
	//	    (r = sshbuf_put_stringb(encoded, kdf)) != 0 ||
	//	    (r = sshbuf_put_u32(encoded, 1)) != 0 ||	/* number of keys */
	//	    (r = sshkey_to_blob(prv, &pubkeyblob, &pubkeylen)) != 0 ||
	//	    (r = sshbuf_put_string(encoded, pubkeyblob, pubkeylen)) != 0)
	//		goto out;

	if ( !BCheckAndEatBytes( buf, "openssh-key-v1", 15 ) )
		return false;

	// Encrypted keys not supported
	if ( !BCheckAndEatBytes( buf, "\x00\x00\x00\x04none\x00\x00\x00\x04none\x00\x00\x00\x00", 20 ) )
	{
		AssertMsg( false, "Tried to use encrypted OpenSSH private key" );
		return false;
	}

	// File should only contain a single key
	if ( !BCheckAndEatBytes( buf, "\x00\x00\x00\x01", 4 ) )
		return false;

	// Public key.  It's actually stored in the file 3 times.
	uint8 arbPubKey1[ 32 ];
	{
		// Size of public key
		uint32 cbEncodedPubKey;
		if ( !BOpenSSHGetUInt32( buf, cbEncodedPubKey ) )
			return false;
		if ( buf.GetBytesRemaining() < (int)cbEncodedPubKey )
			return false;

		// Parse public key
		CUtlBuffer bufPubKey( buf.PeekGet(), cbEncodedPubKey, CUtlBuffer::READ_ONLY );
		bufPubKey.SeekPut( CUtlBuffer::SEEK_HEAD, cbEncodedPubKey );
		buf.SeekGet( CUtlBuffer::SEEK_CURRENT, cbEncodedPubKey );

		if ( !BOpenSSHBinaryEd25519CheckAndEatKeyType( bufPubKey ) )
			return false;
		if ( !BOpenSSHBinaryReadFixedSizeKey( bufPubKey, arbPubKey1, sizeof(arbPubKey1) ) )
			return false;
	}

	// Private key
	{
		uint32 cbEncodedPrivKey;
		if ( !BOpenSSHGetUInt32( buf, cbEncodedPrivKey ) )
			return false;
		if ( buf.GetBytesRemaining() < (int)cbEncodedPrivKey ) // This should actually be the last thing, but if there's extra stuff, I guess we don't care.
			return false;

		// OpenSSH source sshkey.c, to_blob_buf()

		CUtlBuffer bufPrivKey( buf.PeekGet(), cbEncodedPrivKey, CUtlBuffer::READ_ONLY );
		bufPrivKey.SeekPut( CUtlBuffer::SEEK_HEAD, cbEncodedPrivKey );

		// Consume check bytes (used for encrypted keys)

		//	/* Random check bytes */
		//	check = arc4random();
		//	if ((r = sshbuf_put_u32(encrypted, check)) != 0 ||
		//	    (r = sshbuf_put_u32(encrypted, check)) != 0)
		//		goto out;

		uint32 check1, check2;
		if ( !BOpenSSHGetUInt32( bufPrivKey, check1 ) || !BOpenSSHGetUInt32( bufPrivKey, check2 ) || check1 != check2 )
			return false;

		// Key type
		if ( !BOpenSSHBinaryEd25519CheckAndEatKeyType( bufPrivKey ) )
			return false;

		// Public key...again.  One would think that having this large,
		// known know plaintext (TWICE!) is not wise if the key is encrypted
		// with a password....but oh well.
		uint8 arbPubKey2[ 32 ];
		if ( !BOpenSSHBinaryReadFixedSizeKey( bufPrivKey, arbPubKey2, sizeof(arbPubKey2) ) )
			return false;
		if ( V_memcmp( arbPubKey1, arbPubKey2, sizeof(arbPubKey1) ) != 0 )
			return false;

		// And now the entire secret key
		if ( !BOpenSSHBinaryReadFixedSizeKey( bufPrivKey, pKey, 64 ) )
			return false;

		// The "secret" actually consists of the real secret key
		// followed by the public key.  Check that this third
		// copy of the public key matches the other two.
		if ( V_memcmp( arbPubKey1, pKey+32, sizeof(arbPubKey1) ) != 0 )
			return false;

		// OpenSSH stores the key as private first, then public.
		// We want it the other way around
		for ( int i = 0 ; i < 32 ; ++i )
			std::swap( pKey[i], pKey[i+32] );

		// Comment and padding comes after this, but we don't care
	}

	return true;
}

static int OpenSSHBinaryBeginSubBlock( CUtlBuffer &buf )
{
	int nSaveTell = buf.TellPut();
	buf.SeekPut( CUtlBuffer::SEEK_CURRENT, sizeof(uint32) );
	return nSaveTell;
}

static void OpenSSHBinaryEndSubBlock( CUtlBuffer &buf, int nSaveTell )
{
	int nBytesWritten = buf.TellPut() - nSaveTell - sizeof(uint32);
	Assert( nBytesWritten >= 0 );
	*(uint32 *)( (uint8 *)buf.Base() + nSaveTell ) = BigDWord( uint32(nBytesWritten) );
}

static void OpenSSHBinaryWriteEd25519Private( CUtlBuffer &buf, const uint8 *pKey )
{
	// Make sure we don't realloc, so that if we wipe afterwards we don't
	// leave key material lying around.
	buf.EnsureCapacity( 2048 );

	// We store the key as public first, then private
	const uint8 *pPubKey = pKey;
	const uint8 *pPrivKey = pKey+32;

	buf.Put( "openssh-key-v1", 15 );
	buf.Put( "\x00\x00\x00\x04none\x00\x00\x00\x04none\x00\x00\x00\x00", 20 );
	buf.Put( "\x00\x00\x00\x01", 4 );

	// Public key.  It's actually stored in the file 3 times.
	{
		// Size of public key
		int nSaveTell = OpenSSHBinaryBeginSubBlock( buf );
		OpenSSHBinaryEd25519WriteKeyType( buf );
		OpenSSHBinaryWriteFixedSizeKey( buf, pPubKey, 32 );
		OpenSSHBinaryEndSubBlock( buf, nSaveTell );
	}

	// Private key
	{
		int nSaveTell = OpenSSHBinaryBeginSubBlock( buf );

		// Check bytes.  Since we aren't encrypting, it's not useful for
		// these to be random.
		OpenSSHWriteUInt32( buf, 0x12345678 );
		OpenSSHWriteUInt32( buf, 0x12345678 );

		// Key type
		OpenSSHBinaryEd25519WriteKeyType( buf );

		// Public key...again.
		OpenSSHBinaryWriteFixedSizeKey( buf, pPubKey, 32 );

		// And now the entire "secret" key.  But this is actually
		// the private key followed by the public
		OpenSSHWriteUInt32( buf, 64 );
		buf.Put( pPrivKey, 32 );
		buf.Put( pPubKey, 32 );

		// Comment and padding comes after this.
		// Should we write it anything?

		OpenSSHBinaryEndSubBlock( buf, nSaveTell );
	}
}

static bool BParseOpenSSHBinaryEd25519Public( CUtlBuffer &buf, uint8 *pKey )
{
	if ( !BOpenSSHBinaryEd25519CheckAndEatKeyType( buf ) )
		return false;

	if ( !BOpenSSHBinaryReadFixedSizeKey( buf, pKey, 32 ) )
		return false;

	// If there's extra stuff, we don't care
	return true;
}

static void OpenSSHBinaryEd25519WritePublic( CUtlBuffer &buf, const uint8 *pKey )
{
	buf.EnsureCapacity( 128 );
	OpenSSHBinaryEd25519WriteKeyType( buf );
	OpenSSHBinaryWriteFixedSizeKey( buf, pKey, 32 );
}


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


/* SDR_PUBLIC
// PBKDF2-HMAC-SHA256 fixed-round implementation.
static void CalculatePBKDF2SHA256( const void *pubPassword, uint32 cubPassword, const uint8 *pubSalt, uint32 cubSalt, PBKDF2Hash_t &out, uint32 rounds )
{
	Assert( rounds != 0 );

	CHMACSHA256Impl hmacTemplate;
	hmacTemplate.Init( pubPassword, cubPassword );

	CHMACSHA256Impl hmac;
	hmac.InitFrom( hmacTemplate );
	hmac.Update( pubSalt, cubSalt );
	hmac.Update( "\x00\x00\x00\x01", 4 );

	SHA256Digest_t buffer;
	hmac.Final( buffer );

	// For simplicity we only support our own use case, where we have exactly one output block
	COMPILE_TIME_ASSERT( sizeof( SHA256Digest_t ) == sizeof( PBKDF2Hash_t ) );
	V_memcpy( out, buffer, sizeof( SHA256Digest_t ) );
	
	for ( uint32 iRound = 1; iRound < rounds; ++iRound )
	{
		hmac.InitFrom( hmacTemplate );
		hmac.Update( buffer, sizeof( SHA256Digest_t ) );
		hmac.Final( buffer );
		for ( int i = 0; i < sizeof( SHA256Digest_t ); ++i )
			out[i] ^= buffer[i];
	}

	SecureZeroMemory( buffer, sizeof( buffer ) );
}
*/

#ifdef SDR_SUPPORT_RSA_TICKETS
// Decode ASN.1 tag - works only for simple tag/length/value cases, such as SEQUENCE (0x30) and OCTET_STRING (0x04)
static bool DecodeASN1Tag( uint8 expect_tag, const uint8 *buf, size_t buflen, const uint8 **datastart_out, size_t *datalen_out )
{
	*datastart_out = NULL;
	*datalen_out = 0;

	// ASN.1 tag/length/value triple: (tag byte) (encoded len) (raw data)
	if ( buflen < 2 )
	{
		Assert( buflen >= 2 );
		return false;
	}

	if ( buf[0] != expect_tag )
	{
		Assert( buf[0] == expect_tag );
		return false;
	}

	size_t encoded_len = 0;
	size_t encoded_len_bytes = 0;
	if ( (buf[1] & 0x80) == 0 )
	{
		// if length < 128, encoded len is just the length
		encoded_len = buf[1] & 0x7F;
	}
	else
	{
		// else encoded len is 0x80 + number of following big-endian unsigned integer bytes
		encoded_len_bytes = buf[1] & 0x7F;
		if ( encoded_len_bytes < 1 || encoded_len_bytes > sizeof(size_t) )
		{
			Assert( encoded_len_bytes >= 1 && encoded_len_bytes <= sizeof(size_t) );
			return false;
		}
		if ( 2 + encoded_len_bytes > buflen )
		{
			Assert( buflen >= 2 + encoded_len_bytes );
			return false;
		}
		for ( size_t i = 0; i < encoded_len_bytes; ++i )
		{
			encoded_len <<= 8;
			encoded_len += (uint8)buf[2 + i];
		}
		if ( 2 + encoded_len_bytes + encoded_len < encoded_len )
		{
			AssertMsg( false, "ASN.1 length overflow" );
			return false;
		}
	}
	if ( 2 + encoded_len_bytes + encoded_len > buflen )
	{
		Assert( 2 + encoded_len_bytes + encoded_len <= buflen );
		return false;
	}
	*datastart_out = buf + 2 + encoded_len_bytes;
	*datalen_out = encoded_len;
	return true;
}

// Private keys are wrapped in a outer PKCS#8 ASN.1 DER structure. OpenSSL doesn't have code
// to handle this format without passing through base64 PEM first, but we can provide our own.
static ::RSA *OpenSSL_RSAFromPKCS8PrivKey( const uint8 *pDataPtr, size_t cDataLen )
{
	// PKCS#8 wraps the DER-encoded RSA private key as the third OCTET_STRING element in an
	// ASN.1 SEQUENCE with some fixed values as the first two elements.
	if ( DecodeASN1Tag( 0x30/*SEQUENCE*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
	{
		// Skip first two elements which are always encoded exactly the same way
		static const char k_rgExpectSequenceBytes[] = "\x02\x01\x00\x30\x0D\x06\x09\x2A\x86\x48\x86\xF7\x0D\x01\x01\x01\x05"; // final byte is \x00
		if ( cDataLen < sizeof( k_rgExpectSequenceBytes ) || memcmp( pDataPtr, k_rgExpectSequenceBytes, sizeof( k_rgExpectSequenceBytes ) ) != 0 )
		{
			AssertMsg( false, "invalid PKCS#8 RSA Private Key data" );
		}
		else
		{
			pDataPtr += sizeof( k_rgExpectSequenceBytes );
			cDataLen -= sizeof( k_rgExpectSequenceBytes );

			// Parse the ASN.1 OCTET_STRING that contains our DER-encoded RSA private key
			if ( DecodeASN1Tag( 0x04/*OCTET_STRING*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
			{
				// Create OpenSSL RSA struct from DER private key
				return d2i_RSAPrivateKey( NULL, &pDataPtr, (long)cDataLen );
			}
		}
	}
	return NULL;
}
#endif

// Write a tag and its encoded length (or return the number of bytes that we would write)
//SDR_PUBLIC static int WriteASN1TagAndLength( uint8 **pPtr, uint8 tag, int32 len )
//SDR_PUBLIC {
//SDR_PUBLIC 	uint8 discard[8], *pIgnore = discard;
//SDR_PUBLIC 	uint8 * &ptr = pPtr ? *pPtr : pIgnore;
//SDR_PUBLIC 
//SDR_PUBLIC 	*ptr++ = tag;
//SDR_PUBLIC 	if ( len < 128 )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		Assert( len >= 0 );
//SDR_PUBLIC 		*ptr++ = (uint8)(len);
//SDR_PUBLIC 		return 2;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	else if ( len < 65536 )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		*ptr++ = 0x82;
//SDR_PUBLIC 		*ptr++ = (uint8)((uint32)len >> 8);
//SDR_PUBLIC 		*ptr++ = (uint8)(len);
//SDR_PUBLIC 		return 4;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	else if ( len < 256 * 65536 )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		*ptr++ = 0x83;
//SDR_PUBLIC 		*ptr++ = (uint8)((uint32)len >> 16);
//SDR_PUBLIC 		*ptr++ = (uint8)((uint32)len >> 8);
//SDR_PUBLIC 		*ptr++ = (uint8)(len);
//SDR_PUBLIC 		return 5;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	else
//SDR_PUBLIC 	{
//SDR_PUBLIC 		*ptr++ = 0x84;
//SDR_PUBLIC 		*ptr++ = (uint8)((uint32)len >> 24);
//SDR_PUBLIC 		*ptr++ = (uint8)((uint32)len >> 16);
//SDR_PUBLIC 		*ptr++ = (uint8)((uint32)len >> 8);
//SDR_PUBLIC 		*ptr++ = (uint8)(len);
//SDR_PUBLIC 		return 6;
//SDR_PUBLIC 	}
//SDR_PUBLIC }

// Private and public key formats are PKCS#8 DER. OpenSSL doesn't include a simple function
// for writing RSA private keys in this particular format without passing through base64 PEM,
// but we can do it ourselves to avoid that inefficiency.
//SDR_PUBLIC static int OpenSSL_PKCS8PrivKeyFromRSA( const ::RSA *pRSA, uint8 **pPtr )
//SDR_PUBLIC {
//SDR_PUBLIC 	// Output: 0x30 + (sequence length) + (fixed bytes) + 0x04 + (string length) + (string)
//SDR_PUBLIC 
//SDR_PUBLIC 	// Work out overall length by going backwards... start with final string, add prefixes
//SDR_PUBLIC 	int rsalen = i2d_RSAPrivateKey( pRSA, NULL );
//SDR_PUBLIC 	Assert( rsalen > 0 && rsalen < 10 * 1024 * 1024 );
//SDR_PUBLIC 
//SDR_PUBLIC 	int seqlen = WriteASN1TagAndLength( NULL, 0x04, rsalen ) + rsalen;
//SDR_PUBLIC 	static const char k_rgFixedSequencePrefixBytes[] = "\x02\x01\x00\x30\x0D\x06\x09\x2A\x86\x48\x86\xF7\x0D\x01\x01\x01\x05"; // final byte is \x00
//SDR_PUBLIC 	seqlen += sizeof( k_rgFixedSequencePrefixBytes );
//SDR_PUBLIC 
//SDR_PUBLIC 	int total = WriteASN1TagAndLength( NULL, 0x30, seqlen ) + seqlen;
//SDR_PUBLIC 	if ( pPtr )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		uint8 *pExpectEnd = *pPtr + total;
//SDR_PUBLIC 		(void) WriteASN1TagAndLength( pPtr, 0x30, seqlen );
//SDR_PUBLIC 		V_memcpy( *pPtr, k_rgFixedSequencePrefixBytes, sizeof( k_rgFixedSequencePrefixBytes ) );
//SDR_PUBLIC 		(*pPtr) += sizeof( k_rgFixedSequencePrefixBytes );
//SDR_PUBLIC 		(void)WriteASN1TagAndLength( pPtr, 0x04, rsalen );
//SDR_PUBLIC 		Assert( *pPtr == pExpectEnd - rsalen );
//SDR_PUBLIC 		(void)i2d_RSAPrivateKey( pRSA, pPtr );
//SDR_PUBLIC 		Assert( *pPtr == pExpectEnd );
//SDR_PUBLIC 	}
//SDR_PUBLIC 
//SDR_PUBLIC 	return total;
//SDR_PUBLIC }


// Parse X.509 RSA public key structure to extract modulus and exponent as big-endian byte sequences
#ifdef SDR_SUPPORT_RSA_TICKETS
static bool ExtractModulusAndExponentFromX509PubKey( const uint8 *pDataPtr, size_t cDataLen, const uint8 **ppModulus, size_t *pcubModulus, const uint8 **ppExponent, size_t *pcubExponent )
{
	// X.509-format RSA public keys are wrapped in an outer SEQUENCE, with an initial inner SEQUENCE containing ( OID for rsaEncryption, NULL )
	if ( !DecodeASN1Tag( 0x30 /*SEQUENCE*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
		return false;
	static const char k_rgchRSAPubKeyAlgoSequence[] = "\x30\x0D\x06\x09\x2A\x86\x48\x86\xF7\x0D\x01\x01\x01\x05"; // final \x00 implied by string handling
	if ( cDataLen <= sizeof( k_rgchRSAPubKeyAlgoSequence ) || memcmp( pDataPtr, k_rgchRSAPubKeyAlgoSequence, sizeof( k_rgchRSAPubKeyAlgoSequence ) ) != 0 )
		return false;
	pDataPtr += sizeof( k_rgchRSAPubKeyAlgoSequence );
	cDataLen -= sizeof( k_rgchRSAPubKeyAlgoSequence );
	// ... followed by a BIT STRING (*not* OCTET STRING as with PKCS#8) that contains the actual key data
	if ( !DecodeASN1Tag( 0x03 /*BIT STRING*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) || cDataLen < 1 )
		return false;
	// Discard the number of zero-padding bits which were added on top of the BIT STRING; don't care.
	++pDataPtr;
	--cDataLen;
	// Now examining BIT STRING contents, which are in turn another ASN.1 SEQUENCE of two INTEGERs
	// for the public-key modulus and exponent.
	if ( !DecodeASN1Tag( 0x30/*SEQUENCE*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
		return false;
	if ( !DecodeASN1Tag( 0x02 /*INTEGER*/, pDataPtr, cDataLen, ppModulus, pcubModulus ) )
		return false;
	size_t cAdvanceBeyondTag = *ppModulus + *pcubModulus - pDataPtr;
	pDataPtr += cAdvanceBeyondTag;
	cDataLen -= cAdvanceBeyondTag;
	if ( !DecodeASN1Tag( 0x02 /*INTEGER*/, pDataPtr, cDataLen, ppExponent, pcubExponent ) )
		return false;
	// Exponent and modulus may start with a leading 0 byte to prevent ASN.1 sign extension - strip it
	if ( *pcubModulus > 1 && (*ppModulus)[0] == 0x00 )
	{
		++(*ppModulus);
		--(*pcubModulus);
	}
	if ( *pcubExponent > 1 && (*ppExponent)[0] == 0x00 )
	{
		++(*ppExponent);
		--(*pcubExponent);
	}
	return true;
}
#endif // #ifdef SDR_SUPPORT_RSA_TICKETS


// Parse X.509 RSA public key structure to extract modulus (no need for private exponent at this time)
//SDR_PUBLIC static bool ExtractModulusFromPKCS8PrivKey( const uint8 *pDataPtr, size_t cDataLen, const uint8 **ppModulus, size_t *pcubModulus )
//SDR_PUBLIC {
//SDR_PUBLIC 	// PKCS#8 wraps the DER-encoded RSA private key as the third OCTET_STRING element in an
//SDR_PUBLIC 	// ASN.1 SEQUENCE with some fixed values as the first two elements.
//SDR_PUBLIC 	if ( !DecodeASN1Tag( 0x30/*SEQUENCE*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
//SDR_PUBLIC 		return false;
//SDR_PUBLIC 
//SDR_PUBLIC 	// Skip first two elements which are always encoded exactly the same way
//SDR_PUBLIC 	static const char k_rgExpectSequenceBytes[] = "\x02\x01\x00\x30\x0D\x06\x09\x2A\x86\x48\x86\xF7\x0D\x01\x01\x01\x05"; // final byte is \x00
//SDR_PUBLIC 	if ( cDataLen < sizeof( k_rgExpectSequenceBytes ) || memcmp( pDataPtr, k_rgExpectSequenceBytes, sizeof( k_rgExpectSequenceBytes ) ) != 0 )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		AssertMsg( false, "invalid PKCS#8 RSA Private Key data" );
//SDR_PUBLIC 		return false;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	pDataPtr += sizeof( k_rgExpectSequenceBytes );
//SDR_PUBLIC 	cDataLen -= sizeof( k_rgExpectSequenceBytes );
//SDR_PUBLIC 
//SDR_PUBLIC 	// Parse the ASN.1 OCTET_STRING that contains our DER-encoded RSA private key
//SDR_PUBLIC 	if ( !DecodeASN1Tag( 0x04/*OCTET_STRING*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
//SDR_PUBLIC 		return false;
//SDR_PUBLIC 
//SDR_PUBLIC 	// DER-encoded RSA private key contains a SEQUENCE of fields
//SDR_PUBLIC 	if ( !DecodeASN1Tag( 0x30/*SEQUENCE*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
//SDR_PUBLIC 		return false;
//SDR_PUBLIC 
//SDR_PUBLIC 	// First field in sequence is INTEGER (0x02) length 1 (0x01) value 0 (0x00)
//SDR_PUBLIC 	if ( cDataLen < 3 || pDataPtr[0] != 0x02 || pDataPtr[1] != 0x01 || pDataPtr[2] != 0x00 )
//SDR_PUBLIC 		return false;
//SDR_PUBLIC 
//SDR_PUBLIC 	pDataPtr += 3;
//SDR_PUBLIC 	cDataLen -= 3;
//SDR_PUBLIC 	
//SDR_PUBLIC 	// Second field in sequence is big-endian INTEGER containing modulus
//SDR_PUBLIC 	if ( !DecodeASN1Tag( 0x02 /*INTEGER*/, pDataPtr, cDataLen, ppModulus, pcubModulus ) )
//SDR_PUBLIC 		return false;
//SDR_PUBLIC 
//SDR_PUBLIC 	// Modulus may start with a leading 0 byte to prevent ASN.1 sign extension - strip it
//SDR_PUBLIC 	if ( *pcubModulus > 1 && (*ppModulus)[0] == 0x00 )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		++(*ppModulus);
//SDR_PUBLIC 		--(*pcubModulus);
//SDR_PUBLIC 	}
//SDR_PUBLIC 
//SDR_PUBLIC 	return true;
//SDR_PUBLIC }


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
// Purpose: Encrypts the specified data with the specified key.  Uses AES (Rijndael) symmetric
//			encryption.  The encrypted data may then be decrypted by calling SymmetricDecrypt
//			with the same key.  Generates a random initialization vector of the
//			appropriate size.
// Input:	pubPlaintextData -	Data to be encrypted
//			cubPlaintextData -	Size of data to be encrypted
//			pubEncryptedData -  Pointer to buffer to receive encrypted data
//			pcubEncryptedData - Pointer to a variable that at time of call contains the size of
//								the receive buffer for encrypted data.  When the method returns, this will contain
//								the actual size of the encrypted data.
//			pubKey -			the key to encrypt the data with
//			cubKey -			Size of the key (must be k_nSymmetricKeyLen)
// Output:  true if successful, false if encryption failed
//-----------------------------------------------------------------------------

bool CCrypto::SymmetricEncrypt( const uint8 *pubPlaintextData, const uint32 cubPlaintextData, 
								uint8 *pubEncryptedData, uint32 *pcubEncryptedData,
								const uint8 *pubKey, const uint32 cubKey )
{
	bool bRet = false;

	//
	// Generate a random IV
	//
	byte rgubIV[k_nSymmetricBlockSize];
	GenerateRandomBlock( rgubIV, k_nSymmetricBlockSize );

	bRet = SymmetricEncryptHelper( pubPlaintextData, cubPlaintextData, rgubIV, k_nSymmetricBlockSize, pubEncryptedData, pcubEncryptedData, pubKey, cubKey, true /*prepend encrypted IV*/ );

	return bRet;
}


//-----------------------------------------------------------------------------
// Purpose: Encrypts the specified data with the specified key.  Uses AES (Rijndael) symmetric
//			encryption.  The encrypted data may then be decrypted by calling SymmetricDecrypt
//			with the same key.
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

bool CCrypto::SymmetricEncryptChosenIV( const uint8 * pubPlaintextData, uint32 cubPlaintextData,
										const uint8 * pIV, uint32 cubIV, uint8 * pubEncryptedData,
										uint32 * pcubEncryptedData, const uint8 * pubKey, uint32 cubKey )
{
	return SymmetricEncryptHelper( pubPlaintextData, cubPlaintextData, pIV, cubIV, pubEncryptedData, pcubEncryptedData, pubKey, cubKey, true /*prepend encrypted IV*/ );
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
//			pubPlaintextData -  Pointer to buffer to receive decrypted data
//			pcubPlaintextData - Pointer to a variable that at time of call contains the size of
//								the receive buffer for decrypted data.  When the method returns, this will contain
//								the actual size of the decrypted data.
//			pubKey -			the key to decrypt the data with
//			cubKey -			Size of the key (must be k_nSymmetricKeyLen)
// Output:  true if successful, false if decryption failed
//-----------------------------------------------------------------------------
bool CCrypto::SymmetricDecrypt( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
							   uint8 *pubPlaintextData, uint32 *pcubPlaintextData, 
							   const uint8 *pubKey, const uint32 cubKey )
{
	return SymmetricDecryptRecoverIV( pubEncryptedData, cubEncryptedData, pubPlaintextData, pcubPlaintextData, NULL, 0, pubKey, cubKey );
}


//-----------------------------------------------------------------------------
// Purpose: Decrypts the specified data with the specified key.  Uses AES (Rijndael) symmetric
//			decryption.  
// Input:	pubEncryptedData -	Data to be decrypted
//			cubEncryptedData -	Size of data to be decrypted
//			pubPlaintextData -  Pointer to buffer to receive decrypted data
//			pcubPlaintextData - Pointer to a variable that at time of call contains the size of
//								the receive buffer for decrypted data.  When the method returns, this will contain
//								the actual size of the decrypted data.
//			pubIV -				buffer for the decrypted IV, optional
//			cubIV -				the size of the buffer for the decrypted IV (must be 16 if pubIV != NULL)
//			pubKey -			the key to decrypt the data with
//			cubKey -			Size of the key (must be k_nSymmetricKeyLen)
// Output:  true if successful, false if decryption failed
//-----------------------------------------------------------------------------
bool CCrypto::SymmetricDecryptRecoverIV( const uint8 *pubEncryptedData, uint32 cubEncryptedData,
	uint8 *pubPlaintextData, uint32 *pcubPlaintextData, uint8 *pubIV, uint32 cubIV,
	const uint8 *pubKey, const uint32 cubKey, bool bVerifyPaddingBytes )
{
	Assert( pubEncryptedData );
	Assert( cubEncryptedData);
	Assert( pubPlaintextData );
	Assert( pcubPlaintextData );
	Assert( *pcubPlaintextData );
	Assert( pubKey );
	Assert( k_nSymmetricKeyLen256 == cubKey || k_nSymmetricKeyLen128 == cubKey );	// the only key length supported is k_nSymmetricKeyLen
	Assert( pubIV == NULL || cubIV == k_nSymmetricBlockSize );

	// the initialization vector (IV) must be stored in the first block of bytes.
	// If the size of encrypted data is not at least the block size, it is not valid
	if ( cubEncryptedData < k_nSymmetricBlockSize )
		return false;

	AES_KEY key;
	if ( AES_set_decrypt_key( pubKey, cubKey * 8, &key ) < 0 )
		return false;

	// Our first block is straight AES block encryption of IV with user key, no XOR.
	uint8 rgubIV[ k_nSymmetricBlockSize ];
	AES_decrypt( pubEncryptedData, rgubIV, &key );
	pubEncryptedData += k_nSymmetricBlockSize;
	cubEncryptedData -= k_nSymmetricBlockSize;

	bool bRet = BDecryptAESUsingOpenSSL( pubEncryptedData, cubEncryptedData, pubPlaintextData, pcubPlaintextData, &key, rgubIV, bVerifyPaddingBytes );
	if ( bRet && pubIV )
	{
		memcpy( pubIV, rgubIV, Min<size_t>( sizeof( rgubIV ), cubIV ) );
	}

	SecureZeroMemory( &key, sizeof(key) );
	SecureZeroMemory( rgubIV, sizeof(rgubIV) );
	return bRet;
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
// Purpose: For specified plaintext data size, returns what size of symmetric
//			encrypted data will be
//-----------------------------------------------------------------------------
uint32 CCrypto::GetSymmetricEncryptedSize( uint32 cubPlaintextData )
{
	return k_nSymmetricBlockSize /* IV */ + ALIGN_VALUE( cubPlaintextData + 1, k_nSymmetricBlockSize );
}

//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Encrypts the specified data with the specified text password.  
//SDR_PUBLIC 	//			Uses the SHA256 hash of the password as the key for AES (Rijndael) symmetric
//SDR_PUBLIC 	//			encryption. A SHA1 HMAC of the result is appended, for authentication on 
//SDR_PUBLIC 	//			the receiving end.
//SDR_PUBLIC 	//			The encrypted data may then be decrypted by calling DecryptWithPasswordAndAuthenticate
//SDR_PUBLIC 	//			with the same password.
//SDR_PUBLIC 	// Input:	pubPlaintextData -	Data to be encrypted
//SDR_PUBLIC 	//			cubPlaintextData -	Size of data to be encrypted
//SDR_PUBLIC 	//			pubEncryptedData -  Pointer to buffer to receive encrypted data
//SDR_PUBLIC 	//			pcubEncryptedData - Pointer to a variable that at time of call contains the size of
//SDR_PUBLIC 	//								the receive buffer for encrypted data.  When the method returns, this will contain
//SDR_PUBLIC 	//								the actual size of the encrypted data.
//SDR_PUBLIC 	//			pchPassword -		text password
//SDR_PUBLIC 	// Output:  true if successful, false if encryption failed
//SDR_PUBLIC 	//
//SDR_PUBLIC 	// This is DEPRECATED because you can attack it fairly easily by brute-forcing the HMAC. 
//SDR_PUBLIC 	// Just e
//SDR_PUBLIC 	//
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::EncryptWithPasswordAndHMAC_DEPRECATED( const uint8 *pubPlaintextData, uint32 cubPlaintextData,
//SDR_PUBLIC 									 uint8 * pubEncryptedData, uint32 * pcubEncryptedData,
//SDR_PUBLIC 									 const char *pchPassword )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		//
//SDR_PUBLIC 		// Generate a random IV
//SDR_PUBLIC 		//
//SDR_PUBLIC 		byte rgubIV[k_nSymmetricBlockSize];
//SDR_PUBLIC 		GenerateRandomBlock( rgubIV, k_nSymmetricBlockSize );
//SDR_PUBLIC 	
//SDR_PUBLIC 		return EncryptWithPasswordAndHMACWithIV_DEPRECATED( pubPlaintextData, cubPlaintextData, rgubIV, k_nSymmetricBlockSize, pubEncryptedData, pcubEncryptedData, pchPassword );
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Encrypts the specified data with the specified text password.  
//SDR_PUBLIC 	//			Uses the SHA256 hash of the password as the key for AES (Rijndael) symmetric
//SDR_PUBLIC 	//			encryption. A SHA1 HMAC of the result is appended, for authentication on 
//SDR_PUBLIC 	//			the receiving end.
//SDR_PUBLIC 	//			The encrypted data may then be decrypted by calling DecryptWithPasswordAndAuthenticate
//SDR_PUBLIC 	//			with the same password.
//SDR_PUBLIC 	// Input:	pubPlaintextData -	Data to be encrypted
//SDR_PUBLIC 	//			cubPlaintextData -	Size of data to be encrypted
//SDR_PUBLIC 	//			pIV -				IV to use for AES encryption. Should be random and never used before unless you know
//SDR_PUBLIC 	//								exactly what you're doing.
//SDR_PUBLIC 	//			cubIV -				size of the IV - should be same ase the AES blocksize.
//SDR_PUBLIC 	//			pubEncryptedData -  Pointer to buffer to receive encrypted data
//SDR_PUBLIC 	//			pcubEncryptedData - Pointer to a variable that at time of call contains the size of
//SDR_PUBLIC 	//								the receive buffer for encrypted data.  When the method returns, this will contain
//SDR_PUBLIC 	//								the actual size of the encrypted data.
//SDR_PUBLIC 	//			pchPassword -		text password
//SDR_PUBLIC 	// Output:  true if successful, false if encryption failed
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::EncryptWithPasswordAndHMACWithIV_DEPRECATED( const uint8 *pubPlaintextData, uint32 cubPlaintextData,
//SDR_PUBLIC 									 const uint8 * pIV, uint32 cubIV,
//SDR_PUBLIC 									 uint8 * pubEncryptedData, uint32 * pcubEncryptedData,
//SDR_PUBLIC 									 const char *pchPassword )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		uint8 rgubKey[k_nSymmetricKeyLen];
//SDR_PUBLIC 		if ( !pchPassword || !pchPassword[0] )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		if ( !cubPlaintextData )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		uint32 cubBuffer = *pcubEncryptedData;
//SDR_PUBLIC 		uint32 cubExpectedResult = GetSymmetricEncryptedSize( cubPlaintextData ) + sizeof( SHADigest_t );
//SDR_PUBLIC 	
//SDR_PUBLIC 		if ( cubBuffer < cubExpectedResult )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		GenerateSHA256Digest( (const uint8 *)pchPassword, V_strlen( pchPassword ), &rgubKey );
//SDR_PUBLIC 		
//SDR_PUBLIC 		bool bRet = SymmetricEncryptChosenIV( pubPlaintextData, cubPlaintextData, pIV, cubIV, pubEncryptedData, pcubEncryptedData, rgubKey, k_nSymmetricKeyLen );
//SDR_PUBLIC 		if ( bRet )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			// calc HMAC
//SDR_PUBLIC 			uint32 cubEncrypted = *pcubEncryptedData;
//SDR_PUBLIC 			*pcubEncryptedData += sizeof( SHADigest_t );
//SDR_PUBLIC 			if ( cubBuffer < *pcubEncryptedData )
//SDR_PUBLIC 				return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 			SHADigest_t *pHMAC = (SHADigest_t*)( pubEncryptedData + cubEncrypted );
//SDR_PUBLIC 			bRet = CCrypto::GenerateHMAC( pubEncryptedData, cubEncrypted, rgubKey, k_nSymmetricKeyLen, pHMAC );
//SDR_PUBLIC 		}
//SDR_PUBLIC 	
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Decrypts the specified data with the specified password.  Uses AES (Rijndael) symmetric
//SDR_PUBLIC 	//			decryption. First, the HMAC is verified - if it is not correct, then we know that
//SDR_PUBLIC 	//			the key is incorrect or the data is corrupted, and the decryption fails.
//SDR_PUBLIC 	// Input:	pubEncryptedData -	Data to be decrypted
//SDR_PUBLIC 	//			cubEncryptedData -	Size of data to be decrypted
//SDR_PUBLIC 	//			pubPlaintextData -  Pointer to buffer to receive decrypted data
//SDR_PUBLIC 	//			pcubPlaintextData - Pointer to a variable that at time of call contains the size of
//SDR_PUBLIC 	//								the receive buffer for decrypted data.  When the method returns, this will contain
//SDR_PUBLIC 	//								the actual size of the decrypted data.
//SDR_PUBLIC 	//			pchPassword -		the text password to decrypt the data with
//SDR_PUBLIC 	// Output:  true if successful, false if decryption failed
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::DecryptWithPasswordAndAuthenticate_DEPRECATED( const uint8 * pubEncryptedData, uint32 cubEncryptedData, 
//SDR_PUBLIC 									 uint8 * pubPlaintextData, uint32 * pcubPlaintextData,
//SDR_PUBLIC 									 const char *pchPassword )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		uint8 rgubKey[k_nSymmetricKeyLen];
//SDR_PUBLIC 		if ( !pchPassword || !pchPassword[0] )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		if ( cubEncryptedData <= sizeof( SHADigest_t ) )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		GenerateSHA256Digest( (const uint8 *)pchPassword, V_strlen( pchPassword ), &rgubKey );
//SDR_PUBLIC 	
//SDR_PUBLIC 		uint32 cubCiphertext = cubEncryptedData - sizeof( SHADigest_t );
//SDR_PUBLIC 		SHADigest_t *pHMAC = (SHADigest_t*)( pubEncryptedData + cubCiphertext  );
//SDR_PUBLIC 		SHADigest_t hmacActual;
//SDR_PUBLIC 		bool bRet = CCrypto::GenerateHMAC( pubEncryptedData, cubCiphertext, rgubKey, k_nSymmetricKeyLen, &hmacActual );
//SDR_PUBLIC 	
//SDR_PUBLIC 		if ( bRet )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			// invalid ciphertext or key
//SDR_PUBLIC 			if ( V_memcmp( &hmacActual, pHMAC, sizeof( SHADigest_t ) ) )
//SDR_PUBLIC 				return false;
//SDR_PUBLIC 			
//SDR_PUBLIC 			bRet = SymmetricDecrypt( pubEncryptedData, cubCiphertext, pubPlaintextData, pcubPlaintextData, rgubKey, k_nSymmetricKeyLen );
//SDR_PUBLIC 		}
//SDR_PUBLIC 	
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	#ifndef CRYPTO_DISABLE_ENCRYPT_WITH_PASSWORD
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Encrypts the specified data with the specified text passphrase.
//SDR_PUBLIC 	//
//SDR_PUBLIC 	// EncryptWithPassphrase / DecryptWithPassphrase uses the following format:
//SDR_PUBLIC 	//  <1 byte algo id> <1 byte parameter> <16 bytes random IV> <AES-256 CBC of data> <HMAC-SHA256 of all preceding bytes>
//SDR_PUBLIC 	//
//SDR_PUBLIC 	// Let "intermediate secret" be the password digest generated according to the algorithm id and parameter:
//SDR_PUBLIC 	//   key for AES-256 CBC is HMAC-SHA256( key = intermediate secret, signed data = 4 bytes "AES\x01" )
//SDR_PUBLIC 	//   key for HMAC-SHA256 is HMAC-SHA256( key = intermediate secret, signed data = 4 bytes "HMAC" )
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::EncryptWithPassphraseImpl( const uint8 *pubPlaintextData, uint32 cubPlaintextData, uint8 * pubEncryptedData, uint32 * pcubEncryptedData, const char *pchPassphrase, int nPassphraseLength, bool bStrong )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		if ( nPassphraseLength < 0 )
//SDR_PUBLIC 			nPassphraseLength = (int) V_strlen( pchPassphrase );
//SDR_PUBLIC 	
//SDR_PUBLIC 		COMPILE_TIME_ASSERT( sizeof( SHA256Digest_t ) == 32 );
//SDR_PUBLIC 		uint32 cubRequiredSpace = 2 + CCrypto::GetSymmetricEncryptedSize( cubPlaintextData ) + 32;
//SDR_PUBLIC 		if ( cubRequiredSpace > *pcubEncryptedData )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			*pcubEncryptedData = cubRequiredSpace;
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 		}
//SDR_PUBLIC 	
//SDR_PUBLIC 		PassphraseHelperFn_t pHelper = NULL;
//SDR_PUBLIC 		if ( bStrong )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			// Make it effing expensive to crack
//SDR_PUBLIC 	#ifdef ENABLE_CRYPTO_SCRYPT
//SDR_PUBLIC 			// Scrypt is tuned to require ~400ms and 256MB of temporary memory on server CPUs
//SDR_PUBLIC 			pubEncryptedData[0] = 0x02; // hardcoded 0x02 = scrypt
//SDR_PUBLIC 			pubEncryptedData[1] = 0x00; // parameter is currently reserved, must be 0
//SDR_PUBLIC 			pHelper = &CCrypto::PassphraseHelper_Scrypt;
//SDR_PUBLIC 	#else
//SDR_PUBLIC 			// PBKDF2 with absurdly high round count is less effective than scrypt, but still strong
//SDR_PUBLIC 			pubEncryptedData[0] = 0x01; // hardcoded 0x01 = PBKDF2
//SDR_PUBLIC 			pubEncryptedData[1] = 21; // 2^21 rounds is over 2 million rounds, takes ~1.5 seconds
//SDR_PUBLIC 			pHelper = &CCrypto::PassphraseHelper_PBKDF2SHA256;
//SDR_PUBLIC 	#endif
//SDR_PUBLIC 		}
//SDR_PUBLIC 		else
//SDR_PUBLIC 		{
//SDR_PUBLIC 			// Choose faster mobile- and web-friendly settings
//SDR_PUBLIC 			pubEncryptedData[0] = 0x01; // hardcoded 0x01 = PBKDF2
//SDR_PUBLIC 			pubEncryptedData[1] = 14; // parameter is log2(rounds), 2^14 rounds is 16384 rounds
//SDR_PUBLIC 			pHelper = &CCrypto::PassphraseHelper_PBKDF2SHA256;
//SDR_PUBLIC 		}
//SDR_PUBLIC 	
//SDR_PUBLIC 		GenerateRandomBlock( pubEncryptedData + 2, 16 ); // random 16-byte IV
//SDR_PUBLIC 	
//SDR_PUBLIC 		SHA256Digest_t aeskey, hmackey;
//SDR_PUBLIC 		uint8 digest[32] = {};
//SDR_PUBLIC 		if ( !pHelper || !pHelper( digest, pchPassphrase, nPassphraseLength, pubEncryptedData + 2, 16, pubEncryptedData[0], pubEncryptedData[1] ) )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			Assert( false ); // shouldn't be possible for helper to fail
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 		}
//SDR_PUBLIC 		GenerateHMAC256( (const uint8*)"AES\x01", 4, digest, sizeof( digest ), &aeskey );
//SDR_PUBLIC 		GenerateHMAC256( (const uint8*)"HMAC", 4, digest, sizeof( digest ), &hmackey );
//SDR_PUBLIC 		SecureZeroMemory( digest, sizeof(digest) );
//SDR_PUBLIC 	
//SDR_PUBLIC 		uint32 cubSpaceForSymmetricEncrypt = cubRequiredSpace - 18;
//SDR_PUBLIC 		bool bRet = SymmetricEncryptWithIV( pubPlaintextData, cubPlaintextData, pubEncryptedData + 2, 16, pubEncryptedData + 18, &cubSpaceForSymmetricEncrypt, aeskey, sizeof(aeskey) );
//SDR_PUBLIC 		SecureZeroMemory( aeskey, sizeof( aeskey ) );
//SDR_PUBLIC 	
//SDR_PUBLIC 		if ( bRet && 2 + 16 + cubSpaceForSymmetricEncrypt + 32 == cubRequiredSpace )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			SHA256Digest_t hmac;
//SDR_PUBLIC 			GenerateHMAC256( pubEncryptedData, 2 + 16 + cubSpaceForSymmetricEncrypt, hmackey, sizeof( hmackey ), &hmac );
//SDR_PUBLIC 			SecureZeroMemory( hmackey, sizeof( hmackey ) );
//SDR_PUBLIC 			memcpy( pubEncryptedData + 2 + 16 + cubSpaceForSymmetricEncrypt, hmac, sizeof( hmac ) );
//SDR_PUBLIC 			SecureZeroMemory( hmac, sizeof( hmac ) );
//SDR_PUBLIC 		}
//SDR_PUBLIC 		else
//SDR_PUBLIC 		{
//SDR_PUBLIC 			Assert( false ); // this shouldn't ever happen!! aside from implementation bugs...
//SDR_PUBLIC 			SecureZeroMemory( hmackey, sizeof( hmackey ) );
//SDR_PUBLIC 			SecureZeroMemory( pubEncryptedData + 18, cubRequiredSpace - 18 );
//SDR_PUBLIC 			bRet = false;
//SDR_PUBLIC 		}
//SDR_PUBLIC 	
//SDR_PUBLIC 		*pcubEncryptedData = cubRequiredSpace;
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Decrypt the specified data with the specified text passphrase.
//SDR_PUBLIC 	//
//SDR_PUBLIC 	// EncryptWithPassphrase / DecryptWithPassphrase uses the following format:
//SDR_PUBLIC 	//  <1 byte 0x01> <1 byte rounds_log2> <16 bytes random IV> <AES-256 CBC of data> <HMAC-SHA256 of all preceding bytes>
//SDR_PUBLIC 	//
//SDR_PUBLIC 	// Let "intermediate secret" be the password digest generated according to the algorithm id and parameter:
//SDR_PUBLIC 	//   key for AES-256 CBC is HMAC-SHA256( key = intermediate secret, signed data = 4 bytes "AES\x01" )
//SDR_PUBLIC 	//   key for HMAC-SHA256 is HMAC-SHA256( key = intermediate secret, signed data = 4 bytes "HMAC" )
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::DecryptWithPassphraseImpl( const uint8 *pubEncryptedData, uint32 cubEncryptedData, uint8 * pubPlaintextData, uint32 * pcubPlaintextData, const char *pchPassphrase, int nPassphraseLength, bool bVerifyIntegrity )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		if ( nPassphraseLength < 0 )
//SDR_PUBLIC 			nPassphraseLength = (int)V_strlen( pchPassphrase );
//SDR_PUBLIC 	
//SDR_PUBLIC 		if ( cubEncryptedData < 2 + 16 + 16 + 32 )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		uint8 digest[32] = {};
//SDR_PUBLIC 		switch ( pubEncryptedData[0] )
//SDR_PUBLIC 		{
//SDR_PUBLIC 		case 0x01:
//SDR_PUBLIC 			Assert( PassphraseHelper_PBKDF2SHA256( digest, pchPassphrase, nPassphraseLength, pubEncryptedData + 2, 16, pubEncryptedData[0], pubEncryptedData[1] ) );
//SDR_PUBLIC 			break;
//SDR_PUBLIC 	#ifdef ENABLE_CRYPTO_SCRYPT
//SDR_PUBLIC 		case 0x02:
//SDR_PUBLIC 			Assert( PassphraseHelper_Scrypt( digest, pchPassphrase, nPassphraseLength, pubEncryptedData + 2, 16, pubEncryptedData[0], pubEncryptedData[1] ) );
//SDR_PUBLIC 			break;
//SDR_PUBLIC 	#endif
//SDR_PUBLIC 		default:
//SDR_PUBLIC 			AssertMsg1( false, "Unhandled algorithm 0x%02x, this build does not know how to decrypt it", pubEncryptedData[0] );
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 		}
//SDR_PUBLIC 	
//SDR_PUBLIC 		SHA256Digest_t aeskey, hmackey;
//SDR_PUBLIC 		GenerateHMAC256( (const uint8*)"AES\x01", 4, digest, sizeof( digest ), &aeskey );
//SDR_PUBLIC 		if ( bVerifyIntegrity )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			GenerateHMAC256( (const uint8*)"HMAC", 4, digest, sizeof( digest ), &hmackey );
//SDR_PUBLIC 		}
//SDR_PUBLIC 		SecureZeroMemory( digest, sizeof( digest ) );
//SDR_PUBLIC 	
//SDR_PUBLIC 		if ( bVerifyIntegrity )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			SHA256Digest_t hmac;
//SDR_PUBLIC 			GenerateHMAC256( pubEncryptedData, cubEncryptedData - 32, hmackey, sizeof( hmackey ), &hmac );
//SDR_PUBLIC 			SecureZeroMemory( hmackey, sizeof( hmackey ) );
//SDR_PUBLIC 			int deltaBits = 0;
//SDR_PUBLIC 			for ( int i = 0; i < 32; ++i )
//SDR_PUBLIC 			{
//SDR_PUBLIC 				deltaBits |= (int)(uint8)( hmac[i] ^ pubEncryptedData[cubEncryptedData - 32 + i] );
//SDR_PUBLIC 			}
//SDR_PUBLIC 			SecureZeroMemory( hmac, sizeof( hmac ) );
//SDR_PUBLIC 			if ( deltaBits != 0 )
//SDR_PUBLIC 				return false;
//SDR_PUBLIC 		}
//SDR_PUBLIC 	
//SDR_PUBLIC 		bool bRet = SymmetricDecryptWithIV( pubEncryptedData + 18, cubEncryptedData - 18 - 32, pubEncryptedData + 2, 16, pubPlaintextData, pcubPlaintextData, aeskey, sizeof( aeskey ), bVerifyIntegrity );
//SDR_PUBLIC 		SecureZeroMemory( aeskey, sizeof( aeskey ) );
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	// //-----------------------------------------------------------------------------
//SDR_PUBLIC 	// // Purpose: Helper to implement PBKDF2 algorithm for EncryptWithPassphrase
//SDR_PUBLIC 	// //-----------------------------------------------------------------------------
//SDR_PUBLIC 	// bool CCrypto::PassphraseHelper_PBKDF2SHA256( uint8 ( &rgubDigest )[32], const char *pchPassphrase, int nPassphraseLength, const uint8 *pubSalt, uint32 cubSalt, uint8 ubAlgorithmID, uint8 ubParameter )
//SDR_PUBLIC 	// {
//SDR_PUBLIC 	// 	// Parameter is log2(rounds)
//SDR_PUBLIC 	// 	Assert( ubAlgorithmID == 0x01 && ubParameter > 10 && ubParameter < 31 );
//SDR_PUBLIC 	// 	if ( !( ubAlgorithmID == 0x01 && ubParameter > 10 && ubParameter < 31 ) )
//SDR_PUBLIC 	// 		return false;
//SDR_PUBLIC 	// 
//SDR_PUBLIC 	// 	if ( nPassphraseLength < 0 )
//SDR_PUBLIC 	// 		nPassphraseLength = (int)V_strlen( pchPassphrase );
//SDR_PUBLIC 	// 
//SDR_PUBLIC 	// 	CalculatePBKDF2SHA256( pchPassphrase, (uint32)nPassphraseLength, pubSalt, cubSalt, rgubDigest, 1u << ubParameter );
//SDR_PUBLIC 	// 	return true;
//SDR_PUBLIC 	// }
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Helper to implement scrypt algorithm for EncryptWithPassphrase
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	#ifdef ENABLE_CRYPTO_SCRYPT
//SDR_PUBLIC 	bool CCrypto::PassphraseHelper_Scrypt( uint8( &rgubDigest )[32], const char *pchPassphrase, int nPassphraseLength, const uint8 *pubSalt, uint32 cubSalt, uint8 ubAlgorithmID, uint8 ubParameter )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		// Parameter is always zero, we hardcode scrypt parameters
//SDR_PUBLIC 		Assert( ubAlgorithmID == 0x02 && ubParameter == 0 );
//SDR_PUBLIC 		if ( !( ubAlgorithmID == 0x02 && ubParameter == 0 ) )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		if ( nPassphraseLength < 0 )
//SDR_PUBLIC 			nPassphraseLength = (int)V_strlen( pchPassphrase );
//SDR_PUBLIC 	
//SDR_PUBLIC 		bool bRet = GenerateStrongScryptDigest( pchPassphrase, nPassphraseLength, pubSalt, cubSalt, &rgubDigest );
//SDR_PUBLIC 		Assert( bRet );
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	#endif
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Encrypts the specified data with the specified text passphrase.
//SDR_PUBLIC 	// Choses a "strong" algorithm that requires a large amount of system resources
//SDR_PUBLIC 	// to effectively make brute-force dictionary attacks impossible. Can require
//SDR_PUBLIC 	// several seconds of CPU work and/or up to 256 MB of temporary RAM to decrypt!
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::EncryptWithPassphrase_Strong( const uint8 *pubPlaintextData, uint32 cubPlaintextData, uint8 * pubEncryptedData, uint32 * pcubEncryptedData, const char *pchPassphrase, int nPassphraseLength /* = -1 */ )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		return EncryptWithPassphraseImpl( pubPlaintextData, cubPlaintextData, pubEncryptedData, pcubEncryptedData, pchPassphrase, nPassphraseLength, true );
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Encrypts the specified data with the specified text passphrase.
//SDR_PUBLIC 	// Currently 16384 PBKDF2-SHA256 rounds, significantly slows down brute-forcing.
//SDR_PUBLIC 	// This is on par with what modern password managers do to protect master files.
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::EncryptWithPassphrase_Fast( const uint8 *pubPlaintextData, uint32 cubPlaintextData, uint8 * pubEncryptedData, uint32 * pcubEncryptedData, const char *pchPassphrase, int nPassphraseLength /* = -1 */ )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		return EncryptWithPassphraseImpl( pubPlaintextData, cubPlaintextData, pubEncryptedData, pcubEncryptedData, pchPassphrase, nPassphraseLength, false );
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Decrypts the specified data with the specified text passphrase.
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::DecryptWithPassphrase( const uint8 *pubEncryptedData, uint32 cubEncryptedData, uint8 * pubPlaintextData, uint32 * pcubPlaintextData, const char *pchPassphrase, int nPassphraseLength /* = -1 */, bool bVerifyIntegrity /* = true */ )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		return DecryptWithPassphraseImpl( pubEncryptedData, cubEncryptedData, pubPlaintextData, pcubPlaintextData, pchPassphrase, nPassphraseLength, bVerifyIntegrity );
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	#ifdef ENABLE_CRYPTO_25519
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: variation EncryptWithPassphrase_Strong with no CBC trailing padding
//SDR_PUBLIC 	// and no integrity checks. Extremely compact representation.
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::EncryptECPrivateKeyWithPassphrase( const CEC25519PrivateKeyBase &privateKey, uint8 *pubEncryptedData, uint32 *pcubEncryptedData, const char *pchPassphrase, int nPassphraseLen, bool bStrong )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		Assert( privateKey.IsValid() );
//SDR_PUBLIC 		if ( !privateKey.IsValid() )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		Assert( pcubEncryptedData && *pcubEncryptedData >= 2 + 16 + 32 );
//SDR_PUBLIC 		if ( !pcubEncryptedData || *pcubEncryptedData < 2 + 16 + 32 )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		uint8 bufPrivate[32];
//SDR_PUBLIC 		memcpy( bufPrivate, privateKey.GetData() + 32, 32 );
//SDR_PUBLIC 	
//SDR_PUBLIC 		uint8 bufEncrypted[128] = {};
//SDR_PUBLIC 		uint32 buflen = sizeof(bufEncrypted);
//SDR_PUBLIC 	
//SDR_PUBLIC 		bool bRet = ( bStrong ? EncryptWithPassphrase_Strong : EncryptWithPassphrase_Fast )( bufPrivate, 32, bufEncrypted, &buflen, pchPassphrase, nPassphraseLen ) && buflen == 2 + 16 + 32 + 16 + 32;
//SDR_PUBLIC 		SecureZeroMemory( bufPrivate, sizeof( bufPrivate ) );
//SDR_PUBLIC 	
//SDR_PUBLIC 		// Don't output the final CBC block, which is all padding, or the HMAC that follows
//SDR_PUBLIC 		*pcubEncryptedData = 2 + 16 + 32;
//SDR_PUBLIC 		memcpy( pubEncryptedData, bufEncrypted, 2 + 16 + 32 );
//SDR_PUBLIC 		SecureZeroMemory( bufEncrypted, sizeof( bufEncrypted ) );
//SDR_PUBLIC 	
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: variation of DecryptWithPassphrase with no CBC trailing padding
//SDR_PUBLIC 	// and no integrity checks. Extremely compact representation.
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::DecryptECPrivateKeyWithPassphrase( const uint8 *pubEncryptedData, uint32 cubEncryptedData, CEC25519PrivateKeyBase *pPrivateKey, const char *pchPassphrase, int nPassphraseLength )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		pPrivateKey->Wipe();
//SDR_PUBLIC 		Assert( pPrivateKey->GetKeyType() == k_ECryptoKeyTypeKeyExchangePrivate || pPrivateKey->GetKeyType() == k_ECryptoKeyTypeSigningPrivate );
//SDR_PUBLIC 		if ( pPrivateKey->GetKeyType() != k_ECryptoKeyTypeKeyExchangePrivate && pPrivateKey->GetKeyType() != k_ECryptoKeyTypeSigningPrivate )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 		if ( cubEncryptedData != 2 + 16 + 32 )
//SDR_PUBLIC 			return false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		uint8 bufEncrypted[ 2 + 16 + 32 + 16 + 32 ] = {}; // zero init
//SDR_PUBLIC 		memcpy( bufEncrypted, pubEncryptedData, 2 + 16 + 32 );
//SDR_PUBLIC 		// final 16 + 32 bytes (final CBC block + HMAC) will be zeros, don't care.
//SDR_PUBLIC 	
//SDR_PUBLIC 		uint8 bufDecrypted[ 32 + 16 ]; // need room for whatever junk the all-zero trailing CBC block decrypts to
//SDR_PUBLIC 		uint32 cubDecrypted = sizeof( bufDecrypted );
//SDR_PUBLIC 		if ( DecryptWithPassphrase( bufEncrypted, sizeof( bufEncrypted ), bufDecrypted, &cubDecrypted, pchPassphrase, nPassphraseLength, false /* do not verify HMAC or padding */ ) && cubDecrypted >= 32 )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			pPrivateKey->RebuildFromPrivateData( bufDecrypted );
//SDR_PUBLIC 		}
//SDR_PUBLIC 	
//SDR_PUBLIC 		SecureZeroMemory( bufDecrypted, sizeof( bufDecrypted ) );
//SDR_PUBLIC 		SecureZeroMemory( bufEncrypted, sizeof( bufEncrypted ) );
//SDR_PUBLIC 		return pPrivateKey->IsValid();
//SDR_PUBLIC 	}
//SDR_PUBLIC 	#endif
//SDR_PUBLIC 	
//SDR_PUBLIC 	#endif // #ifndef CRYPTO_DISABLE_ENCRYPT_WITH_PASSWORD
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Generates a new pair of private/public RSA keys
//SDR_PUBLIC 	// Input:	cKeyBits -			Bit length for the key to generate
//SDR_PUBLIC 	//			rsaKeyPublic -		Reference to return the generated public key
//SDR_PUBLIC 	//			rsaKeyPrivate -		Reference to return the generated private key
//SDR_PUBLIC 	// Output:  true if successful, false if key generation failed
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::RSAGenerateKeys( uint32 cKeyBits, CRSAPublicKey &rsaKeyPublic, CRSAPrivateKey &rsaKeyPrivate )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		bool bSuccess = false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		rsaKeyPublic.Wipe();
//SDR_PUBLIC 		rsaKeyPrivate.Wipe();
//SDR_PUBLIC 	
//SDR_PUBLIC 		OneTimeCryptoInitOpenSSL();
//SDR_PUBLIC 	
//SDR_PUBLIC 		::BIGNUM *pBN_E = BN_new();
//SDR_PUBLIC 		::RSA *pRSA = RSA_new();
//SDR_PUBLIC 	
//SDR_PUBLIC 		// Crypto++ defaults to E=17; E=65537 is generally accepted as a much better choice for
//SDR_PUBLIC 		// practical security reasons. E=3 is 8x faster for RSA operations, but E=65537 mitigates
//SDR_PUBLIC 		// attacks against known weaknesses in earlier (now-deprecated) padding schemes. -henryg
//SDR_PUBLIC 		if ( BN_set_word( pBN_E, 65537 ) && RSA_generate_key_ex( pRSA, cKeyBits, pBN_E, NULL ) )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			CUtlMemory<uint8> mem;
//SDR_PUBLIC 	
//SDR_PUBLIC 			// Encode private key in portable PKCS#8 DER format
//SDR_PUBLIC 			{
//SDR_PUBLIC 				int lengthToEncodePriv = OpenSSL_PKCS8PrivKeyFromRSA( pRSA, NULL );
//SDR_PUBLIC 				Assert( lengthToEncodePriv > 0 && lengthToEncodePriv < 1024*1024 ); /* 1 MB sanity check */
//SDR_PUBLIC 				if ( lengthToEncodePriv > 0 && lengthToEncodePriv < 1024*1024 )
//SDR_PUBLIC 				{
//SDR_PUBLIC 					mem.EnsureCapacity( lengthToEncodePriv + 1 );
//SDR_PUBLIC 					uint8 *pPtr = mem.Base();
//SDR_PUBLIC 					(void) OpenSSL_PKCS8PrivKeyFromRSA( pRSA, &pPtr );
//SDR_PUBLIC 					Assert( pPtr == mem.Base() + lengthToEncodePriv );
//SDR_PUBLIC 					if ( pPtr == mem.Base() + lengthToEncodePriv )
//SDR_PUBLIC 					{
//SDR_PUBLIC 						rsaKeyPrivate.Set( mem.Base(), (uint32)lengthToEncodePriv );
//SDR_PUBLIC 					}
//SDR_PUBLIC 					SecureZeroMemory( mem.Base(), (size_t)mem.CubAllocated() );
//SDR_PUBLIC 				}
//SDR_PUBLIC 			}
//SDR_PUBLIC 	
//SDR_PUBLIC 			// Encode public key in portable X.509 public key DER format
//SDR_PUBLIC 			{
//SDR_PUBLIC 				int lengthToEncodePub = i2d_RSA_PUBKEY( pRSA, NULL );
//SDR_PUBLIC 				Assert( lengthToEncodePub > 0 && lengthToEncodePub < 1024*1024 ); /* 1 MB sanity check */
//SDR_PUBLIC 				if ( lengthToEncodePub > 0 && lengthToEncodePub < 1024*1024 )
//SDR_PUBLIC 				{
//SDR_PUBLIC 					mem.EnsureCapacity( lengthToEncodePub + 1 );
//SDR_PUBLIC 					uint8 *pPtr = mem.Base();
//SDR_PUBLIC 					(void)i2d_RSA_PUBKEY( pRSA, &pPtr );
//SDR_PUBLIC 					Assert( pPtr == mem.Base() + lengthToEncodePub );
//SDR_PUBLIC 					if ( pPtr == mem.Base() + lengthToEncodePub )
//SDR_PUBLIC 					{
//SDR_PUBLIC 						rsaKeyPublic.Set( mem.Base(), (uint32)lengthToEncodePub );
//SDR_PUBLIC 					}
//SDR_PUBLIC 					SecureZeroMemory( mem.Base(), (size_t)mem.CubAllocated() );
//SDR_PUBLIC 				}
//SDR_PUBLIC 			}
//SDR_PUBLIC 		}
//SDR_PUBLIC 	
//SDR_PUBLIC 		BN_free( pBN_E );
//SDR_PUBLIC 		RSA_free( pRSA );
//SDR_PUBLIC 	
//SDR_PUBLIC 		DispatchOpenSSLErrors( "CCrypto::RSAGenerateKeys" );
//SDR_PUBLIC 	
//SDR_PUBLIC 		bSuccess = rsaKeyPublic.IsValid() && rsaKeyPrivate.IsValid();
//SDR_PUBLIC 	
//SDR_PUBLIC 		return bSuccess;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Returns the maximum plaintext length that can be encrypted in a
//SDR_PUBLIC 	//			single block for the given public key.
//SDR_PUBLIC 	// Input:	rsaKey - Reference to public key
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	uint32 CCrypto::GetRSAMaxPlaintextSize( const CRSAPublicKey &rsaKey )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		return GetRSAEncryptionBlockSize( rsaKey ) - k_nRSAOAEPOverheadBytes;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Returns the ciphertext block size resulting from encryption of
//SDR_PUBLIC 	///			a single block for the given public key.
//SDR_PUBLIC 	// Input:	rsaKey - Reference to public key
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	uint32 CCrypto::GetRSAEncryptionBlockSize( const CRSAPublicKey &rsaKey )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		return rsaKey.GetModulusBytes( nullptr, 0 );
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Encrypts the specified data with the specified RSA public key.  
//SDR_PUBLIC 	//			The encrypted data may then be decrypted by calling RSADecrypt with the
//SDR_PUBLIC 	//			corresponding RSA private key.
//SDR_PUBLIC 	// Input:	pubPlaintextData -	Data to be encrypted
//SDR_PUBLIC 	//			cubPlaintextData -	Size of data to be encrypted
//SDR_PUBLIC 	//			pubEncryptedData -  Pointer to buffer to receive encrypted data
//SDR_PUBLIC 	//			pcubEncryptedData - Pointer to a variable that at time of call contains the size of
//SDR_PUBLIC 	//								the receive buffer for encrypted data.  When the method returns, this will contain
//SDR_PUBLIC 	//								the actual size of the encrypted data.
//SDR_PUBLIC 	//			pubPublicKey -		the RSA public key to encrypt the data with
//SDR_PUBLIC 	//			cubPublicKey -		Size of the key (must be k_nSymmetricKeyLen)
//SDR_PUBLIC 	// Output:  true if successful, false if encryption failed
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::RSAEncrypt( const uint8 *pubPlaintextData, uint32 cubPlaintextData, 
//SDR_PUBLIC 							  uint8 *pubEncryptedData, uint32 *pcubEncryptedData, 
//SDR_PUBLIC 							  const uint8 *pubPublicKey, const uint32 cubPublicKey )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		VPROF_BUDGET( "CCrypto::RSAEncrypt", VPROF_BUDGETGROUP_ENCRYPTION );
//SDR_PUBLIC 		bool bRet = false;
//SDR_PUBLIC 		Assert( cubPlaintextData > 0 );	// must pass in some data
//SDR_PUBLIC 	
//SDR_PUBLIC 		OneTimeCryptoInitOpenSSL();
//SDR_PUBLIC 		const uint8 *pPublicKeyPtr = pubPublicKey;
//SDR_PUBLIC 		::RSA *rsa = d2i_RSA_PUBKEY( NULL, &pPublicKeyPtr, cubPublicKey );
//SDR_PUBLIC 		if ( rsa )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			int nEncryptedBytesPerIteration = RSA_size( rsa );
//SDR_PUBLIC 			// Sanity check on RSA modulus size - between 512 and 32k bits
//SDR_PUBLIC 			if ( nEncryptedBytesPerIteration < 60 || nEncryptedBytesPerIteration > 4000 )
//SDR_PUBLIC 			{
//SDR_PUBLIC 				AssertMsg1( false, "Invalid RSA modulus: %d bytes wide", nEncryptedBytesPerIteration );
//SDR_PUBLIC 			}
//SDR_PUBLIC 			else
//SDR_PUBLIC 			{
//SDR_PUBLIC 				// calculate how many blocks of encryption will we need to do and how big the output will be
//SDR_PUBLIC 				int nPlaintextBytesPerIteration = nEncryptedBytesPerIteration - k_nRSAOAEPOverheadBytes;
//SDR_PUBLIC 				uint32 cBlocks = 1 + ( ( cubPlaintextData - 1 ) / (uint32)nPlaintextBytesPerIteration );
//SDR_PUBLIC 				uint32 cubCipherText = cBlocks * (uint32)nEncryptedBytesPerIteration;
//SDR_PUBLIC 				// ensure there is sufficient room in output buffer for result
//SDR_PUBLIC 				if ( cubCipherText > (*pcubEncryptedData) )
//SDR_PUBLIC 				{
//SDR_PUBLIC 					AssertMsg2( false, "CCrypto::RSAEncrypt: insufficient output buffer for encryption, needed %d got %d\n", cubCipherText, *pcubEncryptedData );
//SDR_PUBLIC 				}
//SDR_PUBLIC 				else
//SDR_PUBLIC 				{
//SDR_PUBLIC 					uint8 *pubOutputStart = pubEncryptedData;
//SDR_PUBLIC 					for ( ; cBlocks > 0; --cBlocks )
//SDR_PUBLIC 					{
//SDR_PUBLIC 						// encrypt either all remaining plaintext, or maximum allowed plaintext per RSA encryption operation
//SDR_PUBLIC 						uint32 cubToEncrypt = Min( cubPlaintextData, (uint32)nPlaintextBytesPerIteration );
//SDR_PUBLIC 						// encrypt the plaintext
//SDR_PUBLIC 						if ( RSA_public_encrypt( cubToEncrypt, pubPlaintextData, pubEncryptedData, rsa, RSA_PKCS1_OAEP_PADDING ) != nEncryptedBytesPerIteration )
//SDR_PUBLIC 						{
//SDR_PUBLIC 							SecureZeroMemory( pubOutputStart, cubCipherText );
//SDR_PUBLIC 							AssertMsg( false, "RSA encryption failed" );
//SDR_PUBLIC 							break;
//SDR_PUBLIC 						}
//SDR_PUBLIC 						pubPlaintextData += cubToEncrypt;
//SDR_PUBLIC 						cubPlaintextData -= cubToEncrypt;
//SDR_PUBLIC 						pubEncryptedData += nEncryptedBytesPerIteration;
//SDR_PUBLIC 					}
//SDR_PUBLIC 	
//SDR_PUBLIC 					bRet = ( cBlocks == 0 );
//SDR_PUBLIC 					*pcubEncryptedData = bRet ? cubCipherText : 0;
//SDR_PUBLIC 				}
//SDR_PUBLIC 			}
//SDR_PUBLIC 			RSA_free( rsa );
//SDR_PUBLIC 		}
//SDR_PUBLIC 		DispatchOpenSSLErrors( "CCrypto::RSAEncrypt" );
//SDR_PUBLIC 	
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Decrypts the specified data with the specified RSA private key 
//SDR_PUBLIC 	// Input:	pubEncryptedData -	Data to be decrypted
//SDR_PUBLIC 	//			cubEncryptedData -	Size of data to be decrypted
//SDR_PUBLIC 	//			pubPlaintextData -  Pointer to buffer to receive decrypted data
//SDR_PUBLIC 	//			pcubPlaintextData - Pointer to a variable that at time of call contains the size of
//SDR_PUBLIC 	//								the receive buffer for decrypted data.  When the method returns, this will contain
//SDR_PUBLIC 	//								the actual size of the decrypted data.
//SDR_PUBLIC 	//			pubPrivateKey -		the RSA private key key to decrypt the data with
//SDR_PUBLIC 	//			cubPrivateKey -		Size of the key (must be k_nSymmetricKeyLen)
//SDR_PUBLIC 	// Output:  true if successful, false if decryption failed
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::RSADecrypt( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
//SDR_PUBLIC 							  uint8 *pubPlaintextData, uint32 *pcubPlaintextData, 
//SDR_PUBLIC 							  const uint8 *pubPrivateKey, const uint32 cubPrivateKey, bool bLegacyPKCSv15 )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		VPROF_BUDGET( "CCrypto::RSADecrypt", VPROF_BUDGETGROUP_ENCRYPTION );
//SDR_PUBLIC 		bool bRet = false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		OneTimeCryptoInitOpenSSL();
//SDR_PUBLIC 		::RSA *rsa = OpenSSL_RSAFromPKCS8PrivKey( pubPrivateKey, cubPrivateKey );
//SDR_PUBLIC 		if ( rsa )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			int nEncryptedBytesPerIteration = RSA_size( rsa );
//SDR_PUBLIC 			// Sanity check on RSA modulus size - between 512 and 32k bits
//SDR_PUBLIC 			if ( nEncryptedBytesPerIteration < 60 || nEncryptedBytesPerIteration > 4000 )
//SDR_PUBLIC 			{
//SDR_PUBLIC 				AssertMsg1( false, "Invalid RSA modulus: %d bytes wide", nEncryptedBytesPerIteration );
//SDR_PUBLIC 			}
//SDR_PUBLIC 			else
//SDR_PUBLIC 			{
//SDR_PUBLIC 				// calculate how many blocks of decryption will we need to do and how big the output will be
//SDR_PUBLIC 				int nPlaintextBytesPerIteration = nEncryptedBytesPerIteration - k_nRSAOAEPOverheadBytes;
//SDR_PUBLIC 				uint32 cBlocks = 1 + ((cubEncryptedData - 1) / (uint32)nEncryptedBytesPerIteration);
//SDR_PUBLIC 				uint32 cubMaxPlaintext = cBlocks * (uint32)nPlaintextBytesPerIteration;
//SDR_PUBLIC 				// ensure there is sufficient room in output buffer for result
//SDR_PUBLIC 				if ( cubMaxPlaintext > (*pcubPlaintextData) )
//SDR_PUBLIC 				{
//SDR_PUBLIC 					AssertMsg2( false, "CCrypto::RSAEncrypt: insufficient output buffer for decryption, needed %d got %d\n", cubMaxPlaintext, *pcubPlaintextData );
//SDR_PUBLIC 				}
//SDR_PUBLIC 				else
//SDR_PUBLIC 				{
//SDR_PUBLIC 					uint8 *pubOutputStart = pubPlaintextData;
//SDR_PUBLIC 					for ( ; cBlocks > 0; --cBlocks )
//SDR_PUBLIC 					{
//SDR_PUBLIC 						// decrypt either all remaining ciphertext, or maximum allowed per RSA decryption operation
//SDR_PUBLIC 						uint32 cubToDecrypt = Min( cubEncryptedData, (uint32)nEncryptedBytesPerIteration );
//SDR_PUBLIC 						int ret = RSA_private_decrypt( cubToDecrypt, pubEncryptedData, pubPlaintextData, rsa, bLegacyPKCSv15 ? RSA_PKCS1_PADDING : RSA_PKCS1_OAEP_PADDING );
//SDR_PUBLIC 						if ( ret <= 0 || ret > nPlaintextBytesPerIteration || ( ret < nPlaintextBytesPerIteration && cBlocks != 1 ) )
//SDR_PUBLIC 						{
//SDR_PUBLIC 							ERR_clear_error(); // if RSA_private_decrypt failed, we don't spew - could be invalid data.
//SDR_PUBLIC 							SecureZeroMemory( pubOutputStart, cubMaxPlaintext );
//SDR_PUBLIC 							pubPlaintextData = pubOutputStart;
//SDR_PUBLIC 							break;
//SDR_PUBLIC 						}
//SDR_PUBLIC 						pubEncryptedData += nEncryptedBytesPerIteration;
//SDR_PUBLIC 						cubEncryptedData -= nEncryptedBytesPerIteration;
//SDR_PUBLIC 						pubPlaintextData += ret;
//SDR_PUBLIC 					}
//SDR_PUBLIC 	
//SDR_PUBLIC 					bRet = ( cBlocks == 0 );
//SDR_PUBLIC 					*pcubPlaintextData = (int)( pubPlaintextData - pubOutputStart );
//SDR_PUBLIC 				}
//SDR_PUBLIC 			}
//SDR_PUBLIC 			RSA_free( rsa );
//SDR_PUBLIC 		}
//SDR_PUBLIC 		DispatchOpenSSLErrors( "CCrypto::RSADecrypt" );
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Decrypts the specified data with the specified RSA PUBLIC key,
//SDR_PUBLIC 	//			using no padding (eg un-padded signature).
//SDR_PUBLIC 	// Input:	pubEncryptedData -	Data to be decrypted
//SDR_PUBLIC 	//			cubEncryptedData -	Size of data to be decrypted
//SDR_PUBLIC 	//			pubPlaintextData -  Pointer to buffer to receive decrypted data
//SDR_PUBLIC 	//			pcubPlaintextData - Pointer to a variable that at time of call contains the size of
//SDR_PUBLIC 	//								the receive buffer for decrypted data.  When the method returns, this will contain
//SDR_PUBLIC 	//								the actual size of the decrypted data.
//SDR_PUBLIC 	//			pubPublicKey -		the RSA public key key to decrypt the data with
//SDR_PUBLIC 	//			cubPublicKey -		Size of the key
//SDR_PUBLIC 	// Output:  true if successful, false if decryption failed
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::RSAPublicDecrypt_NoPadding_DEPRECATED( const uint8 *pubEncryptedData, uint32 cubEncryptedData, 
//SDR_PUBLIC 							 uint8 *pubPlaintextData, uint32 *pcubPlaintextData, 
//SDR_PUBLIC 							 const uint8 *pubPublicKey, const uint32 cubPublicKey )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		// NOTE: This is not a generally secure use of RSA encryption! It may serve to
//SDR_PUBLIC 		// temporarily obfuscate the numeric value of a CD KEY, but the algorithm and key
//SDR_PUBLIC 		// can be cracked with enough samples due to the lack of a secure padding mode.
//SDR_PUBLIC 		// And as there is no integrity check, attackers can flip bits in transit. -henryg
//SDR_PUBLIC 	
//SDR_PUBLIC 		VPROF_BUDGET( "CCrypto::RSAPublicDecrypt_NoPadding", VPROF_BUDGETGROUP_ENCRYPTION );
//SDR_PUBLIC 		bool bRet = false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		Assert( cubEncryptedData > 0 );	// must pass in some data
//SDR_PUBLIC 	
//SDR_PUBLIC 		// BUGBUG taylor
//SDR_PUBLIC 		// This probably only works for reasonably small ciphertext sizes.
//SDR_PUBLIC 	
//SDR_PUBLIC 		OneTimeCryptoInitOpenSSL();
//SDR_PUBLIC 		const uint8 *pPublicKeyPtr = pubPublicKey;
//SDR_PUBLIC 		::RSA *rsa = d2i_RSA_PUBKEY( NULL, &pPublicKeyPtr, cubPublicKey );
//SDR_PUBLIC 		if ( rsa )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			int nModulusBytes = RSA_size( rsa );
//SDR_PUBLIC 			// Sanity check on RSA modulus size - severely relaxed for compatibility with
//SDR_PUBLIC 			// the absurd 64-bit RSA keys used by DOOM3 activation codes (!?!) -henryg
//SDR_PUBLIC 			if ( nModulusBytes < 1 || nModulusBytes > 4000 )
//SDR_PUBLIC 			{
//SDR_PUBLIC 				AssertMsg1( false, "Invalid RSA modulus: %d bytes wide", nModulusBytes );
//SDR_PUBLIC 			}
//SDR_PUBLIC 			else
//SDR_PUBLIC 			{
//SDR_PUBLIC 				CUtlMemory<uint8> mem;
//SDR_PUBLIC 				mem.EnsureCapacity( nModulusBytes );
//SDR_PUBLIC 				int ret = RSA_public_decrypt( cubEncryptedData, pubEncryptedData, mem.Base(), rsa, RSA_NO_PADDING );
//SDR_PUBLIC 				// No padding scheme, should always have a full modulus-width array of bytes as output
//SDR_PUBLIC 				if ( ret == nModulusBytes )
//SDR_PUBLIC 				{
//SDR_PUBLIC 					// For compatibility with pre-existing code, always strip all trailing null bytes
//SDR_PUBLIC 					uint cubDecrypted = (uint)nModulusBytes;
//SDR_PUBLIC 					while ( cubDecrypted > 0 && mem[cubDecrypted-1] == 0 )
//SDR_PUBLIC 						--cubDecrypted;
//SDR_PUBLIC 	
//SDR_PUBLIC 					// For compatibility with pre-existing code, fail quietly if there isn't enough space
//SDR_PUBLIC 					if ( *pcubPlaintextData >= cubDecrypted )
//SDR_PUBLIC 					{
//SDR_PUBLIC 						V_memcpy( pubPlaintextData, mem.Base(), cubDecrypted );
//SDR_PUBLIC 						*pcubPlaintextData = cubDecrypted;
//SDR_PUBLIC 						bRet = true;
//SDR_PUBLIC 					}
//SDR_PUBLIC 				}
//SDR_PUBLIC 				SecureZeroMemory( mem.Base(), nModulusBytes );
//SDR_PUBLIC 			}
//SDR_PUBLIC 			RSA_free( rsa );
//SDR_PUBLIC 		}
//SDR_PUBLIC 		DispatchOpenSSLErrors( "CCrypto::RSAPublicDecrypt_NoPadding_DEPRECATED" );
//SDR_PUBLIC 	
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}


#ifdef ENABLE_CRYPTO_25519
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
#endif


//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Given an RSA private key, return the length in bytes of a signature
//SDR_PUBLIC 	//			done using that key.
//SDR_PUBLIC 	// Input:	rsaKey - RSA private key
//SDR_PUBLIC 	// Output:	Length in bytes of a corresponding signature
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	uint32 CCrypto::GetRSASignatureSize( const CRSAPrivateKey &rsaKey )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		size_t cubModulus;
//SDR_PUBLIC 		const uint8 *pubModulus;
//SDR_PUBLIC 		if ( !ExtractModulusFromPKCS8PrivKey( rsaKey.GetData(), rsaKey.GetLength(), &pubModulus, &cubModulus ) )
//SDR_PUBLIC 			return 0;
//SDR_PUBLIC 		Assert( cubModulus <= 0xFFFFFFFFu );
//SDR_PUBLIC 		return cubModulus <= 0xFFFFFFFFu ? (uint32)cubModulus : 0;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Given an RSA public key, return the length in bytes of a signature
//SDR_PUBLIC 	//			done using that key.
//SDR_PUBLIC 	// Input:	rsaKey - RSA public key
//SDR_PUBLIC 	// Output:	Length in bytes of a corresponding signature
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	uint32 CCrypto::GetRSASignatureSize( const CRSAPublicKey &rsaKey )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		return GetRSAEncryptionBlockSize( rsaKey );
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
#ifdef SDR_SUPPORT_RSA_TICKETS
#endif
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Generates an RSA signature block for the specified data with the specified
//SDR_PUBLIC 	//			RSA private key.  The signature can be verified by calling RSAVerifySignature
//SDR_PUBLIC 	//			with the RSA public key.
//SDR_PUBLIC 	// Input:	pubData -			Data to be signed
//SDR_PUBLIC 	//			cubData -			Size of data to be signed
//SDR_PUBLIC 	//			pubSignature -		Pointer to buffer to receive signature block
//SDR_PUBLIC 	//			pcubSignature -		Pointer to a variable that at time of call contains the size of
//SDR_PUBLIC 	//								the pubSignature buffer.  When the method returns, this will contain
//SDR_PUBLIC 	//								the actual size of the signature block
//SDR_PUBLIC 	//			pubPrivateKey -		The RSA private key to use to sign the data
//SDR_PUBLIC 	//			cubPrivateKey -		Size of the key
//SDR_PUBLIC 	// Output:  true if successful, false if signature failed
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::RSASign( const uint8 *pubData, const uint32 cubData, 
//SDR_PUBLIC 						   uint8 *pubSignature, uint32 *pcubSignature,
//SDR_PUBLIC 						   const uint8 *pubPrivateKey, const uint32 cubPrivateKey )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		VPROF_BUDGET( "CCrypto::RSASign", VPROF_BUDGETGROUP_ENCRYPTION );
//SDR_PUBLIC 		Assert( pubData );
//SDR_PUBLIC 		Assert( pubPrivateKey );
//SDR_PUBLIC 		Assert( cubPrivateKey > 0 );
//SDR_PUBLIC 		Assert( pubSignature );
//SDR_PUBLIC 		Assert( pcubSignature );
//SDR_PUBLIC 		Assert( *pcubSignature > 0 );
//SDR_PUBLIC 		bool bRet = false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		OneTimeCryptoInitOpenSSL();
//SDR_PUBLIC 		::RSA *rsa = OpenSSL_RSAFromPKCS8PrivKey( pubPrivateKey, cubPrivateKey );
//SDR_PUBLIC 		if ( rsa )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			if ( *pcubSignature < (uint)RSA_size( rsa ) )
//SDR_PUBLIC 			{
//SDR_PUBLIC 				AssertMsg2( false, "Insufficient signature buffer passed to RSASign, got %u needed %d", *pcubSignature, RSA_size( rsa ) );
//SDR_PUBLIC 			}
//SDR_PUBLIC 			else
//SDR_PUBLIC 			{
//SDR_PUBLIC 				SHADigest_t digest;
//SDR_PUBLIC 				CCrypto::GenerateSHA1Digest( (const uint8*)pubData, cubData, &digest );
//SDR_PUBLIC 				bRet = !!RSA_sign( NID_sha1, digest, sizeof( digest ), pubSignature, pcubSignature, rsa );
//SDR_PUBLIC 			}
//SDR_PUBLIC 			RSA_free( rsa );
//SDR_PUBLIC 		}
//SDR_PUBLIC 		DispatchOpenSSLErrors( "CCrypto::RSASign" );
//SDR_PUBLIC 	
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Verifies that signature block is authentic for given data & RSA public key
//SDR_PUBLIC 	// Input:	pubData -			Data that was signed
//SDR_PUBLIC 	//			cubData -			Size of data that was signed signed
//SDR_PUBLIC 	//			pubSignature -		Signature block
//SDR_PUBLIC 	//			cubSignature -		Size of signature block
//SDR_PUBLIC 	//			pubPublicKey -		The RSA public key to use to verify the signature 
//SDR_PUBLIC 	//								(must be from same pair as RSA private key used to generate signature)
//SDR_PUBLIC 	//			cubPublicKey -		Size of the key
//SDR_PUBLIC 	// Output:  true if successful and signature is authentic, false if signature does not match or other error
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::RSAVerifySignature( const uint8 *pubData, const uint32 cubData, 
//SDR_PUBLIC 									  const uint8 *pubSignature, const uint32 cubSignature, 
//SDR_PUBLIC 									  const uint8 *pubPublicKey, const uint32 cubPublicKey )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		VPROF_BUDGET( "CCrypto::RSAVerifySignature", VPROF_BUDGETGROUP_ENCRYPTION );
//SDR_PUBLIC 		Assert( pubData );
//SDR_PUBLIC 		Assert( pubSignature );
//SDR_PUBLIC 		Assert( pubPublicKey );
//SDR_PUBLIC 		
//SDR_PUBLIC 		bool bRet = false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		OneTimeCryptoInitOpenSSL();
//SDR_PUBLIC 		const uint8 *pPublicKeyPtr = pubPublicKey;
//SDR_PUBLIC 		if ( ::RSA *rsa = d2i_RSA_PUBKEY( NULL, &pPublicKeyPtr, cubPublicKey ) )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			SHADigest_t digest;
//SDR_PUBLIC 			GenerateSHA1Digest( pubData, cubData, &digest );
//SDR_PUBLIC 			bRet = !!RSA_verify( NID_sha1, digest, sizeof(digest), pubSignature, cubSignature, rsa );
//SDR_PUBLIC 			ERR_clear_error(); // if RSA_verify failed, we don't spew - could be invalid data.
//SDR_PUBLIC 			RSA_free( rsa );
//SDR_PUBLIC 		}
//SDR_PUBLIC 		DispatchOpenSSLErrors( "CCrypto::RSAVerifySignature" );
//SDR_PUBLIC 	
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Generates an RSA signature block for the specified data with the specified
//SDR_PUBLIC 	//			RSA private key.  The signature can be verified by calling RSAVerifySignature
//SDR_PUBLIC 	//			with the RSA public key.
//SDR_PUBLIC 	// Input:	pubData -			Data to be signed
//SDR_PUBLIC 	//			cubData -			Size of data to be signed
//SDR_PUBLIC 	//			pubSignature -		Pointer to buffer to receive signature block
//SDR_PUBLIC 	//			pcubSignature -		Pointer to a variable that at time of call contains the size of
//SDR_PUBLIC 	//								the pubSignature buffer.  When the method returns, this will contain
//SDR_PUBLIC 	//								the actual size of the signature block
//SDR_PUBLIC 	//			pubPrivateKey -		The RSA private key to use to sign the data
//SDR_PUBLIC 	//			cubPrivateKey -		Size of the key
//SDR_PUBLIC 	// Output:  true if successful, false if signature failed
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::RSASignSHA256( const uint8 *pubData, const uint32 cubData, 
//SDR_PUBLIC 						   uint8 *pubSignature, uint32 *pcubSignature,
//SDR_PUBLIC 						   const uint8 *pubPrivateKey, const uint32 cubPrivateKey )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		VPROF_BUDGET( "CCrypto::RSASign", VPROF_BUDGETGROUP_ENCRYPTION );
//SDR_PUBLIC 		Assert( pubData );
//SDR_PUBLIC 		Assert( pubPrivateKey );
//SDR_PUBLIC 		Assert( cubPrivateKey > 0 );
//SDR_PUBLIC 		Assert( pubSignature );
//SDR_PUBLIC 		Assert( pcubSignature );
//SDR_PUBLIC 		Assert( *pcubSignature > 0 );
//SDR_PUBLIC 		bool bRet = false;
//SDR_PUBLIC 	
//SDR_PUBLIC 		OneTimeCryptoInitOpenSSL();
//SDR_PUBLIC 		::RSA *rsa = OpenSSL_RSAFromPKCS8PrivKey( pubPrivateKey, cubPrivateKey );
//SDR_PUBLIC 		if ( rsa )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			SHA256Digest_t digest;
//SDR_PUBLIC 			CCrypto::GenerateSHA256Digest( (const uint8*)pubData, cubData, &digest );
//SDR_PUBLIC 			bRet = !!RSA_sign( NID_sha256, digest, sizeof( digest ), pubSignature, pcubSignature, rsa );
//SDR_PUBLIC 			RSA_free( rsa );
//SDR_PUBLIC 		}
//SDR_PUBLIC 		DispatchOpenSSLErrors( "CCrypto::RSASignSHA256" );
//SDR_PUBLIC 	
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}
//SDR_PUBLIC 	
//SDR_PUBLIC 	
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	// Purpose: Verifies that signature block is authentic for given data & RSA public key
//SDR_PUBLIC 	// Input:	pubData -			Data that was signed
//SDR_PUBLIC 	//			cubData -			Size of data that was signed signed
//SDR_PUBLIC 	//			pubSignature -		Signature block
//SDR_PUBLIC 	//			cubSignature -		Size of signature block
//SDR_PUBLIC 	//			pubPublicKey -		The RSA public key to use to verify the signature 
//SDR_PUBLIC 	//								(must be from same pair as RSA private key used to generate signature)
//SDR_PUBLIC 	//			cubPublicKey -		Size of the key
//SDR_PUBLIC 	// Output:  true if successful and signature is authentic, false if signature does not match or other error
//SDR_PUBLIC 	//-----------------------------------------------------------------------------
//SDR_PUBLIC 	bool CCrypto::RSAVerifySignatureSHA256( const uint8 *pubData, const uint32 cubData, 
//SDR_PUBLIC 									  const uint8 *pubSignature, const uint32 cubSignature, 
//SDR_PUBLIC 									  const uint8 *pubPublicKey, const uint32 cubPublicKey )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		VPROF_BUDGET( "CCrypto::RSAVerifySignature", VPROF_BUDGETGROUP_ENCRYPTION );
//SDR_PUBLIC 		Assert( pubData );
//SDR_PUBLIC 		Assert( pubSignature );
//SDR_PUBLIC 		Assert( pubPublicKey );
//SDR_PUBLIC 	
//SDR_PUBLIC 		bool bRet = false;	
//SDR_PUBLIC 	
//SDR_PUBLIC 		OneTimeCryptoInitOpenSSL();
//SDR_PUBLIC 		const uint8 *pPublicKeyPtr = pubPublicKey;
//SDR_PUBLIC 		if ( ::RSA *rsa = d2i_RSA_PUBKEY( NULL, &pPublicKeyPtr, cubPublicKey ) )
//SDR_PUBLIC 		{
//SDR_PUBLIC 			SHA256Digest_t digest;
//SDR_PUBLIC 			GenerateSHA256Digest( pubData, cubData, &digest );
//SDR_PUBLIC 			bRet = !!RSA_verify( NID_sha256, digest, sizeof( digest ), pubSignature, cubSignature, rsa );
//SDR_PUBLIC 			ERR_clear_error(); // if RSA_verify failed, we don't spew - could be invalid data.
//SDR_PUBLIC 			RSA_free( rsa );
//SDR_PUBLIC 		}
//SDR_PUBLIC 		DispatchOpenSSLErrors( "CCrypto::RSAVerifySignatureSHA256" );
//SDR_PUBLIC 	
//SDR_PUBLIC 		return bRet;
//SDR_PUBLIC 	}


//-----------------------------------------------------------------------------
// Purpose: Hex-encodes a block of data.  (Binary -> text representation.)  The output
//			is null-terminated and can be treated as a string.
// Input:	pubData -			Data to encode
//			cubData -			Size of data to encode
//			pchEncodedData -	Pointer to string buffer to store output in
//			cchEncodedData -	Size of pchEncodedData buffer
//-----------------------------------------------------------------------------
bool CCrypto::HexEncode( const uint8 *pubData, const uint32 cubData, char *pchEncodedData, uint32 cchEncodedData )
{
	VPROF_BUDGET( "CCrypto::HexEncode", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pubData );
	Assert( cubData );
	Assert( pchEncodedData );
	Assert( cchEncodedData > 0 );

	if ( cchEncodedData < ( ( cubData * 2 ) + 1 ) )
	{
		Assert( cchEncodedData >= ( cubData * 2 ) + 1 );  // expands to 2x input + NULL, must have room in output buffer
		*pchEncodedData = '\0';
		return false;
	}

	for ( uint32 i = 0; i < cubData; ++i )
	{
		uint8 c = *pubData++;
		*pchEncodedData++ = "0123456789ABCDEF"[c >> 4];
		*pchEncodedData++ = "0123456789ABCDEF"[c & 15];
	}
	*pchEncodedData = '\0';

	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Hex-decodes a block of data.  (Text -> binary representation.)  
// Input:	pchData -			Null-terminated hex-encoded string 
//			pubDecodedData -	Pointer to buffer to store output in
//			pcubDecodedData -	Pointer to variable that contains size of
//								output buffer.  At exit, is filled in with actual size
//								of decoded data.
//-----------------------------------------------------------------------------
bool CCrypto::HexDecode( const char *pchData, uint8 *pubDecodedData, uint32 *pcubDecodedData )
{
	VPROF_BUDGET( "CCrypto::HexDecode", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pchData );
	Assert( pubDecodedData );
	Assert( pcubDecodedData );
	Assert( *pcubDecodedData );

	const char *pchDataOrig = pchData;

	// Crypto++ HexDecoder silently skips unrecognized characters. So we do the same.
	uint32 cubOut = 0;
	uint32 cubOutMax = *pcubDecodedData;
	while ( cubOut < cubOutMax )
	{
		uint8 val = 0;
		while ( true ) // try to parse first hex character in a pair
		{
			uint8 c = (uint8)*pchData++;
			if ( (uint8)(c - '0') < 10 )
			{
				val = (uint8)(c - '0') * 16;
				break;
			}
			if ( (uint8)(c - 'a') < 6 )
			{
				val = (uint8)(c - 'a') * 16 + 160;
				break;
			}
			if ( (uint8)(c - 'A') < 6 )
			{
				val = (uint8)(c - 'A') * 16 + 160;
				break;
			}
			if ( c == 0 )
			{
				// end of input string. done decoding.
				*pcubDecodedData = cubOut;
				return true;
			}
			// some other character, ignore, keep trying to get first half.
		}

		while ( true ) // try to parse second hex character in a pair
		{
			uint8 c = (uint8)*pchData++;
			if ( (uint8)(c - '0') < 10 )
			{
				val += (uint8)(c - '0');
				break;
			}
			if ( (uint8)(c - 'a') < 6 )
			{
				val += (uint8)(c - 'a') + 10;
				break;
			}
			if ( (uint8)(c - 'A') < 6 )
			{
				val += (uint8)(c - 'A') + 10;
				break;
			}
			if ( c == 0 )
			{
				// end of input string. done decoding. half-byte 'val' is discarded.
				*pcubDecodedData = cubOut;
				return true;
			}
			// some other character, ignore. keep trying to get second half.
		}

		// got a full byte.
		pubDecodedData[cubOut++] = val;
	}

	if ( *pchData == 0 )
	{
		// end of input string simultaneous with end of output buffer. done decoding.
		*pcubDecodedData = cubOut;
		return true;
	}

	// Out of space.
	AssertMsg2( false, "CCrypto::HexDecode: insufficient output buffer (input length %u, output size %u)", (uint) V_strlen( pchDataOrig ), cubOutMax );
	return false;
}


static const int k_LineBreakEveryNGroups = 18; // line break every 18 groups of 4 characters (every 72 characters)

//-----------------------------------------------------------------------------
// Purpose: Returns the expected buffer size that should be passed to Base64Encode.
// Input:	cubData -			Size of data to encode
//			bInsertLineBreaks -	If line breaks should be inserted automatically
//-----------------------------------------------------------------------------
uint32 CCrypto::Base64EncodeMaxOutput( const uint32 cubData, const char *pszLineBreak )
{
	// terminating null + 4 chars per 3-byte group + line break after every 18 groups (72 output chars) + final line break
	uint32 nGroups = (cubData+2)/3;
	str_size cchRequired = 1 + nGroups*4 + ( pszLineBreak ? V_strlen(pszLineBreak)*(1+(nGroups-1)/k_LineBreakEveryNGroups) : 0 );
	return cchRequired;
}


//-----------------------------------------------------------------------------
// Purpose: Base64-encodes a block of data.  (Binary -> text representation.)  The output
//			is null-terminated and can be treated as a string.
// Input:	pubData -			Data to encode
//			cubData -			Size of data to encode
//			pchEncodedData -	Pointer to string buffer to store output in
//			cchEncodedData -	Size of pchEncodedData buffer
//			bInsertLineBreaks -	If "\n" line breaks should be inserted automatically
//-----------------------------------------------------------------------------
bool CCrypto::Base64Encode( const uint8 *pubData, uint32 cubData, char *pchEncodedData, uint32 cchEncodedData, bool bInsertLineBreaks )
{
	const char *pszLineBreak = bInsertLineBreaks ? "\n" : NULL;
	uint32 cchRequired = Base64EncodeMaxOutput( cubData, pszLineBreak );
	AssertMsg2( cchEncodedData >= cchRequired, "CCrypto::Base64Encode: insufficient output buffer for encoding, needed %d got %d\n", cchRequired, cchEncodedData );
	return Base64Encode( pubData, cubData, pchEncodedData, &cchEncodedData, pszLineBreak );
}

//-----------------------------------------------------------------------------
// Purpose: Base64-encodes a block of data.  (Binary -> text representation.)  The output
//			is null-terminated and can be treated as a string.
// Input:	pubData -			Data to encode
//			cubData -			Size of data to encode
//			pchEncodedData -	Pointer to string buffer to store output in
//			pcchEncodedData -	Pointer to size of pchEncodedData buffer; adjusted to number of characters written (before NULL)
//			pszLineBreak -		String to be inserted every 72 characters; empty string or NULL pointer for no line breaks
// Note: if pchEncodedData is NULL and *pcchEncodedData is zero, *pcchEncodedData is filled with the actual required length
// for output. A simpler approximation for maximum output size is (cubData * 4 / 3) + 5 if there are no linebreaks.
//-----------------------------------------------------------------------------
bool CCrypto::Base64Encode( const uint8 *pubData, uint32 cubData, char *pchEncodedData, uint32* pcchEncodedData, const char *pszLineBreak )
{
	VPROF_BUDGET( "CCrypto::Base64Encode", VPROF_BUDGETGROUP_ENCRYPTION );
	
	if ( pchEncodedData == NULL )
	{
		AssertMsg( *pcchEncodedData == 0, "NULL output buffer with non-zero size passed to Base64Encode" );
		*pcchEncodedData = Base64EncodeMaxOutput( cubData, pszLineBreak );
		return true;
	}
	
	const uint8 *pubDataEnd = pubData + cubData;
	char *pchEncodedDataStart = pchEncodedData;
	str_size unLineBreakLen = pszLineBreak ? V_strlen( pszLineBreak ) : 0;
	int nNextLineBreak = unLineBreakLen ? k_LineBreakEveryNGroups : INT_MAX;

	const char * const pszBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	uint32 cchEncodedData = *pcchEncodedData;
	if ( cchEncodedData == 0 )
		goto out_of_space;

	--cchEncodedData; // pre-decrement for the terminating null so we don't forget about it

	// input 3 x 8-bit, output 4 x 6-bit
	while ( pubDataEnd - pubData >= 3 )
	{
		if ( cchEncodedData < 4 + unLineBreakLen )
			goto out_of_space;
		
		if ( nNextLineBreak == 0 )
		{
			memcpy( pchEncodedData, pszLineBreak, unLineBreakLen );
			pchEncodedData += unLineBreakLen;
			cchEncodedData -= unLineBreakLen;
			nNextLineBreak = k_LineBreakEveryNGroups;
		}

		uint32 un24BitsData;
		un24BitsData  = (uint32) pubData[0] << 16;
		un24BitsData |= (uint32) pubData[1] << 8;
		un24BitsData |= (uint32) pubData[2];
		pubData += 3;

		pchEncodedData[0] = pszBase64Chars[ (un24BitsData >> 18) & 63 ];
		pchEncodedData[1] = pszBase64Chars[ (un24BitsData >> 12) & 63 ];
		pchEncodedData[2] = pszBase64Chars[ (un24BitsData >>  6) & 63 ];
		pchEncodedData[3] = pszBase64Chars[ (un24BitsData      ) & 63 ];
		pchEncodedData += 4;
		cchEncodedData -= 4;
		--nNextLineBreak;
	}

	// Clean up remaining 1 or 2 bytes of input, pad output with '='
	if ( pubData != pubDataEnd )
	{
		if ( cchEncodedData < 4 + unLineBreakLen )
			goto out_of_space;

		if ( nNextLineBreak == 0 )
		{
			memcpy( pchEncodedData, pszLineBreak, unLineBreakLen );
			pchEncodedData += unLineBreakLen;
			cchEncodedData -= unLineBreakLen;
		}

		uint32 un24BitsData;
		un24BitsData = (uint32) pubData[0] << 16;
		if ( pubData+1 != pubDataEnd )
		{
			un24BitsData |= (uint32) pubData[1] << 8;
		}
		pchEncodedData[0] = pszBase64Chars[ (un24BitsData >> 18) & 63 ];
		pchEncodedData[1] = pszBase64Chars[ (un24BitsData >> 12) & 63 ];
		pchEncodedData[2] = pubData+1 != pubDataEnd ? pszBase64Chars[ (un24BitsData >> 6) & 63 ] : '=';
		pchEncodedData[3] = '=';
		pchEncodedData += 4;
		cchEncodedData -= 4;
	}

	if ( unLineBreakLen )
	{
		if ( cchEncodedData < unLineBreakLen )
			goto out_of_space;
		memcpy( pchEncodedData, pszLineBreak, unLineBreakLen );
		pchEncodedData += unLineBreakLen;
		cchEncodedData -= unLineBreakLen;
	}

	*pchEncodedData = 0;
	*pcchEncodedData = pchEncodedData - pchEncodedDataStart;
	return true;

out_of_space:
	*pchEncodedData = 0;
	*pcchEncodedData = Base64EncodeMaxOutput( cubData, pszLineBreak );
	AssertMsg( false, "CCrypto::Base64Encode: insufficient output buffer (up to n*4/3+5 bytes required, plus linebreaks)" );
	return false;
}


//-----------------------------------------------------------------------------
// Purpose: Base64-decodes a block of data.  (Text -> binary representation.)  
// Input:	pchData -			Null-terminated hex-encoded string 
//			pubDecodedData -	Pointer to buffer to store output in
//			pcubDecodedData -	Pointer to variable that contains size of
//								output buffer.  At exit, is filled in with actual size
//								of decoded data.
// Note: if NULL is passed as the output buffer and *pcubDecodedData is zero, the function
// will calculate the actual required size and place it in *pcubDecodedData. A simpler upper
// bound on the required size is ( strlen(pchData)*3/4 + 1 ).
//-----------------------------------------------------------------------------
bool CCrypto::Base64Decode( const char *pchData, uint8 *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters )
{
	return Base64Decode( pchData, ~0u, pubDecodedData, pcubDecodedData, bIgnoreInvalidCharacters );
}

//-----------------------------------------------------------------------------
// Purpose: Base64-decodes a block of data.  (Text -> binary representation.)  
// Input:	pchData -			base64-encoded string, null terminated
//			cchDataMax -		maximum length of string unless a null is encountered first
//			pubDecodedData -	Pointer to buffer to store output in
//			pcubDecodedData -	Pointer to variable that contains size of
//								output buffer.  At exit, is filled in with actual size
//								of decoded data.
// Note: if NULL is passed as the output buffer and *pcubDecodedData is zero, the function
// will calculate the actual required size and place it in *pcubDecodedData. A simpler upper
// bound on the required size is ( strlen(pchData)*3/4 + 2 ).
//-----------------------------------------------------------------------------
bool CCrypto::Base64Decode( const char *pchData, uint32 cchDataMax, uint8 *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidCharacters )
{
	VPROF_BUDGET( "CCrypto::Base64Decode", VPROF_BUDGETGROUP_ENCRYPTION );
	
	uint32 cubDecodedData = *pcubDecodedData;
	uint32 cubDecodedDataOrig = cubDecodedData;

	if ( pubDecodedData == NULL )
	{
		AssertMsg( *pcubDecodedData == 0, "NULL output buffer with non-zero size passed to Base64Decode" );
		cubDecodedDataOrig = cubDecodedData = ~0u;
	}
	
	// valid base64 character range: '+' (0x2B) to 'z' (0x7A)
	// table entries are 0-63, -1 for invalid entries, -2 for '='
	static const signed char rgchInvBase64[] = {
		62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
		-1, -1, -1, -2, -1, -1, -1,  0,  1,  2,  3,  4,  5,  6,  7,
		 8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
		23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31,
		32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46,
		47, 48, 49, 50, 51
	};
	COMPILE_TIME_ASSERT( V_ARRAYSIZE(rgchInvBase64) == 0x7A - 0x2B + 1 );

	uint32 un24BitsWithSentinel = 1;
	while ( cchDataMax-- > 0 )
	{
		char c = *pchData++;

		if ( (uint8)(c - 0x2B) >= V_ARRAYSIZE( rgchInvBase64 ) )
		{
			if ( c == '\0' )
				break;

			if ( !bIgnoreInvalidCharacters && !( c == '\r' || c == '\n' || c == '\t' || c == ' ' ) )
				goto decode_failed;
			else
				continue;
		}

		c = rgchInvBase64[(uint8)(c - 0x2B)];
		if ( (signed char)c < 0 )
		{
			if ( (signed char)c == -2 ) // -2 -> terminating '='
				break;

			if ( !bIgnoreInvalidCharacters )
				goto decode_failed;
			else
				continue;
		}

		un24BitsWithSentinel <<= 6;
		un24BitsWithSentinel |= c;
		if ( un24BitsWithSentinel & (1<<24) )
		{
			if ( cubDecodedData < 3 ) // out of space? go to final write logic
				break;
			if ( pubDecodedData )
			{
				pubDecodedData[0] = (uint8)( un24BitsWithSentinel >> 16 );
				pubDecodedData[1] = (uint8)( un24BitsWithSentinel >> 8);
				pubDecodedData[2] = (uint8)( un24BitsWithSentinel );
				pubDecodedData += 3;
			}
			cubDecodedData -= 3;
			un24BitsWithSentinel = 1;
		}
	}

	// If un24BitsWithSentinel contains data, output the remaining full bytes
	if ( un24BitsWithSentinel >= (1<<6) )
	{
		// Possibilities are 3, 2, 1, or 0 full output bytes.
		int nWriteBytes = 3;
		while ( un24BitsWithSentinel < (1<<24) )
		{
			nWriteBytes--;
			un24BitsWithSentinel <<= 6;
		}

		// Write completed bytes to output
		while ( nWriteBytes-- > 0 )
		{
			if ( cubDecodedData == 0 )
			{
				AssertMsg( false, "CCrypto::Base64Decode: insufficient output buffer (up to n*3/4+2 bytes required)" );
				goto decode_failed;
			}
			if ( pubDecodedData )
			{
				*pubDecodedData++ = (uint8)(un24BitsWithSentinel >> 16);
			}
			--cubDecodedData;
			un24BitsWithSentinel <<= 8;
		}
	}
	
	*pcubDecodedData = cubDecodedDataOrig - cubDecodedData;
	return true;

decode_failed:
	*pcubDecodedData = cubDecodedDataOrig - cubDecodedData;
	return false;
}

inline bool BParsePEMHeaderOrFooter( const char *&pchPEM, const char *pchEnd, const char *pszBeginOrEnd, const char *pszExpectedType )
{
	// Eat any leading whitespace of any kind
	for (;;)
	{
		if ( pchPEM >= pchEnd || *pchPEM == '\0' )
			return false;
		if ( !V_isspace( *pchPEM ) )
			break;
		++pchPEM;
	}

	// Require at least one dash, and each any number of them
	if ( pchPEM >= pchEnd || *pchPEM != '-' )
		return false;
	while ( *pchPEM == '-' )
	{
		++pchPEM;
		if ( pchPEM >= pchEnd || *pchPEM == '\0' )
			return false;
	}

	// Eat tabs and spaces
	for (;;)
	{
		if ( pchPEM >= pchEnd || *pchPEM == '\0' )
			return false;
		if ( *pchPEM != ' ' && *pchPEM != '\t' )
			break;
		++pchPEM;
	}

	// Require and eat the word "BEGIN" or "END"
	int l = V_strlen( pszBeginOrEnd );
	if ( pchPEM + l >= pchEnd || V_strnicmp( pchPEM, pszBeginOrEnd, l ) != 0 )
		return false;
	pchPEM += l;

	// Eat tabs and spaces
	for (;;)
	{
		if ( pchPEM >= pchEnd || *pchPEM == '\0' )
			return false;
		if ( *pchPEM != ' ' && *pchPEM != '\t' )
			break;
		++pchPEM;
	}

	// Remember where the type field was
	const char *pszType = pchPEM;

	// Skip over the type, to the ending dashes.
	// Fail if we hit end of input or end of line before we find the dash
	for (;;)
	{
		if ( pchPEM >= pchEnd || *pchPEM == '\0' || *pchPEM == '\r' || *pchPEM == '\n' )
			return false;
		if ( *pchPEM == '-' )
			break;
		++pchPEM;
	}

	// Confirm the type is what they expected
	if ( pszExpectedType )
	{
		int cchExpectedType = V_strlen( pszExpectedType );
		if ( pszType + cchExpectedType > pchPEM || V_strnicmp( pszType, pszExpectedType, cchExpectedType ) != 0 )
			return false;
	}

	// Eat any remaining dashes
	while ( pchPEM < pchEnd && *pchPEM == '-' )
		++pchPEM;

	// Eat any trailing whitespace of any kind
	while ( pchPEM < pchEnd && V_isspace( *pchPEM ) )
		++pchPEM;

	// OK
	return true;
}

//-----------------------------------------------------------------------------
const char *CCrypto::LocatePEMBody( const char *pchPEM, uint32 *pcch, const char *pszExpectedType )
{
	if ( !pchPEM || !pcch || !*pcch )
		return nullptr;

	const char *pchEnd = pchPEM + *pcch;
	if ( !BParsePEMHeaderOrFooter( pchPEM, pchEnd, "BEGIN", pszExpectedType ) )
		return nullptr;

	const char *pchBody = pchPEM;

	// Scan until we hit a character that isn't a valid
	for (;;)
	{
		if ( pchPEM >= pchEnd )
			return nullptr;
		if ( *pchPEM == '-' )
			break;
		++pchPEM;
	}

	// Save size of body
	uint32 cchBody = pchPEM - pchBody;

	// Eat the footer
	if ( !BParsePEMHeaderOrFooter( pchPEM, pchEnd, "END", pszExpectedType ) )
		return nullptr;

	// Should we check for leftover garbage here?
	// let's just ignore it.

	// Success, return body location and size to caller
	*pcch = cchBody;
	return pchBody;
}


//-----------------------------------------------------------------------------
// Purpose: Generate a SHA1 hash
// Input:	pchInput -			Plaintext string of item to hash (null terminated)
//			pOutDigest -		Pointer to receive hashed digest output
//-----------------------------------------------------------------------------
bool CCrypto::GenerateSHA1Digest( const uint8 *pubInput, const int cubInput, SHADigest_t *pOutDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateSHA1Digest", VPROF_BUDGETGROUP_ENCRYPTION );
	//Assert( pubInput );
	Assert( cubInput >= 0 );
	Assert( pOutDigest );

	SHA_CTX c;
	if ( !SHA1_Init( &c ) )
		return false;
	SHA1_Update( &c, pubInput, cubInput );
	SHA1_Final( *pOutDigest, &c );
	SecureZeroMemory( &c, sizeof(c) );
	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Generate a SHA256 hash
// Input:	pchInput -			Plaintext string of item to hash (null terminated)
//			pOutDigest -		Pointer to receive hashed digest output
//-----------------------------------------------------------------------------
bool CCrypto::GenerateSHA256Digest( const uint8 *pubInput, const int cubInput, SHA256Digest_t *pOutDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateSHA256Digest", VPROF_BUDGETGROUP_ENCRYPTION );
	//Assert( pubInput );
	Assert( cubInput >= 0 );
	Assert( pOutDigest );

	SHA256_CTX c;
	if ( !SHA256_Init( &c ) )
		return false;
	SHA256_Update( &c, pubInput, cubInput );
	SHA256_Final( *pOutDigest, &c );
	SecureZeroMemory( &c, sizeof(c) );
	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Generate a hash Salt - be careful, over-writing an existing salt
// will render the hashed value unverifiable.
//-----------------------------------------------------------------------------
bool CCrypto::GenerateSalt( Salt_t *pSalt )
{
	VPROF_BUDGET( "CCrypto::GenerateSalt", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pSalt );

	bool bSuccess = GenerateRandomBlock( *pSalt, sizeof(Salt_t) );
	return bSuccess;
}


//-----------------------------------------------------------------------------
// Purpose: Generate a SHA1 hash using a salt.
// Input:	pchInput -			Plaintext string of item to hash (null terminated)
//			pSalt -				Salt
//			pOutDigest -		Pointer to receive salted digest output
//-----------------------------------------------------------------------------
bool CCrypto::GenerateSaltedSHA1Digest( const char *pchInput, const Salt_t *pSalt, SHADigest_t *pOutDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateSaltedSHA1Digest", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pchInput );
	Assert( pSalt );
	Assert( pOutDigest );

	int iInputLen = V_strlen( pchInput );
	uint8 *pubSaltedInput = new uint8[ iInputLen + sizeof( Salt_t ) ];

	// Insert half the salt before the input string and half at the end.
	// This is probably unnecessary (to split the salt) but we're stuck with it for historical reasons.
	uint8 *pubCursor = pubSaltedInput;
	V_memcpy( pubCursor, (uint8 *)pSalt, sizeof(Salt_t) / 2 );
	pubCursor += sizeof( Salt_t ) / 2;
	V_memcpy( pubCursor, pchInput, iInputLen );
	pubCursor += iInputLen;
	V_memcpy( pubCursor, (uint8 *)pSalt + sizeof(Salt_t) / 2, sizeof(Salt_t) / 2 );

	bool bSuccess = GenerateSHA1Digest( pubSaltedInput, iInputLen + sizeof( Salt_t ), pOutDigest );

	delete [] pubSaltedInput;

	return bSuccess;
}


#ifdef ENABLE_CRYPTO_SCRYPT
//-----------------------------------------------------------------------------
// Purpose: Wrap the external scrypt() function for use in Steam
//-----------------------------------------------------------------------------
static void scrypt_VALVE( const uint8 *pubInput, uint32 nInputLen, const uint8 *pubSalt, uint32 cubSalt, unsigned char Nfactor, unsigned char rfactor, unsigned char pfactor, unsigned char *out, size_t bytes )
{
	// Due to global constructor ordering, etc, there is no place we can guarantee
	// that these assignments will occur exactly once at the right time. So instead
	// we just repeatedly slam these function pointers ahead of every call. They're
	// atomic pointer-sized writes, and writing a constant value; no big deal.
	scrypt_set_fatal_error( []( const char * pMsg ) { AssertFatalMsg( false, pMsg ); } );
	scrypt_malloc_VALVE = []( size_t n ) -> void* { return PvAlloc( n ); };
	scrypt_free_VALVE = []( void* p ) -> void { return FreePv( p ); };
	scrypt( pubInput, nInputLen, pubSalt, cubSalt, Nfactor, rfactor, pfactor, out, bytes );
}

//-----------------------------------------------------------------------------
// Purpose: Wrap the external scrypt() function for use in Steam, tuned for
// maximum reasonable level of resistance to cracking attempts. Takes approx
// 0.5 sec and 256 MB to compute on a modern 64-bit 3.0 GHz Intel CPU core.
//-----------------------------------------------------------------------------
bool CCrypto::GenerateStrongScryptDigest( const char *pchInput, int32 nInputLen, const uint8 *pubSalt, uint32 cubSalt, SHA256Digest_t *pDigestOut )
{
	VPROF_BUDGET( "CCrypto::GenerateStrongScryptDigest", VPROF_BUDGETGROUP_ENCRYPTION );
	
	if ( nInputLen < 0 )
		nInputLen = V_strlen( pchInput );

	// We hardcode our scrypt parameters. The official recommendations from the original
	// 2009 paper were 13/3/0 in our notation, which means 16 MB of RAM. Memory and CPU
	// usage both go up exponentially with the first parameter, so 16/3/0 is 8x stronger
	// than the 2009 recommendation. 16/4/0 doubles memory size again, requiring 256 MB
	// of RAM for one evaluation, to harden against GPU-based or custom-ASIC cracking.
	const int kOverallParamLog2MinusOne = 16;
	const int kMemorySizeParamLog2 = 4;
	const int kParallelismParamLog2 = 0;
	scrypt_VALVE( (const uint8*)pchInput, (uint32)nInputLen, pubSalt, cubSalt, kOverallParamLog2MinusOne, kMemorySizeParamLog2, kParallelismParamLog2, *pDigestOut, sizeof(SHA256Digest_t) );
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Wrap the external scrypt() function for use in Steam, tuned for
// a moderate and reasonable level of resistance to cracking attempts without
// bottlenecking our login servers. Tuned to take approx 10-20ms per execution.
//-----------------------------------------------------------------------------
bool CCrypto::GenerateFastScryptDigest( const char *pchInput, int32 nInputLen, const uint8 *pubSalt, uint32 cubSalt, SHA256Digest_t *pDigestOut )
{
	VPROF_BUDGET( "CCrypto::GenerateFastScryptDigest", VPROF_BUDGETGROUP_ENCRYPTION );

	if ( nInputLen < 0 )
		nInputLen = V_strlen( pchInput );

	// We hardcode our scrypt parameters. The official recommendations from the original
	// 2009 paper were 13/3/0 in our notation, which means 16 MB of RAM. We weaken this
	// slightly by halving the overall parameter (meaning 8 MB RAM, half the CPU work) to
	// keep execution under 20ms on our servers. Still far, far stronger than PBKDF2.
	const int kOverallParamLog2MinusOne = 12;
	const int kMemorySizeParamLog2 = 3;
	const int kParallelismParamLog2 = 0;
	scrypt_VALVE( (const uint8*)pchInput, (uint32)nInputLen, pubSalt, cubSalt, kOverallParamLog2MinusOne, kMemorySizeParamLog2, kParallelismParamLog2, *pDigestOut, sizeof( SHA256Digest_t ) );
	return true;
}
#endif


//-----------------------------------------------------------------------------
// Purpose: Generates a cryptographiacally random block of data fit for any use.
// NOTE: Function terminates process on failure rather than returning false!
//-----------------------------------------------------------------------------
bool CCrypto::GenerateRandomBlock( void *pvDest, int cubDest )
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

	// NOTE: Function terminates process on failure rather than returning false.
	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Generate a keyed-hash MAC using SHA1
// Input:	pubData -			Plaintext data to digest
//			cubData -			length of data
//			pubKey -			key to use in HMAC
//			cubKey -			length of key
//			pOutDigest -		Pointer to receive hashed digest output
//-----------------------------------------------------------------------------
bool CCrypto::GenerateHMAC( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHADigest_t *pOutputDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateHMAC", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pubData );
	Assert( cubData > 0 );
	Assert( pubKey );
	Assert( cubKey > 0 );
	Assert( pOutputDigest );

	CHMACSHA1Impl hmac;
	hmac.Init( pubKey, cubKey );
	hmac.Update( pubData, cubData );
	hmac.Final( *pOutputDigest );
	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Generate a keyed-hash MAC using SHA-256
//-----------------------------------------------------------------------------
bool CCrypto::GenerateHMAC256( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHA256Digest_t *pOutputDigest )
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
	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Generate a keyed-hash MAC using MD5
//-----------------------------------------------------------------------------
bool CCrypto::GenerateHMACMD5( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, MD5Digest_t *pOutputDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateHMACMD5", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pubData );
	Assert( cubData > 0 );
	Assert( pubKey );
	Assert( cubKey > 0 );
	Assert( pOutputDigest );
	if ( !pubData || !cubData || !pubKey || !cubKey || !pOutputDigest )
		return false;

	CHMACMD5Impl hmac;
	hmac.Init( pubKey, cubKey );
	hmac.Update( pubData, cubData );
	hmac.Final( *pOutputDigest );
	return true;
}


#ifdef DBGFLAG_VALIDATE
//-----------------------------------------------------------------------------
// Purpose: validates memory structures
//-----------------------------------------------------------------------------
void CCrypto::ValidateStatics( CValidator &validator, const char *pchName )
{
}
#endif // DBGFLAG_VALIDATE

/* SDR_PUBLIC

//-----------------------------------------------------------------------------
// Purpose: Given a plaintext password, check whether it matches an existing
// hash
//-----------------------------------------------------------------------------
bool CCrypto::BValidatePasswordHash( const char *pchInput, EPasswordHashAlg hashType, const PasswordHash_t &DigestStored, const Salt_t &Salt, PasswordHash_t *pDigestComputed )
{
	VPROF_BUDGET( "CCrypto::BValidatePasswordHash", VPROF_BUDGETGROUP_ENCRYPTION );

	bool bResult = false;
	size_t cDigest = k_HashLengths[hashType];
	Assert( cDigest != 0 );
	PasswordHash_t tmpDigest;
	PasswordHash_t *pOutputDigest = pDigestComputed;
	if ( pOutputDigest == NULL )
	{
		pOutputDigest = &tmpDigest;
	}

	BGeneratePasswordHash( pchInput, hashType, Salt, *pOutputDigest );
	bResult = ( 0 == V_memcmp( &DigestStored, pOutputDigest, cDigest ) );

	return bResult;
}

//-----------------------------------------------------------------------------
// Purpose: Given a plaintext password and salt, generate a password hash of 
// the requested type.
//-----------------------------------------------------------------------------
bool CCrypto::BGeneratePasswordHash( const char *pchInput, EPasswordHashAlg hashType, const Salt_t &Salt, PasswordHash_t &OutPasswordHash )
{
	VPROF_BUDGET( "CCrypto::BGeneratePasswordHash", VPROF_BUDGETGROUP_ENCRYPTION );

	bool bResult = false;
	size_t cDigest = k_HashLengths[hashType];

	switch ( hashType )
	{
	case k_EHashSHA1:
		bResult = CCrypto::GenerateSaltedSHA1Digest( pchInput, &Salt, (SHADigest_t *)&OutPasswordHash.sha );
		break;
	case k_EHashBigPassword:
	{
		//
		// This is a fake algorithm to test widening of the column.  It's a salted SHA-1 hash with 0x01 padding
		// on either side of it.
		//
		size_t cDigestSHA1 = k_HashLengths[k_EHashSHA1];
		size_t cPadding = ( cDigest - cDigestSHA1 ) / 2;
		
		AssertMsg( ( ( cDigest - cDigestSHA1 ) % 2 ) == 0, "Invalid hash width for k_EHashBigPassword, needs to be even." );

		CCrypto::GenerateSaltedSHA1Digest( pchInput, &Salt, (SHADigest_t *)( (uint8 *)&OutPasswordHash.bigpassword + cPadding ) );
		memset( (uint8 *)&OutPasswordHash, 0x01, cPadding );
		memset( (uint8 *)&OutPasswordHash + cPadding + cDigestSHA1 , 0x01, cPadding );
		bResult = true;
		break;
	}
	case k_EHashPBKDF2_1000:
		bResult = CCrypto::BGeneratePBKDF2Hash( pchInput, Salt, 1000, OutPasswordHash );
		break;
	case k_EHashPBKDF2_5000:
		bResult = CCrypto::BGeneratePBKDF2Hash( pchInput, Salt, 5000, OutPasswordHash );
		break;
	case k_EHashPBKDF2_10000:
		bResult = CCrypto::BGeneratePBKDF2Hash( pchInput, Salt, 10000, OutPasswordHash );
		break;
	case k_EHashSHA1WrappedWithPBKDF2_10000:
		bResult = CCrypto::BGenerateWrappedSHA1PasswordHash( pchInput, Salt, 10000, OutPasswordHash );
		break;
#ifdef ENABLE_CRYPTO_SCRYPT
	case k_EHashScryptFast:
		bResult = CCrypto::GenerateFastScryptDigest( pchInput, -1, Salt, sizeof(Salt), &OutPasswordHash.pbkdf2 );
		break;
#endif
	default:
		AssertMsg1( false, "Invalid password hash type %u passed to BGeneratePasswordHash\n", hashType );
		bResult = false;
	}

	return bResult;
}

//-----------------------------------------------------------------------------
// Purpose: Given a plaintext password and salt and a count of rounds, generate a PBKDF2 hash
//			with the requested number of rounds.
//-----------------------------------------------------------------------------
bool CCrypto::BGeneratePBKDF2Hash( const char* pchInput, const Salt_t &Salt, unsigned int rounds, PasswordHash_t &OutPasswordHash )
{
	CalculatePBKDF2SHA256( pchInput, V_strlen( pchInput ), Salt, sizeof(Salt_t), OutPasswordHash.pbkdf2, rounds );
	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Given a plaintext password and salt and a count of rounds, generate a SHA1 hash wrapped with
//			a PBKDF2 hash with the specified number of rounds.
//			Used to provide a stronger password hash for accounts that haven't logged in in a while.
//-----------------------------------------------------------------------------
bool CCrypto::BGenerateWrappedSHA1PasswordHash( const char *pchInput, const Salt_t &Salt, unsigned int rounds, PasswordHash_t &OutPasswordHash )
{
	bool bResult;
	bResult = CCrypto::GenerateSaltedSHA1Digest( pchInput, &Salt, (SHADigest_t *)&OutPasswordHash.sha );
	CalculatePBKDF2SHA256( (const byte *)&OutPasswordHash.sha, sizeof( OutPasswordHash.sha ), Salt, sizeof(Salt_t), OutPasswordHash.pbkdf2, rounds );
	return bResult;
}

//-----------------------------------------------------------------------------
// Purpose: Given an existing password hash and salt, attempt to construct a stronger
//			password hash and return the new hash type.
//
//			Currently the only transformation available is from a SHA1 (or BigPassword)
//			hash to a PBKDF2 hash with 10,000 rounds.  In the future this function
//			may be extended to allow additional transformations.
//-----------------------------------------------------------------------------
bool CCrypto::BUpgradeOrWrapPasswordHash( PasswordHash_t &InPasswordHash, EPasswordHashAlg hashTypeIn, const Salt_t &Salt, PasswordHash_t &OutPasswordHash, EPasswordHashAlg &hashTypeOut )
{
	bool bResult = false;

	if ( hashTypeIn == k_EHashSHA1 || hashTypeIn == k_EHashBigPassword )
	{
		//
		// Can wrap a SHA1 hash with any PBKDF variant, but right now only 10,000 rounds is
		// implemented.
		//
		if ( hashTypeOut == k_EHashPBKDF2_10000 )
		{
			hashTypeOut = k_EHashSHA1WrappedWithPBKDF2_10000;

			byte * pbHash;
			if ( hashTypeIn == k_EHashSHA1 )
			{
				pbHash = (byte *)&InPasswordHash.sha;
			}
			else
			{
				//
				// Need to unroll BigPasswordHash into unpadded SHA1
				//
				size_t cDigest = k_HashLengths[k_EHashBigPassword];
				size_t cDigestSHA1 = k_HashLengths[k_EHashSHA1];
				size_t cPadding = ( cDigest - cDigestSHA1 ) / 2;

				AssertMsg( ( ( cDigest - cDigestSHA1 ) % 2 ) == 0, "Invalid hash width for k_EHashBigPassword, needs to be even." );
				pbHash = (byte *)&InPasswordHash.sha + cPadding;
			}

			// PBKDF2 is deterministic and has no invalid inputs. This cannot fail.
			CalculatePBKDF2SHA256( pbHash, k_HashLengths[k_EHashSHA1], Salt, sizeof(Salt), OutPasswordHash.pbkdf2, 10000 );
			bResult = true;
		}
		else
		{
			Assert( hashTypeOut == k_EHashPBKDF2_10000 );
			bResult = false;
		}
	}
	else
	{
		bResult = false;
		Assert( false );
	}

	return bResult;
}

*/

//-----------------------------------------------------------------------------
// Purpose: Initialize an RSA key object from a hex encoded string
// Input:	pchEncodedKey -		Pointer to the hex encoded key string
// Output:  true if successful, false if initialization failed
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::SetFromHexEncodedString( const char *pchEncodedKey )
{
	uint32 cubKey = V_strlen( pchEncodedKey ) / 2 + 1;
	EnsureCapacity( cubKey );

	bool bSuccess = CCrypto::HexDecode( pchEncodedKey, m_pbKey, &cubKey );
	if ( bSuccess )
	{
		m_cubKey = cubKey;
	}
	else
	{
		Wipe();
	}
	return bSuccess;
}


//-----------------------------------------------------------------------------
// Purpose: Initialize an RSA key object from a base-64 encoded string
// Input:	pchEncodedKey -		Pointer to the base-64 encoded key string
// Output:  true if successful, false if initialization failed
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::SetFromBase64EncodedString( const char *pchEncodedKey )
{
	uint32 cubKey = V_strlen( pchEncodedKey ) * 3 / 4 + 1;
	EnsureCapacity( cubKey );

	bool bSuccess = CCrypto::Base64Decode( pchEncodedKey, m_pbKey, &cubKey );
	if ( bSuccess )
	{
		m_cubKey = cubKey;
	}
	else
	{
		Wipe();
	}
	return bSuccess;
}


//-----------------------------------------------------------------------------
// Purpose: Initialize an RSA key object from a buffer
// Input:	pData -				Pointer to the buffer
//			cbData -			Length of the buffer
// Output:  true if successful, false if initialization failed
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::Set( const void* pData, const uint32 cbData )
{
	if ( pData == m_pbKey )
		return true;
	Wipe();
	EnsureCapacity( cbData );
	V_memcpy( m_pbKey, pData, cbData );
	m_cubKey = cbData;
	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Serialize an RSA key object to a buffer, in PEM format
// Input:	pchPEMData -		Pointer to string buffer to store output in (or NULL to just calculate required size)
//			cubPEMData -		Size of pchPEMData buffer
//			pcubPEMData -		Pointer to number of bytes written to pchPEMData (including terminating nul), or
//								required size of pchPEMData if it is NULL or not big enough.
// Output:  true if successful, false if it failed
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::GetAsPEM( char *pchPEMData, uint32_t cubPEMData, uint32_t *pcubPEMData ) const
{
	CAutoWipeBuffer bufTemp;

	if ( !IsValid() )
		return false;

	const char *pchPrefix = "", *pchSuffix = "";
	uint32 cubBinary = m_cubKey;
	const uint8 *pbBinary = m_pbKey;
	if ( m_eKeyType == k_ECryptoKeyTypeRSAPublic )
	{
		pchPrefix = "-----BEGIN PUBLIC KEY-----";
		pchSuffix = "-----END PUBLIC KEY-----";
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeRSAPrivate )
	{
		pchPrefix = "-----BEGIN PRIVATE KEY-----";
		pchSuffix = "-----END PRIVATE KEY-----";
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
	{
		pchPrefix = k_szOpenSSHPrivatKeyPEMHeader;
		pchSuffix = k_szOpenSSHPrivatKeyPEMFooter;

		OpenSSHBinaryWriteEd25519Private( bufTemp, m_pbKey );
		cubBinary = bufTemp.TellPut();
		pbBinary = (const uint8 *)bufTemp.Base();
	}
	else
	{
		Assert( false ); // nonsensical to call this on non-RSA keys
		return false;
	}
	
	uint32_t uRequiredBytes = V_strlen( pchPrefix ) + 2 + V_strlen( pchSuffix ) + 2 + CCrypto::Base64EncodeMaxOutput( cubBinary, "\r\n" );
	if ( pcubPEMData )
		*pcubPEMData = uRequiredBytes;

	if ( pchPEMData )
	{
		if ( cubPEMData < uRequiredBytes )
			return false;

		V_strncpy( pchPEMData, pchPrefix, cubPEMData );
		V_strncat( pchPEMData, "\r\n", cubPEMData );
		uint32_t uRemainingBytes = cubPEMData - V_strlen( pchPEMData );
	
		if ( !CCrypto::Base64Encode( pbBinary, cubBinary, pchPEMData + V_strlen( pchPEMData ), &uRemainingBytes, "\r\n" ) )
			return false;

		V_strncat( pchPEMData, pchSuffix, cubPEMData );
		V_strncat( pchPEMData, "\r\n", cubPEMData );
		if ( pcubPEMData )
			*pcubPEMData = V_strlen( pchPEMData ) + 1;
	}

	return true;
}

bool CECSigningPublicKey::GetAsOpenSSHAuthorizedKeys( char *pchData, uint32 cubData, uint32 *pcubData, const char *pszComment ) const
{
	if ( !IsValid() )
		return false;

	int cchComment = pszComment ? V_strlen( pszComment ) : 0;

	CUtlBuffer bufBinary;
	OpenSSHBinaryEd25519WritePublic( bufBinary, m_pbKey );

	static const char pchPrefix[] = "ssh-ed25519 ";

	uint32_t uRequiredBytes =
		V_strlen( pchPrefix )
		+ CCrypto::Base64EncodeMaxOutput( bufBinary.TellPut(), "" )
		+ ( cchComment > 0 ? 1 : 0 ) // space
		+ cchComment
		+ 1; // '\0'
	if ( pcubData )
		*pcubData = uRequiredBytes;

	if ( pchData )
	{
		if ( cubData < uRequiredBytes )
			return false;

		V_strncpy( pchData, pchPrefix, cubData );
		uint32_t uRemainingBytes = cubData - V_strlen( pchData );
	
		if ( !CCrypto::Base64Encode( (const uint8 *)bufBinary.Base(), bufBinary.TellPut(), pchData + V_strlen( pchData ), &uRemainingBytes, "" ) )
			return false;

		if ( pszComment )
		{
			V_strncat( pchData, " ", cubData );
			V_strncat( pchData, pszComment, cubData );
		}
		if ( pcubData )
			*pcubData = V_strlen( pchData ) + 1;
	}

	return true;
}


//-----------------------------------------------------------------------------
// Purpose: Compare two keys for equality
// Output:  true if the keys are identical
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::operator==( const CCryptoKeyBase &rhs ) const
{
	return ( m_eKeyType == rhs.m_eKeyType ) && ( m_cubKey == rhs.m_cubKey ) && ( V_memcmp( m_pbKey, rhs.m_pbKey, m_cubKey ) == 0 );
}


//-----------------------------------------------------------------------------
// Purpose: Make sure there's enough space in the allocated key buffer
//			for the designated data size.
//-----------------------------------------------------------------------------
void CCryptoKeyBase::EnsureCapacity( uint32 cbData )
{
	Wipe();
	m_pbKey = new uint8[cbData];

	//
	// Note: Intentionally not setting m_cubKey here - it's the size
	// of the key not the size of the allocation.
	//
}


#ifdef DBGFLAG_VALIDATE
void CCryptoKeyBase::Validate( CValidator &validator, const char *pchName ) const
{
	validator.ClaimMemory( m_pbKey );
}
#endif // DBGFLAG_VALIDATE

//-----------------------------------------------------------------------------
// Purpose: Load key from file (best-guess at PKCS#8 PEM, Base64, hex, or binary)
//-----------------------------------------------------------------------------
bool CCryptoKeyBase::LoadFromAndWipeBuffer( void *pBuffer, size_t cBytes )
{
	Wipe();

	Assert( cBytes < 1024*1024*10 ); // sanity check: 10 MB key? no thanks
	if ( cBytes > 0 && cBytes < 1024*1024*10 )
	{

		// Ensure null termination
		char *pchBase = (char*) malloc( cBytes + 1 );
		V_memcpy( pchBase, pBuffer, cBytes );
		pchBase[cBytes] = '\0';

		if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
		{
			// Fixed key size
			EnsureCapacity( 64 );

			CAutoWipeBuffer buf;
			if (
				V_strstr( pchBase, k_szOpenSSHPrivatKeyPEMHeader )
				&& DecodePEMBody( pchBase, (uint32)cBytes, buf )
				&& BParseOpenSSHBinaryEd25519Private( buf, m_pbKey )
			) {
				m_cubKey = 64;

				#ifdef ENABLE_CRYPTO_25519

					// Check that the public key matches the private one.
					// (And also that all of our code works.)
					uint8 arCheckPublic[ 32 ];
					V_memcpy( arCheckPublic, m_pbKey, 32 );
					(( CEC25519PrivateKeyBase * )this )->RebuildFromPrivateData( m_pbKey+32 );
					if ( V_memcmp( arCheckPublic, m_pbKey, 32 ) != 0 )
					{
						AssertMsg( false, "Ed25519 key public doesn't match private!" );
						Wipe();
					}
				#endif
			}
			else
			{
				Wipe();
			}
		}
		else if ( m_eKeyType == k_ECryptoKeyTypeSigningPublic )
		{
			// Fixed key size
			EnsureCapacity( 32 );

			// OpenSSH authorized_keys format?

			CAutoWipeBuffer buf;
			int idxStart = -1, idxEnd = -1;
			sscanf( pchBase, "ssh-ed25519 %nAAAA%*s%n", &idxStart, &idxEnd );
			if (
				idxStart > 0
				&& idxEnd > idxStart
				&& DecodeBase64ToBuf( pchBase + idxStart, idxEnd-idxStart, buf )
				&& BParseOpenSSHBinaryEd25519Public( buf, m_pbKey )
			)
			{
				// OK
				m_cubKey = 32;
			}
			else
			{
				Wipe();
			}
		}
		else if ( m_eKeyType != k_ECryptoKeyTypeRSAPublic && m_eKeyType != k_ECryptoKeyTypeRSAPrivate )
		{
			// TODO?
		}
		else
		{
			// strip PEM header if we find it
			const char *pchPEMPrefix = ( m_eKeyType == k_ECryptoKeyTypeRSAPrivate ) ? "-----BEGIN PRIVATE KEY-----" : "-----BEGIN PUBLIC KEY-----";
			if ( const char *pchData = V_strstr( pchBase, pchPEMPrefix ) )
			{
				SetFromBase64EncodedString( V_strstr( pchData, "KEY-----" ) + 8 );
			}
			else if ( pchBase[0] == 'M' && pchBase[1] == 'I' )
			{
				SetFromBase64EncodedString( pchBase );
			}
			else if ( pchBase[0] == 0x30 && (uint8)pchBase[1] == 0x82 )
			{
				Set( (const uint8*)pchBase, (uint32)cBytes );
			}
			else if ( pchBase[0] == '3' && pchBase[1] == '0' && pchBase[2] == '8' && pchBase[3] == '2' )
			{
				SetFromHexEncodedString( pchBase );
			}
		}

		SecureZeroMemory( pchBase, cBytes );
		free( pchBase );
	}

	// Wipe input buffer
	SecureZeroMemory( pBuffer, cBytes );
	return IsValid();
}

//SDR_PUBLIC //-----------------------------------------------------------------------------
//SDR_PUBLIC void CRSAPrivateKey::GetPublicKey( CRSAPublicKey *pPublicKey ) const
//SDR_PUBLIC {
//SDR_PUBLIC 	pPublicKey->Wipe();
//SDR_PUBLIC 
//SDR_PUBLIC 	::RSA *rsa = OpenSSL_RSAFromPKCS8PrivKey( m_pbKey, m_cubKey );
//SDR_PUBLIC 	if ( rsa )
//SDR_PUBLIC 	{
//SDR_PUBLIC 		CUtlMemory<uint8> mem;
//SDR_PUBLIC 
//SDR_PUBLIC 		// Encode public key in portable X.509 public key DER format
//SDR_PUBLIC 		{
//SDR_PUBLIC 			int lengthToEncodePub = i2d_RSA_PUBKEY( rsa, NULL );
//SDR_PUBLIC 			Assert( lengthToEncodePub > 0 && lengthToEncodePub < 1024*1024 ); /* 1 MB sanity check */
//SDR_PUBLIC 			if ( lengthToEncodePub > 0 && lengthToEncodePub < 1024*1024 )
//SDR_PUBLIC 			{
//SDR_PUBLIC 				mem.EnsureCapacity( lengthToEncodePub + 1 );
//SDR_PUBLIC 				uint8 *pPtr = mem.Base();
//SDR_PUBLIC 				(void)i2d_RSA_PUBKEY( rsa, &pPtr );
//SDR_PUBLIC 				Assert( pPtr == mem.Base() + lengthToEncodePub );
//SDR_PUBLIC 				if ( pPtr == mem.Base() + lengthToEncodePub )
//SDR_PUBLIC 				{
//SDR_PUBLIC 					pPublicKey->Set( mem.Base(), (uint32)lengthToEncodePub );
//SDR_PUBLIC 				}
//SDR_PUBLIC 				SecureZeroMemory( mem.Base(), (size_t)mem.CubAllocated() );
//SDR_PUBLIC 			}
//SDR_PUBLIC 		}
//SDR_PUBLIC 
//SDR_PUBLIC 		RSA_free( rsa );
//SDR_PUBLIC 	}
//SDR_PUBLIC 	DispatchOpenSSLErrors( "CRSAPrivateKey::GetPublicKey" );
//SDR_PUBLIC }

#ifdef SDR_SUPPORT_RSA_TICKETS
//-----------------------------------------------------------------------------
// Purpose: Get modulus of RSA public key as big-endian bytes, returns actual length
//-----------------------------------------------------------------------------
uint32 CRSAPublicKey::GetModulusBytes( uint8 *pBufferOut, uint32 cbMaxBufferOut ) const
{
	uint8 const *pModulus = 0;
	uint8 const *pExponent = 0;
	size_t cbModulus = 0;
	size_t cbExponent = 0;
	if ( !ExtractModulusAndExponentFromX509PubKey( GetData(), GetLength(), &pModulus, &cbModulus, &pExponent, &cbExponent ) )
		return 0;
	if ( cbModulus > 0xFFFFFFFFu )
		return 0; 
	if ( pBufferOut && cbMaxBufferOut >= cbModulus )
		V_memcpy( pBufferOut, pModulus, cbModulus );
	return (uint32)cbModulus;
}
#endif

//SDR_PUBLIC //-----------------------------------------------------------------------------
//SDR_PUBLIC // Purpose: Get exponent of RSA public key as big-endian bytes, returns actual length
//SDR_PUBLIC //-----------------------------------------------------------------------------
//SDR_PUBLIC uint32 CRSAPublicKey::GetExponentBytes( uint8 *pBufferOut, uint32 cbMaxBufferOut ) const
//SDR_PUBLIC {
//SDR_PUBLIC 	uint8 const *pModulus = 0;
//SDR_PUBLIC 	uint8 const *pExponent = 0;
//SDR_PUBLIC 	size_t cbModulus = 0;
//SDR_PUBLIC 	size_t cbExponent = 0;
//SDR_PUBLIC 	if ( !ExtractModulusAndExponentFromX509PubKey( GetData(), GetLength(), &pModulus, &cbModulus, &pExponent, &cbExponent ) )
//SDR_PUBLIC 		return 0;
//SDR_PUBLIC 	if ( cbExponent > 0xFFFFFFFFu )
//SDR_PUBLIC 		return 0;
//SDR_PUBLIC 	if ( pBufferOut && cbMaxBufferOut >= cbExponent )
//SDR_PUBLIC 		V_memcpy( pBufferOut, pExponent, cbExponent );
//SDR_PUBLIC 	return (uint32)cbExponent;
//SDR_PUBLIC }

//-----------------------------------------------------------------------------
// Purpose: Retrieve the public half of our internal (public,private) pair
//-----------------------------------------------------------------------------
void CEC25519PrivateKeyBase::GetPublicKey( CCryptoPublicKeyBase *pPublicKey ) const
{
	pPublicKey->Wipe();
	Assert( IsValid() );
	if ( !IsValid() )
		return;
	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate )
	{
		Assert( pPublicKey->GetKeyType() == k_ECryptoKeyTypeKeyExchangePublic );
		if ( pPublicKey->GetKeyType() != k_ECryptoKeyTypeKeyExchangePublic )
			return;
	}
	else if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate )
	{
		Assert( pPublicKey->GetKeyType() == k_ECryptoKeyTypeSigningPublic );
		if ( pPublicKey->GetKeyType() != k_ECryptoKeyTypeSigningPublic )
			return;
	}
	else
	{
		Assert( false ); // impossible, we must be one or the other if valid
		return;
	}
	pPublicKey->Set( GetData(), 32 );
}


//-----------------------------------------------------------------------------
// Purpose: Verify that a set of public and private curve25519 keys are matched
//-----------------------------------------------------------------------------
bool CEC25519PrivateKeyBase::MatchesPublicKey( const CCryptoPublicKeyBase &publicKey ) const
{
	if ( m_eKeyType == k_ECryptoKeyTypeKeyExchangePrivate && publicKey.GetKeyType() != k_ECryptoKeyTypeKeyExchangePublic )
		return false;
	if ( m_eKeyType == k_ECryptoKeyTypeSigningPrivate && publicKey.GetKeyType() != k_ECryptoKeyTypeSigningPublic )
		return false;
	if ( !IsValid() || !publicKey.IsValid() || publicKey.GetLength() != 32 )
		return false;
	return memcmp( GetData(), publicKey.GetData(), 32 ) == 0;
}


#ifdef ENABLE_CRYPTO_25519
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



//-----------------------------------------------------------------------------
// Purpose: power-of-two radix conversion with translation table. Can be used
//			for hex, octal, binary, base32, base64, etc. This is a class because
//			the decoding is done via a generated reverse-lookup table.
//-----------------------------------------------------------------------------
CCustomPow2RadixEncoder::CCustomPow2RadixEncoder( const char *pchEncodingTable )
{
	Init( (const uint8*)pchEncodingTable, (uint32)V_strlen( pchEncodingTable ) );
}


bool CCustomPow2RadixEncoder::EncodeBits( const uint8 *pubData, size_t cDataBits, char *pchEncodedData, size_t cchEncodedData ) const
{
	size_t cubResult = EncodeImpl( pubData, (cDataBits + 7) >> 3, 7 & -(int)cDataBits, (uint8*)pchEncodedData, cchEncodedData, true );
	bool bOK = cubResult > 0 && cubResult <= cchEncodedData;
	return bOK;
}


bool CCustomPow2RadixEncoder::Encode( const uint8 *pubData, size_t cubData, char *pchEncodedData, size_t cchEncodedData ) const
{
	size_t cubResult = EncodeImpl( pubData, cubData, 0, (uint8*)pchEncodedData, cchEncodedData, true );
	bool bOK = cubResult > 0 && cubResult <= cchEncodedData;
	return bOK;
}


bool CCustomPow2RadixEncoder::Decode( const char *pchData, uint8 *pubDecodedData, uint32 *pcubDecodedData, bool bIgnoreInvalidChars, bool bDropTrailingPartialByte ) const
{
	size_t cubResult = DecodeImpl( (const uint8*)pchData, V_strlen( pchData ), pubDecodedData, *pcubDecodedData, bIgnoreInvalidChars, bDropTrailingPartialByte );
	bool bOK = cubResult > 0 && cubResult <= *pcubDecodedData;
	*pcubDecodedData = (uint32)cubResult;
	return bOK;
}


void CCustomPow2RadixEncoder::Init( const uint8 *pubEncodingTable, uint32 cubEncodingTable )
{
	if ( cubEncodingTable < 2 || cubEncodingTable > 128 || !basetypes::IsPowerOf2( cubEncodingTable ) )
	{
		AssertMsg( false, "CCustomPow2Encoder only supports base2, base4, base8, base16, base32" );
		m_uRadixBits = 0;
	}
	else
	{
		memset( m_rgubEncodingTable, 0xFF, sizeof( m_rgubEncodingTable ) );
		memset( m_rgubDecodingTable, 0xFF, sizeof( m_rgubDecodingTable ) );
		m_uRadixBits = FindLeastSignificantBit( cubEncodingTable );
		for ( uint32 i = 0; i < cubEncodingTable; ++i )
		{
			m_rgubEncodingTable[i] = pubEncodingTable[i];
			m_rgubDecodingTable[(uint8)pubEncodingTable[i]] = (uint8)i;
		}
	}
}


size_t CCustomPow2RadixEncoder::EncodeImpl( const uint8 *pubData, size_t cubData, uint cIgnoredTrailingBits, uint8 *pchEncodedData, size_t cchEncodedData, bool bAddTrailingNull ) const
{
	Assert( cchEncodedData );
	if ( !m_uRadixBits || (bAddTrailingNull && !cchEncodedData) )
		return 0;
	size_t result = TranslateBits( pubData, cubData, cIgnoredTrailingBits, m_uRadixBits, m_rgubEncodingTable, 8, pchEncodedData, cchEncodedData, false, false );
	if ( bAddTrailingNull )
	{
		if ( result >= cchEncodedData )
		{
			AssertMsg( false, "not enough space in output buffer for trailing null" );
			pchEncodedData[cchEncodedData - 1] = (uint8)'\0';
		}
		else
		{
			pchEncodedData[result] = (uint8)'\0';
		}
		return result + 1;
	}
	else
	{
		return result;
	}
}


size_t CCustomPow2RadixEncoder::DecodeImpl( const uint8 *pubData, size_t cubData, uint8 *pchDecodedData, size_t cchDecodedData, bool bIgnoreInvalidChars, bool bDropTrailingPartialByte ) const
{
	if ( !m_uRadixBits )
		return 0;
	return TranslateBits( pubData, cubData, 0, 8, m_rgubDecodingTable, m_uRadixBits, pchDecodedData, cchDecodedData, bIgnoreInvalidChars, bDropTrailingPartialByte );
}


size_t CCustomPow2RadixEncoder::TranslateBits( const uint8 *pSrc, size_t cubSrc, uint cIgnoredTrailingBits, uint uLookupBitWidth, const uint8 *pLookupTable, uint uOutputPackBitWidth, uint8 *pDest, size_t cubDest, bool bIgnoreInvalidChars, bool bDropTrailingPartialByte )
{
	Assert( uLookupBitWidth >= 1 && uLookupBitWidth <= 8 && uOutputPackBitWidth >= 1 && uOutputPackBitWidth <= 8 && cIgnoredTrailingBits < 8 );
	uint uWorkBuf = 0;
	uint uWorkBufBits = 0;
	uint uOutputBuf = 0;
	uint uOutputBufBits = 0;

	uint uMaskTableInput = (1u << uLookupBitWidth) - 1;
	uint8 uMaskInvalidEntry = (uint8)((~0u) << uOutputPackBitWidth); // NOTE: usually (uint8)0 when encoding
	size_t iSrc = 0;
	size_t iDest = 0;

	while ( iSrc < cubSrc )
	{
		uWorkBufBits += 8;
		uWorkBuf <<= 8;
		uWorkBuf |= pSrc[iSrc];
		++iSrc;
		if ( iSrc == cubSrc )
		{
			// last byte was processed - strip trailing bits if that was requested
			uWorkBuf >>= cIgnoredTrailingBits;
			uWorkBufBits -= cIgnoredTrailingBits;

			// zero-pad input bits to be an exact multiple of lookup-table input width
			while ( (uWorkBufBits % uLookupBitWidth) != 0 )
			{
				uWorkBuf <<= 1;
				uWorkBufBits++;
			}
		}

		while ( uWorkBufBits >= uLookupBitWidth )
		{
			uWorkBufBits -= uLookupBitWidth;
			uint uTranslated = pLookupTable[(uWorkBuf >> uWorkBufBits) & uMaskTableInput];

			if ( uTranslated & uMaskInvalidEntry )
			{
				if ( !bIgnoreInvalidChars )
					return 0;
				// else this translated value is ignored!
			}
			else
			{
				uOutputBufBits += uOutputPackBitWidth;
				uOutputBuf <<= uOutputPackBitWidth;
				uOutputBuf |= uTranslated;

				if ( uOutputBufBits >= 8 )
				{
					uOutputBufBits -= 8;
					if ( iDest < cubDest )
					{
						pDest[iDest] = (uint8)(uOutputBuf >> uOutputBufBits);
					}
					++iDest;
				}
			}
		}
	}

	// Output the final partial byte if requested (only meaningful when decoding
	// a bitstring that was not originally a multiple of 8 bits before encoding)
	if ( uOutputBufBits > 0 && !bDropTrailingPartialByte )
	{
		uint8 uLastOutChar = (uint8)(uOutputBuf << (8 - uOutputBufBits));
		if ( iDest < cubDest )
		{
			pDest[iDest] = uLastOutChar;
		}
		++iDest;
	}

	AssertMsg( iDest <= cubDest, "not enough space in output buffer" );
	return iDest;
}


CCustomHexEncoder::CCustomHexEncoder( const char *pchEncodeTable ) : CCustomPow2RadixEncoder( pchEncodeTable )
{
	Assert( V_strlen( pchEncodeTable ) == 16 && m_uRadixBits == 4 );
	if ( m_uRadixBits != 4 )
		m_uRadixBits = 0;
}


CCustomBase32Encoder::CCustomBase32Encoder( const char *pchEncodeTable ) : CCustomPow2RadixEncoder( pchEncodeTable )
{
	Assert( V_strlen( pchEncodeTable ) == 32 && m_uRadixBits == 5 );
	if ( m_uRadixBits != 5 )
		m_uRadixBits = 0;
}

