//====== Copyright Valve Corporation, All rights reserved. ====================

#include <steamnetworkingsockets/isteamnetworkingsockets.h>
#include "steamnetworkingsockets_connections.h"
#include "steamnetworkingsockets_lowlevel.h"
#include "csteamnetworkingsockets.h"
#include "crypto.h"
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_gameserver.h>
#endif

#include "steamnetworkingconfig.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

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

/// Replace internal states that are not visible outside of the API with
/// the corresponding state that we show the the application.
inline ESteamNetworkingConnectionState CollapseConnectionStateToAPIState( ESteamNetworkingConnectionState eState )
{
	// All the hidden internal states are assigned negative values
	if ( eState < 0 )
		return k_ESteamNetworkingConnectionState_None;
	return eState;
}

struct TrustedKey
{
	typedef char KeyData[33];
	TrustedKey( uint64 id, const KeyData &data ) : m_id( id )
	{
		m_key.Set( &data[0], sizeof(KeyData)-1 );
	}
	const uint64 m_id;
	CECSigningPublicKey m_key;
};

// !KLUDGE! For now, we only have one trusted CA key.
// Note that it's important to burn this key into the source code,
// *not* load it from a file.  Our threat model for eavesdropping/tampering
// includes the player!  Everything outside of this process is untrusted.
// Obviously they can tamper with the process or modify the executable,
// but that puts them into VAC territory.
const TrustedKey s_arTrustedKeys[1] = {
	TrustedKey( 18220590129359924542llu, "\x9a\xec\xa0\x4e\x17\x51\xce\x62\x68\xd5\x69\x00\x2c\xa1\xe1\xfa\x1b\x2d\xbc\x26\xd3\x6b\x4e\xa3\xa0\x08\x3a\xd3\x72\x82\x9b\x84" )
};

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
//	DbgVerify( CCrypto::GenerateSHA256Digest( key.GetData(), key.GetLength(), &digest ) );
//	SpewWarning( "TrustedKey( %llullu, \"%s\" )\n", LittleQWord( *(uint64*)&digest ), bufText.String() );
//}

/////////////////////////////////////////////////////////////////////////////
//
// Message storage
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkingMessage *CSteamNetworkingMessage::New( CSteamNetworkConnectionBase *pParent, uint32 cbSize, int64 nMsgNum, SteamNetworkingMicroseconds usecNow )
{
	// FIXME Should avoid this dynamic memory call with some sort of pooling
	CSteamNetworkingMessage *pMsg = new CSteamNetworkingMessage;

	pMsg->m_steamIDSender = pParent->m_steamIDRemote;
	pMsg->m_pData = malloc( cbSize );
	pMsg->m_cbSize = cbSize;
	pMsg->m_nChannel = -1;
	pMsg->m_conn = pParent->m_hConnectionSelf;
	pMsg->m_nConnUserData = pParent->GetUserData();
	pMsg->m_usecTimeReceived = usecNow;
	pMsg->m_nMessageNumber = nMsgNum;
	pMsg->m_pfnRelease = CSteamNetworkingMessage::Delete;

	return pMsg;
}

void CSteamNetworkingMessage::Delete( SteamNetworkingMessage_t *pIMsg )
{
	CSteamNetworkingMessage *pMsg = static_cast<CSteamNetworkingMessage *>( pIMsg );

	free( pMsg->m_pData );

	// We must not currently be in any queue.  In fact, our parent
	// might have been destroyed.
	Assert( !pMsg->m_linksSameConnection.m_pQueue );
	Assert( !pMsg->m_linksSameConnection.m_pPrev );
	Assert( !pMsg->m_linksSameConnection.m_pNext );
	Assert( !pMsg->m_linksSecondaryQueue.m_pQueue );
	Assert( !pMsg->m_linksSecondaryQueue.m_pPrev );
	Assert( !pMsg->m_linksSecondaryQueue.m_pNext );

	// Self destruct
	// FIXME Should avoid this dynamic memory call with some sort of pooling
	delete pMsg;
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
	UnlinkFromQueue( &CSteamNetworkingMessage::m_linksSameConnection );
	UnlinkFromQueue( &CSteamNetworkingMessage::m_linksSecondaryQueue );
}

