//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Simple UDP-based LAN Discovery beacon and client.
// Enables GameNetworkingSockets applications to advertise and discover servers
// on the same local subnet without requiring central matchmaking or manual IP entry.
//
//=============================================================================

#pragma once

#include <steam/steamnetworkingtypes.h>
#include <steam/isteamnetworkingutils.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

const uint16 k_nDefaultDiscoveryPort = 27015;

/// Information about a server found on the local network.
struct DiscoveredServer_t
{
	SteamNetworkingIPAddr m_addr;
	std::string m_strServerName;
};

/// Advertises a local server to the LAN via UDP beacon.
class CLanDiscoveryBeacon
{
public:
	CLanDiscoveryBeacon();
	~CLanDiscoveryBeacon();

	/// Start advertising a GameNetworkingSockets listen port.
	/// nGnsPort: the port where ISteamNetworkingSockets is listening.
	/// pszServerName: optional human-readable name for the server.
	/// nDiscoveryPort: UDP port to listen for discovery broadcasts (default 27015).
	bool Start( uint16 nGnsPort, const char *pszServerName = nullptr, uint16 nDiscoveryPort = k_nDefaultDiscoveryPort );

	/// Stop the beacon and close the UDP socket.
	void Stop();

private:
	std::atomic<bool> m_bRunning{ false };
	std::thread m_thread;

	void BeaconThread( uint16 nGnsPort, std::string strServerName, uint16 nDiscoveryPort );
};

/// Client utility to discover servers on the local subnet.
class CLanDiscoveryClient
{
public:
	/// Broadcast a discovery request on the LAN and collect responses.
	/// outServers: list of discovered servers.
	/// nTimeoutMs: how long to wait for responses in milliseconds (default 1500ms).
	/// nDiscoveryPort: the discovery port to query (default 27015).
	/// Returns true if at least one server was found.
	static bool Discover( std::vector<DiscoveredServer_t> &outServers, int nTimeoutMs = 1500, uint16 nDiscoveryPort = k_nDefaultDiscoveryPort );
};
