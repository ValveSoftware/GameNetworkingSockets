//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Networking API similar to Berkeley sockets, but for games.
// - connection-oriented API (like TCP, not UDP)
// - but unlike TCP, it's message-oriented, not stream-oriented
// - mix of reliable and unreliable messages
// - fragmentation and reassembly
// - Supports connectivity over plain UDPv4
// - Also supports SDR ("Steam Datagram Relay") connections, which are
//   addressed by SteamID.  There is a "P2P" use case and also a "hosted
//   dedicated server" use case.
//
//=============================================================================

#ifndef ISTEAMNETWORKINGSOCKETS
#define ISTEAMNETWORKINGSOCKETS
#ifdef _WIN32
#pragma once
#endif

#include "steamnetworkingtypes.h"

class ISteamNetworkingSocketsCallbacks;

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
enum { k_iSteamNetworkingCallbacks = 1200 };
#else
// #KLUDGE! This is so we don't have to link with steam_api.lib
#include <steam/steam_api.h>
#include <steam/steam_gameserver.h>
#endif

//-----------------------------------------------------------------------------
/// Lower level networking interface that more closely mirrors the standard
/// Berkeley sockets model.  Sockets are hard!  You should probably only use
/// this interface under the existing circumstances:
///
/// - You have an existing socket-based codebase you want to port, or coexist with.
/// - You want to be able to connect based on IP address, rather than (just) Steam ID.
/// - You need low-level control of bandwidth utilization, when to drop packets, etc.
///
/// Note that neither of the terms "connection" and "socket" will correspond
/// one-to-one with an underlying UDP socket.  An attempt has been made to
/// keep the semantics as similar to the standard socket model when appropriate,
/// but some deviations do exist.
class ISteamNetworkingSockets
{
public:

	/// Creates a "server" socket that listens for clients to connect to, either by calling
	/// ConnectSocketBySteamID or ConnectSocketByIPv4Address.
	///
	/// nSteamConnectVirtualPort specifies how clients can connect to this socket using
	/// ConnectBySteamID.  A negative value indicates that this functionality is
	/// disabled and clients must connect by IP address.  It's very common for applications
	/// to only have one listening socket; in that case, use zero.  If you need to open
	/// multiple listen sockets and have clients be able to connect to one or the other, then
	/// nSteamConnectVirtualPort should be a small integer constant unique to each listen socket
	/// you create.
	///
	/// In the open-source version of this API, you must pass -1 for nSteamConnectVirtualPort
	///
	/// If you want clients to connect to you by your IPv4 addresses using
	/// ConnectByIPv4Address, then you must set nPort to be nonzero.  Steam will
	/// bind a UDP socket to the specified local port, and clients will send packets using
	/// ordinary IP routing.  It's up to you to take care of NAT, protecting your server
	/// from DoS, etc.  If you don't need clients to connect to you by IP, then set nPort=0.
	/// Use nIP if you wish to bind to a particular local interface.  Typically you will use 0,
	/// which means to listen on all interfaces, and accept the default outbound IP address.
	/// If nPort is zero, then nIP must also be zero.
	///
	/// A SocketStatusCallback_t callback when another client attempts a connection.
	virtual HSteamListenSocket CreateListenSocket( int nSteamConnectVirtualPort, uint32 nIP, uint16 nPort ) = 0;

	/// Creates a connection and begins talking to a remote destination.  The remote host
	/// must be listening with a matching call to CreateListenSocket.
	///
	/// Use ConnectBySteamID to connect using the SteamID (client or game server) as the network address.
	/// Use ConnectByIPv4Address to connect by IP address.
	///
	/// A SteamNetConnectionStatusChangedCallback_t callback will be triggered when we start connecting,
	/// and then another one on timeout or successful connection
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
	virtual HSteamNetConnection ConnectBySteamID( CSteamID steamIDTarget, int nVirtualPort ) = 0;
#endif
	virtual HSteamNetConnection ConnectByIPv4Address( uint32 nIP, uint16 nPort ) = 0;

