//====== Copyright Valve Corporation, All rights reserved. ====================

#include <time.h>

#include <steam/isteamnetworkingsockets.h>
#include "steamnetworkingsockets_connections.h"
#include "steamnetworkingsockets_lowlevel.h"
#include "../steamnetworkingsockets_certstore.h"
#include "csteamnetworkingsockets.h"
#include "crypto.h"

#include "tier0/memdbgoff.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
	#include "csteamnetworkingmessages.h"
#endif

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
	#include "../../common/steammessages_gamenetworkingui.pb.h"
#endif

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
	#include <steam/steamnetworkingfakeip.h>
#endif

#include <tier0/valve_tracelogging.h> // Includes windows.h :(  Include this last before memdbgon

TRACELOGGING_DECLARE_PROVIDER( HTraceLogging_SteamNetworkingSockets );

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifdef __GNUC__
	// error: assuming signed overflow does not occur when assuming that (X + c) < X is always false [-Werror=strict-overflow]
	// current steamrt:scout gcc "g++ (SteamRT 4.8.4-1ubuntu15~12.04+steamrt1.2+srt1) 4.8.4" requires this at the top due to optimizations
	#pragma GCC diagnostic ignored "-Wstrict-overflow"
#endif

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

const int k_nMaxRecentLocalConnectionIDs = 256;
static CUtlVectorFixed<uint16,k_nMaxRecentLocalConnectionIDs> s_vecRecentLocalConnectionIDs;

/// Check if we've sent a "spam reply", meaning a reply to an incoming
/// message that could be random spoofed garbage.  Returns false if we've
/// recently sent one and cannot send any more right now without risking
/// being taken advantage of.  Returns true if we haven't sent too many
/// such packets recently, and it's OK to send one now.  (If true is returned,
/// it's assumed that you will send one.)
bool BCheckGlobalSpamReplyRateLimit( SteamNetworkingMicroseconds usecNow )
{
	static SteamNetworkingMicroseconds s_usecLastSpamReplySent;
	if ( s_usecLastSpamReplySent + k_nMillion/4 > usecNow )
		return false;
	s_usecLastSpamReplySent = usecNow;
	return true;
}

// Hack code used to generate C++ code to add a new CA key to the table above
//void KludgePrintPublicKey()
//{
//	CECSigningPublicKey key;
//	char *x = strdup( "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJrsoE4XUc5iaNVpACyh4fobLbwm02tOo6AIOtNygpuE" );
//	DbgVerify( key.LoadFromAndWipeBuffer( x, strlen(x) ) );
//	CUtlStringBuilder bufText;
//	for ( uint32 i = 0 ; i < key.GetLength() ; ++i )
//	{
//		bufText.AppendFormat("\\x%02x", key.GetData()[i] );
//	}
//	SHA256Digest_t digest;
//	CCrypto::GenerateSHA256Digest( key.GetData(), key.GetLength(), &digest );
//	SpewWarning( "TrustedKey( %llullu, \"%s\" )\n", LittleQWord( *(uint64*)&digest ), bufText.String() );
//}

/////////////////////////////////////////////////////////////////////////////
//
// Message storage
//
/////////////////////////////////////////////////////////////////////////////

void CSteamNetworkingMessage::DefaultFreeData( SteamNetworkingMessage_t *pMsg )
{
	free( pMsg->m_pData );
}


void CSteamNetworkingMessage::ReleaseFunc( SteamNetworkingMessage_t *pIMsg )
{
	CSteamNetworkingMessage *pMsg = static_cast<CSteamNetworkingMessage *>( pIMsg );

	// Free up the buffer, if we have one
	if ( pMsg->m_pData && pMsg->m_pfnFreeData )
		(*pMsg->m_pfnFreeData)( pMsg );
	pMsg->m_pData = nullptr; // Just for grins

	// We must not currently be in any queue.  In fact, our parent
	// might have been destroyed.
	Assert( !pMsg->m_links.m_pQueue );
	Assert( !pMsg->m_links.m_pPrev );
	Assert( !pMsg->m_links.m_pNext );
	Assert( !pMsg->m_linksSecondaryQueue.m_pQueue );
	Assert( !pMsg->m_linksSecondaryQueue.m_pPrev );
	Assert( !pMsg->m_linksSecondaryQueue.m_pNext );

	// Self destruct
	// FIXME Should avoid this dynamic memory call with some sort of pooling
	delete pMsg;
}

CSteamNetworkingMessage *CSteamNetworkingMessage::New( uint32 cbSize )
{
	// FIXME Should avoid this dynamic memory call with some sort of pooling
	CSteamNetworkingMessage *pMsg = new CSteamNetworkingMessage;

	// NOTE: Intentionally not memsetting the whole thing;
	// this struct is pretty big.

	// Allocate buffer if requested
	if ( cbSize )
	{
		pMsg->m_pData = malloc( cbSize );
		if ( pMsg->m_pData == nullptr )
		{
			delete pMsg;
			SpewError( "Failed to allocate %d-byte message buffer", cbSize );
			return nullptr;
		}
		pMsg->m_cbSize = cbSize;
		pMsg->m_pfnFreeData = CSteamNetworkingMessage::DefaultFreeData;
	}
	else
	{
		pMsg->m_cbSize = 0;
		pMsg->m_pData = nullptr;
		pMsg->m_pfnFreeData = nullptr;
	}

	// Clear identity
	pMsg->m_conn = k_HSteamNetConnection_Invalid;
	pMsg->m_identityPeer.m_eType = k_ESteamNetworkingIdentityType_Invalid;
	pMsg->m_identityPeer.m_cbSize = 0;

	// Set the release function
	pMsg->m_pfnRelease = ReleaseFunc;

	// Clear these fields
	pMsg->m_nConnUserData = 0;
	pMsg->m_usecTimeReceived = 0;
	pMsg->m_nMessageNumber = 0;
	pMsg->m_nChannel = -1;
	pMsg->m_nFlags = 0;
	pMsg->m_idxLane = 0;
	pMsg->m_links.Clear();
	pMsg->m_linksSecondaryQueue.Clear();

	return pMsg;
}

void CSteamNetworkingMessage::Unlink()
{
	// Unlink from any queues we are in
	UnlinkFromQueue( &CSteamNetworkingMessage::m_links );
	UnlinkFromQueue( &CSteamNetworkingMessage::m_linksSecondaryQueue );
}

ShortDurationLock g_lockAllRecvMessageQueues( "all_recv_msg_queue" );

void SteamNetworkingMessageQueue::AssertLockHeld() const
{
	if ( m_pRequiredLock )
		m_pRequiredLock->AssertHeldByCurrentThread();
}

void SteamNetworkingMessageQueue::PurgeMessages()
{
	AssertLockHeld();
	while ( !empty() )
	{
		CSteamNetworkingMessage *pMsg = m_pFirst;
		pMsg->Unlink();
		Assert( m_pFirst != pMsg );
		pMsg->Release();
	}
	Assert( m_nMessageCount == 0 );
}

int SteamNetworkingMessageQueue::RemoveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	int nMessagesReturned = 0;
	AssertLockHeld();

	while ( !empty() && nMessagesReturned < nMaxMessages )
	{
		// Locate message, put into caller's list
		CSteamNetworkingMessage *pMsg = m_pFirst;
		ppOutMessages[nMessagesReturned++] = pMsg;

		// Unlink from all queues
		pMsg->Unlink();

		// That should have unlinked from *us*, so it shouldn't be in our queue anymore
		Assert( m_pFirst != pMsg );
	}

	return nMessagesReturned;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkPollGroup
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkPollGroup::CSteamNetworkPollGroup( CSteamNetworkingSockets *pInterface )
: m_pSteamNetworkingSocketsInterface( pInterface )
, m_hPollGroupSelf( k_HSteamListenSocket_Invalid )
{
	// Object creation is rare; to keep things simple we require the global lock
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

	m_queueRecvMessages.m_pRequiredLock = &g_lockAllRecvMessageQueues;
}

CSteamNetworkPollGroup::~CSteamNetworkPollGroup()
{
	// Object deletion is rare; to keep things simple we require the global lock
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	m_lock.AssertHeldByCurrentThread();

	FOR_EACH_VEC_BACK( m_vecConnections, i )
	{
		CSteamNetworkConnectionBase *pConn = m_vecConnections[i];
		ConnectionScopeLock connectionLock( *pConn ); // NOTE: It's OK to take more than one lock here, because we hold the global lock
		Assert( pConn->m_pPollGroup == this );
		pConn->RemoveFromPollGroup();
		Assert( m_vecConnections.Count() == i );
	}

	// We should not have any messages now!  but if we do, unlink them
	{
		ShortDurationScopeLock lockMessageQueues( g_lockAllRecvMessageQueues );
		Assert( m_queueRecvMessages.empty() );

		// But if we do, unlink them but leave them in the main queue.
		while ( !m_queueRecvMessages.empty() )
		{
			CSteamNetworkingMessage *pMsg = m_queueRecvMessages.m_pFirst;

			// The poll group queue is the "secondary queue"
			Assert( pMsg->m_linksSecondaryQueue.m_pQueue == &m_queueRecvMessages );

			// They should be in some other queue (for the connection) as the main queue.
			// That owns them and make sure they get deleted!
			Assert( pMsg->m_links.m_pQueue != nullptr );

			// OK, do the work
			pMsg->UnlinkFromQueue( &CSteamNetworkingMessage::m_linksSecondaryQueue );

			// Make sure it worked.
			Assert( pMsg != m_queueRecvMessages.m_pFirst );
		}
	}

	// Remove us from global table, if we're in it
	if ( m_hPollGroupSelf != k_HSteamNetPollGroup_Invalid )
	{
		g_tables_lock.AssertHeldByCurrentThread();
		int idx = m_hPollGroupSelf & 0xffff;
		if ( g_mapPollGroups.IsValidIndex( idx ) && g_mapPollGroups[ idx ] == this )
		{
			g_mapPollGroups[ idx ] = nullptr; // Just for grins
			g_mapPollGroups.RemoveAt( idx );
		}
		else
		{
			AssertMsg( false, "Poll group handle bookkeeping bug!" );
		}

		m_hPollGroupSelf = k_HSteamNetPollGroup_Invalid;
	}

	// Unlock, so our lock debugging doesn't complain.
	// Here we assume that we're only locked once
	m_lock.unlock();
}

void CSteamNetworkPollGroup::AssignHandleAndAddToGlobalTable()
{
	// Object creation is rare; to keep things simple we require the global lock
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	g_tables_lock.AssertHeldByCurrentThread();

	Assert( m_hPollGroupSelf == k_HSteamNetPollGroup_Invalid );

	// We actually don't do map "lookups".  We assume the number of listen sockets
	// is going to be reasonably small.
	static int s_nDummy;
	++s_nDummy;
	int idx = g_mapPollGroups.Insert( s_nDummy, this );
	Assert( idx < 0x1000 );

	// Use upper 15 bits as a connection sequence number, so that listen socket handles
	// are not reused within a short time period.
	// (The top bit is reserved, so that listen socket handles and poll group handles
	// come from a different namespace, so that we can immediately detect using the wrong
	// and make that bug more obvious.)
	static uint32 s_nUpperBits = 0;
	s_nUpperBits += 0x10000;
	if ( s_nUpperBits & 0x10000000 )
		s_nUpperBits = 0x10000;

	// Set the handle
	m_hPollGroupSelf = HSteamNetPollGroup( idx | s_nUpperBits | 0x80000000 );
}


/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketBase
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkListenSocketBase::CSteamNetworkListenSocketBase( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: m_hListenSocketSelf( k_HSteamListenSocket_Invalid )
, m_pSteamNetworkingSocketsInterface( pSteamNetworkingSocketsInterface )
{
	m_connectionConfig.Init( &pSteamNetworkingSocketsInterface->m_connectionConfig );
}

CSteamNetworkListenSocketBase::~CSteamNetworkListenSocketBase()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	AssertMsg( m_mapChildConnections.Count() == 0, "Destroy() not used properly" );

	// Remove us from global table, if we're in it
	if ( m_hListenSocketSelf != k_HSteamListenSocket_Invalid )
	{
		TableScopeLock tableScopeLock( g_tables_lock ); // NOTE: We can do this since listen sockets are protected by the global lock, not an object lock

		int idx = m_hListenSocketSelf & 0xffff;
		if ( g_mapListenSockets.IsValidIndex( idx ) && g_mapListenSockets[ idx ] == this )
		{
			g_mapListenSockets[ idx ] = nullptr; // Just for grins
			g_mapListenSockets.RemoveAt( idx );
		}
		else
		{
			AssertMsg( false, "Listen socket handle bookkeeping bug!" );
		}

		m_hListenSocketSelf = k_HSteamListenSocket_Invalid;
	}

	#ifdef STEAMNETWORKINGSOCKETS_STEAMCLIENT
		Assert( !m_pLegacyPollGroup ); // Should have been cleaned up by Destroy()
	#endif
}

bool CSteamNetworkListenSocketBase::BInitListenSocketCommon( int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

	Assert( m_hListenSocketSelf == k_HSteamListenSocket_Invalid );

	// Assign us a handle, and add us to the global table
	{

		// We actually don't do map "lookups".  We assume the number of listen sockets
		// is going to be reasonably small.
		static int s_nDummy;
		++s_nDummy;
		int idx = g_mapListenSockets.Insert( s_nDummy, this );
		Assert( idx < 0x1000 );

		// Use upper 15 bits as a connection sequence number, so that listen socket handles
		// are not reused within a short time period.
		// (The top bit is reserved, so that listen socket handles and poll group handles
		// come from a different namespace, so that we can immediately detect using the wrong
		// and make that bug more obvious.)
		static uint32 s_nUpperBits = 0;
		s_nUpperBits += 0x10000;
		if ( s_nUpperBits & 0x10000000 )
			s_nUpperBits = 0x10000;

		// Add it to our table of listen sockets
		m_hListenSocketSelf = HSteamListenSocket( idx | s_nUpperBits );
	}

	// Set options, if any
	if ( pOptions )
	{
		for ( int i = 0 ; i < nOptions ; ++i )
		{
			if ( !m_pSteamNetworkingSocketsInterface->m_pSteamNetworkingUtils->SetConfigValueStruct( pOptions[i], k_ESteamNetworkingConfig_ListenSocket, m_hListenSocketSelf ) )
			{
				V_sprintf_safe( errMsg, "Error setting option %d", pOptions[i].m_eValue );
				return false;
			}
		}
	}
	else if ( nOptions != 0 )
	{
		V_strcpy_safe( errMsg, "Options list is NULL, but nOptions != 0?" );
		return false;
	}

	// Check if symmetric is enabled, then make sure it's supported.
	// It cannot be changed after listen socket creation
	m_connectionConfig.SymmetricConnect.Lock();
	if ( BSymmetricMode() )
	{
		if ( !BSupportsSymmetricMode() )
		{
			V_strcpy_safe( errMsg, "Symmetric mode not supported" );
			return false;
		}
	}

	// OK
	return true;
}

void CSteamNetworkListenSocketBase::Destroy()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

	// Destroy all child connections
	FOR_EACH_HASHMAP( m_mapChildConnections, h )
	{
		CSteamNetworkConnectionBase *pChild = m_mapChildConnections[ h ];
		ConnectionScopeLock connectionLock( *pChild );
		Assert( pChild->m_pParentListenSocket == this );
		Assert( pChild->m_hSelfInParentListenSocketMap == h );

		int n = m_mapChildConnections.Count();
		pChild->ConnectionQueueDestroy();
		Assert( m_mapChildConnections.Count() == n-1 );
		(void)n; // Suppress warning if asserts aren't enabled
	}

	#ifdef STEAMNETWORKINGSOCKETS_STEAMCLIENT
	if ( m_pLegacyPollGroup )
	{
		m_pLegacyPollGroup->m_lock.lock(); // Don't use scope object.  It will unlock when we destruct
		m_pLegacyPollGroup.reset();
	}
	#endif

	// Self destruct
	delete this;
}

bool CSteamNetworkListenSocketBase::APIGetAddress( SteamNetworkingIPAddr *pAddress )
{
	// Base class doesn't know
	return false;
}

bool CSteamNetworkListenSocketBase::BSupportsSymmetricMode()
{
	return false;
}

bool CSteamNetworkListenSocketBase::BAddChildConnection( CSteamNetworkConnectionBase *pConn, SteamNetworkingErrMsg &errMsg )
{
	pConn->AssertLocksHeldByCurrentThread();

	// Safety check
	if ( pConn->m_pParentListenSocket || pConn->m_hSelfInParentListenSocketMap != -1 || pConn->m_hConnectionSelf != k_HSteamNetConnection_Invalid )
	{
		Assert( pConn->m_pParentListenSocket == nullptr );
		Assert( pConn->m_hSelfInParentListenSocketMap == -1 );
		Assert( pConn->m_hConnectionSelf == k_HSteamNetConnection_Invalid );
		V_sprintf_safe( errMsg, "Cannot add child connection - connection already has a parent or is in connection map?" );
		return false;
	}

	if ( pConn->m_identityRemote.IsInvalid() || !pConn->m_unConnectionIDRemote )
	{
		Assert( !pConn->m_identityRemote.IsInvalid() );
		Assert( pConn->m_unConnectionIDRemote );
		V_sprintf_safe( errMsg, "Cannot add child connection - connection not initialized with remote identity/ConnID" );
		return false;
	}

	RemoteConnectionKey_t key{ pConn->m_identityRemote, pConn->m_unConnectionIDRemote };
	if ( m_mapChildConnections.Find( key ) != m_mapChildConnections.InvalidIndex() )
	{
		V_sprintf_safe( errMsg, "Duplicate child connection!  %s %u", SteamNetworkingIdentityRender( pConn->m_identityRemote ).c_str(), pConn->m_unConnectionIDRemote );
		AssertMsg1( false, "%s", errMsg );
		return false;
	}

	// Setup linkage
	pConn->m_pParentListenSocket = this;
	pConn->m_hSelfInParentListenSocketMap = m_mapChildConnections.Insert( key, pConn );
	pConn->m_bConnectionInitiatedRemotely = true;

	// Connection configuration will inherit from us
	pConn->m_connectionConfig.Init( &m_connectionConfig );

	// If we are possibly providing an old interface that did not have poll groups,
	// add the connection to the default poll group.  (But note that certain use cases,
	// e.g. custom signaling, the poll group may have already been assigned by the app code.
	// Don't override it, if so.)
	#ifdef STEAMNETWORKINGSOCKETS_STEAMCLIENT
	if ( !pConn->m_pPollGroup )
	{
		if ( !m_pLegacyPollGroup )
			m_pLegacyPollGroup.reset( new CSteamNetworkPollGroup( m_pSteamNetworkingSocketsInterface ) );
		pConn->SetPollGroup( m_pLegacyPollGroup.get() );
	}
	#endif

	return true;
}

void CSteamNetworkListenSocketBase::AboutToDestroyChildConnection( CSteamNetworkConnectionBase *pConn )
{
	Assert( pConn->m_pParentListenSocket == this );
	int hChild = pConn->m_hSelfInParentListenSocketMap;

	pConn->m_pParentListenSocket = nullptr;
	pConn->m_hSelfInParentListenSocketMap = -1;

	if ( m_mapChildConnections[ hChild ] == pConn )
	{
		 m_mapChildConnections[ hChild ] = nullptr; // just for kicks
		 m_mapChildConnections.RemoveAt( hChild );
	}
	else
	{
		AssertMsg( false, "Listen socket child list corruption!" );
		FOR_EACH_HASHMAP( m_mapChildConnections, h )
		{
			if ( m_mapChildConnections[h] == pConn )
				m_mapChildConnections.RemoveAt(h);
		}
	}
}

/////////////////////////////////////////////////////////////////////////////
//
// Abstract connection classes
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkConnectionBase::CSteamNetworkConnectionBase( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, ConnectionScopeLock &scopeLock )
: ILockableThinker( m_defaultLock )
, m_pSteamNetworkingSocketsInterface( pSteamNetworkingSocketsInterface )
{
	m_hConnectionSelf = k_HSteamNetConnection_Invalid;
	m_eConnectionState = k_ESteamNetworkingConnectionState_None;
	m_eConnectionWireState = k_ESteamNetworkingConnectionState_None;
	m_usecWhenEnteredConnectionState = 0;
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
		m_usecWhenNextDiagnosticsUpdate = k_nThinkTime_Never;
	#endif
	m_usecWhenSentConnectRequest = 0;
	m_usecWhenCreated = 0;
	m_ulHandshakeRemoteTimestamp = 0;
	m_usecWhenReceivedHandshakeRemoteTimestamp = 0;
	m_eEndReason = k_ESteamNetConnectionEnd_Invalid;
	m_szEndDebug[0] = '\0';
	memset( &m_identityLocal, 0, sizeof(m_identityLocal) );
	memset( &m_identityRemote, 0, sizeof(m_identityRemote) );
	m_unConnectionIDLocal = 0;
	m_unConnectionIDRemote = 0;
	m_pParentListenSocket = nullptr;
	m_pPollGroup = nullptr;
	m_hSelfInParentListenSocketMap = -1;
	m_bCertHasIdentity = false;
	m_bCryptKeysValid = false;
	m_eNegotiatedCipher = k_ESteamNetworkingSocketsCipher_INVALID;
	m_cbEncryptionOverhead = k_cbAESGCMTagSize;
	memset( m_szAppName, 0, sizeof( m_szAppName ) );
	memset( m_szDescription, 0, sizeof( m_szDescription ) );
	m_bConnectionInitiatedRemotely = false;
	m_pTransport = nullptr;
	m_nSupressStateChangeCallbacks = 0;
 
	// Initialize configuration using parent interface for now.
	m_connectionConfig.Init( &m_pSteamNetworkingSocketsInterface->m_connectionConfig );

	// We should always hold the lock while initializing a connection
	m_pLock = &m_defaultLock;
	scopeLock.Lock( *m_pLock );
}

