//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_CONNECTIONS_H
#define STEAMNETWORKINGSOCKETS_CONNECTIONS_H
#pragma once

#include "../steamnetworkingsockets_internal.h"
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include "../steamdatagram_internal.h"
#include <steam/steamdatagram_tickets.h>
#endif
#include "../steamnetworking_statsutils.h"
#include <tier1/utlhashmap.h>
#include <tier1/netadr.h>
#include "steamnetworkingsockets_lowlevel.h"
#include "keypair.h"
#include "crypto.h"
#include <tier0/memdbgoff.h>
#include <steamnetworkingsockets_messages.pb.h>
#include <tier0/memdbgon.h>

#include "steamnetworkingsockets_snp.h"

struct SteamNetConnectionStatusChangedCallback_t;
class ISteamNetworkingSocketsSerialized;

namespace SteamNetworkingSocketsLib {

const SteamNetworkingMicroseconds k_usecConnectRetryInterval = k_nMillion/2;
const SteamNetworkingMicroseconds k_usecFinWaitTimeout = 5*k_nMillion;

typedef char ConnectionEndDebugMsg[ k_cchSteamNetworkingMaxConnectionCloseReason ];

class CSteamNetworkingSockets;
class CSteamNetworkingMessages;
class CSteamNetworkConnectionBase;
class CSharedSocket;
struct SteamNetworkingMessageQueue;
struct SNPAckSerializerHelper;

// Fixed size byte array that automatically wipes itself upon destruction.
// Used for storage of secret keys, etc.
template <int N>
class AutoWipeFixedSizeBuffer
{
public:
	enum { k_nSize = N };
	uint8 m_buf[ N ];

	// You can wipe before destruction if you want
	inline void Wipe() { SecureZeroMemory( m_buf, N ); }

	// Wipe on destruction
	inline ~AutoWipeFixedSizeBuffer() { Wipe(); }
};

/// In various places, we need a key in a map of remote connections.
struct RemoteConnectionKey_t
{
	SteamNetworkingIdentity m_identity;
	uint32 m_unConnectionID;

	// NOTE: If we assume that peers are well behaved, then we
	// could just use the connection ID, which is a random number.
	// but let's not assume that.  In fact, if we really need to
	// protect against malicious clients we might have to include
	// some random private data so that they don't know how our hash
	// function works.  We'll assume for now that this isn't a problem
	struct Hash { uint32 operator()( const RemoteConnectionKey_t &x ) const { return SteamNetworkingIdentityHash{}( x.m_identity ) ^ x.m_unConnectionID; } };
	inline bool operator ==( const RemoteConnectionKey_t &x ) const
	{
		return m_unConnectionID == x.m_unConnectionID && m_identity == x.m_identity;
	}
};

/// Base class for connection-type-specific context structure 
struct SendPacketContext_t
{
	inline SendPacketContext_t( SteamNetworkingMicroseconds usecNow, const char *pszReason ) : m_usecNow( usecNow ), m_pszReason( pszReason ) {}
	const SteamNetworkingMicroseconds m_usecNow;
	int m_cbMaxEncryptedPayload;
	const char *m_pszReason; // Why are we sending this packet?
};

template<typename TStatsMsg>
struct SendPacketContext : SendPacketContext_t
{
	inline SendPacketContext( SteamNetworkingMicroseconds usecNow, const char *pszReason ) : SendPacketContext_t( usecNow, pszReason ) {}

	uint32 m_nFlags; // Message flags that we need to set.
	TStatsMsg msg; // Type-specific stats message
	int m_cbMsgSize; // Size of message
	int m_cbTotalSize; // Size needed in the header, including the serialized size field

	void SlamFlagsAndCalcSize()
	{
		SetStatsMsgFlagsIfNotImplied( msg, m_nFlags );
		m_cbTotalSize = m_cbMsgSize = msg.ByteSize();
		if ( m_cbMsgSize > 0 )
			m_cbTotalSize += VarIntSerializedSize( (uint32)m_cbMsgSize );
	}

