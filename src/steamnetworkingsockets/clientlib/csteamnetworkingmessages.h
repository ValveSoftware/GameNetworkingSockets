//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef CSTEAMNETWORKINGMESSAGES_H
#define CSTEAMNETWORKINGMESSAGES_H
#pragma once

#include <tier1/utlhashmap.h>
#include <steam/steamnetworkingtypes.h>
#include <steam/isteamnetworkingmessages.h>
#include "steamnetworkingsockets_connections.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES

#if defined( STEAMNETWORKINGSOCKETS_STEAMCLIENT ) || defined( STEAMNETWORKINGSOCKETS_STREAMINGCLIENT )
	#include <steam/iclientnetworkingmessages.h>
#else
	typedef ISteamNetworkingMessages IClientNetworkingMessages;
#endif

class CMsgSteamDatagramConnectRequest;

namespace SteamNetworkingSocketsLib {

class CSteamNetworkingSockets;
class CSteamNetworkingMessage;
class CSteamNetworkingMessages;

/////////////////////////////////////////////////////////////////////////////
//
// Steam API interfaces
//
/////////////////////////////////////////////////////////////////////////////

struct SteamNetworkingMessagesSession : public IThinker
{
	SteamNetworkingMessagesSession( const SteamNetworkingIdentity &identityRemote, CSteamNetworkingMessages &steamNetworkingP2P );
	virtual ~SteamNetworkingMessagesSession();

	SteamNetworkingIdentity m_identityRemote;
	CSteamNetworkingMessages &m_steamNetworkingMessagesOwner;
	CSteamNetworkConnectionBase *m_pConnection; // active connection, if any.  Might be NULL!

	/// Queue of inbound messages
	SteamNetworkingMessageQueue m_queueRecvMessages;

	CUtlHashMap<int,bool,std::equal_to<int>,std::hash<int>> m_mapOpenChannels;

	/// If we get tot his time, the session has been idle
	/// and we should clean it up.
	SteamNetworkingMicroseconds m_usecIdleTimeout;

	/// True if the connection has changed state and we need to check on it
	bool m_bConnectionStateChanged;

	/// True if the current connection ever managed to go fully connected
	bool m_bConnectionWasEverConnected;

	/// Most recent info about the connection.
	SteamNetConnectionInfo_t m_lastConnectionInfo;
	SteamNetworkingQuickConnectionStatus m_lastQuickStatus;

	/// Close the connection with the specified reason info
	void CloseConnection( int nReason, const char *pszDebug );

	/// Record that we have been used
	void MarkUsed( SteamNetworkingMicroseconds usecNow );

	/// Ensure that we are scheduled to wake up and get service
	/// at the next time it looks like we might need to do something
	void ScheduleThink();

	// Implements IThinker
	virtual void Think( SteamNetworkingMicroseconds usecNow ) override;

	/// Check on the connection state
	void CheckConnection( SteamNetworkingMicroseconds usecNow );

	void UpdateConnectionInfo();

	void LinkConnection( CSteamNetworkConnectionBase *pConn );
	void UnlinkConnection();

	void ReceivedMessage( CSteamNetworkingMessage *pMsg );
	void ConnectionStateChanged( SteamNetConnectionStatusChangedCallback_t *pInfo );

	#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName );
	#endif
};

class CSteamNetworkingMessages : public IClientNetworkingMessages
{
public:
	STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW
	CSteamNetworkingMessages( CSteamNetworkingSockets &steamNetworkingSockets );
	virtual ~CSteamNetworkingMessages();

	bool BInit();
	void FreeResources();

	// Implements ISteamNetworkingMessages
	virtual EResult SendMessageToUser( const SteamNetworkingIdentity &identityRemote, const void *pubData, uint32 cubData, int nSendFlags, int nChannel ) override;
	virtual int ReceiveMessagesOnChannel( int nChannel, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) override;
	virtual bool AcceptSessionWithUser( const SteamNetworkingIdentity &identityRemote ) override;
	virtual bool CloseSessionWithUser( const SteamNetworkingIdentity &identityRemote ) override;
	virtual bool CloseChannelWithUser( const SteamNetworkingIdentity &identityRemote, int nChannel ) override;
	virtual ESteamNetworkingConnectionState GetSessionConnectionInfo( const SteamNetworkingIdentity &identityRemote, SteamNetConnectionInfo_t *pConnectionInfo, SteamNetworkingQuickConnectionStatus *pQuickStatus ) override;

	#ifdef DBGFLAG_VALIDATE
	virtual void Validate( CValidator &validator, const char *pchName ) override;
	#endif

	void NewConnection( CSteamNetworkConnectionBase *pConn );

	CSteamNetworkingSockets &m_steamNetworkingSockets;

	struct Channel
	{
		SteamNetworkingMessageQueue m_queueRecvMessages;

		~Channel();
	};

	CSteamNetworkListenSocketBase *m_pListenSocket = nullptr;
	CSteamNetworkPollGroup *m_pPollGroup = nullptr;

	Channel *FindOrCreateChannel( int nChannel );
	void DestroySession( const SteamNetworkingIdentity &identityRemote );

	void PollMessages( SteamNetworkingMicroseconds usecNow );

private:

	SteamNetworkingMessagesSession *FindSession( const SteamNetworkingIdentity &identityRemote );
	SteamNetworkingMessagesSession *FindOrCreateSession( const SteamNetworkingIdentity &identityRemote );

	CUtlHashMap< SteamNetworkingIdentity, SteamNetworkingMessagesSession *, std::equal_to<SteamNetworkingIdentity>, SteamNetworkingIdentityHash > m_mapSessions;
	CUtlHashMap<int,Channel*,std::equal_to<int>,std::hash<int>> m_mapChannels;

	static void ConnectionStatusChangedCallback( SteamNetConnectionStatusChangedCallback_t *pInfo );
};

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES

#endif // CSTEAMNETWORKINGMESSAGES_H
