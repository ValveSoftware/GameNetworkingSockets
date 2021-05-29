//====== Copyright Valve Corporation, All rights reserved. ====================

#include <atomic>
#include <tier1/utlbuffer.h>
#include "steamnetworkingsockets_internal.h"
#include "../tier1/ipv6text.h"

// Must be the last include
#include <tier0/memdbgon.h>

namespace SteamNetworkingSocketsLib
{


std::string Indent( const char *s )
{
	if ( s == nullptr || *s == '\0' )
		return std::string();

	// Make one pass through, and count up how long the result will be
	int l = 2; // initial tab, plus terminating '\0';
	for ( const char *p = s ; *p ; ++p )
	{
		++l;
		if ( *p == '\n' || *p == '\r' )
		{
			if ( p[1] != '\n' && p[1] != '\r' && p[1] != '\0' )
			{
				++l;
			}
		}
	}

	std::string result;
	result.reserve( l );
	result += '\t';
	for ( const char *p = s ; *p ; ++p )
	{
		result += *p;
		if ( *p == '\n' || *p == '\r' )
		{
			if ( p[1] != '\n' && p[1] != '\r' && p[1] != '\0' )
			{
				result += '\t';
			}
		}
	}

	return result;
}

const char *GetAvailabilityString( ESteamNetworkingAvailability a )
{
	switch ( a )
	{
		case k_ESteamNetworkingAvailability_CannotTry: return "Dependency unavailable";
		case k_ESteamNetworkingAvailability_Failed: return "Failed";
		case k_ESteamNetworkingAvailability_Waiting: return "Waiting";
		case k_ESteamNetworkingAvailability_Retrying: return "Retrying";
		case k_ESteamNetworkingAvailability_Previously: return "Lost";
		case k_ESteamNetworkingAvailability_NeverTried: return "Not Attempted";
		case k_ESteamNetworkingAvailability_Attempting: return "Attempting";
		case k_ESteamNetworkingAvailability_Current: return "OK";
	}

	Assert( false );
	return "???";
}

uint32 Murmorhash32( const void *data, size_t len )
{
  uint32 h = 0;
  const uint8 *key = (const uint8 *)data;
  if (len > 3) {
    const uint32* key_x4 = (const uint32*) key;
    size_t i = len >> 2;
    do {
      uint32 k = *key_x4++;
      k *= 0xcc9e2d51;
      k = (k << 15) | (k >> 17);
      k *= 0x1b873593;
      h ^= k;
      h = (h << 13) | (h >> 19);
      h = (h * 5) + 0xe6546b64;
    } while (--i);
    key = (const uint8*) key_x4;
  }
  if (len & 3) {
    size_t i = len & 3;
    uint32 k = 0;
    key = &key[i - 1];
    do {
      k <<= 8;
      k |= *key--;
    } while (--i);
    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;
    h ^= k;
  }
  h ^= len;
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

uint32 SteamNetworkingIdentityHash::operator()(struct SteamNetworkingIdentity const &x ) const
{
	// Make sure we don't have any packing or alignment issues
	COMPILE_TIME_ASSERT( offsetof( SteamNetworkingIdentity, m_eType ) == 0 );
	COMPILE_TIME_ASSERT( sizeof( x.m_eType ) == 4 );
	COMPILE_TIME_ASSERT( offsetof( SteamNetworkingIdentity, m_cbSize ) == 4 );
	COMPILE_TIME_ASSERT( sizeof( x.m_cbSize ) == 4 );
	COMPILE_TIME_ASSERT( offsetof( SteamNetworkingIdentity, m_steamID64 ) == 8 );

	return Murmorhash32( &x, sizeof( x.m_eType ) + sizeof( x.m_cbSize ) + x.m_cbSize );
}

} // namespace SteamNetworkingSocketsLib
using namespace SteamNetworkingSocketsLib;

///////////////////////////////////////////////////////////////////////////////
//
// SteamNetworkingIdentity helpers
//
///////////////////////////////////////////////////////////////////////////////

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingIPAddr_ToString( const SteamNetworkingIPAddr *pAddr, char *buf, size_t cbBuf, bool bWithPort )
{
	if ( pAddr->IsIPv4() )
	{
		const uint8 *ip4 = pAddr->m_ipv4.m_ip;
		if ( bWithPort )
			V_snprintf( buf, cbBuf, "%u.%u.%u.%u:%u", ip4[0], ip4[1], ip4[2], ip4[3], pAddr->m_port );
		else
			V_snprintf( buf, cbBuf, "%u.%u.%u.%u", ip4[0], ip4[1], ip4[2], ip4[3] );
	}
	else
	{
		char temp[ k_ncchMaxIPV6AddrStringWithoutPort ];
		IPv6IPToString( temp, pAddr->m_ipv6 );
		if ( bWithPort )
		{
			V_snprintf( buf, cbBuf, "[%s]:%u", temp, pAddr->m_port );
		}
		else
		{
			V_strncpy( buf, temp, cbBuf );
		}
	}
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingIPAddr_ParseString( SteamNetworkingIPAddr *pAddr, const char *pszStr )
{
	// IPv4?
	{
		int n1, n2, n3, n4, n5;
		int nRes = sscanf( pszStr, "%d.%d.%d.%d:%d", &n1, &n2, &n3, &n4, &n5 );
		if ( nRes >= 4 )
		{
			pAddr->Clear();

			// Assume 0 for port, if we weren't able to parse one.
			// Note that we could be accepting some bad IP addresses
			// here that we probably should reject.  E.g. "1.2.3.4:garbage"
			if ( nRes < 5 )
				n5 = 0;
			else if ( (uint16)n5 != n5 )
				return false; // port number not 16-bit value

			// Make sure octets are in range 0...255
			if ( ( n1 | n2 | n3 | n4 ) & ~0xff )
				return false;

			pAddr->m_ipv4.m_ffff = 0xffff;
			pAddr->m_ipv4.m_ip[0] = uint8(n1);
			pAddr->m_ipv4.m_ip[1] = uint8(n2);
			pAddr->m_ipv4.m_ip[2] = uint8(n3);
			pAddr->m_ipv4.m_ip[3] = uint8(n4);
			pAddr->m_port = uint16(n5);
			return true;
		}
	}

	// Try IPv6
	int port = -1;
	uint32_t scope;
	if ( !ParseIPv6Addr( pszStr, pAddr->m_ipv6, &port, &scope ) )
	{
		// ParseIPv6Addr might have modified some of the bytes -- so if we fail,
		// just always clear everything, so that behaviour is more consistent.
		pAddr->Clear();
		return false;
	}

	// Return port, if it was present
	pAddr->m_port = uint16( std::max( 0, port ) );

	// Parsed successfully
	return true;
}

STEAMNETWORKINGSOCKETS_INTERFACE ESteamNetworkingFakeIPType SteamNetworkingIPAddr_GetFakeIPType( const SteamNetworkingIPAddr *pAddr )
{
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		uint32 nIPv4 = pAddr->GetIPv4();
		return GetIPv4FakeIPType( nIPv4 );
	#else
		return k_ESteamNetworkingFakeIPType_NotFake;
	#endif
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingIdentity_ToString( const SteamNetworkingIdentity *pIdentity, char *buf, size_t cbBuf )
{
	switch ( pIdentity->m_eType )
	{
		case k_ESteamNetworkingIdentityType_Invalid:
			V_strncpy( buf, "invalid", cbBuf );
			break;

		case k_ESteamNetworkingIdentityType_SteamID:
			V_snprintf( buf, cbBuf, "steamid:%llu", (unsigned long long)pIdentity->m_steamID64 );
			break;

		case k_ESteamNetworkingIdentityType_IPAddress:
			V_strncpy( buf, "ip:", cbBuf );
			if ( cbBuf > 4 )
				pIdentity->m_ip.ToString( buf+3, cbBuf-3, pIdentity->m_ip.m_port != 0 );
			break;

		case k_ESteamNetworkingIdentityType_GenericString:
			V_snprintf( buf, cbBuf, "str:%s", pIdentity->m_szGenericString );
			break;

		case k_ESteamNetworkingIdentityType_GenericBytes:
			V_strncpy( buf, "gen:", cbBuf );
			if ( cbBuf > 5 )
			{
				static const char hexdigits[] = "0123456789abcdef";
				char *d = buf+4;
				int l = std::min( pIdentity->m_cbSize, int(cbBuf-5) / 2 );
				for ( int i = 0 ; i < l ; ++i )
				{
					uint8 b = pIdentity->m_genericBytes[i];
					*(d++) = hexdigits[b>>4];
					*(d++) = hexdigits[b&0xf];
				}
				*d = '\0';
			}
			break;

		case k_ESteamNetworkingIdentityType_UnknownType:
			V_strncpy( buf, pIdentity->m_szUnknownRawString, cbBuf );
			break;

		default:
			V_snprintf( buf, cbBuf, "bad_type:%d", pIdentity->m_eType );
	}
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamNetworkingIdentity_ParseString( SteamNetworkingIdentity *pIdentity, size_t sizeofIdentity, const char *pszStr )
{
	const size_t sizeofHeader = offsetof( SteamNetworkingIdentity, m_cbSize ) + sizeof( pIdentity->m_cbSize );
	COMPILE_TIME_ASSERT( sizeofHeader == 8 );

	// Safety check against totally bogus size
	if ( pIdentity == nullptr || sizeofIdentity < 32 )
		return false;
	memset( pIdentity, 0, sizeofIdentity );
	if ( pszStr == nullptr || *pszStr == '\0' )
		return false;

// NOTE: Reversing this decision.  99% of use cases, we really want the function to return
//       false unless the identity is valid.  The 1% of cases that want to allow this can
//       specifically check for this string.
//	if ( V_strcmp( pszStr, "invalid" ) == 0 )
//		return true; // Specifically parsed as invalid is considered "success"!

	size_t sizeofData = sizeofIdentity - sizeofHeader;

	if ( V_strncmp( pszStr, "steamid:", 8 ) == 0 )
	{
		pszStr += 8;
		unsigned long long temp;
		if ( sscanf( pszStr, "%llu", &temp ) != 1 )
			return false;
		CSteamID steamID( (uint64)temp );
		if ( !steamID.IsValid() )
			return false;
		pIdentity->SetSteamID64( (uint64)temp );
		return true;
	}

	if ( V_strncmp( pszStr, "ip:", 3 ) == 0 )
	{
		pszStr += 3;
		SteamNetworkingIPAddr tempAddr;
		if ( sizeofData < sizeof(tempAddr) )
			return false;
		if ( !tempAddr.ParseString( pszStr ) )
			return false;
		pIdentity->SetIPAddr( tempAddr );
		return true;
	}

	if ( V_strncmp( pszStr, "str:", 4 ) == 0 )
	{
		pszStr += 4;
		size_t l = strlen( pszStr );
		if ( l >= sizeofData )
			return false;
		return pIdentity->SetGenericString( pszStr );
	}

	if ( V_strncmp( pszStr, "gen:", 4 ) == 0 )
	{
		pszStr += 4;
		size_t l = strlen( pszStr );
		if ( l < 2 || (l & 1 ) != 0 )
			return false;
		size_t nBytes = l>>1;
		uint8 tmp[ SteamNetworkingIdentity::k_cbMaxGenericBytes ];
		if ( nBytes >= sizeofData || nBytes > sizeof(tmp) )
			return false;
		for ( size_t i = 0 ; i < nBytes ; ++i )
		{
			unsigned x;
			if ( sscanf( pszStr, "%2x", &x ) != 1 )
				return false;
			tmp[i] = (uint8)x;
			pszStr += 2;
		}

		return pIdentity->SetGenericBytes( tmp, nBytes );
	}

	// Unknown prefix.
	// The relays should always be running the latest code.  No client should
	// be using a protocol newer than a relay.
	#ifdef IS_STEAMDATAGRAMROUTER
		return false;
	#else

		// Does it looks like it is a string
		// of the form <prefix>:data ?  We assume that we will only
		// ever use prefixes from a restricted character set, and we
		// won't ever make them too long.
		int cchPrefix = 0;
		do
		{
			// Invalid type prefix or end of string?
			// Note: lowercase ONLY.  Identifiers are case sensitive (we need this to be true
			// because we want to be able to hash them and compare them as dumb bytes), so
			// any use of uppercase letters is really asking for big problems.
			char c = pszStr[cchPrefix];
			if ( ( c < 'a' || c > 'z' )
				&& ( c < '0' || c > '9' )
				&& c != '_'
			) {
				return false;
			}

			// Char is OK to be in the prefix, move on
			++cchPrefix;
			if ( cchPrefix > 16 )
				return false;
		} while ( pszStr[cchPrefix] != ':' );

		// OK, as far as we can tell, it might be valid --- unless it's too long
		int cbSize = V_strlen(pszStr)+1;
		if ( cbSize > SteamNetworkingIdentity::k_cchMaxString )
			return false;
		if ( (size_t)cbSize > sizeofData )
			return false;

		// Just save the exact raw string we were asked to "parse".  We don't
		// really understand it, but for many purposes just using the string
		// as an identifier will work fine!
		pIdentity->m_eType = k_ESteamNetworkingIdentityType_UnknownType;
		pIdentity->m_cbSize = cbSize;
		memcpy( pIdentity->m_szUnknownRawString, pszStr, cbSize );

		return true;
	#endif
}
