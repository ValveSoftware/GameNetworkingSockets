#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <random>
#include <chrono>
#include <thread>

#include <steam/steam_gameserver.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include "../examples/trivial_signaling_client.h"

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
int g_nMessagesSent;
int g_nMessagesRecv;

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

HSteamListenSocket g_hListenSock;
HSteamNetPollGroup g_hPollGroup;

struct Connection
{
	Connection( HSteamNetConnection hConnection ) : m_hConnection( hConnection ) {}
	const HSteamNetConnection m_hConnection;
	ESteamNetworkingConnectionState m_eState = k_ESteamNetworkingConnectionState_Connecting;
	SteamNetworkingMicroseconds m_usecNextMessageSendTime = 0;
	int m_nMessagesSent = 0;

	void CloseConnection( int nReason, const char *pszDebug, bool bEnableLinger );

	// Send a simple string message to out peer, using reliable transport.
	void SendMessageToPeer( const char *pszMsg )
	{
		//TEST_Printf( "Sending msg '%s'\n", pszMsg );
		EResult r = SteamGameServerNetworkingSockets()->SendMessageToConnection(
			m_hConnection, pszMsg, (int)strlen(pszMsg)+1, k_nSteamNetworkingSend_Reliable, nullptr );
		if ( r != k_EResultOK )
		{
			TEST_Printf( "WARNING: SendMessageToConnection returned %d\n", r );
		}
	}

	void Think( SteamNetworkingMicroseconds usecNow )
	{
		if ( m_eState != k_ESteamNetworkingConnectionState_Connected || usecNow < m_usecNextMessageSendTime )
			return;
		if ( m_nMessagesSent > 1000 )
		{
			if ( g_eTestRole == k_ETestRole_Client )
				CloseConnection( k_ESteamNetConnectionEnd_App_Generic, "Normal shutdown", true ); // self destruct!
			return;
		}

		++m_nMessagesSent;
		++g_nMessagesSent;
		char msg[256];
		sprintf( msg, "Test message %d", m_nMessagesSent );

		SendMessageToPeer( msg );

		m_usecNextMessageSendTime = usecNow + SteamNetworkingMicroseconds( (float)rand() / (float)RAND_MAX * 75e3 );
	}

};

std::vector<Connection *> s_vecConnections;

int FindConnection( HSteamNetConnection hConnection )
{
	for ( int i = 0 ; i < (int)s_vecConnections.size() ; ++i )
	{
		if ( s_vecConnections[i]->m_hConnection == hConnection )
			return i;
	}

	return -1;
}

void Connection::CloseConnection( int nReason, const char *pszDebug, bool bEnableLinger )
{
	assert( m_hConnection != k_HSteamNetConnection_Invalid );

	SteamGameServerNetworkingSockets()->CloseConnection( m_hConnection, nReason, pszDebug, bEnableLinger );

	int idx = FindConnection( m_hConnection );
	assert( idx >= 0 );
	assert( s_vecConnections[idx] == this );
	s_vecConnections.erase( s_vecConnections.begin() + idx );

	delete this;
}

Connection *AddConnection( HSteamNetConnection hConn )
{
	assert( hConn != k_HSteamNetConnection_Invalid );

	SteamGameServerNetworkingSockets()->SetConnectionPollGroup( hConn, g_hPollGroup );

	Connection *c = new Connection( hConn );
	s_vecConnections.push_back( c );
	return c;
}

