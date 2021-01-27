//========= Copyright Valve Corporation, All rights reserved. =================//
//
// Purpose: 
//
// NetAdr.cpp: implementation of the CNetAdr class.
//
//=============================================================================//

#include <functional>
#include <tier1/netadr.h>

#ifdef WIN32
	#include <ws2tcpip.h>
	#undef SetPort
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
#endif

#include <vstdlib/strtools.h>
#include "ipv6text.h"

const byte k_ipv6Bytes_LinkLocalAllNodes[16] = { 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }; // ff02:1
const byte k_ipv6Bytes_Loopback[16]          = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }; // ::1
const byte k_ipv6Bytes_Any[16]               = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // ::0

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

void netadr_t::ToString( char *pchBuffer, uint32 unBufferSize, bool baseOnly ) const
{
	if (type == NA_LOOPBACK_DEPRECATED)
	{
		V_strncpy (pchBuffer, "loopback", unBufferSize );
	}
	else if (type == NA_BROADCAST_DEPRECATED)
	{
		V_strncpy (pchBuffer, "broadcast", unBufferSize );
	}
	else if (type == NA_IP)
	{
		if ( baseOnly)
		{
			V_snprintf(pchBuffer, unBufferSize, "%i.%i.%i.%i", ipv4.b1, ipv4.b2, ipv4.b3, ipv4.b4 );
		}
		else
		{
			V_snprintf(pchBuffer, unBufferSize, "%i.%i.%i.%i:%i", ipv4.b1, ipv4.b2, ipv4.b3, ipv4.b4, port );
		}
	}
	else if (type == NA_IPV6)
	{
		// Format IP into a temp that we know is big enough
		char temp[ k_ncchMaxIPV6AddrStringWithPort ];
		if ( baseOnly )
			IPv6IPToString( temp, ipv6Byte );
		else
			IPv6AddrToString( temp, ipv6Byte, port, ipv6Scope );

		// Now put into caller's buffer, and handle truncation
		V_strncpy( pchBuffer, temp, unBufferSize );
	}
	else
	{
		V_strncpy(pchBuffer, "unknown", unBufferSize );
	}
}

// Is the IP part of one of the reserved blocks?
bool netadr_t::IsReservedAdr () const
{
	switch ( type )
	{
		case NA_LOOPBACK_DEPRECATED:
			return true;

		case NA_BROADCAST_DEPRECATED:
			// Makes no sense to me, but this is what the old code did
			return false;

		case NA_IP:
			if ( (ipv4.b1 == 10) ||									// 10.x.x.x is reserved
				(ipv4.b1 == 127) ||									// 127.x.x.x 
				(ipv4.b1 == 172 && ipv4.b2 >= 16 && ipv4.b2 <= 31) ||	// 172.16.x.x  - 172.31.x.x 
				(ipv4.b1 == 192 && ipv4.b2 >= 168) ) 					// 192.168.x.x
			{
				return true;
			}
			else
			{
				return false;
			}

		case NA_IPV6:
			// Private addresses, fc00::/7
			// Range is fc00:: to fdff:ffff:etc
			if ( ipv6Byte[0] >= 0xFC && ipv6Byte[1] <= 0xFD )
			{
				return true;
			}
			
			// Link-local fe80::/10
			// Range is fe80:: to febf::
			if ( ipv6Byte[0] == 0xFE
				&& ( ipv6Byte[1] >= 0x80 && ipv6Byte[1] <= 0xBF ) )
			{
				return true;
			}
			
			return false;

		default:
			Assert( false );
	}
	return false;
}

bool netadr_t::HasIP() const
{
	switch ( type )
	{
		default:
			Assert( false );
		case NA_NULL:
			return false;
		case NA_IP:
			if ( ip == 0 )
				return false;
			break;
		case NA_IPV6:
			if ( ipv6Qword[0] == 0 && ipv6Qword[1] == 0 )
				return false;
			break;
	}

	return true;
}

bool netadr_t::HasPort() const
{
	return ( port != 0 );
}

