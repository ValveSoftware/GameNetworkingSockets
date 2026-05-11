#pragma once

/////////////////////////////////////////////////////////////////////////////
//
// Interface for the mock network framework exposes to tests
//
/////////////////////////////////////////////////////////////////////////////

#include <vector>
#include <steam/steamnetworkingtypes.h>

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_MOCK

struct TEST_mocknetwork_interface_t
{
	SteamNetworkingIPAddr m_ip; // port not relevant and must be zero
	int m_nSendLatencyMS = 0;
};

enum class TEST_mocknetwork_nat_type
{

    // IPs are not translated at all, they are "public IPs"
    None,

    // Internal IP:port mapped to public IP:port.  Any peer can send to the
    // public port
    FullCone,

    // Inbound traffic to public port from IP X will only be forwarded if
    // an inbound traffic was previously sent to X(but any port)
    RestrictedCone,

    // Inbound traffic to public port from IP X and port P will only be
    // forwarded if an inbound traffic was previously sent to X *and* port P
    PortRestrictedCone,

    // Every internal IP:port <-> remote IP:port generates a new NAT port
    Symmetric,
};

struct TEST_mocknetwork_config_t
{
    std::vector<TEST_mocknetwork_interface_t> m_vecInterfaces;
	SteamNetworkingIPAddr m_ipv4_gateway; // Port not relevant and must be zero
    TEST_mocknetwork_nat_type m_natType = TEST_mocknetwork_nat_type::None;
};

void TEST_mocknetwork_init( const TEST_mocknetwork_config_t &info );

extern bool TEST_mocknetwork_active;

#else

constexpr bool TEST_mocknetwork_active = false;

#endif // STEAMNETWORKINGSOCKETS_ENABLE_MOCK
