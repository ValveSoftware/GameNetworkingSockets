//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_CONNECTIONS_H
#define STEAMNETWORKINGSOCKETS_CONNECTIONS_H
#pragma once

#include "../steamnetworkingsockets_internal.h"
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include "../steamdatagram_internal.h"
#include <steamnetworkingsockets/steamdatagram_tickets.h>
#endif
#include "../steamnetworking_statsutils.h"
#include <tier1/utllinkedlist.h>
#include <tier1/netadr.h>
#include "steamnetworkingsockets_lowlevel.h"
#include "keypair.h"
#include <tier0/memdbgoff.h>
#include <steamnetworkingsockets_messages.pb.h>
#include <tier0/memdbgon.h>

#include "steamnetworkingsockets_snp.h"

struct SteamNetConnectionStatusChangedCallback_t;
class ISteamNetworkingSocketsSerialized;
class CMsgSteamSockets_UDP_ChallengeRequest;
class CMsgSteamSockets_UDP_ConnectRequest;
class CMsgSteamSockets_UDP_ConnectionClosed;
class CMsgSteamSockets_UDP_Stats;

namespace SteamNetworkingSocketsLib {

const SteamNetworkingMicroseconds k_usecConnectRetryInterval = k_nMillion/2;
const SteamNetworkingMicroseconds k_usecFinWaitTimeout = 5*k_nMillion;

typedef char ConnectionEndDebugMsg[ k_cchSteamNetworkingMaxConnectionCloseReason ];

class CSteamNetworkingSockets;
class CSteamNetworkConnectionBase;
class CSteamNetworkListenSocketStandard;
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

/////////////////////////////////////////////////////////////////////////////
//
// Message storage implementation
//
/////////////////////////////////////////////////////////////////////////////

class CSteamNetworkingMessage : public SteamNetworkingMessage_t
{
public:
	static CSteamNetworkingMessage *New( CSteamNetworkConnectionBase *pParent, uint32 cbSize, int64 nMsgNum, SteamNetworkingMicroseconds usecNow );
	static void Delete( SteamNetworkingMessage_t *piMsg );

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

/////////////////////////////////////////////////////////////////////////////
//
// Abstract base classes
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

	struct ChildConnectionKey_t
	{
		ChildConnectionKey_t() {}
		ChildConnectionKey_t( const CSteamID &steamIDRemote, uint32 unConnectionID ) : m_steamIDRemote( steamIDRemote ), m_unConnectionID( unConnectionID ) {}
		CSteamID m_steamIDRemote;
		uint32 m_unConnectionID = 0;
		uint32 __pad = 0;
		inline bool operator <( const ChildConnectionKey_t &x ) const
		{
			if ( m_steamIDRemote.ConvertToUint64() < x.m_steamIDRemote.ConvertToUint64() ) return true;
			if ( m_steamIDRemote.ConvertToUint64() > x.m_steamIDRemote.ConvertToUint64() ) return false;
			return m_unConnectionID < x.m_unConnectionID;
		}
		inline bool operator ==( const ChildConnectionKey_t &x ) const
		{
			return m_steamIDRemote == x.m_steamIDRemote && m_unConnectionID == x.m_unConnectionID;
		}
	};

	/// List of child connections, ordered by SteamID / connection ID
	CUtlOrderedMap<ChildConnectionKey_t, CSteamNetworkConnectionBase *> m_mapChildConnections;

	/// Linked list of messages received through any connection on this listen socket
	SteamNetworkingMessageQueue m_queueRecvMessages;

	/// Index into the global list
	HSteamListenSocket m_hListenSocketSelf;

	/// IP and port we are bound to, if any
	uint32 m_unIP;
	uint16 m_unPort;

	/// What interface is responsible for this listen socket?
	CSteamNetworkingSockets *const m_pSteamNetworkingSocketsInterface;

protected:
	CSteamNetworkListenSocketBase( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );
	virtual ~CSteamNetworkListenSocketBase(); // hidden destructor, don't call directly.  Use Destroy()
};

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
	EResult APISendMessageToConnection( const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType );

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
	virtual void APIGetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow ) const;