bool netadr_t::IsValid() const
{
	return HasIP() && HasPort();
}

bool netadr_t::CompareAdr(const netadr_t &a, bool onlyBase) const
{
	if ( a.type != type )
		return false;

	if ( type == NA_IP )
	{
		if ( !onlyBase && (port != a.port) )
			return false;

		if ( a.ip == ip )
			return true;
	}
	else if ( type == NA_IPV6 )
	{
		if ( !onlyBase )
		{
			if ( port != a.port )
				return false;

			// NOTE: We are intentionally not comparing the scope here.
			//       The examples where comparing the scope breaks simple
			//       stuff in unexpected ways seems more common than examples
			//       where you need to compare the scope.  If you need to compare
			//       them, then do it yourself.
		}

		if ( a.ipv6Qword[0] == ipv6Qword[0] && a.ipv6Qword[1] == ipv6Qword[1] )
			return true;
	}

	return false;
}

bool netadr_t::operator<(const netadr_t &netadr) const
{
	// NOTE: This differs from behaviour in Steam branch,
	//       because Steam has some legacy behaviour it needed
	//       to maintain.  We don't have this baggage and can
	//       do the sane thing.
	if ( type < netadr.type ) return true;
	if ( type > netadr.type ) return false;
	switch ( type )
	{
		case NA_IPV6:
		{
			int c = memcmp( ipv6Byte, netadr.ipv6Byte, sizeof(ipv6Byte) );
			if ( c < 0 ) return true;
			if ( c > 0 ) return false;
				
			// NOTE: Do not compare scope
			break;
		}
			
		case NA_IP:
			if ( ip < netadr.ip ) return true;
			if ( ip > netadr.ip ) return false;
			break;
	}

	// Break tie using port
	return port < netadr.port;
}

unsigned int netadr_t::GetHashKey( const netadr_t &netadr )
{
	// See: boost::hash_combine
	size_t result;
	switch ( netadr.type )
	{
		default:
			result = std::hash<int>{}( netadr.type );
			break;

		case NA_IP:
			result = std::hash<uint32>{}( netadr.ip );
			result ^= std::hash<uint16>{}( netadr.port ) + 0x9e3779b9 + (result << 6) + (result >> 2);
			break;

		case NA_IPV6:
			result = std::hash<uint64>{}( netadr.ipv6Qword[0] );
			result ^= std::hash<uint64>{}( netadr.ipv6Qword[1] ) + 0x9e3779b9 + (result << 6) + (result >> 2);
			result ^= std::hash<uint16>{}( netadr.port ) + 0x9e3779b9 + (result << 6) + (result >> 2);
			break;
	}

	return (unsigned int)result;
}

bool netadr_t::IsLoopback() const
{
	switch ( type )
	{
		case NA_NULL:
		case NA_BROADCAST_DEPRECATED:
			return false;
		case NA_LOOPBACK_DEPRECATED:
			return true;
		case NA_IP:
			return ( ip & 0xff000000 ) == 0x7f000000; // 127.x.x.x
		case NA_IPV6:
			return ipv6Qword[0] == 0 &&
			#ifdef VALVE_BIG_ENDIAN
				ipv6Qword[1] == 1;
			#else
				ipv6Qword[1] == 0x0100000000000000ull;
			#endif
	}
	Assert( false );
	return false;
}

bool netadr_t::IsBroadcast() const
{
	switch ( type )
	{
		case NA_NULL:
		case NA_LOOPBACK_DEPRECATED:
			return false;
		case NA_BROADCAST_DEPRECATED:
			return true;
		case NA_IP:
			return ip == 0xffffffff; // 255.255.255.255
		case NA_IPV6:
			// There might other IPs than could be construed as "broadcast",
			// but just check for the one used by SetIPV6Broadcast()
			return memcmp( ipv6Byte, k_ipv6Bytes_LinkLocalAllNodes, 16 ) == 0;
	}
	Assert( false );
	return false;
}