	bool Serialize( byte *&p )
	{
		if ( m_cbTotalSize <= 0 )
			return false;

		// Serialize the stats size, var-int encoded
		byte *pOut = SerializeVarInt( p, uint32( m_cbMsgSize ) );

		// Serialize the actual message
		pOut = msg.SerializeWithCachedSizesToArray( pOut );

		// Make sure we wrote the number of bytes we expected
		if ( pOut != p + m_cbTotalSize )
		{
			// ABORT!
			AssertMsg( false, "Size mismatch after serializing inline stats blob" );
			return false;
		}

		// Advance pointer
		p = pOut;
		return true;
	}

	void CalcMaxEncryptedPayloadSize( size_t cbHdrReserve )
	{
		Assert( m_cbTotalSize >= 0 );
		m_cbMaxEncryptedPayload = k_cbSteamNetworkingSocketsMaxUDPMsgLen - (int)cbHdrReserve - m_cbTotalSize;
		if ( m_cbMaxEncryptedPayload < 512 )
		{
			AssertMsg2( m_cbMaxEncryptedPayload < 512, "%s is really big (%d bytes)!", msg.GetTypeName().c_str(), m_cbTotalSize );
		}
	}

};

/////////////////////////////////////////////////////////////////////////////
//
// Message storage implementation
//
/////////////////////////////////////////////////////////////////////////////

class CSteamNetworkingMessage : public SteamNetworkingMessage_t
{
public:
	static CSteamNetworkingMessage *New( CSteamNetworkConnectionBase *pParent, uint32 cbSize, int64 nMsgNum, SteamNetworkingMicroseconds usecNow );
	static void DefaultFreeData( SteamNetworkingMessage_t *pMsg );

	/// Remove it from queues
	void Unlink();

	struct Links
	{
		SteamNetworkingMessageQueue *m_pQueue = nullptr;
		CSteamNetworkingMessage *m_pPrev = nullptr;
		CSteamNetworkingMessage *m_pNext = nullptr;
	};

	/// Next message on the same connection
	Links m_linksSameConnection;

	/// Next message from the same listen socket or P2P channel (depending on message type)
	Links m_linksSecondaryQueue;

	/// Override connection handle
	inline void SetConnection( HSteamNetConnection hConn ) { m_conn = hConn; }

	/// Setup user data
	inline void SetConnectionUserData( int64 nUserData )
	{
		m_nConnUserData = nUserData;
	}

	/// Setup P2P channel
	inline void SetChannel( int nChannel )
	{
		m_nChannel = nChannel;
	}

	void LinkToQueueTail( Links CSteamNetworkingMessage::*pMbrLinks, SteamNetworkingMessageQueue *pQueue );
	void UnlinkFromQueue( Links CSteamNetworkingMessage::*pMbrLinks );
};

struct SteamNetworkingMessageQueue
{
	CSteamNetworkingMessage *m_pFirst = nullptr;
	CSteamNetworkingMessage *m_pLast = nullptr;

	inline bool IsEmpty() const
	{
		if ( m_pFirst )
		{
			Assert( m_pLast );
			return false;
		}
		Assert( !m_pLast );
		return true;
	}

	/// Remove the first messages out of the queue (up to nMaxMessages).  Returns the number returned
	int RemoveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages );

	/// Delete all queued messages
	void PurgeMessages();
};

/// Connections created through the "messages" interface are not directly exposed to the app.
// They have different mechanisms for notifying of received messages and state changes.
class ISteamNetworkingMessagesSession
{
public:
	CSteamNetworkConnectionBase *m_pConnection; // active connection, if any.  Might be NULL!
	virtual void ReceivedMessage( const void *pData, int cbData, int64 nMsgNum, SteamNetworkingMicroseconds usecNow ) = 0;
	virtual void ConnectionStateChanged( ESteamNetworkingConnectionState eOldState, ESteamNetworkingConnectionState eNewState ) = 0;
};

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketBase
//
/////////////////////////////////////////////////////////////////////////////

/// Abstract base class for a listen socket that can accept connections.
class CSteamNetworkListenSocketBase
{
public:

	/// Destroy the listen socket, and all of its accepted connections
	virtual void Destroy();

	/// Called when we receive a connection attempt, to setup the linkage.
	void AddChildConnection( CSteamNetworkConnectionBase *pConn );

	/// This gets called on an accepted connection before it gets destroyed
	virtual void AboutToDestroyChildConnection( CSteamNetworkConnectionBase *pConn );

	int APIReceiveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages );
	virtual bool APIGetAddress( SteamNetworkingIPAddr *pAddress );

	/// Map of child connections
	CUtlHashMap<RemoteConnectionKey_t, CSteamNetworkConnectionBase *, std::equal_to<RemoteConnectionKey_t>, RemoteConnectionKey_t::Hash > m_mapChildConnections;

	/// Linked list of messages received through any connection on this listen socket
	SteamNetworkingMessageQueue m_queueRecvMessages;

	/// Index into the global list
	HSteamListenSocket m_hListenSocketSelf;

	/// What interface is responsible for this listen socket?
	CSteamNetworkingSockets *const m_pSteamNetworkingSocketsInterface;

	/// Configuration options that will apply to all connections accepted through this listen socket
	ConnectionConfig m_connectionConfig;

protected:
	CSteamNetworkListenSocketBase( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );
	virtual ~CSteamNetworkListenSocketBase(); // hidden destructor, don't call directly.  Use Destroy()
};

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkConnectionBase
//
/////////////////////////////////////////////////////////////////////////////

/// Abstract interface for a connection to a remote host over any underlying
/// transport.  Most of the common functionality for implementing reliable
/// connections on top of unreliable datagrams, connection quality measurement,
/// etc is implemented here. 
class CSteamNetworkConnectionBase : protected IThinker
{
public:

//
// API entry points
//

	/// Called when we close the connection locally
	void APICloseConnection( int nReason, const char *pszDebug, bool bEnableLinger );

	/// Send a message
	EResult APISendMessageToConnection( const void *pData, uint32 cbData, int nSendFlags );

	/// Flush any messages queued for Nagle
	EResult APIFlushMessageOnConnection();

	/// Receive the next message(s)
	int APIReceiveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages );

	/// Accept a connection.  This will involve sending a message
	/// to the client, and calling ConnectionState_Connected on the connection
	/// to transition it to the connected state.
	virtual EResult APIAcceptConnection() = 0;

	/// Fill in quick connection stats
	void APIGetQuickConnectionStatus( SteamNetworkingQuickConnectionStatus &stats );

	/// Fill in detailed connection stats
	virtual void APIGetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow );

//
// Accessor
//

	// Get/set user data
	inline int64 GetUserData() const { return m_nUserData; }
	void SetUserData( int64 nUserData );

	// Get/set name
	inline const char *GetAppName() const { return m_szAppName; }
	void SetAppName( const char *pszName );

	// Debug description
	inline const char *GetDescription() const { return m_szDescription; }

	/// High level state of the connection
	ESteamNetworkingConnectionState GetState() const { return m_eConnectionState; }

	/// Check if the connection is 'connected' from the perspective of the wire protocol.
	/// (The wire protocol doesn't care about local states such as linger)
	bool BStateIsConnectedForWirePurposes() const { return m_eConnectionState == k_ESteamNetworkingConnectionState_Connected || m_eConnectionState == k_ESteamNetworkingConnectionState_Linger; }

	/// Accessor for remote address (if we know it)
	/// FIXME - Should we delete this and move to derived classes?
	/// It's not always meaningful
	const netadr_t &GetRemoteAddr() const { return m_netAdrRemote; }

	/// Reason connection ended
	ESteamNetConnectionEnd GetConnectionEndReason() const { return m_eEndReason; }
	const char *GetConnectionEndDebugString() const { return m_szEndDebug; }

	/// When did we enter the current state?
	inline SteamNetworkingMicroseconds GetTimeEnteredConnectionState() const { return m_usecWhenEnteredConnectionState; }

	/// Fill in connection details
	virtual void PopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const;

