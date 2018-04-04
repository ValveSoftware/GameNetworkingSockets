#include <assert.h>
#include <stdio.h>
#include <string>
#include <random>
#include <chrono>
#include <thread>

#include <steamnetworkingsockets/isteamnetworkingsockets.h>
#include <steamnetworkingsockets/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#define PORT_SERVER			27200	// Default server port, UDP/TCP

static void InitSteamDatagramConnectionSockets()
{
	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		SteamDatagramErrMsg errMsg;
		if ( !GameNetworkingSockets_Init( errMsg ) )
		{
			fprintf( stderr, "GameNetworkingSockets_Init failed.  %s", errMsg );
			exit(1);
		}
	#else
		SteamAPI_Init();

		SteamDatagramErrMsg errMsg;
		if ( !SteamDatagramClient_Init( -1, errMsg ) )
		{
			fprintf( stderr, "SteamDatagramClient_Init failed.  %s", errMsg );
			exit(1);
		}
	#endif
}

static void ShutdownSteamDatagramConnectionSockets()
{
	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		GameNetworkingSockets_Kill();
	#else
		SteamDatagramClient_Kill();
	#endif
}

static HSteamListenSocket g_hSteamListenSocket = k_HSteamListenSocket_Invalid;

struct TestMsg
{
	int64 m_nMsgNum;
	bool m_bReliable;
	int m_cbSize;
	static constexpr int k_cbMaxSize = 20*1000;
	uint8 m_data[ k_cbMaxSize ];
};

struct SSteamNetConnection
{
	SSteamNetConnection( const char *pName )
	: m_sName( pName )
	{
	}

	std::string m_sName;
	int64 m_nReliableSendMsgCount = 0;
	int64 m_nSendMsgCount = 0;
	int64 m_nReliableExpectedRecvMsg = 1;
	int64 m_nExpectedRecvMsg = 1;
	HSteamNetConnection m_hSteamNetConnection = k_HSteamNetConnection_Invalid;
	SteamNetworkingMicroseconds m_usecLastTransmit = 0;
	SteamNetworkingMicroseconds m_usecTransmitTime = 0;
	bool m_bIsConnected = false;
	int m_nMaxPendingBytes = 384 * 1024;
	bool m_bQuiet = true;
	SteamNetworkingMicroseconds m_usecQuietDuration = 5000000;
	SteamNetworkingMicroseconds m_usecActiveDuration = 10000000;
	SteamNetworkingMicroseconds m_usecWhenStateEnd = 0;
};

static SSteamNetConnection g_connectionServer( "Server" );
static SSteamNetConnection g_connectionClient( "Client" );

static std::default_random_engine g_rand;
static SteamNetworkingMicroseconds g_usecTestElapsed;

static void Send( ISteamNetworkingSockets *pSteamSocketNetworking, SSteamNetConnection &connection )
{
	if ( connection.m_hSteamNetConnection == k_HSteamNetConnection_Invalid )
		return;

	if ( g_usecTestElapsed - connection.m_usecLastTransmit < connection.m_usecTransmitTime )
		return;

	// Start and stop active use of the connection.  This exercizes a bunch of important edge cases
	// such as keepalives, the bandwidth estimation, etc.
	if ( g_usecTestElapsed > connection.m_usecWhenStateEnd )
	{
		connection.m_bQuiet = !connection.m_bQuiet;
		connection.m_usecWhenStateEnd = g_usecTestElapsed +
			( connection.m_bQuiet ? connection.m_usecQuietDuration : connection.m_usecActiveDuration );
	}
	if ( connection.m_bQuiet )
		return;

	TestMsg msg;
	msg.m_bReliable = std::uniform_real_distribution<>()( g_rand ) < .75;
	msg.m_cbSize = std::uniform_int_distribution<>( 20, msg.m_bReliable ? TestMsg::k_cbMaxSize : 5000 )( g_rand );
	//bIsReliable = false;
	//nBytes = 1200-13;

	// Don't send unless we are clear
	SteamNetworkingQuickConnectionStatus info;
	pSteamSocketNetworking->GetQuickConnectionStatus( connection.m_hSteamNetConnection, &info );

	if ( info.m_cbPendingReliable + info.m_cbPendingUnreliable >= connection.m_nMaxPendingBytes )
	{
//		Log_Msg( LOG_NETWORKING_STEAMCONNECTIONS, "Send: %s stopped due to %d bytes pending\n",
//			connection.m_sName.c_str(), connection.m_nMaxPendingBytes );
		return; // Still stuff in the buffer
	}

	connection.m_usecLastTransmit = g_usecTestElapsed;

	msg.m_nMsgNum = msg.m_bReliable ? ++connection.m_nReliableSendMsgCount : ++connection.m_nSendMsgCount;
	for ( int n = 0; n < msg.m_cbSize; ++n )
	{
		msg.m_data[n] = (uint8)( msg.m_nMsgNum + n );
	}

	EResult result = pSteamSocketNetworking->SendMessageToConnection(
		connection.m_hSteamNetConnection, 
		&msg,
		(uint32)( sizeof(msg) - sizeof(msg.m_data) + msg.m_cbSize ),
		msg.m_bReliable ? k_ESteamNetworkingSendType_Reliable : k_ESteamNetworkingSendType_Unreliable );

	if ( result != k_EResultOK )
	{
		fprintf( stderr, "***ERROR ON Send: %s %.3f %s message %lld, %d bytes (pending %d bytes)\n", 
			 connection.m_sName.c_str(), 
			 g_usecTestElapsed*1e-6,
			 msg.m_bReliable ? "reliable" : "unreliable",
			 (long long)msg.m_nMsgNum, 
			 msg.m_cbSize,
			 info.m_cbPendingReliable + info.m_cbPendingUnreliable );
	}
#if 0
	else
		printf( "Send: %s %.3f %s message %lld, %d bytes (pending %d bytes)\n", 
			 connection.m_sName.c_str(), 
			 g_usecTestElapsed*1e-6,
			 msg.m_bReliable ? "reliable" : "unreliable",
			 (long long)msg.m_nMsgNum, 
			 msg.m_cbSize,
			 info.m_cbPendingReliable + info.m_cbPendingUnreliable );
#endif
}

