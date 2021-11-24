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

// These tables are protected by global lock
// FIXME - do they have to be?  We could probably use the table lock
// so that most API calls to existing sessions don't need to take
// the global lock
static CUtlHashMap<HSteamNetConnection,CMessagesEndPointSession*,std::equal_to<HSteamNetConnection>,std::hash<HSteamNetConnection>> g_mapSessionsByConnection;

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
		SteamNetworkingGlobalLock scopeLock;
		SteamNetworkingGlobalLock::SetLongLockWarningThresholdMS( "CreateSteamNetworkingMessages", 10 );
		m_pSteamNetworkingMessages = new CSteamNetworkingMessages( *this );
		if ( !m_pSteamNetworkingMessages->BInit() )
		{
			// NOTE: We re gonna keep trying to do this and failing repeatedly.
			m_pSteamNetworkingMessages->DestroyMessagesEndPoint();
			m_pSteamNetworkingMessages = nullptr;
		}
	}
	return m_pSteamNetworkingMessages;
}

/////////////////////////////////////////////////////////////////////////////
//
// CMessagesEndPoint
//
/////////////////////////////////////////////////////////////////////////////

CMessagesEndPoint::CMessagesEndPoint( CSteamNetworkingSockets &steamNetworkingSockets, int nLocalVirtualPort )
: m_steamNetworkingSockets( steamNetworkingSockets )
, m_nLocalVirtualPort( nLocalVirtualPort )
{
}

CMessagesEndPoint::~CMessagesEndPoint()
{
	m_sharedConnectionLock.AssertHeldByCurrentThread();
	Assert( !m_pListenSocket );
	m_sharedConnectionLock.unlock();
}

bool CMessagesEndPoint::BInit()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread("CMessagesEndPoint::BInit");

	if ( m_steamNetworkingSockets.m_mapMessagesEndpointByVirtualPort.HasElement( m_nLocalVirtualPort ) )
	{
		AssertMsg( false, "Tried to create multiple messages endopints on vport %d", m_nLocalVirtualPort );
		return false;
	}

	m_steamNetworkingSockets.m_mapMessagesEndpointByVirtualPort.Insert( m_nLocalVirtualPort, this );
	return true;
}

bool CMessagesEndPoint::BCreateListenSocket()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread("CMessagesEndPoint::BCreateListenSocket");
	Assert( !m_pListenSocket );

	// Create listen socket
	{
		SteamNetworkingConfigValue_t opt[1];
		opt[0].SetInt32( k_ESteamNetworkingConfig_SymmetricConnect, 1 );
		m_pListenSocket = m_steamNetworkingSockets.InternalCreateListenSocketP2P( m_nLocalVirtualPort, 1, opt );
		if ( !m_pListenSocket )
			return false;
	}

	Assert( !m_pListenSocket->m_pMessagesEndPointOwner );
	m_pListenSocket->m_pMessagesEndPointOwner = this;

	Assert( m_steamNetworkingSockets.m_mapListenSocketsByVirtualPort.HasElement( m_nLocalVirtualPort ) );

	return true;
}

void CMessagesEndPoint::FreeResources()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread("CMessagesEndPoint::FreeResources");
	m_sharedConnectionLock.AssertHeldByCurrentThread();

	// Destroy listen socket, if any
	if ( m_pListenSocket )
	{
		Assert( m_steamNetworkingSockets.m_mapListenSocketsByVirtualPort.HasElement( m_nLocalVirtualPort ) );

		m_pListenSocket->Destroy();
		m_pListenSocket = nullptr;

		Assert( !m_steamNetworkingSockets.m_mapListenSocketsByVirtualPort.HasElement( m_nLocalVirtualPort ) );
	}

	// Remove from map by virtual port, if we're in it
	int idx = m_steamNetworkingSockets.m_mapMessagesEndpointByVirtualPort.Find( m_nLocalVirtualPort );
	if ( idx != m_steamNetworkingSockets.m_mapMessagesEndpointByVirtualPort.InvalidIndex() )
	{
		if ( m_steamNetworkingSockets.m_mapMessagesEndpointByVirtualPort[idx] == this )
		{
			m_steamNetworkingSockets.m_mapMessagesEndpointByVirtualPort[idx] = nullptr; // Just for grins
			m_steamNetworkingSockets.m_mapMessagesEndpointByVirtualPort.RemoveAt( idx );
		}
	}
}