size_t netadr_t::ToSockadr(void *addr, size_t addr_size) const
{
	size_t struct_size = 0;
	memset( addr, 0, addr_size);

	switch ( type )
	{
		default:
		case NA_NULL:
			Assert( false );
			break;

		case NA_LOOPBACK_DEPRECATED:
		{
			if ( addr_size < sizeof(sockaddr_in) )
			{
				AssertMsg( false, "Address too small!" );
				return struct_size;
			}
			auto *s = (struct sockaddr_in*)addr;
			s->sin_family = AF_INET;
			COMPILE_TIME_ASSERT( (uint32)INADDR_LOOPBACK == INADDR_LOOPBACK ); // Defined as a 64-bit value on some platforms.
			s->sin_addr.s_addr = BigDWord( (uint32)INADDR_LOOPBACK );
			s->sin_port = BigWord( port );
			struct_size = sizeof(sockaddr_in);
		}
		break;

		case NA_BROADCAST_DEPRECATED:
		{
			if ( addr_size < sizeof(sockaddr_in) )
			{
				AssertMsg( false, "Address too small!" );
				return struct_size;
			}
			auto *s = (struct sockaddr_in*)addr;
			s->sin_family = AF_INET;
			COMPILE_TIME_ASSERT( (uint32)INADDR_BROADCAST == INADDR_BROADCAST ); // Defined as a 64-bit value on some platforms.
			s->sin_addr.s_addr = BigDWord( (uint32)INADDR_BROADCAST );
			s->sin_port = BigWord( port );
			struct_size = sizeof(sockaddr_in);
		}
		break;

		case NA_IP:
		{
			if ( addr_size < sizeof(sockaddr_in) )
			{
				AssertMsg( false, "Address too small!" );
				return struct_size;
			}
			auto *s = (struct sockaddr_in*)addr;
			s->sin_family = AF_INET;
			s->sin_addr.s_addr = BigDWord( ip );
			s->sin_port = BigWord( port );
			struct_size = sizeof(sockaddr_in);
		}
		break;

		case NA_IPV6:
		{
			if ( addr_size < sizeof(sockaddr_in6) )
			{
				AssertMsg( false, "Address too small!" );
				return struct_size;
			}
			auto *s = (struct sockaddr_in6*)addr;
			s->sin6_family = AF_INET6;
			COMPILE_TIME_ASSERT( sizeof(s->sin6_addr) == sizeof(ipv6Byte) );
			memcpy( &s->sin6_addr, ipv6Byte, sizeof(s->sin6_addr) );
			s->sin6_scope_id = ipv6Scope;
			s->sin6_port = BigWord( port );
			struct_size = sizeof(sockaddr_in6);
		}
		break;
	}

	return struct_size;
}

void netadr_t::GetIPV6( byte *result ) const
{
	switch ( type )
	{
		default:
			Assert( false );
		case NA_NULL:
			// ::
			memset( result, 0, 16 );
			break;

		case NA_LOOPBACK_DEPRECATED:
			// ::1
			memset( result, 0, 16 );
			result[15] = 1;
			return;

		case NA_BROADCAST_DEPRECATED:
			memcpy( result, k_ipv6Bytes_LinkLocalAllNodes, 16 );
			break;

		case NA_IP:
			// ::ffff:aabb.ccdd
			memset( result, 0, 10 );
			result[10] = 0xff;
			result[11] = 0xff;
			result[12] = ipv4.b1;
			result[13] = ipv4.b2;
			result[14] = ipv4.b3;
			result[15] = ipv4.b4;
			break;

		case NA_IPV6:
			memcpy( result, ipv6Byte, 16 );
			break;
	}
}

bool netadr_t::IsMappedIPv4() const
{
	if ( type != NA_IPV6 )
		return false;
	if (
		ipv6Qword[0] != 0 // 0...7
		|| ipv6Byte[8] != 0
		|| ipv6Byte[9] != 0
		|| ipv6Byte[10] != 0xff
		|| ipv6Byte[11] != 0xff
	) {
		return false;
	}
	return true;
}

