//====== Copyright Valve Corporation, All rights reserved. ====================

#include <time.h>

#include <steam/isteamnetworkingsockets.h>
#include "steamnetworkingsockets_connections.h"
#include "steamnetworkingsockets_lowlevel.h"
#include "../steamnetworkingsockets_certstore.h"
#include "csteamnetworkingsockets.h"
#include "crypto.h"

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
	pMsg->m_nChannel = -1;
	pMsg->m_nFlags = 0;
	pMsg->m_links.Clear();
	pMsg->m_linksSecondaryQueue.Clear();

	return pMsg;
}

CSteamNetworkingMessage *CSteamNetworkingMessage::New( CSteamNetworkConnectionBase *pParent, uint32 cbSize, int64 nMsgNum, int nFlags, SteamNetworkingMicroseconds usecNow )
{
	CSteamNetworkingMessage *pMsg = New( cbSize );
	if ( !pMsg )
	{
		// Failed!  if it's for a reliable message, then we must abort the connection.
		// If unreliable message....well we've spewed, but let's try to keep on chugging.
		if ( pParent && ( nFlags & k_nSteamNetworkingSend_Reliable ) )
			pParent->ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Failed to allocate buffer to receive reliable message" );
		return nullptr;
	}

	if ( pParent )
	{
		pMsg->m_identityPeer = pParent->m_identityRemote;
		pMsg->m_conn = pParent->m_hConnectionSelf;
		pMsg->m_nConnUserData = pParent->GetUserData();
	}
	pMsg->m_usecTimeReceived = usecNow;
	pMsg->m_nMessageNumber = nMsgNum;
	pMsg->m_nFlags = nFlags;

	return pMsg;
}

void CSteamNetworkingMessage::LinkBefore( CSteamNetworkingMessage *pSuccessor, Links CSteamNetworkingMessage::*pMbrLinks, SteamNetworkingMessageQueue *pQueue )
{
	// Make sure we're not already in a queue
	UnlinkFromQueue( pMbrLinks );

	// No successor?
	if ( !pSuccessor )
	{
		LinkToQueueTail( pMbrLinks, pQueue );
		return;
	}

	// Otherwise, the queue cannot be empty, since it at least contains the successor
	Assert( pQueue->m_pFirst );
	Assert( pQueue->m_pLast );
	Assert( (pSuccessor->*pMbrLinks).m_pQueue == pQueue );

	CSteamNetworkingMessage *pPrev = (pSuccessor->*pMbrLinks).m_pPrev;
	if ( pPrev )
	{
		Assert( pQueue->m_pFirst != pSuccessor );
		Assert( (pPrev->*pMbrLinks).m_pNext == pSuccessor );
		Assert( (pPrev->*pMbrLinks).m_pQueue == pQueue );

		(pPrev->*pMbrLinks).m_pNext = this;
		(this->*pMbrLinks).m_pPrev = pPrev;
	}
	else
	{
		Assert( pQueue->m_pFirst == pSuccessor );
		pQueue->m_pFirst = this;
		(this->*pMbrLinks).m_pPrev = nullptr; // Should already be null, but let's slam it again anyway
	}

	// Finish up
	(this->*pMbrLinks).m_pQueue = pQueue;
	(this->*pMbrLinks).m_pNext = pSuccessor;
	(pSuccessor->*pMbrLinks).m_pPrev = this;
}

void CSteamNetworkingMessage::LinkToQueueTail( Links CSteamNetworkingMessage::*pMbrLinks, SteamNetworkingMessageQueue *pQueue )
{
	// Locate previous link that should point to us.
	// Does the queue have anything in it?
	if ( pQueue->m_pLast )
	{
		Assert( pQueue->m_pFirst );
		Assert( !(pQueue->m_pLast->*pMbrLinks).m_pNext );
		(pQueue->m_pLast->*pMbrLinks).m_pNext = this;
	}
	else
	{
		Assert( !pQueue->m_pFirst );
		pQueue->m_pFirst = this;
	}

	// Link back to the previous guy, if any
	(this->*pMbrLinks).m_pPrev = pQueue->m_pLast;

	// We're last in the list, nobody after us
	(this->*pMbrLinks).m_pNext = nullptr;
	pQueue->m_pLast = this;

	// Remember what queue we're in
	(this->*pMbrLinks).m_pQueue = pQueue;
}

void CSteamNetworkingMessage::UnlinkFromQueue( Links CSteamNetworkingMessage::*pMbrLinks )
{
	Links &links = this->*pMbrLinks;
	if ( links.m_pQueue == nullptr )
		return;
	SteamNetworkingMessageQueue &q = *links.m_pQueue;

	// Unlink from previous
	if ( links.m_pPrev )
	{
		Assert( q.m_pFirst != this );
		Assert( (links.m_pPrev->*pMbrLinks).m_pNext == this );
		(links.m_pPrev->*pMbrLinks).m_pNext = links.m_pNext;
	}
	else
	{
		Assert( q.m_pFirst == this );
		q.m_pFirst = links.m_pNext;
	}

	// Unlink from next
	if ( links.m_pNext )
	{
		Assert( q.m_pLast != this );
		Assert( (links.m_pNext->*pMbrLinks).m_pPrev == this );
		(links.m_pNext->*pMbrLinks).m_pPrev = links.m_pPrev;
	}
	else
	{
		Assert( q.m_pLast == this );
		q.m_pLast = links.m_pPrev;
	}

	// Clear links
	links.m_pQueue = nullptr;
	links.m_pPrev = nullptr;
	links.m_pNext = nullptr;
}

void CSteamNetworkingMessage::Unlink()
{
	// Unlink from any queues we are in
	UnlinkFromQueue( &CSteamNetworkingMessage::m_links );
	UnlinkFromQueue( &CSteamNetworkingMessage::m_linksSecondaryQueue );
}

void SteamNetworkingMessageQueue::PurgeMessages()
{

	while ( !empty() )
	{
		CSteamNetworkingMessage *pMsg = m_pFirst;
		pMsg->Unlink();
		Assert( m_pFirst != pMsg );
		pMsg->Release();
	}
}

int SteamNetworkingMessageQueue::RemoveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	int nMessagesReturned = 0;

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
}