void CMessagesEndPoint::DestroyMessagesEndPoint()
{
	m_sharedConnectionLock.lock();
	FreeResources();
	delete this;
}

/////////////////////////////////////////////////////////////////////////////
//
// MessagesEndPointSession
//
/////////////////////////////////////////////////////////////////////////////

CMessagesEndPointSession::CMessagesEndPointSession(
	const SteamNetworkingIdentity &identityRemote,
	CMessagesEndPoint &endPoint )
	: ILockableThinker<ConnectionLock>( endPoint.m_sharedConnectionLock )
	, m_identityRemote( identityRemote )
	, m_messageEndPointOwner( endPoint )
{
	m_pConnection = nullptr;
	m_bAppScheduledTimeout = false;
	m_bConnectionWasEverConnected = false;

	MarkUsed( SteamNetworkingSockets_GetLocalTimestamp() );
}

CMessagesEndPointSession::~CMessagesEndPointSession()
{
	// Detach from all connections, we're about to be destroyed
	ClearActiveConnection();
	UnlinkFromInactiveConnections();
	Assert( m_vecLinkedConnections.empty() );
}

void CMessagesEndPointSession::MarkUsed( SteamNetworkingMicroseconds usecNow )
{
	m_usecIdleTimeout = usecNow + k_usecSteamNetworkingP2PSessionIdleTimeout;
	m_bAppScheduledTimeout = false;
	ScheduleThink();
}

void CMessagesEndPointSession::ScheduleThink()
{
	Assert( m_usecIdleTimeout > 0 ); // We should always have an idle timeout set!
	if ( len( m_vecLinkedConnections ) > 1 || ( m_pConnection && m_vecLinkedConnections[0] != m_pConnection ) )
		SetNextThinkTimeASAP(); // Unlink ASAP
	else
		EnsureMinThinkTime( m_usecIdleTimeout );
}

void CMessagesEndPointSession::UnlinkFromInactiveConnections()
{
	for ( int i = len( m_vecLinkedConnections )-1 ; i >= 0 ; --i )
	{
		if ( m_vecLinkedConnections[i] != m_pConnection )
		{
			UnlinkConnectionNow( m_vecLinkedConnections[i] );
		}
	}

	Assert( len( m_vecLinkedConnections ) == ( ( m_pConnection != nullptr ) ? 1 : 0 ) );
}

void CMessagesEndPointSession::UnlinkConnectionNow( CSteamNetworkConnectionBase *pConn )
{
	// Should only be doing this stuff when we hold the global lock and
	// our own lock.  HOWEVER - we might be called when cleaning up a
	// connection.  In that case, it's up to the connection to clean
	// up properly
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CMessagesEndPointSession::UnlinkConnectionNow" );
	m_pLock->AssertHeldByCurrentThread();

	Assert( pConn->m_pMessagesEndPointSessionOwner == this );

	// If it was the active connection, clear it
	if ( pConn == m_pConnection )
		ClearActiveConnection();

	// Connection should be inactive.  Make sure it cleans up properly
	AssertMsg( !pConn->BStateIsActive(), "[%s] Unlinking connection in state %d", pConn->GetDescription(), pConn->GetState() );
	pConn->ConnectionState_FinWait();

	// Remove from list of linked connections
	DbgVerify( find_and_remove_element( m_vecLinkedConnections, pConn ) );

	// Mark connection as no longer associated with this session
	pConn->m_pMessagesEndPointSessionOwner = nullptr;

	// Change connection back to using its own lock
	Assert( pConn->m_pLock == m_pLock );
	pConn->m_pLock = &pConn->m_defaultLock;

}

