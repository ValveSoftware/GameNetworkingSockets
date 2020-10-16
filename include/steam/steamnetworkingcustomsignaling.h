//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Interfaces needed to implement your own P2P signaling service.  If you
// aren't using P2P connections, or you can use the default service provided
// by the platform (e.g. a typical Steam game), then you don't need anything
// in this file.
//
//=============================================================================

#ifndef STEAMNETWORKINGCUSTOMSIGNALING
#define STEAMNETWORKINGCUSTOMSIGNALING
#pragma once

#include "steamnetworkingtypes.h"

class ISteamNetworkingSockets;

/// Interface used to send signaling messages for a particular connection.
///
/// - For connections initiated locally, you will construct it and pass
///   it to ISteamNetworkingSockets::ConnectP2PCustomSignaling.
/// - For connections initiated remotely and "accepted" locally, you
///   will return it from ISteamNetworkingSignalingRecvContext::OnConnectRequest
class ISteamNetworkingConnectionSignaling
{
public:
	/// Called to send a rendezvous message to the remote peer.  This may be called
	/// from any thread, at any time, so you need to be thread-safe!  Don't take
	/// any locks that might hold while calling into SteamNetworkingSockets functions,
	/// because this could lead to deadlocks.
	///
	/// Note that when initiating a connection, we may not know the identity
	/// of the peer, if you did not specify it in ConnectP2PCustomSignaling.
	///
	/// Return true if a best-effort attempt was made to deliver the message.
	/// If you return false, it is assumed that the situation is fatal;
	/// the connection will be closed, and Release() will be called
	/// eventually.
	///
	/// Signaling objects will not be shared between connections.
	/// You can assume that the same value of hConn will be used
	/// every time.
	virtual bool SendSignal( HSteamNetConnection hConn, const SteamNetConnectionInfo_t &info, const void *pMsg, int cbMsg ) = 0;

	/// Called when the connection no longer needs to send signals.
	/// Note that this happens eventually (but not immediately) after
	/// the connection is closed.  Signals may need to be sent for a brief
	/// time after the connection is closed, to clean up the connection.
	///
	/// If you do not need to save any additional per-connection information
	/// and can handle SendSignal() using only the arguments supplied, you do
	/// not need to actually create different objects per connection.  In that
	/// case, it is valid for all connections to use the same global object, and
	/// for this function to do nothing.
	virtual void Release() = 0;
};

/// Interface used when a custom signal is received.
/// See ISteamNetworkingSockets::ReceivedP2PCustomSignal
class ISteamNetworkingSignalingRecvContext
{
public:

	/// Called when the signal represents a request for a new connection.
	///
	/// If you want to ignore the request, just return NULL.  In this case,
	/// the peer will NOT receive any reply.  You should consider ignoring
	/// requests rather than actively rejecting them, as a security measure.
	/// If you actively reject requests, then this makes it possible to detect
	/// if a user is online or not, just by sending them a request.
	///
	/// If you wish to send back a rejection, then use
	/// ISteamNetworkingSockets::CloseConnection() and then return NULL.
	/// We will marshal a properly formatted rejection signal and
	/// call SendRejectionSignal() so you can send it to them.
	///
	/// If you return a signaling object, the connection is NOT immediately
	/// accepted by default.  Instead, it stays in the "connecting" state,
	/// and the usual callback is posted, and your app can accept the
	/// connection using ISteamNetworkingSockets::AcceptConnection.  This
	/// may be useful so that these sorts of connections can be more similar
	/// to your application code as other types of connections accepted on
	/// a listen socket.  If this is not useful and you want to skip this
	/// callback process and immediately accept the connection, call
	/// ISteamNetworkingSockets::AcceptConnection before returning the
	/// signaling object.
	///
	/// After accepting a connection (through either means), the connection
	/// will transition into the "finding route" state.
	virtual ISteamNetworkingConnectionSignaling *OnConnectRequest( HSteamNetConnection hConn, const SteamNetworkingIdentity &identityPeer, int nLocalVirtualPort ) = 0;

	/// This is called to actively communicate rejection or failure
	/// to the incoming message.  If you intend to ignore all incoming requests
	/// that you do not wish to accept, then it's not strictly necessary to
	/// implement this.
	virtual void SendRejectionSignal( const SteamNetworkingIdentity &identityPeer, const void *pMsg, int cbMsg ) = 0;
};

/// The function signature of the callback used to obtain a signaling object
/// for connections initiated locally.  These are used for
/// ISteamNetworkingSockets::ConnectP2P, and when using the
/// ISteamNetworkingMessages interface.  To install the callback for all
/// interfaces, do something like this:
/// SteamNetworkingUtils()->SetGlobalConfigValuePtr( k_ESteamNetworkingConfig_Callback_CreateConnectionSignaling, (void*)fnCallback );
typedef ISteamNetworkingConnectionSignaling * (*FnSteamNetworkingSocketsCreateConnectionSignaling)( ISteamNetworkingSockets *pLocalInterface, const SteamNetworkingIdentity &identityPeer, int nLocalVirtualPort, int nRemoteVirtualPort );

#endif // STEAMNETWORKINGCUSTOMSIGNALING

