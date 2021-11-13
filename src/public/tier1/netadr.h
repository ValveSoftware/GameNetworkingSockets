//========= Copyright © Valve Corporation, All rights reserved. ============//
#ifndef NETADR_H
#define NETADR_H
#ifdef _WIN32
#pragma once
#endif

#include <tier0/basetypes.h>
#include <tier0/dbg.h>
#undef SetPort

// Max length of a netadr_t in string format (with port),
// including null
const int k_ncchMaxNetAdrString = 48;

typedef enum : unsigned short
{ 
	// Reserved invalid/dummy address type
	k_EIPTypeInvalid = 0,

	// Do not use this.  In some primordial code, "loopback" often actually meant "localhost",
	// and sometimes it meant "internal buffers, not actually using the network system at all."
	// We don't need the latter in Steam, and we don't need a separate address for the former (just
	// use the appropriate reserved address!)
	k_EIPTypeLoopbackDeprecated,

	// Do not use this.  There are already reserved IP addresses to refer to this concept.
	// It is not a separate address *type*.  In fact, there is an IPv4 broadcast addresses and an
	// IPv6 broadcast address, so it's not entirely clear exactly what this means.
	k_EIPTypeBroadcastDeprecated,

	k_EIPTypeV4,
	k_EIPTypeV6,
} ipadrtype_t;

#define NETADR_DEFINED
#pragma pack(push,1)

extern const byte k_ipv6Bytes_LinkLocalAllNodes[16];
extern const byte k_ipv6Bytes_Loopback[16];
extern const byte k_ipv6Bytes_Any[16];

//-----------------------------------------------------------------------------
// CIPAddress
//
// Class to encapsulate any IP address regardless of flavor (ipv4,v6)
// 
//-----------------------------------------------------------------------------
class CIPAddress
{
public:

	inline CIPAddress() { Clear(); }
	explicit	CIPAddress( uint unIP ) { Clear(); SetIPv4( unIP ); }
	explicit	CIPAddress( const char *pch ) { Clear(); SetFromString( pch ); }

	// NOTE: For historical reasons, the default constructor sets address type
	// to *k_EIPTypeV4* (not k_EIPTypeInvalid!) but the IP and port are 0, so IsValid() will return false
	void	Clear() {
		memset( m_rgubIPv6, 0, sizeof( m_rgubIPv6 ) );
		m_unIPv6Scope = 0;
		m_usType = k_EIPTypeV4;
	}

	/// Get address type
	ipadrtype_t GetType() const { return ipadrtype_t( m_usType ); }

	/// Set address type without changing any other fields
	void	SetType( ipadrtype_t type );

	/// Set IPv4 IP given host-byte order argument.
	/// Also slams the address type to k_EIPTypeV4.  Port does not change
	void	SetIPv4(uint unIP);

	/// Assignment. Equivalent to operator=() but is better
	/// for CIPAndPort to use
	void	SetIP( const CIPAddress that ) { *this = that; }

	/// Set IPv4 given individual address octets.
	/// Also slams the address type to k_EIPTypeV4.  Port does not change
	void	SetIPv4(uint8 b1, uint8 b2, uint8 b3, uint8 b4);

	/// Attempt to parse address string.  Will never attempt
	/// DNS name resolution.  Returns false if we cannot parse an
	/// IPv4 address or IPv6 address.  If the port is not present,
	/// it is set to zero.
	bool    SetFromString( const char *psz, uint16 *punPort = nullptr );

	/// Set to IPv4 broadcast address.  Does not change the port
	void	SetIPV4Broadcast() { SetIPv4( 0xffffffff ); }

	/// Set to IPv6 broadcast (actually link scope all nodes) address on the specified
	/// IPv6 scope.  Does not change the port
	void	SetIPV6Broadcast( uint32 nScope = 0 ) { SetIPV6( k_ipv6Bytes_LinkLocalAllNodes, nScope ); }

	/// Set to IPv4 loopback address (127.0.0.1).  Does not change the port
	void	SetIPV4Loopback() { SetIPv4( 0x7f000001 ); }

	/// Set to IPv6 loopback address (::1).  The scope is reset to zero.
	/// Does not change the port.
	void	SetIPV6Loopback() { SetIPV6( k_ipv6Bytes_Loopback, 0 ); }

