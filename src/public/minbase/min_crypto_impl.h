//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Minimal-dependency crypto helper routine implementations.
//
//=============================================================================

#ifndef _MIN_CRYPTO_IMPL_H
#define _MIN_CRYPTO_IMPL_H

// This file is included in multiple places so without
// some kind of inlining you'll get multiple definitions.
// If you're careful you can override that, but we'll
// default to a basic inline.
#ifndef MIN_CRYPTO_INLINE
#define MIN_CRYPTO_INLINE inline
#endif

#if defined(_MSC_VER)
extern "C" unsigned __int64 __emulu( unsigned int, unsigned int );
extern "C" unsigned char _BitScanReverse( unsigned long* Index, unsigned long Mask );
#pragma intrinsic(__emulu)
#pragma intrinsic(_BitScanReverse)
#endif


enum EMinCryptoError
{
	k_EMinCryptoErrorNone = 0,
	k_EMinCryptoErrorInvalidKey = 1,
	k_EMinCryptoErrorInternalBufferTooSmall = 2,
	k_EMinCryptoErrorOutputBufferTooSmall = 3,
};

MIN_CRYPTO_INLINE unsigned char HexDecodeHalfByte( char c )
{
	// Only works for valid inputs (0-9, a-z, A-Z).
	uint8 x = (uint8)(c - '0') & (uint8)0x1F;
	return ( x > (uint8)9 ) ? x - (uint8)7 : x;
} 

MIN_CRYPTO_INLINE bool HexDecode( const char *pchData, uint8 *pubDecodedData, uint32 *pcubDecodedData )
{
	// Only works for valid inputs (0-9, a-z, A-Z).
	uint32 i = 0;
	size_t cchData = strlen( pchData );
	if ( cchData > 0xffffffff ||
			( cchData % 2 ) != 0 )
	{
		return false;
	}
	for ( i = 0; i < (uint32)cchData ; i += 2 ) 
	{
		uint8 b1 = HexDecodeHalfByte( pchData[i] );
		uint8 b2 = HexDecodeHalfByte( pchData[i+1] );
		if ( i / 2 >= *pcubDecodedData )
			return false;

		*pubDecodedData++ = ( b1 << 4 ) + b2;  // <<4 multiplies by 16
	}
	*pcubDecodedData = i / 2;
	return true;
}

// Simple SHA1 C public-domain implementation by Wei Dei and other contributors.
// Edited by Valve for #define removal, endian detection, general simplification.
// Downloaded from https://oauth.googlecode.com/svn/code/c/liboauth/src/sha1.c 5/7/2016
// Original attribution:
//	/* This code is public-domain - it is based on libcrypt
//	 * placed in the public domain by Wei Dai and other contributors.
//	 */
typedef struct sha1nfo {
	uint32 buffer[64/4];
	uint32 state[20/4];
	uint32 byteCount;
	uint8 bufferOffset;
	uint8 littleEndianFlip;
} sha1nfo;

MIN_CRYPTO_INLINE void sha1_init(sha1nfo *s) {
	union { uint32 a; uint8 b[4]; } endian_detect = { 3 };
	s->state[0] = 0x67452301u;
	s->state[1] = 0xefcdab89u;
	s->state[2] = 0x98badcfeu;
	s->state[3] = 0x10325476u;
	s->state[4] = 0xc3d2e1f0u;
	s->byteCount = 0;
	s->bufferOffset = 0;
	s->littleEndianFlip = endian_detect.b[0]; // 3 if little-endian, 0 if big-endian
}

MIN_CRYPTO_INLINE uint32 sha1_rol32(uint32 number, uint8 bits) {
	return ((number << bits) | (number >> (32-bits)));
}

MIN_CRYPTO_INLINE void sha1_hashBlock(sha1nfo *s) {
	uint8 i;
	uint32 a,b,c,d,e,t;

	a=s->state[0];
	b=s->state[1];
	c=s->state[2];
	d=s->state[3];
	e=s->state[4];
	for (i=0; i<80; i++) {
		if (i>=16) {
			t = s->buffer[(i+13)&15] ^ s->buffer[(i+8)&15] ^ s->buffer[(i+2)&15] ^ s->buffer[i&15];
			s->buffer[i&15] = sha1_rol32(t,1);
		}
		if (i<20) {
			t = (d ^ (b & (c ^ d))) + 0x5a827999u;
		} else if (i<40) {
			t = (b ^ c ^ d) + 0x6ed9eba1u;
		} else if (i<60) {
			t = ((b & c) | (d & (b | c))) + 0x8f1bbcdcu;
		} else {
			t = (b ^ c ^ d) + 0xca62c1d6u;
		}
		t+=sha1_rol32(a,5) + e + s->buffer[i&15];
		e=d;
		d=c;
		c=sha1_rol32(b,30);
		b=a;
		a=t;
	}
	s->state[0] += a;
	s->state[1] += b;
	s->state[2] += c;
	s->state[3] += d;
	s->state[4] += e;
}

