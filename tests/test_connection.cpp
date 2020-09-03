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
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#define PORT_SERVER			27200	// Default server port, UDP/TCP

static std::default_random_engine g_rand;
static SteamNetworkingMicroseconds g_usecTestElapsed;

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
	float m_flSendRate = 0.0f;
	float m_flRecvRate = 0.0f;
	int64 m_nSendInterval = 0;
	int64 m_nRecvInterval = 0;

	inline void UpdateInterval( float flElapsed )
	{
		m_flSendRate = m_nSendInterval / flElapsed;
		m_flRecvRate = m_nRecvInterval / flElapsed;

		m_nSendInterval = 0;
		m_nRecvInterval = 0;
	}

	inline void UpdateStats()
	{
		SteamNetworkingSockets()->GetQuickConnectionStatus( m_hSteamNetConnection, &m_info );
	}

	inline int GetQueuedSendBytes()
	{
		return m_info.m_cbPendingReliable + m_info.m_cbPendingUnreliable;
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
		m_nSendInterval += cbSend;

		EResult result = SteamNetworkingSockets()->SendMessageToConnection(
			m_hSteamNetConnection, 
			&msg,
			cbSend,
			msg.m_bReliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable, nullptr );

		if ( result != k_EResultOK )
		{
			TEST_Printf( "***ERROR ON Send: %s %.3f %s message %lld, %d bytes (pending %d bytes)\n", 
				 m_sName.c_str(), 
				 g_usecTestElapsed*1e-6,
				 msg.m_bReliable ? "reliable" : "unreliable",
				 (long long)msg.m_nMsgNum, 
				 msg.m_cbSize,
				 GetQueuedSendBytes() );
			abort();
		}
	#if 0
		else
			TEST_Printf( "Send: %s %.3f %s message %lld, %d bytes (pending %d bytes)\n", 
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

		// Check for sequence number anomaly.
		int64 &nExpectedMsgNum = pTestMsg->m_bReliable ? pConnection->m_nReliableExpectedRecvMsg : pConnection->m_nExpectedRecvMsg;
		if ( pTestMsg->m_nMsgNum != nExpectedMsgNum && pTestMsg->m_bReliable )
		{

			// Print that it happened.
			TEST_Printf(
				"Recv: %s, %s MISMATCH NUM wanted %lld got %lld\n",
				pConnection->m_sName.c_str(),
				pTestMsg->m_bReliable ? "RELIABLE" : "UNRELIABLE",
				(long long)nExpectedMsgNum,
				(long long)pTestMsg->m_nMsgNum );

			// This should not happen for reliable messages!
			assert( !pTestMsg->m_bReliable );
		}

		float flDelay = ( SteamNetworkingUtils()->GetLocalTimestamp() - pTestMsg->m_usecWhenSent ) * 1e-6f;
		pConnection->m_nRecvInterval += pIncomingMsg->GetSize();
		if ( pTestMsg->m_bReliable )
		{
			pConnection->m_flReliableMsgDelay += ( flDelay - pConnection->m_flReliableMsgDelay ) * .25f;
		}
		else
		{
			pConnection->m_flUnreliableMsgDelay += ( flDelay - pConnection->m_flUnreliableMsgDelay ) * .25f;
		}

		nExpectedMsgNum = pTestMsg->m_nMsgNum + 1;
		pIncomingMsg->Release();
	}
}