//
// Lifetime management
//

	/// Schedule destruction at the next possible opportunity
	void QueueDestroy();

	/// Free up all resources.  Close sockets, etc
	virtual void FreeResources();

	/// Destroy the connection NOW
	void Destroy();

//
// Connection state machine
// Functions to transition to the specified state.
//

	void ConnectionState_ProblemDetectedLocally( ESteamNetConnectionEnd eReason, PRINTF_FORMAT_STRING const char *pszFmt, ... ) FMTFUNCTION( 3, 4 );
	void ConnectionState_ClosedByPeer( int nReason, const char *pszDebug );
	void ConnectionState_FindingRoute( SteamNetworkingMicroseconds usecNow );
	void ConnectionState_Connected( SteamNetworkingMicroseconds usecNow );
	void ConnectionState_FinWait();

//
// Misc internal stuff
//

	/// What interface is responsible for this connection?
	CSteamNetworkingSockets *const m_pSteamNetworkingSocketsInterface;

	/// Our public handle
	HSteamNetConnection m_hConnectionSelf;

	/// Who is on the other end?  This might be invalid if we don't know yet.  (E.g. direct UDP connections.)
	SteamNetworkingIdentity m_identityRemote;

	/// Who are we?
	SteamNetworkingIdentity m_identityLocal;

	/// The listen socket through which we were accepted, if any.
	CSteamNetworkListenSocketBase *m_pParentListenSocket;

	/// Our handle in our parent's m_listAcceptedConnections (if we were accepted on a listen socket)
	int m_hSelfInParentListenSocketMap;

	// Was this connection created as part of the "messages" interface?  If so, what interface
	// owns us, and if so, are we still associated with an active session?
	CSteamNetworkingMessages *m_pMessagesInterface;
	ISteamNetworkingMessagesSession *m_pMessagesSession;

	// Linked list of received messages
	SteamNetworkingMessageQueue m_queueRecvMessages;

	/// The unique 64-bit end-to-end connection ID.  Each side picks 32 bits
	uint32 m_unConnectionIDLocal;
	uint32 m_unConnectionIDRemote;

	/// Track end-to-end stats for this connection.
	LinkStatsTracker<LinkStatsTrackerEndToEnd> m_statsEndToEnd;

	/// When we accept a connection, they will send us a timestamp we should send back
	/// to them, so that they can estimate the ping
	uint64 m_ulHandshakeRemoteTimestamp;
	SteamNetworkingMicroseconds m_usecWhenReceivedHandshakeRemoteTimestamp;

	/// Connection configuration
	ConnectionConfig m_connectionConfig;

	/// Expand the packet number and decrypt a data chunk.
	/// Returns the full 64-bit packet number, or 0 on failure.
	int64 DecryptDataChunk( uint16 nWireSeqNum, int cbPacketSize, const void *pChunk, int cbChunk, void *pDecrypted, uint32 &cbDecrypted, SteamNetworkingMicroseconds usecNow );

	/// Process a decrypted data chunk
	bool ProcessPlainTextDataChunk( int64 nFullSequenceNumber, const void *pDecrypted, uint32 cbDecrypted, int usecTimeSinceLast, SteamNetworkingMicroseconds usecNow );

	/// Called when we receive an (end-to-end) packet with a sequence number
	bool RecvNonDataSequencedPacket( int64 nPktNum, SteamNetworkingMicroseconds usecNow );

	// Called from SNP to update transmit/receive speeds
	void UpdateSpeeds( int nTXSpeed, int nRXSpeed );

	/// Called when the async process to request a cert has failed.
	void CertRequestFailed( ESteamNetConnectionEnd nConnectionEndReason, const char *pszMsg );
	bool BHasLocalCert() const { return m_msgSignedCertLocal.has_cert(); }
	void InitLocalCrypto( const CMsgSteamDatagramCertificateSigned &msgSignedCert, const CECSigningPrivateKey &keyPrivate, bool bCertHasIdentity );
	void InterfaceGotCert();

	void SNP_PopulateP2PSessionStateStats( P2PSessionState_t &info ) const;
	bool SNP_BHasAnyBufferedRecvData() const
	{
		return !m_receiverState.m_bufReliableStream.empty();
	}
	bool SNP_BHasAnyUnackedSentReliableData() const
	{
		return m_senderState.m_cbPendingReliable > 0 || m_senderState.m_cbSentUnackedReliable > 0;
	}

	/// Return true if we have any reason to send a packet.  This doesn't mean we have the bandwidth
	/// to send it now, it just means we would like to send something ASAP
	inline bool SNP_WantsToSendPacket() const
	{
		return m_receiverState.TimeWhenFlushAcks() < INT64_MAX || SNP_TimeWhenWantToSendNextPacket() < INT64_MAX;
	}

	/// Called by SNP pacing layer, when it has some data to send and there is bandwidth available.
	/// The derived class should setup a context, reserving the space it needs, and then call SNP_SendPacket.
	/// Returns true if a packet was sent successfuly, false if there was a problem.
	virtual bool SendDataPacket( SteamNetworkingMicroseconds usecNow ) = 0;

	/// Send a data packet now, even if we don't have the bandwidth available.  Returns true if a packet was
	/// sent successfully, false if there was a problem.  This will call SendEncryptedDataChunk to do the work
	bool SNP_SendPacket( SendPacketContext_t &ctx );

	// Steam memory accounting
	#ifdef DBGFLAG_VALIDATE
	static void ValidateStatics( CValidator &validator );
	#endif