void SteamNetworkingMessageQueue::PurgeMessages()
{

	while ( !IsEmpty() )
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

	while ( !IsEmpty() && nMessagesReturned < nMaxMessages )
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
// CSteamNetworkListenSocketBase
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkListenSocketBase::CSteamNetworkListenSocketBase( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: m_pSteamNetworkingSocketsInterface( pSteamNetworkingSocketsInterface )
{
	m_hListenSocketSelf = k_HSteamListenSocket_Invalid;
	m_unIP = 0;
	m_unPort = 0;
}

CSteamNetworkListenSocketBase::~CSteamNetworkListenSocketBase()
{
	AssertMsg( m_mapChildConnections.Count() == 0 && !m_queueRecvMessages.m_pFirst && !m_queueRecvMessages.m_pLast, "Destroy() not used properly" );
}

void CSteamNetworkListenSocketBase::Destroy()
{

	// Destroy all child connections
	for (;;)
	{
		unsigned n = m_mapChildConnections.Count();
		if ( n == 0 )
			break;

		int h = m_mapChildConnections.FirstInorder();
		CSteamNetworkConnectionBase *pChild = m_mapChildConnections[ h ];
		Assert( pChild->m_pParentListenSocket == this );
		Assert( pChild->m_hSelfInParentListenSocketMap == h );

		pChild->Destroy();
		Assert( m_mapChildConnections.Count() == n-1 );
	}

	// Self destruct
	delete this;
}

int CSteamNetworkListenSocketBase::APIReceiveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	return m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
}

void CSteamNetworkListenSocketBase::AddChildConnection( CSteamNetworkConnectionBase *pConn )
{
	Assert( pConn->m_pParentListenSocket == nullptr );
	Assert( pConn->m_hSelfInParentListenSocketMap == -1 );
	Assert( pConn->m_hConnectionSelf == k_HSteamNetConnection_Invalid );

	ChildConnectionKey_t key( pConn->m_steamIDRemote, pConn->m_unConnectionIDRemote );
	Assert( m_mapChildConnections.Find( key ) == m_mapChildConnections.InvalidIndex() );

	// Setup linkage
	pConn->m_pParentListenSocket = this;
	pConn->m_hSelfInParentListenSocketMap = m_mapChildConnections.Insert( key, pConn );
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
		FOR_EACH_MAP_FAST( m_mapChildConnections, h )
		{
			if ( m_mapChildConnections[h] == pConn )
				m_mapChildConnections.RemoveAt(h);
		}
	}
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketStandard
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkListenSocketStandard::CSteamNetworkListenSocketStandard( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkListenSocketBase( pSteamNetworkingSocketsInterface )
{
	m_pSockIPV4Connections = nullptr;
	m_nSteamConnectVirtualPort = -1;
}

CSteamNetworkListenSocketStandard::~CSteamNetworkListenSocketStandard()
{
	// Clean up socket, if any
	if ( m_pSockIPV4Connections )
	{
		delete m_pSockIPV4Connections;
		m_pSockIPV4Connections = nullptr;
	}

	// Remove from virtual port map
	if ( m_nSteamConnectVirtualPort >= 0 )
	{
		int h = m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.Find( m_nSteamConnectVirtualPort );
		if ( h != m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.InvalidIndex() && m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort[h] == this )
		{
			m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort[h] = nullptr; // just for grins
			m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.RemoveAt( h );
		}
		else
		{
			AssertMsg( false, "Bookkeeping bug!" );
		}
		m_nSteamConnectVirtualPort = -1;
	}
}

bool CSteamNetworkListenSocketStandard::BInit( int nSteamConnectVirtualPort, uint32 nIP, uint16 nPort, SteamDatagramErrMsg &errMsg )
{
	Assert( m_pSockIPV4Connections == nullptr );
	Assert( m_nSteamConnectVirtualPort == -1 );

	if ( nPort == 0 && nSteamConnectVirtualPort == -1 )
	{
		V_strcpy_safe( errMsg, "Didn't specify any protocols to listen for" );
		return false;
	}
	if ( nPort == 0 && nIP != 0 )
	{
		V_strcpy_safe( errMsg, "Must specify local port to listen for IPv4." );
		return false;
	}

	// Listen for P2P?
	if ( nSteamConnectVirtualPort != -1 )
	{
		if ( m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.HasElement( nSteamConnectVirtualPort ) )
		{
			V_sprintf_safe( errMsg, "Already have a listen socket on P2P virtual port %d", nSteamConnectVirtualPort );
			return false;
		}
		m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.Insert( nSteamConnectVirtualPort, this );
		m_nSteamConnectVirtualPort = nSteamConnectVirtualPort;
	}

	// Listen for plain IPv4?
	if ( nPort )
	{
		m_pSockIPV4Connections = new CSharedSocket;
		if ( !m_pSockIPV4Connections->BInit( nIP, nPort, CRecvPacketCallback( ReceivedIPv4FromUnknownHost, this ), errMsg ) )
		{
			delete m_pSockIPV4Connections;
			m_pSockIPV4Connections = nullptr;
			return false;
		}
		m_unIP = nIP;
		m_unPort = nPort;
	}

	CCrypto::GenerateRandomBlock( m_argbChallengeSecret, sizeof(m_argbChallengeSecret) );

	return true;
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
	//m_nVirtualPort = -1;
	m_nUserData = -1;
	m_eConnectionState = k_ESteamNetworkingConnectionState_None;
	m_usecWhenEnteredConnectionState = 0;
	m_usecWhenSentConnectRequest = 0;
	m_ulHandshakeRemoteTimestamp = 0;
	m_usecWhenReceivedHandshakeRemoteTimestamp = 0;
	m_eEndReason = k_ESteamNetConnectionEnd_Invalid;
	m_szEndDebug[0] = '\0';
	m_unConnectionIDLocal = 0;
	m_unConnectionIDRemote = 0;
	m_pParentListenSocket = nullptr;
	m_hSelfInParentListenSocketMap = -1;
	m_bCryptKeysValid = false;
}

CSteamNetworkConnectionBase::~CSteamNetworkConnectionBase()
{
	Assert( m_hConnectionSelf == k_HSteamNetConnection_Invalid );
	Assert( m_eConnectionState == k_ESteamNetworkingConnectionState_Dead );
	Assert( m_queueRecvMessages.IsEmpty() );
	Assert( m_pParentListenSocket == nullptr );
}

void CSteamNetworkConnectionBase::Destroy()
{

	// Make sure all resources have been freed, etc
	FreeResources();

	// Self destruct NOW
	delete this;
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

	// Detach from the listen socket that owns us, if any
	if ( m_pParentListenSocket )
		m_pParentListenSocket->AboutToDestroyChildConnection( this );

	// Remove from global connection list
	if ( m_hConnectionSelf != k_HSteamNetConnection_Invalid )
	{
		int idx = m_hConnectionSelf & 0xffff;
		if ( g_listConnections[ idx ] == this )
		{
			g_listConnections[ idx ] = nullptr; // Just for grins
			g_listConnections.Remove( idx );
		}
		else
		{
			AssertMsg( false, "Connection list bookeeping corruption" );
			g_listConnections.FindAndRemove( this );
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
}

bool CSteamNetworkConnectionBase::BInitConnection( uint32 nPeerProtocolVersion, SteamNetworkingMicroseconds usecNow, SteamDatagramErrMsg &errMsg )
{
	// Select random connection ID, and make sure it passes certain sanity checks
	Assert( m_unConnectionIDLocal == 0 );
	for ( int tries = 0 ; tries < 10000 ; ++tries )
	{
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
		bool bFoundDup = false;
		for ( CSteamNetworkConnectionBase *pConn: g_listConnections )
		{
			if ( ( pConn->m_unConnectionIDLocal & 0xffff ) == ( m_unConnectionIDLocal & 0xffff ) && pConn != this )
			{
				bFoundDup = true;
				break;
			}
		}
		if ( !bFoundDup )
			break;
	}

	Assert( m_hConnectionSelf == k_HSteamNetConnection_Invalid );

	Assert( m_pParentListenSocket == nullptr || m_pSteamNetworkingSocketsInterface == m_pParentListenSocket->m_pSteamNetworkingSocketsInterface );
	#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
		m_steamIDLocal = m_pSteamNetworkingSocketsInterface->GetSteamID();
	#endif

	m_eEndReason = k_ESteamNetConnectionEnd_Invalid;
	m_szEndDebug[0] = '\0';
	m_statsEndToEnd.Init( usecNow, true ); // Until we go connected don't try to send acks, etc
	m_statsEndToEnd.m_nPeerProtocolVersion = nPeerProtocolVersion;

	// Make sure our cheesy make-unique-handle system doesn't overflow
	if ( g_listConnections.Count() >= 0xffff )
	{
		V_strcpy_safe( errMsg, "Too many connections." );
		return false;
	}

	// Use upper 16 bits as a connection sequence number, so that connection handles
	// are not reused within a short time period.
	static uint32 s_nUpperBits = 0;
	s_nUpperBits += 0x10000;
	if ( s_nUpperBits == 0 )
		s_nUpperBits = 0x10000;

	// Add it to our table of active sockets
	m_hConnectionSelf = g_listConnections.AddToTail( this ) | s_nUpperBits;

	// Set a default name if we haven't been given one
	if ( m_sName.empty() )
	{
		char temp[ 64 ];
		V_sprintf_safe( temp, "%d", m_hConnectionSelf & ~s_nUpperBits );
		m_sName = temp;
	}
	
	// Clear everything out
	ClearCrypto();

	// Switch connection state, queue state change notifications.
	SetState( k_ESteamNetworkingConnectionState_Connecting, usecNow );

	// Take action to start obtaining a cert, or if we already have one, then set it now
	InitConnectionCrypto( usecNow );

	// Queue us to think ASAP.
	SetNextThinkTime( usecNow );

	return true;
}

void CSteamNetworkConnectionBase::InitConnectionCrypto( SteamNetworkingMicroseconds usecNow )
{
	BThinkCryptoReady( usecNow );
}

void CSteamNetworkConnectionBase::ClearCrypto()
{
	m_msgCertRemote.Clear();
	m_msgCryptRemote.Clear();

	m_keyExchangePrivateKeyLocal.Wipe();
	m_msgCryptLocal.Clear();
	m_msgSignedCryptLocal.Clear();

	m_bCryptKeysValid = false;
	m_cryptKeySend.Wipe();
	m_cryptKeyRecv.Wipe();
	m_cryptIVSend.Wipe();
	m_cryptIVRecv.Wipe();
}

bool CSteamNetworkConnectionBase::RecvNonDataSequencedPacket( uint16 nWireSeqNum, SteamNetworkingMicroseconds usecNow )
{
	// Get the full end-to-end packet number
	int16 nGap = nWireSeqNum - uint16( m_statsEndToEnd.m_nLastRecvSequenceNumber );
	int64 nFullSequenceNumber = m_statsEndToEnd.m_nLastRecvSequenceNumber + nGap;
	Assert( uint16( nFullSequenceNumber ) == nWireSeqNum );

	// Check the packet gap.  If it's too old, just discard it immediately.
	if ( nGap < -16 )
		return false;
	if ( nFullSequenceNumber <= 0 ) // Sequence number 0 is not used, and we don't allow negative sequence numbers
		return false;

	// Let SNP know when we received it, so we can track loss evens and send acks
	if ( SNP_RecordReceivedPktNum( nFullSequenceNumber, usecNow ) )
	{

		// And also the general purpose sequence number/stats tracker
		// for the end-to-end flow.
		m_statsEndToEnd.TrackRecvSequencedPacketGap( nGap, usecNow, 0 );
	}

	return true;
}

bool CSteamNetworkConnectionBase::BThinkCryptoReady( SteamNetworkingMicroseconds usecNow )
{
	Assert( GetState() == k_ESteamNetworkingConnectionState_Connecting );

	// Do we already have a cert?
	if ( m_msgSignedCertLocal.has_cert() )
		return true;

	// Already have a a signed cert?
	if ( m_pSteamNetworkingSocketsInterface->m_msgSignedCert.has_ca_signature() )
	{

		// Use it!
		InitLocalCrypto( m_pSteamNetworkingSocketsInterface->m_msgSignedCert, m_pSteamNetworkingSocketsInterface->m_keyPrivateKey );
		return true;
	}

	// Check if we have intentionally disabled auth
	// !KLUDGE! This is not exactly the right test, since we're checking a
	// connection-type-specific convar and this is generic connection code.
	// might want to revisit this and make BAllowLocalUnsignedCert return
	// slightly more nuanced return value that distinguishes between
	// "Don't even try" from "try, but continue if we fail"
	if ( BAllowLocalUnsignedCert() && steamdatagram_ip_allow_connections_without_auth )
	{
		InitLocalCryptoWithUnsignedCert();
		return true;
	}

	// Otherwise, we don't have a signed cert (yet?).  Try (again?) to get one.
	// If this fails (either immediately, or asynchronously), we will
	// get a CertFailed call with the appropriate code, and we can decide
	// what we want to do.
	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Need a cert authority!" );
		Assert( false );
	#else
		m_pSteamNetworkingSocketsInterface->AsyncCertRequest();
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
	InitLocalCrypto( m_pSteamNetworkingSocketsInterface->m_msgSignedCert, m_pSteamNetworkingSocketsInterface->m_keyPrivateKey );

	// Don't check state machine now, let's just schedule immediate wake up to deal with it
	SetNextThinkTime( SteamNetworkingSockets_GetLocalTimestamp() );
}

void CSteamNetworkConnectionBase::InitLocalCrypto( const CMsgSteamDatagramCertificateSigned &msgSignedCert, const CECSigningPrivateKey &keyPrivate )
{
	Assert( msgSignedCert.has_cert() );
	Assert( keyPrivate.IsValid() );

	// Save off the signed certificate
	m_msgSignedCertLocal = msgSignedCert;

	// Set our base protocol type
	m_msgCryptLocal.set_is_snp( true );

	// Generate a keypair for key exchange
	CECKeyExchangePublicKey publicKeyLocal;
	CCrypto::GenerateKeyExchangeKeyPair( &publicKeyLocal, &m_keyExchangePrivateKeyLocal );
	m_msgCryptLocal.set_key_type( CMsgSteamDatagramSessionCryptInfo_EKeyType_CURVE25519 );
	m_msgCryptLocal.set_key_data( publicKeyLocal.GetData(), publicKeyLocal.GetLength() );

	// Generate some more randomness for the secret key
	uint64 crypt_nonce;
	CCrypto::GenerateRandomBlock( &crypt_nonce, sizeof(crypt_nonce) );
	m_msgCryptLocal.set_nonce( crypt_nonce );

	// Serialize and sign the crypt key with the private key that matches this cert
	m_msgSignedCryptLocal.set_info( m_msgCryptLocal.SerializeAsString() );
	CryptoSignature_t sig;
	CCrypto::GenerateSignature( (const uint8 *)m_msgSignedCryptLocal.info().c_str(), (uint32)m_msgSignedCryptLocal.info().length(), keyPrivate, &sig );
	m_msgSignedCryptLocal.set_signature( &sig, sizeof(sig) );
}

void CSteamNetworkConnectionBase::InitLocalCryptoWithUnsignedCert()
{

	// Generate a keypair
	CECSigningPrivateKey keyPrivate;
	CECSigningPublicKey keyPublic;
	CCrypto::GenerateSigningKeyPair( &keyPublic, &keyPrivate );

	// Generate a cert
	CMsgSteamDatagramCertificate msgCert;
	msgCert.set_key_data( keyPublic.GetData(), keyPublic.GetLength() );
	msgCert.set_key_type( CMsgSteamDatagramCertificate_EKeyType_ED25519 );
	msgCert.set_steam_id( m_steamIDLocal.ConvertToUint64() );
	msgCert.set_app_id( m_pSteamNetworkingSocketsInterface->m_nAppID );

	// Should we set an expiry?  I mean it's unsigned, so it has zero value, so probably not
	//s_msgCertLocal.set_time_created( );

	// Serialize into "signed" message type, although we won't actually sign it.
	CMsgSteamDatagramCertificateSigned msgSignedCert;
	msgSignedCert.set_cert( msgCert.SerializeAsString() );

	// Standard init, as if this were a normal cert
	InitLocalCrypto( msgSignedCert, keyPrivate );
}

void CSteamNetworkConnectionBase::CertRequestFailed( ESteamNetConnectionEnd nConnectionEndReason, const char *pszMsg )
{

	// Make sure we care about this
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting )
		return;
	if ( BHasLocalCert() )
		return;

	// Do we require a signed cert?
	if ( !BAllowLocalUnsignedCert() )
	{
		// This is fatal
		SpewWarning( "Connection %u cannot use self-signed cert; failing connection.\n", m_unConnectionIDLocal );
		ConnectionState_ProblemDetectedLocally( nConnectionEndReason, "Cert failure: %s", pszMsg );
		return;
	}

	SpewWarning( "Connection %u is continuing with self-signed cert.\n", m_unConnectionIDLocal );
	InitLocalCryptoWithUnsignedCert();

	// Schedule immediate wake up to check on state machine
	SetNextThinkTime( SteamNetworkingSockets_GetLocalTimestamp() );
}

bool CSteamNetworkConnectionBase::BRecvCryptoHandshake( const CMsgSteamDatagramCertificateSigned &msgCert, const CMsgSteamDatagramSessionCryptInfoSigned &msgSessionInfo, bool bServer )
{

	// Have we already done key exchange?
	if ( m_bCryptKeysValid )
	{
		// FIXME - Probably should check that they aren't changing any keys.
		return true;
	}

	// Make sure we have what we need
	if ( !msgCert.has_cert() || !msgSessionInfo.has_info() )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Crypto handshake missing cert or session data" );
		return false;
	}

	// Deserialize the cert
	if ( !m_msgCertRemote.ParseFromString( msgCert.cert() ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Cert failed protobuf decode" );
		return false;
	}

	// Identity public key
	CECSigningPublicKey keySigningPublicKeyRemote;
	if ( m_msgCertRemote.key_type() != CMsgSteamDatagramCertificate_EKeyType_ED25519 )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Unsupported identity key type" );
		return false;
	}
	if ( !keySigningPublicKeyRemote.Set( m_msgCertRemote.key_data().c_str(), (uint32)m_msgCertRemote.key_data().length() ) || !keySigningPublicKeyRemote.IsValid() )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Cert has invalid identity key" );
		return false;
	}

	// We need a cert.  If we don't have one by now, then we might try generating one
	if ( m_msgSignedCertLocal.has_cert() )
	{
		Assert( m_msgCryptLocal.has_nonce() );
		Assert( m_msgCryptLocal.has_key_data() );
		Assert( m_msgCryptLocal.has_key_type() );
	}
	else
	{
		if ( !BAllowLocalUnsignedCert() )
		{
			// Derived class / calling code should check for this and handle it better and fail
			// earlier with a more specific error message.  (Or allow self-signed certs)
			//ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "We don't have cert, and self-signed certs not allowed" );
			//return false;
			SpewWarning( "We don't have cert, and unsigned certs are not supposed to be allowed here.  Continuing anyway temporarily." );
		}

		// Proceed with an unsigned cert
		InitLocalCryptoWithUnsignedCert();
	}

	// If cert has an App ID restriction, then it better match our App
	if ( m_msgCertRemote.has_app_id() )
	{
		if ( m_msgCertRemote.app_id() != m_pSteamNetworkingSocketsInterface->m_nAppID )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert is for AppID %u instead of %u", m_msgCertRemote.app_id(), m_pSteamNetworkingSocketsInterface->m_nAppID );
			return false;
		}
	}

	// Special cert for gameservers in our data center?
	if ( m_msgCertRemote.gameserver_datacenter_ids_size()>0 && msgCert.has_ca_signature() )
	{
		if ( !m_steamIDRemote.BAnonGameServerAccount() )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Certs restricted data center are for anon GS only.  Not %s", m_steamIDRemote.Render() );
			return false;
		}
	}
	else
	{
		if ( !m_msgCertRemote.has_steam_id() )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert must be bound to a SteamID." );
			return false;
		}
		if ( !m_msgCertRemote.has_app_id() )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert must be bound to an AppID." );
			return false;
		}

		CSteamID steamIDCert( (uint64)m_msgCertRemote.steam_id() ); // !KLUDGE! The overload in CSteamID isn't working.  Why must we depend on some define?  Can't we just make this always work?
		if ( steamIDCert != m_steamIDRemote )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert was issued to %s, not %s", steamIDCert.Render(), m_steamIDRemote.Render() );
			return false;
		}
	}

	// Check if they are presenting a signature, then check it
	if ( msgCert.has_ca_signature() )
	{
		// Scan list of trusted CA keys
		bool bTrusted = false;
		for ( const TrustedKey &k: s_arTrustedKeys )
		{
			if ( msgCert.ca_key_id() != k.m_id )
				continue;
			if (
				msgCert.ca_signature().length() == sizeof(CryptoSignature_t)
				&& CCrypto::VerifySignature( (const uint8*)msgCert.cert().c_str(), (uint32)msgCert.cert().length(), k.m_key, *(const CryptoSignature_t *)msgCert.ca_signature().c_str() ) )
			{
				bTrusted = true;
				break;
			}
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Invalid cert signature" );
			return false;
		}
		if ( !bTrusted )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Cert signed with key %llu; not in trusted list", (uint64) msgCert.ca_key_id() );
			return false;
		}

		long rtNow = time( nullptr );
		#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
		if ( m_pSteamNetworkingSocketsInterface->m_pSteamUtils )
		{
			rtNow = m_pSteamNetworkingSocketsInterface->m_pSteamUtils->GetServerRealTime();
		}
		else
		{
			AssertMsg( false, "No ISteamUtils?  Using local clock to check if cert expired!" );
		}
		#endif

		// Make sure hasn't expired.  All signed certs without an expiry should be considered invalid!
		// For unsigned certs, there's no point in checking the expiry, since anybody who wanted
		// to do bad stuff could just change it, we have no protection against tampering.
		long rtExpiry = m_msgCertRemote.time_expiry();
		if ( rtNow > rtExpiry )
		{
			//ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, msg );
			//return false;
			SpewWarning( "Cert failure: Cert expired %ld secs ago at %ld\n", rtNow-rtExpiry, rtExpiry );
		}

		// Let derived class check for particular auth/crypt requirements
		if ( !BCheckRemoteCert() )
		{
			Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
			return false;
		}

	}
	else
	{
		if ( BAllowRemoteUnsignedCert() )
		{
			SpewMsg( "Remote host is using an unsigned cert.  Allowing connection, but it's not secure!\n" );
		}
		else
		{
			// Caller might have switched the state and provided a specific message.
			// if not, we'll do that for them
			if ( GetState() != k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
			{
				Assert( GetState() == k_ESteamNetworkingConnectionState_Connecting );
				ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCert, "Unsigned certs are not allowed" );
			}
			return false;
		}
	}

	// Deserialize crypt info
	if ( !m_msgCryptRemote.ParseFromString( msgSessionInfo.info() ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Crypt info failed protobuf decode" );
		return false;
	}

	// Key exchange public key
	CECKeyExchangePublicKey keyExchangePublicKeyRemote;
	if ( m_msgCryptRemote.key_type() != CMsgSteamDatagramSessionCryptInfo_EKeyType_CURVE25519 )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Unsupported DH key type" );
		return false;
	}
	if ( !keyExchangePublicKeyRemote.Set( m_msgCryptRemote.key_data().c_str(), (uint32)m_msgCryptRemote.key_data().length() ) || !keyExchangePublicKeyRemote.IsValid() )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Invalid DH key" );
		return false;
	}

	// SNP must be same on both ends
	if ( !m_msgCryptRemote.is_snp() )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Incompatible protocol format (SNP)" );
		return false;
	}

	// Diffie–Hellman key exchange to get "premaster secret"
	AutoWipeFixedSizeBuffer<sizeof(SHA256Digest_t)> premasterSecret;
	CCrypto::PerformKeyExchange( m_keyExchangePrivateKeyLocal, keyExchangePublicKeyRemote, &premasterSecret.m_buf );
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
	DbgVerify( CCrypto::GenerateHMAC256( (const uint8 *)salt, sizeof(salt), premasterSecret.m_buf, premasterSecret.k_nSize, &prk.m_buf ) );
	premasterSecret.Wipe();

	//
	// 2. Expand: Use PRK as seed to generate all the different keys we need, mixing with connection-specific context
	//

	COMPILE_TIME_ASSERT( sizeof( m_cryptKeyRecv ) == sizeof(SHA256Digest_t) );
	COMPILE_TIME_ASSERT( sizeof( m_cryptKeySend ) == sizeof(SHA256Digest_t) );
	COMPILE_TIME_ASSERT( sizeof( m_cryptIVRecv ) <= sizeof(SHA256Digest_t) );
	COMPILE_TIME_ASSERT( sizeof( m_cryptIVSend ) <= sizeof(SHA256Digest_t) );

	uint8 *expandOrder[4] = { m_cryptKeySend.m_buf, m_cryptKeyRecv.m_buf, m_cryptIVSend.m_buf, m_cryptIVRecv.m_buf };
	int expandSize[4] = { m_cryptKeySend.k_nSize, m_cryptKeyRecv.k_nSize, m_cryptIVSend.k_nSize, m_cryptIVRecv.k_nSize };
	const std::string *context[4] = { &msgCert.cert(), &m_msgSignedCertLocal.cert(), &msgSessionInfo.info(), &m_msgSignedCryptLocal.info() };
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
		DbgVerify( CCrypto::GenerateHMAC256( pStart, pLastByte - pStart + 1, prk.m_buf, prk.k_nSize, &expandTemp ) );
		V_memcpy( expandOrder[ idxExpand ], &expandTemp, expandSize[ idxExpand ] );

		//SpewMsg( "%s key %d: %02x%02x%02x%02x\n", bServer ? "Server" : "Client", idxExpand, expandTemp[0], expandTemp[1], expandTemp[2], expandTemp[3] );

		// Copy previous digest to use in generating the next one
		pStart = (uint8 *)bufContext.Base();
		V_memcpy( pStart, &expandTemp, sizeof(SHA256Digest_t) );
	}

	//
	// Tidy up key droppings
	//
	SecureZeroMemory( bufContext.Base(), bufContext.SizeAllocated() );
	SecureZeroMemory( expandTemp, sizeof(expandTemp) );

	// We're ready
	m_bCryptKeysValid = true;
	return true;
}

