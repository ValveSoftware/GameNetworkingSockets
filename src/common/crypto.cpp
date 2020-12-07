//========= Copyright Valve LLC, All rights reserved. ========================

#include "crypto.h"

///////////////////////////////////////////////////////////////////////////////
//
// SipHash, used for challenge generation
//
// http://en.wikipedia.org/wiki/SipHash
//
// Code was copied from here:
// https://github.com/veorq/SipHash/blob/1b85a33b71f0fdd49942037a503b6798d67ef765/siphash24.c
//
///////////////////////////////////////////////////////////////////////////////

/* default: SipHash-2-4 */
#define cROUNDS 2
#define dROUNDS 4

#define ROTL(x,b) (uint64_t)( ((x) << (b)) | ( (x) >> (64 - (b))) )
#define U8TO64_LE(p) LittleQWord( *(const uint64*)(p) );

#define SIPROUND                                        \
  do {                                                  \
    v0 += v1; v1=ROTL(v1,13); v1 ^= v0; v0=ROTL(v0,32); \
    v2 += v3; v3=ROTL(v3,16); v3 ^= v2;                 \
    v0 += v3; v3=ROTL(v3,21); v3 ^= v0;                 \
    v2 += v1; v1=ROTL(v1,17); v1 ^= v2; v2=ROTL(v2,32); \
  } while(0)

uint64_t CCrypto::SipHash( const void *data, size_t inlen, const CCrypto::SipHashKey_t &k )
{
  const uint8_t *in = (const uint8_t *)data;

  /* "somepseudorandomlygeneratedbytes" */
  uint64_t v0 = 0x736f6d6570736575ULL;
  uint64_t v1 = 0x646f72616e646f6dULL;
  uint64_t v2 = 0x6c7967656e657261ULL;
  uint64_t v3 = 0x7465646279746573ULL;
  uint64_t b;
  uint64_t k0 = k[0];
  uint64_t k1 = k[1];
  uint64_t m;
  int i;
  const uint8_t *end = in + inlen - ( inlen % sizeof( uint64_t ) );
  const int left = inlen & 7;
  b = ( ( uint64_t )inlen ) << 56;
  v3 ^= k1;
  v2 ^= k0;
  v1 ^= k1;
  v0 ^= k0;

  for ( ; in != end; in += 8 )
  {
    m = U8TO64_LE( in );
    v3 ^= m;

    //TRACE;
    for( i=0; i<cROUNDS; ++i ) SIPROUND;

    v0 ^= m;
  }

  switch( left )
  {
  case 7: b |= ( ( uint64_t )in[ 6] )  << 48;
 // FALLTHROUGH
  case 6: b |= ( ( uint64_t )in[ 5] )  << 40;
 // FALLTHROUGH
  case 5: b |= ( ( uint64_t )in[ 4] )  << 32;
 // FALLTHROUGH
  case 4: b |= ( ( uint64_t )in[ 3] )  << 24;
 // FALLTHROUGH
  case 3: b |= ( ( uint64_t )in[ 2] )  << 16;
 // FALLTHROUGH
  case 2: b |= ( ( uint64_t )in[ 1] )  <<  8;
 // FALLTHROUGH
  case 1: b |= ( ( uint64_t )in[ 0] ); break;
 // FALLTHROUGH
  case 0: break;
  }


  v3 ^= b;

  //TRACE;
  for( i=0; i<cROUNDS; ++i ) SIPROUND;

  v0 ^= b;

  v2 ^= 0xff;

  //TRACE;
  for( i=0; i<dROUNDS; ++i ) SIPROUND;

  b = v0 ^ v1 ^ v2  ^ v3;

  return b;
}

#ifdef DBGFLAG_VALIDATE
//-----------------------------------------------------------------------------
// Purpose: validates memory structures
//-----------------------------------------------------------------------------
void CCrypto::ValidateStatics( CValidator &validator, const char *pchName )
{
}
#endif // DBGFLAG_VALIDATE