void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *pInfo )
{
	// What's the state of the connection?
	switch ( pInfo->m_info.m_eState )
	{
	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		TEST_Printf( "Steam Net connection %x %s, reason %d: %s\n",
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
		TEST_Printf( "No steam Net connection %x (%s)\n", pInfo->m_hConn, pInfo->m_info.m_steamIDRemote.Render() );

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
			TEST_Printf( "[%s] Accepting\n", pInfo->m_info.m_szConnectionDescription );
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
		TEST_Printf( "[%s] connected\n", pInfo->m_info.m_szConnectionDescription );

		break;

	default:
		// Silences -Wswitch
		break;
	}
}

static void PumpCallbacksAndMakeSureStillConnected()
{
	TEST_PumpCallbacks();
	assert( g_peerClient.m_bIsConnected  );
	assert( g_peerServer.m_bIsConnected );
	assert( g_peerServer.m_hSteamNetConnection != k_HSteamNetConnection_Invalid );
	assert( g_peerClient.m_hSteamNetConnection != k_HSteamNetConnection_Invalid );
}

inline std::string FormatQuality( float q )
{
	if ( q < 0.0f ) return "???";
	char buf[32];
	sprintf( buf, "%.1f%%", q*100.0f );
	return buf;
}

static void PrintStatus( const SFakePeer &p1, const SFakePeer &p2 )
{
	const SteamNetworkingQuickConnectionStatus &info1 = p1.m_info;
	const SteamNetworkingQuickConnectionStatus &info2 = p2.m_info;
	TEST_Printf( "\n" );
	TEST_Printf( "%12s %12s\n", p1.m_sName.c_str(), p2.m_sName.c_str() );
	TEST_Printf( "%10dms %10dms  Ping\n", info1.m_nPing, info2.m_nPing );
	TEST_Printf( "%12s %12s  Quality\n", FormatQuality( info1.m_flConnectionQualityLocal ).c_str(), FormatQuality( info2.m_flConnectionQualityLocal ).c_str() );
	TEST_Printf( "%11.1fK %11.1fK  Send buffer\n", ( info1.m_cbPendingReliable+info1.m_cbPendingUnreliable )/1024.0f, ( info2.m_cbPendingReliable+info2.m_cbPendingUnreliable )/1024.0f );
	TEST_Printf( "%11.1fK %11.1fK  Send rate (app)\n", p1.m_flSendRate/1024.0f, p2.m_flSendRate/1024.0f );
	TEST_Printf( "%11.1fK %11.1fK  Send rate (wire)\n", info1.m_flOutBytesPerSec/1024.0f, info2.m_flOutBytesPerSec/1024.0f );
	TEST_Printf( "%12.1f %12.1f  Send pkts/sec (wire)\n", info1.m_flOutPacketsPerSec, info2.m_flOutPacketsPerSec );
	TEST_Printf( "%11.1fK %11.1fK  Send bandwidth (estimate)\n", info1.m_nSendRateBytesPerSecond/1024.0f, info2.m_nSendRateBytesPerSecond/1024.0f );
	TEST_Printf( "%11.1fK %11.1fK  Recv rate (app)\n", p1.m_flRecvRate/1024.0f, p2.m_flRecvRate/1024.0f );
	TEST_Printf( "%11.1fK %11.1fK  Recv rate (wire)\n", info1.m_flInBytesPerSec/1024.0f, info2.m_flInBytesPerSec/1024.0f );
	TEST_Printf( "%12.1f %12.1f  Recv pkts/sec (wire)\n", info1.m_flInPacketsPerSec, info2.m_flInPacketsPerSec );
	TEST_Printf( "%10.1fms %10.1fms  Send buffer drain time, based on bandwidth\n", ( info1.m_cbPendingReliable+info1.m_cbPendingUnreliable )*1000.0f/info1.m_nSendRateBytesPerSecond, ( info2.m_cbPendingReliable+info2.m_cbPendingUnreliable )*1000.0f/info2.m_nSendRateBytesPerSecond );
	TEST_Printf( "%10.1fms %10.1fms  App RTT (reliable)\n", p1.m_flReliableMsgDelay*1e3, p2.m_flReliableMsgDelay*1e3 );
	TEST_Printf( "%10.1fms %10.1fms  App RTT (unreliable)\n", p1.m_flUnreliableMsgDelay*1e3, p2.m_flUnreliableMsgDelay*1e3 );
}

static void TestNetworkConditions( int rate, float loss, int lag, float reorderPct, int reorderLag, bool bActLikeGame )
{
	ISteamNetworkingSockets *pSteamSocketNetworking = SteamNetworkingSockets();

	TEST_Printf( "---------------------------------------------------\n" );
	TEST_Printf( "NETWORK CONDITIONS\n" );
	TEST_Printf( "Rate . . . . . . : %d Bps\n", rate );
	TEST_Printf( "Loss . . . . . . : %g%%\n", loss );
	TEST_Printf( "Ping . . . . . . : %d\n", lag*2 );
	TEST_Printf( "Reorder. . . . . : %g%% @ %dms\n", reorderPct, reorderLag );
	TEST_Printf( "Act like game. . : %d\n", (int)bActLikeGame );
	TEST_Printf( "---------------------------------------------------\n" );

	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_SendRateMin, rate );
	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_SendRateMax, rate );

	SteamNetworkingUtils()->SetGlobalConfigValueFloat( k_ESteamNetworkingConfig_FakePacketLoss_Send, loss );
	SteamNetworkingUtils()->SetGlobalConfigValueFloat( k_ESteamNetworkingConfig_FakePacketLoss_Recv, 0 );
	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_FakePacketLag_Send, lag );
	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_FakePacketLag_Recv, 0 );
	SteamNetworkingUtils()->SetGlobalConfigValueFloat( k_ESteamNetworkingConfig_FakePacketReorder_Send, reorderPct );
	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_FakePacketReorder_Time, reorderLag );

	SteamNetworkingMicroseconds usecWhenStarted = SteamNetworkingUtils()->GetLocalTimestamp();

	// Loop!

	//SteamNetworkingMicroseconds usecLastNow = usecWhenStarted;