//
// Accessor
//

	// Get/set user data
	inline int64 GetUserData() const { return m_nUserData; }
	void SetUserData( int64 nUserData );

	// Get/set name
	inline const char *GetName() const { return m_sName.c_str(); }
	void SetName( const char *pszName ) { m_sName = pszName; }

	/// High level state of the connection
	ESteamNetworkingConnectionState GetState() const { return m_eConnectionState; }

	/// Check if the connection is 'connected' from the perspective of the wire protocol.
	/// (The wire protocol doesn't care about local states such as linger)
	bool BStateIsConnectedForWirePurposes() const { return m_eConnectionState == k_ESteamNetworkingConnectionState_Connected || m_eConnectionState == k_ESteamNetworkingConnectionState_Linger; }

	/// Accessor for remote address (if we know it)
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

	/// Who is on the other end?  This might be invalid if we don't know yet.  (E.g. direct IPv4 connections.)
	CSteamID m_steamIDRemote;

	/// Our own SteamID.
	CSteamID m_steamIDLocal;

	/// The listen socket through which we were accepted, if any.
	CSteamNetworkListenSocketBase *m_pParentListenSocket;

	/// Our handle in our parent's m_listAcceptedConnections (if we were accepted on a listen socket)
	int m_hSelfInParentListenSocketMap;

	// Linked list of received messages
	SteamNetworkingMessageQueue m_queueRecvMessages;

	/// The unique 64-bit end-to-end connection ID.  Each side picks 32 bits
	uint32 m_unConnectionIDLocal;
	uint32 m_unConnectionIDRemote;

	/// Track end-to-end stats for this connection.
	LinkStatsTrackerEndToEnd m_statsEndToEnd;

	/// When we accept a connection, they will send us a timestamp we should send back
	/// to them, so that they can estimate the ping
	uint64 m_ulHandshakeRemoteTimestamp;
	SteamNetworkingMicroseconds m_usecWhenReceivedHandshakeRemoteTimestamp;

	/// Derived classes will call this when they receive a packet, after removing
	/// the appropriate transport level framing.
	bool RecvDataChunk( uint16 nWireSeqNum, const void *pChunk, int cbChunk, int cbPacketSize, int usecTimeSinceLast, SteamNetworkingMicroseconds usecNow );

	/// Called when we receive an (end-to-end) packet with a sequence number
	bool RecvNonDataSequencedPacket( uint16 nWireSeqNum, SteamNetworkingMicroseconds usecNow );

	void GetDebugText( char *pszOut, int nOutCCH );

	// Called from SNP to update transmit/receive speeds
	void UpdateSpeeds( int nTXSpeed, int nRXSpeed );

	// Connection configuration values.
	// 0 means "use global default"
	void SetMinimumRate( int nRate );
	void SetMaximumRate( int nRate );
	inline int GetMinimumRate() const { return m_senderState.m_n_minRate; }
	inline int GetMaximumRate() const { return m_senderState.m_n_maxRate; }


	/// Called when the async process to request a cert has failed.
	void CertRequestFailed( ESteamNetConnectionEnd nConnectionEndReason, const char *pszMsg );
	bool BHasLocalCert() const { return m_msgSignedCertLocal.has_cert(); }
	void InitLocalCrypto( const CMsgSteamDatagramCertificateSigned &msgSignedCert, const CECSigningPrivateKey &keyPrivate );
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
		return m_receiverState.m_usecWhenFlushAck < INT64_MAX || m_senderState.TimeWhenWantToSendNextPacket() < INT64_MAX;
	}

	/// Send a data packet now, even if we don't have the bandwidth available
	int SNP_SendPacket( SteamNetworkingMicroseconds usecNow, int cbMaxEncryptedPayload, void *pConnectionData );