	/// Accept an incoming connection that has been received on a listen socket.
	///
	/// When a connection attempt is received (perhaps after a few basic handshake
	/// packets have been exchanged to prevent trivial spoofing), a connection interface
	/// object is created in the k_ESteamNetworkingConnectionState_Connecting state
	/// and a SteamNetConnectionStatusChangedCallback_t is posted.  At this point, your
	/// application MUST either accept or close the connection.  (It may not ignore it.)
	/// Accepting the connection will transition it either into the connected state,
	/// of the finding route state, depending on the connection type.
	///
	/// You should take action within a second or two, because accepting the connection is
	/// what actually sends the reply notifying the client that they are connected.  If you
	/// delay taking action, from the client's perspective it is the same as the network
	/// being unresponsive, and the client may timeout the connection attempt.  In other
	/// words, the client cannot distinguish between a delay caused by network problems
	/// and a delay caused by the application.
	///
	/// This means that if your application goes for more than a few seconds without
	/// processing callbacks (for example, while loading a map), then there is a chance
	/// that a client may attempt to connect in that interval and fail due to timeout.
	///
	/// If the application does not respond to the connection attempt in a timely manner,
	/// and we stop receiving communication from the client, the connection attempt will
	/// be timed out locally, transitioning the connection to the
	/// k_ESteamNetworkingConnectionState_ProblemDetectedLocally state.  The client may also
	/// close the connection before it is accepted, and a transition to the
	/// k_ESteamNetworkingConnectionState_ClosedByPeer is also possible depending the exact
	/// sequence of events.
	///
	/// Returns k_EResultInvalidParam if the handle is invalid.
	/// Returns k_EResultInvalidState if the connection is not in the appropriate state.
	/// (Remember that the connection state could change in between the time that the
	/// notification being posted to the queue and when it is received by the application.)
	virtual EResult AcceptConnection( HSteamNetConnection hConn ) = 0;

	/// Disconnects from the remote host and invalidates the connection handle.
	/// Any unread data on the connection is discarded.
	///
	/// nReason is an application defined code that will be received on the other
	/// end and recorded (when possible) in backend analytics.  The value should
	/// come from a restricted range.  (See ESteamNetConnectionEnd.)  If you don't need
	/// to communicate any information to the remote host, and do not want analytics to
	/// be able to distinguish "normal" connection terminations from "exceptional" ones,
	/// You may pass zero, in which case the generic value of
	/// k_ESteamNetConnectionEnd_App_Generic will be used.
	///
	/// pszDebug is an optional human-readable diagnostic string that will be received
	/// by the remote host and recorded (when possible) in backend analytics.
	///
	/// If you wish to put the socket into a "linger" state, where an attempt is made to
	/// flush any remaining sent data, use bEnableLinger=true.  Otherwise reliable data
	/// is not flushed.
	///
	/// If the connection has already ended and you are just freeing up the
	/// connection interface, the reason code, debug string, and linger flag are
	/// ignored.
	virtual bool CloseConnection( HSteamNetConnection hPeer, int nReason, const char *pszDebug, bool bEnableLinger ) = 0;

	/// Destroy a listen socket, and all the client sockets generated by accepting connections
	/// on the listen socket.
	///
	/// pszNotifyRemoteReason determines what cleanup actions are performed on the client
	/// sockets being destroyed.  (See DestroySocket for more details.)
	///
	/// Note that if cleanup is requested and you have requested the listen socket bound to a
	/// particular local port to facilitate direct UDP/IPv4 connections, then the underlying UDP
	/// socket must remain open until all clients have been cleaned up.
	virtual bool CloseListenSocket( HSteamListenSocket hSocket, const char *pszNotifyRemoteReason ) = 0;

