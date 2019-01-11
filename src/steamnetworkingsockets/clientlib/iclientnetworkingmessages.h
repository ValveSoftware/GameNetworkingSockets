//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef ICLIENTNETWORKINGMESSAGES_H
#define ICLIENTNETWORKINGMESSAGES_H
#pragma once

#include <steam/isteamnetworkingmessages.h>
#include "iclientnetworkingsockets.h" // for CLIENTNETWORKINGSOCKETS_OVERRIDE

/////////////////////////////////////////////////////////////////////////////
//
// IClientNetworkingMessages
//
// In Steam, this is a non-versioned interface used internally.  It only
// implements the latest version of ISteamNetworkingMessages, and we
// define adapters to convert users of old ISteamNetworkingMessages
// versions to be able to talk to this interface.
//
// Outside of Steam, this layer of version is not needed, and
// ISteamNetworkingMessages and IClientNetworkingMessages should
// be equivalent.  This layer shouldn't add any runtime cost in that case.
//
/////////////////////////////////////////////////////////////////////////////

class IClientNetworkingMessages
#ifndef STEAMNETWORKINGSOCKETS_STEAM
: public ISteamNetworkingMessages
#endif
{
public:
	virtual EResult SendMessageToUser( const SteamNetworkingIdentity &identityRemote, const void *pubData, uint32 cubData, ESteamNetworkingSendType eSendType, int nChannel ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int ReceiveMessagesOnChannel( int nChannel, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool AcceptSessionWithUser( const SteamNetworkingIdentity &identityRemote ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool CloseSessionWithUser( const SteamNetworkingIdentity &identityRemote ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool CloseChannelWithUser( const SteamNetworkingIdentity &identityRemote, int nChannel ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool GetSessionState( const SteamNetworkingIdentity &identityRemote, P2PSessionState_t *pConnectionState ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
};

#endif // ICLIENTNETWORKINGMESSAGES_H
