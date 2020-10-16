//====== Copyright Valve Corporation, All rights reserved. ====================

#include "csteamnetworkingmessages.h"
#include "csteamnetworkingsockets.h"
#include "steamnetworkingsockets_p2p.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Make sure we're enabled
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES

#pragma pack(push,1)
struct P2PMessageHeader
{
	uint8 m_nFlags;
	int m_nToChannel;
};
#pragma pack(pop)
COMPILE_TIME_ASSERT( sizeof(P2PMessageHeader) == 5 );

// FIXME TODO:
// * Need to clear P2P error when we start connecting or get a successful result
// * When we get P2P error callback from steam, need to flow that back up through to the session
// * Handle race condition when we try to send a message right as the connection is timing out
// * Nuke interface when higher level Kill calls are made
// * Only do kludge to always send early messages as reliable on old code, not new code.

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

const SteamNetworkingMicroseconds k_usecSteamNetworkingP2PSessionIdleTimeout = 3*60*k_nMillion;
const int k_ESteamNetConnectionEnd_P2P_SessionClosed = k_ESteamNetConnectionEnd_App_Min + 1;
const int k_ESteamNetConnectionEnd_P2P_SessionIdleTimeout = k_ESteamNetConnectionEnd_App_Min + 2;

static CUtlHashMap<HSteamNetConnection,SteamNetworkingMessagesSession*,std::equal_to<HSteamNetConnection>,std::hash<HSteamNetConnection>> g_mapSessionsByConnection;
static CUtlHashMap<HSteamListenSocket,CSteamNetworkingMessages*,std::equal_to<HSteamListenSocket>,std::hash<HSteamListenSocket>> g_mapMessagesInterfaceByListenSocket;

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingSockets
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkingMessages *CSteamNetworkingSockets::GetSteamNetworkingMessages()
{
	// Already exist?
	if ( !m_pSteamNetworkingMessages )
	{
		SteamDatagramTransportLock scopeLock;
		SteamDatagramTransportLock::SetLongLockWarningThresholdMS( "CreateSteamNetworkingMessages", 10 );
		m_pSteamNetworkingMessages = new CSteamNetworkingMessages( *this );
		if ( !m_pSteamNetworkingMessages->BInit() )
		{
			// NOTE: We re gonna keep trying to do this and failing repeatedly.
			delete m_pSteamNetworkingMessages;
			m_pSteamNetworkingMessages = nullptr;
		}
	}
	return m_pSteamNetworkingMessages;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingMessages
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkingMessages::Channel::~Channel()
{
	// Should be empty!
	Assert( m_queueRecvMessages.empty() );

	// But in case not
	m_queueRecvMessages.PurgeMessages();
}

CSteamNetworkingMessages::CSteamNetworkingMessages( CSteamNetworkingSockets &steamNetworkingSockets )
: m_steamNetworkingSockets( steamNetworkingSockets )
{
}

bool CSteamNetworkingMessages::BInit()
{
	// Create listen socket
	{
		SteamNetworkingConfigValue_t opt[2];
		opt[0].SetPtr( k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)ConnectionStatusChangedCallback );
		opt[1].SetInt32( k_ESteamNetworkingConfig_SymmetricConnect, 1 );
		m_pListenSocket = m_steamNetworkingSockets.InternalCreateListenSocketP2P( k_nVirtualPort_Messages, 2, opt );
		if ( !m_pListenSocket )
			return false;
	}

	// Add us to map by listen socket
	Assert( !g_mapMessagesInterfaceByListenSocket.HasElement( m_pListenSocket->m_hListenSocketSelf ) );
	g_mapMessagesInterfaceByListenSocket.InsertOrReplace( m_pListenSocket->m_hListenSocketSelf, this );

	// Create poll group
	m_pPollGroup = GetPollGroupByHandle( m_steamNetworkingSockets.CreatePollGroup() );
	if ( !m_pPollGroup )
	{
		AssertMsg( false, "Failed to create/find poll group" );
		return false;
	}

	return true;
}