	/// Set connection user data.  Returns false if the handle is invalid.
	virtual bool SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData ) = 0;

	/// Fetch connection user data.  Returns -1 if handle is invalid
	/// or if you haven't set any userdata on the connection.
	virtual int64 GetConnectionUserData( HSteamNetConnection hPeer ) = 0;

	/// Set a name for the connection, used mostly for debugging
	virtual void SetConnectionName( HSteamNetConnection hPeer, const char *pszName ) = 0;

	/// Fetch connection name.  Returns false if handle is invalid
	virtual bool GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen ) = 0;

	/// Send a message to the remote host on the connected socket.
	///
	/// eSendType determines the delivery guarantees that will be provided,
	/// when data should be buffered, etc.
	///
	/// Note that the semantics we use for messages are not precisely
	/// the same as the semantics of a standard "stream" socket.
	/// (SOCK_STREAM)  For an ordinary stream socket, the boundaries
	/// between chunks are not considered relevant, and the sizes of
	/// the chunks of data written will not necessarily match up to
	/// the sizes of the chunks that are returned by the reads on
	/// the other end.  The remote host might read a partial chunk,
	/// or chunks might be coalesced.  For the message semantics 
	/// used here, however, the sizes WILL match.  Each send call 
	/// will match a successful read call on the remote host 
	/// one-for-one.  If you are porting existing stream-oriented 
	/// code to the semantics of reliable messages, your code should 
	/// work the same, since reliable message semantics are more 
	/// strict than stream semantics.  The only caveat is related to 
	/// performance: there is per-message overhead to retain the 
	/// messages sizes, and so if your code sends many small chunks 
	/// of data, performance will suffer. Any code based on stream 
	/// sockets that does not write excessively small chunks will 
	/// work without any changes. 
	virtual EResult SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType ) = 0;

	/// If Nagle is enabled (its on by default) then when calling 
	/// SendMessageToConnection the message will be queued up the Nagle time
	/// before being sent to merge small messages into the same packet.
	///
	/// Call this function to flush any queued messages and send them immediately
	/// on the next transmission time (often that means right now).
	virtual EResult FlushMessagesOnConnection( HSteamNetConnection hConn ) = 0;

	/// Fetch the next available message(s) from the socket, if any.
	/// Returns the number of messages returned into your array, up to nMaxMessages.
	/// If the connection handle is invalid, -1 is returned.
	///
	/// The order of the messages returned in the array is relevant.
	/// Reliable messages will be received in the order they were sent (and with the
	/// same sizes --- see SendMessageToConnection for on this subtle difference from a stream socket).
	///
	/// FIXME - We're still debating the exact set of guarantees for unreliable, so this might change.
	/// Unreliable messages may not be received.  The order of delivery of unreliable messages
	/// is NOT specified.  They may be received out of order with respect to each other or
	/// reliable messages.  They may be received multiple times!
	///
	/// If any messages are returned, you MUST call Release() to each of them free up resources
	/// after you are done.  It is safe to keep the object alive for a little while (put it
	/// into some queue, etc), and you may call Release() from any thread.
	virtual int ReceiveMessagesOnConnection( HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) = 0; 

	/// Same as ReceiveMessagesOnConnection, but will return the next message available
	/// on any client socket that was accepted through the specified listen socket.  Examine
	/// SteamNetworkingMessage_t::m_conn to know which client connection.
	///
	/// Delivery order of messages among different clients is not defined.  They may
	/// be returned in an order different from what they were actually received.  (Delivery
	/// order of messages from the same client is well defined, and thus the order of the
	/// messages is relevant!)
	virtual int ReceiveMessagesOnListenSocket( HSteamListenSocket hSocket, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) = 0; 

	/// Returns information about the specified connection.
	virtual bool GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo ) = 0;

	/// Returns brief set of connection status that you might want to display
	/// to the user in game.
	virtual bool GetQuickConnectionStatus( HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats ) = 0;

	/// Returns detailed connection stats in text format.  Useful
	/// for dumping to a log, etc.
	///
	/// Returns:
	/// -1 failure (bad connection handle)
	/// 0 OK, your buffer was filled in and '\0'-terminated
	/// >0 Your buffer was either nullptr, or it was too small and the text got truncated.  Try again with a buffer of at least N bytes.
	virtual int GetDetailedConnectionStatus( HSteamNetConnection hConn, char *pszBuf, int cbBuf ) = 0;

	/// Returns information about the listen socket.
	///
	/// *pnIP and *pnPort will be 0 if the socket is set to listen for connections based
	/// on SteamID only.  If your listen socket accepts connections on IPv4, then both
	/// fields will return nonzero, even if you originally passed a zero IP.  However,
	/// note that the address returned may be a private address (e.g. 10.0.0.x or 192.168.x.x),
	/// and may not be reachable by a general host on the Internet.
	virtual bool GetListenSocketInfo( HSteamListenSocket hSocket, uint32 *pnIP, uint16 *pnPort ) = 0;

	/// Create a pair of connections that are talking to each other, e.g. a loopback connection.
	/// This is very useful for testing, or so that your client/server code can work the same
	/// even when you are running a local "server".
	///
	/// The two connections will immediately be placed into the connected state, and no callbacks
	/// will be posted immediately.  After this, if you close either connection, the other connection
	/// will receive a callback, exactly as if they were communicating over the network.  You must
	/// close *both* sides in order to fully clean up the resources!
	///
	/// By default, internal buffers are used, completely bypassing the network, the chopping up of
	/// messages into packets, encryption, copying the payload, etc.  This means that loopback
	/// packets, by default, will not simulate lag or loss.  Passing true for bUseNetworkLoopback will
	/// cause the socket pair to send packets through the local network loopback device (127.0.0.1)
	/// on ephemeral ports.  Fake lag and loss are supported in this case, and CPU time is expended
	/// to encrypt and decrypt.
	///
	/// The SteamID assigned to both ends of the connection will be the SteamID of this interface.
	virtual bool CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback ) = 0;

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

	//
	// Clients connecting to dedicated servers hosted in a data center,
	// using central-authority-granted tickets.
	//

	/// Called when we receive a ticket from our central matchmaking system.  Puts the
	/// ticket into a persistent cache, and optionally returns the parsed ticket.
	///
	/// See stamdatagram_ticketgen.h for more details.
	virtual bool ReceivedRelayAuthTicket( const void *pvTicket, int cbTicket, SteamDatagramRelayAuthTicket *pOutParsedTicket ) = 0;

	/// Search cache for a ticket to talk to the server on the specified virtual port.
	/// If found, returns the number of second until the ticket expires, and optionally
	/// the complete cracked ticket.  Returns 0 if we don't have a ticket.
	///
	/// Typically this is useful just to confirm that you have a ticket, before you
	/// call ConnectToHostedDedicatedServer to connect to the server.
	virtual int FindRelayAuthTicketForServer( CSteamID steamID, int nVirtualPort, SteamDatagramRelayAuthTicket *pOutParsedTicket ) = 0;

	/// Client call to connect to a server hosted in a Valve data center, on the specified virtual
	/// port.  You should have received a ticket for this server, or else this connect call will fail!
	///
	/// You may wonder why tickets are stored in a cache, instead of simply being passed as an argument
	/// here.  The reason is to make reconnection to a gameserver robust, even if the client computer loses
	/// connection to Steam or the central backend, or the app is restarted or crashes, etc.
	virtual HSteamNetConnection ConnectToHostedDedicatedServer( CSteamID steamIDTarget, int nVirtualPort ) = 0;

	//
	// Servers hosted in Valve data centers
	//

	/// Return info about the hosted server.  You will need to send this information to your
	/// backend, and put it in tickets, so that the relays will know how to forward traffic from
	/// clients to your server.  See SteamDatagramRelayAuthTicket for more info.
	///
	/// NOTE ABOUT DEVELOPMENT ENVIRONMENTS:
	/// In production in our data centers, these parameters are configured via environment variables.
	/// In development, the only one you need to set is SDR_LISTEN_PORT, which is the local port you
	/// want to listen on.  Furthermore, if you are running your server behind a corporate firewall,
	/// you probably will not be able to put the routing information returned by this function into
	/// tickets.   Instead, it should be a public internet address that the relays can use to send
	/// data to your server.  So you might just end up hardcoding a public address and setup port
	/// forwarding on your corporate firewall.  In that case, the port you put into the ticket
	/// needs to be the public-facing port opened on your firewall, if it is different from the
	/// actual server port.  In a development environment, the POPID will be zero, and that's OK
	/// and what you should put into the ticket.
	///
	/// Returns false if the SDR_LISTEN_PORT environment variable is not set.
	virtual bool GetHostedDedicatedServerInfo( SteamDatagramServiceNetID *pRouting, SteamNetworkingPOPID *pPopID ) = 0;

	/// Create a listen socket on the specified virtual port.  The physical UDP port to use
	/// will be determined by the SDR_LISTEN_PORT environment variable.  If a UDP port is not
	/// configured, this call will fail.
	///
	/// Note that this call MUST be made through the SteamNetworkingSocketsGameServer() interface
	virtual HSteamListenSocket CreateHostedDedicatedServerListenSocket( int nVirtualPort ) = 0;