protected:
	CSteamNetworkConnectionBase( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );
	virtual ~CSteamNetworkConnectionBase(); // hidden destructor, don't call directly.  Use Destroy()

	/// Initialize connection bookkeeping
	bool BInitConnection( SteamNetworkingMicroseconds usecNow, SteamDatagramErrMsg &errMsg );

	/// Called from BInitConnection, to start obtaining certs, etc
	virtual void InitConnectionCrypto( SteamNetworkingMicroseconds usecNow );

	/// If this is a direct UDP connection, what is the address of the remote host?
	/// FIXME - Should we delete this and move to derived classes?
	/// It's not always meaningful
	netadr_t m_netAdrRemote;

	/// The reason code for why the connection was closed.
	ESteamNetConnectionEnd m_eEndReason;
	ConnectionEndDebugMsg m_szEndDebug;

	/// User data
	int64 m_nUserData;

	/// Name assigned by app (for debugging)
	char m_szAppName[ k_cchSteamNetworkingMaxConnectionDescription ];

	/// More complete debug description (for debugging)
	char m_szDescription[ k_cchSteamNetworkingMaxConnectionDescription ];
	void SetDescription();

	/// Set the connection description.  Should include the connection type and peer address.
	typedef char ConnectionTypeDescription_t[64];
	virtual void GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const = 0;

	// Implements IThinker.
	// Connections do not override this.  Do any periodic work in ThinkConnection()
	virtual void Think( SteamNetworkingMicroseconds usecNow ) OVERRIDE final;

	/// Check state of connection.  Check for timeouts, and schedule time when we
	/// should think next
	void CheckConnectionStateAndSetNextThinkTime( SteamNetworkingMicroseconds usecNow );

	/// Misc periodic processing.
	/// Called from within CheckConnectionStateAndSetNextThinkTime.
	virtual void ThinkConnection( SteamNetworkingMicroseconds usecNow );

	/// Called when a timeout is detected
	void ConnectionTimedOut( SteamNetworkingMicroseconds usecNow );

	/// Called when a timeout is detected.  Derived connection types can inspect this
	/// to provide a more specific explanation.  Base class just uses the generic reason codes.
	virtual void GuessTimeoutReason( ESteamNetConnectionEnd &nReasonCode, ConnectionEndDebugMsg &msg, SteamNetworkingMicroseconds usecNow );

	/// Hook to allow connections to customize message sending.
	/// (E.g. loopback.)
	virtual EResult _APISendMessageToConnection( const void *pData, uint32 cbData, int nSendFlags );

	/// Base class calls this to ask derived class to surround the 
	/// "chunk" with the appropriate framing, and route it to the 
	/// appropriate host.  A "chunk" might contain a mix of reliable 
	/// and unreliable data.  We use the same framing for data 
	/// payloads for all connection types.  Return value is 
	/// the number of bytes written to the network layer, UDP/IP 
	/// header is not included.
	///
	/// pConnectionContext is whatever the connection later passed
	/// to SNP_SendPacket, if the connection initiated the sending
	/// of the packet
	virtual int SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctx ) = 0;

	/// Called when we receive a complete message.  Should allocate a message object and put it into the proper queues
	void ReceivedMessage( const void *pData, int cbData, int64 nMsgNum, SteamNetworkingMicroseconds usecNow );

	/// Called when the state changes
	virtual void ConnectionStateChanged( ESteamNetworkingConnectionState eOldState );

	/// Called to (maybe) post a callback
	virtual void PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState );

	/// Return true if we are currently able to send end-to-end messages.
	virtual bool BCanSendEndToEndConnectRequest() const = 0;
	virtual bool BCanSendEndToEndData() const = 0;
	virtual void SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow ) = 0;
	virtual void SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason ) = 0;
	//virtual bool BSendEndToEndPing( SteamNetworkingMicroseconds usecNow );
	virtual bool BAllowLocalUnsignedCert() const;

	void QueueEndToEndAck( bool bImmediate, SteamNetworkingMicroseconds usecNow )
	{
		if ( bImmediate )
		{
			m_receiverState.QueueFlushAllAcks( 0 );
			SetNextThinkTimeASAP();
		}
		else
		{
			m_receiverState.QueueFlushAllAcks( usecNow + k_usecMaxDataAckDelay );
			EnsureMinThinkTime( m_receiverState.TimeWhenFlushAcks() );
		}
	}

	/// Check if we need to send stats or acks.  If so, return a reason string
	// FIXME - This needs to be refactored.  There is some redundancy in the different
	// transport code that uses it
	const char *NeedToSendEndToEndStatsOrAcks( SteamNetworkingMicroseconds usecNow )
	{
		if ( m_receiverState.TimeWhenFlushAcks() <= usecNow )
			return "SNPFlushAcks";
		return m_statsEndToEnd.NeedToSend( usecNow );
	}


	/// Timestamp when we last sent an end-to-end connection request packet
	SteamNetworkingMicroseconds m_usecWhenSentConnectRequest;

	//
	// Crypto
	//

	void ClearCrypto();
	bool BThinkCryptoReady( SteamNetworkingMicroseconds usecNow );
	void InitLocalCryptoWithUnsignedCert();

	// Remote crypt info
	CMsgSteamDatagramCertificate m_msgCertRemote;
	CMsgSteamDatagramSessionCryptInfo m_msgCryptRemote;

	// Local crypto info for this connection
	CECKeyExchangePrivateKey m_keyExchangePrivateKeyLocal;
	CMsgSteamDatagramSessionCryptInfo m_msgCryptLocal;
	CMsgSteamDatagramSessionCryptInfoSigned m_msgSignedCryptLocal;
	CMsgSteamDatagramCertificateSigned m_msgSignedCertLocal;
	bool m_bCertHasIdentity; // Does the cert contain the identity we will use for this connection?

	// AES keys used in each direction
	bool m_bCryptKeysValid;
	AES_GCM_EncryptContext m_cryptContextSend;
	AES_GCM_DecryptContext m_cryptContextRecv;

	// Initialization vector for AES-GCM.  These are combined with
	// the packet number so that the effective IV is unique per
	// packet.  We use a 96-bit IV, which is what TLS uses (RFC5288),
	// what NIST recommends (https://dl.acm.org/citation.cfm?id=2206251),
	// and what makes GCM the most efficient. 
	AutoWipeFixedSizeBuffer<12> m_cryptIVSend;
	AutoWipeFixedSizeBuffer<12> m_cryptIVRecv;

	// Check the certs, save keys, etc
	bool BRecvCryptoHandshake( const CMsgSteamDatagramCertificateSigned &msgCert, const CMsgSteamDatagramSessionCryptInfoSigned &msgSessionInfo, bool bServer );

	/// Check if the remote cert and crypt info are acceptable.  If not, you should abort
	/// the connection with an appropriate code.  You can assume that a signature was
	/// present and that it has been checked, and if any generic restrictions are present
	/// (steam ID and app) that can be checked by the base class, that they have already
	/// been checked.
	///
	/// If the cert is not signed, we won't call this (why bother?), instead we will just
	/// check if unsigned certs are allowed.
	virtual bool BCheckRemoteCert();

	/// Called when we the remote host presents us with an unsigned cert.
	enum ERemoteUnsignedCert
	{
		k_ERemoteUnsignedCert_Disallow,
		k_ERemoteUnsignedCert_AllowWarn,
		k_ERemoteUnsignedCert_Allow,
	};
	virtual ERemoteUnsignedCert AllowRemoteUnsignedCert();

	//
	// "SNP" - Steam Networking Protocol.  (Sort of audacious to stake out this acronym, don't you think...?)
	//         The layer that does end-to-end reliability and bandwidth estimation
	//

	void SNP_InitializeConnection( SteamNetworkingMicroseconds usecNow );
	EResult SNP_SendMessage( SteamNetworkingMicroseconds usecNow, const void *pData, int cbData, int nSendFlags );
	SteamNetworkingMicroseconds SNP_ThinkSendState( SteamNetworkingMicroseconds usecNow );
	SteamNetworkingMicroseconds SNP_GetNextThinkTime( SteamNetworkingMicroseconds usecNow );
	SteamNetworkingMicroseconds SNP_TimeWhenWantToSendNextPacket() const;
	void SNP_PrepareFeedback( SteamNetworkingMicroseconds usecNow );
	bool SNP_RecvDataChunk( int64 nPktNum, const void *pChunk, int cbChunk, SteamNetworkingMicroseconds usecNow );
	void SNP_ReceiveUnreliableSegment( int64 nMsgNum, int nOffset, const void *pSegmentData, int cbSegmentSize, bool bLastSegmentInMessage, SteamNetworkingMicroseconds usecNow );
	bool SNP_ReceiveReliableSegment( int64 nPktNum, int64 nSegBegin, const uint8 *pSegmentData, int cbSegmentSize, SteamNetworkingMicroseconds usecNow );
	int SNP_ClampSendRate();
	void SNP_PopulateDetailedStats( SteamDatagramLinkStats &info );
	void SNP_PopulateQuickStats( SteamNetworkingQuickConnectionStatus &info, SteamNetworkingMicroseconds usecNow );
	bool SNP_RecordReceivedPktNum( int64 nPktNum, SteamNetworkingMicroseconds usecNow, bool bScheduleAck );
	EResult SNP_FlushMessage( SteamNetworkingMicroseconds usecNow );

	/// Accumulate "tokens" into our bucket base on the current calculated send rate
	void SNP_TokenBucket_Accumulate( SteamNetworkingMicroseconds usecNow );

	/// Mark a packet as dropped
	void SNP_SenderProcessPacketNack( int64 nPktNum, SNPInFlightPacket_t &pkt, const char *pszDebug );

	/// Check in flight packets.  Expire any that need to be, and return the time when the
	/// next one that is not yet expired will be expired.
	SteamNetworkingMicroseconds SNP_SenderCheckInFlightPackets( SteamNetworkingMicroseconds usecNow );

	SSNPSenderState m_senderState;
	SSNPReceiverState m_receiverState;