void CSteamNetworkingMessages::FreeResources()
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread( "CSteamNetworkingMessages::FreeResources" );

	// Destroy all of our sessions.  This will detach all of our connections
	FOR_EACH_HASHMAP( m_mapSessions, i )
	{
		DestroySession( m_mapSessions.Key(i) );
	}
	Assert( m_mapSessions.Count() == 0 );
	m_mapSessions.Purge();
	m_mapChannels.PurgeAndDeleteElements();

	// Destroy poll group, if any
	delete m_pPollGroup;
	m_pPollGroup = nullptr;

	// Destroy listen socket, if any
	if ( m_pListenSocket )
	{

		// Remove us from the map
		int idx = g_mapMessagesInterfaceByListenSocket.Find( m_pListenSocket->m_hListenSocketSelf );
		if ( idx == g_mapMessagesInterfaceByListenSocket.InvalidIndex() )
		{
			Assert( false );
		}
		else
		{
			Assert( g_mapMessagesInterfaceByListenSocket[idx] == this );
			g_mapMessagesInterfaceByListenSocket[idx] = nullptr; // Just for grins
			g_mapMessagesInterfaceByListenSocket.RemoveAt( idx );
		}

		m_pListenSocket->Destroy();
		m_pListenSocket = nullptr;
	}

	int idx = m_steamNetworkingSockets.m_mapListenSocketsByVirtualPort.Find( k_nVirtualPort_Messages );
	if ( idx != m_steamNetworkingSockets.m_mapListenSocketsByVirtualPort.InvalidIndex() )
	{
		DbgVerify( m_steamNetworkingSockets.CloseListenSocket( m_steamNetworkingSockets.m_mapListenSocketsByVirtualPort[ idx ]->m_hListenSocketSelf ) );
	}
	Assert( !m_steamNetworkingSockets.m_mapListenSocketsByVirtualPort.HasElement( k_nVirtualPort_Messages ) );
}


CSteamNetworkingMessages::~CSteamNetworkingMessages()
{
	SteamDatagramTransportLock scopeLock;

	FreeResources();

	// make sure our parent knows we have been destroyed
	if ( m_steamNetworkingSockets.m_pSteamNetworkingMessages == this )
	{
		m_steamNetworkingSockets.m_pSteamNetworkingMessages = nullptr;
	}
	else
	{
		// We should never create more than one messages interface for any given sockets interface!
		Assert( m_steamNetworkingSockets.m_pSteamNetworkingMessages == nullptr );
	}
}

void CSteamNetworkingMessages::ConnectionStatusChangedCallback( SteamNetConnectionStatusChangedCallback_t *pInfo )
{
	// These callbacks should happen synchronously, while we have the lock
	SteamDatagramTransportLock::AssertHeldByCurrentThread( "CSteamNetworkingMessages::ConnectionStatusChangedCallback" );

	// New connection?
	if ( pInfo->m_eOldState == k_ESteamNetworkingConnectionState_None )
	{

		// New connections should only ever transition into to the "connecting" state
		if ( pInfo->m_info.m_eState != k_ESteamNetworkingConnectionState_Connecting )
		{
			AssertMsg( false, "Unexpected state transition from 'none' to %d", pInfo->m_info.m_eState );
			return;
		}

		// Are we initiating this?
		if ( pInfo->m_info.m_hListenSocket == k_HSteamListenSocket_Invalid )
			return; // ignore

		// New incoming connection.
		int h = g_mapMessagesInterfaceByListenSocket.Find( pInfo->m_info.m_hListenSocket );
		if ( h == g_mapSessionsByConnection.InvalidIndex() )
		{
			AssertMsg( false, "ConnectionStatusChangedCallback, but listen socket not found?" );

			// FIXME - if we hit this bug, we leak the connection.  Should we try to clean up better?
			return;
		}
		CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( pInfo->m_hConn );
		if ( !pConn )
		{
			AssertMsg( false, "Can't find connection by handle?" );
			return;
		}
		Assert( pConn->GetState() == k_ESteamNetworkingConnectionState_Connecting );
		g_mapMessagesInterfaceByListenSocket[h]->NewConnection( pConn );
		return;
	}

	// Change to a known connection with a session?
	int h = g_mapSessionsByConnection.Find( pInfo->m_hConn );
	if ( h != g_mapSessionsByConnection.InvalidIndex() )
	{
		g_mapSessionsByConnection[h]->ConnectionStateChanged( pInfo );
		return;
	}
}