#endif // #ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

	//
	// Gets some debug text from the connection
	//
	virtual bool GetConnectionDebugText( HSteamNetConnection hConn, char *pOut, int nOutCCH ) = 0;

	//
	// Set and get configuration values, see ESteamNetworkingConfigurationValue for individual descriptions.
	//
	// Returns the value or -1 is eConfigValue is invalid
	virtual int32 GetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue ) = 0;
	// Returns true if successfully set
	virtual bool SetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue, int32 nValue ) = 0;

	// Return the name of an int configuration value, or NULL if config value isn't known
	virtual const char *GetConfigurationValueName( ESteamNetworkingConfigurationValue eConfigValue ) = 0;

	//
	// Set and get configuration strings, see ESteamNetworkingConfigurationString for individual descriptions.
	//
	// Get the configuration string, returns length of string needed if pDest is nullpr or destSize is 0
	// returns -1 if the eConfigValue is invalid
	virtual int32 GetConfigurationString( ESteamNetworkingConfigurationString eConfigString, char *pDest, int32 destSize ) = 0;
	virtual bool SetConfigurationString( ESteamNetworkingConfigurationString eConfigString, const char *pString ) = 0;

	// Return the name of a string configuration value, or NULL if config value isn't known
	virtual const char *GetConfigurationStringName( ESteamNetworkingConfigurationString eConfigString ) = 0;

	//
	// Set and get configuration values, see ESteamNetworkingConnectionConfigurationValue for individual descriptions.
	//
	// Returns the value or -1 is eConfigValue is invalid
	virtual int32 GetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue ) = 0;
	// Returns true if successfully set
	virtual bool SetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue, int32 nValue ) = 0;

	// TEMP KLUDGE Call to invoke all queued callbacks.
	// Eventually this function will go away, and callwacks will be ordinary Steamworks callbacks.
	// You should call this at the same time you call SteamAPI_RunCallbacks and SteamGameServer_RunCallbacks
	// to minimize potential changes in timing when that change happens.
	virtual void RunCallbacks( ISteamNetworkingSocketsCallbacks *pCallbacks ) = 0;
