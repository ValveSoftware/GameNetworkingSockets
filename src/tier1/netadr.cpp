//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// NetAdr.cpp: implementation of the CNetAdr class.
//
//=============================================================================//

#include <tier1/netadr.h>

#ifdef WIN32
	#include <winsock.h>
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
#endif

#undef SetPort

void netadr_t::ToString( char *pchBuffer, uint32 unBufferSize, bool baseOnly ) const
{
	strncpy (pchBuffer, "unknown", unBufferSize );

	if (type == NA_LOOPBACK)
	{
		strncpy (pchBuffer, "loopback", unBufferSize );
	}
	else if (type == NA_BROADCAST)
	{
		strncpy (pchBuffer, "broadcast", unBufferSize );
	}
	else if (type == NA_IP)
	{
		if ( baseOnly)
		{
#ifdef VALVE_BIG_ENDIAN
			_snprintf (pchBuffer, unBufferSize, "%i.%i.%i.%i", ipByte[0], ipByte[1], ipByte[2], ipByte[3] );
#else
			_snprintf (pchBuffer, unBufferSize, "%i.%i.%i.%i", ipByte[3], ipByte[2], ipByte[1], ipByte[0] );
#endif
		}
		else
		{
#ifdef VALVE_BIG_ENDIAN
			_snprintf (pchBuffer, unBufferSize, "%i.%i.%i.%i:%i", ipByte[0], ipByte[1], ipByte[2], ipByte[3], port );
#else
			_snprintf (pchBuffer, unBufferSize, "%i.%i.%i.%i:%i", ipByte[3], ipByte[2], ipByte[1], ipByte[0], port );
#endif
		}
	}
}


void netadr_t::ToSockadr (struct sockaddr * s) const
{
	memset ( s, 0, sizeof(struct sockaddr));
	
	// Note: we use htonl/s to convert IP & port from host (little-endian) to network (big-endian) order

	((struct sockaddr_in*)s)->sin_family = AF_INET;
	((struct sockaddr_in*)s)->sin_port = htons( port );

	if (type == NA_BROADCAST)
	{
		((struct sockaddr_in*)s)->sin_addr.s_addr = htonl( INADDR_BROADCAST );
	}
	else if (type == NA_IP)
	{
		((struct sockaddr_in*)s)->sin_addr.s_addr = htonl( ip );
	}
	else if (type == NA_LOOPBACK )
	{
		((struct sockaddr_in*)s)->sin_addr.s_addr =  htonl( INADDR_LOOPBACK );
	}
}

bool netadr_t::SetFromSockadr(const struct sockaddr * s)
{
	if (s->sa_family == AF_INET)
	{
		type = NA_IP;
		// Note: we use ntohl/s to convert IP & port from network (big-endian) to host (little-endian) order
		ip = ntohl ( ((struct sockaddr_in *)s)->sin_addr.s_addr );
		port = ntohs( ((struct sockaddr_in *)s)->sin_port );
		return true;
	}
	else
	{
		Clear();
		return false;
	}
}


bool netadr_t::SetFromString( const char *pch )
{
	ip = 0;
	port = 0;
	type = NA_IP;

	if ( !pch || pch[0] == 0 )			// but let's not crash
		return false;

	if ( pch[0] >= '0' && pch[0] <= '9' && strchr( pch, '.' ) )
	{
		int n1, n2, n3, n4, n5;
		int nRes = sscanf( pch, "%d.%d.%d.%d:%d", &n1, &n2, &n3, &n4, &n5 );
		if ( nRes >= 4 )
		{
			SetIP( n1, n2, n3, n4 );
		}

		if ( nRes == 5 )
		{
			SetPort( ( uint16 ) n5 );
		}

		if ( nRes >= 4 )
			return true;
	}

	return false;
}

