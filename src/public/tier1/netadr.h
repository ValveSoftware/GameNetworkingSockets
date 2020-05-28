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

typedef enum
{ 
	// Reserved invalid/dummy address type
	NA_NULL = 0,

	// Do not use this.  In some primordial code, "loopback" often actually meant "localhost",
	// and sometimes it meant "internal buffers, not actually using the network system at all."
	// We don't need the latter in Steam, and we don't need a separate address for the former (just
	// use the appropriate reserved address!)
	NA_LOOPBACK_DEPRECATED,

	// Do not use this.  There are already reserved IP addresses to refer to this concept.
	// It is not a seperate address *type*.  In fact, there is an IPv4 broadcast addresses and an
	// IPv6 broadcast address, so it's not entirely clear exactly what this means.
	NA_BROADCAST_DEPRECATED,

	NA_IP, // IPv4
	NA_IPV6,
} netadrtype_t;

const netadrtype_t k_EIPTypeInvalid = NA_NULL;
const netadrtype_t k_EIPTypeV4 = NA_IP;
const netadrtype_t k_EIPTypeV6 = NA_IPV6;

#define NETADR_DEFINED
#pragma pack(push,1)

extern const byte k_ipv6Bytes_LinkLocalAllNodes[16];
extern const byte k_ipv6Bytes_Loopback[16];
extern const byte k_ipv6Bytes_Any[16];

class netadr_t
{
public:

	// NOTE: For historical reasons, the default constructor sets address type
	// to *NA_IP* (not NA_NULL!) but the IP and port are 0, so IsValid() will return false
	inline netadr_t() { memset((void *)this, 0, sizeof(*this) ); type = NA_IP; }
	inline netadr_t( uint unIP, uint16 usPort ) { memset((void *)this, 0, sizeof(*this) ); SetIPAndPort( unIP, usPort ); }
	explicit	netadr_t( uint unIP ) { memset((void *)this, 0, sizeof(*this) ); SetIPv4( unIP ); }
	explicit	netadr_t( const char *pch ) { memset((void *)this, 0, sizeof(*this) ); SetFromString( pch ); }

	/// Set to invalid address (NA_NULL)
	void	Clear() { memset((void *)this, 0, sizeof(*this)); }

	/// Get address type
	netadrtype_t GetType() const { return netadrtype_t( type ); }

	/// Set address type without changing any other fields
	void	SetType( netadrtype_t type );

	/// Set port, without changing address type or IP
	void	SetPort( unsigned short port );
	
	/// Set IPv4 IP given host-byte order argument.
	/// Also slams the address type to NA_IP.  Port does not change
	void	SetIPv4(uint unIP);

	/// Set IPv4 given individual address octets.
	/// Also slams the address type to NA_IP.  Port does not change
	void	SetIPv4(uint8 b1, uint8 b2, uint8 b3, uint8 b4);

	/// Set IPv4 IP and port at the same time.  Also sets address type to NA_IP
	void    SetIPAndPort( uint unIP, unsigned short usPort ) { SetIPv4( unIP ); SetPort( usPort ); }

	/// Attempt to parse address string.  Will never attempt
	/// DNS name resolution.  Returns false if we cannot parse an
	/// IPv4 address or IPv6 address.  If the port is not present,
	/// it is set to zero.
	bool    SetFromString( const char *psz );

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

	/// DNS resolution.  Blocking, so it may take a long time!  Inadvisable from the main thread!
	bool	BlockingResolveAndSetFromString( const char *psz, bool bUseIPv6 = false );

	/// Returns true if two addresses are equal.
	bool	CompareAdr(const netadr_t &a, bool onlyBase = false) const;

	/// Get the IPv4 IP, in host byte order.  Should only be called on IPv4 addresses.
	/// For historical reasons, this can be called on an NA_NULL address, and usually
	/// will return 0.
	uint	GetIPv4() const;

	/// Fetch port (host byte order)
	unsigned short GetPort() const { return port; }

	/// Get IPv6 bytes.  This will work on any address type
	/// An NA_NULL address returns all zeros.
	/// For the IPv4 address aa.bb.cc.dd, we will return ::ffff:aabb:ccdd
	void GetIPV6( byte *result ) const;

	/// Get pointer to IPV6 bytes.  This should only be called on IPv6
	/// address, will assert otherwise
	const byte *GetIPV6Bytes() const;

	/// Set IPv6 address (as 16 bytes) and scope.
	/// This slams the address type to NA_IPV6, but
	/// does not alter the port.
	void SetIPV6( const byte *bytes, uint32 nScope = 0 );

