#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <random>
#include <chrono>
#include <thread>
#include <vector>
#include <cmath>


#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include "../examples/trivial_signaling_client.h"
#include "../src/steamnetworkingsockets/clientlib/steamnetworkingsockets_mock.h"

#define DEFAULT_STUN_SERVER "stun.l.google.com:19302"

HSteamListenSocket g_hListenSock;
HSteamNetConnection g_hConnection;
int g_nRepeat = 1;
int g_nConnectionsDone = 0;
bool g_bExpectFailure = false;  // if true, connection failure is the expected outcome
int g_nConnectionTimeoutMS = -1; // -1 = library default
enum ETestRole
{
	k_ETestRole_Undefined,
	k_ETestRole_Server,
	k_ETestRole_Client,
	k_ETestRole_Symmetric,
};
ETestRole g_eTestRole = k_ETestRole_Undefined;

int g_nVirtualPortLocal = 0; // Used when listening, and when connecting
int g_nVirtualPortRemote = 0; // Only used when connecting
ESteamNetworkingSocketsDebugOutputType g_eTestP2PRendezvousLogLevel = k_ESteamNetworkingSocketsDebugOutputType_Verbose;

// Bail on sending if total pending bytes exceed this.
static const int k_nSendBufferLimit = 32*1024;

// Number of ticks to exchange messages before the non-server side closes.
int g_nTicks = 40; // 40 ticks * 50ms/tick = ~2 seconds

// Per-connection state, reset at the start of each connection.
bool g_bConnected = false;
int g_nTicksDone = 0;
int g_nSendCounterReliable = 0;
int g_nSendCounterUnreliable = 0;
int g_nRecvExpectedReliable = 0;
int g_nRecvExpectedUnreliable = 0;

static std::mt19937 g_rng( std::random_device{}() );

void PrintUsage()
{
	fprintf( stderr,
		"Usage: test_p2p [options]\n"
		"\n"
		"  --identity-local <identity>        Local identity string\n"
		"  --identity-remote <identity>        Remote identity string (not needed for --server)\n"
		"  --signaling-server <host:port>      Trivial signaling server (default: localhost:10000)\n"
		"  --server                            Act as server (listen for connection)\n"
		"  --client                            Act as client (connect to server)\n"
		"  --symmetric                         Symmetric connect mode\n"
		"  --log <file>                        Write log to file\n"
		"  --spewlevel <level>                 Console spew level: msg, verbose, debug\n"
		"  --loglevel-p2prendezvous <level>    P2P rendezvous log level: msg, verbose, debug\n"
		"  --stun-server <host:port>           STUN server address (default: " DEFAULT_STUN_SERVER ")\n"
		"  --turn-server <host:port>           TURN relay server address\n"
		"  --ice-implementation <n>            ICE implementation: 0=default, 1=native\n"
		"  --repeat <n>                        Repeat the connection test N times (default: 1)\n"
		"  --ticks <n>                         Number of 50ms send/receive ticks per connection (default: 40 = ~2s)\n"
		"  --expect-failure                    Treat connection failure as success, success as failure\n"
		"  --timeout-ms <n>                    Override initial connection timeout in milliseconds\n"
		"  --signaling-loss <pct>              Drop this %% of outbound signals (0-100, default: 0)\n"
		"  --signaling-dup <pct>               Duplicate this %% of outbound signals (0-100, default: 0)\n"
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_MOCK
		"\n"
		"Mock network options:\n"
		"  --mock-adapter <ip>                 Add a mock network adapter (repeatable).\n"
		"                                        Assigned to the most recently declared gateway,\n"
		"                                        or public (no NAT) if no gateway declared yet.\n"
		"  --mock-latency <ms>                 One-way send latency for the last --mock-adapter.\n"
		"  --mock-loss <pct>                   Outbound packet loss %% for subsequently added adapters.\n"
		"  --mock-disabled                     Mark the last --mock-adapter as down.\n"
		"  --mock-gateway <ip>                 Declare a NAT gateway with this public IP.\n"
		"                                        Subsequent --mock-adapters are assigned to it.\n"
		"  --mock-nat <type>                   NAT type for last gateway: full-cone (default),\n"
		"                                        restricted-cone, port-restricted-cone, symmetric\n"
		"  --mock-internal-latency <ms>        VPN-tunnel latency for last gateway (host->exit).\n"
		"  --mock-external-latency <ms>        WAN latency for last gateway (exit->internet).\n"
#endif
	);
}

