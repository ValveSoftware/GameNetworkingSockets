#pragma once

/////////////////////////////////////////////////////////////////////////////
//
// Interface for the mock network framework exposes to tests
//
/////////////////////////////////////////////////////////////////////////////

#include <vector>
#include <steam/steamnetworkingtypes.h>

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_MOCK

enum class TEST_mocknetwork_nat_type
{
	// Internal IP:port mapped to public IP:port.  Any peer can send to the public port.
	FullCone,

	// Inbound traffic to public port from IP X will only be forwarded if
	// an outbound packet was previously sent to X (but any port).
	RestrictedCone,

	// Inbound traffic to public port from IP X and port P will only be
	// forwarded if an outbound packet was previously sent to X *and* port P.
	PortRestrictedCone,

	// Every unique remote IP:port gets a separate external port.
	Symmetric,
};

struct TEST_mocknetwork_gateway_t
{
	SteamNetworkingIPAddr m_public_ip; // NAT public IP (127.0.100.x for IPv4, fd7f:0:100::x for IPv6); port must be zero

	TEST_mocknetwork_nat_type m_natType = TEST_mocknetwork_nat_type::FullCone;

	// Whether the gateway supports hairpinning: internally-originated packets addressed
	// to another host on the same gateway's public IP are looped back internally.
	// Many consumer routers do NOT support this.  (Not yet implemented; reserved for future use.)
	bool m_bHairpinSupported = true;

	// Latency from the local host to this gateway's exit point.
	// Models VPN-style scenarios where the exit node is geographically distant.
	int m_nInternalLatencyMS = 0;

	// Extra latency applied to all packets leaving this gateway to the public internet.
	// Models WAN/ISP link quality; applies to all destinations equally (including STUN servers).
	int m_nExternalLatencyMS = 0;
};

struct TEST_mocknetwork_interface_t
{
	SteamNetworkingIPAddr m_ip; // local IP to bind on; port must be zero

	// Index into TEST_mocknetwork_config_t::m_vecGateways, or -1 if this is a
	// public-facing interface (no NAT).  Public interfaces must use an IP in the
	// 127.0.100.x range.
	int m_iGateway = -1;

	// Artificial one-way latency applied to every outbound packet on this interface.
	// Models local link quality (e.g. WiFi jitter vs Ethernet).
	int m_nSendLatencyMS = 0;

	// If false, the interface is "down": no packets are sent or received.
	bool m_bEnabled = true;
};

struct TEST_mocknetwork_config_t
{
	// NAT gateways.  Interfaces with m_iGateway >= 0 route outbound traffic through
	// the corresponding entry here.
	std::vector<TEST_mocknetwork_gateway_t> m_vecGateways;

	// Network interfaces on this host.  Public interfaces (m_iGateway == -1) use
	// 127.0.100.x (IPv4) or fd7f:0:100::x (IPv6).  Private interfaces use 127.0.X.x / fd7f:0:X::x
	// (X != 100).  The same subnet can be shared across interfaces to model hosts on the same LAN.
	std::vector<TEST_mocknetwork_interface_t> m_vecInterfaces;
};

void TEST_mocknetwork_init( const TEST_mocknetwork_config_t &config );

extern bool TEST_mocknetwork_active;

#else

constexpr bool TEST_mocknetwork_active = false;

#endif // STEAMNETWORKINGSOCKETS_ENABLE_MOCK