CSteamNetworkConnectionBase::~CSteamNetworkConnectionBase()
{
	Assert( m_eConnectionState == k_ESteamNetworkingConnectionState_Dead );
	Assert( m_eConnectionWireState == k_ESteamNetworkingConnectionState_Dead );
	Assert( m_queueRecvMessages.empty() );
	Assert( m_pParentListenSocket == nullptr );
	Assert( m_pMessagesEndPointSessionOwner == nullptr );

	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	g_tables_lock.AssertHeldByCurrentThread();

	// Remove from global connection list
	if ( m_hConnectionSelf != k_HSteamNetConnection_Invalid )
	{
		int idx = g_mapConnections.Find( uint16( m_hConnectionSelf ) );
		if ( idx == g_mapConnections.InvalidIndex() || g_mapConnections[ idx ] != this )
		{
			AssertMsg( false, "Connection list bookeeping corruption" );
			FOR_EACH_HASHMAP( g_mapConnections, i )
			{
				if ( g_mapConnections[i] == this )
					g_mapConnections.RemoveAt( i );
			}
		}
		else
		{
			g_mapConnections[ idx ] = nullptr; // Just for grins
			g_mapConnections.RemoveAt( idx );
		}

		m_hConnectionSelf = k_HSteamNetConnection_Invalid;
	}

	// Save connection ID so we avoid using the same thing in the very near future.
	if ( m_unConnectionIDLocal )
	{
		// Trim history to max.  If we're really cycling through connections fast, this
		// history won't be very useful, but that should be an extremely rare edge case,
		// and the worst thing that happens is that we have a higher chance of reusing
		// a connection ID that shares the same bottom 16 bits.
		while ( s_vecRecentLocalConnectionIDs.Count() >= k_nMaxRecentLocalConnectionIDs )
			s_vecRecentLocalConnectionIDs.Remove( 0 );
		s_vecRecentLocalConnectionIDs.AddToTail( (uint16)m_unConnectionIDLocal );

		// Clear it, since this function should be idempotent
		m_unConnectionIDLocal = 0;
	}
}

static std::vector<CSteamNetworkConnectionBase *> s_vecPendingDeleteConnections;
static ShortDurationLock s_lockPendingDeleteConnections( "connection_delete_queue" );

void CSteamNetworkConnectionBase::ConnectionQueueDestroy()
{
	AssertLocksHeldByCurrentThread();

	// Make sure all resources have been freed, etc
	FreeResources();

	// We don't need to be in the thinker list
	IThinker::ClearNextThinkTime();

	// Put into list
	s_lockPendingDeleteConnections.lock();
	s_vecPendingDeleteConnections.push_back(this);
	s_lockPendingDeleteConnections.unlock();
}

void CSteamNetworkConnectionBase::ProcessDeletionList()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	if ( s_vecPendingDeleteConnections.empty() )
		return;

	// Swap into a temp vector.  Our lock hygiene code doesn't
	// want us to take a ShortDurationLock and then take any
	// other locks.
	s_lockPendingDeleteConnections.lock();
	std::vector<CSteamNetworkConnectionBase *> vecTemp( std::move( s_vecPendingDeleteConnections ) );
	s_vecPendingDeleteConnections.clear();
	s_lockPendingDeleteConnections.unlock();

	// Now actually process the list.  We need the tables
	// lock in order to remove connections from global tables.
	TableScopeLock tablesLock( g_tables_lock );
	for ( CSteamNetworkConnectionBase *pConnection: vecTemp )
	{
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
			CMessagesEndPointSession *pMessagesSession = pConnection->m_pMessagesEndPointSessionOwner;
			if ( pMessagesSession )
			{
				ConnectionScopeLock scopeLock( *pMessagesSession->m_pLock );
				pMessagesSession->SetNextThinkTimeASAP();
				pMessagesSession->UnlinkConnectionNow( pConnection );
				Assert( pConnection->m_pMessagesEndPointSessionOwner == nullptr );
			}
		#endif
		delete pConnection;
	}
}

void CConnectionTransport::TransportDestroySelfNow()
{
	AssertLocksHeldByCurrentThread();

	// Call virtual functions while we still can
	TransportFreeResources();

	// Self destruct NOW
	delete this;
}

void CConnectionTransport::TransportFreeResources()
{
}

void CSteamNetworkConnectionBase::FreeResources()
{
	AssertLocksHeldByCurrentThread();

	// Make sure we're marked in the dead state, and also if we were in an
	// API-visible state, this will queue the state change notification
	// while we still know who our listen socket is (if any).
	//
	// NOTE: Once this happens, any table lookup that finds us will return NULL.
	// So we basically don't exist to you if all you have is a handle
	SetState( k_ESteamNetworkingConnectionState_Dead, SteamNetworkingSockets_GetLocalTimestamp() );

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		m_fakeIPRefRemote.Clear();
	#endif

	// Discard any messages that weren't retrieved
	g_lockAllRecvMessageQueues.lock();
	m_queueRecvMessages.PurgeMessages();
	g_lockAllRecvMessageQueues.unlock();

	// If we are in a poll group, remove us from the group
	RemoveFromPollGroup();

	// Detach from the listen socket that owns us, if any
	if ( m_pParentListenSocket )
		m_pParentListenSocket->AboutToDestroyChildConnection( this );

	// Make sure and clean out crypto keys and such now
	ClearCrypto();

	// Clean up our transport
	DestroyTransport();
}

void CSteamNetworkConnectionBase::DestroyTransport()
{
	AssertLocksHeldByCurrentThread( "DestroyTransport" );
	if ( m_pTransport )
	{
		m_pTransport->TransportDestroySelfNow();
		m_pTransport = nullptr;
	}
}

void CSteamNetworkConnectionBase::RemoveFromPollGroup()
{
	AssertLocksHeldByCurrentThread( "RemoveFromPollGroup" );
	if ( !m_pPollGroup )
		return;
	PollGroupScopeLock pollGroupLock( m_pPollGroup->m_lock );

	// Scan all of our messages, and make sure they are not in the secondary queue
	{
		ShortDurationScopeLock lockMessageQueues( g_lockAllRecvMessageQueues );
		for ( CSteamNetworkingMessage *pMsg = m_queueRecvMessages.m_pFirst ; pMsg ; pMsg = pMsg->m_links.m_pNext )
		{
			Assert( pMsg->m_links.m_pQueue == &m_queueRecvMessages );

			// It *should* be in the secondary queue of the poll group
			Assert( pMsg->m_linksSecondaryQueue.m_pQueue == &m_pPollGroup->m_queueRecvMessages );

			// OK, do the work
			pMsg->UnlinkFromQueue( &CSteamNetworkingMessage::m_linksSecondaryQueue );
		}
	}

	// Remove us from the poll group's list.  DbgVerify because we should be in the list!
	DbgVerify( m_pPollGroup->m_vecConnections.FindAndFastRemove( this ) );

	// We're not in a poll group anymore
	m_pPollGroup = nullptr;
}

void CSteamNetworkConnectionBase::SetPollGroup( CSteamNetworkPollGroup *pPollGroup )
{
	AssertLocksHeldByCurrentThread( "SetPollGroup" );

	// Quick early-out for no change
	if ( m_pPollGroup == pPollGroup )
		return;

	// Clearing it?
	if ( !pPollGroup )
	{
		RemoveFromPollGroup();
		return;
	}

	// Grab locks for old and new poll groups.  Remember, we can take multiple locks without
	// worrying about deadlock because we hold the global lock
	PollGroupScopeLock pollGroupLockNew( pPollGroup->m_lock );
	PollGroupScopeLock pollGroupLockOld;
	if ( m_pPollGroup )
		pollGroupLockOld.Lock( m_pPollGroup->m_lock );

	// Scan all messages that are already queued for this connection,
	// and insert them into the poll groups queue in the (approximate)
	// appropriate spot.  Using local timestamps should be really close
	// for ordering messages between different connections.  Remember
	// that the API very clearly does not provide strong guarantees
	// regarding ordering of messages from different connections, and
	// really anybody who is expecting or relying on such guarantees
	// is probably doing something wrong.
	{
		ShortDurationScopeLock lockMessageQueues( g_lockAllRecvMessageQueues );
		CSteamNetworkingMessage *pInsertBefore = pPollGroup->m_queueRecvMessages.m_pFirst;
		for ( CSteamNetworkingMessage *pMsg = m_queueRecvMessages.m_pFirst ; pMsg ; pMsg = pMsg->m_links.m_pNext )
		{
			Assert( pMsg->m_links.m_pQueue == &m_queueRecvMessages );

			// Unlink it from existing poll group queue, if any
			if ( pMsg->m_linksSecondaryQueue.m_pQueue )
			{
				Assert( m_pPollGroup && pMsg->m_linksSecondaryQueue.m_pQueue == &m_pPollGroup->m_queueRecvMessages );
				pMsg->UnlinkFromQueue( &CSteamNetworkingMessage::m_linksSecondaryQueue );
			}
			else
			{
				Assert( !m_pPollGroup );
			}

			// Scan forward in the poll group message queue, until we find the insertion point
			for (;;)
			{

				// End of queue?
				if ( !pInsertBefore )
				{
					pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_linksSecondaryQueue, &pPollGroup->m_queueRecvMessages );
					break;
				}

				Assert( pInsertBefore->m_linksSecondaryQueue.m_pQueue == &pPollGroup->m_queueRecvMessages );
				if ( pInsertBefore->m_usecTimeReceived > pMsg->m_usecTimeReceived )
				{
					pMsg->LinkBefore( pInsertBefore, &CSteamNetworkingMessage::m_linksSecondaryQueue, &pPollGroup->m_queueRecvMessages );
					break;
				}

				pInsertBefore = pInsertBefore->m_linksSecondaryQueue.m_pNext;
			}
		}
	}

	// Tell previous poll group, if any, that we are no longer with them
	if ( m_pPollGroup )
	{
		DbgVerify( m_pPollGroup->m_vecConnections.FindAndFastRemove( this ) );
	}

	// Link to new poll group
	m_pPollGroup = pPollGroup;
	Assert( !m_pPollGroup->m_vecConnections.HasElement( this ) );
	m_pPollGroup->m_vecConnections.AddToTail( this );
}

bool CSteamNetworkConnectionBase::BInitConnection( SteamNetworkingMicroseconds usecNow, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	AssertLocksHeldByCurrentThread( "Base::BInitConnection" );

	// Should only be called while we are in the initial state
	Assert( GetState() == k_ESteamNetworkingConnectionState_None );

	// Make sure MTU values are initialized
	UpdateMTUFromConfig( true );

	Assert( m_hConnectionSelf == k_HSteamNetConnection_Invalid );

	Assert( m_pParentListenSocket == nullptr || m_pSteamNetworkingSocketsInterface == m_pParentListenSocket->m_pSteamNetworkingSocketsInterface );

	// We need to know who we are
	if ( m_identityLocal.IsInvalid() )
	{
		if ( !m_pSteamNetworkingSocketsInterface->GetIdentity( &m_identityLocal ) )
		{
			V_strcpy_safe( errMsg, "We don't know our local identity." );
			return false;
		}
	}

	m_eEndReason = k_ESteamNetConnectionEnd_Invalid;
	m_szEndDebug[0] = '\0';
	m_statsEndToEnd.Init( usecNow, ELinkActivityLevel::Disconnected ); // Until we go connected don't try to send acks, etc
	m_usecWhenCreated = usecNow;

	// Select random connection ID, and make sure it passes certain sanity checks
	{
		Assert( m_unConnectionIDLocal == 0 );

		// OK, even though *usually* we cannot take the table lock after holding
		// a connection lock (since we often take them in the opposite order),
		// in this case it's OK because:
		//
		// 1.) We hold the global lock AND
		// 2.) This is a new connection, and not previously in the table so there is no way
		//     for any other thread that might be holding the table lock at this time to
		//     subsequently try to wait on any locks that we hold.
		TableScopeLock tableLock( g_tables_lock );

		// We make sure the lower 16 bits are unique.  Make sure we don't have too many connections.
		// This definitely could be relaxed, but honestly we don't expect this library to be used in situations
		// where you need that many connections.
		if ( g_mapConnections.Count() >= 0x1fff )
		{
			V_strcpy_safe( errMsg, "Too many connections." );
			return false;
		}

		int tries = 0;
		for (;;) {
			if ( ++tries > 10000 )
			{
				V_strcpy_safe( errMsg, "Unable to find unique connection ID" );
				return false;
			}
			CCrypto::GenerateRandomBlock( &m_unConnectionIDLocal, sizeof(m_unConnectionIDLocal) );

			// Make sure neither half is zero
			if ( ( m_unConnectionIDLocal & 0xffff ) == 0 )
				continue;
			if ( ( m_unConnectionIDLocal & 0xffff0000 ) == 0 )
				continue;

			// Check recent connections
			if ( s_vecRecentLocalConnectionIDs.HasElement( (uint16)m_unConnectionIDLocal ) )
				continue;

			// Check active connections
			if ( g_mapConnections.HasElement( (uint16)m_unConnectionIDLocal ) )
				continue;

			// This one's good
			break;
		}

		// Let's use the the connection ID as the connection handle.  It's random, not reused
		// within a short time interval, and we print it in our debugging in places, and you
		// can see it on the wire for debugging.  In the past we has a "clever" method of
		// assigning the handle that had some cute performance tricks for lookups and
		// guaranteeing handles wouldn't be reused.  But making it be the same as the
		// ConnectionID is probably just more useful and less confusing.
		m_hConnectionSelf = m_unConnectionIDLocal;

		// Add it to our table of active sockets.
		g_mapConnections.Insert( int16( m_hConnectionSelf ), this );
	} // Release table scope lock

	// Set options, if any
	if ( pOptions )
	{
		for ( int i = 0 ; i < nOptions ; ++i )
		{
			if ( !m_pSteamNetworkingSocketsInterface->m_pSteamNetworkingUtils->SetConfigValueStruct( pOptions[i], k_ESteamNetworkingConfig_Connection, m_hConnectionSelf ) )
			{
				V_sprintf_safe( errMsg, "Error setting option %d", pOptions[i].m_eValue );
				return false;
			}
		}
	}
	else if ( nOptions != 0 )
	{
		V_strcpy_safe( errMsg, "Options list is NULL, but nOptions != 0?" );
		return false;
	}

	// Bind effective user data into the connection now.  It can no longer be inherited
	m_connectionConfig.ConnectionUserData.Set( m_connectionConfig.ConnectionUserData.Get() );

	// Make sure a description has been set for debugging purposes
	SetDescription();

	// Clear everything out
	ClearCrypto();

	// We should still be in the initial state
	Assert( GetState() == k_ESteamNetworkingConnectionState_None );

	// Take action to start obtaining a cert, or if we already have one, then set it now
	InitConnectionCrypto( usecNow );
	if ( GetState() != k_ESteamNetworkingConnectionState_None )
	{
		Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
		V_sprintf_safe( errMsg, "Crypto init error.  %s", m_szEndDebug );
		return false;
	}

	// Start with a single lane
	DbgVerify( SNP_ConfigureLanes( 1, nullptr, nullptr ) == k_EResultOK );

	return true;
}

bool CSteamNetworkConnectionBase::BSupportsSymmetricMode()
{
	return false;
}

void CSteamNetworkConnectionBase::SetAppName( const char *pszName )
{
	V_strcpy_safe( m_szAppName, pszName ? pszName : "" );

	// Re-calculate description
	SetDescription();
}

void CSteamNetworkConnectionBase::SetDescription()
{
	AssertLocksHeldByCurrentThread(); // Yes, we need the global lock, too, because the description is often accessed while holding the global lock, but not the connection lock

	ConnectionTypeDescription_t szTypeDescription;
	GetConnectionTypeDescription( szTypeDescription );

	if ( m_szAppName[0] )
		V_sprintf_safe( m_szDescription, "#%u %s '%s'", m_unConnectionIDLocal, szTypeDescription, m_szAppName );
	else
		V_sprintf_safe( m_szDescription, "#%u %s", m_unConnectionIDLocal, szTypeDescription );
}

void CSteamNetworkConnectionBase::InitConnectionCrypto( SteamNetworkingMicroseconds usecNow )
{
	BThinkCryptoReady( usecNow );
}

void CSteamNetworkConnectionBase::ClearCrypto()
{
	AssertLocksHeldByCurrentThread();
	m_msgCertRemote.Clear();
	m_msgCryptRemote.Clear();
	m_bCertHasIdentity = false;
	m_bRemoteCertHasTrustedCASignature = false;
	m_keyPrivate.Wipe();
	ClearLocalCrypto();
}

void CSteamNetworkConnectionBase::ClearLocalCrypto()
{
	AssertLocksHeldByCurrentThread();
	m_eNegotiatedCipher = k_ESteamNetworkingSocketsCipher_INVALID;
	m_cbEncryptionOverhead = k_cbAESGCMTagSize;
	m_keyExchangePrivateKeyLocal.Wipe();
	m_msgCryptLocal.Clear();
	m_msgSignedCryptLocal.Clear();
	m_bCryptKeysValid = false;
	m_pCryptContextSend.reset();
	m_pCryptContextRecv.reset();
	m_cryptIVSend.Wipe();
	m_cryptIVRecv.Wipe();
}

void CSteamNetworkConnectionBase::RecvNonDataSequencedPacket( int64 nPktNum, SteamNetworkingMicroseconds usecNow )
{
	SNP_DebugCheckPacketGapMap();

	// Note: order of operations is important between these two calls.
	// SNP_RecordReceivedPktNum assumes that TrackProcessSequencedPacket is always called first

	// Update general sequence number/stats tracker for the end-to-end flow.
	int idxMultiPath = 0; // Assume for now
	m_statsEndToEnd.TrackProcessSequencedPacket( nPktNum, usecNow, 0, idxMultiPath );

	// Let SNP know when we received it, so we can track loss events and send acks.  We do
	// not schedule acks to be sent at this time, but when they are sent, we will implicitly
	// ack this one
	SNP_RecordReceivedPktNum( nPktNum, usecNow, false );
}

bool CSteamNetworkConnectionBase::BThinkCryptoReady( SteamNetworkingMicroseconds usecNow )
{
	// Should only be called from initial states
	AssertLocksHeldByCurrentThread();
	Assert( GetState() == k_ESteamNetworkingConnectionState_None || GetState() == k_ESteamNetworkingConnectionState_Connecting );

	// Do we already have a cert?
	if ( m_msgSignedCertLocal.has_cert() )
		return true;

	// If we are using an anonymous identity, then always use self-signed.
	// CA's should never issue a certificate for this identity, because that
	// is meaningless.  No peer should ever honor such a certificate.
	if ( m_identityLocal.IsLocalHost() )
	{
		SetLocalCertUnsigned();
		return true;
	}

	// Check for fetching a cert, if a previous cert attempt failed,
	// or the cert we have is old
	#ifdef STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT
		if ( AllowLocalUnsignedCert() != k_EUnsignedCert_Allow )
		{

			// Make sure request is in flight if needed
			// If this fails (either immediately, or asynchronously), we will
			// get a CertFailed call with the appropriate code, and we can decide
			// what we want to do.
			m_pSteamNetworkingSocketsInterface->CheckAuthenticationPrerequisites( usecNow );

			// Handle synchronous failure.
			if ( GetState() != k_ESteamNetworkingConnectionState_None && GetState() != k_ESteamNetworkingConnectionState_Connecting )
				return false;

			// If fetching of cert or trusted cert list in flight, then wait for that to finish
			SteamNetAuthenticationStatus_t authStatus;
			m_pSteamNetworkingSocketsInterface->GetAuthenticationStatus( &authStatus );
			switch ( authStatus.m_eAvail )
			{
				case k_ESteamNetworkingAvailability_CannotTry:
				case k_ESteamNetworkingAvailability_Failed:
				case k_ESteamNetworkingAvailability_Previously:
				case k_ESteamNetworkingAvailability_NeverTried:
				default:
					AssertMsg2( false, "Unexpected auth avail %d (%s)", authStatus.m_eAvail, authStatus.m_debugMsg );
					break;

				case k_ESteamNetworkingAvailability_Retrying:
				case k_ESteamNetworkingAvailability_Waiting:
				case k_ESteamNetworkingAvailability_Attempting:
					// Keep waiting
					return false;

				case k_ESteamNetworkingAvailability_Current:
					break;
			}

		}
	#endif

	// Already have a signed cert?
	int nSecondsUntilCertExpiry = m_pSteamNetworkingSocketsInterface->GetSecondsUntilCertExpiry();
	if ( nSecondsUntilCertExpiry > 0 )
	{

		// We do have a cert -- but if it's close to expiring, wait for any active fetch to finish,
		// because there's a good chance that our peer will reject it.  (We usually refresh our certs
		// well ahead of time, so we really should never hit this.)  Note that if this request
		// fails, we will get a callback, and an opportunity to attempt to proceed with an unsigned cert
		if ( nSecondsUntilCertExpiry < 300 && m_pSteamNetworkingSocketsInterface->BCertRequestInFlight() )
			return false;

		// Use it!
		SpewVerbose( "[%s] Our cert expires in %d seconds.\n", GetDescription(), nSecondsUntilCertExpiry );
		SetLocalCert( m_pSteamNetworkingSocketsInterface->m_msgSignedCert, m_pSteamNetworkingSocketsInterface->m_keyPrivateKey, m_pSteamNetworkingSocketsInterface->BCertHasIdentity() );
		return true;
	}

	// Check if we want to intentionally disable auth
	if ( AllowLocalUnsignedCert() == k_EUnsignedCert_Allow )
	{
		SetLocalCertUnsigned();
		return true;
	}

	// Otherwise, we don't have a signed cert (yet?).
	#ifndef STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Need a cert authority!" );
		Assert( false );
	#endif
	return false;
}