private:

	void SNP_GatherAckBlocks( SNPAckSerializerHelper &helper, SteamNetworkingMicroseconds usecNow );
	uint8 *SNP_SerializeAckBlocks( const SNPAckSerializerHelper &helper, uint8 *pOut, const uint8 *pOutEnd, SteamNetworkingMicroseconds usecNow );
	uint8 *SNP_SerializeStopWaitingFrame( uint8 *pOut, const uint8 *pOutEnd, SteamNetworkingMicroseconds usecNow );

	void SetState( ESteamNetworkingConnectionState eNewState, SteamNetworkingMicroseconds usecNow );
	ESteamNetworkingConnectionState m_eConnectionState;

	/// Timestamp when we entered the current state.  Used for various
	/// timeouts.
	SteamNetworkingMicroseconds m_usecWhenEnteredConnectionState;

	// !DEBUG! Log of packets we sent.
	#ifdef SNP_ENABLE_PACKETSENDLOG
	struct PacketSendLog
	{
		// State before we sent anything
		SteamNetworkingMicroseconds m_usecTime;
		int m_cbPendingReliable;
		int m_cbPendingUnreliable;
		int m_nPacketGaps;
		float m_fltokens;
		int64 m_nPktNumNextPendingAck;
		SteamNetworkingMicroseconds m_usecNextPendingAckTime;
		int64 m_nMaxPktRecv;
		int64 m_nMinPktNumToSendAcks;

		int m_nAckBlocksNeeded;

		// What we sent
		int m_nAckBlocksSent;
		int64 m_nAckEnd;
		int m_nReliableSegmentsRetry;
		int m_nSegmentsSent;
		int m_cbSent;
	};
	std::vector<PacketSendLog> m_vecSendLog;
	#endif
};

