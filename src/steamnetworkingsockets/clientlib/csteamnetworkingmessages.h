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
class CSteamNetworkConnectionP2P;
class CSteamNetworkListenSocketP2P;
struct SteamNetworkingMessagesSession;

/////////////////////////////////////////////////////////////////////////////
//
// Stuff shared between CSteamNetworkingMessages and Fake UDP ports
//
/////////////////////////////////////////////////////////////////////////////

// CMessagesEndPoint is a base class for CSteamNetworkingMessages and FakeUDPPorts.
class CMessagesEndPoint
{
public:
	CSteamNetworkingSockets &m_steamNetworkingSockets;
	const int m_nLocalVirtualPort;

	virtual bool BHandleNewIncomingConnection( CSteamNetworkConnectionBase *pConn, ConnectionScopeLock &connectionLock ) = 0;

	void DestroyMessagesEndPoint();

	CSteamNetworkListenSocketP2P *m_pListenSocket = nullptr; // Might be NULL for "ephemeral" endpoints that cannot receive unsolicited traffic

	// !SPEED! *All* of the sessions and connections share the same lock!
	// This could be improved, if we encounter a use case that needs it!
	// We could use one lock per session, and then all connection(s) in that session
	// would use the same lock.
	ConnectionLock m_sharedConnectionLock;

protected:
	bool BInit();
	bool BCreateListenSocket();

	CMessagesEndPoint( CSteamNetworkingSockets &steamNetworkingSockets, int nLocalVirtualPort );
	virtual ~CMessagesEndPoint();

	virtual void FreeResources();
};

/// MessagesEndPointSession tracks a connection with a peer and handles
/// timing it out when it goes idle
class CMessagesEndPointSession : public ILockableThinker<ConnectionLock>
{
public:
	SteamNetworkingIdentity m_identityRemote;
	CMessagesEndPoint &m_messageEndPointOwner;

	// Currently active connection, if any.  Might be NULL in some circumstances.
	// If non-null, this connection will also appear in m_vecLinkedConnections
	CSteamNetworkConnectionBase *m_pConnection;

	// *All* connections that currently think that we are the owner.  This will almost always
	// have 0 or 1 connections.  In rare occasions it might have 2.
	vstd::small_vector<CSteamNetworkConnectionBase *, 2> m_vecLinkedConnections;

	/// Called when the state changes
	void SessionConnectionStateChanged( CSteamNetworkConnectionBase *pConn, ESteamNetworkingConnectionState eOldState );

	/// If we get to this time, the session has been idle
	/// and we should clean it up.
	SteamNetworkingMicroseconds m_usecIdleTimeout;

	/// Try if the app scheduled cleanup
	bool m_bAppScheduledTimeout;

	/// True if the current connection ever managed to go fully connected
	bool m_bConnectionWasEverConnected;

	/// True if the connection has changed state since
	/// the last time we checked on it.
	bool m_bConnectionStateChanged;

	/// Record that we have been used
	void MarkUsed( SteamNetworkingMicroseconds usecNow );

	/// Ensure that we are scheduled to wake up and get service
	/// at the next time it looks like we might need to do something
	void ScheduleThink();

	virtual void SetActiveConnection( CSteamNetworkConnectionBase *pConn, ConnectionScopeLock &connectionLock );
	virtual void ClearActiveConnection();

	/// Called when a message is received on one of our connections
	virtual void ReceivedMessage( CSteamNetworkingMessage *pMsg, CSteamNetworkConnectionBase *pConn ) = 0;

	/// Try to unlink from any old connections.  The locking and
	/// object ownership is complicated here.  This must be called
	/// from a safe place, when functions on the stack might have
	/// locked the session but not any connections.
	void UnlinkFromInactiveConnections();

	/// Unlink from the current connection NOW
	void UnlinkConnectionNow( CSteamNetworkConnectionBase *pConn );

protected:
	CMessagesEndPointSession( const SteamNetworkingIdentity &identityRemote, CMessagesEndPoint &endPoint );
	virtual ~CMessagesEndPointSession();

	/// Called when the state changes
	virtual void ActiveConnectionStateChanged();
};

/////////////////////////////////////////////////////////////////////////////
//
// Steam API interfaces
//
/////////////////////////////////////////////////////////////////////////////