MIN_CRYPTO_INLINE void sha1_addUncounted(sha1nfo *s, uint8 data) {
	uint8 * const b = (uint8*) s->buffer;
	b[s->bufferOffset ^ s->littleEndianFlip] = data;
	s->bufferOffset++;
	if (s->bufferOffset == sizeof(s->buffer)) {
		sha1_hashBlock(s);
		s->bufferOffset = 0;
	}
}

MIN_CRYPTO_INLINE void sha1_writebyte(sha1nfo *s, uint8 data) {
	++s->byteCount;
	sha1_addUncounted(s, data);
}

MIN_CRYPTO_INLINE void sha1_write(sha1nfo *s, const char *data, size_t len) {
	for (;len--;) sha1_writebyte(s, (uint8) *data++);
}

MIN_CRYPTO_INLINE void sha1_pad(sha1nfo *s) {
	// Implement SHA-1 padding (fips180-2 5.1.1)
	// Pad with 0x80 followed by 0x00 until the end of the block
	sha1_addUncounted(s, 0x80);
	while (s->bufferOffset != 56) sha1_addUncounted(s, 0x00);
	// Append length in the last 8 bytes
	sha1_addUncounted(s, 0); // We're only using 32 bit lengths
	sha1_addUncounted(s, 0); // But SHA-1 supports 64 bit lengths
	sha1_addUncounted(s, 0); // So zero pad the top bits
	sha1_addUncounted(s, (uint8)(s->byteCount >> 29)); // Shifting to multiply by 8
	sha1_addUncounted(s, (uint8)(s->byteCount >> 21)); // as SHA-1 supports bitstreams as well as
	sha1_addUncounted(s, (uint8)(s->byteCount >> 13)); // byte.
	sha1_addUncounted(s, (uint8)(s->byteCount >> 5));
	sha1_addUncounted(s, (uint8)(s->byteCount << 3));
}
// End of public-domain SHA implementation

MIN_CRYPTO_INLINE void ComputeSHA1Digest( const void *pData, uint32 cubData, uint8 *pDigestOut )
{
	sha1nfo s;
	sha1_init( &s );
	sha1_write( &s, (const char*)pData, cubData );
	sha1_pad( &s );
	for ( size_t i = 0; i < 20; ++i )
		pDigestOut[i] = ((uint8*)s.state)[i ^ s.littleEndianFlip];
}


// Simple unsigned big-integer implementation - only the bare necessities
// to support modular exponentiation for RSA signature verification.
class CSimpleRSABigNum
{
protected:
	// Words are in least-significant to most-significant order. Leading zeros
	// are never counted in nwords/nbits except after _Untrimmed functions.
	uint32 *words;
	uint32 ncapacity;
	uint32 nwords;
	uint32 nbits;

public:
	CSimpleRSABigNum() { words = NULL; nbits = nwords = ncapacity = 0; }
	CSimpleRSABigNum( const CSimpleRSABigNum& x ) { words = NULL; nbits = nwords = ncapacity = 0; *this = x; }
	explicit CSimpleRSABigNum( uint32 x ) { words = NULL; nbits = nwords = ncapacity = 0; SetFromUint32( x ); }
	~CSimpleRSABigNum() { delete[] words; }

	CSimpleRSABigNum& operator=(const CSimpleRSABigNum& x)
	{
		EnsureCapacity( x.nwords );
		nwords = x.nwords;
		nbits = x.nbits;
		for ( uint32 i = 0; i < nwords; ++i )
			words[i] = x.words[i];
		return *this;
	}

	uint32 CountBytes() const { return (nbits + 7u) / 8u; }
	uint32 CountBits() const { return nbits; }

	void Clear() { nwords = nbits = 0; }

	void SetFromUint32( uint32 value )
	{
		EnsureCapacity( 1 );
		words[0] = value;
		nwords = 1;
		nbits = 32;
		TrimLeadingZeros();
	}

	void SetFromBigEndianBytes( const uint8 *pubData, uint32 cubData )
	{
		// We store words in least-significant to most-significant order.
		// On little-endian systems, individual word bytes are also LSB to MSB;
		// on big-endian systems, we have to reverse direction of bytes in a word.
		union { uint32 a; uint8 b[4]; } endian_detect = { 3 };
		size_t big_endian_flip = endian_detect.b[3]; // 3 if big-endian, 0 if not
		Clear();
		ZeroExtend_Untrimmed( ( cubData + 3u ) / 4u );
		for ( size_t idest = 0, isrc = cubData; isrc--; ++idest )
			((uint8*)words)[idest ^ big_endian_flip] = pubData[isrc];
		TrimLeadingZeros();
	}