#if defined(SANITIZER) || defined(LIGHT_TESTS)
	const SteamNetworkingMicroseconds usecQuietDuration = 500000;
	const SteamNetworkingMicroseconds usecActiveDuration = 500000;
	const float flWaitBetweenPrints = 1.0f;
	int nIterations = 1;
#else
	const SteamNetworkingMicroseconds usecQuietDuration = 10000000;
	const SteamNetworkingMicroseconds usecActiveDuration = 25000000;
	const float flWaitBetweenPrints = 5.0f;
	int nIterations = 4;
#endif
	bool bQuiet = true;
	SteamNetworkingMicroseconds usecWhenStateEnd = 0;
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

		bool bCheckStateChange = ( usecWhenStateEnd == 0 ); 
		float flElapsedPrint = ( now - usecLastPrint ) * 1e-6f;
		if ( flElapsedPrint > flWaitBetweenPrints )
		{
			g_peerServer.UpdateInterval( flElapsedPrint );
			g_peerClient.UpdateInterval( flElapsedPrint );
			PrintStatus( g_peerServer, g_peerClient );
			usecLastPrint = now;
			bCheckStateChange = true;
		}

		// Start and stop active use of the connection.  This exercises a bunch of important edge cases
		// such as keepalives, the bandwidth estimation, etc.
		if ( bCheckStateChange && g_usecTestElapsed > usecWhenStateEnd )
		{
			if ( bQuiet )
			{
				if ( nServerPending == 0 &&  nClientPending == 0 )
				{
					bQuiet = false;
					usecWhenStateEnd = g_usecTestElapsed + ( bQuiet ? usecQuietDuration : usecActiveDuration );
					if ( nIterations-- <= 0 )
						break;
					TEST_Printf( "Entering active time (sending enabled)\n" );
				}
			}
			else
			{
				bQuiet = true;
				usecWhenStateEnd = g_usecTestElapsed + usecQuietDuration;
				TEST_Printf( "Entering quiet time (no sending) to see how fast queues drain\n" );
			}
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
	SteamNetworkingIPAddr bindServerAddress;
	bindServerAddress.Clear();
	bindServerAddress.m_port = PORT_SERVER;

	SteamNetworkingIPAddr connectToServerAddress;
	connectToServerAddress.SetIPv4( 0x7f000001, PORT_SERVER );

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
	g_hSteamListenSocket = pSteamSocketNetworking->CreateListenSocketIP( bindServerAddress, 0, nullptr );
	g_peerClient.m_hSteamNetConnection = pSteamSocketNetworking->ConnectByIPAddress( connectToServerAddress, 0, nullptr );
	pSteamSocketNetworking->SetConnectionName( g_peerClient.m_hSteamNetConnection, "Client" );

//	// Send a few random message, before we get connected, just to test that case
//	g_peerClient.SendRandomMessage( true );
//	g_peerClient.SendRandomMessage( true );
//	g_peerClient.SendRandomMessage( true );

	// Wait for connection to complete
	while ( !g_peerClient.m_bIsConnected || !g_peerServer.m_bIsConnected )
		TEST_PumpCallbacks();

	auto Test = []( int rate, float loss, int lag, float reorderPct, int reorderLag )
	{
		TestNetworkConditions( rate, loss, lag, reorderPct, reorderLag, false );
		TestNetworkConditions( rate, loss, lag, reorderPct, reorderLag, true );
	};

#ifndef LIGHT_TESTS
	Test( 64000, 20, 100, 4, 50 ); // low bandwidth, terrible packet loss
	Test( 1000000, 20, 100, 4, 10 ); // high bandwidth, terrible packet loss
	Test( 1000000, 2, 5, 2, 1 ); // wifi (high bandwideth, low packet loss, occasional reordering with very small delay)
	Test( 2000000, 0, 0, 0, 0 ); // LAN (high bandwidth, negligible lag/loss)
	Test( 128000, 20, 100, 4, 40 );
	Test( 500000, 20, 100, 4, 30 );

	Test( 64000, 0, 0, 0, 0 );
	Test( 128000, 0, 0, 0, 0 );
	Test( 256000, 0, 0, 0, 0 );
	Test( 500000, 0, 0, 0, 0 );
	Test( 1000000, 0, 0, 0, 0 );

	Test( 64000, 1, 25, 1, 10 );
	Test( 1000000, 1, 25, 1, 10 );

	Test( 64000, 5, 50, 2, 50 );
#endif
	Test( 1000000, 5, 50, 2, 10 );
}

