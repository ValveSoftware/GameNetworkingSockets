//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Simple UDP-based LAN Discovery beacon and client.
//
//=============================================================================

#include "lan_discovery.h"
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
	#include <unistd.h>
	#include <sys/socket.h>
	#include <sys/types.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <poll.h>
	#include <errno.h>
	typedef int SOCKET;
	constexpr SOCKET INVALID_SOCKET = -1;
	inline void closesocket( SOCKET s ) { close( s ); }
	inline int GetSocketError() { return errno; }
#else
	#include <winsock2.h>
	#include <ws2tcpip.h>
	typedef int socklen_t;
	inline int GetSocketError() { return WSAGetLastError(); }
	#define poll WSAPoll

	struct ScopedWinsockInit_t
	{
		ScopedWinsockInit_t()
		{
			WSADATA wsaData;
			WSAStartup( MAKEWORD( 2, 2 ), &wsaData );
		}
		~ScopedWinsockInit_t()
		{
			WSACleanup();
		}
	};
#endif

// Helper: score IP address (prefer physical LAN over container/virtual bridge)
static int ScoreIPAddress( const char *pszIP )
{
	// 192.168.x.x / 10.x.x.x -> highest priority (real home/office Wi-Fi and LAN)
	if ( strncmp( pszIP, "192.168.", 8 ) == 0 || strncmp( pszIP, "10.", 3 ) == 0 )
		return 100;
	// 127.x.x.x -> local loopback
	if ( strncmp( pszIP, "127.", 4 ) == 0 )
		return 50;
	// 172.16-31.x.x (often Docker or container virtual bridge) -> lower priority
	if ( strncmp( pszIP, "172.", 4 ) == 0 )
		return 10;
	return 30;
}

CLanDiscoveryBeacon::CLanDiscoveryBeacon()
{
}

CLanDiscoveryBeacon::~CLanDiscoveryBeacon()
{
	Stop();
}

bool CLanDiscoveryBeacon::Start( uint16 nGnsPort, const char *pszServerName, uint16 nDiscoveryPort )
{
	Stop();

	std::string strName = pszServerName ? pszServerName : "GameNetworkingSocketsServer";

	m_bRunning = true;
	m_thread = std::thread( &CLanDiscoveryBeacon::BeaconThread, this, nGnsPort, strName, nDiscoveryPort );
	return true;
}

void CLanDiscoveryBeacon::Stop()
{
	if ( m_bRunning.exchange( false ) )
	{
		if ( m_thread.joinable() )
			m_thread.join();
	}
}

void CLanDiscoveryBeacon::BeaconThread( uint16 nGnsPort, std::string strServerName, uint16 nDiscoveryPort )
{
#ifdef _WIN32
	ScopedWinsockInit_t winsock;
#endif

	SOCKET fd = socket( AF_INET, SOCK_DGRAM, 0 );
	if ( fd == INVALID_SOCKET )
	{
		fprintf( stderr, "[LAN Discovery] Failed to create UDP socket: %d\n", GetSocketError() );
		return;
	}

	int opt = 1;
	setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt) );

	sockaddr_in bindAddr{};
	bindAddr.sin_family = AF_INET;
	bindAddr.sin_port = htons( nDiscoveryPort );
	bindAddr.sin_addr.s_addr = htonl( INADDR_ANY );

	if ( bind( fd, (sockaddr *)&bindAddr, sizeof(bindAddr) ) < 0 )
	{
		fprintf( stderr, "[LAN Discovery] Failed to bind beacon port %d: %d\n", nDiscoveryPort, GetSocketError() );
		closesocket( fd );
		return;
	}

	printf( "[LAN Discovery] Beacon listening on UDP port %d (advertising GNS port %d)\n", nDiscoveryPort, nGnsPort );
	fflush( stdout );

	while ( m_bRunning )
	{
		pollfd pfd{};
		pfd.fd = fd;
		pfd.events = POLLIN;

		int rc = poll( &pfd, 1, 100 );
		if ( rc <= 0 || !m_bRunning )
			continue;

		char szBuffer[256];
		sockaddr_in fromAddr{};
		socklen_t nFromLen = sizeof(fromAddr);

		int cbRecv = recvfrom( fd, szBuffer, sizeof(szBuffer) - 1, 0, (sockaddr *)&fromAddr, &nFromLen );
		if ( cbRecv <= 0 )
			continue;

		szBuffer[cbRecv] = '\0';

		char szFromIP[INET_ADDRSTRLEN];
		inet_ntop( AF_INET, &fromAddr.sin_addr, szFromIP, sizeof(szFromIP) );
		uint16 nFromPort = ntohs( fromAddr.sin_port );

		if ( strncmp( szBuffer, "GNS_DISCOVER", 12 ) == 0 )
		{
			printf( "[LAN Discovery] Query from %s:%d -> responding with port %d (%s)\n",
				szFromIP, nFromPort, nGnsPort, strServerName.c_str() );
			fflush( stdout );

			char szOffer[256];
			int cbOffer = snprintf( szOffer, sizeof(szOffer), "GNS_OFFER:%u:%s", nGnsPort, strServerName.c_str() );
			sendto( fd, szOffer, cbOffer, 0, (sockaddr *)&fromAddr, nFromLen );
		}
		else if ( strncmp( szBuffer, "GNS_PING", 8 ) == 0 )
		{
			printf( "[LAN Discovery] Mobile/external ping from %s:%d -> responding PONG\n", szFromIP, nFromPort );
			fflush( stdout );

			const char szPong[] = "GNS_PONG\n";
			sendto( fd, szPong, sizeof(szPong) - 1, 0, (sockaddr *)&fromAddr, nFromLen );
		}
	}

	// Close cleanly exactly once upon loop exit
	closesocket( fd );
}