EResult CSteamNetworkingMessages::SendMessageToUser( const SteamNetworkingIdentity &identityRemote, const void *pubData, uint32 cubData, int nSendFlags, int nRemoteChannel )
{
	// FIXME SteamNetworkingIdentity
	if ( identityRemote.GetSteamID64() == 0 )
	{
		AssertMsg1( false, "Identity %s isn't valid for Messages sessions.  (Only SteamIDs currently supported).", SteamNetworkingIdentityRender( identityRemote ).c_str() );
		return k_EResultInvalidSteamID;
	}
	if ( !IsValidSteamIDForIdentity( identityRemote.GetSteamID() ) )
	{
		AssertMsg1( false, "%s isn't valid SteamID for identity.", identityRemote.GetSteamID().Render() );
		return k_EResultInvalidSteamID;
	}

	SteamDatagramTransportLock scopeLock( "SendMessageToUser" );
	SteamNetworkingMessagesSession *pSess = FindOrCreateSession( identityRemote );
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Check on connection if needed
	pSess->CheckConnection( usecNow );

	CSteamNetworkConnectionBase *pConn = pSess->m_pConnection;
	if ( pConn )
	{

		// Implicit accept?
		if ( pConn->m_bConnectionInitiatedRemotely && pConn->GetState() == k_ESteamNetworkingConnectionState_Connecting )
		{
			SpewVerbose( "Messages session %s: Implicitly accepted connection %s via SendMessageToUser\n", SteamNetworkingIdentityRender( identityRemote ).c_str(), pConn->GetDescription() );
			pConn->APIAcceptConnection();
			pSess->UpdateConnectionInfo();
		}
	}
	else
	{

		// No active connection.
		// Did the previous one fail?
		SteamNetConnectionInfo_t &info = pSess->m_lastConnectionInfo;
		if ( info.m_eState != k_ESteamNetworkingConnectionState_None )
		{
			if ( !( nSendFlags & k_nSteamNetworkingSend_AutoRestartBrokenSession ) )
			{
				SpewVerbose( "Previous messages connection %s broken (%d, %s), rejecting SendMessageToUser\n",
					info.m_szConnectionDescription, info.m_eEndReason, info.m_szEndDebug );
				return k_EResultConnectFailed;
			}

			SpewVerbose( "Previous messages connection %s broken (%d, %s), restarting session as per AutoRestartBrokenSession\n",
				info.m_szConnectionDescription, info.m_eEndReason, info.m_szEndDebug );
			memset( &info, 0, sizeof(info) );
			memset( &pSess->m_lastQuickStatus, 0, sizeof(pSess->m_lastQuickStatus) );
		}

		// Try to create one
		SteamNetworkingConfigValue_t opt[2];
		opt[0].SetPtr( k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)ConnectionStatusChangedCallback );
		opt[1].SetInt32( k_ESteamNetworkingConfig_SymmetricConnect, 1 );
		pConn = m_steamNetworkingSockets.InternalConnectP2PDefaultSignaling( identityRemote, k_nVirtualPort_Messages, 2, opt );
		if ( !pConn )
		{
			AssertMsg( false, "Failed to create connection to '%s' for new messages session", SteamNetworkingIdentityRender( identityRemote ).c_str() );
			return k_EResultFail;
		}

		SpewVerbose( "[%s] Created connection for messages session\n", pConn->GetDescription() );
		pSess->LinkConnection( pConn );
	}

	// KLUDGE Old P2P always sent messages that had to be queued reliably!
	// (It had to do with better buffering or something.)  If we change this,
	// we are almost certainly going to break some games that depend on it.
	// Yes, this is kind of crazy, we should try to scope it tighter.
	if ( pConn->GetState() != k_ESteamNetworkingConnectionState_Connected )
		nSendFlags = k_nSteamNetworkingSend_Reliable;

	// Allocate a message, and put our header in front.
	int cbSend = cubData + sizeof(P2PMessageHeader);
	CSteamNetworkingMessage *pMsg = (CSteamNetworkingMessage *)m_steamNetworkingSockets.m_pSteamNetworkingUtils->AllocateMessage( cbSend );
	if ( !pMsg )
	{
		pSess->m_pConnection->ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_AppException_Generic, "Failed to allocate message" );
		return k_EResultFail;
	}
	pMsg->m_nFlags = nSendFlags;

	P2PMessageHeader *hdr = static_cast<P2PMessageHeader *>( pMsg->m_pData );
	hdr->m_nFlags = 1;
	hdr->m_nToChannel = LittleDWord( nRemoteChannel );
	memcpy( hdr+1, pubData, cubData );

	// Reset idle timeout, schedule a wakeup call
	pSess->MarkUsed( usecNow );

	// Send it
	int64 nMsgNumberOrResult = pConn->_APISendMessageToConnection( pMsg, usecNow, nullptr );
	if ( nMsgNumberOrResult > 0 )
		return k_EResultOK;
	return EResult( -nMsgNumberOrResult );
}

