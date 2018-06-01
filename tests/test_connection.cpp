#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
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

static std::default_random_engine g_rand;
static SteamNetworkingMicroseconds g_usecTestElapsed;

FILE *g_fpLog = nullptr;
SteamNetworkingMicroseconds g_logTimeZero;

static void DebugOutput( int eType, const char *pszMsg )
{
	SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - g_logTimeZero;
	if ( g_fpLog )
		fprintf( g_fpLog, "%10.6f %s\n", time*1e-6, pszMsg );
	if ( eType <= k_ESteamNetworkingSocketsDebugOutputType_Msg )
	{
		printf( "%10.6f %s\n", time*1e-6, pszMsg );
		fflush(stdout);
	}
	if ( eType == k_ESteamNetworkingSocketsDebugOutputType_Bug )
	{
		fflush(stdout);
		fflush(stderr);
		if ( g_fpLog )
			fflush( g_fpLog );

		// !KLUDGE! Our logging (which is done while we hold the lock)
		// is occasionally triggering this assert.  Just ignroe that one
		// error for now.
		// Yes, this is a kludge.
		if ( strstr( pszMsg, "SteamDatagramTransportLock held for" ) )
			return;

		assert( !"TEST FAILED" );
	}
}

static void Printf( const char *fmt, ... )
{
	char text[ 2048 ];
	va_list ap;
	va_start( ap, fmt );
	vsprintf( text, fmt, ap );
	va_end(ap);
	char *nl = strchr( text, '\0' ) - 1;
	if ( nl >= text && *nl == '\n' )
		*nl = '\0';
	DebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Msg, text );
}

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

	g_fpLog = fopen( "log.txt", "wt" );
	g_logTimeZero = SteamNetworkingUtils()->GetLocalTimestamp();

	SteamNetworkingSockets_SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Debug, DebugOutput );
	//SteamNetworkingSockets_SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Verbose, DebugOutput );
	//SteamNetworkingSockets_SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput );
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
	SteamNetworkingMicroseconds m_usecWhenSent;
	bool m_bReliable;
	int m_cbSize;
	static constexpr int k_cbMaxSize = 10*1000;
	uint8 m_data[ k_cbMaxSize ];
};

struct SFakePeer
{
	SFakePeer( const char *pName )
	: m_sName( pName )
	{
	}

	std::string m_sName;
	int64 m_nReliableSendMsgCount = 0;
	int64 m_nSendMsgCount = 0;
	int64 m_nReliableExpectedRecvMsg = 1;
	int64 m_nExpectedRecvMsg = 1;
	float m_flReliableMsgDelay = 0.0f;
	float m_flUnreliableMsgDelay = 0.0f;
	HSteamNetConnection m_hSteamNetConnection = k_HSteamNetConnection_Invalid;
	bool m_bIsConnected = false;
	int m_nMaxPendingBytes = 384 * 1024;
	SteamNetworkingQuickConnectionStatus m_info;
	float m_flSendReliableRate = 0.0f;
	float m_flRecvReliableRate = 0.0f;
	int64 m_nSendReliableInterval = 0;
	int64 m_nRecvReliableInterval = 0;

	inline void UpdateInterval( float flElapsed )
	{
		m_flSendReliableRate = m_nSendReliableInterval / flElapsed;
		m_flRecvReliableRate = m_nRecvReliableInterval / flElapsed;

		m_nSendReliableInterval = 0;
		m_nRecvReliableInterval = 0;
	}

	inline void UpdateStats()
	{
		SteamNetworkingSockets()->GetQuickConnectionStatus( m_hSteamNetConnection, &m_info );
	}

	inline int GetQueuedSendBytes()
	{
		return m_info.m_cbPendingReliable + m_info.m_cbPendingUnreliable;
	}

	inline void PrintStatus()
	{
		Printf( "%-10s:  %8d pending  %4d ping  %5.1f%% qual\n",
			m_sName.c_str(), m_info.m_cbPendingReliable + m_info.m_cbPendingUnreliable, 
			m_info.m_nPing, m_info.m_flConnectionQualityLocal*100.0f );
		Printf( "%-10s reliable %5.1fKB out %5.1fKB in  msg: %6lld out %6lld in %4.0fms delay\n",
			"", m_flSendReliableRate, m_flRecvReliableRate,
			(long long)m_nReliableSendMsgCount, (long long)m_nReliableExpectedRecvMsg,
			m_flReliableMsgDelay*1000.0f
		);
	}

