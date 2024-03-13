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

// It's 2021 and the C language doesn't have a cross-platform way to
// compare strings in a case-insensitive way
#ifdef _MSC_VER
	#define strcasecmp(a,b) stricmp(a,b)
#endif


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
		Reset();
	}

	std::string m_sName;
	int64 m_nReliableSendMsgCount;
	int64 m_nUnreliableSendMsgCount;
	int64 m_nReliableExpectedRecvMsg;
	int64 m_nExpectedRecvMsg;
	float m_flReliableMsgDelay;
	float m_flUnreliableMsgDelay;
	HSteamNetConnection m_hSteamNetConnection;
	bool m_bIsConnected;
	int m_cbSendBuffer;
	SteamNetConnectionRealTimeStatus_t m_realtimeStatus;
	float m_flSendRate;
	float m_flRecvRate;
	int64 m_nSendInterval;
	int64 m_nRecvInterval;

	void Reset()
	{
		m_nReliableSendMsgCount = 0;
		m_nUnreliableSendMsgCount = 0;
		m_nReliableExpectedRecvMsg = 1;
		m_nExpectedRecvMsg = 1;
		m_flReliableMsgDelay = 0.0f;
		m_flUnreliableMsgDelay = 0.0f;
		m_hSteamNetConnection = k_HSteamNetConnection_Invalid;
		m_bIsConnected = false;
		m_cbSendBuffer = 384 * 1024;
		memset( &m_realtimeStatus, 0, sizeof(m_realtimeStatus) );
		m_flSendRate = 0.0f;
		m_flRecvRate = 0.0f;
		m_nSendInterval = 0;
		m_nRecvInterval = 0;
	}

	void Close()
	{
		if ( m_hSteamNetConnection != k_HSteamNetConnection_Invalid )
		{
			SteamNetworkingSockets()->CloseConnection( m_hSteamNetConnection, 0, nullptr, false );
			m_hSteamNetConnection = k_HSteamNetConnection_Invalid;
		}
		Reset();
	}

	inline void UpdateInterval( float flElapsed )
	{
		m_flSendRate = m_nSendInterval / flElapsed;
		m_flRecvRate = m_nRecvInterval / flElapsed;

		m_nSendInterval = 0;
		m_nRecvInterval = 0;
	}

	inline void UpdateStats()
	{
		SteamNetworkingSockets()->GetConnectionRealTimeStatus( m_hSteamNetConnection, &m_realtimeStatus, 0, nullptr );
	}

	void SetConnectionConfig()
	{
		SteamNetworkingUtils()->SetConnectionConfigValueInt32( m_hSteamNetConnection, k_ESteamNetworkingConfig_SendBufferSize, m_cbSendBuffer );
	}

	inline int GetQueuedSendBytes()
	{
		return m_realtimeStatus.m_cbPendingReliable + m_realtimeStatus.m_cbPendingUnreliable + m_realtimeStatus.m_cbSentUnackedReliable;
	}

	void SendRandomMessage( bool bReliable, int cbMaxSize )
	{
		TestMsg msg;
		msg.m_bReliable = bReliable;
		msg.m_usecWhenSent = SteamNetworkingUtils()->GetLocalTimestamp();
		msg.m_cbSize = std::uniform_int_distribution<>( 20, cbMaxSize )( g_rand );
		//bIsReliable = false;
		//nBytes = 1200-13;

		msg.m_nMsgNum = msg.m_bReliable ? ++m_nReliableSendMsgCount : ++m_nUnreliableSendMsgCount;
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

static void CloseConnections()
{
	g_peerClient.Close();
	g_peerServer.Close();
	if ( g_hSteamListenSocket != k_HSteamNetConnection_Invalid )
	{
		SteamNetworkingSockets()->CloseListenSocket( g_hSteamListenSocket );
		g_hSteamListenSocket = k_HSteamNetConnection_Invalid;
	}
}

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
			g_peerServer.SetConnectionConfig();

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
	const SteamNetConnectionRealTimeStatus_t &info1 = p1.m_realtimeStatus;
	const SteamNetConnectionRealTimeStatus_t &info2 = p2.m_realtimeStatus;
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

static void ClearConfig()
{
	for (
		ESteamNetworkingConfigValue eValue = SteamNetworkingUtils()->IterateGenericEditableConfigValues( k_ESteamNetworkingConfig_Invalid, true );
		eValue != k_ESteamNetworkingConfig_Invalid;
		eValue = SteamNetworkingUtils()->IterateGenericEditableConfigValues( eValue, true )
	) {
		if ( eValue == k_ESteamNetworkingConfig_IP_AllowWithoutAuth )
			continue;
		SteamNetworkingUtils()->SetConfigValue( eValue, k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Int32, nullptr );
	}
}

static void TestNetworkConditions( int rate, float loss, int lag, float reorderPct, int reorderLag, bool bActLikeGame, bool bQuickTest )
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

	SteamNetworkingMicroseconds usecQuietDuration  = SteamNetworkingMicroseconds( ( bQuickTest ? 1.0 :  8.0 ) * 1e6 );
	SteamNetworkingMicroseconds usecActiveDuration = SteamNetworkingMicroseconds( ( bQuickTest ? 5.0 : 25.0 ) * 1e6 );
	float flWaitBetweenPrints = bQuickTest ? 2.0f : 5.0f;
	int nIterations = bQuickTest ? 2 : 4;

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
			if ( nServerPending < g_peerServer.m_cbSendBuffer - 16*1024 )
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
			if ( nClientPending < g_peerClient.m_cbSendBuffer - 16*1024 )
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

static void Test_Connection( bool bQuickTest )
{
	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged( OnSteamNetConnectionStatusChanged );

	CloseConnections();

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

	g_peerClient.SetConnectionConfig();

//	// Send a few random message, before we get connected, just to test that case
//	g_peerClient.SendRandomMessage( true );
//	g_peerClient.SendRandomMessage( true );
//	g_peerClient.SendRandomMessage( true );

	// Wait for connection to complete
	while ( !g_peerClient.m_bIsConnected || !g_peerServer.m_bIsConnected )
		TEST_PumpCallbacks();

	auto Test = [bQuickTest]( int rate, float loss, int lag, float reorderPct, int reorderLag )
	{
		TestNetworkConditions( rate, loss, lag, reorderPct, reorderLag, false, bQuickTest );
		TestNetworkConditions( rate, loss, lag, reorderPct, reorderLag, true, bQuickTest );
	};

	if ( bQuickTest )
	{
		// Quick test, just do two situations
		Test(  128000, 10, 50, 2, 50 ); // Low bandwidth, high packet loss
		Test( 1000000,  5, 10, 1, 10 ); // Medium bandwidth, still pretty bad packet loss
	}
	else
	{
		Test( 64000, 20, 100, 4, 50 ); // low bandwidth, terrible packet loss
		Test( 1000000, 20, 100, 4, 10 ); // high bandwidth, terrible packet loss
		Test( 1000000, 2, 5, 2, 1 ); // wifi (high bandwidth, low packet loss, occasional reordering with very small delay)
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
		Test( 1000000, 5, 50, 2, 10 );
	}
}

static void Test_quick() { Test_Connection( true ); }
static void Test_soak() { Test_Connection( false ); }

// Some tests for identity string handling.  Doesn't really have anything to do with
// connectivity, this is just a conveinent place for this to live
void Test_identity()
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
		const char *pszTempXBoxID = "8fg37rfsdf";
		assert( id1.SetXboxPairwiseID( pszTempXBoxID ) );
		id1.ToString( tempBuf, sizeof(tempBuf ) );
		assert( id2.ParseString( tempBuf ) );
		assert( strcmp( id2.GetXboxPairwiseID(), pszTempXBoxID ) == 0 );
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

void Test_lane_quick_queueanddrain()
{
	// Create a loopback connection, over the local network.
	// (With a loopback over internal buffers, all messages
	// are delivered instantly and lanes are irrelevant.)
	HSteamNetConnection hSender, hRecver;
	assert( SteamNetworkingSockets()->CreateSocketPair( &hSender, &hRecver, true, nullptr, nullptr ) );

	// Set the send rate to a fixed value
	const int k_nSendRate = 128*1024;
	SteamNetworkingUtils()->SetConnectionConfigValueInt32( hSender, k_ESteamNetworkingConfig_SendRateMin, k_nSendRate );
	SteamNetworkingUtils()->SetConnectionConfigValueInt32( hSender, k_ESteamNetworkingConfig_SendRateMax, k_nSendRate );

	// Configure lanes.
	constexpr int k_nLanes = 4;
	int priorities[k_nLanes] = { 2,  0,  1,  1 };
	uint16 weights[k_nLanes] = { 1,  1, 25, 75 };
	assert( k_EResultOK == SteamNetworkingSockets()->ConfigureConnectionLanes( hSender, k_nLanes, priorities, weights ) );

	// We're gonna dump a whole bunch of stuff at once and watch it
	// drain, broke up into messages
	constexpr int k_nMsgPerLane = 128;
	constexpr int k_cbMsg = 1024; // We're sending unreliable messages, so don't make them huge
	constexpr int k_cbLaneData = k_nMsgPerLane * k_cbMsg;
	constexpr int k_nTotalMsg = k_nLanes * k_nMsgPerLane;
	constexpr int k_cbTotalData = k_nLanes * k_cbLaneData;

	// Allow us to buffer up a whole bunch
	SteamNetworkingUtils()->SetConnectionConfigValueInt32( hSender, k_ESteamNetworkingConfig_SendBufferSize, k_cbTotalData + 1024 );

	// Dump a fixed amount of data into each lane
	{
		SteamNetworkingMessage_t *pMessages[k_nTotalMsg];
		int idxMsg = 0;
		for ( int idxLane = 0 ; idxLane < k_nLanes ; ++idxLane )
		{
			for ( int j = 0 ; j < k_nMsgPerLane ; ++j )
			{
				 SteamNetworkingMessage_t *pMsg = SteamNetworkingUtils()->AllocateMessage( k_cbMsg );
				 assert( pMsg->m_cbSize == k_cbMsg );
				 pMsg->m_conn = hSender;
				 pMsg->m_nFlags = 0; // Just send everything unreliable for this test
				 pMsg->m_idxLane = (uint16)idxLane;
				 pMessages[idxMsg++] = pMsg;
			}
		}
		assert( idxMsg == k_nTotalMsg );
		SteamNetworkingSockets()->SendMessages( idxMsg, pMessages, nullptr );
	}

	// Remember when we sent all the messages
	SteamNetworkingMicroseconds usecStartTime = SteamNetworkingUtils()->GetLocalTimestamp();

	// Get back realtime status
	SteamNetConnectionRealTimeStatus_t status;
	SteamNetConnectionRealTimeLaneStatus_t laneStatus[k_nLanes];
	assert( k_EResultOK == SteamNetworkingSockets()->GetConnectionRealTimeStatus( hSender, &status, k_nLanes, laneStatus ) );

	// We should be able to predict the results of these estimates very accurately
	const SteamNetworkingMicroseconds usecTol = 50*1000;
	const double flSecTol = usecTol * 1e-6;
	const int cbTol = (int)( k_nSendRate * flSecTol );

	assert( status.m_cbPendingReliable == 0 );
	assert( status.m_cbPendingUnreliable <= k_cbTotalData );
	assert( status.m_cbPendingUnreliable > k_cbTotalData - cbTol );

	// Lane 1 has the lowest priority number, and should be the only one
	// with any data sent
	assert( laneStatus[1].m_cbPendingReliable == 0 );
	assert( laneStatus[1].m_cbPendingUnreliable <= k_cbLaneData );
	assert( laneStatus[1].m_cbPendingUnreliable > k_cbLaneData - cbTol );
	SteamNetworkingMicroseconds usecExpectedQueueTime1 = (SteamNetworkingMicroseconds)( laneStatus[1].m_cbPendingUnreliable * 1e6 / k_nSendRate );
	assert( laneStatus[1].m_usecQueueTime < usecExpectedQueueTime1 + usecTol );
	assert( laneStatus[1].m_usecQueueTime > usecExpectedQueueTime1 - usecTol );

	// After lane 1 finishes, we expect the next chunk of time
	// to be divided between lanes 2 and 3, at a ratio of 25:75, respectively.
	// This means when lane 3 finishes, we will have sent all of lane 3,
	// plus 1/3rd of lane 2.  Thus it will finish in the time it takes
	// to send 4/3rd of a lane.
	assert( laneStatus[3].m_cbPendingReliable == 0 );
	assert( laneStatus[3].m_cbPendingUnreliable == k_cbLaneData ); // Should not have sent any data on 3 yet
	SteamNetworkingMicroseconds usecExpectedQueueTime3 = (SteamNetworkingMicroseconds)( usecExpectedQueueTime1 + k_cbLaneData * 4.0/3.0 * 1e6 / k_nSendRate );
	assert( laneStatus[3].m_usecQueueTime < usecExpectedQueueTime3 + usecTol );
	assert( laneStatus[3].m_usecQueueTime > usecExpectedQueueTime3 - usecTol );

	// After lane 3 finishes, lane 2 will have the connection all to itself,
	// to send the remaining 2/3rd of the complete lane data we buffered
	assert( laneStatus[2].m_cbPendingReliable == 0 );
	assert( laneStatus[2].m_cbPendingUnreliable == k_cbLaneData ); // Should not have sent any data on 2 yet
	SteamNetworkingMicroseconds usecExpectedQueueTime2 = (SteamNetworkingMicroseconds)( usecExpectedQueueTime3 + k_cbLaneData * 2.0/3.0 * 1e6 / k_nSendRate );
	assert( laneStatus[2].m_usecQueueTime < usecExpectedQueueTime2 + usecTol );
	assert( laneStatus[2].m_usecQueueTime > usecExpectedQueueTime2 - usecTol );

	// Finally, lane 0, the lowest priority lane, will drain
	assert( laneStatus[0].m_cbPendingReliable == 0 );
	assert( laneStatus[0].m_cbPendingUnreliable == k_cbLaneData ); // Should not have sent any data on 0 yet
	SteamNetworkingMicroseconds usecExpectedQueueTime0 = (SteamNetworkingMicroseconds)( usecExpectedQueueTime2 + k_cbLaneData * 1e6 / k_nSendRate );
	assert( laneStatus[0].m_usecQueueTime < usecExpectedQueueTime0 + usecTol );
	assert( laneStatus[0].m_usecQueueTime > usecExpectedQueueTime0 - usecTol );

	// Send one last one-byte message on each lane,
	// so we can tell when we are done for that lane
	{
		SteamNetworkingMessage_t *pMessages[k_nLanes];
		for ( int idxLane = 0 ; idxLane < k_nLanes ; ++idxLane )
		{
			SteamNetworkingMessage_t *pMsg = SteamNetworkingUtils()->AllocateMessage( 1 );
			assert( pMsg->m_cbSize == 1 );
			pMsg->m_conn = hSender;
			pMsg->m_nFlags = 0; // Just send everything unreliable for this test
			pMsg->m_idxLane = (uint16)idxLane;
			pMessages[idxLane] = pMsg;
		}
		SteamNetworkingSockets()->SendMessages( k_nLanes, pMessages, nullptr );
	}

	int cbLaneReceived[k_nLanes] = {};
	int nLanesFinished = 0;
	while ( nLanesFinished < k_nLanes )
	{
		SteamNetworkingMessage_t *pMsg;
		while ( SteamNetworkingSockets()->ReceiveMessagesOnConnection( hRecver, &pMsg, 1 ) == 1 )
		{
			int idxLane = pMsg->m_idxLane;

			//TEST_Printf( "RX Lane %d msg %lld sz %d\n", idxLane, pMsg->m_nMessageNumber, pMsg->m_cbSize );

			switch ( idxLane )
			{
				case 1:
					assert( cbLaneReceived[2] == 0 );
					assert( cbLaneReceived[3] == 0 );
					assert( cbLaneReceived[0] == 0 );
					break;
				case 3:
					assert( cbLaneReceived[1] == k_cbLaneData+1 );
					assert( nLanesFinished == 1 );
					assert( cbLaneReceived[0] == 0 );
					break;
				case 2:
					assert( cbLaneReceived[1] == k_cbLaneData+1 );
					assert( nLanesFinished == 1 || nLanesFinished == 2 );
					assert( cbLaneReceived[0] < 2048 ); // First bit of lane 0 might come in before the last part of this lane
					break;
				case 0:
					assert( cbLaneReceived[1] == k_cbLaneData+1 );
					assert( cbLaneReceived[3] == k_cbLaneData+1 );

					// The very first packet might deliver some data on lane 0
					// before the other lanes due to internal quirks of serialization,
					// but in general we should have finished all the other lanes first
					if ( cbLaneReceived[0] > 2048 )
					{
						assert( cbLaneReceived[2] == k_cbLaneData+1 );
						assert( nLanesFinished == 3 );
					}
					break;
			}
			cbLaneReceived[idxLane] += pMsg->m_cbSize;
			if ( pMsg->m_cbSize == 1 )
			{
				assert( cbLaneReceived[idxLane] == k_cbLaneData+1 );
				++nLanesFinished;
				float msElapsed = ( SteamNetworkingUtils()->GetLocalTimestamp() - usecStartTime ) * 1e-3f;
				TEST_Printf( "Lane %d finished @ %.1fms, expected %.1fms.  %6d %6d %6d %6d\n",
					idxLane,
					msElapsed, laneStatus[idxLane].m_usecQueueTime * 1e-3f,
					cbLaneReceived[0],
					cbLaneReceived[1],
					cbLaneReceived[2],
					cbLaneReceived[3]
				);

				if ( idxLane == 3 )
				{
					assert( cbLaneReceived[2] * 75 <= ( cbLaneReceived[3]+k_cbMsg ) * 25 );
					assert( (cbLaneReceived[2]+k_cbMsg) * 75 >= cbLaneReceived[3] * 25 );
				}
			}
			else
			{
				assert( pMsg->m_cbSize == k_cbMsg );
				assert( cbLaneReceived[idxLane] <= k_cbLaneData );
			}
			pMsg->Release();
		}
		TEST_PumpCallbacks();
	}

	// Cleanup
	SteamNetworkingSockets()->CloseConnection( hSender, 0, nullptr, false );
	SteamNetworkingSockets()->CloseConnection( hRecver, 0, nullptr, false );
}

// Test a particular use case that we have specifically been asked about:
// Three lanes:
// - Lane for most gameplay traffic.
// - "Priority" lane for certain urgent messages
// - "Background" land for content download
void Test_lane_quick_priority_and_background()
{
	// Create a loopback connection, over the local network.
	// (With a loopback over internal buffers, all messages
	// are delivered instantly and lanes are irrelevant.)
	HSteamNetConnection hServer, hClient;
	assert( SteamNetworkingSockets()->CreateSocketPair( &hServer, &hClient, true, nullptr, nullptr ) );
	SteamNetworkingSockets()->SetConnectionName( hServer, "server" );
	SteamNetworkingSockets()->SetConnectionName( hClient, "client" );

	// Set the send rate to a fixed value
	const int k_nSendRate = 256*1024;
	SteamNetworkingUtils()->SetConnectionConfigValueInt32( hServer, k_ESteamNetworkingConfig_SendRateMin, k_nSendRate );
	SteamNetworkingUtils()->SetConnectionConfigValueInt32( hServer, k_ESteamNetworkingConfig_SendRateMax, k_nSendRate );

	// Configure lanes.
	constexpr int k_nLanes = 3;
	constexpr int k_LaneGameplay = 0;
	constexpr int k_LaneUrgent = 1;
	constexpr int k_LaneBackground = 2;
	int priorities[k_nLanes] = {  1, 0,  1 };
	uint16 weights[k_nLanes] = { 75, 1, 25 };
	assert( k_EResultOK == SteamNetworkingSockets()->ConfigureConnectionLanes( hServer, k_nLanes, priorities, weights ) );

	// Allow us to buffer up a decent amount for background content download
	constexpr int k_cbMaxBackgroundInFlight = 1024 * 1024;
	SteamNetworkingUtils()->SetConnectionConfigValueInt32( hServer, k_ESteamNetworkingConfig_SendBufferSize, k_cbMaxBackgroundInFlight + 64*1024 );

	// Remember when we sent all the messages
	SteamNetworkingMicroseconds usecStartTime = SteamNetworkingUtils()->GetLocalTimestamp();
	SteamNetworkingMicroseconds usecNextSendUrgent = 0;
	SteamNetworkingMicroseconds usecNextSendGameplay = usecStartTime;

	const int k_nFakeLag = 50;

	// Set some reasonable network conditions
	SteamNetworkingUtils()->SetGlobalConfigValueFloat( k_ESteamNetworkingConfig_FakePacketLoss_Send, 2.0 );
	SteamNetworkingUtils()->SetGlobalConfigValueFloat( k_ESteamNetworkingConfig_FakePacketLoss_Recv, 0 );
	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_FakePacketLag_Send, k_nFakeLag );
	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_FakePacketLag_Recv, 0 );
	SteamNetworkingUtils()->SetGlobalConfigValueFloat( k_ESteamNetworkingConfig_FakePacketReorder_Send, .5 );
	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_FakePacketReorder_Time, 25 );

	int64 nMsgSent[ k_nLanes ] = {};
	int64 nMsgRecv[ k_nLanes ] = {};
	int64 nLatencyTotal[ k_nLanes ] = {};
	int64 nLatencySqTotal[ k_nLanes ] = {};

	for (;;)
	{
		SteamNetworkingMicroseconds usecNow = SteamNetworkingUtils()->GetLocalTimestamp();

		// RUn the test for 60 seconds
		if ( usecStartTime + 60*1000*1000 < usecNow )
			break;

		// Get back realtime status
		SteamNetConnectionRealTimeStatus_t status;
		SteamNetConnectionRealTimeLaneStatus_t laneStatus[k_nLanes];
		assert( k_EResultOK == SteamNetworkingSockets()->GetConnectionRealTimeStatus( hServer, &status, k_nLanes, laneStatus ) );

		// Keep the background download pipe full
		if ( laneStatus[ k_LaneBackground ].m_cbPendingReliable + k_cbMaxSteamNetworkingSocketsMessageSizeSend <= k_cbMaxBackgroundInFlight )
		{
			SteamNetworkingMessage_t *pMsg = SteamNetworkingUtils()->AllocateMessage( k_cbMaxSteamNetworkingSocketsMessageSizeSend );
			pMsg->m_conn = hServer;
			pMsg->m_nFlags = k_nSteamNetworkingSend_Reliable;
			pMsg->m_idxLane = k_LaneBackground;

			// Shove the time when we sent it into the body,
			// but otherwise leave the rest of the body unitialized
			*(SteamNetworkingMicroseconds *)pMsg->m_pData = usecNow;

			int64 nMsgNum;
			SteamNetworkingSockets()->SendMessages( 1, &pMsg, &nMsgNum );
			++nMsgSent[k_LaneBackground];
			assert( nMsgNum == nMsgSent[k_LaneBackground] );
		}

		// Time to send a small priority message?
		if ( usecNow >= usecNextSendUrgent )
		{
			SteamNetworkingMessage_t *pMsg = SteamNetworkingUtils()->AllocateMessage( std::uniform_int_distribution<>( 100, 500 )( g_rand ) );
			pMsg->m_conn = hServer;
			pMsg->m_nFlags = k_nSteamNetworkingSend_ReliableNoNagle;
			pMsg->m_idxLane = k_LaneUrgent;

			// Shove the time when we sent it into the body,
			// but otherwise leave the rest of the body unitialized
			*(SteamNetworkingMicroseconds *)pMsg->m_pData = usecNow;

			int64 nMsgNum;
			SteamNetworkingSockets()->SendMessages( 1, &pMsg, &nMsgNum );
			++nMsgSent[k_LaneUrgent];
			assert( nMsgNum == nMsgSent[k_LaneUrgent] );

			// Schedule the next send at a random interval
			usecNextSendUrgent = usecNow + std::uniform_int_distribution<>( 500, 1500 )( g_rand ) * 1000;
		}

		// Time to send a gameplay message
		if ( usecNow >= usecNextSendGameplay )
		{

			// Send a gameplay message server->client
			{
				SteamNetworkingMessage_t *pMsg = SteamNetworkingUtils()->AllocateMessage( std::uniform_int_distribution<>( 1000, 5000 )( g_rand ) );
				pMsg->m_conn = hServer;
				pMsg->m_idxLane = k_LaneGameplay;

				// Occasionally send reliable
				pMsg->m_nFlags = std::uniform_int_distribution<>( 0, 100 )( g_rand ) < 30 ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle;

				// Shove the time when we sent it into the body,
				// but otherwise leave the rest of the body unitialized
				*(SteamNetworkingMicroseconds *)pMsg->m_pData = usecNow;

				int64 nMsgNum;
				SteamNetworkingSockets()->SendMessages( 1, &pMsg, &nMsgNum );
				++nMsgSent[k_LaneGameplay];
				assert( nMsgNum == nMsgSent[k_LaneGameplay] );
			}

			// Send a gameplay message client->server
			{
				SteamNetworkingMessage_t *pMsg = SteamNetworkingUtils()->AllocateMessage( std::uniform_int_distribution<>( 100, 2000 )( g_rand ) );
				pMsg->m_conn = hClient;
				pMsg->m_idxLane = k_LaneGameplay;

				// Occasionally send reliable
				pMsg->m_nFlags = std::uniform_int_distribution<>( 0, 100 )( g_rand ) < 30 ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle;

				int64 nMsgNum;
				SteamNetworkingSockets()->SendMessages( 1, &pMsg, &nMsgNum );
				assert( nMsgNum >= 0 );
			}

			// Schedule the next send at 30hz
			usecNextSendGameplay += 1000*1000 / 30;
		}

		usecNow = SteamNetworkingUtils()->GetLocalTimestamp();

		// Client receive messages
		{
			SteamNetworkingMessage_t *pMsg;
			while ( SteamNetworkingSockets()->ReceiveMessagesOnConnection( hClient, &pMsg, 1 ) == 1 )
			{
				SteamNetworkingMicroseconds usecLatency = usecNow - *(SteamNetworkingMicroseconds*)pMsg->m_pData;
				int idxLane = pMsg->m_idxLane;
				if ( idxLane != k_LaneGameplay || pMsg->m_nMessageNumber%30 == 0 )
					TEST_Printf( "RX lane %d one-way latency %6.1fms  #%lld\n", idxLane, usecLatency*1e-3, pMsg->m_nMessageNumber );

				++nMsgRecv[ idxLane ];
				if ( idxLane != k_LaneGameplay )
					assert( pMsg->m_nMessageNumber == nMsgRecv[ idxLane ] );

				int msLatency = (int)( usecLatency / 1000 );
				nLatencyTotal[ idxLane ] += msLatency;
				nLatencySqTotal[ idxLane ] += msLatency*msLatency;

				pMsg->Release();
			}
		}

		// Server receive messages
		{
			SteamNetworkingMessage_t *pMsg;
			while ( SteamNetworkingSockets()->ReceiveMessagesOnConnection( hServer, &pMsg, 1 ) == 1 )
			{
				pMsg->Release();
			}
		}

		// Background tasks, etc
		TEST_PumpCallbacks();
	}
	TEST_Printf( "\n\n" );

	// Cleanup
	SteamNetworkingSockets()->CloseConnection( hServer, 0, nullptr, false );
	SteamNetworkingSockets()->CloseConnection( hClient, 0, nullptr, false );

	float flAvgLatencyMS[ k_nLanes ];
	float flRMSLatencyMS[ k_nLanes ];
	for ( int i = 0 ; i < 3 ; ++i )
	{
		flAvgLatencyMS[i] = (float)nLatencyTotal[i] / (float)nMsgRecv[i];
		flRMSLatencyMS[i] = sqrt( (float)nLatencySqTotal[i] / (float)nMsgRecv[i] );

		// FIXME - print stddev?
		TEST_Printf( "Lane %d: %6lld msgs, one-way latency avg %6.1fms, RMS %6.1fms\n",
			i, nMsgRecv[i], flAvgLatencyMS[i], flRMSLatencyMS[i] );
	}

	// Check numbers...PIOMA
	//assert( flRMSPingMS[k_LaneUrgent] < flRMSPingMS[k_LaneGameplay] ); // We are comparing pings of always reliable msgs to pings that might sometimes be unreliable, so not totally a fair comparison
	//assert( flRMSPingMS[k_LaneGameplay] < k_nFakeLag*1.2 + 10 );

}