bool CSteamNetworkConnectionBase::BAllowLocalUnsignedCert() const
{
	// !KLUDGE! For now, assume this is OK.  We need to make this configurable and lock it down
	return true;
}

bool CSteamNetworkConnectionBase::BAllowRemoteUnsignedCert()
{
	// !KLUDGE! For now, assume this is OK.  We need to make this configurable and lock it down
	return true;
}

bool CSteamNetworkConnectionBase::BCheckRemoteCert()
{

	// No additional checks at the base class
	return true;
}

void CSteamNetworkConnectionBase::SetUserData( int64 nUserData )
{
	m_nUserData = nUserData;

	// Change user data on all messages that haven't been pulled out
	// of the queue yet.  This way we don't expose the client to weird
	// race conditions where they create a connection, and before they
	// are able to install their user data, some messages come in
	for ( CSteamNetworkingMessage *m = m_queueRecvMessages.m_pFirst ; m ; m = m->m_linksSameConnection.m_pNext )
	{
		Assert( m->GetConnection() == m_hConnectionSelf );
		m->SetConnectionUserData( m_nUserData );
	}
}

void CSteamNetworkConnectionBase::PopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const
{
	info.m_eState = CollapseConnectionStateToAPIState( m_eConnectionState );
	info.m_hListenSocket = m_pParentListenSocket ? m_pParentListenSocket->m_hListenSocketSelf : k_HSteamListenSocket_Invalid;
	info.m_unIPRemote = m_netAdrRemote.GetIP();
	info.m_unPortRemote = m_netAdrRemote.GetPort();
	info.m_idPOPRemote = 0;
	info.m_idPOPRelay = 0;
	info.m_steamIDRemote = m_steamIDRemote;
	info.m_nUserData = m_nUserData;
	info.m_eEndReason = m_eEndReason;
	V_strcpy_safe( info.m_szEndDebug, m_szEndDebug );
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

void CSteamNetworkConnectionBase::APIGetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow ) const
{
	stats.Clear();
	PopulateConnectionInfo( stats.m_info );

	// Copy end-to-end stats
	m_statsEndToEnd.GetLinkStats( stats.m_statsEndToEnd, usecNow );

	// Congestion control and bandwidth estimation
	SNP_PopulateDetailedStats( stats.m_statsEndToEnd );
}