	bool CopyToBigEndianBytes( uint8 *pubData, uint32 *pcubData ) const
	{
		if ( *pcubData < CountBytes() )
		{
			*pcubData = CountBytes();
			return false;
		}
		union { uint32 a; uint8 b[4]; } endian_detect = { 3 };
		size_t big_endian_flip = endian_detect.b[3]; // 3 if big-endian, 0 if not
		for ( size_t isrc = 0, idest = CountBytes(); idest--; ++isrc )
			pubData[idest] = ((uint8*)words)[isrc ^ big_endian_flip];
		*pcubData = CountBytes();
		return true;
	}

	void Multiply( const CSimpleRSABigNum& rhs )
	{
		// Traditional long-hand multiplication accumulated one row at a time,
		// where a row is a single word of *this multiplied by the entirity of rhs.
		// Note that rhs may alias to self! Write output to result, swap at end.
		CSimpleRSABigNum result, temp;
		result.ZeroExtend_Untrimmed( rhs.nwords + nwords + 2 );
		temp.ZeroExtend_Untrimmed( rhs.nwords + 1 );
		for ( uint32 i = 0; i < nwords; ++i )
		{
			uint32 words_i = words[i], carry = 0;
			for ( uint32 j = 0; j < rhs.nwords; ++j )
			{
				uint64 mul = Mulitply32x32( words_i, rhs.words[j] ) + carry;
				temp.words[j] = (uint32)mul;
				carry = (uint32)(mul >> 32);
			}
			temp.words[rhs.nwords] = carry;
			result.AddWithWordOffset_Untrimmed( temp, i );
		}
		result.TrimLeadingZeros();
		Swap( result );
	}

	void ShiftDown( uint32 shift, uint32 skip_low_words = 0 )
	{
		if ( nbits == 0 || shift == 0 )
			return;

		if ( shift >= nbits )
		{
			Clear();
			return;
		}

		uint32 wordoffset = shift / 32u;
		uint32 bitoffset = shift & 31u;
		if ( wordoffset != 0 )
		{
			for ( uint32 i = wordoffset + skip_low_words; i < nwords; ++i )
				words[i - wordoffset] = words[i];
			nbits -= wordoffset * 32u;
			nwords -= wordoffset;
		}
		if ( bitoffset != 0 )
		{
			uint32 *p = &words[skip_low_words];
			uint32 n = nwords - skip_low_words - 1;
			// optimization: better codegen and lower latency on fixed-width shifts for case shift==1.
			// makes RSA almost 20% faster. in testing, specializing values other than 1 had no impact.
			if ( bitoffset == 1u )
			{
				for ( ; n; --n, ++p )
					p[0] = (p[0] >> 1u) | (p[1] << 31u);
			}
			else
			{
				for ( ; n; --n, ++p )
					p[0] = (p[0] >> bitoffset) | (p[1] << (32u - bitoffset));
			}
			p[0] >>= bitoffset;
			nbits -= bitoffset;
			nwords = (nbits + 31u) / 32u;
		}
	}

	void ShiftUp( uint32 shift )
	{
		// Could write code similar to ShiftDown, but simpler to just reuse Multiply and AddWithWordOffset.
		// Not performance sensitive - fewer than 20 invocations per RSA operation regardless of key size.
		CSimpleRSABigNum mul;
		mul.AddWithWordOffset_Untrimmed( CSimpleRSABigNum( 1u << (shift & 31u) ), (shift / 32u) );
		Multiply( mul );
	}

	void SlowDivide( const CSimpleRSABigNum& divisor, CSimpleRSABigNum& remainder )
	{
		if ( divisor.nbits == 0 )
			return;
		// Simple long-division: subtract rhs if possible at every possible bit offset
		// from most significant to least significant.
		CSimpleRSABigNum shifted_divisor( divisor );
		uint32 bitindex = ( nbits > divisor.nbits ) ? nbits - divisor.nbits : 0u;
		shifted_divisor.ShiftUp( bitindex );
		Swap( remainder );
		Clear();
		ZeroExtend_Untrimmed( (bitindex / 32u) + 1 );
		for (;;)
		{
			if ( remainder.SubtractIfNotUnderflow( shifted_divisor, bitindex / 32u ) )
				words[bitindex / 32u] |= ( 1u << ( bitindex & 31u ) );
			if ( bitindex-- == 0 )
				break;
			shifted_divisor.ShiftDown( 1, bitindex / 32u );
		}
		TrimLeadingZeros();
	}