	void SendRandomMessage( bool bReliable, int cbMaxSize )
	{
		TestMsg msg;
		msg.m_bReliable = bReliable;
		msg.m_usecWhenSent = SteamNetworkingUtils()->GetLocalTimestamp();
		msg.m_cbSize = std::uniform_int_distribution<>( 20, cbMaxSize )( g_rand );
		//bIsReliable = false;
		//nBytes = 1200-13;

		msg.m_nMsgNum = msg.m_bReliable ? ++m_nReliableSendMsgCount : ++m_nSendMsgCount;
		for ( int n = 0; n < msg.m_cbSize; ++n )
		{
			msg.m_data[n] = (uint8)( msg.m_nMsgNum + n );
		}

		int cbSend = (int)( sizeof(msg) - sizeof(msg.m_data) + msg.m_cbSize );
		if ( bReliable )
		{
			m_nSendReliableInterval += cbSend;
		}
		else
		{
			//m_nRecvReliableInterval += pIncomingMsg->GetSize();
		}

		EResult result = SteamNetworkingSockets()->SendMessageToConnection(
			m_hSteamNetConnection, 
			&msg,
			cbSend,
			msg.m_bReliable ? k_ESteamNetworkingSendType_Reliable : k_ESteamNetworkingSendType_Unreliable );

		if ( result != k_EResultOK )
		{
			Printf( "***ERROR ON Send: %s %.3f %s message %lld, %d bytes (pending %d bytes)\n", 
				 m_sName.c_str(), 
				 g_usecTestElapsed*1e-6,
				 msg.m_bReliable ? "reliable" : "unreliable",
				 (long long)msg.m_nMsgNum, 
				 msg.m_cbSize,
				 GetQueuedSendBytes() );
		}
	#if 0
		else
			Printf( "Send: %s %.3f %s message %lld, %d bytes (pending %d bytes)\n", 
				 connection.m_sName.c_str(), 
				 g_usecTestElapsed*1e-6,
				 msg.m_bReliable ? "reliable" : "unreliable",
				 (long long)msg.m_nMsgNum, 
				 msg.m_cbSize,
				 GetQueuedSendBytes() );
	#endif
	}

	void Send()
	{
		bool bReliable = std::uniform_real_distribution<>()( g_rand ) < .60;
		SendRandomMessage( bReliable, bReliable ? TestMsg::k_cbMaxSize : 2000 );
	}
};

static SFakePeer g_peerServer( "Server" );
static SFakePeer g_peerClient( "Client" );