void CSteamNetworkConnectionBase::InterfaceGotCert()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	ConnectionScopeLock connectionLock( *this );

	// Make sure we care about this
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting )
		return;
	if ( BHasLocalCert() )
		return;

	// Setup with this cert
	SetLocalCert( m_pSteamNetworkingSocketsInterface->m_msgSignedCert, m_pSteamNetworkingSocketsInterface->m_keyPrivateKey, m_pSteamNetworkingSocketsInterface->BCertHasIdentity() );

	// Don't check state machine now, let's just schedule immediate wake up to deal with it
	SetNextThinkTime( SteamNetworkingSockets_GetLocalTimestamp() );
}

void CSteamNetworkConnectionBase::SetLocalCert( const CMsgSteamDatagramCertificateSigned &msgSignedCert, const CECSigningPrivateKey &keyPrivate, bool bCertHasIdentity )
{
	AssertLocksHeldByCurrentThread();

	Assert( msgSignedCert.has_cert() );
	Assert( keyPrivate.IsValid() );

	// Ug, we have to save off the private key.  I hate to have copies of the private key,
	// but we'll only keep this around for a brief time.  It's possible for the
	// interface to get a new cert (with a new private key) while we are starting this
	// connection.  We'll keep using the old one, which may be totally valid.
	m_keyPrivate.CopyFrom( keyPrivate );

	// Save off the signed certificate
	m_msgSignedCertLocal = msgSignedCert;
	m_bCertHasIdentity = bCertHasIdentity;

	// If we are the "client", then we can wrap it up right now
	if ( !m_bConnectionInitiatedRemotely )
	{
		SetCryptoCipherList();
		FinalizeLocalCrypto();
	}
}

void CSteamNetworkConnectionBase::SetCryptoCipherList()
{
	AssertLocksHeldByCurrentThread();
	Assert( m_msgCryptLocal.ciphers_size() == 0 ); // Should only do this once

	// Select the ciphers we want to use, in preference order.
	// Also, lock it, we cannot change it any more
	m_connectionConfig.Unencrypted.Lock();
	int unencrypted = m_connectionConfig.Unencrypted.Get();
	switch ( unencrypted )
	{
		default:
			AssertMsg( false, "Unexpected value for 'Unencrypted' config value" );
			// FALLTHROUGH
			// |
			// |
			// V
		case 0:
			// Not allowed
			m_msgCryptLocal.add_ciphers( k_ESteamNetworkingSocketsCipher_AES_256_GCM );
			break;

		case 1:
			// Allowed, but prefer encrypted
			m_msgCryptLocal.add_ciphers( k_ESteamNetworkingSocketsCipher_AES_256_GCM );
			m_msgCryptLocal.add_ciphers( k_ESteamNetworkingSocketsCipher_NULL );
			break;

		case 2:
			// Allowed, preferred
			m_msgCryptLocal.add_ciphers( k_ESteamNetworkingSocketsCipher_NULL );
			m_msgCryptLocal.add_ciphers( k_ESteamNetworkingSocketsCipher_AES_256_GCM );
			break;

		case 3:
			// Required
			m_msgCryptLocal.add_ciphers( k_ESteamNetworkingSocketsCipher_NULL );
			break;
	}
}

void CSteamNetworkConnectionBase::FinalizeLocalCrypto()
{
	AssertLocksHeldByCurrentThread( "FinalizeLocalCrypto" );

	// Make sure we have what we need
	Assert( m_msgCryptLocal.ciphers_size() > 0 );
	Assert( m_keyPrivate.IsValid() );

	// Should only do this once
	Assert( !m_msgSignedCryptLocal.has_info() );

	// Set protocol version
	m_msgCryptLocal.set_protocol_version( k_nCurrentProtocolVersion );

	// Generate a keypair for key exchange
	CECKeyExchangePublicKey publicKeyLocal;
	CCrypto::GenerateKeyExchangeKeyPair( &publicKeyLocal, &m_keyExchangePrivateKeyLocal );
	m_msgCryptLocal.set_key_type( CMsgSteamDatagramSessionCryptInfo_EKeyType_CURVE25519 );
	publicKeyLocal.GetRawDataAsStdString( m_msgCryptLocal.mutable_key_data() );

	// Generate some more randomness for the secret key
	uint64 crypt_nonce;
	CCrypto::GenerateRandomBlock( &crypt_nonce, sizeof(crypt_nonce) );
	m_msgCryptLocal.set_nonce( crypt_nonce );

	// Serialize and sign the crypt key with the private key that matches this cert
	m_msgSignedCryptLocal.set_info( m_msgCryptLocal.SerializeAsString() );
	CryptoSignature_t sig;
	m_keyPrivate.GenerateSignature( m_msgSignedCryptLocal.info().c_str(), m_msgSignedCryptLocal.info().length(), &sig );
	m_msgSignedCryptLocal.set_signature( &sig, sizeof(sig) );

	// Note: In certain circumstances, we may need to do this again, so don't wipte the key just yet
	//m_keyPrivate.Wipe();

	// Probably a state change relevant to diagnostics
	CheckScheduleDiagnosticsUpdateASAP();
}

void CSteamNetworkConnectionBase::SetLocalCertUnsigned()
{
	AssertLocksHeldByCurrentThread();

	// Generate a keypair
	CECSigningPrivateKey keyPrivate;
	CECSigningPublicKey keyPublic;
	CCrypto::GenerateSigningKeyPair( &keyPublic, &keyPrivate );

	// Generate a cert
	CMsgSteamDatagramCertificate msgCert;
	keyPublic.GetRawDataAsStdString( msgCert.mutable_key_data() );
	msgCert.set_key_type( CMsgSteamDatagramCertificate_EKeyType_ED25519 );
	SteamNetworkingIdentityToProtobuf( m_identityLocal, msgCert, identity_string, legacy_identity_binary, legacy_steam_id );
	msgCert.add_app_ids( m_pSteamNetworkingSocketsInterface->m_pSteamNetworkingUtils->GetAppID() );

	// Should we set an expiry?  I mean it's unsigned, so it has zero value, so probably not
	//s_msgCertLocal.set_time_created( );

	// Serialize into "signed" message type, although we won't actually sign it.
	CMsgSteamDatagramCertificateSigned msgSignedCert;
	msgSignedCert.set_cert( msgCert.SerializeAsString() );

	// Standard init, as if this were a normal cert
	SetLocalCert( msgSignedCert, keyPrivate, true );
}

void CSteamNetworkConnectionBase::CertRequestFailed( ESteamNetConnectionEnd nConnectionEndReason, const char *pszMsg )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	ConnectionScopeLock connectionLock( *this );

	// Make sure we care about this
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting && GetState() != k_ESteamNetworkingConnectionState_None )
		return;
	if ( BHasLocalCert() )
		return;

	// Do we require a signed cert?
	EUnsignedCert eLocalUnsignedCert = AllowLocalUnsignedCert();
	if ( eLocalUnsignedCert == k_EUnsignedCert_Disallow )
	{
		// This is fatal
		SpewWarning( "[%s] Cannot use unsigned cert; failing connection.\n", GetDescription() );
		ConnectionState_ProblemDetectedLocally( nConnectionEndReason, "Cert failure: %s", pszMsg );
		return;
	}
	if ( eLocalUnsignedCert == k_EUnsignedCert_AllowWarn )
		SpewWarning( "[%s] Continuing with self-signed cert.\n", GetDescription() );
	SetLocalCertUnsigned();

	// Schedule immediate wake up to check on state machine
	SetNextThinkTime( SteamNetworkingSockets_GetLocalTimestamp() );
}

bool CSteamNetworkConnectionBase::BRecvCryptoHandshake(
	const CMsgSteamDatagramCertificateSigned &msgCert,
	const CMsgSteamDatagramSessionCryptInfoSigned &msgSessionInfo,
	bool bServer )
{
	SteamNetworkingErrMsg errMsg;
	ESteamNetConnectionEnd eFailure = RecvCryptoHandshake( msgCert, msgSessionInfo, bServer, errMsg );
	if ( eFailure == k_ESteamNetConnectionEnd_Invalid )
		return true;

	ConnectionState_ProblemDetectedLocally( eFailure, "%s", errMsg );
	return false;
}

ESteamNetConnectionEnd CSteamNetworkConnectionBase::RecvCryptoHandshake(
	const CMsgSteamDatagramCertificateSigned &msgCert,
	const CMsgSteamDatagramSessionCryptInfoSigned &msgSessionInfo,
	bool bServer,
	SteamNetworkingErrMsg &errMsg )
{
	AssertLocksHeldByCurrentThread( "BRecvCryptoHandshake" );
	SteamNetworkingErrMsg tmpErrMsg;

	// Have we already done key exchange?
	if ( m_bCryptKeysValid )
	{
		// FIXME - Probably should check that they aren't changing any keys.
		Assert( m_eNegotiatedCipher != k_ESteamNetworkingSocketsCipher_INVALID );
		return k_ESteamNetConnectionEnd_Invalid;
	}
	Assert( m_eNegotiatedCipher == k_ESteamNetworkingSocketsCipher_INVALID );

	// Make sure we have what we need
	if ( !msgCert.has_cert() || !msgSessionInfo.has_info() )
	{
		V_strcpy_safe( errMsg, "Crypto handshake missing cert or session data" );
		return k_ESteamNetConnectionEnd_Remote_BadCrypt;
	}

	// Save off the exact serialized data in the cert and crypt info,
	// for key generation material.
	m_sCertRemote = msgCert.cert();
	m_sCryptRemote = msgSessionInfo.info();

	// If they presented a signature, it must be valid
	const CertAuthScope *pCACertAuthScope = nullptr; 
	if ( msgCert.has_ca_signature() )
	{

		// Check the signature and chain of trust, and expiry, and deserialize the signed cert
		time_t timeNow = m_pSteamNetworkingSocketsInterface->m_pSteamNetworkingUtils->GetTimeSecure();
		pCACertAuthScope = CertStore_CheckCert( msgCert, m_msgCertRemote, timeNow, tmpErrMsg );
		if ( !pCACertAuthScope )
		{

			// We might allow this
			EUnsignedCert eAllow = AllowRemoteUnsignedCert();
			if ( eAllow == k_EUnsignedCert_AllowWarn )
			{
				SpewMsg( "[%s] Ignore cert failure (%s)  Connection is not secure.\n", GetDescription(), tmpErrMsg );
			}
			else if ( eAllow != k_EUnsignedCert_Allow )
			{
				V_sprintf_safe( errMsg, "Bad cert: %s", tmpErrMsg );
				return k_ESteamNetConnectionEnd_Remote_BadCert;
			}
		}
	}

	// If we didn't deserialize the cert above, do so now
	if ( !pCACertAuthScope )
	{

		// Deserialize the cert
		if ( !m_msgCertRemote.ParseFromString( m_sCertRemote ) )
		{
			V_strcpy_safe( errMsg, "Cert failed protobuf decode" );
			return k_ESteamNetConnectionEnd_Remote_BadCrypt;
		}

		// We'll check if unsigned certs are allowed below, after we know a bit more info
	}

	// Check identity from cert
	SteamNetworkingIdentity identityCert;
	int rIdentity = SteamNetworkingIdentityFromCert( identityCert, m_msgCertRemote, tmpErrMsg );
	if ( rIdentity < 0 )
	{
		V_sprintf_safe( errMsg, "Bad cert identity.  %s", tmpErrMsg );
		return k_ESteamNetConnectionEnd_Remote_BadCert;
	}
	if ( rIdentity > 0 && !identityCert.IsLocalHost() )
	{

		// They sent an identity.  Then it must match the identity we expect!
		if ( !( identityCert == m_identityRemote ) )
		{
			V_sprintf_safe( errMsg, "Cert was issued to %s, not %s",
				SteamNetworkingIdentityRender( identityCert ).c_str(), SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
			return k_ESteamNetConnectionEnd_Remote_BadCert;
		}

		// We require certs to be bound to a particular AppID.
		if ( m_msgCertRemote.app_ids_size() == 0 )
		{
			V_strcpy_safe( errMsg, "Cert must be bound to an AppID." );
			return k_ESteamNetConnectionEnd_Remote_BadCert;
		}
	}
	else if ( !msgCert.has_ca_signature() )
	{
		// If we're going to allow an unsigned cert (we'll check below),
		// then anything goes, so if they omit the identity, that's fine
		// with us because they could have forged anything anyway.
	}
	else
	{

		// Signed cert, not issued to a particular identity!  This is only allowed
		// right now when connecting to anonymous gameservers
		if ( !m_identityRemote.GetSteamID().BAnonGameServerAccount() )
		{
			V_sprintf_safe( errMsg, "Certs with no identity can only by anonymous gameservers, not %s", SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
			return k_ESteamNetConnectionEnd_Remote_BadCert;
		}

		// And cert must be scoped to a data center, we don't permit blanked certs for anybody with no restrictions at all
		if ( m_msgCertRemote.gameserver_datacenter_ids_size() == 0 )
		{
			V_strcpy_safe( errMsg, "Cert with no identity must be scoped to PoPID." );
			return k_ESteamNetConnectionEnd_Remote_BadCert;
		}
	}

	// OK, we've parsed everything out, now do any connection-type-specific checks on the cert
	ESteamNetConnectionEnd eRemoteCertFailure = CheckRemoteCert( pCACertAuthScope, errMsg );
	if ( eRemoteCertFailure )
		return eRemoteCertFailure;

	// Check the signature of the crypt info
	if ( !BCheckSignature( m_sCryptRemote, m_msgCertRemote.key_type(), m_msgCertRemote.key_data(), msgSessionInfo.signature(), errMsg ) )
	{
		return k_ESteamNetConnectionEnd_Remote_BadCrypt;
	}

	// Remember if we they were authenticated
	m_bRemoteCertHasTrustedCASignature = ( pCACertAuthScope != nullptr );

	// Deserialize crypt info
	if ( !m_msgCryptRemote.ParseFromString( m_sCryptRemote ) )
	{
		V_strcpy_safe( errMsg, "Crypt info failed protobuf decode" );
		return k_ESteamNetConnectionEnd_Remote_BadCrypt;
	}

	// Protocol version
	if ( m_msgCryptRemote.protocol_version() < k_nMinRequiredProtocolVersion )
	{
		V_sprintf_safe( errMsg, "Peer is running old software and needs to be updated.  (V%u, >=V%u is required)",
			m_msgCryptRemote.protocol_version(), k_nMinRequiredProtocolVersion );
		return k_ESteamNetConnectionEnd_Remote_BadProtocolVersion;
	}

	// Did they already send a protocol version in an earlier message?  If so, it needs to match.
	if ( m_statsEndToEnd.m_nPeerProtocolVersion != 0 && m_statsEndToEnd.m_nPeerProtocolVersion != m_msgCryptRemote.protocol_version() )
	{
		V_sprintf_safe( errMsg, "Claiming protocol V%u now, but earlier was using V%u",m_msgCryptRemote.protocol_version(), m_statsEndToEnd.m_nPeerProtocolVersion );
		return k_ESteamNetConnectionEnd_Remote_BadProtocolVersion;
	}
	m_statsEndToEnd.m_nPeerProtocolVersion = m_msgCryptRemote.protocol_version();

	// Check if we have configured multiple lanes, then they must understand it
	if ( len( m_senderState.m_vecLanes ) > 1 && m_statsEndToEnd.m_nPeerProtocolVersion < 11 )
	{
		V_strcpy_safe( errMsg, "Peer is using old protocol and cannot receive multiple lanes" );
		return k_ESteamNetConnectionEnd_Remote_BadProtocolVersion;
	}

	// Starting with protocol 10, the connect request/OK packets always implicitly
	// have a packet number of 1, and thus the next packet (often the first data packet)
	// is assigned a sequence number of 2, at a minimum.
	Assert( m_statsEndToEnd.m_nNextSendSequenceNumber >= 1 );
	Assert( m_statsEndToEnd.m_nMaxRecvPktNum >= 0 );
	if ( m_statsEndToEnd.m_nPeerProtocolVersion >= 10 )
	{
		if ( m_statsEndToEnd.m_nNextSendSequenceNumber == 1 )
			m_statsEndToEnd.m_nNextSendSequenceNumber = 2;
		if ( m_statsEndToEnd.m_nMaxRecvPktNum == 0 )
			m_statsEndToEnd.InitMaxRecvPktNum( 1 );
	}
	m_receiverState.InitPacketGapMap( m_statsEndToEnd.m_nMaxRecvPktNum, m_statsEndToEnd.m_usecTimeLastRecvSeq );

	// Check for legacy client that didn't send a list of ciphers
	if ( m_msgCryptRemote.ciphers_size() == 0 )
		m_msgCryptRemote.add_ciphers( k_ESteamNetworkingSocketsCipher_AES_256_GCM );

	// We need our own cert.  If we don't have one by now, then we might try generating one
	if ( !m_msgSignedCertLocal.has_cert() )
	{

		// Double-check that this is allowed
		EUnsignedCert eLocalUnsignedCert = AllowLocalUnsignedCert();
		if ( eLocalUnsignedCert == k_EUnsignedCert_Disallow )
		{
			// Derived class / calling code should check for this and handle it better and fail
			// earlier with a more specific error message.  (Or allow self-signed certs)
			V_strcpy_safe( errMsg, "We don't have cert, and self-signed certs not allowed" );
			return k_ESteamNetConnectionEnd_Misc_InternalError;
		}
		if ( eLocalUnsignedCert == k_EUnsignedCert_AllowWarn )
			SpewWarning( "[%s] Continuing with self-signed cert.\n", GetDescription() );

		// Proceed with an unsigned cert
		SetLocalCertUnsigned();
	}

	// If we are the client, then we have everything we need and can finish up right now
	if ( !m_bConnectionInitiatedRemotely )
	{
		// The server MUST send back the single cipher that they decided to use
		if ( m_msgCryptRemote.ciphers_size() != 1 )
		{
			V_strcpy_safe( errMsg, "Server must select exactly only one cipher!" );
			return k_ESteamNetConnectionEnd_Remote_BadCrypt;
		}
		return FinishCryptoHandshake( bServer, errMsg );
	}

	// Return success status
	return k_ESteamNetConnectionEnd_Invalid;
}

ESteamNetConnectionEnd CSteamNetworkConnectionBase::FinishCryptoHandshake( bool bServer, SteamNetworkingErrMsg &errMsg )
{
	AssertLocksHeldByCurrentThread( "BFinishCryptoHandshake" );

	// On the server, we have been waiting to decide what ciphers we are willing to use.
	// (Because we want to give the app to set any connection options).
	if ( m_bConnectionInitiatedRemotely )
	{
		Assert( m_msgCryptLocal.ciphers_size() == 0 );
		SetCryptoCipherList();
	}
	Assert( m_msgCryptLocal.ciphers_size() > 0 );
	
	// Find a mutually-acceptable cipher
	Assert( m_eNegotiatedCipher == k_ESteamNetworkingSocketsCipher_INVALID );
	m_eNegotiatedCipher = k_ESteamNetworkingSocketsCipher_INVALID;
	for ( int eCipher : m_msgCryptLocal.ciphers() )
	{
		if ( std::find( m_msgCryptRemote.ciphers().begin(), m_msgCryptRemote.ciphers().end(), eCipher ) != m_msgCryptRemote.ciphers().end() )
		{
			m_eNegotiatedCipher = ESteamNetworkingSocketsCipher(eCipher);
			break;
		}
	}
	switch (m_eNegotiatedCipher )
	{
		default:
		case k_ESteamNetworkingSocketsCipher_INVALID:
			V_strcpy_safe( errMsg, "Failed to negotiate mutually-agreeable cipher" );
			return k_ESteamNetConnectionEnd_Remote_BadCrypt;

		case k_ESteamNetworkingSocketsCipher_NULL:
			m_cbEncryptionOverhead = 0;
			break;

		case k_ESteamNetworkingSocketsCipher_AES_256_GCM:
			m_cbEncryptionOverhead = k_cbAESGCMTagSize;
			break;
	}

	// Recalculate MTU
	UpdateMTUFromConfig( true );

	// If we're the server, then lock in that single cipher as the only
	// acceptable cipher, and then we are ready to seal up our crypt info
	// and send it back to them in accept message(s)
	if ( m_bConnectionInitiatedRemotely )
	{
		Assert( !m_msgSignedCryptLocal.has_info() );
		m_msgCryptLocal.clear_ciphers();
		m_msgCryptLocal.add_ciphers( m_eNegotiatedCipher );
		FinalizeLocalCrypto();
	}
	Assert( m_msgSignedCryptLocal.has_info() );

	// At this point, we know that we will never the private key again.  So let's
	// wipe it now, to minimize the number of copies of this hanging around in memory.
	m_keyPrivate.Wipe();

	// Key exchange public key
	CECKeyExchangePublicKey keyExchangePublicKeyRemote;
	if ( m_msgCryptRemote.key_type() != CMsgSteamDatagramSessionCryptInfo_EKeyType_CURVE25519 )
	{
		V_sprintf_safe( errMsg, "Unsupported DH key type %d", (int)m_msgCryptRemote.key_type() );
		return k_ESteamNetConnectionEnd_Remote_BadCrypt;
	}
	if ( !keyExchangePublicKeyRemote.SetRawDataWithoutWipingInput( m_msgCryptRemote.key_data().c_str(), m_msgCryptRemote.key_data().length() ) )
	{
		V_strcpy_safe( errMsg, "Invalid DH key" );
		return k_ESteamNetConnectionEnd_Remote_BadCrypt;
	}

	// Diffie-Hellman key exchange to get "premaster secret"
	AutoWipeFixedSizeBuffer<sizeof(SHA256Digest_t)> premasterSecret;
	if ( !CCrypto::PerformKeyExchange( m_keyExchangePrivateKeyLocal, keyExchangePublicKeyRemote, &premasterSecret.m_buf ) )
	{
		V_strcpy_safe( errMsg, "Key exchange failed" );
		return k_ESteamNetConnectionEnd_Remote_BadCrypt;
	}
	//SpewMsg( "%s premaster: %02x%02x%02x%02x\n", bServer ? "Server" : "Client", premasterSecret.m_buf[0], premasterSecret.m_buf[1], premasterSecret.m_buf[2], premasterSecret.m_buf[3] );

	// We won't need this again, so go ahead and discard it now.
	m_keyExchangePrivateKeyLocal.Wipe();

	//
	// HMAC Key derivation function.
	//
	// https://tools.ietf.org/html/rfc5869
	// https://docs.google.com/document/d/1g5nIXAIkN_Y-7XJW5K45IblHd_L2f5LTaDUDwvZ5L6g/edit - Google QUIC as of 4/26/2017
	//

	//
	// 1. Extract: take premaster secret from key exchange and mix it so that it's evenly distributed, producing Pseudorandom key ("PRK")
	//
	uint64 salt[2] = { LittleQWord( m_msgCryptRemote.nonce() ), LittleQWord( m_msgCryptLocal.nonce() ) };
	if ( bServer )
		std::swap( salt[0], salt[1] );
	AutoWipeFixedSizeBuffer<sizeof(SHA256Digest_t)> prk;
	CCrypto::GenerateHMAC256( (const uint8 *)salt, sizeof(salt), premasterSecret.m_buf, premasterSecret.k_nSize, &prk.m_buf );
	premasterSecret.Wipe();

	//
	// 2. Expand: Use PRK as seed to generate all the different keys we need, mixing with connection-specific context
	//

	AutoWipeFixedSizeBuffer<32> cryptKeySend;
	AutoWipeFixedSizeBuffer<32> cryptKeyRecv;
	COMPILE_TIME_ASSERT( sizeof( cryptKeyRecv ) == sizeof(SHA256Digest_t) );
	COMPILE_TIME_ASSERT( sizeof( cryptKeySend ) == sizeof(SHA256Digest_t) );
	COMPILE_TIME_ASSERT( sizeof( m_cryptIVRecv ) <= sizeof(SHA256Digest_t) );
	COMPILE_TIME_ASSERT( sizeof( m_cryptIVSend ) <= sizeof(SHA256Digest_t) );

	uint8 *expandOrder[4] = { cryptKeySend.m_buf, cryptKeyRecv.m_buf, m_cryptIVSend.m_buf, m_cryptIVRecv.m_buf };
	int expandSize[4] = { cryptKeySend.k_nSize, cryptKeyRecv.k_nSize, m_cryptIVSend.k_nSize, m_cryptIVRecv.k_nSize };
	const std::string *context[4] = { &m_sCertRemote, &m_msgSignedCertLocal.cert(), &m_sCryptRemote, &m_msgSignedCryptLocal.info() };
	uint32 unConnectionIDContext[2] = { LittleDWord( m_unConnectionIDLocal ), LittleDWord( m_unConnectionIDRemote ) };

	// Make sure that both peers do things the same, so swap "local" and "remote" on one side arbitrarily.
	if ( bServer )
	{
		std::swap( expandOrder[0], expandOrder[1] );
		std::swap( expandOrder[2], expandOrder[3] );
		std::swap( expandSize[0], expandSize[1] ); // Actually NOP, but makes me feel better
		std::swap( expandSize[2], expandSize[3] );
		std::swap( context[0], context[1] );
		std::swap( context[2], context[3] );
		std::swap( unConnectionIDContext[0], unConnectionIDContext[1] );
	}
	//SpewMsg( "%s unConnectionIDContext = [ %u, %u ]\n", bServer ? "Server" : "Client", unConnectionIDContext[0], unConnectionIDContext[1] );

	// Generate connection "context" buffer
	CUtlBuffer bufContext( 0, (int)( sizeof(SHA256Digest_t) + sizeof(unConnectionIDContext) + 64 + context[0]->length() + context[1]->length() + context[2]->length() + context[3]->length() ), 0 );
	bufContext.SeekPut( CUtlBuffer::SEEK_HEAD, sizeof(SHA256Digest_t) );
	uint8 *pStart = (uint8 *)bufContext.PeekPut();

	// Write connection ID(s) into context buffer
	bufContext.Put( unConnectionIDContext, sizeof(unConnectionIDContext) );

	bufContext.Put( "Steam datagram", 14 );
	for ( const std::string *c: context )
		bufContext.Put( c->c_str(), (int)c->length() );

	// Now extract the keys according to the method in the RFC
	uint8 *pLastByte = (uint8 *)bufContext.PeekPut();
	SHA256Digest_t expandTemp;
	for ( int idxExpand = 0 ; idxExpand < 4 ; ++idxExpand )
	{
		*pLastByte = idxExpand+1;
		CCrypto::GenerateHMAC256( pStart, pLastByte - pStart + 1, prk.m_buf, prk.k_nSize, &expandTemp );
		V_memcpy( expandOrder[ idxExpand ], &expandTemp, expandSize[ idxExpand ] );

		//SpewMsg( "%s key %d: %02x%02x%02x%02x\n", bServer ? "Server" : "Client", idxExpand, expandTemp[0], expandTemp[1], expandTemp[2], expandTemp[3] );

		// Copy previous digest to use in generating the next one
		pStart = (uint8 *)bufContext.Base();
		V_memcpy( pStart, &expandTemp, sizeof(SHA256Digest_t) );
	}

	// Set encryption keys into the contexts, and set parameters
	switch ( m_eNegotiatedCipher )
	{
		default:
			Assert( false );
			V_strcpy_safe( errMsg, "Internal error!" );
			return k_ESteamNetConnectionEnd_Misc_InternalError;

		case k_ESteamNetworkingSocketsCipher_NULL:
			break;

		case k_ESteamNetworkingSocketsCipher_AES_256_GCM:
		{
			auto *pSend = new AES_GCM_EncryptContext;
			auto *pRecv = new AES_GCM_DecryptContext;
			m_pCryptContextSend.reset( pSend );
			m_pCryptContextRecv.reset( pRecv );

			if (
				!pSend->Init( cryptKeySend.m_buf, cryptKeySend.k_nSize, m_cryptIVSend.k_nSize, k_cbAESGCMTagSize )
				|| !pRecv->Init( cryptKeyRecv.m_buf, cryptKeyRecv.k_nSize, m_cryptIVRecv.k_nSize, k_cbAESGCMTagSize ) )
			{
				V_strcpy_safe( errMsg, "Error initializing crypto" );
				return k_ESteamNetConnectionEnd_Remote_BadCrypt;
			}
		} break;

	}

	//
	// Tidy up key droppings
	//
	SecureZeroMemory( bufContext.Base(), bufContext.SizeAllocated() );
	SecureZeroMemory( expandTemp, sizeof(expandTemp) );

	// This isn't sensitive info, but we don't need it any more, so go ahead and free up memory
	m_sCertRemote.clear();
	m_sCryptRemote.clear();

	// Make sure the connection description is set.
	// This is often called after we know who the remote host is
	SetDescription();

	// We're ready
	m_bCryptKeysValid = true;
	return k_ESteamNetConnectionEnd_Invalid;
}

EUnsignedCert CSteamNetworkConnectionBase::AllowLocalUnsignedCert()
{
	// Base class - we will not attempt connections without a local cert,
	// unless we are running in a way that is configured without one
	if ( !m_pSteamNetworkingSocketsInterface->BCanRequestCert() )
		return k_EUnsignedCert_Allow;
	else
		return k_EUnsignedCert_Disallow;
}

EUnsignedCert CSteamNetworkConnectionBase::AllowRemoteUnsignedCert()
{
	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		// We don't have a cert authority.  We probably ought to make this customizable
		return k_EUnsignedCert_Allow;
	#else
		return k_EUnsignedCert_Disallow;
	#endif
}

ESteamNetConnectionEnd CSteamNetworkConnectionBase::CheckRemoteCert( const CertAuthScope *pCACertAuthScope, SteamNetworkingErrMsg &errMsg )
{
	AssertLocksHeldByCurrentThread( "BFinishCryptoHandshake" );

	// Allowed for this app?
	if ( !CheckCertAppID( m_msgCertRemote, pCACertAuthScope, m_pSteamNetworkingSocketsInterface->m_pSteamNetworkingUtils->GetAppID(), errMsg ) )
		return k_ESteamNetConnectionEnd_Remote_BadCert;

	// Check if we don't allow unsigned certs
	if ( pCACertAuthScope == nullptr )
	{
		EUnsignedCert eAllow = AllowRemoteUnsignedCert();
		if ( eAllow == k_EUnsignedCert_AllowWarn )
		{
			SpewMsg( "[%s] Remote host is using an unsigned cert.  Allowing connection, but it's not secure!\n", GetDescription() );
		}
		else if ( eAllow != k_EUnsignedCert_Allow )
		{
			V_strcpy_safe( errMsg, "Unsigned certs are not allowed" );
			return k_ESteamNetConnectionEnd_Remote_BadCert;
		}
	}

	return k_ESteamNetConnectionEnd_Invalid;
}

void CSteamNetworkConnectionBase::SetUserData( int64 nUserData )
{
	m_pLock->AssertHeldByCurrentThread();
	m_connectionConfig.ConnectionUserData.Set( nUserData );

	// Change user data on all messages that haven't been pulled out
	// of the queue yet.  This way we don't expose the client to weird
	// race conditions where they create a connection, and before they
	// are able to install their user data, some messages come in
	g_lockAllRecvMessageQueues.lock();
	for ( CSteamNetworkingMessage *m = m_queueRecvMessages.m_pFirst ; m ; m = m->m_links.m_pNext )
	{
		Assert( m->m_conn == m_hConnectionSelf );
		m->m_nConnUserData = nUserData;
	}
	g_lockAllRecvMessageQueues.unlock();
}

void CConnectionTransport::TransportConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	AssertLocksHeldByCurrentThread();
}

bool CConnectionTransport::BCanSendEndToEndConnectRequest() const
{
	// You should override this, or your connection should not call it!
	Assert( false );
	return false;
}

void CConnectionTransport::SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow )
{
	// You should override this, or your connection should not call it!
	Assert( false );
}