/* SDR_PUBLIC 
bool netadr_t::BlockingResolveAndSetFromString( const char *psz )
{
	if ( !psz[0] )
		return false;

	if ( SetFromString( psz ) )
		return true;

	char szHostName[ 256 ];
	strncpy( szHostName, psz, sizeof(szHostName) );
	szHostName[ V_ARRAYSIZE(szHostName) - 1 ] = 0;

	char *pchColon = strchr( szHostName, ':' );
	if ( pchColon )
	{
		*pchColon = 0;
	}

	Clear();

	if ( ThreadInMainThread() )
	{
		Msg( "about to perform blocking dns call from the main thread.  consider refactoring.\n");
	}

	type = NA_IP;

	// Check our tiny hostname lookup cache.
	{
		time_t now = time( NULL );

		AUTO_LOCK( GetNetAdrResolveCacheMutex() );
		for ( auto &entry : s_netadrResolveCache )
		{
			if ( entry.ipv4 && entry.expire > now && stricmp( szHostName, entry.hostname ) == 0 )
			{
				SetIP( entry.ipv4 );
				break;
			}
		}
	}

	// If we didn't get a valid IP from the cache, call gethostbyname
	if ( GetIP() == 0 )
	{
		struct hostent *h = gethostbyname( szHostName );
		if ( !h || !h->h_addr_list || !h->h_addr_list[0] )
		{
			// Put a reference to s_netadrResolveCacheMutexEnforceConstruction in an unlikely code 
			// path so that the compiler can't optimize its global initializer away; we need it to
			// run before there is any chance of threaded access.
			return ( (intp)s_netadrResolveCacheMutexEnforceConstruction == 0x01 ); // aka "return false;"
		}

		SetIP( ntohl( *(int *)h->h_addr_list[0] ) );

		COMPILE_TIME_ASSERT( sizeof( s_netadrResolveCache[0].hostname ) == sizeof( szHostName ) );

		// Store in our tiny hostname lookup cache for five seconds.
		time_t expire = time( NULL ) + 5;

		AUTO_LOCK( GetNetAdrResolveCacheMutex() );
		uint32 counter = s_netadrResolveCacheCounter++;
		auto &entry = s_netadrResolveCache[ counter % V_ARRAYSIZE( s_netadrResolveCache ) ];
		strcpy( entry.hostname, szHostName );
		entry.expire = expire;
		entry.ipv4 = this->ip;
	}

	if ( pchColon ) 
	{
		SetPort( atoi( ++pchColon ) );
	}
	return true;

}

void netadr_t::SetFromSocket( int hSocket )
{	
	port = ip = 0;
	type = NA_IP;
		
	struct sockaddr address;
	int namelen = sizeof(address);
	if ( getsockname( hSocket, (struct sockaddr *)&address, (socklen_t *)&namelen) == 0 )
	{
		SetFromSockadr( &address );
	}
}
*/

//---------------------------------------------------------------------------------
//
// netmask_t
//
//---------------------------------------------------------------------------------


bool	netmask_t::SetFromString( const char *pchCIDR )
{
	if ( !pchCIDR )			// but let's not crash
		return false;

	uint uFirstByte = 0;
	uint uSecondByte = 0;
	uint uThirdByte = 0;
	uint uFourthByte = 0;
	uint uNumNetworkBits;
	uint uNumCharsConsumed = 0;

	char buf[32];
	// This is an MS extension (the regexp), it'll compile but not likely work under other platforms
	int NumMatches = sscanf(pchCIDR, "%16[^/]/%u%n", buf, &uNumNetworkBits, &uNumCharsConsumed);

	if ( NumMatches != 2 )
	{
		return false;
	}

	if ( uNumNetworkBits > 32 )
	{
		//AssertMsg( false, "Invalid CIDR mask size" );	
		return false;
	}

	// Guarantee null-termination.
	buf[ V_ARRAYSIZE(buf) - 1 ] = 0;

	if ( sscanf( buf, "%d.%d.%d.%d", &uFirstByte, &uSecondByte, &uThirdByte, &uFourthByte ) < (int)( ( uNumNetworkBits + 7 ) / 8 ) )
	{
		return false;
	}
	SetBaseIP( uFirstByte, uSecondByte, uThirdByte, uFourthByte );

	uint unMask = uNumNetworkBits ? ( ~0u << (32 - uNumNetworkBits) ) : 0;
	SetMask( unMask );

	return true;
}

bool	netmask_t::SetFromString( const char *pchBaseIP, const char *pchMask )
{
	if ( !pchBaseIP )			// but let's not crash
		return false;

	if ( !pchMask )			// but let's not crash
		return false;

	SetBaseIP( 0, 0, 0, 0 );
	SetMask( 255, 255, 255, 255 );
	int n1 = 0, n2 = 0, n3 = 0, n4 = 0;
	if ( sscanf( pchBaseIP, "%d.%d.%d.%d", &n1, &n2, &n3, &n4 ) < 4 )
		return false;

	SetBaseIP( n1, n2, n3, n4 );

	
	if ( sscanf( pchMask, "%d.%d.%d.%d", &n1, &n2, &n3, &n4 ) < 4 )
		return false;

	SetMask( n1, n2, n3, n4 );

	return true;
}


const char* netmask_t::ToCIDRString( char *pchBuffer, uint32 unBufferSize ) const // returns xxx.xxx.xxx.xxx/xx 
{
	uint uMask = ~m_Mask; // invert, so looks like 0000111
	uint uNumNetworkBits = 0;
	while ( uNumNetworkBits <= 32 )
	{
		if ( !uMask )
			break;

		uMask >>= 1;
		uNumNetworkBits++;
	}

	uNumNetworkBits = 32 - uNumNetworkBits;

	_snprintf (pchBuffer, unBufferSize, "%i.%i.%i.%i/%i", m_BaseIPByte[3], m_BaseIPByte[2], m_BaseIPByte[1], m_BaseIPByte[0], uNumNetworkBits );

	return pchBuffer;
}