EResult CSteamNetworkConnectionBase::APISendMessageToConnection( const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType )
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
			if ( eSendType & k_nSteamNetworkingSendFlags_NoDelay )
				return k_EResultIgnored;
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			return k_EResultNoConnection;
	}

	// Connection-type specific logic
	return _APISendMessageToConnection( pData, cbData, eSendType );
}

EResult CSteamNetworkConnectionBase::_APISendMessageToConnection( const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType )
{

	// Message too big?
	if ( cbData > k_cbMaxSteamNetworkingSocketsMessageSizeSend )
	{
		AssertMsg2( false, "Message size %d is too big.  Max is %d", cbData, k_cbMaxSteamNetworkingSocketsMessageSizeSend );
		return k_EResultInvalidParam;
	}

	// Fake loss?
	if ( !( eSendType & k_nSteamNetworkingSendFlags_Reliable ) )
	{
		if ( WeakRandomFloat( 0, 100.0 ) < steamdatagram_fakemessageloss_send )
			return k_EResultOK;
	}

	// Using SNP?
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	return SNP_SendMessage( usecNow, pData, cbData, eSendType );
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

bool CSteamNetworkConnectionBase::RecvDataChunk( uint16 nWireSeqNum, const void *pChunk, int cbChunk, int cbPacketSize, int usecTimeSinceLast, SteamNetworkingMicroseconds usecNow )
{
	Assert( m_bCryptKeysValid );

	// Get the full end-to-end packet number
	int16 nGap = nWireSeqNum - uint16( m_statsEndToEnd.m_nLastRecvSequenceNumber );
	int64 nFullSequenceNumber = m_statsEndToEnd.m_nLastRecvSequenceNumber + nGap;
	Assert( uint16( nFullSequenceNumber ) == nWireSeqNum );

	// Check the packet gap.  If it's too old, just discard it immediately.
	if ( nGap < -16 )
		return false;
	if ( nFullSequenceNumber <= 0 ) // Sequence number 0 is not used, and we don't allow negative sequence numbers
		return false;

	// Decrypt the chunk
	uint8 arDecryptedChunk[ k_cbSteamNetworkingSocketsMaxPlaintextPayloadRecv ];
	*(uint64 *)&m_cryptIVRecv.m_buf = LittleQWord( nFullSequenceNumber );

	//SpewMsg( "Recv decrypt IV %llu + %02x%02x%02x%02x, key %02x%02x%02x%02x\n", *(uint64 *)&m_cryptIVRecv.m_buf, m_cryptIVRecv.m_buf[8], m_cryptIVRecv.m_buf[9], m_cryptIVRecv.m_buf[10], m_cryptIVRecv.m_buf[11], m_cryptKeyRecv.m_buf[0], m_cryptKeyRecv.m_buf[1], m_cryptKeyRecv.m_buf[2], m_cryptKeyRecv.m_buf[3] );

	uint32 cbDecrypted = sizeof(arDecryptedChunk);
	if ( !CCrypto::SymmetricDecryptWithIV(
		(const uint8 *)pChunk, cbChunk, // encrypted
		m_cryptIVRecv.m_buf, m_cryptIVRecv.k_nSize, // IV
		arDecryptedChunk, &cbDecrypted, // output
		m_cryptKeyRecv.m_buf, m_cryptKeyRecv.k_nSize // Key
	) ) {

		// Just drop packet.
		// The assumption is that we either have a bug or some weird thing,
		// or that somebody is spoofing / tampering.  If it's the latter
		// we don't want to magnify the impact of their efforts
		SpewWarningRateLimited( usecNow, "%s packet data chunk failed to decrypt!  Could be tampering/spoofing or a bug.", m_sName.c_str() );
		return false;
	}

	//SpewVerbose( "Connection %u recv seqnum %lld (gap=%d) sz=%d %02x %02x %02x %02x\n", m_unConnectionID, unFullSequenceNumber, nGap, cbDecrypted, arDecryptedChunk[0], arDecryptedChunk[1], arDecryptedChunk[2], arDecryptedChunk[3] );

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
	if ( nGap > 0x4000 )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Generic,
			"Pkt number lurch by %d; %04x->%04x",
			nGap, (uint16)m_statsEndToEnd.m_nLastRecvSequenceNumber, nWireSeqNum);
		return false;
	}

	// Pass on to reassembly/reliability layer.  It may instruct us to act like we never received this
	// packet
	if ( !SNP_RecvDataChunk( nFullSequenceNumber, arDecryptedChunk, cbDecrypted, cbPacketSize, usecNow ) )
	{
		SpewDebug( "%s discarding pkt %lld\n", m_sName.c_str(), (long long)nFullSequenceNumber );
		return false;
	}

	// Packet is OK.  Track end-to-end flow.
	m_statsEndToEnd.TrackRecvPacket( cbPacketSize, usecNow );
	m_statsEndToEnd.TrackRecvSequencedPacketGap( nGap, usecNow, usecTimeSinceLast );
	return true;
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
	ESteamNetworkingConnectionState eOldState = m_eConnectionState;
	m_eConnectionState = eNewState;

	// Remember when we entered this state
	m_usecWhenEnteredConnectionState = usecNow;

	// Give derived classes get a chance to take action on state changes
	ConnectionStateChanged( eOldState );
}