// Called when a connection undergoes a state transition.
void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *pInfo )
{
	int idxExistingConn = FindConnection( pInfo->m_hConn );

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
		//assert( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer );
		//assert( pInfo->m_info.m_eEndReason == k_ESteamNetConnectionEnd_App_Generic );
		if ( idxExistingConn >= 0 )
		{
			s_vecConnections[ idxExistingConn ]->CloseConnection( 0, nullptr, false );
			return;
		}

		// Why are we hearing about any another connection?
		assert( false );

		break;

	case k_ESteamNetworkingConnectionState_None:
		// Notification that a connection was destroyed.  (By us, presumably.)
		// We don't need this, so ignore it.
		assert( idxExistingConn < 0 );
		break;

	case k_ESteamNetworkingConnectionState_Connecting:

		// Is this a connection we initiated, or one that we are receiving?
		if ( g_hListenSock != k_HSteamListenSocket_Invalid && pInfo->m_info.m_hListenSocket == g_hListenSock )
		{
			// New connection
			assert( idxExistingConn < 0 ); // not really a bug in this code, but a bug in the test

			SteamGameServerNetworkingSockets()->AcceptConnection( pInfo->m_hConn );

			AddConnection( pInfo->m_hConn );

			TEST_Printf( "[%s] Accepting.  (%d connections)\n", pInfo->m_info.m_szConnectionDescription, (int)s_vecConnections.size() );
		}
		else
		{
			// Note that we will get notification when our own connection that
			// we initiate enters this state.
			assert( idxExistingConn >= 0 );
			//TEST_Printf( "[%s] Entered connecting state\n", pInfo->m_info.m_szConnectionDescription );
		}
		break;

	case k_ESteamNetworkingConnectionState_FindingRoute:
		// P2P connections will spend a brief time here where they swap addresses
		// and try to find a route.
		//TEST_Printf( "[%s] finding route\n", pInfo->m_info.m_szConnectionDescription );
		s_vecConnections[ idxExistingConn ]->m_eState = pInfo->m_info.m_eState;
		break;

	case k_ESteamNetworkingConnectionState_Connected:
		// We got fully connected
		assert( idxExistingConn >= 0 );
		TEST_Printf( "[%s] connected\n", pInfo->m_info.m_szConnectionDescription );
		s_vecConnections[ idxExistingConn ]->m_eState = pInfo->m_info.m_eState;
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
	//const char *pszTrivialSignalingService = "localhost:10000";

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
		//else if ( !strcmp( pszSwitch, "--signaling-server" ) )
		//	pszTrivialSignalingService = GetArg();
		else if ( !strcmp( pszSwitch, "--client" ) )
			g_eTestRole = k_ETestRole_Client;
		else if ( !strcmp( pszSwitch, "--server" ) )
			g_eTestRole = k_ETestRole_Server;
		//else if ( !strcmp( pszSwitch, "--symmetric" ) )
		//	g_eTestRole = k_ETestRole_Symmetric;
		else
			TEST_Fatal( "Unexpected command line argument '%s'", pszSwitch );
	}

	if ( g_eTestRole == k_ETestRole_Undefined )
		TEST_Fatal( "Must specify test role (--server, --client, or --symmetric" );
	if ( identityRemote.IsInvalid() && g_eTestRole != k_ETestRole_Server )
	{

		if ( g_eTestRole == k_ETestRole_Client )
		{
			char buf[256];
			memset(buf, 0, sizeof(buf));
			FILE *f = fopen( "server_identity.txt", "rt" );
			if ( f )
			{
				fread( buf, sizeof(buf)-1, 1, f );
				fclose(f);
			}
			if ( !identityRemote.ParseString( buf ) )
				TEST_Fatal( "Failed to parse identity from server_identity.txt" );
			TEST_Printf( "Loaded remote identity %s from server_identity.txt\n", SteamNetworkingIdentityRender( identityRemote ).c_str() );
		}
		else
		{
			TEST_Fatal( "Must specify remote identity using --identity-remote" );
		}
	}

	TEST_InitLog( g_eTestRole == k_ETestRole_Client ? "client.txt" : "server.txt" );

	// Logon to steam

	{
		FILE *f = fopen( "steam_appid.txt", "wt" );
		fprintf( f, "570\n" );
		fclose(f);
	}
	uint16 nGamePort = 27015 + (uint16)g_eTestRole;
	TEST_Printf( "Logging onto steam as anonymous gameserver, using gameport %u\n", nGamePort );
	if ( !SteamGameServer_Init( 0, nGamePort, STEAMGAMESERVER_QUERY_PORT_SHARED, eServerModeNoAuthentication, "1.0.0" ) )
		TEST_Fatal( "SteamGameServer_Init failed\n" );
	SteamGameServer()->LogOnAnonymous();

	while ( !SteamGameServer()->BLoggedOn() )
	{
		TEST_PumpCallbacks();
		SteamGameServer_RunCallbacks();
	}
	assert( SteamGameServer()->GetSteamID().BAnonGameServerAccount() );
	identityLocal.SetSteamID( SteamGameServer()->GetSteamID() );
	TEST_Printf( "Logged onto Steam universe %d, assigned anonymous gameserver identity %s\n", (int)SteamGameServerUtils()->GetConnectedUniverse(), SteamNetworkingIdentityRender( identityLocal ).c_str() );
	if ( g_eTestRole == k_ETestRole_Server )
	{
		TEST_Printf( "Saving server_identity.txt\n" );
		FILE *f = fopen( "server_identity.txt", "wt" );
		fprintf( f, "%s", SteamNetworkingIdentityRender( identityLocal ).c_str() );
		fclose(f);
	}

	// !KLUDGE! We have to set the realm
	SteamDatagram_SetUniverse( false, SteamGameServerUtils()->GetConnectedUniverse() );

	// Initialize library
	SteamNetworkingErrMsg errMsg;
	if ( !SteamDatagramServer_InitSteam( errMsg ) )
	{
		TEST_Fatal( "SteamDatagramServer_Init failed.  %s", errMsg );
	}

	// Hardcode STUN servers
	//SteamNetworkingUtils()->SetGlobalConfigValueString( k_ESteamNetworkingConfig_P2P_STUN_ServerList, "stun.l.google.com:19302" );
	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Public ); // !TEST! Force us to STUN
	//SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable ); // !TEST! Force relay

	// Allow sharing of any kind of ICE address.
	// We don't have any method of relaying (TURN) in this example, so we are essentially
	// forced to disclose our public address if we want to pierce NAT.  But if we
	// had relay fallback, or if we only wanted to connect on the LAN, we could restrict
	// to only sharing private addresses.
	//SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All );

	//// Create the signaling service
	//SteamNetworkingErrMsg errMsg;
	//ITrivialSignalingClient *pSignaling = CreateTrivialSignalingClient( pszTrivialSignalingService, SteamNetworkingSockets(), errMsg );
	//if ( pSignaling == nullptr )
	//	TEST_Fatal( "Failed to initializing signaling client.  %s", errMsg );

	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged( OnSteamNetConnectionStatusChanged );

	g_hPollGroup = SteamGameServerNetworkingSockets()->CreatePollGroup();

	// Comment this line in for more detailed spew about signals, route finding, ICE, etc
	//SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, k_ESteamNetworkingSocketsDebugOutputType_Verbose );

	// Create listen socket to receive connections on, unless we are the client
	if ( g_eTestRole == k_ETestRole_Server )
	{
		TEST_Printf( "Creating listen socket, local virtual port %d\n", g_nVirtualPortLocal );
		g_hListenSock = SteamGameServerNetworkingSockets()->CreateListenSocketP2P( g_nVirtualPortLocal, 0, nullptr );
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
		g_hListenSock = SteamGameServerNetworkingSockets()->CreateListenSocketP2P( g_nVirtualPortLocal, 1, &opt );
		assert( g_hListenSock != k_HSteamListenSocket_Invalid  );
	}

	// Main test loop
	SteamNetworkingMicroseconds usecNextAddNewConnection = 0;
	const size_t k_nMaxConnections = 250;
	SteamNetworkingMicroseconds usecNow = SteamNetworkingUtils()->GetLocalTimestamp();
	SteamNetworkingMicroseconds usecLastRateTime = usecNow;
	for (;;)
	{

		// Check callbacks
		TEST_PumpCallbacks();
		SteamGameServer_RunCallbacks();

		{
			SteamNetworkingMicroseconds usecEndPump = SteamNetworkingUtils()->GetLocalTimestamp();
			int msPump = int( ( usecEndPump - usecNow ) / 1000 );
			usecNow = usecEndPump;
			if ( msPump > 10 )
				TEST_Printf( "WARNING - pump took %dms\n", msPump );
		}

		// Check if it's time to add another connection
		if ( g_eTestRole != k_ETestRole_Server && s_vecConnections.size() < k_nMaxConnections && usecNextAddNewConnection < usecNow )
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
				TEST_Printf( "Adding connection.  Currently %d connections.\n", (int)s_vecConnections.size() );
			}

			AddConnection( SteamGameServerNetworkingSockets()->ConnectP2P( identityRemote, g_nVirtualPortRemote, (int)vecOpts.size(), vecOpts.data() ) );
			SteamNetworkingMicroseconds usecEndConnectP2P = SteamNetworkingUtils()->GetLocalTimestamp();
			int msConnectP2P = int( ( usecEndConnectP2P - usecNow ) / 1000 );
			usecNow = usecEndConnectP2P;
			if ( msConnectP2P > 10 )
				TEST_Printf( "WARNING - ConnectP2P took %dms\n", msConnectP2P );

			usecNextAddNewConnection = usecNow + SteamNetworkingMicroseconds( (float)rand() / (float)RAND_MAX * 500e3 );
		}

		// Think connections.
		// They might delete themselves, so do it in reverse order
		for ( int i = (int)s_vecConnections.size()-1 ; i >= 0 ; --i )
		{
			s_vecConnections[i]->Think( usecNow );
		}

		{
			SteamNetworkingMicroseconds usecEndThink = SteamNetworkingUtils()->GetLocalTimestamp();
			int msThink = int( ( usecEndThink - usecNow ) / 1000 );
			usecNow = usecEndThink;
			if ( msThink > 10 )
				TEST_Printf( "WARNING - Think took %dms\n", msThink );
		}

		// Just discard incoming messages
		for (;;)
		{
			SteamNetworkingMessage_t *pMessage[ 64 ];
			int r = SteamGameServerNetworkingSockets()->ReceiveMessagesOnPollGroup( g_hPollGroup, pMessage, 64 );
			for ( int i = 0 ; i < r ; ++i )
			{
				pMessage[i]->Release();
				++g_nMessagesRecv;
			}
			if ( r < 64 )
				break;
		}

		{
			SteamNetworkingMicroseconds usecEndPollMessages = SteamNetworkingUtils()->GetLocalTimestamp();
			int msPollMessages = int( ( usecEndPollMessages - usecNow ) / 1000 );
			usecNow = usecEndPollMessages;
			if ( msPollMessages > 10 )
				TEST_Printf( "WARNING - Poll messages took %dms\n", msPollMessages );
		}

		double flRateElapsed = ( usecNow - usecLastRateTime ) * 1e-6;
		if ( flRateElapsed > 5.0 )
		{
			TEST_Printf( "Rates: Messages %5.1f sent, %5.1f recv\n", g_nMessagesSent / flRateElapsed, g_nMessagesRecv / flRateElapsed );
			g_nMessagesSent = 0;
			g_nMessagesRecv = 0;
			usecLastRateTime = usecNow;
		}

	}
}