static ESteamNetworkingSocketsDebugOutputType ParseLogLevelValue( const char *pszArg, const char *pszSwitchName )
{
	if ( !strcmp( pszArg, "msg" ) )
		return k_ESteamNetworkingSocketsDebugOutputType_Msg;
	if ( !strcmp( pszArg, "verbose" ) )
		return k_ESteamNetworkingSocketsDebugOutputType_Verbose;
	if ( !strcmp( pszArg, "debug" ) )
		return k_ESteamNetworkingSocketsDebugOutputType_Debug;

	TEST_Fatal( "Invalid %s '%s'. Expected one of: msg, verbose, debug", pszSwitchName, pszArg );
	return k_ESteamNetworkingSocketsDebugOutputType_Msg;
}

void Quit( int rc )
{
	if ( rc == 0 )
	{
		// OK, we cannot just exit the process, because we need to give
		// the connection time to actually send the last message and clean up.
		// If this were a TCP connection, we could just bail, because the OS
		// would handle it.  But this is an application protocol over UDP.
		// So give a little bit of time for good cleanup.  (Also note that
		// we really ought to continue pumping the signaling service, but
		// in this exampple we'll assume that no more signals need to be
		// exchanged, since we've gotten this far.)  If we just terminated
		// the program here, our peer could very likely timeout.  (Although
		// it's possible that the cleanup packets have already been placed
		// on the wire, and if they don't drop, things will get cleaned up
		// properly.)
		TEST_Printf( "Waiting for any last cleanup packets.\n" );
		std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) );
	}

	TEST_Kill();
	exit(rc);
}

// Print a parseable route summary for the active connection.
// Output format: "TEST ROUTE: addr=<ip:port> type=<local|udp|relay>"
void PrintRouteInfo()
{
	SteamNetConnectionInfo_t info;
	if ( !SteamNetworkingSockets()->GetConnectionInfo( g_hConnection, &info ) )
		return;
	const char *pszType;
	if ( info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed )
		pszType = "relay";
	else if ( info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Fast )
		pszType = "local";
	else
		pszType = "udp";
	char szAddr[64];
	info.m_addrRemote.ToString( szAddr, sizeof(szAddr), true );
	TEST_Printf( "TEST ROUTE: addr=%s type=%s\n", szAddr, pszType );
}

// Reset all per-connection counters.  Called at the start of each new connection.
void ResetConnectionCounters()
{
	g_bConnected = false;
	g_nTicksDone = 0;
	g_nSendCounterReliable = 0;
	g_nSendCounterUnreliable = 0;
	g_nRecvExpectedReliable = 0;
	g_nRecvExpectedUnreliable = 0;
}

// Each tick: if the send buffer is below the limit, roll 0-4 messages to send.
// Each message is randomly reliable or unreliable, with a counter in the first
// 4 bytes and an exponentially-distributed size (8-10k, biased toward small).
void SendRandomMessages()
{
	// Bail if send buffer is already full.
	SteamNetConnectionRealTimeStatus_t status;
	if ( SteamNetworkingSockets()->GetConnectionRealTimeStatus( g_hConnection, &status, 0, nullptr ) != k_EResultOK )
		return;
	if ( status.m_cbPendingReliable + status.m_cbPendingUnreliable >= k_nSendBufferLimit )
		return;

	int nCount = std::uniform_int_distribution<int>( 0, 4 )( g_rng );
	for ( int i = 0; i < nCount; ++i )
	{
		bool bReliable = std::uniform_int_distribution<int>( 0, 1 )( g_rng ) != 0;

		// Exponentially distributed size from 8 to 10240 bytes, most messages small.
		double u = std::uniform_real_distribution<double>( 0.0, 1.0 )( g_rng );
		int nSize = 8 + (int)std::exp( u * std::log( 10232.0 ) );
		nSize = std::min( nSize, 10240 );

		// First 4 bytes are the per-channel counter; rest is uninitialized payload.
		std::vector<uint8_t> buf( nSize );
		int32_t nCounter = bReliable ? g_nSendCounterReliable : g_nSendCounterUnreliable;
		memcpy( buf.data(), &nCounter, sizeof(nCounter) );

		int nFlags = bReliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
		EResult r = SteamNetworkingSockets()->SendMessageToConnection(
			g_hConnection, buf.data(), nSize, nFlags, nullptr );
		if ( r != k_EResultOK )
			break; // stop if the send fails (e.g. buffer filled mid-tick)

		if ( bReliable )
			++g_nSendCounterReliable;
		else
			++g_nSendCounterUnreliable;
	}
}