void CSteamNetworkConnectionBase::ReceivedMessage( const void *pData, int cbData, int64 nMsgNum, SteamNetworkingMicroseconds usecNow )
{
//	// !TEST! Enable this during connection test to trap bogus messages earlier
//	#if 1
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
//	#endif

	SpewType( steamdatagram_snp_log_message, "%s: RecvMessage MsgNum=%lld sz=%d\n",
		m_sName.c_str(),
		(long long)nMsgNum,
		cbData );

	// Create a message
	CSteamNetworkingMessage *pMsg = CSteamNetworkingMessage::New( this, cbData, nMsgNum, usecNow );

	// Add to end of my queue.
	pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_linksSameConnection, &m_queueRecvMessages );

	// If we are an inbound, accepted connection, link into the listen socket's queue
	if ( m_pParentListenSocket )
		pMsg->LinkToQueueTail( &CSteamNetworkingMessage::m_linksSecondaryQueue, &m_pParentListenSocket->m_queueRecvMessages );

	// Copy the data
	memcpy( const_cast<void*>( pMsg->GetData() ), pData, cbData );
}

void CSteamNetworkConnectionBase::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{

	// Post a notification when certain state changes occur.  Note that
	// "internal" state changes, where the connection is effectively closed
	// from the application's perspective, are not relevant
	ESteamNetworkingConnectionState eOldAPIState = CollapseConnectionStateToAPIState( eOldState );
	ESteamNetworkingConnectionState eNewAPIState = CollapseConnectionStateToAPIState( GetState() );
	if ( eOldAPIState != eNewAPIState )
		PostConnectionStateChangedCallback( eOldAPIState, eNewAPIState );

	// Any time we switch into a state that is closed from an API perspective,
	// discard any unread received messages
	if ( eNewAPIState == k_ESteamNetworkingConnectionState_None )
		m_queueRecvMessages.PurgeMessages();

	// Check crypto state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:

			// Clear out any secret state, since we can't use it anymore anyway.
			ClearCrypto();

			// And let stats tracking system know that it shouldn't
			// expect to be able to get stuff acked, etc
			m_statsEndToEnd.SetDisconnected( true, m_usecWhenEnteredConnectionState );
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			// Don't bother trading stats back and forth with peer,
			// the only message we will send to them is "connection has been closed"
			m_statsEndToEnd.SetDisconnected( true, m_usecWhenEnteredConnectionState );
			break;

		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_FindingRoute:

			// Key exchange should be complete
			Assert( m_bCryptKeysValid );
			m_statsEndToEnd.SetDisconnected( false, m_usecWhenEnteredConnectionState );
			break;

		case k_ESteamNetworkingConnectionState_Connecting:

			// If we've completed key exchange, then we should be connected
			Assert( !m_bCryptKeysValid );

			// And we shouldn't mark stats object as ready until we go connecteded
			Assert( m_statsEndToEnd.IsDisconnected() );
			break;
	}
}