void CMessagesEndPointSession::SessionConnectionStateChanged( CSteamNetworkConnectionBase *pConn, ESteamNetworkingConnectionState eOldState )
{
	// This must be one of our linked connections
	Assert( pConn->m_pMessagesEndPointSessionOwner == this );
	Assert( has_element( m_vecLinkedConnections, pConn ) );

	// We're using a shared lock right now so we should already be locked!
	m_pLock->AssertHeldByCurrentThread();
	Assert( m_pLock == pConn->m_pLock );

	// A connection that thinks we are its owner has undergone
	// a state change.  Wake up and take action when it is
	// safe to do so.
	SetNextThinkTimeASAP();

	// And if this is our *active* connection (it usually will be),
	// then we might want to take some actions now
	if ( pConn == m_pConnection )
		ActiveConnectionStateChanged();
}

void CMessagesEndPointSession::ActiveConnectionStateChanged()
{

	// Reset idle timeout if we connect
	if ( m_pConnection->GetState() == k_ESteamNetworkingConnectionState_Connecting || m_pConnection->GetState() == k_ESteamNetworkingConnectionState_Connected || m_pConnection->GetState() == k_ESteamNetworkingConnectionState_FindingRoute )
	{
		MarkUsed( SteamNetworkingSockets_GetLocalTimestamp() );
		if ( m_pConnection->GetState() == k_ESteamNetworkingConnectionState_Connected )
			m_bConnectionWasEverConnected = true;
	}

	// Schedule an immediate wakeup of the session, so we can deal with this
	// at a safe time
	m_bConnectionStateChanged = true;
}

void CMessagesEndPointSession::SetActiveConnection( CSteamNetworkConnectionBase *pConn, ConnectionScopeLock &connectionLock )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CMessagesEndPointSession::SetActiveConnection" );
	m_pLock->AssertHeldByCurrentThread();

	ClearActiveConnection();

	Assert( pConn->m_pMessagesEndPointSessionOwner == nullptr );
	pConn->m_pMessagesEndPointSessionOwner = this;

	Assert( !g_mapSessionsByConnection.HasElement( pConn->m_hConnectionSelf ) );
	m_pConnection = pConn;
	g_mapSessionsByConnection.InsertOrReplace( pConn->m_hConnectionSelf, this );

	// Change connection to use the shared lock
	Assert( pConn->m_pLock == &pConn->m_defaultLock );
	pConn->m_pLock->AssertHeldByCurrentThread();
	Assert( connectionLock.BHoldsLock( *pConn->m_pLock ) );
	connectionLock.Abandon();
	connectionLock.Lock( *m_pLock );
	pConn->m_pLock->unlock();
	pConn->m_pLock = m_pLock;

	// Add to list of linked connections
	m_vecLinkedConnections.push_back( pConn );

	m_bConnectionStateChanged = true;
	m_bConnectionWasEverConnected = false;
	SetNextThinkTimeASAP();
	MarkUsed( SteamNetworkingSockets_GetLocalTimestamp() );
}

void CMessagesEndPointSession::ClearActiveConnection()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CMessagesEndPointSession::ClearActiveConnection" );
	m_pLock->AssertHeldByCurrentThread();

	if ( !m_pConnection )
		return;

	// They should still be using the shared lock!
	// (And we won't change this here!)
	Assert( m_pConnection->m_pLock == m_pLock );

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

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingMessages
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkingMessages::Channel::Channel()
{
	m_queueRecvMessages.m_pRequiredLock = &g_lockAllRecvMessageQueues;
		
}

CSteamNetworkingMessages::Channel::~Channel()
{
	ShortDurationScopeLock scopeLock( g_lockAllRecvMessageQueues );

	// Should be empty!
	Assert( m_queueRecvMessages.empty() );

	// But in case not
	m_queueRecvMessages.PurgeMessages();
}

CSteamNetworkingMessages::CSteamNetworkingMessages( CSteamNetworkingSockets &steamNetworkingSockets )
: CMessagesEndPoint( steamNetworkingSockets, k_nVirtualPort_Messages )
{
}

bool CSteamNetworkingMessages::BInit()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

	if ( !CMessagesEndPoint::BInit() )
		return false;
	if ( !BCreateListenSocket() )
		return false;

	return true;
}