	void ExponentiateModulo( const CSimpleRSABigNum &exponent, const CSimpleRSABigNum &modulus )
	{
		if ( nbits == 0 || modulus.nbits == 0 )
			return;

		if ( &exponent == this || &modulus == this )
		{
			ExponentiateModulo( CSimpleRSABigNum( exponent ), CSimpleRSABigNum( modulus ) );
			return;
		}

		// Instead of slow modulo operations, we'll do faster Barrett reductions, which are
		// basically the fixed-point equivalent of multiplying by the precalculated inverse,
		// dropping the fractional part, remultiplying and subtracting to get a remainder.
		CSimpleRSABigNum precalc( 1u ), temp;
		precalc.ShiftUp( 2u * modulus.nwords * 32u );
		precalc.SlowDivide( modulus, temp );

		// If base is more than twice as wide as modulo, can't use Barrett reduction.
		// (Note that in RSA usage, the base value is never wider than the modulus.)
		CSimpleRSABigNum base( *this );
		if ( nbits <= modulus.nbits * 2 )
			base.BarrettReduce( modulus, precalc );
		else
			SlowDivide( modulus, base );

		// Classic algorithm: exponentiation by squaring. Applying modulo after every step
		// keeps the bit size manageable and allows the operation to perform reasonably fast.
		EnsureCapacity( modulus.nwords * 2 + 3 );
		SetFromUint32( 1 );
		// For each bit in the exponent from most to least significant...
		for ( uint32 bitindex = exponent.nbits; bitindex--; )
		{
			// Square working value, then multiply by base if bit is set
			Multiply( *this );
			BarrettReduce( modulus, precalc );
			if ( exponent.words[bitindex / 32u] & (1u << (bitindex & 31u)) )
			{
				Multiply( base );
				BarrettReduce( modulus, precalc );
			}
		}
	}

	void Swap( CSimpleRSABigNum& x )
	{
		uint32 *p = words, c = ncapacity, n = nwords, b = nbits;
		words = x.words; ncapacity = x.ncapacity; nwords = x.nwords; nbits = x.nbits;
		x.words = p; x.ncapacity = c; x.nwords = n; x.nbits = b;
	}

protected:
	void EnsureCapacity( uint32 wordcount )
	{
		if ( ncapacity >= wordcount )
			return;
		uint32 *oldwords = words;
		words = new uint32[ wordcount ];
		ncapacity = wordcount;
		for ( uint32 i = nwords; i--; )
			words[i] = oldwords[i];
		delete[] oldwords;
	}

	static uint64 Mulitply32x32( uint32 a, uint32 b )
	{
#if defined(_MSC_VER)
		return (uint64) __emulu( a, b );
#else
		return (uint64)a * b;
#endif
	}

	static uint32 FindMSBInNonZeroWord( uint32 n )
	{
#if defined(_MSC_VER)
		unsigned long b;
		(void)_BitScanReverse( &b, n );
		return (uint32)b;
#elif defined(__GNUC__)
		return 31u - (uint32)__builtin_clz( n );
#else
		uint32 b = 0;
		for ( ; n; ++b, n >>= 1 ) {}
		return b - 1;
#endif
	}

	void TrimLeadingZeros()
	{
		while ( nwords != 0 && words[nwords - 1] == 0 )
			--nwords;
		if ( nwords != 0 )
			nbits = nwords * 32u + FindMSBInNonZeroWord( words[nwords - 1] ) - 31;
		else
			nbits = 0;
	}

	void ZeroExtend_Untrimmed( uint32 wordcount )
	{
		EnsureCapacity( wordcount );
		while ( nwords < wordcount )
			words[nwords++] = 0;
		nbits = nwords * 32u;
	}

	void Truncate( uint32 wordcount )
	{
		if ( nwords <= wordcount )
			return;
		nwords = wordcount;
		nbits = nwords * 32u;
		TrimLeadingZeros();
	}

	void AddWithWordOffset_Untrimmed( const CSimpleRSABigNum &rhs, uint32 offset )
	{
		ZeroExtend_Untrimmed( ( ( rhs.nwords + offset > nwords ) ? rhs.nwords + offset : nwords ) + 1 );
		uint32 carry = 0;
		uint32 *p = &words[offset];
		const uint32 *q = &rhs.words[0];
		for ( uint32 n = rhs.nwords; n; --n, ++p, ++q )
		{
			uint64 result = (uint64)*p + *q + carry;
			*p = (uint32)result;
			carry = (uint32)(result >> 32u);
		}
		// Push carry upwards until we aren't carrying any more.
		for ( ; carry; ++p )
		{
			uint64 result = (uint64)*p + carry;
			*p = (uint32)result;
			carry = (uint32)(result >> 32u);
		}
		// Trim a single word to prevent excessive growth of nwords in a loop
		if ( words[nwords - 1] == 0 )
		{
			nwords -= 1;
			nbits -= 32;
		}
	}