void CSteamNetworkConnectionBase::PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState )
{
	SteamNetConnectionStatusChangedCallback_t c;
	PopulateConnectionInfo( c.m_info );
	c.m_eOldState = eOldAPIState;
	c.m_hConn = m_hConnectionSelf;
	m_pSteamNetworkingSocketsInterface->QueueCallback( c );
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
			Assert( false );
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
			SetState( k_ESteamNetworkingConnectionState_ProblemDetectedLocally, usecNow );
			break;
	}

	CheckConnectionStateAndSetNextThinkTime( usecNow );
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
			CheckConnectionStateAndSetNextThinkTime( usecNow );
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
			SetState( k_ESteamNetworkingConnectionState_ClosedByPeer, SteamNetworkingSockets_GetLocalTimestamp() );
			break;
	}
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
				SetState( k_ESteamNetworkingConnectionState_Connected, usecNow );

				SNP_InitializeConnection( usecNow );
			}

			break;

		case k_ESteamNetworkingConnectionState_Connected:
			break;
	}

	// Make sure if we have any data already queued, that we start sending it out ASAP
	CheckConnectionStateAndSetNextThinkTime( usecNow );
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
			SetState( k_ESteamNetworkingConnectionState_FindingRoute, usecNow );
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:
			break;
	}

	// Make sure if we have any data already queued, that we start sending it out ASAP
	CheckConnectionStateAndSetNextThinkTime( usecNow );
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

	// CheckConnectionStateAndSetNextThinkTime does all the work of examining the current state
	// and deciding what to do.  But it should be safe to call at any time, whereas Think()
	// has a fixed contract: it should only be called by the thinker framework.
	CheckConnectionStateAndSetNextThinkTime( usecNow );
}

