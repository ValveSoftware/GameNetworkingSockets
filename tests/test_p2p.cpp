#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <random>
#include <chrono>
#include <thread>


#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include "../examples/trivial_signaling_client.h"

HSteamListenSocket g_hListenSock;
HSteamNetConnection g_hConnection;
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

// Send a simple string message to out peer, using reliable transport.
void SendMessageToPeer( const char *pszMsg )
{
	TEST_Printf( "Sending msg '%s'\n", pszMsg );
	EResult r = SteamNetworkingSockets()->SendMessageToConnection(
		g_hConnection, pszMsg, (int)strlen(pszMsg)+1, k_nSteamNetworkingSend_Reliable, nullptr );
	assert( r == k_EResultOK );
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

		// Close our end
		SteamNetworkingSockets()->CloseConnection( pInfo->m_hConn, 0, nullptr, false );

		if ( g_hConnection == pInfo->m_hConn )
		{
			g_hConnection = k_HSteamNetConnection_Invalid;

			// In this example, we will bail the test whenever this happens.
			// Was this a normal termination?
			int rc = 0;
			if ( rc == k_ESteamNetworkingConnectionState_ProblemDetectedLocally || pInfo->m_info.m_eEndReason != k_ESteamNetConnectionEnd_App_Generic )
				rc = 1; // failure
			Quit( rc );
		}
		else
		{
			// Why are we hearing about any another connection?
			assert( false );
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
			// Somebody's knocking
			// Note that we assume we will only ever receive a single connection
			assert( g_hConnection == k_HSteamNetConnection_Invalid ); // not really a bug in this code, but a bug in the test

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
		// P2P connections will spend a bried time here where they swap addresses
		// and try to find a route.
		TEST_Printf( "[%s] finding route\n", pInfo->m_info.m_szConnectionDescription );
		break;

	case k_ESteamNetworkingConnectionState_Connected:
		// We got fully connected
		assert( pInfo->m_hConn == g_hConnection ); // We don't initiate or accept any other connections, so this should be out own connection
		TEST_Printf( "[%s] connected\n", pInfo->m_info.m_szConnectionDescription );
		break;

	default:
		assert( false );
		break;
	}
}

int main( int argc, const char **argv )
{
	SteamNetworkingIdentity identityLocal; identityLocal.Clear();
	SteamNetworkingIdentity identityRemote; identityRemote.Clear();
	const char *pszTrivialSignalingService = "localhost:10000";

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
		else if ( !strcmp( pszSwitch, "--client" ) )
			g_eTestRole = k_ETestRole_Client;
		else if ( !strcmp( pszSwitch, "--server" ) )
			g_eTestRole = k_ETestRole_Server;
		else if ( !strcmp( pszSwitch, "--symmetric" ) )
			g_eTestRole = k_ETestRole_Symmetric;
		else
			TEST_Fatal( "Unexpected command line argument '%s'", pszSwitch );
	}

	if ( g_eTestRole == k_ETestRole_Undefined )
		TEST_Fatal( "Must specify test role (--server, --client, or --symmetric" );
	if ( identityLocal.IsInvalid() )
		TEST_Fatal( "Must specify local identity using --identity-local" );
	if ( identityRemote.IsInvalid() && g_eTestRole != k_ETestRole_Server )
		TEST_Fatal( "Must specify remote identity using --identity-remote" );

	// Initialize library, with the desired local identity
	TEST_Init( &identityLocal );

	// Hardcode STUN servers
	SteamNetworkingUtils()->SetGlobalConfigValueString( k_ESteamNetworkingConfig_P2P_STUN_ServerList, "stun.l.google.com:19302" );

	// Allow sharing of any kind of ICE address.
	// We don't have any method of relaying (TURN) in this example, so we are essentially
	// forced to disclose our public address if we want to pierce NAT.  But if we
	// had relay fallback, or if we only wanted to connect on the LAN, we could restrict
	// to only sharing private addresses.
	SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All );

	// Create the signaling service
	SteamNetworkingErrMsg errMsg;
	ITrivialSignalingClient *pSignaling = CreateTrivialSignalingClient( pszTrivialSignalingService, SteamNetworkingSockets(), errMsg );
	if ( pSignaling == nullptr )
		TEST_Fatal( "Failed to initializing signaling client.  %s", errMsg );

	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged( OnSteamNetConnectionStatusChanged );

	// Comment this line in for more detailed spew about signals, route finding, ICE, etc
	//SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, k_ESteamNetworkingSocketsDebugOutputType_Verbose );

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

	// Begin connecting to peer, unless we are the server
	if ( g_eTestRole != k_ETestRole_Server )
	{
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
		SteamNetworkingErrMsg errMsg;
		ISteamNetworkingConnectionSignaling *pConnSignaling = pSignaling->CreateSignalingForConnection(
			identityRemote,
			errMsg
		);
		assert( pConnSignaling );
		g_hConnection = SteamNetworkingSockets()->ConnectP2PCustomSignaling( pConnSignaling, &identityRemote, g_nVirtualPortRemote, (int)vecOpts.size(), vecOpts.data() );
		assert( g_hConnection != k_HSteamNetConnection_Invalid );

		// Go ahead and send a message now.  The message will be queued until route finding
		// completes.
		SendMessageToPeer( "Greetings!" );
	}

	// Main test loop
	for (;;)
	{
		// Check for incoming signals, and dispatch them
		pSignaling->Poll();

		// Check callbacks
		TEST_PumpCallbacks();

		// If we have a connection, then poll it for messages
		if ( g_hConnection != k_HSteamNetConnection_Invalid )
		{
			SteamNetworkingMessage_t *pMessage;
			int r = SteamNetworkingSockets()->ReceiveMessagesOnConnection( g_hConnection, &pMessage, 1 );
			assert( r == 0 || r == 1 ); // <0 indicates an error
			if ( r == 1 )
			{
				// In this example code we will assume all messages are '\0'-terminated strings.
				// Obviously, this is not secure.
				TEST_Printf( "Received message '%s'\n", pMessage->GetData() );

				// Free message struct and buffer.
				pMessage->Release();

				// If we're the client, go ahead and shut down.  In this example we just
				// wanted to establish a connection and exchange a message, and we've done that.
				// Note that we use "linger" functionality.  This flushes out any remaining
				// messages that we have queued.  Essentially to us, the connection is closed,
				// but on thew wire, we will not actually close it until all reliable messages
				// have been confirmed as received by the client.  (Or the connection is closed
				// by the peer or drops.)  If we are the "client" role, then we know that no such
				// messages are in the pipeline in this test.  But in symmetric mode, it is
				// possible that we need to flush out our message that we sent.
				if ( g_eTestRole != k_ETestRole_Server )
				{
					TEST_Printf( "Closing connection and shutting down.\n" );
					SteamNetworkingSockets()->CloseConnection( g_hConnection, 0, "Test completed OK", true );
					Quit(0);
				}

				// We're the server.  Send a reply.
				SendMessageToPeer( "I got your message" );
			}
		}
	}

	return 0;
}
