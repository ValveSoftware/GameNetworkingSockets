// High speed gateway relay for Syscoin Transactions

#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>
#include <queue>
#include <map>
#include <set>
#include <vector>
#include <cctype>

#include <steam/steamnetworkingsockets.h>
#include "crypto.h"
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#ifdef WIN32
	#include <windows.h> // Ug, for NukeProcess -- see below
#else
	#include <unistd.h>
	#include <signal.h>
#endif
#include <jsonrpccpp/client.h>
#include <jsonrpccpp/client/connectors/httpclient.h>
#include <czmq.h>
#include <stdint.h>
#include <inttypes.h>
using namespace jsonrpc;
using namespace std;

/////////////////////////////////////////////////////////////////////////////
//
// Common stuff
//
/////////////////////////////////////////////////////////////////////////////

bool g_bQuit = false;
bool g_bDebug = true;
SteamNetworkingMicroseconds g_logTimeZero;
std::vector<std::string> outgoingListPeers = {"127.0.0.1:1234"}; // Use IPv6 here in production! MTU of UDP @ IPv6 is 1280 bytes vs IPv4 which is 500 bytes
std::vector<std::string> incomingListPeers = {"127.0.0.1"};
std::string SyscoinCoreRPCURL = "http://u:p@localhost:18370";
std::string SyscoinCoreZMQURL = "tcp://127.0.0.1:28332";
// We do this because I won't want to figure out how to cleanly shut
// down the thread that is reading from stdin.
static void NukeProcess( int rc )
{
	#ifdef WIN32
		ExitProcess( rc );
	#else
		kill( getpid(), SIGKILL );
	#endif
}

static void DebugOutput( ESteamNetworkingSocketsDebugOutputType eType, const char *pszMsg )
{
	SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - g_logTimeZero;
	printf( "%10.6f %s\n", time*1e-6, pszMsg );
	fflush(stdout);
	if ( eType == k_ESteamNetworkingSocketsDebugOutputType_Bug )
	{
		fflush(stdout);
		fflush(stderr);
		NukeProcess(1);
	}
}

static void FatalError( const char *fmt, ... )
{
	char text[ 2048 ];
	va_list ap;
	va_start( ap, fmt );
	vsprintf( text, fmt, ap );
	va_end(ap);
	char *nl = strchr( text, '\0' ) - 1;
	if ( nl >= text && *nl == '\n' )
		*nl = '\0';
	DebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Bug, text );
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

static void InitSteamDatagramConnectionSockets(ISteamNetworkingSockets *pInterface)
{

	SteamDatagramErrMsg errMsg;
	pInterface = GameNetworkingSockets_Init( nullptr, errMsg );
	if ( !pInterface )
		FatalError( "GameNetworkingSockets_Init failed.  %s", errMsg );


	g_logTimeZero = SteamNetworkingUtils()->GetLocalTimestamp();

	SteamNetworkingUtils()->SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput );
}

static void ShutdownSteamDatagramConnectionSockets(ISteamNetworkingSockets *pInterface)
{
	// Give connections time to finish up.  This is an application layer protocol
	// here, it's not TCP.  Note that if you have an application and you need to be
	// more sure about cleanup, you won't be able to do this.  You will need to send
	// a message and then either wait for the peer to close the connection, or
	// you can pool the connection to see if any reliable data is pending.
	std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );

	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		GameNetworkingSockets_Kill(pInterface);
	#else
		SteamDatagramClient_Kill();
	#endif
}

/////////////////////////////////////////////////////////////////////////////
//
// Non-blocking console user input.  Sort of.
// Why is this so hard?
//
/////////////////////////////////////////////////////////////////////////////

std::mutex mutexUserInputQueue;
std::queue< std::string > queueUserInput;

std::thread *s_pThreadUserInput = nullptr;

void LocalUserInput_Init()
{
	s_pThreadUserInput = new std::thread( []()
	{
		while ( !g_bQuit )
		{
			char szLine[ 4000 ];
			if ( !fgets( szLine, sizeof(szLine), stdin ) )
			{
				// Well, you would hope that you could close the handle
				// from the other thread to trigger this.  Nope.
				if ( g_bQuit )
					return;
				g_bQuit = true;
				Printf( "Failed to read on stdin, quitting\n" );
				break;
			}

			mutexUserInputQueue.lock();
			queueUserInput.push( std::string( szLine ) );
			mutexUserInputQueue.unlock();
		}
	} );
}