	bool SubtractIfNotUnderflow( const CSimpleRSABigNum &rhs, uint32 skipwords = 0 )
	{
		if ( skipwords >= rhs.nwords )
			return false;

		// Abort if *this is smaller than rhs
		if ( nbits < rhs.nbits )
			return false;

		if ( nbits == rhs.nbits )
		{
			// Same bit length; compare words from most significant to least.
			// If the current word is smaller, then subtraction will underflow.
			// If the current word is identical, keep advancing to the next word.
			for ( uint32 i = rhs.nwords; i-- > skipwords; )
			{
				if ( words[i] < rhs.words[i] )
					return false;
				if ( words[i] > rhs.words[i] )
					break;
			}
		}

		// We now know that subtraction will not underflow, go ahead and do it.
		uint32 borrow = 0;
		uint32 *p = &words[skipwords];
		const uint32 *q = &rhs.words[skipwords];
		for ( uint32 n = rhs.nwords - skipwords; n; --n, ++p, ++q )
		{
			uint64 result = (uint64)*p - *q - borrow;
			*p = (uint32)result;
			borrow = (uint32)(result >> 32u) & 1u;
		}
		// Push carry (borrow) upwards until we aren't carrying any more.
		for ( ; borrow; ++p )
		{
			uint64 result = (uint64)*p - borrow;
			*p = (uint32)result;
			borrow = (uint32)(result >> 32u) & 1u;
		}
		TrimLeadingZeros();
		return true;
	}

	void BarrettReduce( const CSimpleRSABigNum& modulus, const CSimpleRSABigNum& precalc )
	{
		// See Handbook of Applied Cryptography for reference, http://cacr.uwaterloo.ca/hac/
		if ( nbits > modulus.nbits )
		{
			CSimpleRSABigNum q( *this );
			q.ShiftDown( (modulus.nwords - 1) * 32u );
			q.Multiply( precalc );
			q.ShiftDown( (modulus.nwords + 1) * 32u );
			q.Multiply( modulus );
			// q = q mod (b**(k+1)); *this = *this mod (b**(k+1))
			q.Truncate( modulus.nwords + 1 );
			Truncate( modulus.nwords + 1 );
			// *this -= b; if ( *this < 0 ) *this += (2**32)**(k+1)
			if ( !SubtractIfNotUnderflow( q ) )
			{
				ZeroExtend_Untrimmed( modulus.nwords + 2 );
				words[modulus.nwords + 1] = 1u;
				TrimLeadingZeros();
				SubtractIfNotUnderflow( q );
			}
		}
		// while (*this > modulus) *this -= modulus;
		while ( SubtractIfNotUnderflow( modulus ) ) {}
	}
};

// Decode ASN.1 tag - works only for simple tag/length/value cases such as SEQUENCE (0x30), INTEGER(0x02), OCTET_STRING (0x04), etc
MIN_CRYPTO_INLINE bool ExtractASN1FieldData( uint8 expect_tag, const uint8 *buf, size_t buflen, const uint8 **datastart_out, size_t *datalen_out )
{
	*datastart_out = NULL;
	*datalen_out = 0;

	// ASN.1 tag/length/value triple: (tag byte) (encoded len) (raw data)
	if ( buflen < 2 )
		return false;

	if ( buf[0] != expect_tag )
		return false;

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
		if ( encoded_len_bytes < 1 || encoded_len_bytes > sizeof( size_t ) )
			return false;
		if ( 2 + encoded_len_bytes > buflen )
			return false;
		for ( size_t i = 0; i < encoded_len_bytes; ++i )
		{
			encoded_len <<= 8;
			encoded_len += (uint8)buf[2 + i];
		}
		if ( 2 + encoded_len_bytes + encoded_len < encoded_len )
			return false;
	}
	if ( 2 + encoded_len_bytes + encoded_len > buflen )
		return false;
	*datastart_out = buf + 2 + encoded_len_bytes;
	*datalen_out = encoded_len;
	return true;
}