static void Recv( ISteamNetworkingSockets *pSteamSocketNetworking )
{

	while ( true )
	{
		SSteamNetConnection *pConnection = &g_connectionServer;
		ISteamNetworkingMessage *pIncomingMsg = nullptr;
		int numMsgs = pSteamSocketNetworking->ReceiveMessagesOnConnection( pConnection->m_hSteamNetConnection, &pIncomingMsg, 1 );
		if ( numMsgs <= 0 )
		{
			pConnection = &g_connectionClient;
			numMsgs = pSteamSocketNetworking->ReceiveMessagesOnConnection( pConnection->m_hSteamNetConnection, &pIncomingMsg, 1 );
			if ( numMsgs <= 0 )
				return;
		}

		const TestMsg *pTestMsg = static_cast<const TestMsg*>( pIncomingMsg->GetData() );

		// Size makes sense?
		assert( sizeof(*pTestMsg) - sizeof(pTestMsg->m_data) + pTestMsg->m_cbSize == pIncomingMsg->GetSize() );

		// Check for sequence number nomoly.
		int64 &nExpectedMsgNum = pTestMsg->m_bReliable ? pConnection->m_nReliableExpectedRecvMsg : pConnection->m_nExpectedRecvMsg;
		if ( pTestMsg->m_nMsgNum != nExpectedMsgNum )
		{

			// Print that it happened.
			printf(
				"Recv: %s, %s MISMATCH NUM wanted %lld got %lld\n",
				pConnection->m_sName.c_str(),
				pTestMsg->m_bReliable ? "RELIABLE" : "UNRELIABLE",
				(long long)nExpectedMsgNum,
				(long long)pTestMsg->m_nMsgNum );

			// This should not happen for reliable messages!
			assert( !pTestMsg->m_bReliable );
		}

		nExpectedMsgNum = pTestMsg->m_nMsgNum + 1;
		pIncomingMsg->Release();
	}
}

struct TestSteamNetworkingSocketsCallbacks : public ISteamNetworkingSocketsCallbacks
{
	virtual ~TestSteamNetworkingSocketsCallbacks() {} // Silence GCC warning
	virtual void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *pInfo ) override
	{
		// What's the state of the connection?
		switch ( pInfo->m_info.m_eState )
		{
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			printf( "Steam Net connection %x %s, reason %d: %s\n",
				pInfo->m_hConn,
				( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ? "closed by peer" : "problem detected locally" ),
				pInfo->m_info.m_eEndReason,
				pInfo->m_info.m_szEndDebug
			);

			// Close our end
			SteamNetworkingSockets()->CloseConnection( pInfo->m_hConn, 0, nullptr, false );

			if ( g_connectionServer.m_hSteamNetConnection == pInfo->m_hConn )
			{
				g_connectionServer.m_hSteamNetConnection = k_HSteamNetConnection_Invalid;
			}
			if ( g_connectionClient.m_hSteamNetConnection == pInfo->m_hConn )
			{
				g_connectionClient.m_hSteamNetConnection = k_HSteamNetConnection_Invalid;
			}

			break;

	/*
		case k_ESteamNetworkingConnectionState_None:
			printf( "No steam Net connection %x (%s)\n", pInfo->m_hConn, pInfo->m_info.m_steamIDRemote.Render() );

			if ( g_hSteamNetConnection == pInfo->m_hConn )
			{
				g_bIsConnected = false;
				g_hSteamNetConnection = k_HSteamNetConnection_Invalid;
			}
			break;
	*/

		case k_ESteamNetworkingConnectionState_Connecting:

			// Is this a connection we initiated, or one that we are receiving?
			if ( g_hSteamListenSocket != k_HSteamListenSocket_Invalid && pInfo->m_info.m_hListenSocket == g_hSteamListenSocket )
			{
				// Somebody's knocking
				printf( "Accepting Steam Net connection %x\n", pInfo->m_hConn );
				g_connectionServer.m_hSteamNetConnection = pInfo->m_hConn;
				g_connectionServer.m_bIsConnected = true;
				SteamNetworkingSockets()->AcceptConnection( pInfo->m_hConn );
				SteamNetworkingSockets()->SetConnectionName( g_connectionServer.m_hSteamNetConnection, "Server" );

			}
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			if ( pInfo->m_hConn == g_connectionClient.m_hSteamNetConnection )
			{
				g_connectionClient.m_bIsConnected = true;
			}
			printf( "Connected Steam Net connection %x\n", pInfo->m_hConn );

			break;
		}
	}
};