bool netadr_t::BConvertMappedToIPv4()
{
	if ( !IsMappedIPv4() )
		return false;
	SetIPv4( ipv6Byte[12], ipv6Byte[13], ipv6Byte[14], ipv6Byte[15] );
	return true;
}

bool netadr_t::BConvertIPv4ToMapped()
{
	if ( type != NA_IP )
		return false;

	// Copy off IPv4 address, since it shares the same memory
	// as the IPv6 bytes.  And we don't want to write code that depends
	// on how the memory is laid out or try to be clever.
	uint8 b1 = ipv4.b1;
	uint8 b2 = ipv4.b2;
	uint8 b3 = ipv4.b3;
	uint8 b4 = ipv4.b4;

	type = NA_IPV6;

	// ::ffff:aabb.ccdd
	memset( ipv6Byte, 0, 10 );
	ipv6Byte[10] = 0xff;
	ipv6Byte[11] = 0xff;
	ipv6Byte[12] = b1;
	ipv6Byte[13] = b2;
	ipv6Byte[14] = b3;
	ipv6Byte[15] = b4;

	ipv6Scope = 0;

	return true;
}

void netadr_t::ToSockadrIPV6(void *addr, size_t addr_size) const
{
	memset( addr, 0, addr_size);
	if ( addr_size < sizeof(sockaddr_in6) )
	{
		AssertMsg( false, "Address too small!" );
		return;
	}
	auto *s = (struct sockaddr_in6*)addr;
	s->sin6_family = AF_INET6;
	GetIPV6( s->sin6_addr.s6_addr );
	if ( type == NA_IPV6 )
		s->sin6_scope_id = ipv6Scope;
	s->sin6_port = BigWord( port );
}

bool netadr_t::SetFromSockadr(const void *addr, size_t addr_size)
{
	Clear();
	const auto *s = (const sockaddr *)addr;
	if (!s || addr_size < sizeof(s->sa_family) )
	{
		Assert( false );
		return false;
	}
	switch ( s->sa_family )
	{
		case AF_INET:
		{
			if ( addr_size < sizeof(sockaddr_in) )
			{
				Assert( false );
				return false;
			}
			const auto *sin = (const sockaddr_in *)addr;
			type = NA_IP;
			ip = BigDWord( sin->sin_addr.s_addr );
			port = BigWord( sin->sin_port );
			return true;
		}

		case AF_INET6:
		{
			if ( addr_size < sizeof(sockaddr_in6) )
			{
				Assert( false );
				return false;
			}
			const auto *sin6 = (const sockaddr_in6 *)addr;
			type = NA_IPV6;
			COMPILE_TIME_ASSERT( sizeof(sin6->sin6_addr) == sizeof(ipv6Byte) );
			memcpy( ipv6Byte, &sin6->sin6_addr, sizeof(ipv6Byte) );
			ipv6Scope = sin6->sin6_scope_id;
			port = BigWord( sin6->sin6_port );
			return true;
		}
	}
	return false;
}


bool netadr_t::SetFromString( const char *pch )
{
	Clear();

	if ( !pch || pch[0] == 0 )			// but let's not crash
		return false;

	if ( pch[0] >= '0' && pch[0] <= '9' && strchr( pch, '.' ) )
	{
		int n1, n2, n3, n4, n5;
		int nRes = sscanf( pch, "%d.%d.%d.%d:%d", &n1, &n2, &n3, &n4, &n5 );
		if ( nRes >= 4 )
		{
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

			SetIPv4( n1, n2, n3, n4 );
			SetPort( ( uint16 ) n5 );
			return true;
		}
	}

	// IPv6?
	int tmpPort;
	uint32_t tmpScope;
	if ( ParseIPv6Addr( pch, ipv6Byte, &tmpPort, &tmpScope ) )
	{
		type = NA_IPV6;
		if ( tmpPort >= 0 )
			port = (uint16)tmpPort;
        ipv6Scope = tmpScope;
		return true;
	}

	//	clobber partial state possibly left by ParseIPv6Addr
	Clear();

	return false;
}