protected:
	~ISteamNetworkingSockets(); // Silence some warnings
};
//#define STEAMNETWORKINGSOCKETS_VERSION "SteamNetworkingSockets001"

extern "C" {

// Global accessor.   This will eventually be moved to steam_api.h.
STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamNetworkingSockets();
STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamNetworkingSocketsGameServer();

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE

STEAMNETWORKINGSOCKETS_INTERFACE bool GameNetworkingSockets_Init( SteamDatagramErrMsg &errMsg );
STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_Kill();

#else

/////////////////////////////////////////////////////////////////////////////
// Temp internal gross stuff you should ignore

	typedef void * ( S_CALLTYPE *FSteamInternal_CreateInterface )( const char *);
	typedef void ( S_CALLTYPE *FSteamAPI_RegisterCallback)( class CCallbackBase *pCallback, int iCallback );
	typedef void ( S_CALLTYPE *FSteamAPI_UnregisterCallback)( class CCallbackBase *pCallback );
	typedef void ( S_CALLTYPE *FSteamAPI_RegisterCallResult)( class CCallbackBase *pCallback, SteamAPICall_t hAPICall );
	typedef void ( S_CALLTYPE *FSteamAPI_UnregisterCallResult)( class CCallbackBase *pCallback, SteamAPICall_t hAPICall );
	STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagramClient_Internal_SteamAPIKludge( FSteamAPI_RegisterCallback fnRegisterCallback, FSteamAPI_UnregisterCallback fnUnregisterCallback, FSteamAPI_RegisterCallResult fnRegisterCallResult, FSteamAPI_UnregisterCallResult fnUnregisterCallResult );
	STEAMNETWORKINGSOCKETS_INTERFACE bool SteamDatagramClient_Init_InternalV5( int iPartnerMask, SteamDatagramErrMsg &errMsg, FSteamInternal_CreateInterface fnCreateInterface, HSteamUser hSteamUser, HSteamPipe hSteamPipe );
	STEAMNETWORKINGSOCKETS_INTERFACE bool SteamDatagramServer_Init_Internal( SteamDatagramErrMsg &errMsg, FSteamInternal_CreateInterface fnCreateInterface, HSteamUser hSteamUser, HSteamPipe hSteamPipe );

/////////////////////////////////////////////////////////////////////////////

/// Initialize the user interface.
/// iPartnerMask - set this to 1 for now
inline bool SteamDatagramClient_Init( int iPartnerMask, SteamDatagramErrMsg &errMsg )
{
	SteamDatagramClient_Internal_SteamAPIKludge( &::SteamAPI_RegisterCallback, &::SteamAPI_UnregisterCallback, &::SteamAPI_RegisterCallResult, &::SteamAPI_UnregisterCallResult );
	return SteamDatagramClient_Init_InternalV5( iPartnerMask, errMsg, ::SteamInternal_CreateInterface, ::SteamAPI_GetHSteamUser(), ::SteamAPI_GetHSteamPipe() );
}

/// Shutdown all clients and close all sockets
STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagramClient_Kill();

/// Initialize the game server interface
inline bool SteamDatagramServer_Init( SteamDatagramErrMsg &errMsg )
{
	SteamDatagramClient_Internal_SteamAPIKludge( &::SteamAPI_RegisterCallback, &::SteamAPI_UnregisterCallback, &::SteamAPI_RegisterCallResult, &::SteamAPI_UnregisterCallResult );
	return SteamDatagramServer_Init_Internal( errMsg, &SteamInternal_CreateInterface, ::SteamGameServer_GetHSteamUser(), ::SteamGameServer_GetHSteamPipe() );
}

/// Shutdown the game server interface
STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagramServer_Kill( );

#endif

/// Callback struct used to notify when a connection has changed state
#if defined( VALVE_CALLBACK_PACK_SMALL )
#pragma pack( push, 4 )
#elif defined( VALVE_CALLBACK_PACK_LARGE )
#pragma pack( push, 8 )
#else
#error "Must define VALVE_CALLBACK_PACK_SMALL or VALVE_CALLBACK_PACK_LARGE"
#endif
struct SteamNetConnectionStatusChangedCallback_t
{ 
	enum { k_iCallback = k_iSteamNetworkingCallbacks + 9 }; // Pretty sure this ID is available.  It will probably change later
	HSteamNetConnection m_hConn;		//< Connection handle
	SteamNetConnectionInfo_t m_info;	//< Full connection info
	int m_eOldState;					//< ESNetSocketState.  (Current stats is in m_info)
};
#pragma pack( pop )

/// TEMP callback dispatch mechanism.
/// You'll override this guy and hook any callbacks you are interested in,
/// and then use ISteamNetworkingSockets::RunCallbacks.  Eventually this will go away,
/// and you will register for the callbacks you want using the normal SteamWorks callback
/// mechanisms, and they will get dispatched along with other Steamworks callbacks
/// when you call SteamAPI_RunCallbacks and SteamGameServer_RunCallbacks.
class ISteamNetworkingSocketsCallbacks
{
public:
	inline ISteamNetworkingSocketsCallbacks() {}
	virtual void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t * ) {}
	virtual void OnP2PSessionRequest( P2PSessionRequest_t * ) {}
	virtual void OnP2PSessionConnectFail( P2PSessionConnectFail_t * ) {}
protected:
	inline ~ISteamNetworkingSocketsCallbacks() {}
};

