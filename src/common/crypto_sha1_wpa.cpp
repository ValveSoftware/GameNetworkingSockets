#include "crypto.h"
#include <tier0/dbg.h>
#include <tier0/vprof.h>

#ifdef VALVE_CRYPTO_SHA1_WPA

extern "C" {
// external headers for sha1 and hmac-sha1 support
#include "../external/sha1-wpa/sha1.h"
}

//-----------------------------------------------------------------------------
// Purpose: Generate a keyed-hash MAC using SHA1
// Input:	pubData -			Plaintext data to digest
//			cubData -			length of data
//			pubKey -			key to use in HMAC
//			cubKey -			length of key
//			pOutDigest -		Pointer to receive hashed digest output
//-----------------------------------------------------------------------------
void CCrypto::GenerateHMAC( const uint8 *pubData, uint32 cubData, const uint8 *pubKey, uint32 cubKey, SHADigest_t *pOutputDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateHMAC", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pubData );
	Assert( cubData > 0 );
	Assert( pubKey );
	Assert( cubKey > 0 );
	Assert( pOutputDigest );

	int status = hmac_sha1(pubKey, cubKey, pubData, cubData, (uint8_t*)pOutputDigest);
	AssertFatal(status == 0);
}

// Standalone MD5 per RFC 1321.  What does thi shave to do with SHA1?
// Absolutely nothing, but the places where we need to use the reference
// implementation of SHA1 just so happen to be the same palces we need
// the reference implementation of MD5, so we are shoving this in here.
static void ComputeMD5( const void *pData, size_t cbData, uint8 pOut[16] )
{
	// Per-round shift amounts
	static const uint8 r[64] = {
		7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
		5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
		4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
		6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
	};
	// K[i] = floor( abs(sin(i+1)) * 2^32 )
	static const uint32 K[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
		0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
		0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
		0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
		0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
		0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
		0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
		0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
		0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
	};

	uint32 h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;

	// nChunks covers original data + 0x80 pad byte + 8-byte length field, rounded to 64-byte blocks
	const uint64 nBits   = (uint64)cbData * 8;
	const size_t nChunks = ( cbData + 9 + 63 ) / 64;

	for ( size_t chunk = 0; chunk < nChunks; ++chunk )
	{
		uint32 w[16];
		for ( int i = 0; i < 16; ++i )
		{
			uint32 word = 0;
			for ( int j = 0; j < 4; ++j )
			{
				size_t pos = chunk * 64 + i * 4 + j;
				uint8 b;
				if ( pos < cbData )
					b = ((const uint8 *)pData)[pos];
				else if ( pos == cbData )
					b = 0x80;
				else if ( pos >= nChunks * 64 - 8 )
					b = (uint8)( nBits >> ( ( pos - ( nChunks * 64 - 8 ) ) * 8 ) );
				else
					b = 0;
				word |= (uint32)b << ( j * 8 );
			}
			w[i] = word;
		}

		uint32 a = h0, b = h1, c = h2, d = h3;
		for ( int i = 0; i < 64; ++i )
		{
			uint32 f, g;
			if      ( i < 16 ) { f = ( b & c ) | ( ~b & d );  g = i; }
			else if ( i < 32 ) { f = ( d & b ) | ( ~d & c );  g = ( 5 * i + 1 ) % 16; }
			else if ( i < 48 ) { f = b ^ c ^ d;                g = ( 3 * i + 5 ) % 16; }
			else               { f = c ^ ( b | ~d );           g = ( 7 * i ) % 16; }
			uint32 temp = d;
			d = c;
			c = b;
			uint32 rot = a + f + K[i] + w[g];
			b = b + ( ( rot << r[i] ) | ( rot >> ( 32 - r[i] ) ) );
			a = temp;
		}
		h0 += a; h1 += b; h2 += c; h3 += d;
	}

	// Little-endian output
	for ( int i = 0; i < 4; ++i ) pOut[i]    = (h0 >> (i*8)) & 0xff;
	for ( int i = 0; i < 4; ++i ) pOut[4+i]  = (h1 >> (i*8)) & 0xff;
	for ( int i = 0; i < 4; ++i ) pOut[8+i]  = (h2 >> (i*8)) & 0xff;
	for ( int i = 0; i < 4; ++i ) pOut[12+i] = (h3 >> (i*8)) & 0xff;
}

void CCrypto::GenerateMD5Digest( const void *pData, size_t cbData, MD5Digest_t *pOutputDigest )
{
	VPROF_BUDGET( "CCrypto::GenerateMD5Digest", VPROF_BUDGETGROUP_ENCRYPTION );
	Assert( pOutputDigest );
	ComputeMD5( pData, cbData, *pOutputDigest );
}

#endif // #ifdef VALVE_CRYPTO_SHA1_WPA