void CSteamNetworkConnectionBase::CheckConnectionStateAndSetNextThinkTime( SteamNetworkingMicroseconds usecNow )
{
	// Assume a default think interval just to make sure we check in periodically
	SteamNetworkingMicroseconds usecMinNextThinkTime = usecNow + k_nMillion;
	SteamNetworkingMicroseconds usecMaxNextThinkTime = usecMinNextThinkTime + 100*1000;

	auto UpdateMinThinkTime = [&]( SteamNetworkingMicroseconds usecTime, int msTol ) {
		if ( usecTime < usecMinNextThinkTime )
			usecMinNextThinkTime = usecTime;
		SteamNetworkingMicroseconds usecEnd = usecTime + msTol*1000;
		#ifdef _MSC_VER // Fix warning about optimization assuming no overflow
		Assert( usecEnd > usecTime );
		#endif
		if ( usecEnd < usecMaxNextThinkTime )
			usecMaxNextThinkTime = usecEnd;
	};

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
			// We don't send any data packets or keepalives in this state.
			// We're just waiting for the client API to close us.
			return;

		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connecting:
		{

			// Timeout?
			SteamNetworkingMicroseconds usecTimeout = m_usecWhenEnteredConnectionState + (SteamNetworkingMicroseconds)steamdatagram_timeout_seconds_initial*k_nMillion;
			if ( usecNow >= usecTimeout )
			{
				// Check if the application just didn't ever respond, it's probably a bug.
				// We should squawk about this and let them know.
				if ( m_eConnectionState != k_ESteamNetworkingConnectionState_FindingRoute && m_pParentListenSocket )
				{
					AssertMsg( false, "Application didn't accept or close incoming connection in a reasonable amount of time.  This is probably a bug." );
				}

				ConnectionTimedOut( usecNow );
				AssertMsg( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally, "ConnectionTimedOut didn't do what it is supposed to!" );
				return;
			}

			if ( m_pParentListenSocket || m_eConnectionState == k_ESteamNetworkingConnectionState_FindingRoute )
			{
				UpdateMinThinkTime( usecTimeout, +10 );
			}
			else
			{

				SteamNetworkingMicroseconds usecRetry = usecNow + k_nMillion/20;

				// Do we have all of our crypt stuff ready?
				if ( BThinkCryptoReady( usecNow ) )
				{

					// Time to try to send an end-to-end connection?  If we cannot send packets now, then we
					// really ought to be called again if something changes, but just in case we don't, set a
					// reasonable polling interval.
					if ( BCanSendEndToEndConnectRequest() )
					{
						usecRetry = m_usecWhenSentConnectRequest + k_usecConnectRetryInterval;
						if ( usecNow >= usecRetry )
						{
							SendEndToEndConnectRequest( usecNow ); // don't return true from within BCanSendEndToEndPackets if you can't do this!
							m_usecWhenSentConnectRequest = usecNow;
							usecRetry = m_usecWhenSentConnectRequest + k_usecConnectRetryInterval;
						}
					}
				}

				UpdateMinThinkTime( usecRetry, +5 );
			}
		} break;

		case k_ESteamNetworkingConnectionState_Linger:

			if ( true /* FIXME nothing is queued for send */ )
			{
				// Close the connection ASAP
				ConnectionState_FinWait();
				return;
			}

		// |
		// | otherwise, fall through
		// V
		case k_ESteamNetworkingConnectionState_Connected:
		{
			SteamNetworkingMicroseconds usecNextThinkSNP = SNP_ThinkSendState( usecNow );
			AssertMsg1( usecNextThinkSNP > usecNow, "SNP next think time must be in in the future.  It's %lldusec in the past", (long long)( usecNow - usecNextThinkSNP ) );

			// Set a pretty tight tolerance if SNP wants to wake up at a certain time.
			if ( usecNextThinkSNP < k_nThinkTime_Never )
				UpdateMinThinkTime( usecNextThinkSNP, +1 );
		} break;
	}

	// Update stats
	m_statsEndToEnd.Think( usecNow );

	// Check for sending keepalives or probing a connection that appears to be timing out
	if ( m_eConnectionState != k_ESteamNetworkingConnectionState_Connecting && m_eConnectionState != k_ESteamNetworkingConnectionState_FindingRoute )
	{
		Assert( m_statsEndToEnd.m_usecTimeLastRecv > 0 ); // How did we get connected without receiving anything end-to-end?

		SteamNetworkingMicroseconds usecEndToEndConnectionTimeout = m_statsEndToEnd.m_usecTimeLastRecv + (SteamNetworkingMicroseconds)steamdatagram_timeout_seconds_connected*k_nMillion;
		if ( usecNow >= usecEndToEndConnectionTimeout )
		{
			if ( m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv >= 4 || !BCanSendEndToEndData() )
			{
				ConnectionTimedOut( usecNow );
				AssertMsg( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally, "ConnectionTimedOut didn't do what it is supposed to!" );
				return;
			}
			// The timeout time has expired, but we haven't marked enough packets as dropped yet?
			// Hm, this is weird, probably our aggressive pinging code isn't working or something.
			// In any case, just check in a bit
			UpdateMinThinkTime( usecNow + 100*1000, +100 );
		}
		else
		{
			UpdateMinThinkTime( usecEndToEndConnectionTimeout, +100 );
		}

		// Check for keepalives of varying urgency.
		// Ping aggressively because connection appears to be timing out?
		if ( m_statsEndToEnd.m_nReplyTimeoutsSinceLastRecv > 0 )
		{
			SteamNetworkingMicroseconds usecSendAggressivePing = Max( m_statsEndToEnd.m_usecTimeLastRecv, m_statsEndToEnd.m_usecLastSendPacketExpectingImmediateReply ) + k_usecAggressivePingInterval;
			if ( usecNow >= usecSendAggressivePing )
			{
				if ( BCanSendEndToEndData() )
				{
					SpewVerbose( "Connection to %s appears to be timing out.  Sending keepalive.\n", m_steamIDRemote.Render() );
					Assert( m_statsEndToEnd.BNeedToSendPingImmediate( usecNow ) ); // Make sure logic matches
					SendEndToEndPing( true, usecNow );
					AssertMsg( !m_statsEndToEnd.BNeedToSendPingImmediate( usecNow ), "SendEndToEndPing didn't do its job!" );
					Assert( m_statsEndToEnd.m_usecInFlightReplyTimeout != 0 );
				}
				else
				{
					// Nothing we can do right now.  Just check back in a little bit.
					UpdateMinThinkTime( usecNow+20*1000, +5 );
				}
			}
			else
			{
				UpdateMinThinkTime( usecSendAggressivePing, +20 );
			}
		}

		// Ordinary keepalive?
		if ( m_statsEndToEnd.m_usecInFlightReplyTimeout == 0 )
		{
			// FIXME We really should be a lot better here with an adaptive keepalive time.  If they have been
			// sending us a steady stream of packets, we could expect it to continue at a high rate, so that we
			// can begin to detect a dropped connection much more quickly.  But if the connection is mostly idle, we want
			// to make sure we use a relatively long keepalive.
			SteamNetworkingMicroseconds usecSendKeepalive = m_statsEndToEnd.m_usecTimeLastRecv+k_usecKeepAliveInterval;
			if ( usecNow >= usecSendKeepalive )
			{
				if ( BCanSendEndToEndData() )
				{
					Assert( m_statsEndToEnd.BNeedToSendKeepalive( usecNow ) ); // Make sure logic matches
					SendEndToEndPing( false, usecNow );
					AssertMsg( !m_statsEndToEnd.BNeedToSendKeepalive( usecNow ), "SendEndToEndPing didn't do its job!" );
				}
				else
				{
					// Nothing we can do right now.  Just check back in a little bit.
					UpdateMinThinkTime( usecNow+20*1000, +5 );
				}
			}
			else
			{
				// Not right now, but schedule a wakeup call to do it
				UpdateMinThinkTime( usecSendKeepalive, +100 );
			}
		}
	}

	// Scheduled think time must be in the future.  If some code is setting a think time for right now,
	// then it should have just done it.
	if ( usecMinNextThinkTime <= usecNow )
	{
		AssertMsg1( false, "Scheduled next think time must be in in the future.  It's %lldusec in the past", (long long)( usecNow - usecMinNextThinkTime ) );
		usecMinNextThinkTime = usecNow + 1000;
		usecMaxNextThinkTime = usecMinNextThinkTime + 2000;
	}

	// Hook for derived class to do its connection-type-specific stuff
	ThinkConnection( usecNow );

	// Schedule next time to think, if derived class didn't request an earlier
	// wakeup call.  We ask that we not be woken up early, because none of the code
	// above who is setting this timeout will trigger, and we'll just go back to
	// sleep again.  So better to be just a tiny bit late than a tiny bit early.
	Assert( usecMaxNextThinkTime >= usecMinNextThinkTime+1000 );
	EnsureMinThinkTime( usecMinNextThinkTime, (usecMaxNextThinkTime-usecMinNextThinkTime)/1000 );
}