enum ESteamNetworkingSocketsDebugOutputType
{
	k_ESteamNetworkingSocketsDebugOutputType_None,
	k_ESteamNetworkingSocketsDebugOutputType_Bug, // You used the API incorrectly, or an internal error happened
	k_ESteamNetworkingSocketsDebugOutputType_Error, // Run-time error condition that isn't the result of a bug.  (E.g. we are offline, cannot bind a port, etc)
	k_ESteamNetworkingSocketsDebugOutputType_Important, // Nothing is wrong, but this is an important notification
	k_ESteamNetworkingSocketsDebugOutputType_Warning,
	k_ESteamNetworkingSocketsDebugOutputType_Msg, // Recommended amount
	k_ESteamNetworkingSocketsDebugOutputType_Verbose, // Quite a bit
	k_ESteamNetworkingSocketsDebugOutputType_Debug, // Practically everything
	k_ESteamNetworkingSocketsDebugOutputType_Everything, // Everything
};

/// Setup callback for debug output, and the desired verbosity you want.
typedef void (*FSteamNetworkingSocketsDebugOutput)( /* ESteamNetworkingSocketsDebugOutputType */ int nType, const char *pszMsg );
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetDebugOutputFunction( /* ESteamNetworkingSocketsDebugOutputType */ int eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc );

}

#endif // ISTEAMNETWORKINGSOCKETS
