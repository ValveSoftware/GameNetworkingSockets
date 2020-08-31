// Client of our dummy trivial signaling server service.
// Serves as an example of you how to hook up signaling server
// to SteamNetworkingSockets P2P connections

#pragma once

#include <steam/steamnetworkingcustomsignaling.h>
class ISteamNetworkingSockets;

// FIXME - Eventually I intend to add a mechanism to set the default
//         signaling service, so that SteamnetworkingSockets can
//         initiate creation of signaling sessions.  This will be the
//         interface used for that.
/// Interface used to create signaling sessions to particular peers.
/// Typically this represents a connection to some service.
class ISteamNetworkingCustomSignalingService
{
public:

	/// Setup a session for sending signals for a particular connection.
	/// The signals will always be sent to the same peer.
	///
	/// pszSessionRoutingInfo is reserved for future use, it will always
	/// be NULL right now.
	///
	/// On failure, return NULL
	virtual ISteamNetworkingConnectionCustomSignaling *CreateSignalingForConnection(
		const SteamNetworkingIdentity &identityPeer,
		const char *pszRoutingInfo,
		SteamNetworkingErrMsg &errMsg
	) = 0;
};

/// Interface to our client.
class ITrivialSignalingClient : public ISteamNetworkingCustomSignalingService
{
public:

	/// Poll the server for incoming signals and dispatch them.
	/// We use polling in this example just to keep it simple.
	/// You could use a service thread.
	virtual void Poll() = 0;

	/// Disconnect from the server and close down our polling thread.
	virtual void Release() = 0;
};

// Start connecting to the signaling server.
ITrivialSignalingClient *CreateTrivialSignalingClient(
	const char *address, // Address:port
	ISteamNetworkingSockets *pSteamNetworkingSockets, // Where should we send signals when we get them?
	SteamNetworkingErrMsg &errMsg // Error message is retjrned here if we fail
);

	