// CSteamNetworkingMessages is the concrete implementation of the ISteamNetworkingMessages interface
class CSteamNetworkingMessages final : public CMessagesEndPoint, public IClientNetworkingMessages
{
public:
	STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW
	CSteamNetworkingMessages( CSteamNetworkingSockets &steamNetworkingSockets );

	bool BInit();

	// Implements ISteamNetworkingMessages
	virtual EResult SendMessageToUser( const SteamNetworkingIdentity &identityRemote, const void *pubData, uint32 cubData, int nSendFlags, int nChannel ) override;
	virtual int ReceiveMessagesOnChannel( int nChannel, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages ) override;
	virtual bool AcceptSessionWithUser( const SteamNetworkingIdentity &identityRemote ) override;
	virtual bool CloseSessionWithUser( const SteamNetworkingIdentity &identityRemote ) override;
	virtual bool CloseChannelWithUser( const SteamNetworkingIdentity &identityRemote, int nChannel ) override;
	virtual ESteamNetworkingConnectionState GetSessionConnectionInfo( const SteamNetworkingIdentity &identityRemote, SteamNetConnectionInfo_t *pConnectionInfo, SteamNetConnectionRealTimeStatus_t *pQuickStatus ) override;

	#ifdef DBGFLAG_VALIDATE
	virtual void Validate( CValidator &validator, const char *pchName ) override;
	#endif

	virtual bool BHandleNewIncomingConnection( CSteamNetworkConnectionBase *pConn, ConnectionScopeLock &connectionLock ) override;

	struct Channel
	{
		Channel();
		~Channel();

		SteamNetworkingMessageQueue m_queueRecvMessages;
	};

	Channel *FindOrCreateChannel( int nChannel );
	void DestroySession( const SteamNetworkingIdentity &identityRemote );

	#ifdef DBGFLAG_VALIDATE
	static void ValidateStatics( CValidator &validator );
	#endif
private:

	SteamNetworkingMessagesSession *FindSession( const SteamNetworkingIdentity &identityRemote, ConnectionScopeLock &scopeLock );
	SteamNetworkingMessagesSession *FindOrCreateSession( const SteamNetworkingIdentity &identityRemote, ConnectionScopeLock &scopeLock );

	CUtlHashMap< SteamNetworkingIdentity, SteamNetworkingMessagesSession *, std::equal_to<SteamNetworkingIdentity>, SteamNetworkingIdentityHash > m_mapSessions;
	CUtlHashMap<int,Channel*,std::equal_to<int>,std::hash<int>> m_mapChannels;

	virtual void FreeResources() override;

	virtual ~CSteamNetworkingMessages();
};

struct SteamNetworkingMessagesSession final : public CMessagesEndPointSession
{
	SteamNetworkingMessagesSession( const SteamNetworkingIdentity &identityRemote, CSteamNetworkingMessages &steamNetworkingP2P );
	virtual ~SteamNetworkingMessagesSession();

	/// Upcast
	CSteamNetworkingMessages &MessagesOwner() const { return static_cast<CSteamNetworkingMessages &>( m_messageEndPointOwner ); }

	/// Queue of inbound messages
	SteamNetworkingMessageQueue m_queueRecvMessages;

	CUtlHashMap<int,bool,std::equal_to<int>,std::hash<int>> m_mapOpenChannels;

	/// Most recent info about the connection.
	SteamNetConnectionInfo_t m_lastConnectionInfo;
	SteamNetConnectionRealTimeStatus_t m_lastQuickStatus;

	// Implements CMessagesEndPointSession
	virtual void Think( SteamNetworkingMicroseconds usecNow ) override;
	virtual void SetActiveConnection( CSteamNetworkConnectionBase *pConn, ConnectionScopeLock &connectionLock ) override;
	virtual void ActiveConnectionStateChanged() override;
	virtual void ReceivedMessage( CSteamNetworkingMessage *pMsg, CSteamNetworkConnectionBase *pConn ) override;

	/// Close the connection with the specified reason info
	void CloseConnection( int nReason, const char *pszDebug );

	/// Check on the connection state
	void CheckConnection( SteamNetworkingMicroseconds usecNow );

	void UpdateConnectionInfo();

	#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName );
	#endif
};

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES

#endif // CSTEAMNETWORKINGMESSAGES_H