int CSteamNetworkingMessages::ReceiveMessagesOnChannel( int nLocalChannel, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	SteamDatagramTransportLock scopeLock( "ReceiveMessagesOnChannel" );

	// Pull out all messages from the poll group into per-channel queues
	if ( m_pPollGroup )
	{
		for (;;)
		{
			CSteamNetworkingMessage *pMsg = m_pPollGroup->m_queueRecvMessages.m_pFirst;
			if ( !pMsg )
				break;
			pMsg->Unlink();

			int idxSession = g_mapSessionsByConnection.Find( pMsg->m_conn );
			if ( idxSession == g_mapSessionsByConnection.InvalidIndex() )
			{
				pMsg->Release();
				continue;
			}

			SteamNetworkingMessagesSession *pSess = g_mapSessionsByConnection[ idxSession ];
			Assert( pSess->m_pConnection );
			Assert( this == &pSess->m_steamNetworkingMessagesOwner );
			pSess->ReceivedMessage( pMsg );
		}
	}

	Channel *pChan = FindOrCreateChannel( nLocalChannel );
	return pChan->m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
}

bool CSteamNetworkingMessages::AcceptSessionWithUser( const SteamNetworkingIdentity &identityRemote )
{
	SteamDatagramTransportLock scopeLock( "AcceptSessionWithUser" );
	SteamNetworkingMessagesSession *pSession = FindSession( identityRemote );
	if ( !pSession )
		return false;

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Then there should be a connection
	CSteamNetworkConnectionBase *pConn = pSession->m_pConnection;
	if ( !pConn )
		return false;
	if ( pConn->m_bConnectionInitiatedRemotely && pConn->GetState() == k_ESteamNetworkingConnectionState_Connecting )
		pConn->APIAcceptConnection();
	pSession->MarkUsed( usecNow );
	return true;
}

bool CSteamNetworkingMessages::CloseSessionWithUser( const SteamNetworkingIdentity &identityRemote )
{
	SteamDatagramTransportLock scopeLock( "CloseSessionWithUser" );
	SteamNetworkingMessagesSession *pSession = FindSession( identityRemote );
	if ( !pSession )
		return false;

	pSession->CloseConnection( k_ESteamNetConnectionEnd_P2P_SessionClosed, "CloseSessionWithUser" );

	DestroySession( identityRemote );
	return true;
}

bool CSteamNetworkingMessages::CloseChannelWithUser( const SteamNetworkingIdentity &identityRemote, int nChannel )
{
	SteamDatagramTransportLock scopeLock( "CloseChannelWithUser" );
	SteamNetworkingMessagesSession *pSession = FindSession( identityRemote );
	if ( !pSession )
		return false;

	// Did we even have that channel open with this user?
	int h = pSession->m_mapOpenChannels.Find( nChannel );
	if ( h == pSession->m_mapOpenChannels.InvalidIndex() )
		return false;
	pSession->m_mapOpenChannels.RemoveAt(h);

	// Destroy all unread messages on this channel from this user
	CSteamNetworkingMessage **ppMsg = &pSession->m_queueRecvMessages.m_pFirst;
	for (;;)
	{
		CSteamNetworkingMessage *pMsg = *ppMsg;
		if ( pMsg == nullptr )
			break;
		Assert( pMsg->m_identityPeer == identityRemote );
		if ( pMsg->GetChannel() == nChannel )
		{
			pMsg->Unlink();
			Assert( *ppMsg != pMsg );
			pMsg->Release();
		}
		else
		{
			ppMsg = &pMsg->m_links.m_pPrev;
		}
	}

	// No more open channels?
	if ( pSession->m_mapOpenChannels.Count() == 0 )
		CloseSessionWithUser( identityRemote );
	return true;
}