void CSteamNetworkingMessages::FreeResources()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingMessages::FreeResources" );
	m_sharedConnectionLock.AssertHeldByCurrentThread();

	// Destroy all of our sessions.  This will detach all of our connections
	FOR_EACH_HASHMAP( m_mapSessions, i )
	{
		DestroySession( m_mapSessions.Key(i) );
	}
	Assert( m_mapSessions.Count() == 0 );
	m_mapSessions.Purge();
	m_mapChannels.PurgeAndDeleteElements();

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

	CMessagesEndPoint::FreeResources();
}


CSteamNetworkingMessages::~CSteamNetworkingMessages()
{
	// Must use DestroyMessagesEndPoint
	Assert( m_mapSessions.Count() == 0 );
	Assert( m_steamNetworkingSockets.m_pSteamNetworkingMessages == nullptr );
}

EResult CSteamNetworkingMessages::SendMessageToUser( const SteamNetworkingIdentity &identityRemote, const void *pubData, uint32 cubData, int nSendFlags, int nRemoteChannel )
{
	if ( identityRemote.IsInvalid() )
	{
		AssertMsg( false, "Identity isn't valid for Messages sessions." );
		return k_EResultFail;
	}

	SteamNetworkingGlobalLock scopeLock( "SendMessageToUser" ); // !SPEED! Can we avoid this?
	ConnectionScopeLock connectionLock;
	SteamNetworkingMessagesSession *pSess = FindOrCreateSession( identityRemote, connectionLock );
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
		SteamNetworkingConfigValue_t opt[1];
		opt[0].SetInt32( k_ESteamNetworkingConfig_SymmetricConnect, 1 );
		ConnectionScopeLock connectionLock2;
		pConn = m_steamNetworkingSockets.InternalConnectP2PDefaultSignaling( identityRemote, k_nVirtualPort_Messages, 1, opt, connectionLock2 );
		if ( !pConn )
		{
			AssertMsg( false, "Failed to create connection to '%s' for new messages session", SteamNetworkingIdentityRender( identityRemote ).c_str() );
			return k_EResultFail;
		}

		SpewVerbose( "[%s] Created connection for messages session\n", pConn->GetDescription() );
		pSess->SetActiveConnection( pConn, connectionLock2 );
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
	SteamNetworkingGlobalLock scopeLock( "ReceiveMessagesOnChannel" ); // !SPEED! Can we avoid this?

	Channel *pChan = FindOrCreateChannel( nLocalChannel );

	ShortDurationScopeLock lockMessageQueues( g_lockAllRecvMessageQueues );

	return pChan->m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
}

bool CSteamNetworkingMessages::AcceptSessionWithUser( const SteamNetworkingIdentity &identityRemote )
{
	SteamNetworkingGlobalLock scopeLock( "AcceptSessionWithUser" ); // This is required if we cause a connection state change
	ConnectionScopeLock connectionLock;
	SteamNetworkingMessagesSession *pSession = FindSession( identityRemote, connectionLock );
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
	SteamNetworkingGlobalLock scopeLock( "CloseSessionWithUser" ); // This is required if we cause a connection state change
	ConnectionScopeLock connectionLock;
	SteamNetworkingMessagesSession *pSession = FindSession( identityRemote, connectionLock );
	if ( !pSession )
		return false;

	pSession->CloseConnection( k_ESteamNetConnectionEnd_P2P_SessionClosed, "CloseSessionWithUser" );

	DestroySession( identityRemote );
	return true;
}

bool CSteamNetworkingMessages::CloseChannelWithUser( const SteamNetworkingIdentity &identityRemote, int nChannel )
{
	SteamNetworkingGlobalLock scopeLock( "CloseChannelWithUser" ); // This is required if we cause a connection state change
	ConnectionScopeLock connectionLock;
	SteamNetworkingMessagesSession *pSession = FindSession( identityRemote, connectionLock );
	if ( !pSession )
		return false;

	// Did we even have that channel open with this user?
	int h = pSession->m_mapOpenChannels.Find( nChannel );
	if ( h == pSession->m_mapOpenChannels.InvalidIndex() )
		return false;
	pSession->m_mapOpenChannels.RemoveAt(h);

	// Destroy all unread messages on this channel from this user
	g_lockAllRecvMessageQueues.lock();
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
	g_lockAllRecvMessageQueues.unlock();

	// No more open channels?
	if ( pSession->m_mapOpenChannels.Count() == 0 )
		CloseSessionWithUser( identityRemote );
	return true;
}

