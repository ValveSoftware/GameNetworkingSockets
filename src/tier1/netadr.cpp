//========= Copyright Valve Corporation, All rights reserved. =================//

#include <functional>

#include <tier1/netadr.h>
#include <tier0/platform_sockets.h>

#include <vstdlib/strtools.h>
#include "ipv6text.h"

const byte k_ipv6Bytes_LinkLocalAllNodes[16] = { 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }; // ff02:1
const byte k_ipv6Bytes_Loopback[16]          = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }; // ::1
const byte k_ipv6Bytes_Any[16]               = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // ::0


//---------------------------------------------------------------------------------
//
// CIPAddress
//
//---------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Purpose: Convert the given IP address to a string representation.
//			Optionally will include the given port if punPort is not null.
//-----------------------------------------------------------------------------
void CIPAddress::ToString( char *pchBuffer, uint32 unBufferSize, const uint16 *punPort ) const
{
	if (m_usType == k_EIPTypeLoopbackDeprecated)
	{
		V_strncpy (pchBuffer, "loopback", unBufferSize );
	}
	else if (m_usType == k_EIPTypeBroadcastDeprecated)
	{
		V_strncpy (pchBuffer, "broadcast", unBufferSize );
	}
	else if (m_usType == k_EIPTypeV4)
	{
		if ( !punPort )
		{
			V_snprintf(pchBuffer, unBufferSize, "%i.%i.%i.%i", m_IPv4Bytes.b1, m_IPv4Bytes.b2, m_IPv4Bytes.b3, m_IPv4Bytes.b4 );
		}
		else
		{
			V_snprintf(pchBuffer, unBufferSize, "%i.%i.%i.%i:%i", m_IPv4Bytes.b1, m_IPv4Bytes.b2, m_IPv4Bytes.b3, m_IPv4Bytes.b4, *punPort );
		}
	}
	else if (m_usType == k_EIPTypeV6)
	{
		// Format IP into a temp that we know is big enough
		char temp[ k_cchMaxIPV6AddrStringWithPort ];
		if ( !punPort )
			IPv6IPToString( temp, m_rgubIPv6 );
		else
			IPv6AddrToString( temp, m_rgubIPv6, *punPort, m_unIPv6Scope );

		// Now put into caller's buffer, and handle truncation
		V_strncpy( pchBuffer, temp, unBufferSize );
	}
	else
	{
		V_strncpy(pchBuffer, "unknown", unBufferSize );
	}
}


//-----------------------------------------------------------------------------
// Purpose: Is this IP part of one of the reserved blocks?
//-----------------------------------------------------------------------------
bool CIPAddress::IsReservedAdr () const
{
	// The code below will be incorrect if we are a ipv4-mapped-to-ipv6 address
	// So - unmap ourselves to a temp CIPAddress and ask it
	if ( IsMappedIPv4() )
	{
		CIPAddress ipTemp = *this;
		ipTemp.BConvertMappedToIPv4();
		return ipTemp.IsReservedAdr();
	}

	switch ( m_usType )
	{
		case k_EIPTypeLoopbackDeprecated:
			return true;

		case k_EIPTypeBroadcastDeprecated:
			// Makes no sense to me, but this is what the old code did
			return false;

		case k_EIPTypeV4:
			if ( (m_IPv4Bytes.b1 == 10) ||									// 10.x.x.x is reserved
				(m_IPv4Bytes.b1 == 127) ||									// 127.x.x.x 
				(m_IPv4Bytes.b1 == 169 && m_IPv4Bytes.b2 == 254) ||			// 169.254.x.x is link-local ipv4
				(m_IPv4Bytes.b1 == 172 && m_IPv4Bytes.b2 >= 16 && m_IPv4Bytes.b2 <= 31) ||	// 172.16.x.x  - 172.31.x.x 
				(m_IPv4Bytes.b1 == 192 && m_IPv4Bytes.b2 >= 168) ) 					// 192.168.x.x
			{
				return true;
			}
			else
			{
				return false;
			}

		case k_EIPTypeV6:
			// Private addresses, fc00::/7
			// Range is fc00:: to fdff:ffff:etc
			if ( m_rgubIPv6[0] >= 0xFC && m_rgubIPv6[1] <= 0xFD )
			{
				return true;
			}
			
			// Link-local fe80::/10
			// Range is fe80:: to febf::
			if ( m_rgubIPv6[0] == 0xFE
				&& ( m_rgubIPv6[1] >= 0x80 && m_rgubIPv6[1] <= 0xBF ) )
			{
				return true;
			}
			
			return false;

		default:
			Assert( false );
	}
	return false;
}