ESteamNetworkingConnectionState CSteamNetworkingMessages::GetSessionConnectionInfo( const SteamNetworkingIdentity &identityRemote, SteamNetConnectionInfo_t *pConnectionInfo, SteamNetworkingQuickConnectionStatus *pQuickStatus )
{
	SteamDatagramTransportLock scopeLock( "GetSessionConnectionInfo" );
	if ( pConnectionInfo )
		memset( pConnectionInfo, 0, sizeof(*pConnectionInfo) );
	if ( pQuickStatus )
		memset( pQuickStatus, 0, sizeof(*pQuickStatus) );

	SteamNetworkingMessagesSession *pSess = FindSession( identityRemote );
	if ( pSess == nullptr )
		return k_ESteamNetworkingConnectionState_None;

	pSess->UpdateConnectionInfo();

	if ( pConnectionInfo )
		*pConnectionInfo = pSess->m_lastConnectionInfo;
	if ( pQuickStatus )
		*pQuickStatus = pSess->m_lastQuickStatus;

	return pSess->m_lastConnectionInfo.m_eState;
}

SteamNetworkingMessagesSession *CSteamNetworkingMessages::FindSession( const SteamNetworkingIdentity &identityRemote )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();
	int h = m_mapSessions.Find( identityRemote );
	if ( h == m_mapSessions.InvalidIndex() )
		return nullptr;
	SteamNetworkingMessagesSession *pResult = m_mapSessions[ h ];
	Assert( pResult->m_identityRemote == identityRemote );
	return pResult;
}

SteamNetworkingMessagesSession *CSteamNetworkingMessages::FindOrCreateSession( const SteamNetworkingIdentity &identityRemote )
{
	SteamNetworkingMessagesSession *pResult = FindSession( identityRemote );
	if ( !pResult )
	{
		SpewVerbose( "Messages session %s: created\n", SteamNetworkingIdentityRender( identityRemote ).c_str() );
		pResult = new SteamNetworkingMessagesSession( identityRemote, *this );
		m_mapSessions.Insert( identityRemote, pResult );
	}

	return pResult;
}

CSteamNetworkingMessages::Channel *CSteamNetworkingMessages::FindOrCreateChannel( int nChannel )
{
	int h = m_mapChannels.Find( nChannel );
	if ( h != m_mapChannels.InvalidIndex() )
		return m_mapChannels[h];
	Channel *pChan = new Channel;
	m_mapChannels.Insert( nChannel, pChan );
	return pChan;
}

void CSteamNetworkingMessages::DestroySession( const SteamNetworkingIdentity &identityRemote )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread( "CSteamNetworkingMessages::DestroySession" );
	int h = m_mapSessions.Find( identityRemote );
	if ( h == m_mapSessions.InvalidIndex() )
		return;
	SteamNetworkingMessagesSession *pSess = m_mapSessions[ h ];
	Assert( pSess->m_identityRemote == identityRemote );

	// Remove from table
	m_mapSessions[ h ] = nullptr;
	m_mapSessions.RemoveAt( h );

	// Nuke session memory
	delete pSess;
}

void CSteamNetworkingMessages::NewConnection( CSteamNetworkConnectionBase *pConn )
{
	// All of our connections should have this flag set
	Assert( pConn->BSymmetricMode() );

	// Check if we already have a session with an open connection
	SteamNetworkingMessagesSession *pSess = FindOrCreateSession( pConn->m_identityRemote );
	if ( pSess->m_pConnection )
	{
		AssertMsg( false, "Got incoming messages session connection request when we already had a connection.  This could happen legit, but we aren't handling it right now." );
		pConn->QueueDestroy();
		return;
	}

	// Setup the association
	pSess->LinkConnection( pConn );

	// Post a callback
	SteamNetworkingMessagesSessionRequest_t callback;
	callback.m_identityRemote = pConn->m_identityRemote;
	m_steamNetworkingSockets.QueueCallback( callback, g_Config_Callback_MessagesSessionRequest.Get() );
}