static TestSteamNetworkingSocketsCallbacks g_Callbacks;

static void RunSteamDatagramConnectionTest()
{
	ISteamNetworkingSockets *pSteamSocketNetworking = SteamNetworkingSockets();

	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_DebugWindow, 1 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_Log_RTT, 0 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_Log_Packet, 0 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_Log_Segments, 0 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_Log_Feedback, 0 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_Log_Reliable, 0 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_Log_Message, 0 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_Log_Loss, 1 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_Log_X, 0 );

	//pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_MinRate, 50000 );
	//pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_SNP_MaxRate, 250000 );

	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_FakePacketLoss_Send, 5 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_FakePacketLoss_Recv, 0 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_FakePacketLag_Send, 10 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_FakePacketLag_Recv, 0 );

	// Command line options:
	// -connect:ip -- don't create a server, just try to connect to the given ip
	// -serveronly -- don't create a client only create a server and wait for connection
	bool bServerOnly = false;//( CommandLine()->FindParm( "-serveronly" ) != 0 );
	bool bClientOnly = false;
	uint32 nConnectIP = 0x7f000001; // 127.0.0.1

	//const char *s_pszConnectParm = "-connect:";
	//for ( int i = 0; i < CommandLine()->ParmCount(); ++i )
	//{
	//	if ( V_strnicmp( CommandLine()->GetParm( i ), s_pszConnectParm, V_strlen( s_pszConnectParm ) ) == 0 )
	//	{
	//		bClientOnly = true;
	//		connection_adr.SetFromString( CommandLine()->GetParm( i ) + V_strlen( s_pszConnectParm ) );
	//		break;
	//	}
	//}

	// Create listen socket
	if ( !bClientOnly )
	{
		g_hSteamListenSocket = pSteamSocketNetworking->CreateListenSocket( -1, 0x0u, PORT_SERVER );
	}
	if ( !bServerOnly )
	{
		g_connectionClient.m_hSteamNetConnection = pSteamSocketNetworking->ConnectByIPv4Address( nConnectIP, PORT_SERVER );
		pSteamSocketNetworking->SetConnectionName( g_connectionClient.m_hSteamNetConnection, "Client" );
	}

	SteamNetworkingMicroseconds usecWhenStarted = SteamNetworkingUtils()->GetLocalTimestamp();
	SteamNetworkingMicroseconds usecTestDuration = 120*1000000;
	SteamNetworkingMicroseconds usecWorstCase = usecTestDuration + 30*1000000;

	// Loop!

	//SteamNetworkingMicroseconds usecLastNow = usecWhenStarted;

	while ( true )
	{
		SteamNetworkingMicroseconds now = SteamNetworkingUtils()->GetLocalTimestamp();
		g_usecTestElapsed = now - usecWhenStarted;
		// If flElapsed > 1.0 when in debug, just slamp it
		//if ( Plat_IsInDebugSession() && now - flLastNow > 1.0f )
		//{
		//	flStartTime += now - flLastNow - 1.0f;
		//	flElapsed = now - flStartTime;
		//}
		//usecLastNow = now;

		// Execute Steam net connection callbacks now
		#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
			SteamAPI_RunCallbacks();
		#endif
		pSteamSocketNetworking->RunCallbacks( &g_Callbacks );

		// Break out if we lose connection
		if ( !bServerOnly && g_connectionClient.m_hSteamNetConnection == k_HSteamNetConnection_Invalid )
		{
			break;
		}

		if ( !g_connectionClient.m_bIsConnected && !g_connectionServer.m_bIsConnected )
		{
			continue; // Just spin until connected
		}

		Send( pSteamSocketNetworking, g_connectionServer );
		Send( pSteamSocketNetworking, g_connectionClient );

		Recv( pSteamSocketNetworking );

		if ( g_usecTestElapsed > usecTestDuration && g_connectionServer.m_hSteamNetConnection != k_HSteamNetConnection_Invalid )
		{
			pSteamSocketNetworking->CloseConnection( g_connectionServer.m_hSteamNetConnection, 0, nullptr, false );
			g_connectionServer.m_hSteamNetConnection = k_HSteamNetConnection_Invalid;
		}

		// Make sure we haven't taken too long
		assert( g_usecTestElapsed < usecWorstCase );

		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
	}
}

int main(  )
{
	// Create client and server sockets
	InitSteamDatagramConnectionSockets();

	// Run the test
	RunSteamDatagramConnectionTest();

	ShutdownSteamDatagramConnectionSockets();
	return 0;
}