CSteamNetworkPollGroup::~CSteamNetworkPollGroup()
{
	FOR_EACH_VEC_BACK( m_vecConnections, i )
	{
		CSteamNetworkConnectionBase *pConn = m_vecConnections[i];
		Assert( pConn->m_pPollGroup == this );
		pConn->RemoveFromPollGroup();
		Assert( m_vecConnections.Count() == i );
	}

	// We should not have any messages now!  but if we do, unlink them
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

	// Remove us from global table, if we're in it
	if ( m_hPollGroupSelf != k_HSteamNetPollGroup_Invalid )
	{
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
}

void CSteamNetworkPollGroup::AssignHandleAndAddToGlobalTable()
{
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
: m_pSteamNetworkingSocketsInterface( pSteamNetworkingSocketsInterface )
, m_hListenSocketSelf( k_HSteamListenSocket_Invalid )
#ifdef STEAMNETWORKINGSOCKETS_STEAMCLIENT
, m_legacyPollGroup( pSteamNetworkingSocketsInterface )
#endif
{
	m_connectionConfig.Init( &pSteamNetworkingSocketsInterface->m_connectionConfig );
}

CSteamNetworkListenSocketBase::~CSteamNetworkListenSocketBase()
{
	AssertMsg( m_mapChildConnections.Count() == 0, "Destroy() not used properly" );

	// Remove us from global table, if we're in it
	if ( m_hListenSocketSelf != k_HSteamListenSocket_Invalid )
	{
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
}

bool CSteamNetworkListenSocketBase::BInitListenSocketCommon( int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
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
	m_connectionConfig.m_SymmetricConnect.Lock();
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

	// Destroy all child connections
	FOR_EACH_HASHMAP( m_mapChildConnections, h )
	{
		CSteamNetworkConnectionBase *pChild = m_mapChildConnections[ h ];
		Assert( pChild->m_pParentListenSocket == this );
		Assert( pChild->m_hSelfInParentListenSocketMap == h );

		int n = m_mapChildConnections.Count();
		pChild->ConnectionDestroySelfNow();
		Assert( m_mapChildConnections.Count() == n-1 );
	}

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
		pConn->SetPollGroup( &m_legacyPollGroup );
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

CSteamNetworkConnectionBase::CSteamNetworkConnectionBase( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: m_pSteamNetworkingSocketsInterface( pSteamNetworkingSocketsInterface )
{
	m_hConnectionSelf = k_HSteamNetConnection_Invalid;
	m_nUserData = -1;
	m_eConnectionState = k_ESteamNetworkingConnectionState_None;
	m_eConnectionWireState = k_ESteamNetworkingConnectionState_None;
	m_usecWhenEnteredConnectionState = 0;
	m_usecWhenSentConnectRequest = 0;
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
	memset( m_szAppName, 0, sizeof( m_szAppName ) );
	memset( m_szDescription, 0, sizeof( m_szDescription ) );
	m_bConnectionInitiatedRemotely = false;
	m_pTransport = nullptr;
	m_nSupressStateChangeCallbacks = 0;
 
	// Initialize configuration using parent interface for now.
	m_connectionConfig.Init( &m_pSteamNetworkingSocketsInterface->m_connectionConfig );
}

CSteamNetworkConnectionBase::~CSteamNetworkConnectionBase()
{
	Assert( m_hConnectionSelf == k_HSteamNetConnection_Invalid );
	Assert( m_eConnectionState == k_ESteamNetworkingConnectionState_Dead );
	Assert( m_eConnectionWireState == k_ESteamNetworkingConnectionState_Dead );
	Assert( m_queueRecvMessages.empty() );
	Assert( m_pParentListenSocket == nullptr );
}

void CSteamNetworkConnectionBase::ConnectionDestroySelfNow()
{

	// Make sure all resources have been freed, etc
	FreeResources();

	// Self destruct NOW
	delete this;
}

void CConnectionTransport::TransportDestroySelfNow()
{
	// Call virtual functions while we still can
	TransportFreeResources();

	// Self destruct NOW
	delete this;
}

void CConnectionTransport::TransportFreeResources()
{
}

void CSteamNetworkConnectionBase::QueueDestroy()
{
	FreeResources();

	// We'll delete ourselves from within Think();
	SetNextThinkTime( SteamNetworkingSockets_GetLocalTimestamp() );
}

void CSteamNetworkConnectionBase::FreeResources()
{
	// Make sure we're marked in the dead state, and also if we were in an
	// API-visible state, this will queue the state change notification
	// while we still know who our listen socket is (if any).
	SetState( k_ESteamNetworkingConnectionState_Dead, SteamNetworkingSockets_GetLocalTimestamp() );

	// Discard any messages that weren't retrieved
	m_queueRecvMessages.PurgeMessages();

	// If we are in a poll group, remove us from the group
	RemoveFromPollGroup();

	// Detach from the listen socket that owns us, if any
	if ( m_pParentListenSocket )
		m_pParentListenSocket->AboutToDestroyChildConnection( this );

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

	// Make sure and clean out crypto keys and such now
	ClearCrypto();

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

	// Clean up our transport
	DestroyTransport();
}

void CSteamNetworkConnectionBase::DestroyTransport()
{
	if ( m_pTransport )
	{
		m_pTransport->TransportDestroySelfNow();
		m_pTransport = nullptr;
	}
}

void CSteamNetworkConnectionBase::RemoveFromPollGroup()
{
	if ( !m_pPollGroup )
		return;

	// Scan all of our messages, and make sure they are not in the secondary queue
	for ( CSteamNetworkingMessage *pMsg = m_queueRecvMessages.m_pFirst ; pMsg ; pMsg = pMsg->m_linksSecondaryQueue.m_pNext )
	{
		Assert( pMsg->m_links.m_pQueue == &m_queueRecvMessages );

		// It *should* be in the secondary queue of the poll group
		Assert( pMsg->m_linksSecondaryQueue.m_pQueue == &m_pPollGroup->m_queueRecvMessages );

		// OK, do the work
		pMsg->UnlinkFromQueue( &CSteamNetworkingMessage::m_linksSecondaryQueue );
	}

	// Remove us from the poll group's list.  DbgVerify because we should be in the list!
	DbgVerify( m_pPollGroup->m_vecConnections.FindAndFastRemove( this ) );

	// We're not in a poll group anymore
	m_pPollGroup = nullptr;
}

void CSteamNetworkConnectionBase::SetPollGroup( CSteamNetworkPollGroup *pPollGroup )
{

	// Quick early-out for no change
	if ( m_pPollGroup == pPollGroup )
		return;

	// Clearing it?
	if ( !pPollGroup )
	{
		RemoveFromPollGroup();
		return;
	}

	// Scan all messages that are already queued for this connection,
	// and insert them into the poll groups queue in the (approximate)
	// appropriate spot.  Using local timestamps should be really close
	// for ordering messages between different connections.  Remember
	// that the API very clearly does not provide strong guarantees
	// regarding ordering of messages from different connections, and
	// really anybody who is expecting or relying on such guarantees
	// is probably doing something wrong.
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
	// Should only be called while we are in the initial state
	Assert( GetState() == k_ESteamNetworkingConnectionState_None );

	// Make sure MTU values are initialized
	UpdateMTUFromConfig();

	// We make sure the lower 16 bits are unique.  Make sure we don't have too many connections.
	// This definitely could be relaxed, but honestly we don't expect this library to be used in situations
	// where you need that many connections.
	if ( g_mapConnections.Count() >= 0x1fff )
	{
		V_strcpy_safe( errMsg, "Too many connections." );
		return false;
	}

	// Select random connection ID, and make sure it passes certain sanity checks
	Assert( m_unConnectionIDLocal == 0 );
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
	m_statsEndToEnd.Init( usecNow, true ); // Until we go connected don't try to send acks, etc

	// Let's use the the connection ID as the connection handle.  It's random, not reused
	// within a short time interval, and we print it in our debugging in places, and you
	// can see it on the wire for debugging.  In the past we has a "clever" method of
	// assigning the handle that had some cute performance tricks for lookups and
	// guaranteeing handles wouldn't be reused.  But making it be the same as the
	// ConnectionID is probably just more useful and less confusing.
	m_hConnectionSelf = m_unConnectionIDLocal;

	// Add it to our table of active sockets.
	g_mapConnections.Insert( int16( m_hConnectionSelf ), this );

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
	m_msgCertRemote.Clear();
	m_msgCryptRemote.Clear();
	m_bCertHasIdentity = false;
	m_keyPrivate.Wipe();
	ClearLocalCrypto();
}

void CSteamNetworkConnectionBase::ClearLocalCrypto()
{
	m_eNegotiatedCipher = k_ESteamNetworkingSocketsCipher_INVALID;
	m_keyExchangePrivateKeyLocal.Wipe();
	m_msgCryptLocal.Clear();
	m_msgSignedCryptLocal.Clear();
	m_bCryptKeysValid = false;
	m_cryptContextSend.Wipe();
	m_cryptContextRecv.Wipe();
	m_cryptIVSend.Wipe();
	m_cryptIVRecv.Wipe();
}

void CSteamNetworkConnectionBase::RecvNonDataSequencedPacket( int64 nPktNum, SteamNetworkingMicroseconds usecNow )
{
	// Note: order of operations is important betwen these two calls

	// Let SNP know when we received it, so we can track loss events and send acks.  We do
	// not schedule acks to be sent at this time, but when they are sent, we will implicitly
	// ack this one
	SNP_RecordReceivedPktNum( nPktNum, usecNow, false );

	// Update general sequence number/stats tracker for the end-to-end flow.
	m_statsEndToEnd.TrackProcessSequencedPacket( nPktNum, usecNow, 0 );
}

bool CSteamNetworkConnectionBase::BThinkCryptoReady( SteamNetworkingMicroseconds usecNow )
{
	// Should only be called from initial states
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
	Assert( msgSignedCert.has_cert() );
	Assert( keyPrivate.IsValid() );

	// Ug, we have to save off the private key.  I hate to have copies of the private key,
	// but we'll only keep this around for a brief time.  It's possible for the
	// interface to get a new cert (with a new private key) while we are starting this
	// connection.  We'll keep using the old one, which may be totally valid.
	DbgVerify( m_keyPrivate.CopyFrom( keyPrivate ) );

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
	Assert( m_msgCryptLocal.ciphers_size() == 0 ); // Should only do this once

	// Select the ciphers we want to use, in preference order.
	// Also, lock it, we cannot change it any more
	m_connectionConfig.m_Unencrypted.Lock();
	int unencrypted = m_connectionConfig.m_Unencrypted.Get();
	switch ( unencrypted )
	{
		default:
			AssertMsg( false, "Unexpected value for 'Unencrypted' config value" );
			// FALLTHROUGH
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
}

void CSteamNetworkConnectionBase::SetLocalCertUnsigned()
{

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

bool CSteamNetworkConnectionBase::BRecvCryptoHandshake( const CMsgSteamDatagramCertificateSigned &msgCert, const CMsgSteamDatagramSessionCryptInfoSigned &msgSessionInfo, bool bServer )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread( "BRecvCryptoHandshake" );
	SteamNetworkingErrMsg errMsg;

	// Have we already done key exchange?
	if ( m_bCryptKeysValid )
	{
		// FIXME - Probably should check that they aren't changing any keys.
		Assert( m_eNegotiatedCipher != k_ESteamNetworkingSocketsCipher_INVALID );
		return true;
	}
	Assert( m_eNegotiatedCipher == k_ESteamNetworkingSocketsCipher_INVALID );

	// Make sure we have what we need
	if ( !msgCert.has_cert() || !msgSessionInfo.has_info() )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Crypto handshake missing cert or session data" );
		return false;
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
		pCACertAuthScope = CertStore_CheckCert( msgCert, m_msgCertRemote, timeNow, errMsg );
		if ( !pCACertAuthScope )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Bad cert: %s", errMsg );
			return false;
		}
	}
	else
	{

		// Deserialize the cert
		if ( !m_msgCertRemote.ParseFromString( m_sCertRemote ) )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Cert failed protobuf decode" );
			return false;
		}

		// We'll check if unsigned certs are allowed below, after we know a bit more info
	}

	// Check identity from cert
	SteamNetworkingIdentity identityCert;
	int rIdentity = SteamNetworkingIdentityFromCert( identityCert, m_msgCertRemote, errMsg );
	if ( rIdentity < 0 )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Bad cert identity.  %s", errMsg );
		return false;
	}
	if ( rIdentity > 0 && !identityCert.IsLocalHost() )
	{

		// They sent an identity.  Then it must match the identity we expect!
		if ( !( identityCert == m_identityRemote ) )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert was issued to %s, not %s",
				SteamNetworkingIdentityRender( identityCert ).c_str(), SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
			return false;
		}

		// We require certs to be bound to a particular AppID.
		if ( m_msgCertRemote.app_ids_size() == 0 )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert must be bound to an AppID." );
			return false;
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
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Certs with no identity can only by anonymous gameservers, not %s", SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
			return false;
		}

		// And cert must be scoped to a data center, we don't permit blanked certs for anybody with no restrictions at all
		if ( m_msgCertRemote.gameserver_datacenter_ids_size() == 0 )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert with no identity must be scoped to PoPID." );
			return false;
		}
	}

	// OK, we've parsed everything out, now do any connection-type-specific checks on the cert
	ESteamNetConnectionEnd eRemoteCertFailure = CheckRemoteCert( pCACertAuthScope, errMsg );
	if ( eRemoteCertFailure )
	{
		ConnectionState_ProblemDetectedLocally( eRemoteCertFailure, "%s", errMsg );
		return false;
	}

	// Check the signature of the crypt info
	if ( !BCheckSignature( m_sCryptRemote, m_msgCertRemote.key_type(), m_msgCertRemote.key_data(), msgSessionInfo.signature(), errMsg ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "%s", errMsg );
		return false;
	}

	// Deserialize crypt info
	if ( !m_msgCryptRemote.ParseFromString( m_sCryptRemote ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Crypt info failed protobuf decode" );
		return false;
	}

	// Protocol version
	if ( m_msgCryptRemote.protocol_version() < k_nMinRequiredProtocolVersion )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadProtocolVersion, "Peer is running old software and needs to be updated.  (V%u, >=V%u is required)",
			m_msgCryptRemote.protocol_version(), k_nMinRequiredProtocolVersion );
		return false;
	}

	// Did they already send a protocol version in an earlier message?  If so, it needs to match.
	if ( m_statsEndToEnd.m_nPeerProtocolVersion != 0 && m_statsEndToEnd.m_nPeerProtocolVersion != m_msgCryptRemote.protocol_version() )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadProtocolVersion, "Claiming protocol V%u now, but earlier was using V%u",m_msgCryptRemote.protocol_version(), m_statsEndToEnd.m_nPeerProtocolVersion );
		return false;
	}
	m_statsEndToEnd.m_nPeerProtocolVersion = m_msgCryptRemote.protocol_version();

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
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "We don't have cert, and self-signed certs not allowed" );
			return false;
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
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Server must select exactly only one cipher!" );
			return false;
		}
		if ( !BFinishCryptoHandshake( bServer ) )
		{
			Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
			return false;
		}
	}

	return true;
}