void CConnectionTransport::TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const
{
}

void CConnectionTransport::GetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow )
{
}

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
void CConnectionTransport::TransportPopulateDiagnostics( CGameNetworkingUI_ConnectionState &msgConnectionState, SteamNetworkingMicroseconds usecNow )
{
}
#endif

void CSteamNetworkConnectionBase::ConnectionPopulateInfo( SteamNetConnectionInfo_t &info ) const
{
	m_pLock->AssertHeldByCurrentThread();
	memset( &info, 0, sizeof(info) );

	info.m_eState = CollapseConnectionStateToAPIState( m_eConnectionState );
	info.m_hListenSocket = m_pParentListenSocket ? m_pParentListenSocket->m_hListenSocketSelf : k_HSteamListenSocket_Invalid;
	info.m_identityRemote = m_identityRemote;
	info.m_nUserData = GetUserData();
	info.m_eEndReason = m_eEndReason;
	V_strcpy_safe( info.m_szEndDebug, m_szEndDebug );
	V_strcpy_safe( info.m_szConnectionDescription, m_szDescription );

	// Set security flags
	if ( !m_bRemoteCertHasTrustedCASignature || m_identityRemote.IsInvalid() || m_identityRemote.m_eType == k_ESteamNetworkingIdentityType_IPAddress )
		info.m_nFlags |= k_nSteamNetworkConnectionInfoFlags_Unauthenticated;
	if ( m_eNegotiatedCipher <= k_ESteamNetworkingSocketsCipher_NULL )
		info.m_nFlags |= k_nSteamNetworkConnectionInfoFlags_Unencrypted;

	if ( m_pTransport )
		m_pTransport->TransportPopulateConnectionInfo( info );
}

EResult CSteamNetworkConnectionBase::APIGetRealTimeStatus( SteamNetConnectionRealTimeStatus_t *pStatus, int nLanes, SteamNetConnectionRealTimeLaneStatus_t *pLanes )
{
	m_pLock->AssertHeldByCurrentThread();
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	if ( pLanes )
	{
		if ( nLanes < 0 || nLanes > len( m_senderState.m_vecLanes ) )
		{
			SpewBug( "Invalid lane count %d; Connection only has %d lanes configured\n", nLanes, len( m_senderState.m_vecLanes ) );
			return k_EResultInvalidParam;
		}
	}
	else
	{
		if ( nLanes != 0 )
		{
			SpewBug( "Number of lanes must be zero if array pointer is nonzero\n" );
			return k_EResultInvalidParam;
		}
	}

	if ( pStatus )
	{
		pStatus->m_eState = CollapseConnectionStateToAPIState( m_eConnectionState );
		pStatus->m_nPing = m_statsEndToEnd.m_ping.m_nSmoothedPing;
		if ( m_statsEndToEnd.m_flInPacketsDroppedPct >= 0.0f )
		{
			Assert( m_statsEndToEnd.m_flInPacketsWeirdSequencePct >= 0.0f );
			pStatus->m_flConnectionQualityLocal = 1.0f - m_statsEndToEnd.m_flInPacketsDroppedPct - m_statsEndToEnd.m_flInPacketsWeirdSequencePct;
			Assert( pStatus->m_flConnectionQualityLocal >= 0.0f );
		}
		else
		{
			pStatus->m_flConnectionQualityLocal = -1.0f;
		}

		// FIXME - Can SNP give us a more up-to-date value from the feedback packet?
		if ( m_statsEndToEnd.m_latestRemote.m_flPacketsDroppedPct >= 0.0f )
		{
			Assert( m_statsEndToEnd.m_latestRemote.m_flPacketsWeirdSequenceNumberPct >= 0.0f );
			pStatus->m_flConnectionQualityRemote = 1.0f - m_statsEndToEnd.m_latestRemote.m_flPacketsDroppedPct - m_statsEndToEnd.m_latestRemote.m_flPacketsWeirdSequenceNumberPct;
			Assert( pStatus->m_flConnectionQualityRemote >= 0.0f );
		}
		else
		{
			pStatus->m_flConnectionQualityRemote = -1.0f;
		}

		// Actual current data rates
		pStatus->m_flOutPacketsPerSec = m_statsEndToEnd.m_sent.m_packets.m_flRate;
		pStatus->m_flOutBytesPerSec = m_statsEndToEnd.m_sent.m_bytes.m_flRate;
		pStatus->m_flInPacketsPerSec = m_statsEndToEnd.m_recv.m_packets.m_flRate;
		pStatus->m_flInBytesPerSec = m_statsEndToEnd.m_recv.m_bytes.m_flRate;
	}
	SNP_PopulateRealTimeStatus( pStatus, nLanes, pLanes, usecNow );

	return k_EResultOK;
}

void CSteamNetworkConnectionBase::APIGetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow )
{
	// Connection must be locked, but we don't require the global lock here!
	m_pLock->AssertHeldByCurrentThread();

	stats.Clear();
	ConnectionPopulateInfo( stats.m_info );

	// Copy end-to-end stats
	m_statsEndToEnd.GetLinkStats( stats.m_statsEndToEnd, usecNow );

	// Congestion control and bandwidth estimation
	SNP_PopulateDetailedStats( stats.m_statsEndToEnd );

	if ( m_pTransport )
		m_pTransport->GetDetailedConnectionStatus( stats, usecNow );
}

EResult CSteamNetworkConnectionBase::APISendMessageToConnection( const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber )
{
	// Connection must be locked, but we don't require the global lock here!
	m_pLock->AssertHeldByCurrentThread();

	if ( pOutMessageNumber )
		*pOutMessageNumber = -1;

	// Check connection state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Dead:
		default:
			AssertMsg( false, "Why are making API calls on this connection?" );
			return k_EResultInvalidState;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
			if ( nSendFlags & k_nSteamNetworkingSend_NoDelay )
				return k_EResultIgnored;
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			return k_EResultNoConnection;
	}

	// Fill out a message object
	CSteamNetworkingMessage *pMsg = CSteamNetworkingMessage::New( cbData );
	if ( !pMsg )
		return k_EResultFail;
	pMsg->m_nFlags = nSendFlags;

	// Copy in the payload
	memcpy( pMsg->m_pData, pData, cbData );

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Connection-type specific logic
	int64 nMsgNumberOrResult = _APISendMessageToConnection( pMsg, usecNow, nullptr );
	if ( nMsgNumberOrResult > 0 )
	{
		if ( pOutMessageNumber )
			*pOutMessageNumber = nMsgNumberOrResult;
		return k_EResultOK;
	}
	return EResult( -nMsgNumberOrResult );
}

int64 CSteamNetworkConnectionBase::APISendMessageToConnection( CSteamNetworkingMessage *pMsg, SteamNetworkingMicroseconds usecNow, bool *pbThinkImmediately )
{
	m_pLock->AssertHeldByCurrentThread();

	// Check connection state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Dead:
		default:
			AssertMsg( false, "Why are making API calls on this connection?" );
			pMsg->Release();
			return -k_EResultInvalidState;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
			if ( pMsg->m_nFlags & k_nSteamNetworkingSend_NoDelay )
			{
				pMsg->Release();
				return -k_EResultIgnored;
			}
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			pMsg->Release();
			return -k_EResultNoConnection;
	}

	return _APISendMessageToConnection( pMsg, usecNow, pbThinkImmediately );
}

int64 CSteamNetworkConnectionBase::_APISendMessageToConnection( CSteamNetworkingMessage *pMsg, SteamNetworkingMicroseconds usecNow, bool *pbThinkImmediately )
{

	// Message too big?
	if ( pMsg->m_cbSize > k_cbMaxSteamNetworkingSocketsMessageSizeSend )
	{
		AssertMsg2( false, "Message size %d is too big.  Max is %d", pMsg->m_cbSize, k_cbMaxSteamNetworkingSocketsMessageSizeSend );
		pMsg->Release();
		return -k_EResultInvalidParam;
	}

	// Pass to reliability layer.
	return SNP_SendMessage( pMsg, usecNow, pbThinkImmediately );
}


EResult CSteamNetworkConnectionBase::APIFlushMessageOnConnection()
{
	m_pLock->AssertHeldByCurrentThread();

	// Check connection state
	switch ( GetState() )
	{
	case k_ESteamNetworkingConnectionState_None:
	case k_ESteamNetworkingConnectionState_FinWait:
	case k_ESteamNetworkingConnectionState_Linger:
	case k_ESteamNetworkingConnectionState_Dead:
	default:
		AssertMsg( false, "Why are making API calls on this connection?" );
		return k_EResultInvalidState;

	case k_ESteamNetworkingConnectionState_Connecting:
	case k_ESteamNetworkingConnectionState_FindingRoute:
	case k_ESteamNetworkingConnectionState_Connected:
		break;

	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		return k_EResultNoConnection;
	}

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	return SNP_FlushMessage( usecNow );
}

int CSteamNetworkConnectionBase::APIReceiveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	// Connection must be locked, but we don't require the global lock here!
	m_pLock->AssertHeldByCurrentThread();

	g_lockAllRecvMessageQueues.lock();
	
	int result = m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
	g_lockAllRecvMessageQueues.unlock();

	return result;
}

