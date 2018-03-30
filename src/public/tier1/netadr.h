//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//
// netadr.h
#ifndef NETADR_H
#define NETADR_H
#ifdef _WIN32
#pragma once
#endif

#include <tier0/basetypes.h>

#undef SetPort

typedef enum
{ 
	NA_NULL = 0,
	NA_LOOPBACK,
	NA_BROADCAST,
	NA_IP,
} netadrtype_t;

#define NETADR_DEFINED

class netadr_t
{
public:
	inline netadr_t() { _padding = 0; SetIP( 0 ); SetPort( 0 ); SetType( NA_IP ); }
	inline netadr_t( uint unIP, uint16 usPort ) { _padding = 0; SetIP( unIP ); SetPort( usPort ); SetType( NA_IP ); }
	explicit	netadr_t( const char *pch ) { _padding = 0; SetFromString( pch ); }
	void	Clear();	// invalids Address

	void	SetType( netadrtype_t type );
	void	SetPort( unsigned short port );
	bool	SetFromSockadr(const struct sockaddr *s);
	void	SetIP(uint8 b1, uint8 b2, uint8 b3, uint8 b4);
	void	SetIP(uint unIP);									// Sets IP.  unIP is in host order (little-endian)
	void    SetIPAndPort( uint unIP, unsigned short usPort ) { SetIP( unIP ); SetPort( usPort ); }
	bool    SetFromString( const char *psz ); // returns false if you pass it a name that needs DNS resolution
	// SDR_PUBLIC bool	BlockingResolveAndSetFromString( const char *psz ); // DNS calls may block, inadvisable from the main thread
	bool	CompareAdr (const netadr_t &a, bool onlyBase = false) const;
	bool	CompareClassBAdr (const netadr_t &a) const;

	netadrtype_t	GetType() const;
	uint			GetIP() const;
	unsigned short	GetPort() const;
	void	ToString( char *pchBuffer, uint32 unBufferSize, bool onlyBase = false ) const; // returns xxx.xxx.xxx.xxx:ppppp
	template< size_t maxLenInChars >
	char*	ToString_safe( char (&pDest)[maxLenInChars], bool onlyBase = false ) const
	{
		ToString( &pDest[0], maxLenInChars, onlyBase );
		return	pDest;
	}
	void			ToSockadr(struct sockaddr *s) const;
		
	bool	IsLoopback() const;
	bool	IsReservedAdr() const;
	bool	IsValid() const;	// ip & port != 0
	void    SetFromSocket( int hSocket );

	bool operator==(const netadr_t &netadr) const {return ( CompareAdr( netadr ) );}
	bool operator!=(const netadr_t &netadr) const {return !( CompareAdr( netadr ) );}
	bool operator<(const netadr_t &netadr) const;
	static bool less( const netadr_t &lhs, const netadr_t &rhs );
	static unsigned int GetHashKey( const netadr_t &netadr );
private:
	unsigned short	port;				// port stored in host order (little-endian)
	unsigned short  _padding;
	union {
		uint			ip;				// IP stored in host order (little-endian)
		byte			ipByte[4];		// IP stored in host order (little-endian)
	};
	netadrtype_t	type;
};

// SDR_PUBLIC // We assert that netadr_t can be hashed as a memory block
// SDR_PUBLIC inline uint32 HashItem( const netadr_t &item )
// SDR_PUBLIC {
// SDR_PUBLIC 	return HashItemAsBytes(item);
// SDR_PUBLIC }


class netmask_t
{
public:
	netmask_t() { SetBaseIP( 0 ); SetMask( 0 ); }
	netmask_t( uint unBaseIP, uint unMask ) { SetBaseIP( unBaseIP ); SetMask( unMask ); }
	netmask_t( const char *pchCIDR ) { SetFromString( pchCIDR ); }
	netmask_t( const char *pchBaseIP, const char *pchMask ) { SetFromString( pchBaseIP, pchMask ); }
	void	Clear();

	void	SetBaseIP( uint8 b1, uint8 b2, uint8 b3, uint8 b4 );
	void	SetBaseIP( uint unIP );							// Sets base IP.  unIP is in host order (little-endian)
	void	SetMask( uint8 b1, uint8 b2, uint8 b3, uint8 b4 );
	void	SetMask( uint unMask );
	bool	SetFromString( const char *pchCIDR );
	bool	SetFromString( const char *pchBaseIP, const char *pchMask );

	bool	AdrInRange( uint unIP ) const;

	uint	GetBaseIP() const;
	uint	GetMask() const;
	uint	GetLastIP() const;
	const char* ToCIDRString( char *pchBuffer, uint32 unBufferSize ) const; // returns xxx.xxx.xxx.xxx/xx 

private:
	union {
		uint			m_BaseIP;				// IP stored in host order (little-endian)
		byte			m_BaseIPByte[4];		// IP stored in host order (little-endian)
	};

	union {
		uint			m_Mask;				// IP stored in host order (little-endian)
		byte			m_MaskByte[4];		// IP stored in host order (little-endian)
	};
};


class CUtlNetAdrRender
{
public:
	CUtlNetAdrRender( const netadr_t &obj, bool bBaseOnly = false )
	{
		obj.ToString( m_rgchString, sizeof(m_rgchString), bBaseOnly );
	}

	CUtlNetAdrRender( uint32 unIP )
	{
		netadr_t addr( unIP, 0 );
		addr.ToString( m_rgchString, sizeof(m_rgchString), true );
	}

	CUtlNetAdrRender( uint32 unIP, uint16 nPort )
	{
		netadr_t addr( unIP, nPort );
		addr.ToString( m_rgchString, sizeof(m_rgchString), false );
	}

	const char * String() 
	{ 
		return m_rgchString;
	}

private:

	char m_rgchString[64];
};


class CUtlNetMaskCIDRRender
{
public:
	CUtlNetMaskCIDRRender( const netmask_t &obj )
	{
		obj.ToCIDRString( m_rgchString, sizeof(m_rgchString) );
	}

	const char * String() 
	{ 
		return m_rgchString;
	}

private:

	char m_rgchString[64];
};


inline bool netadr_t::CompareAdr (const netadr_t &a, bool onlyBase) const
{
	if ( a.type != type )
		return false;

	if ( type == NA_LOOPBACK )
		return true;

	if ( type == NA_BROADCAST )
		return true;

	if ( type == NA_IP )
	{
		if ( !onlyBase && (port != a.port) )
			return false;

		if ( a.ip == ip )
			return true;
	}

	return false;
}

inline bool netadr_t::CompareClassBAdr (const netadr_t &a) const
{
	if ( a.type != type )
		return false;

	if ( type == NA_LOOPBACK )
		return true;

	if ( type == NA_IP )
	{
#ifdef VALVE_BIG_ENDIAN
		if (a.ipByte[0] == ipByte[0] && a.ipByte[1] == ipByte[1] )
#else
		if (a.ipByte[3] == ipByte[3] && a.ipByte[2] == ipByte[2] )
#endif
			return true;
	}

	return false;
}

// Is the IP part of one of the reserved blocks?
inline bool netadr_t::IsReservedAdr () const
{
	if ( type == NA_LOOPBACK )
		return true;

	// IP is stored little endian; for an IP of w.x.y.z, ipByte[3] will be w, ipByte[2] will be x, etc
	if ( type == NA_IP )
	{
#ifdef VALVE_BIG_ENDIAN
		if ( (ipByte[0] == 10) ||									// 10.x.x.x is reserved
			(ipByte[0] == 127) ||									// 127.x.x.x 
			(ipByte[0] == 172 && ipByte[1] >= 16 && ipByte[1] <= 31) ||	// 172.16.x.x  - 172.31.x.x 
			(ipByte[0] == 192 && ipByte[1] >= 168) ) 					// 192.168.x.x
			return true;
#else
		if ( (ipByte[3] == 10) ||									// 10.x.x.x is reserved
			(ipByte[3] == 127) ||									// 127.x.x.x 
			(ipByte[3] == 172 && ipByte[2] >= 16 && ipByte[2] <= 31) ||	// 172.16.x.x  - 172.31.x.x 
			(ipByte[3] == 192 && ipByte[2] >= 168) ) 					// 192.168.x.x
			return true;
#endif
	}
	return false;
}

inline bool netadr_t::IsLoopback() const
{
	return type == NA_LOOPBACK 
#ifdef VALVE_BIG_ENDIAN
		|| ( type == NA_IP && ipByte[0] == 127 );
#else
		|| ( type == NA_IP && ipByte[3] == 127 );
#endif
}

inline void netadr_t::Clear()
{
	ip = 0;
	port = 0;
	type = NA_NULL;
}

inline void netadr_t::SetIP(uint8 b1, uint8 b2, uint8 b3, uint8 b4)
{
	ip = ( b4 ) + ( b3 << 8 ) + ( b2 << 16 ) + ( b1 << 24 );
}

inline void netadr_t::SetIP(uint unIP)
{
	ip = unIP;
}

inline void netadr_t::SetType(netadrtype_t newtype)
{
	type = newtype;
}

inline netadrtype_t netadr_t::GetType() const
{
	return type;
}

inline unsigned short netadr_t::GetPort() const
{
	return port;
}

inline uint netadr_t::GetIP() const
{
	return ip;
}

inline bool netadr_t::IsValid() const
{
	return ( (port !=0 ) && (type != NA_NULL) &&
		( ip != 0 ) );
}

#ifdef _WIN32
#undef SetPort	// get around stupid WINSPOOL.H macro
#endif

inline void netadr_t::SetPort(unsigned short newport)
{
	port = newport;
}

inline bool netadr_t::operator<(const netadr_t &netadr) const
{
	return ( ip < netadr.ip ) || (ip == netadr.ip && port < netadr.port);
}

inline void	netmask_t::Clear()
{
	m_BaseIP = 0;
	m_Mask = 0;
}

inline void	netmask_t::SetBaseIP( uint8 b1, uint8 b2, uint8 b3, uint8 b4 )
{
	m_BaseIP = ( b4 ) + ( b3 << 8 ) + ( b2 << 16 ) + ( b1 << 24 );
}

inline void	netmask_t::SetBaseIP( uint unIP )
{
	m_BaseIP = unIP;
}

inline void	netmask_t::SetMask( uint8 b1, uint8 b2, uint8 b3, uint8 b4 )
{
	m_Mask = ( b4 ) + ( b3 << 8 ) + ( b2 << 16 ) + ( b1 << 24 );
}

inline void	netmask_t::SetMask( uint unMask )
{
	m_Mask = unMask;
}

inline bool	netmask_t::AdrInRange( uint unIP ) const
{
	return ( ( unIP	& m_Mask ) == m_BaseIP );
}

inline uint	netmask_t::GetBaseIP() const
{
	return m_BaseIP;
}

inline uint	netmask_t::GetMask() const
{
	return m_Mask;
}

inline uint	netmask_t::GetLastIP() const
{
	return m_BaseIP + ~m_Mask;
}

inline bool netadr_t::less( const netadr_t &lhs, const netadr_t &rhs )
{
	return ( lhs.ip < rhs.ip ) || (lhs.ip == rhs.ip && lhs.port < rhs.port);
}

inline unsigned int netadr_t::GetHashKey( const netadr_t &netadr )
{
	return ( netadr.ip ^ netadr.port );
}

#endif // NETADR_H