#ifdef DBGFLAG_VALIDATE

void CSteamNetworkingMessages::Validate( CValidator &validator, const char *pchName )
{
	ValidateRecursive( m_mapSessions );
	ValidateRecursive( m_mapChannels );
}

#endif

/////////////////////////////////////////////////////////////////////////////
//
// SteamNetworkingMessagesSession
//
/////////////////////////////////////////////////////////////////////////////

SteamNetworkingMessagesSession::SteamNetworkingMessagesSession( const SteamNetworkingIdentity &identityRemote, CSteamNetworkingMessages &steamNetworkingP2P )
: m_steamNetworkingMessagesOwner( steamNetworkingP2P )
, m_identityRemote( identityRemote )
{
	m_pConnection = nullptr;
	m_bConnectionStateChanged = false;
	m_bConnectionWasEverConnected = false;

	memset( &m_lastConnectionInfo, 0, sizeof(m_lastConnectionInfo) );
	memset( &m_lastQuickStatus, 0, sizeof(m_lastQuickStatus) );

	MarkUsed( SteamNetworkingSockets_GetLocalTimestamp() );
}

SteamNetworkingMessagesSession::~SteamNetworkingMessagesSession()
{
	// Discard messages
	m_queueRecvMessages.PurgeMessages();

	// If we have a connection, then nuke it now
	CloseConnection( k_ESteamNetConnectionEnd_P2P_SessionClosed, "P2PSession destroyed" );
}

void SteamNetworkingMessagesSession::CloseConnection( int nReason, const char *pszDebug )
{
	CSteamNetworkConnectionBase *pConn = m_pConnection;
	if ( pConn )
	{
		UpdateConnectionInfo();
		UnlinkConnection();
		pConn->APICloseConnection( nReason, pszDebug, false );
	}
	ScheduleThink();
}

void SteamNetworkingMessagesSession::MarkUsed( SteamNetworkingMicroseconds usecNow )
{
	m_usecIdleTimeout = usecNow + k_usecSteamNetworkingP2PSessionIdleTimeout;
	ScheduleThink();
}

void SteamNetworkingMessagesSession::ScheduleThink()
{
	Assert( m_usecIdleTimeout > 0 ); // We should always have an idle timeout set!
	EnsureMinThinkTime( m_usecIdleTimeout );
}

void SteamNetworkingMessagesSession::UpdateConnectionInfo()
{
	if ( !m_pConnection )
		return;
	if ( CollapseConnectionStateToAPIState( m_pConnection->GetState() ) == k_ESteamNetworkingConnectionState_None )
		return;
	m_pConnection->ConnectionPopulateInfo( m_lastConnectionInfo );
	m_lastConnectionInfo.m_hListenSocket = k_HSteamListenSocket_Invalid; // Always clear this, we don't want users of the API to know this is a thing
	m_pConnection->APIGetQuickConnectionStatus( m_lastQuickStatus );
	if ( m_lastConnectionInfo.m_eState == k_ESteamNetworkingConnectionState_Connected )
		m_bConnectionWasEverConnected = true;
}

void SteamNetworkingMessagesSession::CheckConnection( SteamNetworkingMicroseconds usecNow )
{
	if ( !m_pConnection || !m_bConnectionStateChanged )
		return;

	UpdateConnectionInfo();

	bool bIdle = !m_pConnection->SNP_BHasAnyBufferedRecvData()
		&& !m_pConnection->SNP_BHasAnyUnackedSentReliableData();

	// Check if the connection died
	if ( m_lastConnectionInfo.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally || m_lastConnectionInfo.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer )
	{
		SpewVerbose( "[%s] messages session %s: %d %s\n",
			m_lastConnectionInfo.m_szConnectionDescription,
			m_lastConnectionInfo.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally ? "problem detected locally" : "closed by peer",
			(int)m_lastConnectionInfo.m_eEndReason, m_lastConnectionInfo.m_szEndDebug );
		if ( bIdle && m_bConnectionWasEverConnected )
		{
			SpewVerbose( "    (But connection is idle, so treating this as idle timeout on our end.)" );
			memset( &m_lastConnectionInfo, 0, sizeof(m_lastConnectionInfo) );
			memset( &m_lastQuickStatus, 0, sizeof(m_lastQuickStatus) );
		}
		else
		{
			// Post failure callback.
			SpewVerbose( "[%s] Posting SteamNetworkingMessagesSessionFailed_t\n", m_lastConnectionInfo.m_szConnectionDescription );
			SteamNetworkingMessagesSessionFailed_t callback;
			callback.m_info = m_lastConnectionInfo;
			m_steamNetworkingMessagesOwner.m_steamNetworkingSockets.QueueCallback( callback, g_Config_Callback_MessagesSessionFailed.Get() );
		}

		// Clean up the connection.
		CSteamNetworkConnectionBase *pConn = m_pConnection;
		UnlinkConnection();
		pConn->ConnectionState_FinWait();
	}

	m_bConnectionStateChanged = false;
}