MIN_CRYPTO_INLINE bool ExtractModulusAndExponentFromX509PubKey( const uint8 *pDataPtr, size_t cDataLen, const uint8 **ppModulus, size_t *pcubModulus, const uint8 **ppExponent, size_t *pcubExponent )
{
	// X.509-format RSA public keys are wrapped in an outer SEQUENCE, with an initial inner SEQUENCE containing ( OID for rsaEncryption, NULL )
	if ( !ExtractASN1FieldData( 0x30 /*SEQUENCE*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
		return false;
	static const char k_rgchRSAPubKeyAlgoSequence[] = "\x30\x0D\x06\x09\x2A\x86\x48\x86\xF7\x0D\x01\x01\x01\x05"; // final \x00 implied by string handling
	if ( cDataLen <= sizeof( k_rgchRSAPubKeyAlgoSequence ) || memcmp( pDataPtr, k_rgchRSAPubKeyAlgoSequence, sizeof( k_rgchRSAPubKeyAlgoSequence ) ) != 0 )
		return false;
	pDataPtr += sizeof( k_rgchRSAPubKeyAlgoSequence );
	cDataLen -= sizeof( k_rgchRSAPubKeyAlgoSequence );
	// ... followed by a BIT STRING (*not* OCTET STRING as with PKCS#8) that contains the actual key data
	if ( !ExtractASN1FieldData( 0x03 /*BIT STRING*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) || cDataLen < 1 )
		return false;
	// Discard the number of zero-padding bits which were added on top of the BIT STRING; don't care.
	++pDataPtr;
	--cDataLen;
	// Now examining BIT STRING contents, which are in turn another ASN.1 SEQUENCE of two INTEGERs
	// for the public-key modulus and exponent.
	if ( !ExtractASN1FieldData( 0x30/*SEQUENCE*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
		return false;
	if ( !ExtractASN1FieldData( 0x02 /*INTEGER*/, pDataPtr, cDataLen, ppModulus, pcubModulus ) )
		return false;
	size_t cAdvanceBeyondTag = *ppModulus + *pcubModulus - pDataPtr;
	pDataPtr += cAdvanceBeyondTag;
	cDataLen -= cAdvanceBeyondTag;
	if ( !ExtractASN1FieldData( 0x02 /*INTEGER*/, pDataPtr, cDataLen, ppExponent, pcubExponent ) )
		return false;
	return true;
}


MIN_CRYPTO_INLINE bool ExtractModulusAndExponentFromX509PrivKey( const uint8 *pDataPtr, size_t cDataLen, const uint8 **ppModulus, size_t *pcubModulus, const uint8 **ppExponent, size_t *pcubExponent )
{
	// X.509-format RSA private keys are wrapped in an outer SEQUENCE, with an initial inner SEQUENCE containing ( OID for rsaEncryption, NULL )
	if ( !ExtractASN1FieldData( 0x30 /*SEQUENCE*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
		return false;
	static const char k_rgExpectSequenceBytes[] = "\x02\x01\x00\x30\x0D\x06\x09\x2A\x86\x48\x86\xF7\x0D\x01\x01\x01\x05"; // final byte is \x00
	if ( cDataLen <= sizeof( k_rgExpectSequenceBytes ) || memcmp( pDataPtr, k_rgExpectSequenceBytes, sizeof( k_rgExpectSequenceBytes ) ) != 0 )
		return false;
	pDataPtr += sizeof( k_rgExpectSequenceBytes );
	cDataLen -= sizeof( k_rgExpectSequenceBytes );
	// ... followed by a OCTET STRING
	if ( !ExtractASN1FieldData( 0x04 /*OCTET STRING*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) || cDataLen < 1 )
		return false;
	// Now examining contents, which are in turn another ASN.1 SEQUENCE of integers INTEGERs
	if ( !ExtractASN1FieldData( 0x30/*SEQUENCE*/, pDataPtr, cDataLen, &pDataPtr, &cDataLen ) )
		return false;

	// first the version # which we will ignore
	const uint8 *pVersion;
	size_t cubVersion;
	if ( !ExtractASN1FieldData( 0x02 /*INTEGER*/, pDataPtr, cDataLen, &pVersion, &cubVersion ) )
		return false;
	size_t cAdvanceBeyondTag = pVersion + cubVersion - pDataPtr;
	pDataPtr += cAdvanceBeyondTag;
	cDataLen -= cAdvanceBeyondTag;

	// now the modulus
	if ( !ExtractASN1FieldData( 0x02 /*INTEGER*/, pDataPtr, cDataLen, ppModulus, pcubModulus ) )
		return false;
	cAdvanceBeyondTag = *ppModulus + *pcubModulus - pDataPtr;
	pDataPtr += cAdvanceBeyondTag;
	cDataLen -= cAdvanceBeyondTag;

	// public exponent - which we will ignore
	const uint8 *pExponentPublic;
	size_t cubExponentPublic;
	if ( !ExtractASN1FieldData( 0x02 /*INTEGER*/, pDataPtr, cDataLen, &pExponentPublic, &cubExponentPublic ) )
		return false;
	cAdvanceBeyondTag = pExponentPublic + cubExponentPublic - pDataPtr;
	pDataPtr += cAdvanceBeyondTag;
	cDataLen -= cAdvanceBeyondTag;

	// get private exponent
	if ( !ExtractASN1FieldData( 0x02 /*INTEGER*/, pDataPtr, cDataLen, ppExponent, pcubExponent ) )
		return false;
	return true;
}


