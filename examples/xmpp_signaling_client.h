//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Client for XMPP (Jabber) signaling service (RFC 6120 / RFC 6121).
// Serves as an example of how to hook up an XMPP signaling server
// to SteamNetworkingSockets P2P connections (Issue #136).
//
//=============================================================================

#pragma once

#include <steam/steamnetworkingcustomsignaling.h>
#include <string>

class ISteamNetworkingSockets;

/// Interface to our XMPP signaling client.
class IXmppSignalingClient
{
public:

	/// Create signaling object for a connection to peer.
	/// identityPeer will typically be set with SetGenericString("user@domain").
	virtual ISteamNetworkingConnectionSignaling *CreateSignalingForConnection(
		const SteamNetworkingIdentity &identityPeer,
		SteamNetworkingErrMsg &errMsg ) = 0;

	/// Poll the server for incoming signals and dispatch them.
	virtual void Poll() = 0;

	/// Disconnect from the XMPP server and release resources.
	virtual void Release() = 0;
};

/// Connect to an XMPP server.
/// pszServerAddress: host:port (e.g. "jabber.example.org:5223" or "127.0.0.1:5222").
/// pszJid: user@domain/resource.
/// pszPassword: password for SASL PLAIN authentication.
/// bUseTLS: if true, uses Direct TLS (SSL_connect) on connect; if false, uses plain TCP (for local mocks).
/// pSteamNetworkingSockets: sockets interface to deliver incoming signals to.
/// errMsg: error message buffer if creation fails.
IXmppSignalingClient *CreateXmppSignalingClient(
	const char *pszServerAddress,
	const char *pszJid,
	const char *pszPassword,
	bool bUseTLS,
	ISteamNetworkingSockets *pSteamNetworkingSockets,
	SteamNetworkingErrMsg &errMsg
);

/// Utility helpers for encoding/decoding XMPP signaling stanzas
std::string XmppBase64Encode( const void *pData, int cbData );
bool XmppBase64Decode( const char *pszInput, int cchInput, std::string &sOutDecoded );
std::string XmppFormatMessageStanza( const char *pszFrom, const char *pszTo, const void *pPayload, int cbPayload );
bool XmppExtractMessage( std::string &sBuffer, std::string &sOutFrom, std::string &sOutPayload );