/// Dummy loopback/pipe connection that doesn't actually do any network work.
class CSteamNetworkConnectionPipe : public CSteamNetworkConnectionBase
{
public:

	static bool APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionPipe **pOutConnections, const SteamNetworkingIdentity pIdentity[2] );

	/// The guy who is on the other end.
	CSteamNetworkConnectionPipe *m_pPartner;

	// CSteamNetworkConnectionBase overrides
	virtual bool BCanSendEndToEndConnectRequest() const OVERRIDE;
	virtual bool BCanSendEndToEndData() const OVERRIDE;
	virtual void SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow ) OVERRIDE;
	virtual void SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason ) OVERRIDE;
	virtual EResult APIAcceptConnection() OVERRIDE;
	virtual bool SendDataPacket( SteamNetworkingMicroseconds usecNow ) OVERRIDE;
	virtual int SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctx ) OVERRIDE;
	virtual EResult _APISendMessageToConnection( const void *pData, uint32 cbData, int nSendFlags ) OVERRIDE;
	virtual void ConnectionStateChanged( ESteamNetworkingConnectionState eOldState ) OVERRIDE;
	virtual void PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState ) OVERRIDE;
	virtual ERemoteUnsignedCert AllowRemoteUnsignedCert() OVERRIDE;
	virtual void InitConnectionCrypto( SteamNetworkingMicroseconds usecNow ) OVERRIDE;
	virtual void GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const OVERRIDE;

private:

	// Use CreateSocketPair!
	CSteamNetworkConnectionPipe( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, const SteamNetworkingIdentity &identity );
	virtual ~CSteamNetworkConnectionPipe();

	/// Act like we sent a sequenced packet
	void FakeSendStats( SteamNetworkingMicroseconds usecNow, int cbPktSize );
};

/////////////////////////////////////////////////////////////////////////////
//
// Misc globals
//
/////////////////////////////////////////////////////////////////////////////

extern CUtlHashMap<uint16, CSteamNetworkConnectionBase *, std::equal_to<uint16>, Identity<uint16> > g_mapConnections;
extern CUtlHashMap<int, CSteamNetworkListenSocketBase *, std::equal_to<int>, Identity<int> > g_mapListenSockets;

extern std::string g_sLauncherPartner;

extern bool BCheckGlobalSpamReplyRateLimit( SteamNetworkingMicroseconds usecNow );
extern CSteamNetworkConnectionBase *FindConnectionByLocalID( uint32 nLocalConnectionID );
extern HSteamListenSocket AddListenSocket( CSteamNetworkListenSocketBase *pSock );

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_CONNECTIONS_H