// Drain all pending received messages and verify their counters.
// Reliable counter mismatches are fatal; unreliable are expected and just logged.
void ReceiveAndCheckMessages()
{
	for (;;)
	{
		SteamNetworkingMessage_t *pMsg = nullptr;
		int r = SteamNetworkingSockets()->ReceiveMessagesOnConnection( g_hConnection, &pMsg, 1 );
		assert( r == 0 || r == 1 ); // <0 indicates an error
		if ( r == 0 )
			break;

		if ( pMsg->m_cbSize < (int)sizeof(int32_t) )
			TEST_Fatal( "Received message too short (%d bytes)", pMsg->m_cbSize );

		int32_t nCounter;
		memcpy( &nCounter, pMsg->GetData(), sizeof(nCounter) );
		// For received messages, only the k_nSteamNetworkingSend_Reliable bit is valid in m_nFlags.
		bool bReliable = ( pMsg->m_nFlags & k_nSteamNetworkingSend_Reliable ) != 0;
		pMsg->Release();

		if ( bReliable )
		{
			if ( nCounter != g_nRecvExpectedReliable )
				TEST_Fatal( "Reliable message counter mismatch: expected %d, got %d", g_nRecvExpectedReliable, nCounter );
			++g_nRecvExpectedReliable;
		}
		else
		{
			if ( nCounter != g_nRecvExpectedUnreliable )
				TEST_Printf( "Unreliable message %d arrived out of order (expected %d)\n", nCounter, g_nRecvExpectedUnreliable );
			g_nRecvExpectedUnreliable = nCounter + 1;
		}
	}
}