bool CSteamNetworkConnectionBase::DecryptDataChunk( uint16 nWireSeqNum, int cbPacketSize, const void *pChunk, int cbChunk, RecvPacketContext_t &ctx )
{
	AssertLocksHeldByCurrentThread();

	if ( !m_bCryptKeysValid || !BStateIsActive() )
	{
		Assert( m_bCryptKeysValid );
		Assert( BStateIsActive() );
		return false;
	}

	// Sequence number should be initialized at this point!  We had some cases where
	// the protocol was not properly assigning a sequence number, but those should be
	// fixed now
	AssertMsg1( m_statsEndToEnd.m_nMaxRecvPktNum > 0 || m_statsEndToEnd.m_nPeerProtocolVersion < 10, "[%s] packet number not properly initialized!", GetDescription() );

	// Get the full end-to-end packet number, check if we should process it
	ctx.m_nPktNum = m_statsEndToEnd.ExpandWirePacketNumberAndCheck( nWireSeqNum, ctx.m_idxMultiPath );
	if ( ctx.m_nPktNum <= 0 )
	{

		// Update raw packet counters numbers, but do not update any logical state such as reply timeouts, etc
		m_statsEndToEnd.m_recv.ProcessPacket( cbPacketSize );
		return false;
	}

	// What cipher are we using?
	if ( m_eNegotiatedCipher == k_ESteamNetworkingSocketsCipher_NULL )
	{
		// No encryption!
		ctx.m_cbPlainText = cbChunk;
		ctx.m_pPlainText = pChunk;
	}
	else if ( !m_pCryptContextRecv )
	{
		AssertMsg1( false, "Bogus cipher %d", m_eNegotiatedCipher );
		return false;
	}
	else
	{
		// Adjust the IV by the packet number
		*(uint64 *)&m_cryptIVRecv.m_buf += LittleQWord( ctx.m_nPktNum );
		//SpewMsg( "Recv decrypt IV %llu + %02x%02x%02x%02x  encrypted %d %02x%02x%02x%02x\n",
		//	*(uint64 *)&m_cryptIVRecv.m_buf,
		//	m_cryptIVRecv.m_buf[8], m_cryptIVRecv.m_buf[9], m_cryptIVRecv.m_buf[10], m_cryptIVRecv.m_buf[11],
		//	cbChunk,
		//	*((byte*)pChunk + 0), *((byte*)pChunk + 1), *((byte*)pChunk + 2), *((byte*)pChunk + 3)
		//);

		// Decrypt the chunk and check the auth tag
		uint32 cbDecrypted = sizeof(ctx.m_decrypted);
		bool bDecryptOK = m_pCryptContextRecv->Decrypt(
			pChunk, cbChunk, // encrypted
			m_cryptIVRecv.m_buf, // IV
			ctx.m_decrypted, &cbDecrypted, // output
			nullptr, 0 // no AAD
		);

		// Restore the IV to the base value
		*(uint64 *)&m_cryptIVRecv.m_buf -= LittleQWord( ctx.m_nPktNum );
	
		// Did decryption fail?
		if ( !bDecryptOK ) {

			// Just drop packet.
			// The assumption is that we either have a bug or some weird thing,
			// or that somebody is spoofing / tampering.  If it's the latter
			// we don't want to magnify the impact of their efforts
			SpewWarningRateLimited( ctx.m_usecNow, "[%s] Packet %lld (0x%x) decrypt failed (tampering/spoofing/bug)! mpath%d",
				GetDescription(), (long long)ctx.m_nPktNum, (unsigned)nWireSeqNum, ctx.m_idxMultiPath );

			// Update raw packet counters numbers, but do not update any logical state such as reply timeouts, etc
			m_statsEndToEnd.m_recv.ProcessPacket( cbPacketSize );
			return false;
		}

		ctx.m_cbPlainText = (int)cbDecrypted;
		ctx.m_pPlainText = ctx.m_decrypted;

		//SpewVerbose( "Connection %u recv seqnum %lld (gap=%d) sz=%d %02x %02x %02x %02x\n", m_unConnectionID, unFullSequenceNumber, nGap, cbDecrypted, arDecryptedChunk[0], arDecryptedChunk[1], arDecryptedChunk[2], arDecryptedChunk[3] );
	}

	// OK, we have high confidence that this packet is actually from our peer and has not
	// been tampered with.  Check the gap.  If it's too big, that means we are risking losing
	// our ability to keep the sequence numbers in sync on each end.  This is a relatively
	// large number of outstanding packets.  We should never have this many packets
	// outstanding unacknowledged.  When we stop getting acks we should reduce our packet rate.
	// This isn't really a practical limitation, but it is a theoretical limitation if the
	// bandwidth is extremely high relatively to the latency.
	//
	// Even if the packets are on average only half full (~600 bytes), 16k packets is
	// around 9MB of data.  We probably don't want to have this amount of un-acked data
	// in our buffers, anyway.  If the packets are tiny it would be less, but a
	// a really high packet rate of tiny packets is not a good idea anyway.  Use bigger packets
	// with a lower rate.  If the app is really trying to fill the pipe and blasting a large
	// amount of data (and not forcing us to send small packets), then our code should be sending
	// mostly full packets, which means that this is closer to a gap of around ~18MB.
	int64 nGap = ctx.m_nPktNum - m_statsEndToEnd.m_nMaxRecvPktNum;
	if ( nGap > 0x4000 )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Generic,
			"Pkt number lurch by %lld; %04x->%04x",
			(long long)nGap, (uint16)m_statsEndToEnd.m_nMaxRecvPktNum, nWireSeqNum);
		return false;
	}

	// Decrypted ok.  Track flow, and allow this packet to update the logical state, reply timeouts, etc
	m_statsEndToEnd.TrackRecvPacket( cbPacketSize, ctx.m_usecNow );
	return true;
}

EResult CSteamNetworkConnectionBase::APIAcceptConnection()
{
	AssertLocksHeldByCurrentThread();
	SteamNetworkingErrMsg errMsg;

	// Must be in in state ready to be accepted
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting )
	{
		if ( GetState() == k_ESteamNetworkingConnectionState_ClosedByPeer )
		{
			SpewWarning( "[%s] Cannot accept connection; already closed by remote host.", GetDescription() );
		}
		else if ( BSymmetricMode() && BStateIsActive() )
		{
			SpewMsg( "[%s] Symmetric connection has already been accepted (perhaps implicitly, by attempting matching outbound connection)", GetDescription() );
			return k_EResultDuplicateRequest;
		}
		else
		{
			SpewError( "[%s] Cannot accept connection, current state is %d.", GetDescription(), GetState() );
		}
		return k_EResultInvalidState;
	}

	// Should only be called for connections initiated remotely
	if ( !m_bConnectionInitiatedRemotely )
	{
		SpewError( "[%s] Should not be trying to acccept this connection, it was not initiated remotely.", GetDescription() );
		return k_EResultInvalidParam;
	}

	// Select the cipher.  We needed to wait until now to do it, because the app
	// might have set connection options on a new connection.
	Assert( m_eNegotiatedCipher == k_ESteamNetworkingSocketsCipher_INVALID );
	ESteamNetConnectionEnd eCryptoHandshakeErr = FinishCryptoHandshake( true, errMsg );
	if ( eCryptoHandshakeErr )
	{
		ConnectionState_ProblemDetectedLocally( eCryptoHandshakeErr, "%s", errMsg );
		return k_EResultHandshakeFailed;
	}

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Derived class knows what to do next
	EResult eResult = AcceptConnection( usecNow );
	if ( eResult == k_EResultOK )
	{
		// Make sure they properly transitioned the connection state
		AssertMsg2(
			GetState() == k_ESteamNetworkingConnectionState_FindingRoute || GetState() == k_ESteamNetworkingConnectionState_Connected,
			"[%s] AcceptConnection put the connection into state %d", GetDescription(), (int)GetState() );
	}
	else
	{
		// Nuke connection if we fail.  (If they provided a more specific reason and already closed
		// the connection, this won't do anything.)
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Failed to accept connection." );
	}

	return eResult;
}

EResult CSteamNetworkConnectionBase::AcceptConnection( SteamNetworkingMicroseconds usecNow )
{
	NOTE_UNUSED( usecNow );

	// You need to override this if your connection type can be accepted
	Assert( false );
	return k_EResultFail;
}

void CSteamNetworkConnectionBase::APICloseConnection( int nReason, const char *pszDebug, bool bEnableLinger )
{
	AssertLocksHeldByCurrentThread();

	// If we already know the reason for the problem, we should ignore theirs
	if ( m_eEndReason == k_ESteamNetConnectionEnd_Invalid || BStateIsActive() )
	{
		if ( nReason == 0 )
		{
			nReason = k_ESteamNetConnectionEnd_App_Generic;
		}
		else if ( nReason < k_ESteamNetConnectionEnd_App_Min || nReason > k_ESteamNetConnectionEnd_AppException_Max )
		{
			// Use a special value so that we can detect if people have this bug in our analytics
			nReason = k_ESteamNetConnectionEnd_App_Max;
			pszDebug = "Invalid numeric reason code";
		}

		m_eEndReason = ESteamNetConnectionEnd( nReason );
		if ( m_szEndDebug[0] == '\0' )
		{
			if ( pszDebug == nullptr || *pszDebug == '\0' )
			{
				if ( nReason >= k_ESteamNetConnectionEnd_AppException_Min )
				{
					pszDebug = "Application closed connection in an unusual way";
				}
				else
				{
					pszDebug = "Application closed connection";
				}
			}
			V_strcpy_safe( m_szEndDebug, pszDebug );
		}
	}

	// Check our state to see how to spew, and handle a few exceptional cases
	// where we don't transition to FinWait
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SpewVerbose( "[%s] cleaned up\n", GetDescription() );
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
			SpewMsg( "[%s] closed by app before we got connected (%d) %s\n", GetDescription(), (int)m_eEndReason, m_szEndDebug );
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			if ( BReadyToExitLingerState() )
			{
				SpewMsg( "[%s] Leaving linger state. (%d) %s\n", GetDescription(), (int)m_eEndReason, m_szEndDebug );
			}
			else
			{
				SpewWarning( "[%s] Destroying connection before all data is flushed! (%d) %s\n", GetDescription(), (int)m_eEndReason, m_szEndDebug );
			}
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			if ( bEnableLinger )
			{
				if ( BReadyToExitLingerState() )
				{
					SpewMsg( "[%s] closed by app, linger requested but not needed (%d) %s\n", GetDescription(), (int)m_eEndReason, m_szEndDebug );
				}
				else
				{
					SpewMsg( "[%s] closed by app, entering linger state (%d) %s\n", GetDescription(), (int)m_eEndReason, m_szEndDebug );
					SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
					SetState( k_ESteamNetworkingConnectionState_Linger, usecNow );
					SetNextThinkTimeASAP();
					return;
				}
			}
			else
			{
				SpewMsg( "[%s] closed by app (%d) %s\n", GetDescription(), (int)m_eEndReason, m_szEndDebug );
			}
			break;
	}

	// Enter the FinWait state.  Connection-specific and transport code should
	// watch for this transition and will send any cleanup packets, and enter
	// a state where we wait for the peer to acknowledge and/or retry the
	// close message.
	ConnectionState_FinWait();
}

void CSteamNetworkConnectionBase::SetState( ESteamNetworkingConnectionState eNewState, SteamNetworkingMicroseconds usecNow )
{
	// All connection state transitions require the global lock!
	AssertLocksHeldByCurrentThread();

	if ( eNewState == m_eConnectionState )
		return;
	const ESteamNetworkingConnectionState eOldState = m_eConnectionState;
	m_eConnectionState = eNewState;

	// Remember when we entered this state
	m_usecWhenEnteredConnectionState = usecNow;

	// Set wire state
	switch ( GetState() )
	{
		default:
			Assert( false );
			// FALLTHROUGH
			// |
			// |
			// V

		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			m_eConnectionWireState = eNewState;
			break;

		case k_ESteamNetworkingConnectionState_FinWait:

			// Check where we are coming from
			switch ( eOldState )
			{
				case k_ESteamNetworkingConnectionState_Dead:
				case k_ESteamNetworkingConnectionState_None:
				case k_ESteamNetworkingConnectionState_FinWait:
				default:
					Assert( false );
					break;

				case k_ESteamNetworkingConnectionState_ClosedByPeer:
				case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
					Assert( m_eConnectionWireState == eOldState );
					m_eConnectionWireState = eOldState;
					break;

				case k_ESteamNetworkingConnectionState_Linger:
				case k_ESteamNetworkingConnectionState_Connecting:
				case k_ESteamNetworkingConnectionState_FindingRoute:
				case k_ESteamNetworkingConnectionState_Connected:
					m_eConnectionWireState = k_ESteamNetworkingConnectionState_FinWait;
					break;
			}
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			Assert( eOldState == k_ESteamNetworkingConnectionState_Connected );
			m_eConnectionWireState = k_ESteamNetworkingConnectionState_Connected;
			break;
	}

	// Certain connection options cannot be changed after a certain point
	bool bLock = false;
	if ( GetState() == k_ESteamNetworkingConnectionState_Connecting )
	{
		if ( m_bConnectionInitiatedRemotely )
		{
			// Remote host initiated the connection.  All options below can be tweaked
			// until
		}
		else
		{
			// We initiated the connection.  All options listed below must be set at creation time
			bLock = true;
		}
	}
	else if ( BStateIsActive() )
	{
		bLock = true;
	}
	if ( bLock )
	{
		// Can't change certain options after this point
		m_connectionConfig.IP_AllowWithoutAuth.Lock();
		m_connectionConfig.IPLocalHost_AllowWithoutAuth.Lock();
		m_connectionConfig.Unencrypted.Lock();
		m_connectionConfig.SymmetricConnect.Lock();
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
			m_connectionConfig.SDRClient_DevTicket.Lock();
		#endif
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
			m_connectionConfig.P2P_Transport_ICE_Enable.Lock();
			m_connectionConfig.P2P_Transport_ICE_Implementation.Lock();
		#endif
	}

	// Post a notification when certain state changes occur.  Note that
	// "internal" state changes, where the connection is effectively closed
	// from the application's perspective, are not relevant
	const ESteamNetworkingConnectionState eOldAPIState = CollapseConnectionStateToAPIState( eOldState );
	const ESteamNetworkingConnectionState eNewAPIState = CollapseConnectionStateToAPIState( GetState() );

	// Callbacks temporarily suppressed?
	Assert( m_nSupressStateChangeCallbacks >= 0 );
	if ( m_nSupressStateChangeCallbacks == 0 )
	{
		// If connection is associated with an ad-hoc-style messages endpoint,
		// let them process the state change
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
		if ( m_pMessagesEndPointSessionOwner )
		{
			m_pMessagesEndPointSessionOwner->SessionConnectionStateChanged( this, eOldAPIState );
		}
		else
		#endif
		{
			if ( eOldState == k_ESteamNetworkingConnectionState_None && GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
			{
				// Do not post callbacks for internal failures during connection creation
			}
			else
			{
				PostConnectionStateChangedCallback( eOldAPIState, eNewAPIState );
			}
		}
	}

	// Any time we switch into a state that is closed from an API perspective,
	// discard any unread received messages
	if ( eNewAPIState == k_ESteamNetworkingConnectionState_None )
	{
		g_lockAllRecvMessageQueues.lock();
		m_queueRecvMessages.PurgeMessages();
		g_lockAllRecvMessageQueues.unlock();
	}

	// Slam some stuff when we are in various states
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:

			// Check for leaving connected state
			if ( m_statsEndToEnd.m_usecWhenStartedConnectedState != 0 && m_statsEndToEnd.m_usecWhenEndedConnectedState == 0 )
				m_statsEndToEnd.m_usecWhenEndedConnectedState = usecNow;

			// Let stats tracking system know that it should
			// not attempt to send any stats, nor expect any acks from the peer
			m_statsEndToEnd.SetActivityLevel( ELinkActivityLevel::Disconnected, m_usecWhenEnteredConnectionState );

			// Go head and free up memory now
			SNP_ShutdownConnection();
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			Assert( !m_pMessagesEndPointSessionOwner ); // We shouldn't try to enter the linger state for these types of connections!
			// FALLTHROUGH
			// |
			// |
			// V
		case k_ESteamNetworkingConnectionState_Connected:

			// Key exchange should be complete
			Assert( m_bCryptKeysValid );
			Assert( m_statsEndToEnd.m_usecWhenStartedConnectedState != 0 );

			// Link stats tracker should send and expect, acks, keepalives, etc
			m_statsEndToEnd.SetActivityLevel( ELinkActivityLevel::Active, m_usecWhenEnteredConnectionState );
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:

			// Key exchange should be complete.  (We do that when accepting a connection.)
			Assert( m_bCryptKeysValid );

			// FIXME.  Probably we should NOT set the stats tracker as "active" yet.
			//Assert( m_statsEndToEnd.IsPassive() );
			m_statsEndToEnd.SetActivityLevel( ELinkActivityLevel::Active, m_usecWhenEnteredConnectionState );
			break;

		case k_ESteamNetworkingConnectionState_Connecting:

			// And we shouldn't mark stats object as ready until we go connected
			Assert( m_statsEndToEnd.IsDisconnected() );
			break;
	}

	// Finally, hook for derived class to take action.  But not if we're dead
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
			break;
		default:
			ConnectionStateChanged( eOldState );
			break;
	}
}

void CSteamNetworkConnectionBase::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{

	// If we have a transport, give it a chance to react to the state change
	if ( m_pTransport )
		m_pTransport->TransportConnectionStateChanged( eOldState );
}

CSteamNetworkingMessage *CSteamNetworkConnectionBase::AllocateNewRecvMessage( uint32 cbSize, int nFlags, SteamNetworkingMicroseconds usecNow )
{
	//
	// Check limits
	//

	// Max message size
	if ( (uint32)m_connectionConfig.RecvMaxMessageSize.Get() < cbSize )
	{
		SpewMsg( "[%s] recv message of size %u too large for limit of %d.\n", GetDescription(), cbSize, m_connectionConfig.RecvMaxMessageSize.Get() );
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Failed to allocate a buffer of size %u (limit is %d).", cbSize, m_connectionConfig.RecvMaxMessageSize.Get() );
		return nullptr;
	}

	// Max number of messages buffered
	if ( m_queueRecvMessages.m_nMessageCount >= m_connectionConfig.RecvBufferMessages.Get() )
	{
		SpewWarningRateLimited( usecNow, "[%s] recv queue overflow %d messages already queued.\n", GetDescription(), m_queueRecvMessages.m_nMessageCount );
		return nullptr;
	}

	CSteamNetworkingMessage *pMsg = CSteamNetworkingMessage::New( cbSize );

	if ( !pMsg )
	{
		// Failed!  if it's for a reliable message, then we must abort the connection.
		// If unreliable message....well we've spewed, but let's try to keep on chugging.
		if ( nFlags & k_nSteamNetworkingSend_Reliable )
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Failed to allocate buffer to receive reliable message" );
		return nullptr;
	}

	pMsg->m_usecTimeReceived = usecNow;
	pMsg->m_nFlags = nFlags;
	return pMsg;
}

bool CSteamNetworkConnectionBase::ReceivedMessageData( const void *pData, int cbData, int idxLane, int64 nMsgNum, int nFlags, SteamNetworkingMicroseconds usecNow )
{

	// Create a message
	CSteamNetworkingMessage *pMsg = AllocateNewRecvMessage( cbData, nFlags, usecNow );
	if ( !pMsg )
	{
		// Hm.  this failure really is probably a sign that we are in a pretty bad state,
		// and we are unlikely to recover. Let the caller know about failure (this drops the connection).
		return false;
	}

	// Record lane and message number
	pMsg->m_idxLane = idxLane;
	pMsg->m_nMessageNumber = nMsgNum;

	// Copy the data
	memcpy( pMsg->m_pData, pData, cbData );

	// Receive it
	return ReceivedMessage( pMsg );
}

bool CSteamNetworkConnectionBase::ReceivedMessage( CSteamNetworkingMessage *pMsg )
{
	m_pLock->AssertHeldByCurrentThread();

	SpewVerboseGroup( m_connectionConfig.LogLevel_Message.Get(), "[%s] RecvMessage MsgNum=%lld sz=%d\n",
		GetDescription(),
		(long long)pMsg->m_nMessageNumber,
		pMsg->m_cbSize );

	// Check for redirecting it to the messages endpoint owner
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
		if ( m_pMessagesEndPointSessionOwner )
		{
			m_pMessagesEndPointSessionOwner->ReceivedMessage( pMsg, this );
			return true;
		}
	#endif

	// Fill in a few more details
	pMsg->m_identityPeer = m_identityRemote;
	pMsg->m_conn = m_hConnectionSelf;
	pMsg->m_nConnUserData = GetUserData();

	// Emit ETW event
	TraceLoggingWrite(
		HTraceLogging_SteamNetworkingSockets,
		"MsgRecv",
		TraceLoggingString( GetDescription(), "Connection" ),
		TraceLoggingInt64( pMsg->m_nMessageNumber, "MsgNum" ),
		TraceLoggingUInt16( pMsg->m_idxLane, "Lane" ),
		TraceLoggingBool( ( pMsg->m_nFlags & k_nSteamNetworkingSend_Reliable ) != 0, "Reliable" ),
		TraceLoggingUInt32( pMsg->m_cbSize, "Size" )
	);

	// We use the same lock to protect *all* recv queues, for both connections and poll groups,
	// which keeps this really simple.
	g_lockAllRecvMessageQueues.lock();

	Assert( pMsg->m_cbSize >= 0 );
	if ( m_queueRecvMessages.m_nMessageCount >= m_connectionConfig.RecvBufferMessages.Get() )
	{
		g_lockAllRecvMessageQueues.unlock();
		SpewWarningRateLimited ( SteamNetworkingSockets_GetLocalTimestamp(), "[%s] recv queue overflow %d messages already queued.\n", GetDescription(), m_queueRecvMessages.m_nMessageCount );
		pMsg->Release();
		return false;
	}

	if ( m_queueRecvMessages.m_nMessageSize + pMsg->m_cbSize > m_connectionConfig.RecvBufferSize.Get() )
	{
		g_lockAllRecvMessageQueues.unlock();
		SpewWarningRateLimited ( SteamNetworkingSockets_GetLocalTimestamp(), "[%s] recv queue overflow %d + %d bytes exceeds limit of %d.\n", GetDescription(), m_queueRecvMessages.m_nMessageSize, pMsg->m_cbSize, m_connectionConfig.RecvBufferSize.Get() );
		pMsg->Release();
		return false;
	}

	// Check for messages are received out of order.  If the messages with a higher
	// message number have not been removed from the queue, then we can correct this
	const int64 nMessageNumber = pMsg->m_nMessageNumber;
	if ( m_queueRecvMessages.m_pLast && unlikely( m_queueRecvMessages.m_pLast->m_nMessageNumber > pMsg->m_nMessageNumber ) )
	{

		// Scan the linked list to find the insertion point
		int nMessagesGreaterThanCurrent = 1;
		CSteamNetworkingMessage *pMsgInsertBefore = m_queueRecvMessages.m_pLast;
		const int64 nMessageNumberLast = pMsgInsertBefore->m_nMessageNumber;
		for (;;)
		{
			Assert( pMsgInsertBefore->m_links.m_pQueue == &m_queueRecvMessages );
			CSteamNetworkingMessage *pPrev = pMsgInsertBefore->m_links.m_pPrev;
			if ( !pPrev || pPrev->m_nMessageNumber <= nMessageNumber )
				break;
			pMsgInsertBefore = pPrev;
			++nMessagesGreaterThanCurrent;
		}

		const int64 nMessageNumberInsertBefore = pMsgInsertBefore->m_nMessageNumber;

		// Insert before this in the main queue for the connection
		pMsg->LinkBefore( pMsgInsertBefore, &CSteamNetworkingMessage::m_links, &m_queueRecvMessages );

		// If there's a poll group, then insert immediately
		// before this message in that queue, too.
		if ( m_pPollGroup )
		{
			if ( pMsgInsertBefore->m_linksSecondaryQueue.m_pQueue == &m_pPollGroup->m_queueRecvMessages )
			{
				pMsg->LinkBefore( pMsgInsertBefore, &CSteamNetworkingMessage::m_linksSecondaryQueue, &m_pPollGroup->m_queueRecvMessages );
			}
			else
			{
				Assert( false );
			}
		}
		else
		{
			Assert( pMsgInsertBefore->m_linksSecondaryQueue.m_pQueue == nullptr );
		}

		// Unlock before we spew
		g_lockAllRecvMessageQueues.unlock();

		// NOTE - message could have been pulled out of the queue
		// and consumed by the app already here

		SpewMsgGroup( m_connectionConfig.LogLevel_Message.Get(), "[%s] Received Msg %lld out of order.  %d message(s) queued with higher numbers, IDs %lld ... %lld\n",
			GetDescription(),
			(long long)nMessageNumber,
			nMessagesGreaterThanCurrent,
			(long long)nMessageNumberInsertBefore,
			(long long)nMessageNumberLast );

	}
	else
	{

		// Add to end of my queue.
		pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_links, &m_queueRecvMessages );

		// Add to the poll group, if we are in one
		if ( m_pPollGroup )
			pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_linksSecondaryQueue, &m_pPollGroup->m_queueRecvMessages );

		// Each if() branch has its own unlock, so we can spew in the branch above
		g_lockAllRecvMessageQueues.unlock();
	}

	return true;
}

