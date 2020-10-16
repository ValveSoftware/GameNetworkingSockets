// Client of our dummy trivial signaling server service.
// Serves as an example of you how to hook up signaling server
// to SteamNetworkingSockets P2P connections

#pragma once

#include <steam/steamnetworkingcustomsignaling.h>

class ISteamNetworkingSockets;

/// Interface to our client.
class ITrivialSignalingClient
{
public:

	/// Create signaling object for a connection to peer
    virtual ISteamNetworkingConnectionSignaling *CreateSignalingForConnection(
        const SteamNetworkingIdentity &identityPeer,
        SteamNetworkingErrMsg &errMsg ) = 0;

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

	