bool CLanDiscoveryClient::Discover( std::vector<DiscoveredServer_t> &outServers, int nTimeoutMs, uint16 nDiscoveryPort )
{
	outServers.clear();

#ifdef _WIN32
	ScopedWinsockInit_t winsock;
#endif

	SOCKET fd = socket( AF_INET, SOCK_DGRAM, 0 );
	if ( fd == INVALID_SOCKET )
	{
		fprintf( stderr, "[LAN Discovery] Failed to create UDP client socket: %d\n", GetSocketError() );
		return false;
	}

	int bcast = 1;
	if ( setsockopt( fd, SOL_SOCKET, SO_BROADCAST, (const char *)&bcast, sizeof(bcast) ) < 0 )
	{
		fprintf( stderr, "[LAN Discovery] Failed to enable SO_BROADCAST: %d\n", GetSocketError() );
		closesocket( fd );
		return false;
	}

	sockaddr_in targetAddr{};
	targetAddr.sin_family = AF_INET;
	targetAddr.sin_port = htons( nDiscoveryPort );
	targetAddr.sin_addr.s_addr = htonl( INADDR_BROADCAST );

	const char szRequest[] = "GNS_DISCOVER";
	if ( sendto( fd, szRequest, sizeof(szRequest) - 1, 0, (sockaddr *)&targetAddr, sizeof(targetAddr) ) < 0 )
	{
		fprintf( stderr, "[LAN Discovery] Failed to send broadcast query: %d\n", GetSocketError() );
		closesocket( fd );
		return false;
	}

	int nRemainingMs = nTimeoutMs;
	int nLastBroadcastRemainingMs = nTimeoutMs;

	while ( nRemainingMs > 0 )
	{
		// Retransmit query every 500ms to tolerate wireless packet loss on consumer Wi-Fi
		if ( ( nLastBroadcastRemainingMs - nRemainingMs ) >= 500 )
		{
			sendto( fd, szRequest, sizeof(szRequest) - 1, 0, (sockaddr *)&targetAddr, sizeof(targetAddr) );
			nLastBroadcastRemainingMs = nRemainingMs;
		}

		pollfd pfd{};
		pfd.fd = fd;
		pfd.events = POLLIN;

		int nPollMs = ( nRemainingMs > 100 ) ? 100 : nRemainingMs;
		int rc = poll( &pfd, 1, nPollMs );
		nRemainingMs -= nPollMs;

		if ( rc <= 0 )
			continue;

		char szBuffer[256];
		sockaddr_in fromAddr{};
		socklen_t nFromLen = sizeof(fromAddr);

		int cbRecv = recvfrom( fd, szBuffer, sizeof(szBuffer) - 1, 0, (sockaddr *)&fromAddr, &nFromLen );
		if ( cbRecv <= 0 )
			continue;

		szBuffer[cbRecv] = '\0';

		if ( strncmp( szBuffer, "GNS_OFFER:", 10 ) == 0 )
		{
			uint32 nRemoteGnsPort = 0;
			char szName[128] = "Unknown";

			const char *pColon = strchr( szBuffer + 10, ':' );
			if ( pColon )
			{
				nRemoteGnsPort = (uint32)atoi( szBuffer + 10 );
				snprintf( szName, sizeof(szName), "%s", pColon + 1 );
			}
			else
			{
				nRemoteGnsPort = (uint32)atoi( szBuffer + 10 );
			}

			char szFromIP[INET_ADDRSTRLEN];
			inet_ntop( AF_INET, &fromAddr.sin_addr, szFromIP, sizeof(szFromIP) );

			DiscoveredServer_t srv;
			srv.m_addr.Clear();
			srv.m_addr.ParseString( szFromIP );
			srv.m_addr.m_port = (uint16)nRemoteGnsPort;
			srv.m_strServerName = szName;

			// Avoid duplicates if multiple network interfaces respond
			bool bAlreadyFound = false;
			for ( const auto &existing : outServers )
			{
				if ( existing.m_addr == srv.m_addr )
				{
					bAlreadyFound = true;
					break;
				}
			}

			if ( !bAlreadyFound )
			{
				if ( !outServers.empty() )
				{
					char szExistingIP[64];
					outServers[0].m_addr.ToString( szExistingIP, sizeof(szExistingIP), false );
					if ( ScoreIPAddress( szFromIP ) > ScoreIPAddress( szExistingIP ) )
						outServers.insert( outServers.begin(), srv );
					else
						outServers.push_back( srv );
				}
				else
				{
					outServers.push_back( srv );
				}
			}
		}
	}

	closesocket( fd );
	return !outServers.empty();
}