bool CSteamNetworkConnectionBase::BFinishCryptoHandshake( bool bServer )
{

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
	if ( m_eNegotiatedCipher == k_ESteamNetworkingSocketsCipher_INVALID )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Failed to negotiate mutually-agreeable cipher" );
		return false;
	}

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
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Unsupported DH key type" );
		return false;
	}
	if ( !keyExchangePublicKeyRemote.SetRawDataWithoutWipingInput( m_msgCryptRemote.key_data().c_str(), m_msgCryptRemote.key_data().length() ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Invalid DH key" );
		return false;
	}

	// Diffie–Hellman key exchange to get "premaster secret"
	AutoWipeFixedSizeBuffer<sizeof(SHA256Digest_t)> premasterSecret;
	if ( !CCrypto::PerformKeyExchange( m_keyExchangePrivateKeyLocal, keyExchangePublicKeyRemote, &premasterSecret.m_buf ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Key exchange failed" );
		return false;
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
	if (
		!m_cryptContextSend.Init( cryptKeySend.m_buf, cryptKeySend.k_nSize, m_cryptIVSend.k_nSize, k_cbSteamNetwokingSocketsEncrytionTagSize )
		|| !m_cryptContextRecv.Init( cryptKeyRecv.m_buf, cryptKeyRecv.k_nSize, m_cryptIVRecv.k_nSize, k_cbSteamNetwokingSocketsEncrytionTagSize ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Error initializing crypto" );
		return false;
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
	return true;
}

EUnsignedCert CSteamNetworkConnectionBase::AllowLocalUnsignedCert()
{
	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		// We don't have a cert authority.  We probably ought to make this customizable
		return k_EUnsignedCert_Allow;
	#else
		return k_EUnsignedCert_Disallow;
	#endif
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
	m_nUserData = nUserData;

	// Change user data on all messages that haven't been pulled out
	// of the queue yet.  This way we don't expose the client to weird
	// race conditions where they create a connection, and before they
	// are able to install their user data, some messages come in
	for ( CSteamNetworkingMessage *m = m_queueRecvMessages.m_pFirst ; m ; m = m->m_links.m_pNext )
	{
		Assert( m->m_conn == m_hConnectionSelf );
		m->m_nConnUserData = m_nUserData;
	}
}

void CConnectionTransport::TransportConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
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

void CSteamNetworkConnectionBase::ConnectionPopulateInfo( SteamNetConnectionInfo_t &info ) const
{
	memset( &info, 0, sizeof(info) );

	info.m_eState = CollapseConnectionStateToAPIState( m_eConnectionState );
	info.m_hListenSocket = m_pParentListenSocket ? m_pParentListenSocket->m_hListenSocketSelf : k_HSteamListenSocket_Invalid;
	info.m_identityRemote = m_identityRemote;
	info.m_nUserData = m_nUserData;
	info.m_eEndReason = m_eEndReason;
	V_strcpy_safe( info.m_szEndDebug, m_szEndDebug );
	V_strcpy_safe( info.m_szConnectionDescription, m_szDescription );

	if ( m_pTransport )
		m_pTransport->TransportPopulateConnectionInfo( info );
}

void CSteamNetworkConnectionBase::APIGetQuickConnectionStatus( SteamNetworkingQuickConnectionStatus &stats )
{
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	stats.m_eState = CollapseConnectionStateToAPIState( m_eConnectionState );
	stats.m_nPing = m_statsEndToEnd.m_ping.m_nSmoothedPing;
	if ( m_statsEndToEnd.m_flInPacketsDroppedPct >= 0.0f )
	{
		Assert( m_statsEndToEnd.m_flInPacketsWeirdSequencePct >= 0.0f );
		stats.m_flConnectionQualityLocal = 1.0f - m_statsEndToEnd.m_flInPacketsDroppedPct - m_statsEndToEnd.m_flInPacketsWeirdSequencePct;
		Assert( stats.m_flConnectionQualityLocal >= 0.0f );
	}
	else
	{
		stats.m_flConnectionQualityLocal = -1.0f;
	}

	// FIXME - Can SNP give us a more up-to-date value from the feedback packet?
	if ( m_statsEndToEnd.m_latestRemote.m_flPacketsDroppedPct >= 0.0f )
	{
		Assert( m_statsEndToEnd.m_latestRemote.m_flPacketsWeirdSequenceNumberPct >= 0.0f );
		stats.m_flConnectionQualityRemote = 1.0f - m_statsEndToEnd.m_latestRemote.m_flPacketsDroppedPct - m_statsEndToEnd.m_latestRemote.m_flPacketsWeirdSequenceNumberPct;
		Assert( stats.m_flConnectionQualityRemote >= 0.0f );
	}
	else
	{
		stats.m_flConnectionQualityRemote = -1.0f;
	}

	// Actual current data rates
	stats.m_flOutPacketsPerSec = m_statsEndToEnd.m_sent.m_packets.m_flRate;
	stats.m_flOutBytesPerSec = m_statsEndToEnd.m_sent.m_bytes.m_flRate;
	stats.m_flInPacketsPerSec = m_statsEndToEnd.m_recv.m_packets.m_flRate;
	stats.m_flInBytesPerSec = m_statsEndToEnd.m_recv.m_bytes.m_flRate;
	SNP_PopulateQuickStats( stats, usecNow );
}

void CSteamNetworkConnectionBase::APIGetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow )
{
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
	return m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
}

bool CSteamNetworkConnectionBase::DecryptDataChunk( uint16 nWireSeqNum, int cbPacketSize, const void *pChunk, int cbChunk, RecvPacketContext_t &ctx )
{
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
	ctx.m_nPktNum = m_statsEndToEnd.ExpandWirePacketNumberAndCheck( nWireSeqNum );
	if ( ctx.m_nPktNum <= 0 )
	{

		// Update raw packet counters numbers, but do not update any logical state suc as reply timeouts, etc
		m_statsEndToEnd.m_recv.ProcessPacket( cbPacketSize );
		return false;
	}

	// What cipher are we using?
	switch ( m_eNegotiatedCipher )
	{
		default:
			AssertMsg1( false, "Bogus cipher %d", m_eNegotiatedCipher );
			return false;

		case k_ESteamNetworkingSocketsCipher_NULL:
		{

			// No encryption!
			ctx.m_cbPlainText = cbChunk;
			ctx.m_pPlainText = pChunk;
		}
		break;

		case k_ESteamNetworkingSocketsCipher_AES_256_GCM:
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
			bool bDecryptOK = m_cryptContextRecv.Decrypt(
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
				SpewWarningRateLimited( ctx.m_usecNow, "[%s] Packet data chunk failed to decrypt!  Could be tampering/spoofing or a bug.", GetDescription() );

				// Update raw packet counters numbers, but do not update any logical state suc as reply timeouts, etc
				m_statsEndToEnd.m_recv.ProcessPacket( cbPacketSize );
				return false;
			}

			ctx.m_cbPlainText = (int)cbDecrypted;
			ctx.m_pPlainText = ctx.m_decrypted;

			//SpewVerbose( "Connection %u recv seqnum %lld (gap=%d) sz=%d %02x %02x %02x %02x\n", m_unConnectionID, unFullSequenceNumber, nGap, cbDecrypted, arDecryptedChunk[0], arDecryptedChunk[1], arDecryptedChunk[2], arDecryptedChunk[3] );
		}
		break;
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
	if ( !BFinishCryptoHandshake( true ) )
		return k_EResultHandshakeFailed;

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

	// If we already know the reason for the problem, we should ignore theirs
	if ( m_eEndReason == k_ESteamNetConnectionEnd_Invalid || GetState() == k_ESteamNetworkingConnectionState_Connecting || GetState() == k_ESteamNetworkingConnectionState_FindingRoute || GetState() == k_ESteamNetworkingConnectionState_Connected )
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

	// Check our state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
			ConnectionState_FinWait();
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			if ( bEnableLinger )
			{
				SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
				SetState( k_ESteamNetworkingConnectionState_Linger, usecNow );
				CheckConnectionStateAndSetNextThinkTime( usecNow );
			}
			else
			{
				ConnectionState_FinWait();
			}
			break;
	}
}

void CSteamNetworkConnectionBase::SetState( ESteamNetworkingConnectionState eNewState, SteamNetworkingMicroseconds usecNow )
{
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
		m_connectionConfig.m_IP_AllowWithoutAuth.Lock();
		m_connectionConfig.m_Unencrypted.Lock();
		m_connectionConfig.m_SymmetricConnect.Lock();
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
			m_connectionConfig.m_SDRClient_DebugTicketAddress.Lock();
		#endif
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
			m_connectionConfig.m_P2P_Transport_ICE_Enable.Lock();
		#endif
	}

	// Post a notification when certain state changes occur.  Note that
	// "internal" state changes, where the connection is effectively closed
	// from the application's perspective, are not relevant
	const ESteamNetworkingConnectionState eOldAPIState = CollapseConnectionStateToAPIState( eOldState );
	const ESteamNetworkingConnectionState eNewAPIState = CollapseConnectionStateToAPIState( GetState() );

	// Internal connection used by the higher-level messages interface?
	Assert( m_nSupressStateChangeCallbacks >= 0 );
	if ( m_nSupressStateChangeCallbacks == 0 )
	{
		// Check for posting callback, if connection state has changed from an API perspective
		if ( eOldAPIState != eNewAPIState )
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
		m_queueRecvMessages.PurgeMessages();

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

			// Let stats tracking system know that it shouldn't
			// expect to be able to get stuff acked, etc
			m_statsEndToEnd.SetPassive( true, m_usecWhenEnteredConnectionState );

			// Go head and free up memory now
			SNP_ShutdownConnection();
			break;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connected:

			// Key exchange should be complete
			Assert( m_bCryptKeysValid );
			Assert( m_statsEndToEnd.m_usecWhenStartedConnectedState != 0 );

			// Link stats tracker should send and expect, acks, keepalives, etc
			m_statsEndToEnd.SetPassive( false, m_usecWhenEnteredConnectionState );
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:

			// Key exchange should be complete.  (We do that when accepting a connection.)
			Assert( m_bCryptKeysValid );

			// FIXME.  Probably we should NOT set the stats tracker as "active" yet.
			//Assert( m_statsEndToEnd.IsPassive() );
			m_statsEndToEnd.SetPassive( false, m_usecWhenEnteredConnectionState );
			break;

		case k_ESteamNetworkingConnectionState_Connecting:

			// And we shouldn't mark stats object as ready until we go connected
			Assert( m_statsEndToEnd.IsPassive() );
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

bool CSteamNetworkConnectionBase::ReceivedMessage( const void *pData, int cbData, int64 nMsgNum, int nFlags, SteamNetworkingMicroseconds usecNow )
{
//	// !TEST! Enable this during connection test to trap bogus messages earlier
//		struct TestMsg
//		{
//			int64 m_nMsgNum;
//			bool m_bReliable;
//			int m_cbSize;
//			uint8 m_data[ 20*1000 ];
//		};
//		const TestMsg *pTestMsg = (const TestMsg *)pData;
//
//		// Size makes sense?
//		Assert( sizeof(*pTestMsg) - sizeof(pTestMsg->m_data) + pTestMsg->m_cbSize == cbData );

	// Create a message
	CSteamNetworkingMessage *pMsg = CSteamNetworkingMessage::New( this, cbData, nMsgNum, nFlags, usecNow );
	if ( !pMsg )
	{
		// Hm.  this failure really is probably a sign that we are in a pretty bad state,
		// and we are unlikely to recover.  Should we just abort the connection?
		// Right now, we'll try to muddle on.
		return false;
	}

	// Copy the data
	memcpy( pMsg->m_pData, pData, cbData );

	// Receive it
	ReceivedMessage( pMsg );

	return true;
}

void CSteamNetworkConnectionBase::ReceivedMessage( CSteamNetworkingMessage *pMsg )
{

	SpewVerboseGroup( m_connectionConfig.m_LogLevel_Message.Get(), "[%s] RecvMessage MsgNum=%lld sz=%d\n",
		GetDescription(),
		(long long)pMsg->m_nMessageNumber,
		pMsg->m_cbSize );

	// Add to end of my queue.
	pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_links, &m_queueRecvMessages );

	// Add to the poll group, if we are in one
	if ( m_pPollGroup )
		pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_linksSecondaryQueue, &m_pPollGroup->m_queueRecvMessages );
}

void CSteamNetworkConnectionBase::PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState )
{
	SteamNetConnectionStatusChangedCallback_t c;
	ConnectionPopulateInfo( c.m_info );
	c.m_eOldState = eOldAPIState;
	c.m_hConn = m_hConnectionSelf;

	// !KLUDGE! For ISteamnetworkingMessages connections, we want to process the callback immediately.
	void *fnCallback = m_connectionConfig.m_Callback_ConnectionStatusChanged.Get();
	if ( IsConnectionForMessagesSession() )
	{
		if ( fnCallback )
		{
			FnSteamNetConnectionStatusChanged fnConnectionStatusChanged = (FnSteamNetConnectionStatusChanged)( fnCallback );
			(*fnConnectionStatusChanged)( &c );
		}
		else
		{
			// Currently there is no use case that does this.  It's probably a bug.
			Assert( false );
		}
	}
	else
	{

		// Typical codepath - post to a queue
		m_pSteamNetworkingSocketsInterface->QueueCallback( c, fnCallback );
	}
}