// Called when a connection undergoes a state transition.
void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *pInfo )
{
	// What's the state of the connection?
	switch ( pInfo->m_info.m_eState )
	{
	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:

		TEST_Printf( "[%s] %s, reason %d: %s\n",
			pInfo->m_info.m_szConnectionDescription,
			( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ? "closed by peer" : "problem detected locally" ),
			pInfo->m_info.m_eEndReason,
			pInfo->m_info.m_szEndDebug
		);

		if ( g_hConnection == pInfo->m_hConn )
		{
			// Print route info before closing the handle; GetConnectionInfo won't work after.
			// The non-server side prints this in the main loop; the server side does it here
			// since it never initiates the close itself.
			if ( !g_bExpectFailure && g_eTestRole == k_ETestRole_Server )
				PrintRouteInfo();

			// Close our end
			SteamNetworkingSockets()->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
			g_hConnection = k_HSteamNetConnection_Invalid;
			g_bConnected = false;

			if ( g_bExpectFailure )
			{
				// Connection failure is the expected outcome.
				// ProblemDetectedLocally (or ClosedByPeer with non-app reason) = expected.
				// ClosedByPeer with App_Generic = the peer completed successfully, which is wrong.
				bool bUnexpectedSuccess = ( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer )
				                       && ( pInfo->m_info.m_eEndReason == k_ESteamNetConnectionEnd_App_Generic );
				if ( bUnexpectedSuccess )
				{
					TEST_Printf( "ERROR: connection closed cleanly, but failure was expected\n" );
					Quit( 1 );
				}
				TEST_Printf( "Connection failed as expected\n" );
				SteamNetworkingSocketsLib::TEST_ICE_ctr_Print();
			}
			else
			{
				bool bError = ( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
				           || ( pInfo->m_info.m_eEndReason != k_ESteamNetConnectionEnd_App_Generic );
				if ( bError )
					Quit( 1 );

				if ( g_eTestRole == k_ETestRole_Server )
					SteamNetworkingSocketsLib::TEST_ICE_ctr_Print();
			}

			// Clean close (or expected failure) — main loop starts next iteration or exits.
			++g_nConnectionsDone;
		}
		else
		{
			// Stale handle from a previous iteration being cleaned up — ignore.
			SteamNetworkingSockets()->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
		}

		break;

	case k_ESteamNetworkingConnectionState_None:
		// Notification that a connection was destroyed.  (By us, presumably.)
		// We don't need this, so ignore it.
		break;

	case k_ESteamNetworkingConnectionState_Connecting:

		// Is this a connection we initiated, or one that we are receiving?
		if ( g_hListenSock != k_HSteamListenSocket_Invalid && pInfo->m_info.m_hListenSocket == g_hListenSock )
		{
			// Somebody's knocking.  With --repeat, the new connection request (signaled
			// via TCP) can race ahead of the close notification for the previous connection
			// (sent via UDP).  If the old handle is still around, clean it up now.
			if ( g_hConnection != k_HSteamNetConnection_Invalid )
			{
				TEST_Printf( "Got new connection request while previous connection was still active.  Closing previous connection\n" );
				SteamNetworkingSockets()->CloseConnection( g_hConnection, 0, nullptr, false );
				g_hConnection = k_HSteamNetConnection_Invalid;
				++g_nConnectionsDone;
			}

			SteamNetworkingSocketsLib::TEST_ICE_ctr_Reset();
			ResetConnectionCounters();
			TEST_Printf( "[%s] Accepting\n", pInfo->m_info.m_szConnectionDescription );
			g_hConnection = pInfo->m_hConn;
			SteamNetworkingSockets()->AcceptConnection( pInfo->m_hConn );
		}
		else
		{
			// Note that we will get notification when our own connection that
			// we initiate enters this state.
			assert( g_hConnection == pInfo->m_hConn );
			TEST_Printf( "[%s] Entered connecting state\n", pInfo->m_info.m_szConnectionDescription );
		}
		break;

	case k_ESteamNetworkingConnectionState_FindingRoute:
		// P2P connections will spend a brief time here where they swap addresses
		// and try to find a route.
		TEST_Printf( "[%s] finding route\n", pInfo->m_info.m_szConnectionDescription );
		break;

	case k_ESteamNetworkingConnectionState_Connected:
		// We got fully connected
		assert( pInfo->m_hConn == g_hConnection ); // We don't initiate or accept any other connections, so this should be out own connection
		TEST_Printf( "[%s] connected\n", pInfo->m_info.m_szConnectionDescription );
		g_bConnected = true;
		break;

	default:
		assert( false );
		break;
	}
}

#ifdef _MSC_VER
	#pragma warning( disable: 4702 ) /* unreachable code */
#endif

int main( int argc, const char **argv )
{
	SteamNetworkingIdentity identityLocal; identityLocal.Clear();
	SteamNetworkingIdentity identityRemote; identityRemote.Clear();
	const char *pszTrivialSignalingService = "localhost:10000";
	const char *pszSTUNServer = DEFAULT_STUN_SERVER;
	const char *pszTURNServer = nullptr;
	int g_nICEImplementation = -1; // -1 = not set, use library default
	int g_nICEEnable = k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All;
	int nSignalingLossPct = 0;
	int nSignalingDupPct = 0;
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_MOCK
	TEST_mocknetwork_config_t mockConfig;
	int nCurrentMockLoss = 0; // applied to subsequently added adapters
#endif

	// Parse the command line
	for ( int idxArg = 1 ; idxArg < argc ; ++idxArg )
	{
		const char *pszSwitch = argv[idxArg];

		auto GetArg = [&]() -> const char * {
			if ( idxArg + 1 >= argc )
				TEST_Fatal( "Expected argument after %s", pszSwitch );
			return argv[++idxArg];
		};
		auto ParseIdentity = [&]( SteamNetworkingIdentity &x ) {
			const char *pszArg = GetArg();
			if ( !x.ParseString( pszArg ) )
				TEST_Fatal( "'%s' is not a valid identity string", pszArg );
		};

		if ( !strcmp( pszSwitch, "--identity-local" ) )
			ParseIdentity( identityLocal );
		else if ( !strcmp( pszSwitch, "--identity-remote" ) )
			ParseIdentity( identityRemote );
		else if ( !strcmp( pszSwitch, "--signaling-server" ) )
			pszTrivialSignalingService = GetArg();
		else if ( !strcmp( pszSwitch, "--stun-server" ) )
			pszSTUNServer = GetArg();
		else if ( !strcmp( pszSwitch, "--turn-server" ) )
			pszTURNServer = GetArg();
		else if ( !strcmp( pszSwitch, "--ice-implementation" ) )
			g_nICEImplementation = atoi( GetArg() );
		else if ( !strcmp( pszSwitch, "--ice-enable" ) )
			g_nICEEnable = atoi( GetArg() );
		else if ( !strcmp( pszSwitch, "--repeat" ) )
			g_nRepeat = atoi( GetArg() );
		else if ( !strcmp( pszSwitch, "--ticks" ) )
			g_nTicks = atoi( GetArg() );
		else if ( !strcmp( pszSwitch, "--expect-failure" ) )
			g_bExpectFailure = true;
		else if ( !strcmp( pszSwitch, "--timeout-ms" ) )
			g_nConnectionTimeoutMS = atoi( GetArg() );
		else if ( !strcmp( pszSwitch, "--signaling-loss" ) )
			nSignalingLossPct = atoi( GetArg() );
		else if ( !strcmp( pszSwitch, "--signaling-dup" ) )
			nSignalingDupPct = atoi( GetArg() );
		else if ( !strcmp( pszSwitch, "--client" ) )
			g_eTestRole = k_ETestRole_Client;
		else if ( !strcmp( pszSwitch, "--server" ) )
			g_eTestRole = k_ETestRole_Server;
		else if ( !strcmp( pszSwitch, "--symmetric" ) )
			g_eTestRole = k_ETestRole_Symmetric;
		else if ( !strcmp( pszSwitch, "--log" ) )
		{
			const char *pszArg = GetArg();
			TEST_InitLog( pszArg );
		}
		else if ( !strcmp( pszSwitch, "--spewlevel" ) || !strncmp( pszSwitch, "--spewlevel=", 12 ) )
		{
			const char *pszArg = pszSwitch[11] == '=' ? pszSwitch + 12 : GetArg();
			ESteamNetworkingSocketsDebugOutputType eLogLevel = ParseLogLevelValue( pszArg, "--spewlevel" );
			TEST_SetStdoutDetailLevel( eLogLevel );
		}
		else if ( !strcmp( pszSwitch, "--loglevel-p2prendezvous" ) || !strncmp( pszSwitch, "--loglevel-p2prendezvous=", 25 ) )
		{
			const char *pszArg = pszSwitch[24] == '=' ? pszSwitch + 25 : GetArg();
			g_eTestP2PRendezvousLogLevel = ParseLogLevelValue( pszArg, "--loglevel-p2prendezvous" );
		}
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_MOCK
		else if ( !strcmp( pszSwitch, "--mock-gateway" ) )
		{
			const char *pszArg = GetArg();
			TEST_mocknetwork_gateway_t gw;
			if ( !gw.m_public_ip.ParseString( pszArg ) )
				TEST_Fatal( "'%s' is not a valid IP address for --mock-gateway", pszArg );
			gw.m_public_ip.m_port = 0;
			mockConfig.m_vecGateways.push_back( gw );
		}
		else if ( !strcmp( pszSwitch, "--mock-nat" ) )
		{
			if ( mockConfig.m_vecGateways.empty() )
				TEST_Fatal( "--mock-nat must follow --mock-gateway" );
			const char *pszArg = GetArg();
			TEST_mocknetwork_nat_type eNATType;
			if ( !strcmp( pszArg, "full-cone" ) )
				eNATType = TEST_mocknetwork_nat_type::FullCone;
			else if ( !strcmp( pszArg, "restricted-cone" ) )
				eNATType = TEST_mocknetwork_nat_type::RestrictedCone;
			else if ( !strcmp( pszArg, "port-restricted-cone" ) )
				eNATType = TEST_mocknetwork_nat_type::PortRestrictedCone;
			else if ( !strcmp( pszArg, "symmetric" ) )
				eNATType = TEST_mocknetwork_nat_type::Symmetric;
			else
				TEST_Fatal( "Invalid --mock-nat '%s'. Expected: full-cone, restricted-cone, port-restricted-cone, symmetric", pszArg );
			mockConfig.m_vecGateways.back().m_natType = eNATType;
		}
		else if ( !strcmp( pszSwitch, "--mock-internal-latency" ) )
		{
			if ( mockConfig.m_vecGateways.empty() )
				TEST_Fatal( "--mock-internal-latency must follow --mock-gateway" );
			mockConfig.m_vecGateways.back().m_nInternalLatencyMS = atoi( GetArg() );
		}
		else if ( !strcmp( pszSwitch, "--mock-external-latency" ) )
		{
			if ( mockConfig.m_vecGateways.empty() )
				TEST_Fatal( "--mock-external-latency must follow --mock-gateway" );
			mockConfig.m_vecGateways.back().m_nExternalLatencyMS = atoi( GetArg() );
		}
		else if ( !strcmp( pszSwitch, "--mock-adapter" ) )
		{
			const char *pszArg = GetArg();
			TEST_mocknetwork_interface_t iface;
			if ( !iface.m_ip.ParseString( pszArg ) )
				TEST_Fatal( "'%s' is not a valid IP address for --mock-adapter", pszArg );
			iface.m_ip.m_port = 0;
			iface.m_iGateway = mockConfig.m_vecGateways.empty() ? -1 : (int)mockConfig.m_vecGateways.size() - 1;
			if ( iface.m_iGateway >= 0 )
			{
				const SteamNetworkingIPAddr &gwIP = mockConfig.m_vecGateways[ iface.m_iGateway ].m_public_ip;
				if ( iface.m_ip.IsIPv4() != gwIP.IsIPv4() )
					TEST_Fatal( "--mock-adapter '%s' address family does not match its gateway '%s'",
						pszArg, SteamNetworkingIPAddrRender( gwIP, false ).c_str() );
			}
			iface.m_nSendLossPct = nCurrentMockLoss;
			mockConfig.m_vecInterfaces.push_back( iface );
		}
		else if ( !strcmp( pszSwitch, "--mock-latency" ) )
		{
			if ( mockConfig.m_vecInterfaces.empty() )
				TEST_Fatal( "--mock-latency must follow --mock-adapter" );
			mockConfig.m_vecInterfaces.back().m_nSendLatencyMS = atoi( GetArg() );
		}
		else if ( !strcmp( pszSwitch, "--mock-loss" ) )
		{
			nCurrentMockLoss = atoi( GetArg() );
		}
		else if ( !strcmp( pszSwitch, "--mock-disabled" ) )
		{
			if ( mockConfig.m_vecInterfaces.empty() )
				TEST_Fatal( "--mock-disabled must follow --mock-adapter" );
			mockConfig.m_vecInterfaces.back().m_bEnabled = false;
		}
#endif
		else if ( !strcmp( pszSwitch, "--help" ) || !strcmp( pszSwitch, "-h" ) )
		{
			PrintUsage();
			exit(0);
		}
		else
			TEST_Fatal( "Unexpected command line argument '%s'", pszSwitch );
	}

	if ( g_eTestRole == k_ETestRole_Undefined )
		TEST_Fatal( "Must specify test role (--server, --client, or --symmetric" );
	if ( identityLocal.IsInvalid() )
		TEST_Fatal( "Must specify local identity using --identity-local" );
	if ( identityRemote.IsInvalid() && g_eTestRole != k_ETestRole_Server )
		TEST_Fatal( "Must specify remote identity using --identity-remote" );

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_MOCK
	if ( !mockConfig.m_vecInterfaces.empty() )
		TEST_mocknetwork_init( mockConfig );
#endif

	// Initialize library, with the desired local identity
	TEST_Init( &identityLocal );

	if ( g_nConnectionTimeoutMS > 0 )
		SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_TimeoutInitial, g_nConnectionTimeoutMS );

	SteamNetworkingUtils()->SetGlobalConfigValueString( k_ESteamNetworkingConfig_P2P_STUN_ServerList, pszSTUNServer );
	if ( pszTURNServer != nullptr )
		SteamNetworkingUtils()->SetGlobalConfigValueString( k_ESteamNetworkingConfig_P2P_TURN_ServerList, pszTURNServer );
	if ( g_nICEImplementation >= 0 )
		SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_P2P_Transport_ICE_Implementation, g_nICEImplementation );

	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, g_nICEEnable );

	// Create the signaling service
	SteamNetworkingErrMsg errMsg;
	ITrivialSignalingClient *pSignaling = CreateTrivialSignalingClient( pszTrivialSignalingService, SteamNetworkingSockets(), errMsg, nSignalingLossPct, nSignalingDupPct );
	if ( pSignaling == nullptr )
		TEST_Fatal( "Failed to initializing signaling client.  %s", errMsg );

	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged( OnSteamNetConnectionStatusChanged );

	// Comment this line in for more detailed spew about signals, route finding, ICE, etc
	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, g_eTestP2PRendezvousLogLevel );

	// Create listen socket to receive connections on, unless we are the client
	if ( g_eTestRole == k_ETestRole_Server )
	{
		TEST_Printf( "Creating listen socket, local virtual port %d\n", g_nVirtualPortLocal );
		g_hListenSock = SteamNetworkingSockets()->CreateListenSocketP2P( g_nVirtualPortLocal, 0, nullptr );
		assert( g_hListenSock != k_HSteamListenSocket_Invalid  );
	}
	else if ( g_eTestRole == k_ETestRole_Symmetric )
	{

		// Currently you must create a listen socket to use symmetric mode,
		// even if you know that you will always create connections "both ways".
		// In the future we might try to remove this requirement.  It is a bit
		// less efficient, since it always triggered the race condition case
		// where both sides create their own connections, and then one side
		// decides to their theirs away.  If we have a listen socket, then
		// it can be the case that one peer will receive the incoming connection
		// from the other peer, and since he has a listen socket, can save
		// the connection, and then implicitly accept it when he initiates his
		// own connection.  Without the listen socket, if an incoming connection
		// request arrives before we have started connecting out, then we are forced
		// to ignore it, as the app has given no indication that it desires to
		// receive inbound connections at all.
		TEST_Printf( "Creating listen socket in symmetric mode, local virtual port %d\n", g_nVirtualPortLocal );
		SteamNetworkingConfigValue_t opt;
		opt.SetInt32( k_ESteamNetworkingConfig_SymmetricConnect, 1 ); // << Note we set symmetric mode on the listen socket
		g_hListenSock = SteamNetworkingSockets()->CreateListenSocketP2P( g_nVirtualPortLocal, 1, &opt );
		assert( g_hListenSock != k_HSteamListenSocket_Invalid  );
	}

	// Lambda to initiate a new outbound connection.
	auto ConnectToPeer = [&]()
	{
		SteamNetworkingSocketsLib::TEST_ICE_ctr_Reset();
		ResetConnectionCounters();
		std::vector< SteamNetworkingConfigValue_t > vecOpts;

		// If we want the local and virtual port to differ, we must set
		// an option.  This is a pretty rare use case, and usually not needed.
		// The local virtual port is only usually relevant for symmetric
		// connections, and then, it almost always matches.  Here we are
		// just showing in this example code how you could handle this if you
		// needed them to differ.
		if ( g_nVirtualPortRemote != g_nVirtualPortLocal )
		{
			SteamNetworkingConfigValue_t opt;
			opt.SetInt32( k_ESteamNetworkingConfig_LocalVirtualPort, g_nVirtualPortLocal );
			vecOpts.push_back( opt );
		}

		// Symmetric mode?  Noce that since we created a listen socket on this local
		// virtual port and tagged it for symmetric connect mode, any connections
		// we create that use the same local virtual port will automatically inherit
		// this setting.  However, this is really not recommended.  It is best to be
		// explicit.
		if ( g_eTestRole == k_ETestRole_Symmetric )
		{
			SteamNetworkingConfigValue_t opt;
			opt.SetInt32( k_ESteamNetworkingConfig_SymmetricConnect, 1 );
			vecOpts.push_back( opt );
			TEST_Printf( "Connecting to '%s' in symmetric mode, virtual port %d, from local virtual port %d.\n",
				SteamNetworkingIdentityRender( identityRemote ).c_str(), g_nVirtualPortRemote,
				g_nVirtualPortLocal );
		}
		else
		{
			TEST_Printf( "Connecting to '%s', virtual port %d, from local virtual port %d.\n",
				SteamNetworkingIdentityRender( identityRemote ).c_str(), g_nVirtualPortRemote,
				g_nVirtualPortLocal );
		}

		// Connect using the "custom signaling" path.  Note that when
		// you are using this path, the identity is actually optional,
		// since we don't need it.  (Your signaling object already
		// knows how to talk to the peer) and then the peer identity
		// will be confirmed via rendezvous.
		ISteamNetworkingConnectionSignaling *pConnSignaling = pSignaling->CreateSignalingForConnection(
			identityRemote,
			errMsg
		);
		assert( pConnSignaling );
		g_hConnection = SteamNetworkingSockets()->ConnectP2PCustomSignaling( pConnSignaling, &identityRemote, g_nVirtualPortRemote, (int)vecOpts.size(), vecOpts.data() );
		assert( g_hConnection != k_HSteamNetConnection_Invalid );
	};

	// Begin connecting to peer, unless we are the server
	if ( g_eTestRole != k_ETestRole_Server )
		ConnectToPeer();

	// Main test loop
	for (;;)
	{
		auto tickStart = std::chrono::steady_clock::now();

		// Check for incoming signals, and dispatch them
		pSignaling->Poll();

		// Check callbacks
		TEST_PumpCallbacks();

		// If we have a fully connected connection, exchange messages this tick.
		if ( g_bConnected && g_hConnection != k_HSteamNetConnection_Invalid )
		{
			if ( g_bExpectFailure )
			{
				TEST_Printf( "ERROR: received a message but connection failure was expected\n" );
				Quit( 1 );
			}

			ReceiveAndCheckMessages();
			SendRandomMessages();

			++g_nTicksDone;

			// Non-server side drives the close after the target number of ticks.
			if ( g_eTestRole != k_ETestRole_Server && g_nTicksDone >= g_nTicks )
			{
				PrintRouteInfo();
				SteamNetworkingSocketsLib::TEST_ICE_ctr_Print();
				TEST_Printf( "Closing connection after %d ticks (%d reliable, %d unreliable sent)\n",
					g_nTicksDone, g_nSendCounterReliable, g_nSendCounterUnreliable );

				// Close with linger so the server has time to receive any messages
				// we queued before the close notice arrives.
				SteamNetworkingSockets()->CloseConnection( g_hConnection, k_ESteamNetConnectionEnd_App_Generic, "Test completed OK", true );
				g_hConnection = k_HSteamNetConnection_Invalid;
				g_bConnected = false;
				++g_nConnectionsDone;
				if ( g_nConnectionsDone >= g_nRepeat )
					break;
				TEST_Printf( "Starting next iteration\n" );
				ConnectToPeer();
			}
		}

		// Exit once all expected connections are done.
		// For the server, this means N accepted+closed connections.
		// For the client, normal exits happen inside the message handler above, but
		// when --expect-failure is set the failure is detected in the state callback,
		// so we need this check here too.
		if ( g_nConnectionsDone >= g_nRepeat && g_hConnection == k_HSteamNetConnection_Invalid )
			break;

		// Sleep the remainder of the 50ms tick budget in <=5ms chunks.
		// Small chunks keep actual tick time close to 50ms even on slow/loaded
		// runners where a single sleep_for(50ms) can overshoot by 30-40ms.
		auto tickEnd = tickStart + std::chrono::milliseconds( 50 );
		for (;;)
		{
			auto remaining = tickEnd - std::chrono::steady_clock::now();
			if ( remaining <= std::chrono::milliseconds( 0 ) )
				break;
			if ( remaining > std::chrono::milliseconds( 5 ) )
				remaining = std::chrono::milliseconds( 5 );
			std::this_thread::sleep_for( remaining );
		}
	}

	TEST_Printf( "Shutting down\n" );
	Quit(0);
	return 0;
}