MIN_CRYPTO_INLINE bool ExtractSHA1DigestFromRSASignature( const uint8 *pDataPtr, size_t cDataLen, const uint8 **pDigestOut )
{
	// Verify strict format: 0x01, cPaddingLen * 0xFF, 0x00 0x30 ... 0x04 0x14, SHA1 bytes, <END OF DATA>.
	static const uint8 k_rgubAlgoPrefix[] = { 0x00, 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14 };
	static const size_t k_nSHABytes = 20;
	*pDigestOut = NULL;
	if ( cDataLen < 1 + sizeof( k_rgubAlgoPrefix ) + k_nSHABytes || pDataPtr[0] != 0x01 )
		return false;
	for ( size_t i = 1; i < cDataLen - sizeof( k_rgubAlgoPrefix ) - k_nSHABytes; ++i )
	{
		if ( pDataPtr[ i ] != (uint8)0xFF )
			return false;
	}
	for ( size_t i = 0; i < sizeof( k_rgubAlgoPrefix ); ++i )
	{
		if ( pDataPtr[ cDataLen - sizeof( k_rgubAlgoPrefix ) - k_nSHABytes + i ] != k_rgubAlgoPrefix[i] )
			return false;
	}
	*pDigestOut = pDataPtr + cDataLen - k_nSHABytes;
	return true;
}

MIN_CRYPTO_INLINE bool RSADecodeSignatureDigest( const uint8 *pubSignature, const uint32 cubSignature, 
												 const uint8 *pubPublicKey, const uint32 cubPublicKey,
												 uint8 *pDigestOut )
{
	for ( size_t i = 0; i < 20 /*size of SHA1 hash*/; ++i )
		pDigestOut[i] = 0;

	const uint8 *pModulusBigInt = NULL;
	const uint8 *pExponentBigInt = NULL;
	size_t cModulusBytes = 0;
	size_t cExponentBytes = 0;
	if ( !ExtractModulusAndExponentFromX509PubKey( pubPublicKey, cubPublicKey, &pModulusBigInt, &cModulusBytes, &pExponentBigInt, &cExponentBytes ) )
		return false;

	uint8 rsabuf[1024]; // large enough for an 8192-bit RSA key. in practice, anything over 3072 bits is crazytown.
	uint32 cRSABufLen = sizeof( rsabuf );

	CSimpleRSABigNum a, b, m;
	a.SetFromBigEndianBytes( pubSignature, cubSignature );
	b.SetFromBigEndianBytes( pExponentBigInt, (uint32)cExponentBytes );
	m.SetFromBigEndianBytes( pModulusBigInt, (uint32)cModulusBytes );

	if ( m.CountBytes() > cRSABufLen )
		return false;

	a.ExponentiateModulo( b, m );
	if ( !a.CopyToBigEndianBytes( rsabuf, &cRSABufLen ) )
		return false;

	const uint8 *pSignedDigest = NULL;
	if ( !ExtractSHA1DigestFromRSASignature( rsabuf, cRSABufLen, &pSignedDigest ) )
		return false;

	for ( size_t i = 0; i < 20 /*size of SHA1 hash*/; ++i )
		pDigestOut[i] = pSignedDigest[i];

	return true;
}

MIN_CRYPTO_INLINE bool RSAVerifySignature( const uint8 *pubData, const uint32 cubData, 
                                           const uint8 *pubSignature, const uint32 cubSignature, 
                                           const uint8 *pubPublicKey, const uint32 cubPublicKey )
{
	uint32 signedDigest[5];
	if ( !RSADecodeSignatureDigest( pubSignature, cubSignature, pubPublicKey, cubPublicKey, (uint8*)signedDigest ) )
		return false;

	uint32 actualDigest[5];
	ComputeSHA1Digest( pubData, cubData, (uint8*)actualDigest );

	for ( size_t i = 0; i < 5; ++i )
	{
		if ( signedDigest[i] != actualDigest[i] )
			return false;
	}
	return true;
}