	/// Set to IPV4 "any" address, i.e. INADDR_ANY = 0.0.0.0.
	void	SetIPV4Any() { SetIPv4( 0 ); }

	/// Set to IPV6 "any" address, i.e. IN6ADDR_ANY_INIT (all zeroes)
	void	SetIPV6Any() { SetIPV6( k_ipv6Bytes_Any, 0 ); }

	/// Get the IPv4 IP, in host byte order.  Should only be called on IPv4 addresses.
	/// For historical reasons, this can be called on an k_EIPTypeInvalid address, and usually
	/// will return 0.
	uint	GetIPv4() const;


	/// Get IPv6 bytes.  This will work on any address type
	/// An k_EIPTypeInvalid address returns all zeros.
	/// For the IPv4 address aa.bb.cc.dd, we will return ::ffff:aabb:ccdd
	void GetIPV6( byte *result ) const;

	/// Get pointer to IPV6 bytes.  This should only be called on IPv6
	/// address, will assert otherwise
	const byte *GetIPV6Bytes() const;

	/// Set IPv6 address (as 16 bytes) and scope.
	/// This slams the address type to k_EIPTypeV6, but
	/// does not alter the port.
	void SetIPV6( const byte *bytes, uint32 nScope = 0 );

	/// Get IPv6 scope ID
	uint32 GetIPV6Scope() const;

	/// Set IPv6 scope.  This is only valid for IPv6 addresses.
	/// Will assert for other types
	void SetIPV6Scope( uint32 nScope );

	/// Return true if we are an IPv4-mapped IPv6 address. (::ffff:1.2.3.4)
	bool IsMappedIPv4() const;

	/// If we are an IPv4-mapped IPv6 address, convert to ordinary IPv4
	/// address and return true. Otherwise return false.  The port is not altered.
	bool BConvertMappedToIPv4();

	/// If we are an ordinary IPv4 address, convert to a IPv4-mapped IPv6
	/// address and return true.  Otherwise return false.  The scope is cleared
	/// to zero, and the port is not altered.
	bool BConvertIPv4ToMapped();

	/// Get string representation
	/// IPv4: xxx.xxx.xxx.xxx
	/// IPv6: applies all of RFC5952 rules to get the canonical text representation.
	///       If !onlyBase, then the IP is surrounded by brackets to disambiguate colons
	///       in the address from the port separator: "[aabb::1234]"
	void	ToString( char *pchBuffer, uint32 unBufferSize, const uint16 *punPort = nullptr ) const;

	/// ToString, but with automatic buffer size deduction
	template< size_t maxLenInChars >
	char*	ToString_safe( char (&pDest)[maxLenInChars], const uint16 *punPort = nullptr ) const
	{
		ToString( &pDest[0], maxLenInChars, punPort );
		return	pDest;
	}

	// Convert from any sockaddr-like struct
	bool SetFromSockadr(const void *addr, size_t addr_size, uint16 *punPort = nullptr );
	template <typename T> bool SetFromSockadr(const T *s, uint16 *punPort = nullptr ) { return SetFromSockadr( s, sizeof(*s), punPort ); }	

	bool	HasIP() const;

	bool	IsLoopback() const;
	bool	IsReservedAdr() const;
	bool	IsBroadcast() const;
	bool	IsValid() const;	// m_unIPv4 != 0
	bool	SetFromSocket( int hSocket );

	static CIPAddress CreateIPv4Loopback() { CIPAddress ret; ret.SetIPV4Loopback(); return ret; }
	static CIPAddress CreateIPv6Loopback() { CIPAddress ret; ret.SetIPV6Loopback(); return ret; }

	bool operator==(const CIPAddress &netadr) const;
	bool operator!=(const CIPAddress &netadr) const;
	bool operator<(const CIPAddress &netadr) const;
	static unsigned int GetHashKey( const CIPAddress &netadr );
	unsigned int GetIPHash() const { return GetHashKey( *this ); }
	struct Hash
	{
		inline unsigned int operator()( const CIPAddress &x ) const
		{
			return CIPAddress::GetHashKey( x );
		}
	};

protected:
	union {