static void Recv( ISteamNetworkingSockets *pSteamSocketNetworking )
{

	while ( true )
	{
		SFakePeer *pConnection = &g_peerServer;
		ISteamNetworkingMessage *pIncomingMsg = nullptr;
		int numMsgs = pSteamSocketNetworking->ReceiveMessagesOnConnection( pConnection->m_hSteamNetConnection, &pIncomingMsg, 1 );
		if ( numMsgs <= 0 )
		{
			pConnection = &g_peerClient;
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
			Printf(
				"Recv: %s, %s MISMATCH NUM wanted %lld got %lld\n",
				pConnection->m_sName.c_str(),
				pTestMsg->m_bReliable ? "RELIABLE" : "UNRELIABLE",
				(long long)nExpectedMsgNum,
				(long long)pTestMsg->m_nMsgNum );

			// This should not happen for reliable messages!
			assert( !pTestMsg->m_bReliable );
		}

		float flDelay = ( SteamNetworkingUtils()->GetLocalTimestamp() - pTestMsg->m_usecWhenSent ) * 1e-6f;
		if ( pTestMsg->m_bReliable )
		{
			pConnection->m_nRecvReliableInterval += pIncomingMsg->GetSize();
			pConnection->m_flReliableMsgDelay += ( flDelay - pConnection->m_flReliableMsgDelay ) * .25f;
		}
		else
		{
			pConnection->m_flUnreliableMsgDelay += ( flDelay - pConnection->m_flUnreliableMsgDelay ) * .25f;
			//pConnection->m_nRecvUnreliableInterval += pIncomingMsg->GetSize();
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
			Printf( "Steam Net connection %x %s, reason %d: %s\n",
				pInfo->m_hConn,
				( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ? "closed by peer" : "problem detected locally" ),
				pInfo->m_info.m_eEndReason,
				pInfo->m_info.m_szEndDebug
			);

			// Close our end
			SteamNetworkingSockets()->CloseConnection( pInfo->m_hConn, 0, nullptr, false );

			if ( g_peerServer.m_hSteamNetConnection == pInfo->m_hConn )
			{
				g_peerServer.m_hSteamNetConnection = k_HSteamNetConnection_Invalid;
			}
			if ( g_peerClient.m_hSteamNetConnection == pInfo->m_hConn )
			{
				g_peerClient.m_hSteamNetConnection = k_HSteamNetConnection_Invalid;
			}

			break;

	/*
		case k_ESteamNetworkingConnectionState_None:
			Printf( "No steam Net connection %x (%s)\n", pInfo->m_hConn, pInfo->m_info.m_steamIDRemote.Render() );

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
				Printf( "Accepting Steam Net connection %x\n", pInfo->m_hConn );
				g_peerServer.m_hSteamNetConnection = pInfo->m_hConn;
				g_peerServer.m_bIsConnected = true;
				SteamNetworkingSockets()->AcceptConnection( pInfo->m_hConn );
				SteamNetworkingSockets()->SetConnectionName( g_peerServer.m_hSteamNetConnection, "Server" );

			}
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			if ( pInfo->m_hConn == g_peerClient.m_hSteamNetConnection )
			{
				g_peerClient.m_bIsConnected = true;
			}
			Printf( "Connected Steam Net connection %x\n", pInfo->m_hConn );

			break;
		}
	}
};


static TestSteamNetworkingSocketsCallbacks g_Callbacks;

static void PumpCallbacks()
{
	#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
		SteamAPI_RunCallbacks();
	#endif
	SteamNetworkingSockets()->RunCallbacks( &g_Callbacks );
	std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
}

static void PumpCallbacksAndMakeSureStillConnected()
{
	PumpCallbacks();
	assert( g_peerClient.m_bIsConnected  );
	assert( g_peerServer.m_bIsConnected );
	assert( g_peerServer.m_hSteamNetConnection != k_HSteamNetConnection_Invalid );
	assert( g_peerClient.m_hSteamNetConnection != k_HSteamNetConnection_Invalid );
}

static void TestNetworkConditions( int rate, int loss, int lag, int reorderPct, int reorderLag, bool bActLikeGame )
{
	ISteamNetworkingSockets *pSteamSocketNetworking = SteamNetworkingSockets();

	Printf( "---------------------------------------------------\n" );
	Printf( "NETWORK CONDITIONS\n" );
	Printf( "Rate . . . . . . : %d Bps\n", rate );
	Printf( "Loss . . . . . . : %d%%\n", loss );
	Printf( "Ping . . . . . . : %d\n", lag*2 );
	Printf( "Reorder. . . . . : %d%% @ %dms\n", reorderPct, reorderLag );
	Printf( "Act like game. . : %d\n", (int)bActLikeGame );
	Printf( "---------------------------------------------------\n" );

	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_MinRate, rate );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_MaxRate, rate );

	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_FakePacketLoss_Send, loss );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_FakePacketLoss_Recv, 0 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_FakePacketLag_Send, lag );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_FakePacketLag_Recv, 0 );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_FakePacketReorder_Send, reorderPct );
	pSteamSocketNetworking->SetConfigurationValue( k_ESteamNetworkingConfigurationValue_FakePacketReorder_Time, reorderLag );

	SteamNetworkingMicroseconds usecWhenStarted = SteamNetworkingUtils()->GetLocalTimestamp();

	// Loop!

	//SteamNetworkingMicroseconds usecLastNow = usecWhenStarted;

	SteamNetworkingMicroseconds usecQuietDuration = 5000000;
	SteamNetworkingMicroseconds usecActiveDuration = 10000000;
	bool bQuiet = true;
	SteamNetworkingMicroseconds usecWhenStateEnd = 0;
	int nIterations = 5;
	SteamNetworkingMicroseconds usecLastPrint = SteamNetworkingUtils()->GetLocalTimestamp();

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

		g_peerServer.UpdateStats();
		g_peerClient.UpdateStats();

		int nServerPending = g_peerServer.GetQueuedSendBytes();
		int nClientPending = g_peerClient.GetQueuedSendBytes();

		// Start and stop active use of the connection.  This exercizes a bunch of important edge cases
		// such as keepalives, the bandwidth estimation, etc.
		if ( g_usecTestElapsed > usecWhenStateEnd )
		{
			if ( bQuiet )
			{
				if ( nServerPending == 0 &&  nClientPending == 0 )
				{
					bQuiet = false;
					usecWhenStateEnd = g_usecTestElapsed + ( bQuiet ? usecQuietDuration : usecActiveDuration );
					if ( nIterations-- <= 0 )
						break;
				}
			}
			else
			{
				bQuiet = true;
				usecWhenStateEnd = g_usecTestElapsed + ( bQuiet ? usecQuietDuration : usecActiveDuration );
			}
		}

		float flElapsedPrint = ( now - usecLastPrint ) * 1e-6f;
		if ( flElapsedPrint > 1.0f )
		{
			g_peerServer.UpdateInterval( flElapsedPrint );
			g_peerClient.UpdateInterval( flElapsedPrint );
			g_peerServer.PrintStatus();
			g_peerClient.PrintStatus();
			usecLastPrint = now;
		}

		if ( !bQuiet )
		{
			if ( nServerPending < g_peerServer.m_nMaxPendingBytes )
			{
				if ( bActLikeGame )
				{
					g_peerServer.SendRandomMessage( true, 4000 );
					g_peerServer.SendRandomMessage( false, 2000 );
				}
				else
				{
					g_peerServer.Send();
				}
			}
			if ( nClientPending < g_peerClient.m_nMaxPendingBytes )
			{
				if ( bActLikeGame )
				{
					g_peerClient.SendRandomMessage( true, 4000 );
					g_peerClient.SendRandomMessage( false, 2000 );
				}
				else
				{
					g_peerClient.Send();
				}
			}
		}
		PumpCallbacksAndMakeSureStillConnected();
		Recv( pSteamSocketNetworking );
		if ( bActLikeGame )
			std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );
	}
}

static void RunSteamDatagramConnectionTest()
{
	ISteamNetworkingSockets *pSteamSocketNetworking = SteamNetworkingSockets();


	// Command line options:
	// -connect:ip -- don't create a server, just try to connect to the given ip
	// -serveronly -- don't create a client only create a server and wait for connection
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

	// Initiate connection
	g_hSteamListenSocket = pSteamSocketNetworking->CreateListenSocket( -1, 0x0u, PORT_SERVER );
	g_peerClient.m_hSteamNetConnection = pSteamSocketNetworking->ConnectByIPv4Address( nConnectIP, PORT_SERVER );
	pSteamSocketNetworking->SetConnectionName( g_peerClient.m_hSteamNetConnection, "Client" );

//	// Send a few random message, before we get connected, just to test that case
//	g_peerClient.SendRandomMessage( true );
//	g_peerClient.SendRandomMessage( true );
//	g_peerClient.SendRandomMessage( true );

	// Wait for connection to complete
	while ( !g_peerClient.m_bIsConnected || !g_peerServer.m_bIsConnected )
		PumpCallbacks();

	auto Test = []( int rate, int loss, int lag, int reorderPct, int reorderLag )
	{
		TestNetworkConditions( rate, loss, lag, reorderPct, reorderLag, true );
		TestNetworkConditions( rate, loss, lag, reorderPct, reorderLag, false );
	};

	Test( 64000, 0, 0, 0, 0 );
	Test( 128000, 0, 0, 0, 0 );
	Test( 256000, 0, 0, 0, 0 );
	Test( 500000, 0, 0, 0, 0 );
	Test( 1000000, 0, 0, 0, 0 );
	Test( 2000000, 0, 0, 0, 0 );

	Test( 64000, 1, 25, 1, 10 );
	Test( 1000000, 1, 25, 1, 10 );

	Test( 64000, 5, 50, 2, 50 );
	Test( 1000000, 5, 50, 2, 10 );

	Test( 64000, 20, 100, 4, 50 );
	Test( 128000, 20, 100, 4, 40 );
	Test( 500000, 20, 100, 4, 30 );
	Test( 1000000, 20, 100, 4, 10 );
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