protected:
	CSteamNetworkConnectionBase( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );
	virtual ~CSteamNetworkConnectionBase(); // hidden destructor, don't call directly.  Use Destroy()

	/// Initialize connection bookkeeping
	bool BInitConnection( uint32 nPeerProtocolVersion, SteamNetworkingMicroseconds usecNow, SteamDatagramErrMsg &errMsg );

	/// Called from BInitConnection, to start obtaining certs, etc
	virtual void InitConnectionCrypto( SteamNetworkingMicroseconds usecNow );

	/// The "virtual port" of the connection for connections through the relay network.
	/// Or -1 for raw IPv4 connections.
	//int m_nVirtualPort;

	/// If this is a direct IPv4 connection, what is the address of the remote host?
	netadr_t m_netAdrRemote;

	/// The reason code for why the connection was closed.
	ESteamNetConnectionEnd m_eEndReason;
	ConnectionEndDebugMsg m_szEndDebug;

	/// User data
	int64 m_nUserData;

	/// Name (for debugging)
	std::string m_sName;

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
	virtual EResult _APISendMessageToConnection( const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType );

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
	virtual int SendEncryptedDataChunk( const void *pChunk, int cbChunk, SteamNetworkingMicroseconds usecNow, void *pConnectionContext ) = 0;

	/// Called when we receive a complete message.  Should allocate a message object and put it into the proper queues
	virtual void ReceivedMessage( const void *pData, int cbData, int64 nMsgNum, SteamNetworkingMicroseconds usecNow );

	/// Called when the state changes
	virtual void ConnectionStateChanged( ESteamNetworkingConnectionState eOldState );

	/// Called to (maybe) post a callback
	virtual void PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState );

	/// Return true if we are currently able to send end-to-end messages.
	virtual bool BCanSendEndToEndConnectRequest() const = 0;
	virtual bool BCanSendEndToEndData() const = 0;
	virtual void SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow ) = 0;
	virtual void SendEndToEndPing( bool bUrgent, SteamNetworkingMicroseconds usecNow ) = 0;
	//virtual bool BSendEndToEndPing( SteamNetworkingMicroseconds usecNow );
	virtual bool BAllowLocalUnsignedCert() const;

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

	// AES keys and used in each direction
	bool m_bCryptKeysValid;
	AutoWipeFixedSizeBuffer<32> m_cryptKeySend;
	AutoWipeFixedSizeBuffer<32> m_cryptKeyRecv;

	// AES "initialization vector".  These are combined with the packet number
	AutoWipeFixedSizeBuffer<16> m_cryptIVSend;
	AutoWipeFixedSizeBuffer<16> m_cryptIVRecv;

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

	/// Called when we the remote host presents us with an unsigned cert.  Return true if this
	/// is OK, false if this is not allowed.
	virtual bool BAllowRemoteUnsignedCert();


	//
	// "SNP" - Steam Networking Protocol.  (Sort of audacious to stake out this acronym, don't you think...?)
	//         The layer that does end-to-end reliability and bandwidth estimation
	//

	void SNP_InitializeConnection( SteamNetworkingMicroseconds usecNow );
	EResult SNP_SendMessage( SteamNetworkingMicroseconds usecNow, const void *pData, int cbData, ESteamNetworkingSendType eSendType );
	SteamNetworkingMicroseconds SNP_ThinkSendState( SteamNetworkingMicroseconds usecNow );
	SteamNetworkingMicroseconds SNP_GetNextThinkTime( SteamNetworkingMicroseconds usecNow );
	void SNP_PrepareFeedback( SteamNetworkingMicroseconds usecNow );
	bool SNP_RecvDataChunk( int64 nPktNum, const void *pChunk, int cbChunk, int cbPacketSize, SteamNetworkingMicroseconds usecNow );
	void SNP_ReceiveUnreliableSegment( int64 nMsgNum, int nOffset, const void *pSegmentData, int cbSegmentSize, bool bLastSegmentInMessage, SteamNetworkingMicroseconds usecNow );
	bool SNP_ReceiveReliableSegment( int64 nPktNum, int64 nSegBegin, const uint8 *pSegmentData, int cbSegmentSize, SteamNetworkingMicroseconds usecNow );
	//void SNP_MoveSentToSend( SteamNetworkingMicroseconds usecNow );
	//void SNP_CheckForReliable( SteamNetworkingMicroseconds usecNow );
	void SNP_UpdateX( SteamNetworkingMicroseconds usecNow );
	std::string SNP_GetDebugText();
	void SNP_PopulateDetailedStats( SteamDatagramLinkStats &info ) const;
	void SNP_PopulateQuickStats( SteamNetworkingQuickConnectionStatus &info, SteamNetworkingMicroseconds usecNow );
	//bool SNP_UpdateIMean( uint16 unSeqNum, SteamNetworkingMicroseconds usecNow );
	//bool SNP_AddLossEvent( uint16 unSeqNum, SteamNetworkingMicroseconds usecNow );
	bool SNP_CalcIMean( SteamNetworkingMicroseconds usecNow );
	void SNP_NoFeedbackTimer( SteamNetworkingMicroseconds usecNow );
	//int SNP_CheckForLoss( uint16 unSeqNum, SteamNetworkingMicroseconds usecNow );
	bool SNP_RecordReceivedPktNum( int64 nPktNum, SteamNetworkingMicroseconds usecNow );
	EResult SNP_FlushMessage( SteamNetworkingMicroseconds usecNow );

	/// Mark a packet as dropped
	void SNP_SenderProcessPacketNack( int64 nPktNum, SNPInFlightPacket_t &pkt, const char *pszDebug );

	/// Check in flight packets.  Expire any that need to be, and return the time when the
	/// next one that is not yet expired will be expired.
	SteamNetworkingMicroseconds SNP_SenderCheckInFlightPackets( SteamNetworkingMicroseconds usecNow );

	int GetEffectiveMinRate() const;
	int GetEffectiveMaxRate() const;

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
};