void CSteamNetworkConnectionBase::PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState )
{
	// Send API callback for this state change?
	// Do not post if connection state has not changed from an API perspective.
	// And don't post notifications for internal connections used by ad-hoc-style endpoints.
	if ( eOldAPIState != eNewAPIState && !m_pMessagesEndPointSessionOwner )
	{

		SteamNetConnectionStatusChangedCallback_t c;
		ConnectionPopulateInfo( c.m_info );
		c.m_eOldState = eOldAPIState;
		c.m_hConn = m_hConnectionSelf;

		void *fnCallback = m_connectionConfig.Callback_ConnectionStatusChanged.Get();

		// Typical codepath - post to a queue
		m_pSteamNetworkingSocketsInterface->QueueCallback( c, fnCallback );
	}

	// Send diagnostics for this state change?
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
		if (
			m_pSteamNetworkingSocketsInterface->m_pSteamNetworkingUtils->m_usecConnectionUpdateFrequency == 0 // Not enabled globally right now
			|| ( m_usecWhenNextDiagnosticsUpdate == k_nThinkTime_Never && !BStateIsActive() ) // We've sent a terminal state change.  This transition isn't interesting from a diagnostic standpoint.
		) {
			// Disabled, don't send any more
			m_usecWhenNextDiagnosticsUpdate = k_nThinkTime_Never;
		}
		else if ( m_connectionConfig.EnableDiagnosticsUI.Get() != 0 )
		{

			// Post an update.  If more should be sent, we'll schedule it.
			// NOTE: Here we are going to ask the connection to populate SteamNetConnectionInfo_t info
			// *again*, even though we just called ConnectionPopulateInfo above.  But this keeps the code
			// simpler and connectoin state changes are infrequent, relatively speaking
			SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
			m_pSteamNetworkingSocketsInterface->m_pSteamNetworkingUtils->PostConnectionStateUpdateForDiagnosticsUI( eOldAPIState, this, usecNow );
			Assert( m_usecWhenNextDiagnosticsUpdate > usecNow );

			// If we might need to send another, schedule a wakeup call
			EnsureMinThinkTime( m_usecWhenNextDiagnosticsUpdate );
		}
	#endif
}

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
void CSteamNetworkConnectionBase::CheckScheduleDiagnosticsUpdateASAP()
{
	if ( m_pSteamNetworkingSocketsInterface->m_pSteamNetworkingUtils->m_usecConnectionUpdateFrequency <= 0 )
	{
		m_usecWhenNextDiagnosticsUpdate = k_nThinkTime_Never;
	}
	else if ( m_usecWhenNextDiagnosticsUpdate == k_nThinkTime_Never )
	{
		// We've sent our last update.  Don't send any more
	}
	else if ( m_connectionConfig.EnableDiagnosticsUI.Get() != 0 )
	{
		m_usecWhenNextDiagnosticsUpdate = k_nThinkTime_ASAP;
		SetNextThinkTimeASAP();
	}
}

// FIXME - Should we just move this into SteamNetConnectionInfo_t?
// Seems like maybe we should just provide a localized result directly
// there.
static const char *GetConnectionStateLocToken( ESteamNetworkingConnectionState eOldConnState,  ESteamNetworkingConnectionState eState, int nEndReason )
{
	if ( eState == k_ESteamNetworkingConnectionState_Connecting )
		return "#SteamNetSockets_Connecting";
	if ( eState == k_ESteamNetworkingConnectionState_FindingRoute )
		return "#SteamNetSockets_FindingRoute";
	if ( eState == k_ESteamNetworkingConnectionState_Connected )
		return "#SteamNetSockets_Connected";

	if ( nEndReason == k_ESteamNetConnectionEnd_Misc_Timeout )
	{
		if ( eOldConnState == k_ESteamNetworkingConnectionState_Connecting )
			return "#SteamNetSockets_Disconnect_ConnectionTimedout";
		return "#SteamNetSockets_Disconnect_TimedOut";
	}
	if ( eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
	{
		if ( nEndReason >= k_ESteamNetConnectionEnd_Local_Min && nEndReason <= k_ESteamNetConnectionEnd_Local_Max )
		{
			if ( nEndReason == k_ESteamNetConnectionEnd_Local_ManyRelayConnectivity )
				return "#SteamNetSockets_Disconnect_LocalProblem_ManyRelays";
			if ( nEndReason == k_ESteamNetConnectionEnd_Local_HostedServerPrimaryRelay )
				return "#SteamNetSockets_Disconnect_LocalProblem_HostedServerPrimaryRelay";
			if ( nEndReason == k_ESteamNetConnectionEnd_Local_NetworkConfig )
				return "#SteamNetSockets_Disconnect_LocalProblem_NetworkConfig";
			return "#SteamNetSockets_Disconnect_LocalProblem_Other";
		}

		if ( nEndReason >= k_ESteamNetConnectionEnd_Remote_Min && nEndReason <= k_ESteamNetConnectionEnd_Remote_Max )
		{
			if ( nEndReason == k_ESteamNetConnectionEnd_Remote_Timeout )
			{
				if ( eOldConnState == k_ESteamNetworkingConnectionState_Connecting )
					return "#SteamNetSockets_Disconnect_RemoteProblem_TimeoutConnecting";
				return "#SteamNetSockets_Disconnect_RemoteProblem_Timeout";
			}

			if ( nEndReason == k_ESteamNetConnectionEnd_Remote_BadCrypt )
				return "#SteamNetSockets_Disconnect_RemoteProblem_BadCrypt";
			if ( nEndReason == k_ESteamNetConnectionEnd_Remote_BadCert )
				return "#SteamNetSockets_Disconnect_RemoteProblem_BadCert";
		}

		if ( nEndReason == k_ESteamNetConnectionEnd_Misc_P2P_Rendezvous )
			return "#SteamNetSockets_Disconnect_P2P_Rendezvous";

		if ( nEndReason == k_ESteamNetConnectionEnd_Misc_SteamConnectivity )
			return "#SteamNetSockets_Disconnect_SteamConnectivity";

		if ( nEndReason == k_ESteamNetConnectionEnd_Misc_InternalError )
			return "#SteamNetSockets_Disconnect_InternalError";

		return "#SteamNetSockets_Disconnect_Unusual";
	}
	if ( eState == k_ESteamNetworkingConnectionState_ClosedByPeer )
	{
		if ( nEndReason >= k_ESteamNetConnectionEnd_Local_Min && nEndReason <= k_ESteamNetConnectionEnd_Local_Max )
			return "#SteamNetSockets_PeerClose_LocalProblem";

		if ( nEndReason >= k_ESteamNetConnectionEnd_Remote_Min && nEndReason <= k_ESteamNetConnectionEnd_Remote_Max )
		{
			if ( nEndReason == k_ESteamNetConnectionEnd_Remote_BadCrypt )
				return "#SteamNetSockets_PeerClose_RemoteProblem_BadCrypt";
			if ( nEndReason == k_ESteamNetConnectionEnd_Remote_BadCert )
				return "#SteamNetSockets_PeerClose_RemoteProblem_BadCert";
			// Peer closed the connection, and they think it's our fault somehow?
		}

		if ( nEndReason >= k_ESteamNetConnectionEnd_App_Min && nEndReason <= k_ESteamNetConnectionEnd_App_Max )
		{
			return "#SteamNetSockets_PeerClose_App_Normal";
		}
		if ( nEndReason >= k_ESteamNetConnectionEnd_AppException_Min && nEndReason <= k_ESteamNetConnectionEnd_AppException_Max )
		{
			return "#SteamNetSockets_PeerClose_App_Unusual";
		}
		return "#SteamNetSockets_PeerClose_Ununusual";
	}

	if ( nEndReason >= k_ESteamNetConnectionEnd_App_Min && nEndReason <= k_ESteamNetConnectionEnd_App_Max )
	{
		return "#SteamNetSockets_AppClose_Normal";
	}
	if ( nEndReason >= k_ESteamNetConnectionEnd_AppException_Min && nEndReason <= k_ESteamNetConnectionEnd_AppException_Max )
	{
		return "#SteamNetSockets_AppClose_Unusual";
	}

	return "";
}

void CSteamNetworkConnectionBase::ConnectionPopulateDiagnostics( ESteamNetworkingConnectionState eOldState, CGameNetworkingUI_ConnectionState &msgConnectionState, SteamNetworkingMicroseconds usecNow )
{
	msgConnectionState.set_start_time( (usecNow - m_usecWhenCreated)/k_nMillion );

	// Use the API function to populate the struct
	SteamNetworkingDetailedConnectionStatus stats;
	APIGetDetailedConnectionStatus( stats, usecNow );

	// Fill in diagnostic fields that correspond to SteamNetConnectionInfo_t
	if ( stats.m_eTransportKind != k_ESteamNetTransport_Unknown )
		msgConnectionState.set_transport_kind( stats.m_eTransportKind );
	if ( stats.m_info.m_idPOPRelay )
		msgConnectionState.set_sdrpopid_local( SteamNetworkingPOPIDRender( stats.m_info.m_idPOPRelay ).c_str() );
	if ( stats.m_info.m_idPOPRemote )
		msgConnectionState.set_sdrpopid_remote( SteamNetworkingPOPIDRender( stats.m_info.m_idPOPRemote ).c_str() );
	if ( !stats.m_info.m_addrRemote.IsIPv6AllZeros() )
		msgConnectionState.set_address_remote( SteamNetworkingIPAddrRender( stats.m_info.m_addrRemote ).c_str() );

	msgConnectionState.set_connection_id_local( m_unConnectionIDLocal );
	msgConnectionState.set_identity_local( SteamNetworkingIdentityRender( m_identityLocal ).c_str() );
	if ( !m_identityRemote.IsInvalid() && m_identityRemote.m_eType != k_ESteamNetworkingIdentityType_IPAddress )
		msgConnectionState.set_identity_remote( SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
	switch ( GetState() )
	{
		default:
			Assert( false );
		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_Linger:
			msgConnectionState.set_connection_state( GetState() );
			break;

		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Dead:
			// Use last public state
			msgConnectionState.set_connection_state( GetWireState() );
			break;
	}

	msgConnectionState.set_status_loc_token( GetConnectionStateLocToken( eOldState, stats.m_info.m_eState, stats.m_info.m_eEndReason ) );

	// Check if we've been closed, and schedule the next diagnostics update
	if ( m_eEndReason != k_ESteamNetConnectionEnd_Invalid )
	{
		msgConnectionState.set_close_reason( GetConnectionEndReason() );
		msgConnectionState.set_close_message( GetConnectionEndDebugString() );
	}
	if ( BStateIsActive() )
	{
		m_usecWhenNextDiagnosticsUpdate = usecNow + m_pSteamNetworkingSocketsInterface->m_pSteamNetworkingUtils->m_usecConnectionUpdateFrequency;
	}
	else
	{
		m_usecWhenNextDiagnosticsUpdate = k_nThinkTime_Never;
	}

	// end-to-end stats measured locally
	LinkStatsInstantaneousStructToMsg( stats.m_statsEndToEnd.m_latest, *msgConnectionState.mutable_e2e_quality_local()->mutable_instantaneous() );
	LinkStatsLifetimeStructToMsg( stats.m_statsEndToEnd.m_lifetime, *msgConnectionState.mutable_e2e_quality_local()->mutable_lifetime() );

	// end-to-end stats from remote host, if any
	if ( stats.m_statsEndToEnd.m_flAgeLatestRemote >= 0.0f )
	{
		msgConnectionState.set_e2e_quality_remote_instantaneous_time( uint64( stats.m_statsEndToEnd.m_flAgeLatestRemote * 1e6 ) );
		LinkStatsInstantaneousStructToMsg( stats.m_statsEndToEnd.m_latestRemote, *msgConnectionState.mutable_e2e_quality_remote()->mutable_instantaneous() );
	}
	if ( stats.m_statsEndToEnd.m_flAgeLifetimeRemote >= 0.0f )
	{
		msgConnectionState.set_e2e_quality_remote_lifetime_time( uint64( stats.m_statsEndToEnd.m_flAgeLifetimeRemote * 1e6 ) );
		LinkStatsLifetimeStructToMsg( stats.m_statsEndToEnd.m_lifetimeRemote, *msgConnectionState.mutable_e2e_quality_remote()->mutable_lifetime() );
	}

	if ( !stats.m_addrPrimaryRouter.IsIPv6AllZeros() && stats.m_statsPrimaryRouter.m_lifetime.m_nPktsRecvSequenced > 0 )
	{
		LinkStatsInstantaneousStructToMsg( stats.m_statsPrimaryRouter.m_latest, *msgConnectionState.mutable_front_quality_local()->mutable_instantaneous() );
		LinkStatsLifetimeStructToMsg( stats.m_statsPrimaryRouter.m_lifetime, *msgConnectionState.mutable_front_quality_local()->mutable_lifetime() );

		if ( stats.m_statsPrimaryRouter.m_flAgeLatestRemote >= 0.0f )
		{
			msgConnectionState.set_front_quality_remote_instantaneous_time( uint64( stats.m_statsPrimaryRouter.m_flAgeLatestRemote * 1e6 ) );
			LinkStatsInstantaneousStructToMsg( stats.m_statsPrimaryRouter.m_latestRemote, *msgConnectionState.mutable_front_quality_remote()->mutable_instantaneous() );
		}
		if ( stats.m_statsPrimaryRouter.m_flAgeLifetimeRemote >= 0.0f )
		{
			msgConnectionState.set_front_quality_remote_lifetime_time( uint64( stats.m_statsPrimaryRouter.m_flAgeLifetimeRemote * 1e6 ) );
			LinkStatsLifetimeStructToMsg( stats.m_statsPrimaryRouter.m_lifetimeRemote, *msgConnectionState.mutable_front_quality_remote()->mutable_lifetime() );
		}
	}

	// If any selected transport, give them a chance to fill in info
	if ( m_pTransport )
		m_pTransport->TransportPopulateDiagnostics( msgConnectionState, usecNow );
}

#endif

void CSteamNetworkConnectionBase::ConnectionState_ProblemDetectedLocally( ESteamNetConnectionEnd eReason, const char *pszFmt, ... )
{
	AssertLocksHeldByCurrentThread();

	va_list ap;

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	Assert( eReason > k_ESteamNetConnectionEnd_AppException_Max );
	Assert( pszFmt && *pszFmt );
	if ( m_eEndReason == k_ESteamNetConnectionEnd_Invalid || GetState() == k_ESteamNetworkingConnectionState_Linger )
	{
		m_eEndReason = eReason;
		va_start(ap, pszFmt);
		V_vsprintf_safe( m_szEndDebug, pszFmt, ap );
		va_end(ap);
	}

	// Check our state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			AssertMsg( false, "[%s] problem (%d) %s, but connection already dead (%d %d %s)",
				GetDescription(), (int)eReason, pszFmt, GetState(), (int)m_eEndReason, m_szEndDebug );
			return;

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			// Don't do anything
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			ConnectionState_FinWait();
			return;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:

			SpewMsg( "[%s] problem detected locally (%d): %s\n", GetDescription(), (int)m_eEndReason, m_szEndDebug );

			SetState( k_ESteamNetworkingConnectionState_ProblemDetectedLocally, usecNow );
			break;
	}

	// We don't have enough context to know if it's safe to act now.  Just schedule an immediate
	// wake up call so we will take action at the next safe time
	SetNextThinkTimeASAP();
}

void CSteamNetworkConnectionBase::ConnectionState_FinWait()
{
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Check our state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_None:
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_FinWait:
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:
			SetState( k_ESteamNetworkingConnectionState_FinWait, usecNow );

			// We don't have enough context to know if it's safe to act now.  Just schedule an immediate
			// wake up call so we will take action at the next safe time
			SetNextThinkTimeASAP();
			break;
	}
}

void CSteamNetworkConnectionBase::ConnectionState_ClosedByPeer( int nReason, const char *pszDebug )
{

	// Check our state
	switch ( m_eConnectionState )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_FinWait:
			// Keep hanging out until the fin wait time is up
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			// Hang out to gracefully handle any last stray packets,
			// clean up relay sessions, etc.
			ConnectionState_FinWait();
			break;

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			// Just ignore this.  We detected a problem, but now the peer
			// is also trying to close the connection.  In any case, we
			// need to wait for the client code to close the handle
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			// We already knew this, we're just waiting for
			// the client code to clean up the handle.
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:

			if ( pszDebug && *pszDebug )
				V_strcpy_safe( m_szEndDebug, pszDebug );
			else if ( m_szEndDebug[0] == '\0' )
				V_strcpy_safe( m_szEndDebug, "The remote host closed the connection." );
			m_eEndReason = ESteamNetConnectionEnd( nReason );

			SpewMsg( "[%s] closed by peer (%d): %s\n", GetDescription(), (int)m_eEndReason, m_szEndDebug );

			SetState( k_ESteamNetworkingConnectionState_ClosedByPeer, SteamNetworkingSockets_GetLocalTimestamp() );
			break;
	}
}

bool CSteamNetworkConnectionBase::BConnectionState_Connecting( SteamNetworkingMicroseconds usecNow, SteamNetworkingErrMsg &errMsg )
{
	// Already failed (and they didn't handle it)?  We should only transition to this state from the initial state
	if ( GetState() != k_ESteamNetworkingConnectionState_None )
	{
		V_sprintf_safe( errMsg, "Unexpected state %d", GetState() );
		AssertMsg( false, "[%s] %s", GetDescription(), errMsg );
		return false;
	}

	// Check if symmetric mode is being requested, but doesn't make sense
	if ( BSymmetricMode() )
	{
		if ( !BSupportsSymmetricMode() )
		{
			V_strcpy_safe( errMsg, "SymmetricConnect not supported" );
			return false;
		}
		if ( m_identityRemote.IsInvalid() )
		{
			V_strcpy_safe( errMsg, "Remote identity must be known to use symmetric mode" );
			AssertMsg( false, errMsg );
			return false;
		}
	}

	// Set the state
	SetState( k_ESteamNetworkingConnectionState_Connecting, usecNow );

	// Schedule a wakeup call ASAP so we can start sending out packets immediately
	SetNextThinkTimeASAP();

	// OK
	return true;
}

void CSteamNetworkConnectionBase::ConnectionState_Connected( SteamNetworkingMicroseconds usecNow )
{
	// Check our state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		default:
			Assert( false );
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
			{
				// We must receive a packet in order to be connected!
				Assert( m_statsEndToEnd.m_usecTimeLastRecv > 0 );

				// Should only enter this state once
				Assert( m_statsEndToEnd.m_usecWhenStartedConnectedState == 0 );
				m_statsEndToEnd.m_usecWhenStartedConnectedState = usecNow;

				// Spew, if this is newsworthy
				if ( !m_bConnectionInitiatedRemotely || GetState() == k_ESteamNetworkingConnectionState_FindingRoute )
					SpewMsg( "[%s] connected\n", GetDescription() );

				SetState( k_ESteamNetworkingConnectionState_Connected, usecNow );

				SNP_InitializeConnection( usecNow );
			}

			break;

		case k_ESteamNetworkingConnectionState_Connected:
			break;
	}

	// Schedule a wakeup call ASAP so we can start sending out packets immediately
	SetNextThinkTimeASAP();
}

void CSteamNetworkConnectionBase::ConnectionState_FindingRoute( SteamNetworkingMicroseconds usecNow )
{
	// Check our state, we really should only transition into this state from one state.
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_Connected:
		default:
			Assert( false );
			break;

		case k_ESteamNetworkingConnectionState_Connecting:

			// Spew, if this is newsworthy
			if ( !m_bConnectionInitiatedRemotely )
				SpewMsg( "[%s] finding route\n", GetDescription() );

			SetState( k_ESteamNetworkingConnectionState_FindingRoute, usecNow );
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:
			break;
	}

	// Schedule a wakeup call ASAP so we can start sending out packets immediately
	SetNextThinkTimeASAP();
}

