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
// nLossPct: percentage of outbound signals to silently drop (0-100), to simulate unreliable signaling.
// nDupPct:  percentage of outbound signals to send twice (0-100), to simulate duplicate delivery.
ITrivialSignalingClient *CreateTrivialSignalingClient(
	const char *address, // Address:port
	ISteamNetworkingSockets *pSteamNetworkingSockets, // Where should we send signals when we get them?
	SteamNetworkingErrMsg &errMsg, // Error message is returned here if we fail
	int nLossPct = 0,
	int nDupPct = 0
);

	