void Test_netloopback_throughput()
{
	// Create a loopback connection, over the local network.
	HSteamNetConnection hServer, hClient;
	assert( SteamNetworkingSockets()->CreateSocketPair( &hServer, &hClient, true, nullptr, nullptr ) );
	SteamNetworkingSockets()->SetConnectionName( hServer, "server" );
	SteamNetworkingSockets()->SetConnectionName( hClient, "client" );

	// Try several increasing send rates and make sure we can keep up
	// FIXME Something broken here with this test above 30000, that isn't reproducing
	// for me locally.  Temporarily removing the higher rates until I can investigate.
	//for ( int nSendRateKB: { 8000, 12000, 16000, 20000, 30000, 40000, 50000, 60000 } )
	for ( int nSendRateKB: { 8000, 12000, 16000, 20000, 30000 } )
	{
		const int nSendRate = nSendRateKB*1000; // Use powers of 10 here, not 1024

		TEST_Printf( "-- TESTING SEND RATE: %dKB/sec -------\n\n", nSendRateKB );

		// Set the send rate to a fixed value
		SteamNetworkingUtils()->SetConnectionConfigValueInt32( hServer, k_ESteamNetworkingConfig_SendRateMin, nSendRate );
		SteamNetworkingUtils()->SetConnectionConfigValueInt32( hServer, k_ESteamNetworkingConfig_SendRateMax, nSendRate );
		SteamNetworkingUtils()->SetConnectionConfigValueInt32( hClient, k_ESteamNetworkingConfig_SendRateMin, nSendRate );
		SteamNetworkingUtils()->SetConnectionConfigValueInt32( hClient, k_ESteamNetworkingConfig_SendRateMax, nSendRate );
		SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_LogLevel_PacketGaps, k_ESteamNetworkingSocketsDebugOutputType_Verbose );

		// For this test we'll try to keep about 200ms worth of data queued, so the pipe stays full
		const int k_nBufferQueuedTarget = nSendRate / 5;

		// Set the send buffer
		SteamNetworkingUtils()->SetConnectionConfigValueInt32( hServer, k_ESteamNetworkingConfig_SendBufferSize, k_nBufferQueuedTarget*5/4 + 1024 );

		// How long do we want to send?
		constexpr SteamNetworkingMicroseconds k_usecSendTime = SteamNetworkingMicroseconds( 10 * 1e6 );

		// Run at this send rate for several seconds
		SteamNetworkingMicroseconds usecStartTime = SteamNetworkingUtils()->GetLocalTimestamp();
		SteamNetworkingMicroseconds usecLastPrint = usecStartTime;
		int64 cbBytesSent = 0;
		int64 cbBytesRecv = 0;
		bool bDrain = false;
		for (;;)
		{
			TEST_PumpCallbacks();

			// Query status
			SteamNetConnectionRealTimeStatus_t serverStatus;
			assert( k_EResultOK == SteamNetworkingSockets()->GetConnectionRealTimeStatus( hServer, &serverStatus, 0, nullptr ) );

			SteamNetConnectionRealTimeStatus_t clientStatus;
			assert( k_EResultOK == SteamNetworkingSockets()->GetConnectionRealTimeStatus( hClient, &clientStatus, 0, nullptr ) );

			// Time to enter drain mode?
			SteamNetworkingMicroseconds usecNow = SteamNetworkingUtils()->GetLocalTimestamp();
			if ( !bDrain && usecNow > usecStartTime + k_usecSendTime )
			{
				TEST_Printf( "Entering drain mode\n" );
				bDrain = true;
				usecLastPrint = 0;
			}

			// Time to print status?
			if ( usecLastPrint + 500*1000 < usecNow )
			{
				SteamNetworkingMicroseconds usecElapsed = usecNow - usecStartTime;
				assert( usecElapsed < k_usecSendTime * 2 );
				double flElapsedSeconds = usecElapsed * 1e-6;
				TEST_Printf( "Elapsed:%6.0fms   Sent:%7.0fK   Recv:%7.0fK = %5.0fK/sec  (Wire%6.3f kpkts/sec Qual %5.1f%%)\n",
					flElapsedSeconds * 1e3,
					cbBytesSent * 1e-3,
					cbBytesRecv * 1e-3,
					cbBytesRecv * 1e-3 / flElapsedSeconds,
					clientStatus.m_flInPacketsPerSec * 1e-3,
					clientStatus.m_flConnectionQualityLocal * 100.0f
				);
				usecLastPrint = usecNow;
			}

			// On the server, try to keep the buffer full at a certain amount
			if ( !bDrain )
			{
				while ( serverStatus.m_cbPendingReliable + 1024 < k_nBufferQueuedTarget )
				{
					// How much is missing?
					int cbSendMsg = std::min( k_nBufferQueuedTarget - serverStatus.m_cbPendingReliable, k_cbMaxSteamNetworkingSocketsMessageSizeSend );

					// Don't send tiny messages, just wait until we can queue up some more
					if ( cbSendMsg < 1024 )
						break;

					// Allocate a message.
					SteamNetworkingMessage_t *pSendMsg = SteamNetworkingUtils()->AllocateMessage( cbSendMsg );
					pSendMsg->m_conn = hServer;
					pSendMsg->m_nFlags = k_nSteamNetworkingSend_Reliable;
					// Don't bother initializing the body

					int64 nMsgNumberOrResult;
					SteamNetworkingSockets()->SendMessages( 1, &pSendMsg, &nMsgNumberOrResult );
					if ( nMsgNumberOrResult == -k_EResultLimitExceeded )
					{
						TEST_Printf( "SendMessage returned limit exceeded trying to queue %d + %d = %d\n", serverStatus.m_cbPendingReliable, cbSendMsg, serverStatus.m_cbPendingReliable + cbSendMsg );
						break;
					}
					assert( nMsgNumberOrResult > 0 );

					serverStatus.m_cbPendingReliable += cbSendMsg + 64;
					cbBytesSent += cbSendMsg;
				}
			}

			// On the client, we'll just periodically send small messages,
			// just to keep some traffic going in the other direction.  This
			// isn't necessary, but it's a slightly more realistic test
			if ( clientStatus.m_cbPendingReliable+clientStatus.m_cbSentUnackedReliable == 0 )
			{
				char dummyMsg[ 1024 ];
				EResult r = SteamNetworkingSockets()->SendMessageToConnection( hClient, dummyMsg, sizeof(dummyMsg), k_nSteamNetworkingSend_Reliable, nullptr );
				assert( k_EResultOK == r );
			}

			// Receive server->client messages
			SteamNetworkingMessage_t *pMsg[ 16 ];
			for (;;)
			{
				int nMsg = SteamNetworkingSockets()->ReceiveMessagesOnConnection( hClient, pMsg, 16 );
				if ( nMsg <= 0 )
				{
					assert( nMsg == 0 );
					break;
				}

				for ( int i = 0 ; i < nMsg ; ++i )
				{
					cbBytesRecv += pMsg[i]->m_cbSize;
					assert( cbBytesRecv <= cbBytesSent );
					pMsg[i]->Release();
				}

				if ( nMsg < 16 )
					break;
			}

			// Receive client->server messages
			for (;;)
			{
				int nMsg = SteamNetworkingSockets()->ReceiveMessagesOnConnection( hServer, pMsg, 16 );
				if ( nMsg <= 0 )
				{
					assert( nMsg == 0 );
					break;
				}

				for ( int i = 0 ; i < nMsg ; ++i )
					pMsg[i]->Release();

				if ( nMsg < 16 )
					break;
			}

			// Done?
			if ( bDrain && cbBytesRecv == cbBytesSent )
				break;
		}

		{
			SteamNetworkingMicroseconds usecNow = SteamNetworkingUtils()->GetLocalTimestamp();
			double flElapsedSeconds = ( usecNow - usecStartTime ) * 1e-6;
			TEST_Printf( "TOTAL:  %6.0fms   Sent:%7.0fK   Recv:%7.0fK = %5.0fK/sec\n\n",
				flElapsedSeconds * 1e3,
				cbBytesSent * 1e-3,
				cbBytesRecv * 1e-3,
				cbBytesRecv * 1e-3 / flElapsedSeconds
			);
		}

	}

	// Cleanup
	SteamNetworkingSockets()->CloseConnection( hServer, 0, nullptr, false );
	SteamNetworkingSockets()->CloseConnection( hClient, 0, nullptr, false );
}