// simple encrypt and decrypt
// no blocks, no padding, not safe against timing or side-channel attacks
// caveat emptor, understand these limits before use
MIN_CRYPTO_INLINE bool RSASimpleEncrypt( const uint8 *pubInput, const uint32 cubInput, 
												 const uint8 *pubPublicKey, const uint32 cubPublicKey,
												 uint8 *pOut, uint32 *pcubOut, EMinCryptoError *pEMinCryptoError )
{
	*pEMinCryptoError = k_EMinCryptoErrorNone;

	const uint8 *pModulusBigInt = NULL;
	const uint8 *pExponentBigInt = NULL;
	size_t cModulusBytes = 0;
	size_t cExponentBytes = 0;
	if ( !ExtractModulusAndExponentFromX509PubKey( pubPublicKey, cubPublicKey, &pModulusBigInt, &cModulusBytes, &pExponentBigInt, &cExponentBytes ) )
	{
		*pEMinCryptoError = k_EMinCryptoErrorInvalidKey;
		return false;
	}

	uint8 rsabuf[1024]; // large enough for an 8192-bit RSA key. in practice, anything over 3072 bits is crazytown.
	uint32 cRSABufLen = sizeof( rsabuf );

	CSimpleRSABigNum a, b, m;
	a.SetFromBigEndianBytes( pubInput, cubInput );
	b.SetFromBigEndianBytes( pExponentBigInt, (uint32)cExponentBytes );
	m.SetFromBigEndianBytes( pModulusBigInt, (uint32)cModulusBytes );

	if ( m.CountBytes() > cRSABufLen )
	{
		*pEMinCryptoError = k_EMinCryptoErrorInternalBufferTooSmall;
		return false;
	}

	a.ExponentiateModulo( b, m );
	if ( !a.CopyToBigEndianBytes( rsabuf, &cRSABufLen ) )
	{
		*pEMinCryptoError = k_EMinCryptoErrorInternalBufferTooSmall;
		return false;
	}

	if ( cRSABufLen <= *pcubOut )
	{
		uint32 ibOffset = *pcubOut - cRSABufLen;
		memset(pOut, 0, ibOffset);
		memcpy(&pOut[ibOffset], rsabuf, cRSABufLen);
		return true;
	}
	else
	{
		*pEMinCryptoError = k_EMinCryptoErrorOutputBufferTooSmall;
	}
	*pcubOut = cRSABufLen;

	return false;
}


// simple encrypt and decrypt
// no blocks, no padding, not safe against timing or side-channel attacks
// caveat emptor, understand these limits before use
MIN_CRYPTO_INLINE bool RSASimpleDecrypt( const uint8 *pubInput, const uint32 cubInput, 
												 const uint8 *pubPrivKey, const uint32 cubPrivKey,
												 uint8 *pOut, uint32 *pcubOut, EMinCryptoError *pEMinCryptoError )
{
	*pEMinCryptoError = k_EMinCryptoErrorNone;

	const uint8 *pModulusBigInt = NULL;
	const uint8 *pExponentBigInt = NULL;
	size_t cModulusBytes = 0;
	size_t cExponentBytes = 0;
	if ( !ExtractModulusAndExponentFromX509PrivKey( pubPrivKey, cubPrivKey, &pModulusBigInt, &cModulusBytes, &pExponentBigInt, &cExponentBytes ) )
	{
		*pEMinCryptoError = k_EMinCryptoErrorInvalidKey;
		return false;
	}

	uint8 rsabuf[1024]; // large enough for an 8192-bit RSA key. in practice, anything over 3072 bits is crazytown.
	uint32 cRSABufLen = sizeof( rsabuf );

	CSimpleRSABigNum a, b, m;
	a.SetFromBigEndianBytes( pubInput, cubInput );
	b.SetFromBigEndianBytes( pExponentBigInt, (uint32)cExponentBytes );
	m.SetFromBigEndianBytes( pModulusBigInt, (uint32)cModulusBytes );

	if ( m.CountBytes() > cRSABufLen )
	{
		*pEMinCryptoError = k_EMinCryptoErrorInternalBufferTooSmall;
		return false;
	}

	a.ExponentiateModulo( b, m );
	if ( !a.CopyToBigEndianBytes( rsabuf, &cRSABufLen ) )
	{
		*pEMinCryptoError = k_EMinCryptoErrorInternalBufferTooSmall;
		return false;
	}

	if ( cRSABufLen <= *pcubOut )
	{
		uint32 ibOffset = *pcubOut - cRSABufLen;
		memset( pOut, 0, ibOffset );
		memcpy( &pOut[ibOffset], rsabuf, cRSABufLen );
		// dont change *pcubOut
		return true;
	}
	else
	{
		*pEMinCryptoError = k_EMinCryptoErrorOutputBufferTooSmall;
	}
	*pcubOut = cRSABufLen;

	return false;
}



#endif // #ifndef _MIN_CRYPTO_IMPL_H
