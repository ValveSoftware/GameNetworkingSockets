// Minimal end-to-end smoke test for the iOS build of GameNetworkingSockets:
// stands up a listen socket on loopback, connects to it, and round-trips a
// message.  Exercises crypto, protobuf, the socket thread and SNP.
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

static HSteamListenSocket g_listen = k_HSteamListenSocket_Invalid;
static HSteamNetConnection g_server = k_HSteamNetConnection_Invalid;
static HSteamNetConnection g_client = k_HSteamNetConnection_Invalid;
static bool g_clientConnected = false;

static void DebugOutput( ESteamNetworkingSocketsDebugOutputType t, const char *msg )
{
	printf( "[gns %d] %s\n", (int)t, msg );
}

static void OnStatusChanged( SteamNetConnectionStatusChangedCallback_t *info )
{
	printf( "conn %u: %d -> %d (%s)\n", info->m_hConn, (int)info->m_eOldState,
		(int)info->m_info.m_eState, info->m_info.m_szEndDebug );
	if ( info->m_info.m_hListenSocket == g_listen &&
		 info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting )
	{
		g_server = info->m_hConn;
		SteamNetworkingSockets()->AcceptConnection( info->m_hConn );
	}
	if ( info->m_hConn == g_client &&
		 info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected )
		g_clientConnected = true;
}

int main()
{
	SteamNetworkingErrMsg err;
	if ( !GameNetworkingSockets_Init( nullptr, err ) )
	{
		printf( "FAIL: GameNetworkingSockets_Init: %s\n", err );
		return 1;
	}
	SteamNetworkingUtils()->SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput );
	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged( OnStatusChanged );

	SteamNetworkingIPAddr addr;
	addr.Clear();
	addr.SetIPv4( 0x7f000001, 27789 );

	g_listen = SteamNetworkingSockets()->CreateListenSocketIP( addr, 0, nullptr );
	if ( g_listen == k_HSteamListenSocket_Invalid ) { printf( "FAIL: CreateListenSocketIP\n" ); return 1; }

	g_client = SteamNetworkingSockets()->ConnectByIPAddress( addr, 0, nullptr );
	if ( g_client == k_HSteamNetConnection_Invalid ) { printf( "FAIL: ConnectByIPAddress\n" ); return 1; }

	for ( int i = 0; i < 500 && !g_clientConnected; ++i )
	{
		SteamNetworkingSockets()->RunCallbacks();
		usleep( 10 * 1000 );
	}
	if ( !g_clientConnected ) { printf( "FAIL: never connected\n" ); return 1; }
	printf( "PASS: encrypted connection established over loopback\n" );

	const char kMsg[] = "hello from the iOS simulator";
	SteamNetworkingSockets()->SendMessageToConnection( g_client, kMsg, sizeof(kMsg),
		k_nSteamNetworkingSend_Reliable, nullptr );

	bool got = false;
	for ( int i = 0; i < 500 && !got; ++i )
	{
		SteamNetworkingSockets()->RunCallbacks();
		SteamNetworkingMessage_t *pMsg = nullptr;
		if ( SteamNetworkingSockets()->ReceiveMessagesOnConnection( g_server, &pMsg, 1 ) == 1 )
		{
			got = ( pMsg->m_cbSize == (int)sizeof(kMsg) && memcmp( pMsg->m_pData, kMsg, sizeof(kMsg) ) == 0 );
			printf( "received %d bytes: '%.*s'\n", pMsg->m_cbSize, pMsg->m_cbSize, (const char *)pMsg->m_pData );
			pMsg->Release();
		}
		usleep( 10 * 1000 );
	}
	if ( !got ) { printf( "FAIL: message never arrived intact\n" ); return 1; }
	printf( "PASS: reliable message round-tripped\n" );

	GameNetworkingSockets_Kill();
	printf( "ALL PASS\n" );
	return 0;
}