int main( int argc, const char **argv  )
{
	typedef void (*FnTest)(void);
	struct Test_t {
		const char *m_pszName;
		FnTest m_func;
	};

	#define TEST(x) { #x, Test_ ## x }

	static const Test_t tests[] = {
		TEST(identity),
		TEST(quick),
		TEST(soak),
		TEST(netloopback_throughput),
		TEST(lane_quick_queueanddrain),
		TEST(lane_quick_priority_and_background)
	};

	struct Suite_t {
		const char *m_pszName;
		std::vector< Test_t > m_vecTests;
	};
	static const Suite_t test_suites[] = {
		{ "suite-quick", { TEST(identity), TEST(quick), TEST(lane_quick_queueanddrain), TEST(netloopback_throughput), TEST(lane_quick_priority_and_background) } }
	};

	if ( argc < 2 )
	{
print_usage:
		{
			const char *prog = argv[0];
			while ( strchr( prog, '/' ) )
				prog = strchr( prog, '/' ) + 1;
			while ( strchr( prog, '\\' ) )
				prog = strchr( prog, '\\' ) + 1;
			printf( "Usage: %s test-or-suite-name ...\n", prog );
		}
print_available_tests_and_exit:
		printf( "\n" );
		printf( "Available tests:\n" );
		for ( const Test_t &t: tests )
			printf( "    %s\n", t.m_pszName );
		printf( "Available test suites:\n" );
		for ( const Suite_t &s: test_suites )
			printf( "    %s\n", s.m_pszName );
		return 1;
	}

	std::vector<Test_t> vecTestsToRun;
	for ( int i = 1 ; i < argc ; ++i )
	{
		if ( !strcasecmp( argv[i], "/?" ) || !strcasecmp( argv[i], "-?" ) || !strcasecmp( argv[i], "/?" ) || !strcasecmp( argv[i], "-h" ) || !strcasecmp( argv[i], "--help" ) )
			goto print_usage;

		bool bFound = false;
		for ( const Test_t &t: tests )
		{
			if ( !strcasecmp( argv[i], t.m_pszName ) )
			{
				vecTestsToRun.push_back( t );
				bFound = true;
				break;
			}
		}
		for ( const Suite_t &s: test_suites )
		{
			if ( !strcasecmp( argv[i], s.m_pszName ) )
			{
				for ( const Test_t &t: s.m_vecTests )
					vecTestsToRun.push_back( t );
				bFound = true;
				break;
			}
		}
		if ( !bFound )
		{
			printf( "No such test or suite named '%s' not known\n", argv[i] );
			goto print_available_tests_and_exit;
		}
	}

	// Initialize library
	TEST_Init( nullptr );

	for ( const Test_t &t: vecTestsToRun )
	{
		TEST_Printf( "--------------------------------------\n");
		TEST_Printf( "Running test '%s'\n", t.m_pszName );
		TEST_Printf( "--------------------------------------\n");
		TEST_Printf( "\n");

		// Make sure each test starts with the default config
		ClearConfig();

		// Run the test
		(*t.m_func)();

		TEST_Printf( "\n" );
		TEST_Printf( "Test '%s' completed OK\n\n", t.m_pszName );
	}

	// Shutdown library
	TEST_Kill();	
	return 0;
}

#ifdef NN_NINTENDO_SDK
extern "C" void nnMain() { main( 0, nullptr ); }
#endif