	/// Set IPv6 address (as 16 bytes), port, and scope.
	/// This also sets the address type to NA_IPV6.
	void SetIPV6AndPort( const byte *bytes, uint16 nPort, uint32 nScope = 0 ) { SetIPV6( bytes, nScope ); SetPort( nPort ); }

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
	bool SetFromSockadr(const void *addr, size_t addr_size);
	template <typename T> bool SetFromSockadr(const T *s) { return SetFromSockadr( s, sizeof(*s) ); }

	// Convert to any sockaddr-like struct. Returns the size of the struct written.
	size_t ToSockadr(void *addr, size_t addr_size) const;
	template <typename T> size_t ToSockadr(T *s) const { return ToSockadr( s, sizeof(*s) ); }

	// Convert to sockaddr_in6.  If the address is IPv4 aa.bb.cc.dd, then
	// the mapped address ::ffff:aabb:ccdd is returned
	void ToSockadrIPV6(void *addr, size_t addr_size) const;
	template <typename T> void ToSockadrIPV6(T *s) const { ToSockadrIPV6( s, sizeof(*s) ); }

	bool	HasIP() const;
	bool	HasPort() const;

	bool	IsLoopback() const;
	bool	IsReservedAdr() const;
	bool	IsBroadcast() const;
	bool	IsValid() const;	// ip & port != 0
	bool	SetFromSocket( int hSocket );

	bool operator==(const netadr_t &netadr) const {return ( CompareAdr( netadr ) );}
	bool operator!=(const netadr_t &netadr) const {return !( CompareAdr( netadr ) );}
	bool operator<(const netadr_t &netadr) const;
	static unsigned int GetHashKey( const netadr_t &netadr );
	struct Hash
	{
		inline unsigned int operator()( const netadr_t &x ) const
		{
			return netadr_t::GetHashKey( x );
		}
	};
private:
	unsigned short	type;				// netadrtype_t
	unsigned short	port;				// port stored in host order (little-endian)
	uint32			ipv6Scope;			// IPv6 scope
	union {

		//
		// IPv4
		//
		uint			ip;				// IP stored in host order (little-endian)
		byte			ipByte[4];		// IP stored in host order (little-endian)
		struct
		{
			#ifdef VALVE_BIG_ENDIAN
				uint8 b1, b2, b3, b4;
			#else
				uint8 b4, b3, b2, b1;
			#endif
		} ipv4;

		//
		// IPv6
		//
		byte			ipv6Byte[16];	// Same as inaddr_in6.  (0011:2233:4455:6677:8899:aabb:ccdd:eeff)
		uint64			ipv6Qword[2];	// In a few places we can use these to avoid memcmp. BIG ENDIAN!
	};
};

COMPILE_TIME_ASSERT( sizeof(netadr_t) == 24 );

#pragma pack(pop)


class CUtlNetAdrRender
{
public:
	CUtlNetAdrRender( const netadr_t &obj, bool bBaseOnly = false )
	{
		obj.ToString_safe( m_rgchString, bBaseOnly );
	}

	CUtlNetAdrRender( uint32 unIP )
	{
		netadr_t addr( unIP, 0 );
		addr.ToString_safe( m_rgchString, true );
	}

	CUtlNetAdrRender( uint32 unIP, uint16 nPort )
	{
		netadr_t addr( unIP, nPort );
		addr.ToString_safe( m_rgchString, false );
	}

	const char * String() 
	{ 
		return m_rgchString;
	}

private:

	char m_rgchString[k_ncchMaxNetAdrString];
};


inline void netadr_t::SetIPv4(uint8 b1, uint8 b2, uint8 b3, uint8 b4)
{
	type = NA_IP;
	ip = ( b4 ) + ( b3 << 8 ) + ( b2 << 16 ) + ( b1 << 24 );
}

inline void netadr_t::SetIPv4(uint unIP)
{
	type = NA_IP;
	ip = unIP;
}

inline void netadr_t::SetType(netadrtype_t newtype)
{
	type = newtype;
}

inline uint netadr_t::GetIPv4() const
{
	if ( type != NA_IPV6 )
		return ip;

	AssertMsg( false, "netadr_t::GetIP called on IPv6 address" );
	return 0;
}

inline const byte *netadr_t::GetIPV6Bytes() const
{
	Assert( type == NA_IPV6 );
	return ipv6Byte;
}

inline void netadr_t::SetIPV6( const byte *bytes, uint32 nScope )
{
	type = NA_IPV6;
	memcpy( ipv6Byte, bytes, 16 );
	ipv6Scope = nScope;
}

inline uint32 netadr_t::GetIPV6Scope() const
{
	Assert( type == NA_IPV6 );
	return ipv6Scope;
}

inline void netadr_t::SetIPV6Scope( uint32 nScope )
{
	Assert( type == NA_IPV6 );
	ipv6Scope = nScope;
}

inline void netadr_t::SetPort(unsigned short newport)
{
	port = newport;
}


#endif // NETADR_H