// You really gotta wonder what kind of pedantic garbage was
// going through the minds of people who designed std::string
// that they decided not to include trim.
// https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring

// trim from start (in place)
static inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
        return !std::isspace(ch);
    }));
}

// trim from end (in place)
static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}


// Read the next line of input from stdin, if anything is available.
bool LocalUserInput_GetNext( std::string &result )
{
	bool got_input = false;
	mutexUserInputQueue.lock();
	while ( !queueUserInput.empty() && !got_input )
	{
		result = queueUserInput.front();
		queueUserInput.pop();
		ltrim(result);
		rtrim(result);
		got_input = !result.empty(); // ignore blank lines
	}
	mutexUserInputQueue.unlock();
	return got_input;
}
template<typename T>
std::string HexStr(const T itbegin, const T itend)
{
    std::string rv;
    static const char hexmap[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    rv.reserve(std::distance(itbegin, itend) * 2);
    for(T it = itbegin; it < itend; ++it)
    {
        unsigned char val = (unsigned char)(*it);
        rv.push_back(hexmap[val>>4]);
        rv.push_back(hexmap[val&15]);
    }
    return rv;
}

template<typename T>
inline std::string HexStr(const T& vch)
{
    return HexStr(vch.begin(), vch.end());
}
/////////////////////////////////////////////////////////////////////////////
//
// GatewayClient - Outgoing clients
//
/////////////////////////////////////////////////////////////////////////////

class GatewayClient : private ISteamNetworkingSocketsCallbacks
{
public:
	GatewayClient(const SteamNetworkingIPAddr &serverAddr){
		// Select instance to use.  For now we'll always use the default.
		InitSteamDatagramConnectionSockets(m_pInterface);
		// Start connecting
		char szAddr[ SteamNetworkingIPAddr::k_cchMaxString ];
		serverAddr.ToString( szAddr, sizeof(szAddr), true );
		Printf( "Connecting to gateway server at %s", szAddr );
		m_hConnection = m_pInterface->ConnectByIPAddress( serverAddr, 0, nullptr );
	}
	virtual ~GatewayClient()
	{
		Printf( "Closing Gatewayclient...\n");
		// Close the connection.  We use "linger mode" to ask SteamNetworkingSockets
		// to flush this out and close gracefully.
		m_pInterface->CloseConnection( m_hConnection, 0, "Server Shutdown", false );
		m_hConnection = k_HSteamNetConnection_Invalid;
		ShutdownSteamDatagramConnectionSockets(m_pInterface);
	}
	void Run( )
	{
		while ( !g_bQuit )
		{
			PollIncomingMessages();
			PollConnectionStateChanges();
			std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		}
	}
	void PollIncomingMessages()
	{
		while ( !g_bQuit )
		{
			ISteamNetworkingMessage *pIncomingMsg = nullptr;
			int numMsgs = m_pInterface->ReceiveMessagesOnConnection( m_hConnection, &pIncomingMsg, 1 );
			if ( numMsgs <= 0 )
				break;
			

			// Just echo anything we get from the server
			fwrite( pIncomingMsg->m_pData, 1, pIncomingMsg->m_cbSize, stdout );
			fputc( '\n', stdout );

			// We don't need this anymore.
			pIncomingMsg->Release();
		}
	}
	void SendMessageToClient(const void *data, size_t size, HSteamNetConnection except = k_HSteamNetConnection_Invalid)
	{
		if(m_hConnection != except)
			m_pInterface->SendMessageToConnection( m_hConnection, data, (uint32)size, k_nSteamNetworkingSend_UnreliableNoDelay, nullptr );
	}
	HSteamNetConnection m_hConnection;
	ISteamNetworkingSockets *m_pInterface;
private:


	void PollConnectionStateChanges()
	{
		m_pInterface->RunCallbacks( this );
	}

	virtual void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *pInfo ) override
	{
		// What's the state of the connection?
		switch ( pInfo->m_info.m_eState )
		{
			case k_ESteamNetworkingConnectionState_None:
				// NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
				break;

			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			{
				// Print an appropriate message
				if ( pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting )
				{
					// Note: we could distinguish between a timeout, a rejected connection,
					// or some other transport problem.
					Printf( "We sought the remote host, yet our efforts were met with defeat.  (%s)", pInfo->m_info.m_szEndDebug );
				}
				else if ( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
				{
					Printf( "Alas, troubles beset us; we have lost contact with the host.  (%s)", pInfo->m_info.m_szEndDebug );
				}
				else
				{
					// NOTE: We could check the reason code for a normal disconnection
					Printf( "The host hath bidden us farewell.  (%s)", pInfo->m_info.m_szEndDebug );
				}

				// Clean up the connection.  This is important!
				// The connection is "closed" in the network sense, but
				// it has not been destroyed.  We must close it on our end, too
				// to finish up.  The reason information do not matter in this case,
				// and we cannot linger because it's already closed on the other end,
				// so we just pass 0's.
				if(m_hConnection != k_HSteamNetConnection_Invalid)
				{
					m_pInterface->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
					m_hConnection = k_HSteamNetConnection_Invalid;
				}
				break;
			}

			case k_ESteamNetworkingConnectionState_Connecting:
				Printf( "Client Connection request %s", pInfo->m_info.m_szConnectionDescription );
				// We will get this callback when we start connecting.
				// We can ignore this.
				break;

			case k_ESteamNetworkingConnectionState_Connected:
				Printf( "Connected to server OK" );
				break;

			default:
				// Silences -Wswitch
				break;
		}
	}
};
/////////////////////////////////////////////////////////////////////////////
//
// GatewayServer
//
/////////////////////////////////////////////////////////////////////////////

class GatewayServer : private ISteamNetworkingSocketsCallbacks
{
public:
	// efficient implementation of subscribing to zmq publisher from Syscoin Core
	// it does not allocate memory for topic/message, passes it straight through to SendMessageToAllOutgoingClients for tx
	// and for block we just need indication it was a new block so we can clear incoming message hashes that are received tx's from other gateway peers
	// note we use memcmp to do a comparison check of the topic, because we cast the frame data to char * we can do this, the czmq call's all allocate objects
	// which need to be free'd making them less efficient than simply getting pointers. Note that SendMessageToAllOutgoingClients will internally copy and allocate data
	// before it puts it on the wire for UDP, so in entire process there should only be two allocations, zmsg_t here (which is free'd by zmsg_destroy) and the message data inside of the GameNetworkingSockets library
	// Note: We don't use libzmq (which is C++) because that library throws exceptions and we explicitely compile GameNetworkingSockets with -fno-exceptions for optimization
	void ReadFromCore()
	{
		if(g_bDebug)
			Printf( "ReadFromCore: Setting up ZMQ\n" );
		zsock_t *socket = zsock_new_sub(SyscoinCoreZMQURL.c_str(), NULL);
  		assert(socket);
		zsock_set_subscribe(socket, "rawmempooltx");
		zsock_set_subscribe(socket, "hashblock");
		zsock_set_unbounded(socket); // hwm = 0
		if(g_bDebug)
			Printf( "ReadFromCore: Setup complete\n" );
		while( !g_bQuit )
		{
			zmsg_t *msg = zmsg_recv (socket);
			assert(msg);
			 //  Filter a signal that may come from a dying actor, taken from czmq zsock_recv() which is a helper but allocates objects we can get around using zmq directly with C
			if (zmsg_signal (msg) >= 0) {
				zmsg_destroy (&msg);
				if(g_bDebug)
					Printf( "ReadFromCore: Dying actor, quitting zmq loop...\n" );
				break;
			}
			zframe_t *frame = zmsg_first (msg);
			char *topic = (char*)zframe_data (frame);
			frame = zmsg_next (msg);
			void *pData = zframe_data (frame);
			size_t size = zframe_size (frame);
			if(memcmp(topic, "rawmempooltx", 12) == 0)
			{
				if(g_bDebug)
					Printf( "ReadFromCore: Received mempool tx in bytes %d, relaying to all outgoing clients\n", size );
				SendMessageToAllOutgoingClients(pData, size);	
			}
			else if(memcmp(topic, "hashblock", 9) == 0)
			{
				if(g_bDebug)
					Printf( "ReadFromCore: Received blockhash in bytes %d\n", size );
				ClearIncomingHashes();
			}
			zmsg_destroy (&msg);
		}
		zsock_destroy(&socket);
	}
	void PushToCore()
	{
		if(g_bDebug && !m_vecMessagesIncomingBuffer.empty())
			Printf( "PushToCore: Pushing %d inventory items to Syscoin Core\n", m_vecMessagesIncomingBuffer.size());
		// batch process call to Syscoin Core to send and accept transaction in mempool
		BatchCall bc;
		bool added = false;
		for(ISteamNetworkingMessage* message: m_vecMessagesIncomingBuffer){
			Json::Value param;
			const unsigned char* msgbuf = reinterpret_cast<unsigned char*>(message->m_pData); 
			std::vector<unsigned char> vec(msgbuf, msgbuf+message->m_cbSize);
  			param["hexstring"] = HexStr(vec);
			bc.addCall("sendrawtransaction", param, false);
			// release memory
			message->Release();
			added = true;
		}
		if(added && m_rpcClient != NULL){
			BatchResponse response = m_rpcClient->CallProcedures(bc);
			if(g_bDebug)
				Printf( "PushToCore: batch call error: %d err: %s\n", response.hasErrors()? 1:0, response.getErrorMessage(1).c_str());
		}
		if(g_bDebug && !m_vecMessagesIncomingBuffer.empty())
			Printf( "PushToCore: Done\n");
		m_vecMessagesIncomingBuffer.clear();
	}
	void ClearIncomingHashes()
	{
		m_blockCount++;
		// erase entries atleast 5 blocks old to keep map small
		for (auto it = m_mapIncomingMessageHashes.cbegin(); it != m_mapIncomingMessageHashes.cend() /* not hoisted */; /* no increment */)
		{
			if(m_blockCount - it->second >= 5)
			{
				if(g_bDebug)
					Printf( "ClearIncomingHashes: Removing hash %s that was %d blocks hold\n", HexStr(it->first).c_str(), m_blockCount - it->second);
				it = m_mapIncomingMessageHashes.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
	void StartGatewayThreads()
	{
		if(m_rpcClient != NULL)
		{
			Printf( "Gateway already started!\n");
			return;
		}
		HttpClient client(SyscoinCoreRPCURL);
  		m_rpcClient = new Client(client, jsonrpc::JSONRPC_CLIENT_V1);
		Printf( "Syscoin RPC client on %s\n" , SyscoinCoreRPCURL.c_str());
		// parse outgoing peer list, for relays incoming messages from Syscoin Core or from incoming peer
		std::set< std::string > setOutgoingWhitelist;
		for(const auto& peer: outgoingListPeers){
			setOutgoingWhitelist.insert(peer);
		}
		
		for ( const auto &addr: setOutgoingWhitelist )
		{
			SteamNetworkingIPAddr addrObj;
			if(!addrObj.ParseString(addr.c_str())){
				if(g_bDebug)
					Printf( "Could not parse outgoing peer %s\n" , addr.c_str());
				continue;
			}
			GatewayClient *client = new GatewayClient(addrObj);
			if ( client->m_hConnection == k_HSteamNetConnection_Invalid )
				FatalError( "Failed to create connection" );
			m_setOutgoingClients.insert(client); 
			if(g_bDebug)
				Printf( "Starting client thread for %s\n" , addr.c_str());
			std::thread t(&GatewayClient::Run, client);
			t.detach();
			if(g_bDebug)
				Printf( "Started client thread for %s\n" , addr.c_str());
		}
		if(g_bDebug)
			Printf( "Starting ZMQ thread\n");		
		std::thread t(&GatewayServer::ReadFromCore, this);
		t.detach();
		if(g_bDebug)
			Printf( "Started ZMQ thread\n");
	}
	void Run( uint16 nPort )
	{
		m_blockCount = 0;
		m_rpcClient = NULL;

		InitSteamDatagramConnectionSockets(m_pInterface);

		// Start listening
		m_serverLocalAddr.Clear();
		m_serverLocalAddr.m_port = nPort;
		
		m_hListenSock = m_pInterface->CreateListenSocketIP( m_serverLocalAddr, 0, nullptr );
		if ( m_hListenSock == k_HSteamListenSocket_Invalid )
			FatalError( "Failed to listen on port %d", nPort );
		m_hPollGroup = m_pInterface->CreatePollGroup();
		if ( m_hPollGroup == k_HSteamNetPollGroup_Invalid )
			FatalError( "Failed to listen on port %d", nPort );
		Printf( "Server listening on port %d\n", nPort );
		// parse incoming peer list and save it to whitelist for allowed peers to connect to this server
		for(const auto& peer: incomingListPeers){
			SteamNetworkingIPAddr addrObj;
			if(!addrObj.ParseString(peer.c_str())){
				if(g_bDebug)
					Printf( "Could not parse incoming peer %s\n" , peer.c_str());
				continue;
			}
			char szAddr[ SteamNetworkingIPAddr::k_cchMaxString ];
			addrObj.ToString(szAddr, sizeof(szAddr), false);
			m_setIncomingWhitelist.insert(std::string(szAddr));
		}
		while ( !g_bQuit )
		{
			PollIncomingMessages();
			PollConnectionStateChanges();
			PushToCore();
			PollLocalUserInput();
			std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		}

		// Close all the connections
		Printf( "Closing connections...\n" );
		for ( auto it: m_mapIncomingClients )
		{
			// Send them one more goodbye message.  Note that we also have the
			// connection close reason as a place to send final data.  However,
			// that's usually best left for more diagnostic/debug text not actual
			// protocol strings.
			SendStringToClient( it.first, "Server is shutting down.  Goodbye." );

			// Close the connection.  We use "linger mode" to ask SteamNetworkingSockets
			// to flush this out and close gracefully.
			m_pInterface->CloseConnection( it.first, 0, "Server Shutdown", true );
		}
		if(g_bDebug)
			Printf( "Shutdown outgoing connections\n");
		for ( auto *c: m_setOutgoingClients )
		{
			delete c;
			c = NULL;
		}
		if(g_bDebug)
			Printf( "Close sockets and clean up memory\n");		
		m_mapIncomingClients.clear();
		m_setOutgoingClients.clear();
		m_setIncomingWhitelist.clear();

		m_pInterface->CloseListenSocket( m_hListenSock );
		m_hListenSock = k_HSteamListenSocket_Invalid;

		m_pInterface->DestroyPollGroup( m_hPollGroup );
		m_hPollGroup = k_HSteamNetPollGroup_Invalid;

		delete m_rpcClient;
		m_rpcClient = NULL;
		ShutdownSteamDatagramConnectionSockets(m_pInterface);
	}
private:
	Client *m_rpcClient;
	HSteamListenSocket m_hListenSock;
	HSteamNetPollGroup m_hPollGroup;
	ISteamNetworkingSockets *m_pInterface;
	uint32 m_blockCount;
	SteamNetworkingIPAddr m_serverLocalAddr;
	struct Client_t
	{
		std::string m_sNick;
	};

	std::map< HSteamNetConnection, Client_t > m_mapIncomingClients;
	std::set< GatewayClient*> m_setOutgoingClients;
	// who's allowed to connect to you and send this server messages?
	std::set< std::string > m_setIncomingWhitelist;
	// force unique messages before relaying to outgoing or processing to Syscoin Core
	std::map< std::vector<unsigned char>, uint32 > m_mapIncomingMessageHashes;
	std::vector<ISteamNetworkingMessage *> m_vecMessagesIncomingBuffer;


	void SendStringToClient( HSteamNetConnection conn, const char *str )
	{
		m_pInterface->SendMessageToConnection( conn, str, (uint32)strlen(str), k_nSteamNetworkingSend_Reliable, nullptr );
	}
	void SendStringToAllIncomingClients( const char *str, HSteamNetConnection except = k_HSteamNetConnection_Invalid )
	{
		for ( auto &c: m_mapIncomingClients )
		{
			if ( c.first != except )
				SendStringToClient( c.first, str );
		}
	}
	void SendMessageToAllOutgoingClients( const void *data, size_t size, HSteamNetConnection except = k_HSteamNetConnection_Invalid )
	{
		for ( auto *c: m_setOutgoingClients )
		{
			if(c != NULL)
				c->SendMessageToClient(data, size, except);
		}
	}

	void PollIncomingMessages()
	{
		while ( !g_bQuit )
		{
			ISteamNetworkingMessage *pIncomingMsg = nullptr;
			int numMsgs = m_pInterface->ReceiveMessagesOnPollGroup( m_hPollGroup, &pIncomingMsg, 1 );
			if ( numMsgs == 0 )
				break;
			if ( numMsgs < 0 )
				FatalError( "Error checking for messages" );
			assert( numMsgs == 1 && pIncomingMsg );
			assert( m_mapIncomingClients.find( pIncomingMsg->m_conn ) != m_mapIncomingClients.end() );
		
			SHA256Digest_t digest;
			CCrypto::GenerateSHA256Digest( pIncomingMsg->m_pData, (size_t)pIncomingMsg->m_cbSize, &digest );
			std::vector<unsigned char> vec(digest, digest+sizeof(digest));
			if(g_bDebug)
				Printf( "PollIncomingMessages: Received inventory of %d bytes, hash %s\n", pIncomingMsg->m_cbSize, HexStr(vec).c_str());
			auto ret = m_mapIncomingMessageHashes.emplace(std::move(vec), m_blockCount);
			if (!ret.second){
				// message already exists
				if(g_bDebug)
					Printf( "PollIncomingMessages: Duplicate inventory hash %s\n", HexStr(vec).c_str());
				continue;
			}
	
			if(g_bDebug)
				Printf( "PollIncomingMessages: Sending inventory to all outgoing clients\n");
			// sends to outgoing peers, queue up on the wire as fast as possible
			SendMessageToAllOutgoingClients( pIncomingMsg->m_pData,  (size_t)pIncomingMsg->m_cbSize, pIncomingMsg->m_conn  );
			
			m_vecMessagesIncomingBuffer.emplace_back(pIncomingMsg);
			
		}
	}

	void PollConnectionStateChanges()
	{
		m_pInterface->RunCallbacks( this );
	}

	void PollLocalUserInput()
	{
		std::string cmd;
		while ( !g_bQuit && LocalUserInput_GetNext( cmd ))
		{
			if ( strcmp( cmd.c_str(), "/quit" ) == 0 )
			{
				g_bQuit = true;
				Printf( "Shutting down server" );
				break;
			}
			else if ( strcmp( cmd.c_str(), "/connect" ) == 0 )
			{
				StartGatewayThreads();
				break;
			}

			// That's the only command we support
			Printf( "The server only knows two commands: '/quit' or '/connect" );
		}
	}

	void SetClientNick( HSteamNetConnection hConn, std::map< HSteamNetConnection, Client_t >& mapClient, const char *nick )
	{

		// Remember their nick
		mapClient[hConn].m_sNick = nick;

		// Set the connection name, too, which is useful for debugging
		m_pInterface->SetConnectionName( hConn, nick );
	}

	virtual void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *pInfo ) override
	{
		char temp[1024];

		// What's the state of the connection?
		switch ( pInfo->m_info.m_eState )
		{
			case k_ESteamNetworkingConnectionState_None:
				// NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
				break;

			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			{
				// Ignore if they were not previously connected.  (If they disconnected
				// before we accepted the connection.)
				if ( pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected )
				{

					// Locate the client.  Note that it should have been found, because this
					// is the only codepath where we remove clients (except on shutdown),
					// and connection change callbacks are dispatched in queue order.
					auto itClient = m_mapIncomingClients.find( pInfo->m_hConn );
					assert( itClient != m_mapIncomingClients.end() );

					// Select appropriate log messages
					const char *pszDebugLogAction;
					if ( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
					{
						pszDebugLogAction = "problem detected locally";
						sprintf( temp, "Alas, %s hath fallen into shadow.  (%s)", itClient->second.m_sNick.c_str(), pInfo->m_info.m_szEndDebug );
					}
					else
					{
						// Note that here we could check the reason code to see if
						// it was a "usual" connection or an "unusual" one.
						pszDebugLogAction = "closed by peer";
						sprintf( temp, "%s hath departed", itClient->second.m_sNick.c_str() );
					}

					// Spew something to our own log.  Note that because we put their nick
					// as the connection description, it will show up, along with their
					// transport-specific data (e.g. their IP address)
					Printf( "Connection %s %s, reason %d: %s\n",
						pInfo->m_info.m_szConnectionDescription,
						pszDebugLogAction,
						pInfo->m_info.m_eEndReason,
						pInfo->m_info.m_szEndDebug
					);

					m_mapIncomingClients.erase( itClient );

					// Send a message so everybody else knows what happened
					SendStringToAllIncomingClients( temp, pInfo->m_hConn );
				}
				else
				{
					assert( pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting );
				}

				// Clean up the connection.  This is important!
				// The connection is "closed" in the network sense, but
				// it has not been destroyed.  We must close it on our end, too
				// to finish up.  The reason information do not matter in this case,
				// and we cannot linger because it's already closed on the other end,
				// so we just pass 0's.
				m_pInterface->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
				break;
			}

			case k_ESteamNetworkingConnectionState_Connecting:
			{
				// This must be a new connection
				assert( m_mapIncomingClients.find( pInfo->m_hConn ) == m_mapIncomingClients.end() );

				// if not in our whitelist we close connection
				char szAddr[ SteamNetworkingIPAddr::k_cchMaxString ];
				pInfo->m_info.m_addrRemote.ToString(szAddr, sizeof(szAddr), false);
				if(m_setIncomingWhitelist.find( std::string(szAddr) ) == m_setIncomingWhitelist.end())
				{
					m_pInterface->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
					Printf( "Can't accept connection %s.  Not in whitelist...", szAddr ); 
					break;
				}
				if(pInfo->m_info.m_addrRemote == m_serverLocalAddr)
				{
					m_pInterface->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
					Printf( "Can't accept connection from yourself" ); 
					break;	
				}
				Printf( "Connection request from %s", pInfo->m_info.m_szConnectionDescription );

				// A client is attempting to connect
				// Try to accept the connection.
				if ( m_pInterface->AcceptConnection( pInfo->m_hConn ) != k_EResultOK )
				{
					// This could fail.  If the remote host tried to connect, but then
					// disconnected, the connection may already be half closed.  Just
					// destroy whatever we have on our side.
					m_pInterface->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
					Printf( "Can't accept connection.  (It was already closed?)" );
					break;
				}

				// Assign the poll group
				if ( !m_pInterface->SetConnectionPollGroup( pInfo->m_hConn, m_hPollGroup ) )
				{
					m_pInterface->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
					Printf( "Failed to set poll group?" );
					break;
				}

				// Send them a welcome message
				sprintf( temp, "Welcome, stranger.  Thou art known to us for now as '%s'; upon thine command '/nick' we shall know thee otherwise.", pInfo->m_info.m_szConnectionDescription ); 
				SendStringToClient( pInfo->m_hConn, temp ); 

				// Also send them a list of everybody who is already connected
				if ( m_mapIncomingClients.empty() )
				{
					SendStringToClient( pInfo->m_hConn, "Thou art utterly alone." ); 
				}
				else
				{
					sprintf( temp, "%d companions greet you:", (int)m_mapIncomingClients.size() ); 
					SendStringToClient( pInfo->m_hConn, temp ); 
					for ( auto &c: m_mapIncomingClients )
						SendStringToClient( pInfo->m_hConn, c.second.m_sNick.c_str() ); 
				}

				// Let everybody else know who they are for now
				sprintf( temp, "Hark!  A stranger hath joined this merry host.  For now we shall call them '%s'", pInfo->m_info.m_szConnectionDescription ); 
				SendStringToAllIncomingClients( temp, pInfo->m_hConn ); 

				// Add them to the client list, using std::map wacky syntax
				m_mapIncomingClients[ pInfo->m_hConn ];
				SetClientNick( pInfo->m_hConn, m_mapIncomingClients, pInfo->m_info.m_szConnectionDescription );
				break;
			}

			case k_ESteamNetworkingConnectionState_Connected:
				// We will get a callback immediately after accepting the connection.
				// Since we are the server, we can ignore this, it's not news to us.
				break;

			default:
				// Silences -Wswitch
				break;
		}
	}
};


const uint16 DEFAULT_SERVER_PORT = 27020;

void PrintUsageAndExit( int rc = 1 )
{
	fflush(stderr);
	printf(
R"usage(Usage:
    gateway server [--port PORT]
)usage"
	);
	fflush(stdout);
	exit(rc);
}

int main( int argc, const char *argv[] )
{
	int nPort = DEFAULT_SERVER_PORT;
	if(g_bDebug)
		Printf( "Starting server in Debug mode\n" );
	for ( int i = 1 ; i < argc ; ++i )
	{
		if ( !strcmp( argv[i], "--port" ) )
		{
			++i;
			if ( i >= argc )
				PrintUsageAndExit();
			nPort = atoi( argv[i] );
			if ( nPort <= 0 || nPort > 65535 )
				FatalError( "Invalid port %d", nPort );
			continue;
		}

		PrintUsageAndExit();
	}

	// Create client and server sockets
	LocalUserInput_Init();
	if(g_bDebug)
		Printf( "Trying to run server\n" );
	GatewayServer server;
	server.Run( (uint16)nPort );
	if(g_bDebug)
		Printf( "Shutting down...\n" );
		
	NukeProcess(0);
}