void CSteamNetworkConnectionBase::ConnectionState_ProblemDetectedLocally( ESteamNetConnectionEnd eReason, const char *pszFmt, ... )
{
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
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			Assert( false );
			return;

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

			SpewMsg( "[%s] closed by peer\n", GetDescription() );

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

void CSteamNetworkConnectionBase::Think( SteamNetworkingMicroseconds usecNow )
{
	// If we queued ourselves for deletion, now is a safe time to do it.
	// Self destruct!
	if ( m_eConnectionState == k_ESteamNetworkingConnectionState_Dead )
	{
		delete this;
		return;
	}

	// Safety check against leaving callbacks suppressed.  If this fires, there's a good chance
	// we have already suppressed a callback that we should have posted, which is very bad
	AssertMsg( m_nSupressStateChangeCallbacks == 0, "[%s] m_nSupressStateChangeCallbacks left on!", GetDescription() );
	m_nSupressStateChangeCallbacks = 0;

	// CheckConnectionStateAndSetNextThinkTime does all the work of examining the current state
	// and deciding what to do.  But it should be safe to call at any time, whereas Think()
	// has a fixed contract: it should only be called by the thinker framework.
	CheckConnectionStateAndSetNextThinkTime( usecNow );
}

void CSteamNetworkConnectionBase::CheckConnectionStateAndSetNextThinkTime( SteamNetworkingMicroseconds usecNow )
{
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
			// This really shouldn't happen.  But if it does....
			// We can't be sure that it's safe to delete us now.
			// Just queue us for deletion ASAP.
			Assert( false );
			SetNextThinkTime( usecNow );
			return;

		case k_ESteamNetworkingConnectionState_None:
		default:
			// WAT
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_FinWait:
		{
			// Timeout?
			SteamNetworkingMicroseconds usecTimeout = m_usecWhenEnteredConnectionState + k_usecFinWaitTimeout;
			if ( usecNow >= usecTimeout )
			{
				QueueDestroy();
				return;
			}

			// It's not time yet, make sure we get our callback when it's time.
			EnsureMinThinkTime( usecTimeout );
		}
		return;

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		{
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
			SteamNetworkingMicroseconds usecTimeout = m_usecWhenEnteredConnectionState + (SteamNetworkingMicroseconds)m_connectionConfig.m_TimeoutInitial.Get()*1000;
			if ( usecNow >= usecTimeout )
			{
				// Check if the application just didn't ever respond, it's probably a bug.
				// We should squawk about this and let them know.
				if ( m_eConnectionState != k_ESteamNetworkingConnectionState_FindingRoute && m_bConnectionInitiatedRemotely )
				{
					// Discard this warning for messages sessions.  It's part of the messages API design
					// that the app can ignore these connections if they do not want to accept them.
					if ( IsConnectionForMessagesSession() )
					{
						ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Timeout, "%s", "App did not respond to Messages session request in time, discarding." );
					}
					else
					{
						AssertMsg( false, "Application didn't accept or close incoming connection in a reasonable amount of time.  This is probably a bug." );
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
			if ( m_senderState.m_messagesQueued.empty() && m_senderState.m_unackedReliableMessages.empty() )
			{
				// Close the connection ASAP
				ConnectionState_FinWait();
				return;
			}
			// FALLTHROUGH

		// |
		// | otherwise, fall through
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
	UpdateMTUFromConfig();

	// Check for sending keepalives or probing a connection that appears to be timing out
	if ( BStateIsConnectedForWirePurposes() )
	{
		Assert( m_statsEndToEnd.m_usecTimeLastRecv > 0 ); // How did we get connected without receiving anything end-to-end?
		AssertMsg2( !m_statsEndToEnd.IsPassive(), "[%s] stats passive, but in state %d?", GetDescription(), (int)GetState() );

		// Not able to send end-to-end data?
		bool bCanSendEndToEnd = m_pTransport && m_pTransport->BCanSendEndToEndData();

		// Mark us as "timing out" if we are not able to send end-to-end data
		if ( !bCanSendEndToEnd && m_statsEndToEnd.m_usecWhenTimeoutStarted == 0 )
			m_statsEndToEnd.m_usecWhenTimeoutStarted = usecNow;

		// Are we timing out?
		if ( m_statsEndToEnd.m_usecWhenTimeoutStarted > 0 )
		{

			// When will the timeout hit?
			SteamNetworkingMicroseconds usecEndToEndConnectionTimeout = std::max( m_statsEndToEnd.m_usecWhenTimeoutStarted, m_statsEndToEnd.m_usecTimeLastRecv ) + (SteamNetworkingMicroseconds)m_connectionConfig.m_TimeoutConnected.Get()*1000;

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
			}

			// Make sure we are scheduled to wake up the next time we need to take action
			UpdateMinThinkTime( usecStatsNextThinkTime );
		}
	}

	// Hook for derived class to do its connection-type-specific stuff
	ThinkConnection( usecNow );

	// Schedule next time to think, if derived class didn't request an earlier
	// wakeup call.
	EnsureMinThinkTime( usecMinNextThinkTime );

	#undef UpdateMinThinkTime
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
	m_statsEndToEnd.UpdateSpeeds( nTXSpeed, nRXSpeed );
}

void CSteamNetworkConnectionBase::UpdateMTUFromConfig()
{
	int newMTUPacketSize = m_connectionConfig.m_MTU_PacketSize.Get();
	if ( newMTUPacketSize == m_cbMTUPacketSize )
		return;

	// Shrinking MTU?
	if ( newMTUPacketSize < m_cbMTUPacketSize )
	{
		// We cannot do this while we have any reliable segments in flight!
		// To keep things simple, the retries are always the original ranges,
		// we never have our retries chop up the space differently than
		// the original send
		if ( !m_senderState.m_listReadyRetryReliableRange.empty() || !m_senderState.m_listInFlightReliableRange.empty() )
			return;
	}

	m_cbMTUPacketSize = m_connectionConfig.m_MTU_PacketSize.Get();
	m_cbMaxPlaintextPayloadSend = m_cbMTUPacketSize - ( k_cbSteamNetworkingSocketsMaxUDPMsgLen - k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend );
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

	pConn[1] = new CSteamNetworkConnectionPipe( pSteamNetworkingSocketsInterface, pIdentity[0] );
	pConn[0] = new CSteamNetworkConnectionPipe( pSteamNetworkingSocketsInterface, pIdentity[1] );
	if ( !pConn[0] || !pConn[1] )
	{
failed:
		pConn[0]->ConnectionDestroySelfNow(); pConn[0] = nullptr;
		pConn[1]->ConnectionDestroySelfNow(); pConn[1] = nullptr;
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
	pConn[0]->FakeSendStats( usecNow, 0 );
	pConn[1]->FakeSendStats( usecNow, 0 );

	// Tie the connections to each other, and mark them as connected
	for ( int i = 0 ; i < 2 ; ++i )
	{
		CSteamNetworkConnectionPipe *p = pConn[i];
		CSteamNetworkConnectionPipe *q = pConn[1-i];
		p->m_identityRemote = q->m_identityLocal;
		p->m_unConnectionIDRemote = q->m_unConnectionIDLocal;
		if ( !p->BRecvCryptoHandshake( q->m_msgSignedCertLocal, q->m_msgSignedCryptLocal, i==0 ) )
		{
			AssertMsg( false, "BRecvCryptoHandshake failed creating loopback pipe socket pair" );
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
	CSteamNetworkingSockets *pClientInstance, int nOptions, const SteamNetworkingConfigValue_t *pOptions,
	CSteamNetworkListenSocketBase *pListenSocket,
	SteamNetworkingErrMsg &errMsg
) {
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	CSteamNetworkingSockets *pServerInstance = pListenSocket->m_pSteamNetworkingSocketsInterface;

	CSteamNetworkConnectionPipe *pClient = new CSteamNetworkConnectionPipe( pClientInstance, pClientInstance->InternalGetIdentity() );
	CSteamNetworkConnectionPipe *pServer = new CSteamNetworkConnectionPipe( pServerInstance, pServerInstance->InternalGetIdentity() );
	if ( !pClient || !pServer )
	{
		V_strcpy_safe( errMsg, "new CSteamNetworkConnectionPipe failed" );
failed:
		if ( pClient )
			pClient->ConnectionDestroySelfNow();
		if ( pServer )
			pServer->ConnectionDestroySelfNow();
		return nullptr;
	}

	// Link em up
	pClient->m_pPartner = pServer;
	pServer->m_pPartner = pClient;

	// Initialize client connection.  This triggers a state transition callback
	// to the "connecting" state
	if ( !pClient->BInitConnection( usecNow, nOptions, pOptions, errMsg ) )
		goto failed;

	// Server receives the connection and starts "accepting" it
	if ( !pServer->BBeginAccept( pListenSocket, usecNow, errMsg ) )
		goto failed;

	// Client sends a "connect" packet
	if ( !pClient->BConnectionState_Connecting( usecNow, errMsg ) )
		goto failed;
	Assert( pServer->m_statsEndToEnd.m_nMaxRecvPktNum == 1 );
	pClient->m_statsEndToEnd.m_nNextSendSequenceNumber = pServer->m_statsEndToEnd.m_nMaxRecvPktNum+1;
	pClient->FakeSendStats( usecNow, 0 );

	// Now we wait for the app to accept the connection
	return pClient;
}

CSteamNetworkConnectionPipe::CSteamNetworkConnectionPipe( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, const SteamNetworkingIdentity &identity )
: CSteamNetworkConnectionBase( pSteamNetworkingSocketsInterface )
, CConnectionTransport( *static_cast<CSteamNetworkConnectionBase*>( this ) ) // connection and transport object are the same
, m_pPartner( nullptr )
{
	m_identityLocal = identity;
	m_pTransport = this;

	// Encryption is not used for pipe connections.
	// This is not strictly necessary, since we never even send packets or
	// touch payload bytes at all, we just shift some pointers around.
	// But it's nice to make it official
	m_connectionConfig.m_Unencrypted.Set( 3 );

	// Slam in a really large SNP rate so that we are never rate limited
	int nRate = 0x10000000;
	m_connectionConfig.m_SendRateMin.Set( nRate );
	m_connectionConfig.m_SendRateMax.Set( nRate );
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
	NOTE_UNUSED( pbThinkImmediately );
	if ( !m_pPartner )
	{
		// Caller should have checked the connection at a higher level, so this is a bug
		AssertMsg( false, "No partner pipe?" );
		pMsg->Release();
		return -k_EResultFail;
	}

	// Fake a bunch of stats
	FakeSendStats( usecNow, pMsg->m_cbSize );

	// Set fields to their values applicable on the receiving side
	// NOTE: This assumes that we can muck with the structure,
	//       and that the caller won't need to look at the original
	//       object any more.
	int nMsgNum = ++m_senderState.m_nLastSentMsgNum;
	pMsg->m_nMessageNumber = nMsgNum;
	pMsg->m_conn = m_pPartner->m_hConnectionSelf;
	pMsg->m_identityPeer = m_pPartner->m_identityRemote;
	pMsg->m_nConnUserData = m_pPartner->m_nUserData;
	pMsg->m_usecTimeReceived = usecNow;

	// Pass directly to our partner
	m_pPartner->ReceivedMessage( pMsg );

	return nMsgNum;
}

bool CSteamNetworkConnectionPipe::BBeginAccept( CSteamNetworkListenSocketBase *pListenSocket, SteamNetworkingMicroseconds usecNow, SteamDatagramErrMsg &errMsg )
{

	// Ordinary connections usually learn the client identity and connection ID at this point
	m_identityRemote = m_pPartner->m_identityLocal;
	m_unConnectionIDRemote = m_pPartner->m_unConnectionIDLocal;

	// Act like we came in on this listen socket
	if ( !pListenSocket->BAddChildConnection( this, errMsg ) )
		return false;

	// Assign connection ID, init crypto, transition to "connecting" state.
	if ( !BInitConnection( usecNow, 0, nullptr, errMsg ) )
		return false;

	// Receive the crypto info that is in the client's
	if ( !BRecvCryptoHandshake( m_pPartner->m_msgSignedCertLocal, m_pPartner->m_msgSignedCryptLocal, true ) )
	{
		Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
		V_sprintf_safe( errMsg, "Failed crypto init.  %s", m_szEndDebug );
		return false;
	}

	// Transition to connecting state
	return BConnectionState_Connecting( usecNow, errMsg );
}

EResult CSteamNetworkConnectionPipe::AcceptConnection( SteamNetworkingMicroseconds usecNow )
{
	if ( !m_pPartner )
	{
		Assert( false );
		return k_EResultFail;
	}

	// Mark server side connection as connected
	ConnectionState_Connected( usecNow );

	// "Send connect OK" to partner, and he is connected
	FakeSendStats( usecNow, 0 );

	// Partner "receives" ConnectOK
	m_pPartner->m_identityRemote = m_identityLocal;
	m_pPartner->m_unConnectionIDRemote = m_unConnectionIDLocal;
	if ( !m_pPartner->BRecvCryptoHandshake( m_msgSignedCertLocal, m_msgSignedCryptLocal, false ) )
		return k_EResultHandshakeFailed;
	m_pPartner->ConnectionState_Connected( usecNow );

	return k_EResultOK;
}

void CSteamNetworkConnectionPipe::FakeSendStats( SteamNetworkingMicroseconds usecNow, int cbPktSize )
{
	if ( !m_pPartner )
		return;

	// Get the next packet number we would have sent
	uint16 nSeqNum = m_statsEndToEnd.ConsumeSendPacketNumberAndGetWireFmt( usecNow );

	// And the peer receiving it immediately.  And assume every packet represents
	// a ping measurement.
	int64 nPktNum = m_pPartner->m_statsEndToEnd.ExpandWirePacketNumberAndCheck( nSeqNum );
	Assert( nPktNum+1 == m_statsEndToEnd.m_nNextSendSequenceNumber );
	m_pPartner->m_statsEndToEnd.TrackProcessSequencedPacket( nPktNum, usecNow, -1 );
	m_pPartner->m_statsEndToEnd.TrackRecvPacket( cbPktSize, usecNow );
	m_pPartner->m_statsEndToEnd.m_ping.ReceivedPing( 0, usecNow );

	// Fake sending stats
	m_statsEndToEnd.TrackSentPacket( cbPktSize );
}

void CSteamNetworkConnectionPipe::SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason )
{
	NOTE_UNUSED( eRequest );
	NOTE_UNUSED( pszReason );

	if ( !m_pPartner )
	{
		Assert( false );
		return;
	}

	// Fake sending us a ping request
	m_statsEndToEnd.TrackSentPingRequest( usecNow, false );
	FakeSendStats( usecNow, 0 );

	// Fake partner receiving it
	m_pPartner->m_statsEndToEnd.PeerAckedLifetime( usecNow );
	m_pPartner->m_statsEndToEnd.PeerAckedInstantaneous( usecNow );

	// ...and sending us a reply immediately
	m_pPartner->FakeSendStats( usecNow, 0 );

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
	// Send a "packet"
	FakeSendStats( usecNow, 0 );
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
	info.m_eTransportKind = k_ESteamNetTransport_LoopbackBuffers;
}

void CSteamNetworkConnectionPipe::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	CSteamNetworkConnectionBase::ConnectionStateChanged( eOldState );

	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: // What local "problem" could we have detected??
		default:
			AssertMsg1( false, "Invalid state %d", GetState() );
			// FALLTHROUGH
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
			if ( m_pPartner )
			{
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
				Assert( CollapseConnectionStateToAPIState( m_pPartner->GetState() ) == k_ESteamNetworkingConnectionState_None );
				Assert( m_pPartner->m_pPartner == nullptr );
				m_pPartner = nullptr;
			}
			break;
	}
}

void CSteamNetworkConnectionPipe::DestroyTransport()
{
	// Using the same object for connection and transport
	TransportFreeResources();
	m_pTransport = nullptr;
}

} // namespace SteamNetworkingSocketsLib