void SteamNetworkingMessagesSession::Think( SteamNetworkingMicroseconds usecNow )
{

	// Check on the connection
	CheckConnection( usecNow );

	// Time to idle out the session?
	if ( usecNow >= m_usecIdleTimeout )
	{
		// If we don't have a connection, then we can just self destruct now
		if ( !m_pConnection )
		{
			SpewMsg( "Messages session %s: idle timed out.  Destroying\n", SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
			m_steamNetworkingMessagesOwner.DestroySession( m_identityRemote );
			return;
		}

		// Make sure lower level connection is also idle and nothing is buffered.
		if ( m_pConnection->SNP_BHasAnyBufferedRecvData() )
		{
			// The peer hasn't started sending us data.  (Just not a complete message yet.)
			// This is a relatively small race condition.  Keep extending the timeout
			// until either the connection drops, or the full message gets delivered
			SpewMsg( "Messages session %s: connection [%s] is idle timing out, but we have a partial message from our peer.  Assuming a message was sent just at the timeout deadline.   Extending timeout.\n", SteamNetworkingIdentityRender( m_identityRemote ).c_str(), m_pConnection->GetDescription() );
			m_usecIdleTimeout = usecNow + k_nMillion;
		}
		else if ( m_pConnection->SNP_BHasAnyUnackedSentReliableData() )
		{
			// We *really* ought to think that the peer has acked all of our data.
			// Because our timeouts are really generous compared to ping times,
			// throughput, and max message size
			AssertMsg2( false, "Messages session %s: connection [%s] is idle timing out.  But we still have unacked sent data?!?  This seems bad\n", SteamNetworkingIdentityRender( m_identityRemote ).c_str(), m_pConnection->GetDescription() );
			m_usecIdleTimeout = usecNow + k_nMillion;
		}
		else
		{
			// We're idle.  Nuke the connection.  If the peer has tried to send us any messages,
			// they'll get the notification that we closed the message, and they can resend.
			// the thing is that they should know for sure that no partial messages were delivered,
			// so everything they have queued, they just need to resend.
			SpewMsg( "Messages session %s: idle timing out.  Closing connection [%s] and destroying session\n", SteamNetworkingIdentityRender( m_identityRemote ).c_str(), m_pConnection->GetDescription() );
			CloseConnection( k_ESteamNetConnectionEnd_P2P_SessionIdleTimeout, "Session Idle Timeout" );

			// Self-destruct
			m_steamNetworkingMessagesOwner.DestroySession( m_identityRemote );
			return;
		}
	}

	ScheduleThink();
}

static void FreeMessageDataWithP2PMessageHeader( SteamNetworkingMessage_t *pMsg )
{
	void *hdr = static_cast<P2PMessageHeader *>( pMsg->m_pData ) - 1;
	::free( hdr );
}

void SteamNetworkingMessagesSession::ReceivedMessage( CSteamNetworkingMessage *pMsg )
{

	// Make sure the message is big enough to contain a header
	if ( pMsg->m_cbSize < sizeof(P2PMessageHeader) )
	{
		AssertMsg2( false, "Internal P2P message from %s is %d bytes; that's not big enough for the header!", SteamNetworkingIdentityRender( m_identityRemote ).c_str(), pMsg->m_cbSize );
		pMsg->Release();
		return;
	}
	Assert( pMsg->m_pfnFreeData == CSteamNetworkingMessage::DefaultFreeData );

	// Process the header
	P2PMessageHeader *hdr = static_cast<P2PMessageHeader *>( pMsg->m_pData );
	pMsg->m_nChannel = LittleDWord( hdr->m_nToChannel );
	pMsg->m_cbSize -= sizeof(P2PMessageHeader);
	pMsg->m_pData = hdr+1;
	pMsg->m_pfnFreeData = FreeMessageDataWithP2PMessageHeader;

	// Add to the session
	pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_links, &m_queueRecvMessages );

	// Mark channel as open
	m_mapOpenChannels.Insert( pMsg->m_nChannel, true );

	// Add to end of channel queue
	CSteamNetworkingMessages::Channel *pChannel = m_steamNetworkingMessagesOwner.FindOrCreateChannel( pMsg->m_nChannel );
	pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_linksSecondaryQueue, &pChannel->m_queueRecvMessages );
}