ESteamNetworkingConnectionState CSteamNetworkingMessages::GetSessionConnectionInfo( const SteamNetworkingIdentity &identityRemote, SteamNetConnectionInfo_t *pConnectionInfo, SteamNetConnectionRealTimeStatus_t *pRealTimeStatus )
{
	SteamNetworkingGlobalLock scopeLock( "GetSessionConnectionInfo" ); // !SPEED! We should be able to get rid of this!
	if ( pConnectionInfo )
		memset( pConnectionInfo, 0, sizeof(*pConnectionInfo) );
	if ( pRealTimeStatus )
		memset( pRealTimeStatus, 0, sizeof(*pRealTimeStatus) );

	ConnectionScopeLock connectionLock;
	SteamNetworkingMessagesSession *pSess = FindSession( identityRemote, connectionLock );
	if ( pSess == nullptr )
		return k_ESteamNetworkingConnectionState_None;

	pSess->UpdateConnectionInfo();

	if ( pConnectionInfo )
		*pConnectionInfo = pSess->m_lastConnectionInfo;
	if ( pRealTimeStatus )
		*pRealTimeStatus = pSess->m_lastQuickStatus;

	return pSess->m_lastConnectionInfo.m_eState;
}

SteamNetworkingMessagesSession *CSteamNetworkingMessages::FindSession( const SteamNetworkingIdentity &identityRemote, ConnectionScopeLock &connectionLock )
{
	Assert( !connectionLock.IsLocked() );
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread(); // !SPEED! Could we use a more tightly scoped lock, perhaps the table lock here?
	int h = m_mapSessions.Find( identityRemote );
	if ( h == m_mapSessions.InvalidIndex() )
		return nullptr;
	SteamNetworkingMessagesSession *pResult = m_mapSessions[ h ];
	connectionLock.Lock( *pResult->m_pLock );

	Assert( pResult->m_identityRemote == identityRemote );
	Assert( !pResult->m_pConnection || pResult->m_pConnection->m_pLock == pResult->m_pLock );

	return pResult;
}

SteamNetworkingMessagesSession *CSteamNetworkingMessages::FindOrCreateSession( const SteamNetworkingIdentity &identityRemote, ConnectionScopeLock &connectionLock )
{
	SteamNetworkingMessagesSession *pResult = FindSession( identityRemote, connectionLock );
	if ( !pResult )
	{
		SpewVerbose( "Messages session %s: created\n", SteamNetworkingIdentityRender( identityRemote ).c_str() );
		pResult = new SteamNetworkingMessagesSession( identityRemote, *this );
		connectionLock.Lock( *pResult->m_pLock );
		m_mapSessions.Insert( identityRemote, pResult );
	}

	return pResult;
}

CSteamNetworkingMessages::Channel *CSteamNetworkingMessages::FindOrCreateChannel( int nChannel )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingMessages::FindOrCreateChannel" );

	int h = m_mapChannels.Find( nChannel );
	if ( h != m_mapChannels.InvalidIndex() )
		return m_mapChannels[h];
	Channel *pChan = new Channel;
	m_mapChannels.Insert( nChannel, pChan );
	return pChan;
}

void CSteamNetworkingMessages::DestroySession( const SteamNetworkingIdentity &identityRemote )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingMessages::DestroySession" );
	int h = m_mapSessions.Find( identityRemote );
	if ( h == m_mapSessions.InvalidIndex() )
		return;
	SteamNetworkingMessagesSession *pSess = m_mapSessions[ h ];
	Assert( pSess->m_identityRemote == identityRemote );

	// Hold session/connection lock while we do this.  We'll need to review this
	// code and be a bit more particular if we move away from a shared lock
	Assert( pSess->m_pLock == &m_sharedConnectionLock );
	ConnectionScopeLock connectionLock( m_sharedConnectionLock );

	// Remove from table
	m_mapSessions[ h ] = nullptr;
	m_mapSessions.RemoveAt( h );

	// Nuke session memory
	delete pSess;
}