		//
		// IPv4
		//
		uint			m_unIPv4;			// IP stored in host order (little-endian)
		struct
		{
			#ifdef VALVE_BIG_ENDIAN
				uint8 b1, b2, b3, b4;
			#else
				uint8 b4, b3, b2, b1;
			#endif
		} m_IPv4Bytes;

		//
		// IPv6
		//
		byte			m_rgubIPv6[16];	// Network order! Same as inaddr_in6.  (0011:2233:4455:6677:8899:aabb:ccdd:eeff)
		uint64			m_ipv6Qword[2];	// In a few places we can use these to avoid memcmp. BIG ENDIAN!
	};

	uint32			m_unIPv6Scope;			// IPv6 scope
	ipadrtype_t		m_usType;				// ipadrtype_t
};

COMPILE_TIME_ASSERT( sizeof(CIPAddress) == 22 );

//-----------------------------------------------------------------------------
// CIPAndPort
//
// Encapsulates an IP+port combination.
// 
//-----------------------------------------------------------------------------
class CIPAndPort : public CIPAddress
{
public:
	// NOTE: For historical reasons, the default constructor sets address type
	// to *k_EIPTypeV4* (not k_EIPTypeInvalid!) but the IP and port are 0, so IsValid() will return false
	inline CIPAndPort() : CIPAddress(), m_usPort( 0 ) { }
	inline CIPAndPort( uint unIP, uint16 usPort ) : CIPAddress() { SetIPAndPort( unIP, usPort ); }
	explicit	CIPAndPort( uint unIP ) : CIPAddress( unIP ), m_usPort( 0 ) { }
	explicit	CIPAndPort( const char *pch ) : CIPAddress(), m_usPort( 0 ) { SetFromString( pch ); }
	explicit	CIPAndPort( const CIPAddress &that ) { *this = that; }
	explicit	CIPAndPort( const CIPAddress &that, uint16 usPort ) { *this = that; SetPort( usPort ); }

	CIPAndPort operator=( const CIPAddress &that )
	{
		*static_cast<CIPAddress*>(this) = that;
		return * this;
	}

	CIPAddress& GetIP() { return *this; }
	const CIPAddress& GetIP() const { return *this; }

	bool    SetFromString( const char *psz )
	{
		return CIPAddress::SetFromString( psz, &m_usPort );
	}

	/// Set port, without changing address type or IP
	void	SetPort( unsigned short port );

	/// Set IP and port at the same time.  
	void    SetIPAndPort( CIPAddress ipAddress, unsigned short usPort ) { *this = ipAddress; SetPort( usPort ); }
	
	/// Set IPv4 IP and port at the same time.  Also sets address type to k_EIPTypeV4
	void    SetIPAndPort( uint unIP, unsigned short usPort ) { SetIPv4( unIP ); SetPort( usPort ); }

	/// Set IPv6 address (as 16 bytes), port, and scope.
	/// This also sets the address type to k_EIPTypeV6.
	void SetIPV6AndPort( const byte *bytes, uint16 nPort, uint32 nScope = 0 ) { SetIPV6( bytes, nScope ); SetPort( nPort ); }

	/// Returns true if two addresses are equal.
	bool	CompareAdr(const CIPAndPort &a, bool onlyBase = false) const;

	/// Fetch port (host byte order)
	unsigned short GetPort() const { return m_usPort; }

	bool	HasPort() const;

	bool	IsValid() const;	// m_unIPv4 & port != 0

	/// Get string representation
	/// If onlyBase is true, then the port number is omitted.
	/// IPv4: xxx.xxx.xxx.xxx:ppppp
	/// IPv6: applies all of RFC5952 rules to get the canonical text representation.
	///       If !onlyBase, then the IP is surrounded by brackets to disambiguate colons
	///       in the address from the port separator: "[aabb::1234]:ppppp"
	void	ToString( char *pchBuffer, uint32 unBufferSize, bool onlyBase = false ) const;

	/// ToString, but with automatic buffer size deduction
	template< size_t maxLenInChars >
	char*	ToString_safe( char (&pDest)[maxLenInChars], bool onlyBase = false ) const
	{
		ToString( &pDest[0], maxLenInChars, onlyBase );
		return	pDest;
	}