void SteamNetworkingMessagesSession::ConnectionStateChanged( SteamNetConnectionStatusChangedCallback_t *pInfo )
{
	// If we are already disassociated from our session, then we don't care.
	if ( !m_pConnection )
	{
		AssertMsg( false, "SteamNetworkingMessagesSession::ConnectionStateChanged after detaching from connection?" );
		return;
	}
	Assert( m_pConnection->m_hConnectionSelf == pInfo->m_hConn );

	// If we're dead (about to be destroyed, entering finwait, etc, then unlink from session)
	ESteamNetworkingConnectionState eNewAPIState = pInfo->m_info.m_eState;
	if ( eNewAPIState == k_ESteamNetworkingConnectionState_None )
	{
		UnlinkConnection();
		return;
	}

	// Reset idle timeout if we connect
	if ( eNewAPIState == k_ESteamNetworkingConnectionState_Connecting || eNewAPIState == k_ESteamNetworkingConnectionState_Connected || eNewAPIState == k_ESteamNetworkingConnectionState_FindingRoute )
	{
		MarkUsed( SteamNetworkingSockets_GetLocalTimestamp() );
		if ( eNewAPIState == k_ESteamNetworkingConnectionState_Connected )
			m_bConnectionWasEverConnected = true;
	}

	// Schedule an immediate wakeup of the session, so we can deal with this
	// at a safe time
	m_bConnectionStateChanged = true;
	SetNextThinkTimeASAP();
}

void SteamNetworkingMessagesSession::LinkConnection( CSteamNetworkConnectionBase *pConn )
{
	UnlinkConnection();
	if ( !pConn )
		return;
	Assert( !g_mapSessionsByConnection.HasElement( pConn->m_hConnectionSelf ) );
	m_pConnection = pConn;
	g_mapSessionsByConnection.InsertOrReplace( pConn->m_hConnectionSelf, this );

	m_bConnectionStateChanged = true;
	m_bConnectionWasEverConnected = false;
	SetNextThinkTimeASAP();
	MarkUsed( SteamNetworkingSockets_GetLocalTimestamp() );

	pConn->SetPollGroup( m_steamNetworkingMessagesOwner.m_pPollGroup );
	UpdateConnectionInfo();
}

void SteamNetworkingMessagesSession::UnlinkConnection()
{
	if ( !m_pConnection )
		return;

	int h = g_mapSessionsByConnection.Find( m_pConnection->m_hConnectionSelf );
	if ( h == g_mapSessionsByConnection.InvalidIndex() || g_mapSessionsByConnection[h] != this )
	{
		AssertMsg( false, "Messages session bookkeeping bug" );
	}
	else
	{
		g_mapSessionsByConnection[h] = nullptr; // just for grins
		g_mapSessionsByConnection.RemoveAt( h );
	}

	m_pConnection = nullptr;
	m_bConnectionStateChanged = true;
	SetNextThinkTimeASAP();
}

#ifdef DBGFLAG_VALIDATE

void SteamNetworkingMessagesSession::Validate( CValidator &validator, const char *pchName )
{
	ValidateRecursive( m_mapOpenChannels );
	// FIXME: m_queueRecvMessages
}

#endif

} // namespace SteamNetworkingSocketsLib
using namespace SteamNetworkingSocketsLib;

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