// Some tests for identity string handling.  Doesn't really have anything to do with
// connectivity, this is just a conveinent place for this to live
void TestSteamNetworkingIdentity()
{
	SteamNetworkingIdentity id1, id2;
	char tempBuf[ SteamNetworkingIdentity::k_cchMaxString ];

	{
		CSteamID steamID( 1234, k_EUniversePublic, k_EAccountTypeIndividual );
		id1.SetSteamID( steamID );
		id1.ToString( tempBuf, sizeof(tempBuf ) );
		assert( id2.ParseString( tempBuf ) );
		assert( id2.GetSteamID() == steamID );
	}

	{
		const char *pszTempIPAddr = "ip:192.168.0.0:27015";
		assert( id1.ParseString( pszTempIPAddr ) );
		id1.ToString( tempBuf, sizeof(tempBuf ) );
		assert( strcmp( tempBuf, pszTempIPAddr ) == 0 );

		id1.SetLocalHost();
		id1.ToString( tempBuf, sizeof(tempBuf ) );
		assert( strcmp( tempBuf, "ip:::1" ) == 0 );
	}

	{
		const char *pszTempGenStr = "Locke Lamora";
		assert( id1.SetGenericString( pszTempGenStr ) );
		id1.ToString( tempBuf, sizeof(tempBuf ) );
		assert( strcmp( tempBuf, "str:Locke Lamora" ) == 0 );
		assert( id2.ParseString( tempBuf ) );
		assert( strcmp( id2.GetGenericString(), pszTempGenStr ) == 0 );
	}

}

int main(  )
{
	// Test some identity printing/parsing stuff
	TestSteamNetworkingIdentity();

	// Create client and server sockets
	TEST_Init( nullptr );
	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged( OnSteamNetConnectionStatusChanged );

	// Run the test
	RunSteamDatagramConnectionTest();

	TEST_Kill();	
	return 0;
}

#ifdef NN_NINTENDO_SDK
extern "C" void nnMain() { main(); }
#endif