void CSteamNetworkConnectionBase::ThinkConnection( SteamNetworkingMicroseconds usecNow )
{
}

void CSteamNetworkConnectionBase::ConnectionTimedOut( SteamNetworkingMicroseconds usecNow )
{
	ESteamNetConnectionEnd nReasonCode;
	ConnectionEndDebugMsg msg;

	// Set some generic defaults using our base class version, so
	// this function will work even if the derived class forgets to
	// call the base class.
	CSteamNetworkConnectionBase::GuessTimeoutReason( nReasonCode, msg, usecNow );

	// Check if connection has a more enlightened understanding of what's wrong
	GuessTimeoutReason( nReasonCode, msg, usecNow );

	// Switch connection state
	ConnectionState_ProblemDetectedLocally( nReasonCode, "%s", msg );
}

void CSteamNetworkConnectionBase::GuessTimeoutReason( ESteamNetConnectionEnd &nReasonCode, ConnectionEndDebugMsg &msg, SteamNetworkingMicroseconds usecNow )
{
	NOTE_UNUSED( usecNow );

	nReasonCode = k_ESteamNetConnectionEnd_Misc_Timeout;
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Connecting:
			V_strcpy_safe( msg, "Timed out attempting to connect" );
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:
			V_strcpy_safe( msg, "Timed out attempting to negotiate rendezvous" );
			break;

		default:
			V_strcpy_safe( msg, "Connection dropped" );
			break;
	}
}

void CSteamNetworkConnectionBase::UpdateSpeeds( int nTXSpeed, int nRXSpeed )
{
	m_statsEndToEnd.UpdateSpeeds( nTXSpeed, nRXSpeed );
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkConnectionPipe
//
/////////////////////////////////////////////////////////////////////////////

bool CSteamNetworkConnectionPipe::APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionPipe *pConn[2] )
{
	SteamDatagramErrMsg errMsg;
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	pConn[1] = new CSteamNetworkConnectionPipe( pSteamNetworkingSocketsInterface );
	pConn[0] = new CSteamNetworkConnectionPipe( pSteamNetworkingSocketsInterface );
	if ( !pConn[0] || !pConn[1] )
	{
failed:
		delete pConn[0]; pConn[0] = nullptr;
		delete pConn[1]; pConn[1] = nullptr;
		return false;
	}

	pConn[0]->m_pPartner = pConn[1];
	pConn[1]->m_pPartner = pConn[0];

	// Do generic base class initialization
	for ( int i = 0 ; i < 2 ; ++i )
	{
		if ( !pConn[i]->BInitConnection( k_nCurrentProtocolVersion, usecNow, errMsg ) )
			goto failed;

		// Slam in a really large SNP rate
		int nRate = 0x10000000;
		pConn[i]->SetMinimumRate( nRate );
		pConn[i]->SetMaximumRate( nRate );
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
		p->m_steamIDRemote = q->m_steamIDLocal;
		p->m_unConnectionIDRemote = q->m_unConnectionIDLocal;
		if ( !p->BRecvCryptoHandshake( q->m_msgSignedCertLocal, q->m_msgSignedCryptLocal, i==0 ) )
		{
			AssertMsg( false, "BRecvCryptoHandshake failed creating localhost socket pair" );
			goto failed;
		}
		p->ConnectionState_Connected( usecNow );
	}

	return true;
}

CSteamNetworkConnectionPipe::CSteamNetworkConnectionPipe( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkConnectionBase( pSteamNetworkingSocketsInterface )
, m_pPartner( nullptr )
{
}

CSteamNetworkConnectionPipe::~CSteamNetworkConnectionPipe()
{
	Assert( !m_pPartner );
}

bool CSteamNetworkConnectionPipe::BAllowRemoteUnsignedCert() { return true; }

void CSteamNetworkConnectionPipe::InitConnectionCrypto( SteamNetworkingMicroseconds usecNow )
{
	InitLocalCryptoWithUnsignedCert();
}

EResult CSteamNetworkConnectionPipe::_APISendMessageToConnection( const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType )
{
	if ( !m_pPartner )
	{
		// Caller should have checked the connection at a higher level, so this is a bug
		AssertMsg( false, "No partner pipe?" );
		return k_EResultFail;
	}
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Fake a bunch of stats
	FakeSendStats( usecNow, cbData );

	int64 nMsgNum = ++m_senderState.m_nLastSentMsgNum;

	// Pass directly to our partner
	m_pPartner->ReceivedMessage( pData, cbData, nMsgNum, usecNow );

	return k_EResultOK;
}

void CSteamNetworkConnectionPipe::FakeSendStats( SteamNetworkingMicroseconds usecNow, int cbPktSize )
{
	if ( !m_pPartner )
		return;

	// Fake us sending a packet imediately
	uint16 nSeqNum = m_statsEndToEnd.GetNextSendSequenceNumber( usecNow );
	m_statsEndToEnd.TrackSentPacket( cbPktSize );

	// And the peer receiving it immediately.  And assume every packet represents
	// a ping measurement.
	m_pPartner->m_statsEndToEnd.TrackRecvSequencedPacket( nSeqNum, usecNow, -1 );
	m_pPartner->m_statsEndToEnd.TrackRecvPacket( cbPktSize, usecNow );
	m_pPartner->m_statsEndToEnd.m_ping.ReceivedPing( 0, usecNow );
}

void CSteamNetworkConnectionPipe::SendEndToEndPing( bool bUrgent, SteamNetworkingMicroseconds usecNow )
{
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
	m_pPartner->m_statsEndToEnd.PeerAckedLifetime( usecNow );
	m_pPartner->m_statsEndToEnd.PeerAckedInstantaneous( usecNow );
}

bool CSteamNetworkConnectionPipe::BCanSendEndToEndConnectRequest() const
{
	// We're never not connected, so nobody should ever need to ask this question
	AssertMsg( false, "Shouldn't need to ask this question" );
	return false;
}

bool CSteamNetworkConnectionPipe::BCanSendEndToEndData() const
{
	Assert( m_pPartner );
	return m_pPartner != nullptr;
}

void CSteamNetworkConnectionPipe::SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow )
{
	AssertMsg( false, "Inconceivable!" );
}

EResult CSteamNetworkConnectionPipe::APIAcceptConnection()
{
	AssertMsg( false, "Inconceivable!" );
	return k_EResultFail;
}

int CSteamNetworkConnectionPipe::SendEncryptedDataChunk( const void *pChunk, int cbChunk, SteamNetworkingMicroseconds usecNow, void *pConnectionContext )
{
	AssertMsg( false, "CSteamNetworkConnectionPipe connections shouldn't try to send 'packets'!" );
	return -1;
}

void CSteamNetworkConnectionPipe::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	CSteamNetworkConnectionBase::ConnectionStateChanged( eOldState );

	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: // What local "problem" could we have detected??
		default:
			Assert( false );
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

void CSteamNetworkConnectionPipe::PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState )
{
	// Don't post any callbacks for the initial transitions.
	if ( eNewAPIState == k_ESteamNetworkingConnectionState_Connected || eNewAPIState == k_ESteamNetworkingConnectionState_Connected )
		return;

	// But post callbacks for these guys
	CSteamNetworkConnectionBase::PostConnectionStateChangedCallback( eOldAPIState, eNewAPIState );
}

} // namespace SteamNetworkingSocketsLib