void CSteamNetworkConnectionBase::CheckConnectionStateOrScheduleWakeUp( SteamNetworkingMicroseconds usecNow )
{
	m_pLock->AssertHeldByCurrentThread();

	// FIXME - try to take global lock, and if we can take it, then take action immediately
	SetNextThinkTimeASAP();
}

void CSteamNetworkConnectionBase::Think( SteamNetworkingMicroseconds usecNow )
{
	// NOTE: Lock has already been taken by ILockableThinker.  So we need to unlock it when we're done
	ConnectionScopeLock scopeLock;
	scopeLock.TakeLockOwnership( m_pLock, "CSteamNetworkConnectionBase::Think" );

	// Safety check against leaving callbacks suppressed.  If this fires, there's a good chance
	// we have already suppressed a callback that we should have posted, which is very bad
	AssertMsg( m_nSupressStateChangeCallbacks == 0, "[%s] m_nSupressStateChangeCallbacks left on!", GetDescription() );
	m_nSupressStateChangeCallbacks = 0;

	// Assume a default think interval just to make sure we check in periodically
	SteamNetworkingMicroseconds usecMinNextThinkTime = usecNow + k_nMillion;

	// Use a macro so that if we assert, we'll get a real line number
	#define UpdateMinThinkTime(x) \
	{ \
		/* assign into temporary in case x is an expression with side effects */ \
		SteamNetworkingMicroseconds usecNextThink = (x);  \
		/* Scheduled think time must be in the future.  If some code is setting a think */ \
		/* time for right now, then it should have just done it. */ \
		if ( usecNextThink <= usecNow ) { \
			AssertMsg1( false, "Trying to set next think time %lldusec in the past", (long long)( usecNow - usecMinNextThinkTime ) ); \
			usecNextThink = usecNow + 10*1000; \
		} \
		if ( usecNextThink < usecMinNextThinkTime ) \
			usecMinNextThinkTime = usecNextThink; \
	}

	// Check our state
	switch ( m_eConnectionState )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			// WAT
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_FinWait:
		{
			SteamNetworkingMicroseconds usecTimeout = m_usecWhenEnteredConnectionState + k_usecFinWaitTimeout;

			// If we're linked to a messages session, they need to unlink us!
			#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
				if ( m_pMessagesEndPointSessionOwner )
				{
					m_pMessagesEndPointSessionOwner->SetNextThinkTimeASAP();

					// And go ahead and schedule our own wakeup call
					SetNextThinkTime( std::max( usecTimeout, usecNow + 10*1000 ) );
					return;
				}
			#endif

			if ( usecNow >= usecTimeout )
			{
				ConnectionQueueDestroy();
				return;
			}

			// It's not time yet, make sure we get our callback when it's time.
			SetNextThinkTime( usecTimeout );
		}
		return;

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		{
			// If we're linked to a messages session, they need to unlink us!
			#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
				if ( m_pMessagesEndPointSessionOwner )
				{
					m_pMessagesEndPointSessionOwner->SetNextThinkTimeASAP();
					return;
				}
			#endif

			// We don't send any data packets or keepalives in this state.
			// We're just waiting for the client API to close us.  Let's check
			// in once after a pretty lengthy delay, and assert if we're still alive
			SteamNetworkingMicroseconds usecTimeout = m_usecWhenEnteredConnectionState + 20*k_nMillion;
			if ( usecNow >= usecTimeout )
			{
				SpewBug( "[%s] We are in state %d and have been waiting %.1fs to be cleaned up.  Did you forget to call CloseConnection()?",
					GetDescription(), m_eConnectionState, ( usecNow - m_usecWhenEnteredConnectionState ) * 1e-6f );
			}
			else
			{
				SetNextThinkTime( usecTimeout );
			}
		}
		return;

		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connecting:
		{

			// Timeout?
			SteamNetworkingMicroseconds usecTimeout = m_usecWhenEnteredConnectionState + (SteamNetworkingMicroseconds)m_connectionConfig.TimeoutInitial.Get()*1000;
			if ( usecNow >= usecTimeout )
			{
				// Check if the application just didn't ever respond, it's probably a bug.
				// We should squawk about this and let them know.
				if ( m_eConnectionState != k_ESteamNetworkingConnectionState_FindingRoute && m_bConnectionInitiatedRemotely )
				{
					// Discard this warning if we are handling the state changes internally.  It's part of
					// the API design that the app can ignore these connections if they do not want to accept them.
					if ( m_pMessagesEndPointSessionOwner )
					{
						ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Timeout, "%s", "App did not respond; discarding." );
					}
					else
					{
						SpewBug( "[%s] Application didn't accept or close incoming connection in a reasonable amount of time.  This is probably a bug in application code!\n", GetDescription() );
						ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Timeout, "%s", "App didn't accept or close incoming connection in time." );
					}
				}
				else
				{
					ConnectionTimedOut( usecNow );
				}
				AssertMsg2( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally, "[%s] ConnectionTimedOut didn't do what it is supposed to, state=%d!", GetDescription(), (int)GetState() );
				return;
			}

			// Make sure we wake up when timeout happens
			UpdateMinThinkTime( usecTimeout );

			// FInding route?
			if ( m_eConnectionState == k_ESteamNetworkingConnectionState_FindingRoute )
			{
				UpdateMinThinkTime( ThinkConnection_FindingRoute( usecNow ) );
			}
			else if ( m_bConnectionInitiatedRemotely )
			{
				// We're waiting on the app to accept the connection.  Nothing to do here.
			}
			else
			{

				// Do we have all of our crypt stuff ready?
				if ( BThinkCryptoReady( usecNow ) )
				{
					// Send connect requests
					UpdateMinThinkTime( ThinkConnection_ClientConnecting( usecNow ) );
				}
				else
				{
					// Waiting on certs, etc.  Make sure and check
					// back in periodically. Note that we we should be awoken
					// immediately when we get our cert.  Is this necessary?
					// Might just be hiding a bug.
					UpdateMinThinkTime( usecNow + k_nMillion/20 );
				}
			}
		} break;

		case k_ESteamNetworkingConnectionState_Linger:

			// Have we sent everything we wanted to?
			if ( BReadyToExitLingerState() )
			{

				// Close the connection ASAP
				ConnectionState_FinWait();
				return;
			}
			// FALLTHROUGH
			// |
			// |
			// V
		case k_ESteamNetworkingConnectionState_Connected:
		{
			if ( m_pTransport && m_pTransport->BCanSendEndToEndData() )
			{
				SteamNetworkingMicroseconds usecNextThinkSNP = SNP_ThinkSendState( usecNow );

				// Set a pretty tight tolerance if SNP wants to wake up at a certain time.
				UpdateMinThinkTime( usecNextThinkSNP );
			}
			else
			{
				UpdateMinThinkTime( usecNow + 20*1000 );
			}
		} break;
	}

	// Update stats
	m_statsEndToEnd.Think( usecNow );
	UpdateMTUFromConfig( false );

	// Check for sending keepalives or probing a connection that appears to be timing out
	if ( BStateIsConnectedForWirePurposes() )
	{
		Assert( m_statsEndToEnd.m_usecTimeLastRecv > 0 ); // How did we get connected without receiving anything end-to-end?
		AssertMsg( m_statsEndToEnd.IsActive(), "[%s] stats are in activity level %d, but in state %d?", GetDescription(), (int)m_statsEndToEnd.GetActivityLevel(), (int)GetState() );

		// Not able to send end-to-end data?
		bool bCanSendEndToEnd = m_pTransport && m_pTransport->BCanSendEndToEndData();

		// Mark us as "timing out" if we are not able to send end-to-end data
		if ( !bCanSendEndToEnd && m_statsEndToEnd.m_usecWhenTimeoutStarted == 0 )
			m_statsEndToEnd.m_usecWhenTimeoutStarted = usecNow;

		// Are we timing out?
		if ( m_statsEndToEnd.m_usecWhenTimeoutStarted > 0 )
		{

			// When will the timeout hit?
			SteamNetworkingMicroseconds usecEndToEndConnectionTimeout = std::max( m_statsEndToEnd.m_usecWhenTimeoutStarted, m_statsEndToEnd.m_usecTimeLastRecv ) + (SteamNetworkingMicroseconds)m_connectionConfig.TimeoutConnected.Get()*1000;

			// Time to give up?
			if ( usecNow >= usecEndToEndConnectionTimeout )
			{
				if ( m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv >= 4 || !bCanSendEndToEnd )
				{
					if ( bCanSendEndToEnd )
					{
						Assert( m_statsEndToEnd.m_usecWhenTimeoutStarted > 0 );
						SpewMsg( "[%s] Timed out.  %.1fms since last recv, %.1fms since timeout started, %d consecutive failures\n",
							GetDescription(), ( usecNow - m_statsEndToEnd.m_usecTimeLastRecv ) * 1e-3, ( usecNow - m_statsEndToEnd.m_usecWhenTimeoutStarted ) * 1e-3, m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv );
					}
					else
					{
						SpewMsg( "[%s] Timed out.  Cannot send end-to-end.  %.1fms since last recv, %d consecutive failures\n",
							GetDescription(), ( usecNow - m_statsEndToEnd.m_usecTimeLastRecv ) * 1e-3, m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv );
					}

					// Save state for assert
					#ifdef DBGFLAG_ASSERT
					ESteamNetworkingConnectionState eOldState = GetState();
					#endif

					// Check for connection-type-specific handling
					ConnectionTimedOut( usecNow );

					// Make sure that worked
					AssertMsg3(
						GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally
						|| ( eOldState == k_ESteamNetworkingConnectionState_Linger && GetState() == k_ESteamNetworkingConnectionState_FinWait ),
						"[%s] ConnectionTimedOut didn't do what it is supposed to! (%d -> %d)", GetDescription(), (int)eOldState, (int)GetState() );
					return;
				}
			}

			// Make sure we are waking up regularly to check in while this is going on
			UpdateMinThinkTime( usecNow + 50*1000 );
		}

		// Check for sending keepalives and stats
		if ( bCanSendEndToEnd )
		{

			// Urgent keepalive because we are timing out?
			SteamNetworkingMicroseconds usecStatsNextThinkTime = k_nThinkTime_Never;
			EStatsReplyRequest eReplyRequested;
			const char *pszStatsReason = m_statsEndToEnd.GetSendReasonOrUpdateNextThinkTime( usecNow, eReplyRequested, usecStatsNextThinkTime );
			if ( pszStatsReason )
			{

				// Spew if we're dropping replies
				if ( m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv == 1 )
					SpewVerbose( "[%s] Reply timeout, last recv %.1fms ago.  Sending keepalive.\n", GetDescription(), ( usecNow - m_statsEndToEnd.m_usecTimeLastRecv ) * 1e-3 );
				else if ( m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv > 0 )
					SpewMsg( "[%s] %d reply timeouts, last recv %.1fms ago.  Sending keepalive.\n", GetDescription(), m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv, ( usecNow - m_statsEndToEnd.m_usecTimeLastRecv ) * 1e-3 );

				// Send it
				m_pTransport->SendEndToEndStatsMsg( eReplyRequested, usecNow, pszStatsReason );

				// Re-calculate next think time
				usecStatsNextThinkTime = k_nThinkTime_Never;
				const char *pszStatsReason2 = m_statsEndToEnd.GetSendReasonOrUpdateNextThinkTime( usecNow, eReplyRequested, usecStatsNextThinkTime );
				AssertMsg1( pszStatsReason2 == nullptr && usecStatsNextThinkTime > usecNow, "Stats sending didn't clear stats need to send reason %s!", pszStatsReason2 ? pszStatsReason2 : "??" );
				(void)pszStatsReason2; // Suppress warning if asserts aren't enabled
			}

			// Make sure we are scheduled to wake up the next time we need to take action
			UpdateMinThinkTime( usecStatsNextThinkTime );
		}
	}

	// Hook for derived class to do its connection-type-specific stuff
	ThinkConnection( usecNow );

	// Check for sending diagnostics periodically
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
		if ( m_usecWhenNextDiagnosticsUpdate <= usecNow )
		{
			m_pSteamNetworkingSocketsInterface->m_pSteamNetworkingUtils->PostConnectionStateUpdateForDiagnosticsUI( CollapseConnectionStateToAPIState( GetState() ), this, usecNow );
			Assert( m_usecWhenNextDiagnosticsUpdate > usecNow );
		}
		UpdateMinThinkTime( m_usecWhenNextDiagnosticsUpdate );
	#endif

	// Schedule next time to think, if derived class didn't request an earlier
	// wakeup call.
	EnsureMinThinkTime( usecMinNextThinkTime );

	#undef UpdateMinThinkTime
}

bool CSteamNetworkConnectionBase::BReadyToExitLingerState() const
{
	if ( m_senderState.m_cbPendingReliable == 0 && m_senderState.m_cbSentUnackedReliable == 0 )
	{
		Assert( m_senderState.m_listReadyRetryReliableRange.IsEmpty() );
		return true;
	}
	return false;
}

void CSteamNetworkConnectionBase::ThinkConnection( SteamNetworkingMicroseconds usecNow )
{
}

void CSteamNetworkConnectionBase::ProcessSNPPing( int msPing, RecvPacketContext_t &ctx )
{
	m_statsEndToEnd.m_ping.ReceivedPing( msPing, ctx.m_usecNow );
}

SteamNetworkingMicroseconds CSteamNetworkConnectionBase::ThinkConnection_FindingRoute( SteamNetworkingMicroseconds usecNow )
{
	return k_nThinkTime_Never;
}

SteamNetworkingMicroseconds CSteamNetworkConnectionBase::ThinkConnection_ClientConnecting( SteamNetworkingMicroseconds usecNow )
{
	Assert( !m_bConnectionInitiatedRemotely );

	// Default behaviour for client periodically sending connect requests
	
	// Ask transport if it's ready
	if ( !m_pTransport || !m_pTransport->BCanSendEndToEndConnectRequest() )
		return usecNow + k_nMillion/20; // Nope, check back in just a bit.

	// When to send next retry.
	SteamNetworkingMicroseconds usecRetry = m_usecWhenSentConnectRequest + k_usecConnectRetryInterval;
	if ( usecNow < usecRetry )
		return usecRetry; // attempt already in flight.  Wait until it's time to retry

	// Send a request
	m_pTransport->SendEndToEndConnectRequest( usecNow );
	m_usecWhenSentConnectRequest = usecNow;

	// And wakeup when it will be time to retry
	return m_usecWhenSentConnectRequest + k_usecConnectRetryInterval;
}

void CSteamNetworkConnectionBase::ConnectionTimedOut( SteamNetworkingMicroseconds usecNow )
{
	AssertLocksHeldByCurrentThread();

	ESteamNetConnectionEnd nReasonCode;
	ConnectionEndDebugMsg msg;

	// Set some generic defaults.
	nReasonCode = k_ESteamNetConnectionEnd_Misc_Timeout;
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Connecting:
			// Should use this more specific reason code at somepoint, when I add an API to get localized error messages
			//if ( !m_bConnectionInitiatedRemotely && m_statsEndToEnd.m_usecTimeLastRecv == 0 )
			//{
			//	nReasonCode = k_ESteamNetConnectionEnd_Misc_ServerNeverReplied;
			//	V_strcpy_safe( msg, "" );
			//}
			//else
			//{
				V_strcpy_safe( msg, "Timed out attempting to connect" );
			//}
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:
			nReasonCode = k_ESteamNetConnectionEnd_Misc_P2P_Rendezvous;
			V_strcpy_safe( msg, "Timed out attempting to negotiate rendezvous" );
			break;

		default:
			V_strcpy_safe( msg, "Connection dropped" );
			break;
	}

	// Check if connection has a more enlightened understanding of what's wrong
	ConnectionGuessTimeoutReason( nReasonCode, msg, usecNow );

	// Switch connection state
	ConnectionState_ProblemDetectedLocally( nReasonCode, "%s", msg );
}

void CSteamNetworkConnectionBase::ConnectionGuessTimeoutReason( ESteamNetConnectionEnd &nReasonCode, ConnectionEndDebugMsg &msg, SteamNetworkingMicroseconds usecNow )
{
	// Base class, just delegate to active transport
	if ( m_pTransport )
		m_pTransport->TransportGuessTimeoutReason( nReasonCode, msg, usecNow );
}

void CConnectionTransport::TransportGuessTimeoutReason( ESteamNetConnectionEnd &nReasonCode, ConnectionEndDebugMsg &msg, SteamNetworkingMicroseconds usecNow )
{
	// No enlightenments at base class
}

void CSteamNetworkConnectionBase::UpdateSpeeds( int nTXSpeed, int nRXSpeed )
{
	// FIXME
	//m_statsEndToEnd.UpdateSpeeds( nTXSpeed, nRXSpeed );
}

void CSteamNetworkConnectionBase::UpdateMTUFromConfig( bool bForceRecalc )
{
	if ( bForceRecalc )
	{
		Assert( m_senderState.m_cbSentUnackedReliable == 0 );
		Assert( m_senderState.m_listReadyRetryReliableRange.IsEmpty() );
		Assert( m_senderState.m_listSentReliableSegments.IsEmpty() );
	}
	else
	{
		int newMTUPacketSize = m_connectionConfig.MTU_PacketSize.Get();
		if ( newMTUPacketSize == m_cbMTUPacketSize )
			return;

		// Shrinking MTU?
		if ( newMTUPacketSize < m_cbMTUPacketSize )
		{
			// We cannot do this while we have any reliable segments in flight!
			// To keep things simple, the retries are always the original segments,
			// we never have our retries chop up the space differently than
			// the original send
			//
			// !FIXME! - This could be a deal-breaker.  It might in practice prevent
			// us from ever being able to reduce the MTU.
			if ( m_senderState.m_cbSentUnackedReliable != 0 || !m_senderState.m_listReadyRetryReliableRange.IsEmpty() )
			{
				Assert( !m_senderState.m_listSentReliableSegments.IsEmpty() );
				return;
			}
		}
	}

	m_cbMTUPacketSize = m_connectionConfig.MTU_PacketSize.Get();
	m_cbMaxPlaintextPayloadSend = m_cbMTUPacketSize - ( k_cbSteamNetworkingSocketsMaxUDPMsgLen - k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend ) - m_cbEncryptionOverhead;
	m_cbMaxMessageNoFragment = m_cbMaxPlaintextPayloadSend - k_cbSteamNetworkingSocketsNoFragmentHeaderReserve;

	// Max size of a reliable segment.  This is designed such that a reliable
	// message of size k_cbSteamNetworkingSocketsMaxMessageNoFragment
	// won't get fragmented, except perhaps in an exceedingly degenerate
	// case.  (Even in this case, the protocol will function properly, it
	// will just potentially fragment the message.)  We shouldn't make any
	// hard promises in this department.
	//
	// 1 byte - message header
	// 3 bytes - varint encode msgnum gap between previous reliable message.  (Gap could be greater, but this would be really unusual.)
	// 1 byte - size remainder bytes (assuming message is k_cbSteamNetworkingSocketsMaxMessageNoFragment, we only need a single size overflow byte)
	m_cbMaxReliableMessageSegment = m_cbMaxMessageNoFragment + 5;
}

CSteamNetworkConnectionP2P *CSteamNetworkConnectionBase::AsSteamNetworkConnectionP2P()
{
	return nullptr;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkConnectionPipe
//
/////////////////////////////////////////////////////////////////////////////

bool CSteamNetworkConnectionPipe::APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionPipe *pConn[2], const SteamNetworkingIdentity pIdentity[2] )
{
	SteamDatagramErrMsg errMsg;
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	ConnectionScopeLock scopeLock[2];

	// We can use the "fast path" here, which delivers messages to the other side
	// very efficiently, without taking the global lock or queuing stuff
	constexpr bool bUseFastPath = true;

	pConn[1] = new CSteamNetworkConnectionPipe( pSteamNetworkingSocketsInterface, pIdentity[0], scopeLock[0], bUseFastPath );
	pConn[0] = new CSteamNetworkConnectionPipe( pSteamNetworkingSocketsInterface, pIdentity[1], scopeLock[1], bUseFastPath );
	if ( !pConn[0] || !pConn[1] )
	{
failed:
		pConn[0]->ConnectionQueueDestroy(); pConn[0] = nullptr;
		pConn[1]->ConnectionQueueDestroy(); pConn[1] = nullptr;
		return false;
	}

	pConn[0]->m_pPartner = pConn[1];
	pConn[1]->m_pPartner = pConn[0];

	// Don't post any state changes for these transitions.  We just want to immediately start in the
	// connected state
	++pConn[0]->m_nSupressStateChangeCallbacks;
	++pConn[1]->m_nSupressStateChangeCallbacks;

	// Do generic base class initialization
	for ( int i = 0 ; i < 2 ; ++i )
	{
		CSteamNetworkConnectionPipe *p = pConn[i];
		CSteamNetworkConnectionPipe *q = pConn[1-i];
		if ( !p->BInitConnection( usecNow, 0, nullptr, errMsg ) )
		{
			AssertMsg1( false, "CSteamNetworkConnectionPipe::BInitConnection failed.  %s", errMsg );
			goto failed;
		}
		p->m_identityRemote = q->m_identityLocal;
		p->m_unConnectionIDRemote = q->m_unConnectionIDLocal;
	}

	// Exchange some dummy "connect" packets so that all of our internal variables
	// (and ping) look as realistic as possible
	pConn[0]->FakeSendStatsAndRecv( usecNow, 0 );
	pConn[1]->FakeSendStatsAndRecv( usecNow, 0 );

	// Tie the connections to each other, and mark them as connected
	for ( int i = 0 ; i < 2 ; ++i )
	{
		CSteamNetworkConnectionPipe *p = pConn[i];
		CSteamNetworkConnectionPipe *q = pConn[1-i];
		p->m_identityRemote = q->m_identityLocal;
		p->m_unConnectionIDRemote = q->m_unConnectionIDLocal;
		if ( p->RecvCryptoHandshake( q->m_msgSignedCertLocal, q->m_msgSignedCryptLocal, i==0, errMsg ) != k_ESteamNetConnectionEnd_Invalid )
		{
			AssertMsg( false, "RecvCryptoHandshake failed creating loopback pipe socket pair.  %s", errMsg );
			goto failed;
		}
		if ( !p->BConnectionState_Connecting( usecNow, errMsg ) )
		{
			AssertMsg( false, "BConnectionState_Connecting failed creating loopback pipe socket pair.  %s", errMsg );
			goto failed;
		}
		p->ConnectionState_Connected( usecNow );
	}

	// Any further state changes are legit
	pConn[0]->m_nSupressStateChangeCallbacks = 0;
	pConn[1]->m_nSupressStateChangeCallbacks = 0;
	return true;
}