bool CSteamNetworkingMessages::BHandleNewIncomingConnection( CSteamNetworkConnectionBase *pConn, ConnectionScopeLock &connectionLock )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingMessages::BHandleNewIncomingConnection" ); // New connections can only be created while the global lock is held

	// Caller's scope lock object should hold the current lock
	Assert( connectionLock.BHoldsLock( *pConn->m_pLock ) );

	// All of our connections should have this flag set
	Assert( pConn->BSymmetricMode() );

	// Check if we already have a session with an open connection
	ConnectionScopeLock sessionLock;
	SteamNetworkingMessagesSession *pSess = FindOrCreateSession( pConn->m_identityRemote, sessionLock );
	if ( pSess->m_pConnection )
	{
		AssertMsg( false, "Got incoming messages session connection request when we already had a connection.  This could happen legit, but we aren't handling it right now." );
		return false;
	}

	// Setup the association
	pSess->SetActiveConnection( pConn, connectionLock );

	// Post a callback
	SteamNetworkingMessagesSessionRequest_t callback;
	callback.m_identityRemote = pConn->m_identityRemote;
	m_steamNetworkingSockets.QueueCallback( callback, g_Config_Callback_MessagesSessionRequest.Get() );

	return true;
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

SteamNetworkingMessagesSession::SteamNetworkingMessagesSession( const SteamNetworkingIdentity &identityRemote, CSteamNetworkingMessages &steamNetworkingMessages )
: CMessagesEndPointSession( identityRemote, steamNetworkingMessages )
{
	m_bConnectionStateChanged = false;

	memset( &m_lastConnectionInfo, 0, sizeof(m_lastConnectionInfo) );
	memset( &m_lastQuickStatus, 0, sizeof(m_lastQuickStatus) );

	m_queueRecvMessages.m_pRequiredLock = &g_lockAllRecvMessageQueues;
}

SteamNetworkingMessagesSession::~SteamNetworkingMessagesSession()
{
	// Discard messages
	g_lockAllRecvMessageQueues.lock();
	m_queueRecvMessages.PurgeMessages();
	g_lockAllRecvMessageQueues.unlock();

	// If we have a connection, then nuke it now
	CloseConnection( k_ESteamNetConnectionEnd_P2P_SessionClosed, "P2PSession destroyed" );
}

void SteamNetworkingMessagesSession::CloseConnection( int nReason, const char *pszDebug )
{
	CSteamNetworkConnectionBase *pConn = m_pConnection;
	if ( pConn )
	{
		UpdateConnectionInfo();
		ClearActiveConnection();
		pConn->APICloseConnection( nReason, pszDebug, false );
	}
	SetNextThinkTimeASAP();
}

void SteamNetworkingMessagesSession::UpdateConnectionInfo()
{
	if ( !m_pConnection )
		return;
	if ( CollapseConnectionStateToAPIState( m_pConnection->GetState() ) == k_ESteamNetworkingConnectionState_None )
		return;
	m_pConnection->ConnectionPopulateInfo( m_lastConnectionInfo );
	m_lastConnectionInfo.m_hListenSocket = k_HSteamListenSocket_Invalid; // Always clear this, we don't want users of the API to know this is a thing
	m_pConnection->APIGetRealTimeStatus( &m_lastQuickStatus, 0, nullptr );
	if ( m_lastConnectionInfo.m_eState == k_ESteamNetworkingConnectionState_Connected )
		m_bConnectionWasEverConnected = true;
}

void SteamNetworkingMessagesSession::SetActiveConnection( CSteamNetworkConnectionBase *pConn, ConnectionScopeLock &connectionLock )
{
	CMessagesEndPointSession::SetActiveConnection( pConn, connectionLock );
	UpdateConnectionInfo();
}

void SteamNetworkingMessagesSession::ActiveConnectionStateChanged()
{
	UpdateConnectionInfo();
	CMessagesEndPointSession::ActiveConnectionStateChanged();
}