//-----------------------------------------------------------------------------
// Purpose: Does this object have an IP value set
//-----------------------------------------------------------------------------
bool CIPAddress::HasIP() const
{
	switch ( m_usType )
	{
		default:
			Assert( false );
			// FALLTHROUGH
		case k_EIPTypeInvalid:
			return false;
		case k_EIPTypeV4:
			if ( m_unIPv4 == 0 )
				return false;
			break;
		case k_EIPTypeV6:
			if ( m_ipv6Qword[0] == 0 && m_ipv6Qword[1] == 0 )
				return false;
			break;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Compare this IP address with another for equality
//-----------------------------------------------------------------------------
bool CIPAddress::operator==(const CIPAddress &a) const
{
	if ( a.m_usType != m_usType )
		return false;

	if ( m_usType == k_EIPTypeV4 )
	{
		if ( a.m_unIPv4 == m_unIPv4 )
			return true;
	}
	else if ( m_usType == k_EIPTypeV6 )
	{
		// NOTE: We are intentionally not comparing the scope here.
		//       The examples where comparing the scope breaks simple
		//       stuff in unexpected ways seems more common than examples
		//       where you need to compare the scope.  If you need to compare
		//       them, then do it yourself.

		if ( a.m_ipv6Qword[0] == m_ipv6Qword[0] && a.m_ipv6Qword[1] == m_ipv6Qword[1] )
			return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Inequality comparison
//-----------------------------------------------------------------------------
bool CIPAddress::operator!=(const CIPAddress &netadr) const
{
	return !operator==(netadr);
}


//-----------------------------------------------------------------------------
// Purpose: For sorting lists/trees of IP addresses
//-----------------------------------------------------------------------------
bool CIPAddress::operator<(const CIPAddress &netadr) const
{
	if ( m_usType < netadr.m_usType ) return true;
	if ( m_usType > netadr.m_usType ) return false;

	// Types match
	if ( m_usType == k_EIPTypeV6 )
	{
		int c = memcmp( m_rgubIPv6, netadr.m_rgubIPv6, sizeof(m_rgubIPv6) );
		if ( c < 0 ) return true;
		if ( c > 0 ) return false;
	}
	else
	{
		if ( m_unIPv4 < netadr.m_unIPv4 ) return true;
		if ( m_unIPv4 > netadr.m_unIPv4 ) return false;
	}

	// equal
	return false;
}

unsigned int CIPAddress::GetHashKey( const CIPAddress &netadr )
{
	// See: boost::hash_combine
	size_t result;
	switch ( netadr.m_usType )
	{
		default:
			result = std::hash<int>{}( netadr.m_usType );
			break;

		case k_EIPTypeV4:
			result = std::hash<uint32>{}( netadr.m_unIPv4 );
			break;

		case k_EIPTypeV6:
			result = std::hash<uint64>{}( netadr.m_ipv6Qword[0] );
			result ^= std::hash<uint64>{}( netadr.m_ipv6Qword[1] ) + 0x9e3779b9 + (result << 6) + (result >> 2);
			break;
	}

	return (unsigned int)result;
}

//-----------------------------------------------------------------------------
// Purpose: Legacy - just returns HasIP()
//-----------------------------------------------------------------------------
bool CIPAddress::IsValid() const
{
	return HasIP();
}

//-----------------------------------------------------------------------------
// Purpose: Is this IP address a "loopback" special address, such as 127.0.0.1?
//-----------------------------------------------------------------------------
bool CIPAddress::IsLoopback() const
{
	// The code below will be incorrect if we are a ipv4-mapped-to-ipv6 address
	// So - unmap ourselves to a temp CIPAddress and ask it
	if ( IsMappedIPv4() )
	{
		CIPAddress ipTemp = *this;
		ipTemp.BConvertMappedToIPv4();
		return ipTemp.IsLoopback();
	}

	switch ( m_usType )
	{
		case k_EIPTypeInvalid:
		case k_EIPTypeBroadcastDeprecated:
			return false;
		case k_EIPTypeLoopbackDeprecated:
			return true;
		case k_EIPTypeV4:
			return ( m_unIPv4 & 0xff000000 ) == 0x7f000000; // 127.x.x.x
		case k_EIPTypeV6:
			return m_ipv6Qword[0] == 0 &&
			#ifdef VALVE_BIG_ENDIAN
				m_ipv6Qword[1] == 1;
			#else
				m_ipv6Qword[1] == 0x0100000000000000ull;
			#endif
	}
	Assert( false );
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Is this IP address a broadcast address?
//-----------------------------------------------------------------------------
bool CIPAddress::IsBroadcast() const
{
	// The code below will be incorrect if we are a ipv4-mapped-to-ipv6 address
	// So - unmap ourselves to a temp CIPAddress and return it
	if ( IsMappedIPv4() )
	{
		CIPAddress ipTemp = *this;
		ipTemp.BConvertMappedToIPv4();
		return ipTemp.IsBroadcast();
	}

	switch ( m_usType )
	{
		case k_EIPTypeInvalid:
		case k_EIPTypeLoopbackDeprecated:
			return false;
		case k_EIPTypeBroadcastDeprecated:
			return true;
		case k_EIPTypeV4:
			return m_unIPv4 == 0xffffffff; // 255.255.255.255
		case k_EIPTypeV6:
			// There might other IPs than could be construed as "broadcast",
			// but just check for the one used by SetIPV6Broadcast()
			return memcmp( m_rgubIPv6, k_ipv6Bytes_LinkLocalAllNodes, 16 ) == 0;
	}
	Assert( false );
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Get the ipv6 bytes (network byte order) of this address. Works even
//			if this is an m_IPv4Bytes address, it will return a "mapped" ipv6 address.
//-----------------------------------------------------------------------------
void CIPAddress::GetIPV6( byte *result ) const
{
	switch ( m_usType )
	{
		default:
			Assert( false );
			// FALLTHROUGH
		case k_EIPTypeInvalid:
			// ::
			memset( result, 0, 16 );
			break;

		case k_EIPTypeLoopbackDeprecated:
			// ::1
			memset( result, 0, 16 );
			result[15] = 1;
			return;

		case k_EIPTypeBroadcastDeprecated:
			memcpy( result, k_ipv6Bytes_LinkLocalAllNodes, 16 );
			break;

		case k_EIPTypeV4:
			// ::ffff:aabb.ccdd
			memset( result, 0, 10 );
			result[10] = 0xff;
			result[11] = 0xff;
			result[12] = m_IPv4Bytes.b1;
			result[13] = m_IPv4Bytes.b2;
			result[14] = m_IPv4Bytes.b3;
			result[15] = m_IPv4Bytes.b4;
			break;

		case k_EIPTypeV6:
			memcpy( result, m_rgubIPv6, 16 );
			break;
	}
}


//-----------------------------------------------------------------------------
// Purpose: Is this an ipv6 address that is actually a mapped m_IPv4Bytes address
//-----------------------------------------------------------------------------
bool CIPAddress::IsMappedIPv4() const
{
	if ( m_usType != k_EIPTypeV6 )
		return false;
	if (
		m_ipv6Qword[0] != 0 // 0...7
		|| m_rgubIPv6[8] != 0
		|| m_rgubIPv6[9] != 0
		|| m_rgubIPv6[10] != 0xff
		|| m_rgubIPv6[11] != 0xff
	) {
		return false;
	}
	return true;
}


//-----------------------------------------------------------------------------
// Purpose: For an "m_IPv4Bytes address mapped into ipv6 space", this will internally 
//			convert it to a plain m_IPv4Bytes address.
//-----------------------------------------------------------------------------
bool CIPAddress::BConvertMappedToIPv4()
{
	if ( !IsMappedIPv4() )
		return false;
	SetIPv4( m_rgubIPv6[12], m_rgubIPv6[13], m_rgubIPv6[14], m_rgubIPv6[15] );
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: For an m_IPv4Bytes address, this will internally convert it into a mapped
//			address in ipv6 space.
//-----------------------------------------------------------------------------
bool CIPAddress::BConvertIPv4ToMapped()
{
	if ( m_usType != k_EIPTypeV4 )
		return false;

	// Copy off IPv4 address, since it shares the same memory
	// as the IPv6 bytes.  And we don't want to write code that depends
	// on how the memory is laid out or try to be clever.
	uint8 b1 = m_IPv4Bytes.b1;
	uint8 b2 = m_IPv4Bytes.b2;
	uint8 b3 = m_IPv4Bytes.b3;
	uint8 b4 = m_IPv4Bytes.b4;

	m_usType = k_EIPTypeV6;

	// ::ffff:aabb.ccdd
	memset( m_rgubIPv6, 0, 10 );
	m_rgubIPv6[10] = 0xff;
	m_rgubIPv6[11] = 0xff;
	m_rgubIPv6[12] = b1;
	m_rgubIPv6[13] = b2;
	m_rgubIPv6[14] = b3;
	m_rgubIPv6[15] = b4;

	m_unIPv6Scope = 0;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Initialize the object from a sockaddr structure. May be m_IPv4Bytes or ipv6.
//-----------------------------------------------------------------------------
bool CIPAddress::SetFromSockadr(const void *addr, size_t addr_size, uint16 *punPort )
{
	Clear();
	const auto *s = (const sockaddr *)addr;
	if (!s || addr_size < sizeof(s->sa_family) )
	{
		Assert( false );
		return false;
	}
	switch ( (int)s->sa_family )
	{
		case AF_INET:
		{
			if ( addr_size < sizeof(sockaddr_in) )
			{
				Assert( false );
				return false;
			}
			const auto *sin = (const sockaddr_in *)addr;
			m_usType = k_EIPTypeV4;
			m_unIPv4 = BigDWord( sin->sin_addr.s_addr );
			if ( punPort )
				*punPort = BigWord( sin->sin_port );

			return true;
		}

#ifndef PLATFORM_NO_IPV6
		case AF_INET6:
		{
			if ( addr_size < sizeof(sockaddr_in6) )
			{
				Assert( false );
				return false;
			}
			const auto *sin6 = (const sockaddr_in6 *)addr;
			m_usType = k_EIPTypeV6;
			COMPILE_TIME_ASSERT( sizeof(sin6->sin6_addr) == sizeof(m_rgubIPv6) );
			memcpy( m_rgubIPv6, &sin6->sin6_addr, sizeof(m_rgubIPv6) );
			m_unIPv6Scope = sin6->sin6_scope_id;
			if ( punPort )
				*punPort = BigWord( sin6->sin6_port );

			return true;
		}
#endif
	}
	return false;
}


//-----------------------------------------------------------------------------
// Purpose: Initialize from a string representation of either an m_IPv4Bytes or ipv6
//			address. Optionally can return the port number, if any was found 
//			in the string.
//-----------------------------------------------------------------------------
bool CIPAddress::SetFromString( const char *pch, uint16 *punPort )
{

	Clear();

	if ( punPort )
		*punPort = 0;

	if ( !pch || pch[0] == 0 )			// but let's not crash
		return false;

	if ( pch[0] >= '0' && pch[0] <= '9' && strchr( pch, '.' ) )
	{
		int n1, n2, n3, n4, n5 = 0;
		int nRes = sscanf( pch, "%d.%d.%d.%d:%d", &n1, &n2, &n3, &n4, &n5 );
		if ( nRes >= 4 )
		{
			// Make sure octets are in range 0...255 and port number is legit
			if ( ( ( n1 | n2 | n3 | n4 ) & ~0xff ) || (uint16)n5 != n5 )
				return false;

			SetIPv4( n1, n2, n3, n4 );
			if ( punPort )
			{
				*punPort = (uint16) n5;
			}
			return true;
		}
	}

	// IPv6?
	int tmpPort;
	uint32_t tmpScope;
	if ( ParseIPv6Addr( pch, m_rgubIPv6, &tmpPort, &tmpScope ) )
	{
		m_usType = k_EIPTypeV6;
		if ( tmpPort >= 0 && punPort )
			*punPort = (uint16)tmpPort;
        m_unIPv6Scope = tmpScope;
		return true;
	}

	//	clobber partial state possibly left by ParseIPv6Addr
	Clear();

	return false;
}
//---------------------------------------------------------------------------------
//
// CIPAndPort
//
//---------------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Purpose: Convert the given IP+port combination to a string representation.
//-----------------------------------------------------------------------------
void CIPAndPort::ToString( char *pchBuffer, uint32 unBufferSize, bool baseOnly ) const
{
	CIPAddress::ToString( pchBuffer, unBufferSize, baseOnly ? nullptr : &m_usPort );
}


//-----------------------------------------------------------------------------
// Purpose: Has a port value been set
//-----------------------------------------------------------------------------
bool CIPAndPort::HasPort() const
{
	return ( m_usPort != 0 );
}

//-----------------------------------------------------------------------------
// Purpose: Legacy - only valid if both the IP and the Port are set
//-----------------------------------------------------------------------------
bool CIPAndPort::IsValid() const
{
	return HasIP() && HasPort();
}





//-----------------------------------------------------------------------------
// Purpose: Compare m_unIPv4+port combo for equality
//-----------------------------------------------------------------------------
bool CIPAndPort::CompareAdr(const CIPAndPort &a, bool onlyBase) const
{
	if ( CIPAddress::operator==(a) )
	{
		if ( onlyBase )
			return true;
		else
			return m_usPort == a.m_usPort;
	}
	else
	{
		return false;
	}
}


//-----------------------------------------------------------------------------
// Purpose: For sorting lists/trees of IP+port combinations
//-----------------------------------------------------------------------------
bool CIPAndPort::operator<(const CIPAndPort &netadr) const
{
	// If either address is IPv6, use sane behaviour
	if ( m_usType == k_EIPTypeV6 || netadr.m_usType == k_EIPTypeV6 )
	{
		if ( m_usType < netadr.m_usType ) return true;
		if ( m_usType > netadr.m_usType ) return false;
		Assert( m_usType == k_EIPTypeV6 && netadr.m_usType == k_EIPTypeV6 );
		int c = memcmp( m_rgubIPv6, netadr.m_rgubIPv6, sizeof(m_rgubIPv6) );
		if ( c < 0 ) return true;
		if ( c > 0 ) return false;
	}
	else
	{
		// Even if types differ, just compare the IP portion?
		// This seems wrong to me, but I'm scared to change it
		// in case I break existing behaviour
		if ( m_unIPv4 < netadr.m_unIPv4 ) return true;
		if ( m_unIPv4 > netadr.m_unIPv4 ) return false;
	}

	// port breaks the tie
	return m_usPort < netadr.m_usPort;
}

unsigned int CIPAndPort::GetHashKey( const CIPAndPort &netadr )
{
	// See: boost::hash_combine
	size_t result = CIPAddress::GetHashKey( netadr );
	if ( netadr.m_usType != k_EIPTypeInvalid )
	{
		result ^= std::hash<uint16>{}( netadr.m_usPort ) + 0x9e3779b9 + (result << 6) + (result >> 2);
	}

	return (unsigned int)result;
}



//-----------------------------------------------------------------------------
// Purpose: Convert this m_unIPv4+port to a 'sockaddr' structure
//-----------------------------------------------------------------------------
size_t CIPAndPort::ToSockadr(void *addr, size_t addr_size) const
{
	size_t struct_size = 0;
	memset( addr, 0, addr_size);

	switch ( m_usType )
	{
		default:
		case k_EIPTypeInvalid:
			Assert( false );
			break;

		case k_EIPTypeLoopbackDeprecated:
		{
			if ( addr_size < sizeof(sockaddr_in) )
			{
				AssertMsg( false, "Address too small!" );
				return struct_size;
			}
			auto *s = (struct sockaddr_in*)addr;
			s->sin_family = AF_INET;
			s->sin_addr.s_addr = BigDWord( INADDR_LOOPBACK );
			s->sin_port = BigWord( m_usPort );
			struct_size = sizeof(sockaddr_in);
		}
		break;

		case k_EIPTypeBroadcastDeprecated:
		{
			if ( addr_size < sizeof(sockaddr_in) )
			{
				AssertMsg( false, "Address too small!" );
				return struct_size;
			}
			auto *s = (struct sockaddr_in*)addr;
			s->sin_family = AF_INET;
			s->sin_addr.s_addr = BigDWord( INADDR_BROADCAST );
			s->sin_port = BigWord( m_usPort );
			struct_size = sizeof(sockaddr_in);
		}
		break;

		case k_EIPTypeV4:
		{
			if ( addr_size < sizeof(sockaddr_in) )
			{
				AssertMsg( false, "Address too small!" );
				return struct_size;
			}
			auto *s = (struct sockaddr_in*)addr;
			s->sin_family = AF_INET;
			s->sin_addr.s_addr = BigDWord( m_unIPv4 );
			s->sin_port = BigWord( m_usPort );
			struct_size = sizeof(sockaddr_in);
		}
		break;

#ifndef PLATFORM_NO_IPV6
		case k_EIPTypeV6:
		{
			if ( addr_size < sizeof(sockaddr_in6) )
			{
				AssertMsg( false, "Address too small!" );
				return struct_size;
			}
			auto *s = (struct sockaddr_in6*)addr;
			s->sin6_family = AF_INET6;
			COMPILE_TIME_ASSERT( sizeof(s->sin6_addr) == sizeof(m_rgubIPv6) );
			memcpy( &s->sin6_addr, m_rgubIPv6, sizeof(s->sin6_addr) );
			s->sin6_scope_id = m_unIPv6Scope;
			s->sin6_port = BigWord( m_usPort );
			struct_size = sizeof(sockaddr_in6);
		}
		break;
#endif // #ifndef PLATFORM_NO_IPV6
	}

	return struct_size;
}




//-----------------------------------------------------------------------------
// Purpose: Convert to an ipv6 sockaddr structure
//-----------------------------------------------------------------------------
#ifndef PLATFORM_NO_IPV6
void CIPAndPort::ToSockadrIPV6(void *addr, size_t addr_size) const
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
	if ( m_usType == k_EIPTypeV6 )
		s->sin6_scope_id = m_unIPv6Scope;
	s->sin6_port = BigWord( m_usPort );
}
#endif // PLATFORM_NO_IPV6