CSteamNetworkConnectionPipe *CSteamNetworkConnectionPipe::CreateLoopbackConnection(
	CSteamNetworkingSockets *pClientInstance,
	int nOptions, const SteamNetworkingConfigValue_t *pOptions,
	CSteamNetworkListenSocketBase *pListenSocket, const SteamNetworkingIdentity &identityServerInitial,
	SteamNetworkingErrMsg &errMsg,
	ConnectionScopeLock &scopeLock
) {
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	CSteamNetworkingSockets *pServerInstance = pListenSocket->m_pSteamNetworkingSocketsInterface;

	// Use the fast path unless there is a messages endpoint
	bool bUseFastPath = pListenSocket->m_pMessagesEndPointOwner == nullptr;

	ConnectionScopeLock serverScopeLock;
	CSteamNetworkConnectionPipe *pClient = new CSteamNetworkConnectionPipe( pClientInstance, pClientInstance->InternalGetIdentity(), scopeLock, bUseFastPath );
	CSteamNetworkConnectionPipe *pServer = new CSteamNetworkConnectionPipe( pServerInstance, pServerInstance->InternalGetIdentity(), serverScopeLock, bUseFastPath );
	if ( !pClient || !pServer )
	{
		V_strcpy_safe( errMsg, "new CSteamNetworkConnectionPipe failed" );
failed:
		if ( pClient )
			pClient->ConnectionQueueDestroy();
		if ( pServer )
			pServer->ConnectionQueueDestroy();
		return nullptr;
	}

	// Link em up
	pClient->m_pPartner = pServer;
	pServer->m_pPartner = pClient;

	// Make sure initial identity is whatever the client used to initate the connection.  (E.g. FakeIP).
	// We want these loopback connections to behave as similarly as possible to ordinary connections.
	pClient->m_identityRemote = identityServerInitial;

	// Initialize client connection.  This triggers a state transition callback
	// to the "connecting" state
	if ( !pClient->BInitConnection( usecNow, nOptions, pOptions, errMsg ) )
		goto failed;

	// Server receives the connection and starts "accepting" it
	if ( !pServer->BBeginAccept( pListenSocket, usecNow, serverScopeLock, errMsg ) )
		goto failed;

	// Client sends a "connect" packet
	if ( !pClient->BConnectionState_Connecting( usecNow, errMsg ) )
		goto failed;
	Assert( pServer->m_statsEndToEnd.m_nMaxRecvPktNum == 1 );
	pClient->m_statsEndToEnd.m_nNextSendSequenceNumber = pServer->m_statsEndToEnd.m_nMaxRecvPktNum+1;
	pClient->FakeSendStatsAndRecv( usecNow, 0 );

	// Now we wait for the app to accept the connection
	return pClient;
}

// All pipe connections share the same lock!
static ConnectionLock s_sharedPipeLock;

CSteamNetworkConnectionPipe::CSteamNetworkConnectionPipe( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, const SteamNetworkingIdentity &identity, ConnectionScopeLock &scopeLock, bool bUseFastPath )
: CSteamNetworkConnectionBase( pSteamNetworkingSocketsInterface, scopeLock )
, CConnectionTransport( *static_cast<CSteamNetworkConnectionBase*>( this ) ) // connection and transport object are the same
, m_pPartner( nullptr )
{
	m_identityLocal = identity;
	m_pTransport = this;

	// Some pipe connections can use a shared, global lock.  It's more
	// efficient (we expect lock contention to be very low) because we
	// can very efficiently transfer messages from one side to the other.
	if ( bUseFastPath )
	{
		scopeLock.Unlock();
		m_pLock = &s_sharedPipeLock;
		scopeLock.Lock( *this );
	}

	// Encryption is not used for pipe connections.
	// This is not strictly necessary, since we never even send packets or
	// touch payload bytes at all, we just shift some pointers around.
	// But it's nice to make it official
	m_connectionConfig.Unencrypted.Set( 3 );

	// Slam in a really large SNP rate so that we are never rate limited
	int nRate = 0x10000000;
	m_connectionConfig.SendRateMin.Set( nRate );
	m_connectionConfig.SendRateMax.Set( nRate );

	// Don't limit the recv buffer.  (Send buffer doesn't
	// matter since we immediately transfer.)
	m_connectionConfig.RecvBufferSize.Set( 0x10000000 );
	m_connectionConfig.RecvBufferMessages.Set( 0x10000000 );
	m_connectionConfig.RecvMaxMessageSize.Set( 0x10000000 );

	// Diagnostics usually not useful on these types of connections.
	// (App can enable it or clear this override if it wants to.)
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
		m_connectionConfig.EnableDiagnosticsUI.Set(0);
	#endif
}

CSteamNetworkConnectionPipe::~CSteamNetworkConnectionPipe()
{
	Assert( !m_pPartner );
}

void CSteamNetworkConnectionPipe::GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const
{
	V_strcpy_safe( szDescription, "pipe" );
}

void CSteamNetworkConnectionPipe::InitConnectionCrypto( SteamNetworkingMicroseconds usecNow )
{
	// Always use unsigned cert, since we won't be doing any real crypto anyway
	SetLocalCertUnsigned();
}

EUnsignedCert CSteamNetworkConnectionPipe::AllowRemoteUnsignedCert()
{
	// It's definitely us, and we trust ourselves, right?
	return k_EUnsignedCert_Allow;
}

EUnsignedCert CSteamNetworkConnectionPipe::AllowLocalUnsignedCert()
{
	// It's definitely us, and we trust ourselves, right?  Don't even try to get a cert
	return k_EUnsignedCert_Allow;
}

int64 CSteamNetworkConnectionPipe::_APISendMessageToConnection( CSteamNetworkingMessage *pMsg, SteamNetworkingMicroseconds usecNow, bool *pbThinkImmediately )
{
	m_pLock->AssertHeldByCurrentThread();
	NOTE_UNUSED( pbThinkImmediately );
	if ( !m_pPartner )
	{
		// Caller should have checked the connection at a higher level, so this is a bug
		AssertMsg( false, "No partner pipe?" );
		pMsg->Release();
		return -k_EResultFail;
	}

	if ( (int)pMsg->m_idxLane >= len( m_senderState.m_vecLanes ) )
	{
		pMsg->Release();
		return -k_EResultInvalidParam;
	}
	SSNPSenderState::Lane &lane = m_senderState.m_vecLanes[ pMsg->m_idxLane ];

	// Set fields to their values applicable on the receiving side
	// NOTE: This assumes that we can muck with the structure,
	//       and that the caller won't need to look at the original
	//       object any more.
	int64 nMsgNum = ++lane.m_nLastSentMsgNum;
	pMsg->m_nMessageNumber = nMsgNum;
	pMsg->m_conn = m_pPartner->m_hConnectionSelf;
	pMsg->m_identityPeer = m_pPartner->m_identityRemote;
	pMsg->m_nConnUserData = m_pPartner->GetUserData();
	pMsg->m_usecTimeReceived = usecNow;

	// Fake sending a bunch of stats
	uint16 nWirePktNum = FakeSendStats( usecNow, pMsg->m_cbSize );

	// Can we immediately dispatch to the other side?
	if ( m_pLock == m_pPartner->m_pLock )
	{
		// Special fast path for when we don't need the global lock
		// to receive the messages.
		m_pPartner->FakeRecvStats( pMsg->m_usecTimeReceived, pMsg->m_cbSize, nWirePktNum );
		m_pPartner->ReceivedMessage( pMsg );
	}
	else
	{
		// Each side has its own lock.  We can't deliver it to the
		// other side immediately.

		// Queue a task to run when we have the global lock.
		struct DeliverMsgToPipePartner : CQueuedTaskOnTarget<CSteamNetworkConnectionPipe>
		{
			CSteamNetworkingMessage *m_pMsg;
			uint16 m_nWirePktNum;
			virtual void Run()
			{
				CSteamNetworkConnectionPipe *pConn = Target();
				ConnectionScopeLock connectionLock( *pConn );

				// Make sure connection has not been closed
				if ( pConn->m_pPartner )
				{
					pConn->FakeRecvStats( m_pMsg->m_usecTimeReceived, m_pMsg->m_cbSize, m_nWirePktNum );
					pConn->ReceivedMessage( m_pMsg );
				}
				else
				{
					m_pMsg->Release();
				}
			}
		};
		DeliverMsgToPipePartner *pTask = new DeliverMsgToPipePartner;
		pTask->m_pMsg = pMsg;
		pTask->m_nWirePktNum = nWirePktNum;
		pTask->SetTarget( m_pPartner );

		// Just always queue it -- don't try to handle immediately.
		pTask->QueueToRunWithGlobalLock( "DeliverMsgToPipePartner" );
	}

	// Return message number
	return nMsgNum;
}

bool CSteamNetworkConnectionPipe::BBeginAccept( CSteamNetworkListenSocketBase *pListenSocket, SteamNetworkingMicroseconds usecNow, ConnectionScopeLock &scopeLock, SteamDatagramErrMsg &errMsg )
{
	// We gotta have the global lock and both locks
	CSteamNetworkConnectionBase::AssertLocksHeldByCurrentThread();
	m_pPartner->m_pLock->AssertHeldByCurrentThread();

	// Ordinary connections usually learn the client identity and connection ID at this point
	m_identityRemote = m_pPartner->m_identityLocal;
	m_unConnectionIDRemote = m_pPartner->m_unConnectionIDLocal;

	// If they connected to us via a local FakeIP, we need to link up
	// their FakeIP, possibly assigning them an ephemeral one
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		if ( m_pPartner->m_identityRemote.IsFakeIP() )
		{
			Assert( !m_identityRemote.IsFakeIP() );
			Assert( !m_fakeIPRefRemote.IsValid() );
			int nPartnerLocalVirtualPort = m_pPartner->LocalVirtualPort();
			if ( IsVirtualPortGlobalFakePort( nPartnerLocalVirtualPort ) )
			{
				int nFakePort = nPartnerLocalVirtualPort - k_nVirtualPort_GlobalFakePort0;
				SteamNetworkingFakeIPResult_t fakePort;
				m_pPartner->m_pSteamNetworkingSocketsInterface->GetFakeIP( nFakePort, &fakePort );
				if ( fakePort.m_eResult != k_EResultOK || fakePort.m_unIP == 0 || fakePort.m_unPorts[0] == 0 )
				{
					V_strcpy_safe( errMsg, "Failed to get global FakeIP of client" );
					return false;
				}
				SteamNetworkingIPAddr addrFakeIP;
				addrFakeIP.SetIPv4( fakePort.m_unIP, fakePort.m_unPorts[0] );
				Assert( addrFakeIP.GetFakeIPType() == k_ESteamNetworkingFakeIPType_GlobalIPv4 );
				m_fakeIPRefRemote.Setup( addrFakeIP, m_identityRemote );
			}
			else
			{
				Assert( nPartnerLocalVirtualPort == -1 || IsVirtualPortEphemeralFakePort( nPartnerLocalVirtualPort ) );
				if ( !m_fakeIPRefRemote.SetupNewLocalIP( m_identityRemote, nullptr ) )
				{
					V_strcpy_safe( errMsg, "Failed to allocate ephemeral FakeIP to client" );
					return false;
				}
			}
		}
	#endif

	// Act like we came in on this listen socket
	if ( !pListenSocket->BAddChildConnection( this, errMsg ) )
		return false;

	// Assign connection ID, init crypto, transition to "connecting" state.
	if ( !BInitConnection( usecNow, 0, nullptr, errMsg ) )
		return false;

	// Receive the crypto info that is in the client's
	if ( RecvCryptoHandshake( m_pPartner->m_msgSignedCertLocal, m_pPartner->m_msgSignedCryptLocal, true, errMsg ) != k_ESteamNetConnectionEnd_Invalid )
		return false;

	// Is this connection for a messages session
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
		if ( pListenSocket->m_pMessagesEndPointOwner )
		{
			Assert( m_pLock != m_pPartner->m_pLock ); // We can't use the fast path

			// Switch into the connecting state now.  No state change callbacks
			++m_nSupressStateChangeCallbacks;
			bool bEnteredConnectingStateOK = BConnectionState_Connecting( usecNow, errMsg );
			--m_nSupressStateChangeCallbacks;
			if ( !bEnteredConnectingStateOK )
				return false;

			// Messages endpoint will finish setting up
			if ( !pListenSocket->m_pMessagesEndPointOwner->BHandleNewIncomingConnection( this, scopeLock ) )
			{
				V_sprintf_safe( errMsg, "BHandleNewIncomingConnection returned false" );
				return false;
			}
			return true;
		}
	#endif

	// Transition to connecting state
	return BConnectionState_Connecting( usecNow, errMsg );
}

EResult CSteamNetworkConnectionPipe::AcceptConnection( SteamNetworkingMicroseconds usecNow )
{
	// We gotta have the global lock and our own lock
	CSteamNetworkConnectionBase::AssertLocksHeldByCurrentThread();

	if ( !m_pPartner )
	{
		Assert( false );
		return k_EResultFail;
	}

	// Grab our partner's lock as well
	ConnectionScopeLock scopePartnerLock( *m_pPartner );

	// Mark server side connection as connected
	ConnectionState_Connected( usecNow );

	// "Send connect OK" to partner, and he is connected
	FakeSendStatsAndRecv( usecNow, 0 );

	// At this point, client would ordinarily learn the real identity.
	// if he connected by FakeIP, setup FakeIP reference just like
	// we ordinarily would
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		if ( m_pPartner->m_identityRemote.IsFakeIP() )
		{
			Assert( !m_pPartner->m_fakeIPRefRemote.IsValid() );
			m_pPartner->m_fakeIPRefRemote.Setup( m_pPartner->m_identityRemote.m_ip, m_identityLocal );
		}
	#endif

	// Partner "receives" ConnectOK
	m_pPartner->m_identityRemote = m_identityLocal; // And now they know our real identity
	m_pPartner->m_unConnectionIDRemote = m_unConnectionIDLocal;
	if ( !m_pPartner->BRecvCryptoHandshake( m_msgSignedCertLocal, m_msgSignedCryptLocal, false ) )
		return k_EResultHandshakeFailed;
	m_pPartner->ConnectionState_Connected( usecNow );

	return k_EResultOK;
}

void CSteamNetworkConnectionPipe::FakeSendStatsAndRecv( SteamNetworkingMicroseconds usecNow, int cbPktSize )
{
	uint16 nWirePktNum = FakeSendStats( usecNow, cbPktSize );
	if ( !m_pPartner )
	{
		Assert( false);
		return;
	}
	m_pPartner->FakeRecvStats( usecNow, cbPktSize, nWirePktNum );
}

uint16 CSteamNetworkConnectionPipe::FakeSendStats( SteamNetworkingMicroseconds usecNow, int cbPktSize )
{
	m_pLock->AssertHeldByCurrentThread();

	// Get the next packet number we would have sent
	uint16 nWirePktNum = m_statsEndToEnd.ConsumeSendPacketNumberAndGetWireFmt( usecNow );

	// Fake sending stats
	m_statsEndToEnd.TrackSentPacket( cbPktSize );

	return nWirePktNum;
}

void CSteamNetworkConnectionPipe::FakeRecvStats( SteamNetworkingMicroseconds usecNow, int cbPktSize, uint16 nWirePktNum )
{
	m_pLock->AssertHeldByCurrentThread();

	// And the peer receiving it immediately.  And assume every packet represents
	// a ping measurement.
	int idxMultiPath = 0;
	int64 nPktNum = m_statsEndToEnd.ExpandWirePacketNumberAndCheck( nWirePktNum, idxMultiPath );
	m_statsEndToEnd.TrackProcessSequencedPacket( nPktNum, usecNow, -1, idxMultiPath );
	m_statsEndToEnd.TrackRecvPacket( cbPktSize, usecNow );
	m_statsEndToEnd.m_ping.ReceivedPing( 0, usecNow );
}

void CSteamNetworkConnectionPipe::SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason )
{
	NOTE_UNUSED( eRequest );
	NOTE_UNUSED( pszReason );

	// We gotta have the global lock and our own lock
	CSteamNetworkConnectionBase::AssertLocksHeldByCurrentThread();

	if ( !m_pPartner )
	{
		Assert( false );
		return;
	}

	// Grab our partner's lock as well
	ConnectionScopeLock scopePartnerLock( *m_pPartner );

	// Fake sending us a ping request
	m_statsEndToEnd.TrackSentPingRequest( usecNow, false );
	FakeSendStatsAndRecv( usecNow, 0 );

	// Fake partner receiving it
	m_pPartner->m_statsEndToEnd.PeerAckedLifetime( usecNow );
	m_pPartner->m_statsEndToEnd.PeerAckedInstantaneous( usecNow );

	// ...and sending us a reply immediately
	m_pPartner->FakeSendStatsAndRecv( usecNow, 0 );

	// ... and us receiving it immediately
	m_statsEndToEnd.PeerAckedLifetime( usecNow );
	m_statsEndToEnd.PeerAckedInstantaneous( usecNow );
}

bool CSteamNetworkConnectionPipe::BCanSendEndToEndConnectRequest() const
{
	// We should only ask this question if we still have a chance of connecting.
	// Once we detach from partner, we should switch the connection state, and
	// the base class state machine should never ask this question of us
	Assert( m_pPartner );
	return m_pPartner != nullptr;
}

bool CSteamNetworkConnectionPipe::BCanSendEndToEndData() const
{
	// We should only ask this question if we still have a chance of connecting.
	// Once we detach from partner, we should switch the connection state, and
	// the base class state machine should never ask this question of us
	Assert( m_pPartner );
	return m_pPartner != nullptr;
}

void CSteamNetworkConnectionPipe::SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow )
{
	// We gotta have the global lock and our own lock
	CSteamNetworkConnectionBase::AssertLocksHeldByCurrentThread();
	if ( !m_pPartner )
		return;

	// Grab our partner's lock as well
	ConnectionScopeLock scopePartnerLock( *m_pPartner );

	// Send a "packet"
	FakeSendStatsAndRecv( usecNow, 0 );
}

bool CSteamNetworkConnectionPipe::SendDataPacket( SteamNetworkingMicroseconds usecNow )
{
	AssertMsg( false, "CSteamNetworkConnectionPipe connections shouldn't try to send 'packets'!" );
	return false;
}

int CSteamNetworkConnectionPipe::SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctx )
{
	AssertMsg( false, "CSteamNetworkConnectionPipe connections shouldn't try to send 'packets'!" );
	return -1;
}

void CSteamNetworkConnectionPipe::TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const
{
	CConnectionTransport::TransportPopulateConnectionInfo( info );
	info.m_nFlags |= k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers | k_nSteamNetworkConnectionInfoFlags_Fast;

	// Since we're using loopback buffers, the security flags can't really apply.
	// Make sure they are turned off
	info.m_nFlags &= ~k_nSteamNetworkConnectionInfoFlags_Unauthenticated;
	info.m_nFlags &= ~k_nSteamNetworkConnectionInfoFlags_Unencrypted;
}

void CSteamNetworkConnectionPipe::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	CSteamNetworkConnectionBase::ConnectionStateChanged( eOldState );

	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_FindingRoute:
		default:
			AssertMsg1( false, "Invalid state %d", GetState() );
			// FALLTHROUGH
			// |
			// |
			// V
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: // We can "timeout" and fail to connect, if the app doesn't accept the connection in time.
			if ( m_pPartner )
			{
				// Grab our partner's lock as well
				ConnectionScopeLock scopePartnerLock( *m_pPartner );

				CSteamNetworkConnectionPipe *pPartner = m_pPartner;
				m_pPartner = nullptr; // clear pointer now, to prevent recursion
				pPartner->ConnectionState_ClosedByPeer( m_eEndReason, m_szEndDebug );
			}
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_Connected:
			Assert( m_pPartner );
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:

			// If we have a partner, they should be the ones initiating this.
			// (In the code directly above.)
			if ( m_pPartner )
			{
				Assert( m_pPartner->m_pPartner == nullptr );
				m_pPartner = nullptr;
			}
			break;
	}
}

bool CSteamNetworkConnectionPipe::BSupportsSymmetricMode()
{
	return true;
}

void CSteamNetworkConnectionPipe::DestroyTransport()
{
	// Using the same object for connection and transport
	TransportFreeResources();
	m_pTransport = nullptr;
}

} // namespace SteamNetworkingSocketsLib