void SteamNetworkingMessagesSession::CheckConnection( SteamNetworkingMicroseconds usecNow )
{
	if ( !m_pConnection || !m_bConnectionStateChanged )
		return;

	UpdateConnectionInfo();

	// Safety check in case the connection got nuked without going thorugh an expected terminal state
	if ( !m_pConnection->BStateIsActive() )
	{
		if ( m_lastConnectionInfo.m_eState != k_ESteamNetworkingConnectionState_ProblemDetectedLocally && m_lastConnectionInfo.m_eState != k_ESteamNetworkingConnectionState_ClosedByPeer )
		{
			AssertMsg( false, "[%s] Connection now in state %d without ever passing through expected terminal states", m_pConnection->GetDescription(), m_lastConnectionInfo.m_eState );
			m_lastConnectionInfo.m_eState = k_ESteamNetworkingConnectionState_ProblemDetectedLocally;
			m_lastConnectionInfo.m_eEndReason = k_ESteamNetConnectionEnd_Misc_InternalError;
			V_strcpy_safe( m_lastConnectionInfo.m_szEndDebug, "Internal error" );
		}
	}

	// Check if the connection died
	if ( m_lastConnectionInfo.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally || m_lastConnectionInfo.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer )
	{
		bool bIdle = !m_pConnection->SNP_BHasAnyBufferedRecvData()
			&& !m_pConnection->SNP_BHasAnyUnackedSentReliableData();

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
			MessagesOwner().m_steamNetworkingSockets.QueueCallback( callback, g_Config_Callback_MessagesSessionFailed.Get() );
		}

		// Clean up the connection.
		UnlinkConnectionNow( m_pConnection );
	}

	m_bConnectionStateChanged = false;
}

void SteamNetworkingMessagesSession::Think( SteamNetworkingMicroseconds usecNow )
{
	ConnectionScopeLock scopeLock;
	scopeLock.TakeLockOwnership( m_pLock, "SteamNetworkingMessagesSession::Think" );

	// It's a safe time to try to unlink from any inactive connections
	UnlinkFromInactiveConnections();

	// Check on the connection
	CheckConnection( usecNow );

	// Time to idle out the session?
	if ( usecNow >= m_usecIdleTimeout )
	{
		// If we don't have a connection, then we can just self destruct now
		if ( !m_pConnection )
		{
			SpewMsg( "Messages session %s: idle timed out.  Destroying\n", SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
			MessagesOwner().DestroySession( m_identityRemote );
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
			MessagesOwner().DestroySession( m_identityRemote );
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

void SteamNetworkingMessagesSession::ReceivedMessage( CSteamNetworkingMessage *pMsg, CSteamNetworkConnectionBase *pConn )
{
	m_pLock->AssertHeldByCurrentThread();

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
	pMsg->m_identityPeer = pConn->m_identityRemote;
	pMsg->m_nChannel = LittleDWord( hdr->m_nToChannel );
	pMsg->m_cbSize -= sizeof(P2PMessageHeader);
	pMsg->m_pData = hdr+1;
	pMsg->m_conn = k_HSteamNetConnection_Invalid; // Invalidate this, we don't want app to think it's legit to access to the underlying connection
	pMsg->m_pfnFreeData = FreeMessageDataWithP2PMessageHeader;

	// Mark channel as open
	m_mapOpenChannels.Insert( pMsg->m_nChannel, true );
	CSteamNetworkingMessages::Channel *pChannel = MessagesOwner().FindOrCreateChannel( pMsg->m_nChannel );

	// Grab the lock while we insert into the proper queues
	ShortDurationScopeLock lockMessageQueues( g_lockAllRecvMessageQueues );

	// Add to the session
	pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_links, &m_queueRecvMessages );

	// Add to end of channel queue
	pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_linksSecondaryQueue, &pChannel->m_queueRecvMessages );
}

#ifdef DBGFLAG_VALIDATE

void SteamNetworkingMessagesSession::Validate( CValidator &validator, const char *pchName )
{
	ValidateRecursive( m_mapOpenChannels );
	// FIXME: m_queueRecvMessages
}

void CSteamNetworkingMessages::ValidateStatics( CValidator &validator )
{
	ValidateObj( g_mapSessionsByConnection ); // not recursive, we don't own these
}

#endif

} // namespace SteamNetworkingSocketsLib
using namespace SteamNetworkingSocketsLib;

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