/////////////////////////////////////////////////////////////////////////////
//
// Standard listen socket, is used for both IPv4 connections and
// relayed P2P connections
//
/////////////////////////////////////////////////////////////////////////////

class CSteamNetworkListenSocketStandard : public CSteamNetworkListenSocketBase
{
public:
	CSteamNetworkListenSocketStandard( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );
	virtual ~CSteamNetworkListenSocketStandard();

	/// Setup
	bool BInit( int nSteamConnectVirtualPort, uint32 nIP, uint16 nPort, SteamDatagramErrMsg &errMsg );

private:

	/// The socket we are bound to, if any, for raw IP V4 connections.  We own this socket.
	/// Any connections accepted through us become clients of this shared socket.
	CSharedSocket *m_pSockIPV4Connections;

	/// SDR client used to accept relayed P2P connections
	// FIXME - for now, we will always do the very first messages through the Steam backend.
	// CSDRClient * m_pP2PSDRClient

	/// The "virtual port" of the server for relay connections, or -1 if this functionality
	/// isn't supported.
	int m_nSteamConnectVirtualPort;

	/// Secret used to generate challenges
	uint8_t m_argbChallengeSecret[ 16 ];

	/// Generate a challenge
	uint64 GenerateChallenge( uint16 nTime, uint32 nIP ) const;

	// Callback to handle a packet on the raw IPv4 socket.
	static void ReceivedIPv4FromUnknownHost( const void *pPkt, int cbPkt, const netadr_t &adrFrom, CSteamNetworkListenSocketStandard *pSock );

	// Process packets from a source address that does not already correspond to a session
	void ReceivedIPv4_ChallengeRequest( const CMsgSteamSockets_UDP_ChallengeRequest &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow );
	void ReceivedIPv4_ConnectRequest( const CMsgSteamSockets_UDP_ConnectRequest &msg, const netadr_t &adrFrom, int cbPkt, SteamNetworkingMicroseconds usecNow );
	void ReceivedIPv4_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow );
	void SendMsgIPv4( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t &adrTo );
	void SendPaddedMsgIPv4( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t adrTo );
};

/// Dummy loopback/pipe connection that doesn't actually do any network work.
class CSteamNetworkConnectionPipe : public CSteamNetworkConnectionBase
{
public:

	static bool APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionPipe **pOutConnections );

	/// The guy who is on the other end.
	CSteamNetworkConnectionPipe *m_pPartner;

	// CSteamNetworkConnectionBase overrides
	virtual bool BCanSendEndToEndConnectRequest() const OVERRIDE;
	virtual bool BCanSendEndToEndData() const OVERRIDE;
	virtual void SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow ) OVERRIDE;
	virtual void SendEndToEndPing( bool bUrgent, SteamNetworkingMicroseconds usecNow ) OVERRIDE;
	virtual EResult APIAcceptConnection() OVERRIDE;
	virtual int SendEncryptedDataChunk( const void *pChunk, int cbChunk, SteamNetworkingMicroseconds usecNow, void *pConnectionContext ) OVERRIDE;
	virtual EResult _APISendMessageToConnection( const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType ) OVERRIDE;
	virtual void ConnectionStateChanged( ESteamNetworkingConnectionState eOldState ) OVERRIDE;
	virtual void PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState ) OVERRIDE;
	virtual bool BAllowRemoteUnsignedCert() OVERRIDE;
	virtual void InitConnectionCrypto( SteamNetworkingMicroseconds usecNow ) OVERRIDE;


private:

	// Use CreateSocketPair!
	CSteamNetworkConnectionPipe( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );
	virtual ~CSteamNetworkConnectionPipe();

	/// Act like we sent a sequenced packet
	void FakeSendStats( SteamNetworkingMicroseconds usecNow, int cbPktSize );
};

/////////////////////////////////////////////////////////////////////////////
//
// Misc globals
//
/////////////////////////////////////////////////////////////////////////////

extern CUtlLinkedList<CSteamNetworkConnectionBase *> g_listConnections;
extern CUtlLinkedList<CSteamNetworkListenSocketBase *> g_listListenSockets;

extern CUtlLinkedList<SteamNetConnectionStatusChangedCallback_t> g_listPendingConnectionStatusChangedCallbacks;

extern int g_iPartnerMask;

extern bool BCheckGlobalSpamReplyRateLimit( SteamNetworkingMicroseconds usecNow );
extern CSteamNetworkConnectionBase *FindConnectionByLocalID( uint32 nLocalConnectionID );

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_CONNECTIONS_H