	// Convert from any sockaddr-like struct
	bool SetFromSockadr(const void *addr, size_t addr_size )
	{
		return CIPAddress::SetFromSockadr( addr, addr_size, &m_usPort );
	}
	template <typename T> bool SetFromSockadr(const T *s) { return SetFromSockadr( s, sizeof(*s) ); }	

	// Convert to any sockaddr-like struct. Returns the size of the struct written.
	size_t ToSockadr(void *addr, size_t addr_size) const;
	template <typename T> size_t ToSockadr(T *s) const { return ToSockadr( s, sizeof(*s) ); }

	// Convert to sockaddr_in6.  If the address is IPv4 aa.bb.cc.dd, then
	// the mapped address ::ffff:aabb:ccdd is returned
	void ToSockadrIPV6(void *addr, size_t addr_size) const;
	template <typename T> void ToSockadrIPV6(T *s) const { ToSockadrIPV6( s, sizeof(*s) ); }

	bool	SetFromSocket( int hSocket );

	bool operator==(const CIPAndPort &netadr) const {return ( CompareAdr( netadr ) );}
	bool operator!=(const CIPAndPort &netadr) const {return !( CompareAdr( netadr ) );}
	bool operator<(const CIPAndPort &netadr) const;
	static unsigned int GetHashKey( const CIPAndPort &netadr );
	struct Hash
	{
		inline unsigned int operator()( const CIPAndPort &x ) const
		{
			return CIPAndPort::GetHashKey( x );
		}
	};

private:
	unsigned short	m_usPort = 0;				// port stored in host order (little-endian)
};

#pragma pack(pop)

COMPILE_TIME_ASSERT( sizeof(CIPAndPort) == 24 );
using netadr_t = CIPAndPort;


class CUtlNetAdrRender
{
public:
	CUtlNetAdrRender( const CIPAddress &obj )
	{
		obj.ToString_safe( m_rgchString );
	}

	CUtlNetAdrRender( const CIPAndPort &obj, bool bBaseOnly = false )
	{
		obj.ToString_safe( m_rgchString, bBaseOnly );
	}

	CUtlNetAdrRender( uint32 unIP )
	{
		CIPAndPort addr( unIP, 0 );
		addr.ToString_safe( m_rgchString, true );
	}

	CUtlNetAdrRender( uint32 unIP, uint16 nPort )
	{
		CIPAndPort addr( unIP, nPort );
		addr.ToString_safe( m_rgchString, false );
	}

	const char * String() const
	{ 
		return m_rgchString;
	}

private:

	char m_rgchString[k_ncchMaxNetAdrString];
};


inline void CIPAddress::SetIPv4(uint8 b1, uint8 b2, uint8 b3, uint8 b4)
{
	m_usType = k_EIPTypeV4;
	m_unIPv4 = ( b4 ) + ( b3 << 8 ) + ( b2 << 16 ) + ( b1 << 24 );
}

inline void CIPAddress::SetIPv4(uint unIP)
{
	m_usType = k_EIPTypeV4;
	m_unIPv4 = unIP;
}

inline void CIPAddress::SetType(ipadrtype_t newtype)
{
	m_usType = newtype;
}

inline uint CIPAddress::GetIPv4() const
{
	if ( m_usType != k_EIPTypeV6 )
		return m_unIPv4;

	AssertMsg( false, "CIPAndPort::GetIPv4 called on IPv6 address" );
	return 0;
}

inline const byte *CIPAddress::GetIPV6Bytes() const
{
	Assert( m_usType == k_EIPTypeV6 );
	return m_rgubIPv6;
}

inline void CIPAddress::SetIPV6( const byte *bytes, uint32 nScope )
{
	m_usType = k_EIPTypeV6;
	memcpy( m_rgubIPv6, bytes, 16 );
	m_unIPv6Scope = nScope;
}

inline uint32 CIPAddress::GetIPV6Scope() const
{
	Assert( m_usType == k_EIPTypeV6 );
	return m_unIPv6Scope;
}

inline void CIPAddress::SetIPV6Scope( uint32 nScope )
{
	Assert( m_usType == k_EIPTypeV6 );
	m_unIPv6Scope = nScope;
}

inline void CIPAndPort::SetPort(unsigned short newport)
{
	m_usPort = newport;
}

#endif // NETADR_H
