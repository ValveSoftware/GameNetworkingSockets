//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_UDP_H
#define STEAMNETWORKINGSOCKETS_UDP_H
#pragma once

#include "steamnetworkingsockets_connections.h"
#include <steamnetworkingsockets_messages_udp.pb.h>

namespace SteamNetworkingSocketsLib {

class CConnectionTransportUDPBase;

#pragma pack( push, 1 )

const int k_cbSteamNetworkingMinPaddedPacketSize = 512;

/// A protobuf-encoded message that is padded to ensure a minimum length
struct UDPPaddedMessageHdr
{
	uint8 m_nMsgID;
	uint16 m_nMsgLength;
};

struct UDPDataMsgHdr
{
	enum
	{
		kFlag_ProtobufBlob  = 0x01, // Protobuf-encoded message is inline (CMsgSteamSockets_UDP_Stats)
	};

	uint8 m_unMsgFlags;
	uint32 m_unToConnectionID; // Recipient's portion of the connection ID
	uint16 m_unSeqNum;

	// [optional, if flags&kFlag_ProtobufBlob]  varint-encoded protobuf blob size, followed by blob
	// Data frame(s)
	// End of packet
};
#pragma pack( pop )

template<>
inline uint32 StatsMsgImpliedFlags<CMsgSteamSockets_UDP_Stats>( const CMsgSteamSockets_UDP_Stats &msg )
{
	return msg.has_stats() ? msg.ACK_REQUEST_E2E : 0;
}

struct UDPSendPacketContext_t : SendPacketContext<CMsgSteamSockets_UDP_Stats>
{
	inline explicit UDPSendPacketContext_t( SteamNetworkingMicroseconds usecNow, const char *pszReason ) : SendPacketContext<CMsgSteamSockets_UDP_Stats>( usecNow, pszReason ) {}
	int m_nStatsNeed;

	void Populate( size_t cbHdrtReserve, EStatsReplyRequest eReplyRequested, CConnectionTransportUDPBase *pTransport );

	void Trim( int cbHdrOutSpaceRemaining );
};

struct UDPRecvPacketContext_t : RecvPacketContext_t
{
	CMsgSteamSockets_UDP_Stats *m_pStatsIn;
};

extern std::string DescribeStatsContents( const CMsgSteamSockets_UDP_Stats &msg );
extern bool BCheckRateLimitReportBadPacket( SteamNetworkingMicroseconds usecNow );
extern void ReallyReportBadUDPPacket( const char *pszFrom, const char *pszMsgType, const char *pszFmt, ... );

#define ReportBadUDPPacketFrom( pszFrom, pszMsgType, /* fmt */ ... ) \
	( BCheckRateLimitReportBadPacket( usecNow ) ? ReallyReportBadUDPPacket( pszFrom, pszMsgType, __VA_ARGS__ ) : (void)0 )

#define ReportBadUDPPacketFromConnectionPeer( pszMsgType, /* fmt */ ... ) \
	ReportBadUDPPacketFrom( ConnectionDescription(), pszMsgType, __VA_ARGS__ )

extern bool IsRouteToAddressProbablyLocal( netadr_t addr );

/////////////////////////////////////////////////////////////////////////////
//
// Listen socket used for direct IP connectivity
//
/////////////////////////////////////////////////////////////////////////////

class CSteamNetworkListenSocketDirectUDP : public CSteamNetworkListenSocketBase
{
public:
	CSteamNetworkListenSocketDirectUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );
	virtual bool APIGetAddress( SteamNetworkingIPAddr *pAddress ) override;

	/// Setup
	bool BInit( const SteamNetworkingIPAddr &localAddr, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg );

private:
	virtual ~CSteamNetworkListenSocketDirectUDP(); // hidden destructor, don't call directly.  Use Destroy()

	/// The socket we are bound to.  We own this socket.
	/// Any connections accepted through us become clients of this shared socket.
	CSharedSocket *m_pSock;

	/// Secret used to generate challenges
	uint8_t m_argbChallengeSecret[ 16 ];

	/// Generate a challenge
	uint64 GenerateChallenge( uint16 nTime, const netadr_t &adr ) const;

	// Callback to handle a packet when it doesn't match
	// any known address
	static void ReceivedFromUnknownHost( const void *pPkt, int cbPkt, const netadr_t &adrFrom, CSteamNetworkListenSocketDirectUDP *pSock );

	// Process packets from a source address that does not already correspond to a session
	void Received_ChallengeRequest( const CMsgSteamSockets_UDP_ChallengeRequest &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectRequest( const CMsgSteamSockets_UDP_ConnectRequest &msg, const netadr_t &adrFrom, int cbPkt, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow );
	void SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t &adrTo );
	void SendPaddedMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t adrTo );
};

/////////////////////////////////////////////////////////////////////////////
//
// IP connections
//
/////////////////////////////////////////////////////////////////////////////

class CSteamNetworkConnectionUDP;

/// Base class for transports that (might) end up sending packets
/// directly on the wire.
class CConnectionTransportUDPBase : public CConnectionTransport
{
public:
	CConnectionTransportUDPBase( CSteamNetworkConnectionBase &connection );

	// Implements CSteamNetworkConnectionTransport
	virtual bool SendDataPacket( SteamNetworkingMicroseconds usecNow ) override;
	virtual int SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctx ) override;
	virtual void SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason ) override;

protected:
	void Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, SteamNetworkingMicroseconds usecNow );
	void Received_NoConnection( const CMsgSteamSockets_UDP_NoConnection &msg, SteamNetworkingMicroseconds usecNow );

	void SendPaddedMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg );
	void SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg );
	void SendConnectionClosedOrNoConnection();
	void SendNoConnection( uint32 unFromConnectionID, uint32 unToConnectionID );

	virtual bool SendPacket( const void *pkt, int cbPkt ) = 0;
	virtual bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal ) = 0;

	/// Process stats message, either inline or standalone
	void RecvStats( const CMsgSteamSockets_UDP_Stats &msgStatsIn, SteamNetworkingMicroseconds usecNow );
	virtual void TrackSentStats( UDPSendPacketContext_t &ctx );

	virtual void RecvValidUDPDataPacket( UDPRecvPacketContext_t &ctx );
};


/// Actual, ordinary UDP transport
class CConnectionTransportUDP final : public CConnectionTransportUDPBase
{
public:
	CConnectionTransportUDP( CSteamNetworkConnectionUDP &connection );

	// Implements CSteamNetworkConnectionTransport
	virtual void TransportFreeResources() override;
	virtual bool BCanSendEndToEndConnectRequest() const override;
	virtual bool BCanSendEndToEndData() const override;
	virtual void SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow ) override;
	virtual void TransportConnectionStateChanged( ESteamNetworkingConnectionState eOldState ) override;
	virtual void TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const override;

	/// Interface used to talk to the remote host
	IBoundUDPSocket *m_pSocket;

	bool BConnect( const netadr_t &netadrRemote, SteamDatagramErrMsg &errMsg );
	bool BAccept( CSharedSocket *pSharedSock, const netadr_t &netadrRemote, SteamDatagramErrMsg &errMsg );

	void SendConnectOK( SteamNetworkingMicroseconds usecNow );

	static bool CreateLoopbackPair( CConnectionTransportUDP *pTransport[2] );

protected:
	virtual ~CConnectionTransportUDP(); // Don't call operator delete directly

	static void PacketReceived( const void *pPkt, int cbPkt, const netadr_t &adrFrom, CConnectionTransportUDP *pSelf );

	void Received_ChallengeReply( const CMsgSteamSockets_UDP_ChallengeReply &msg, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectOK( const CMsgSteamSockets_UDP_ConnectOK &msg, SteamNetworkingMicroseconds usecNow );
	void Received_ChallengeOrConnectRequest( const char *pszDebugPacketType, uint32 unPacketConnectionID, SteamNetworkingMicroseconds usecNow );

	// Implements CConnectionTransportUDPBase
	virtual bool SendPacket( const void *pkt, int cbPkt ) override;
	virtual bool SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal ) override;
};

/// A connection over ordinary UDP
class CSteamNetworkConnectionUDP : public CSteamNetworkConnectionBase
{
public:
	CSteamNetworkConnectionUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );

	/// Convenience wrapper to do the upcast, since we know what sort of
	/// listen socket we were connected on.
	inline CSteamNetworkListenSocketDirectUDP *ListenSocket() const { return assert_cast<CSteamNetworkListenSocketDirectUDP *>( m_pParentListenSocket ); }
	inline CConnectionTransportUDP *Transport() const { return assert_cast<CConnectionTransportUDP *>( m_pTransport ); }

	/// Implements CSteamNetworkConnectionBase
	virtual EResult AcceptConnection( SteamNetworkingMicroseconds usecNow ) override;
	virtual void GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const override;
	virtual EUnsignedCert AllowRemoteUnsignedCert() override;
	virtual EUnsignedCert AllowLocalUnsignedCert() override;

	/// Initiate a connection
	bool BInitConnect( const SteamNetworkingIPAddr &addressRemote, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg );

	/// Accept a connection that has passed the handshake phase
	bool BBeginAccept(
		CSteamNetworkListenSocketDirectUDP *pParent,
		const netadr_t &adrFrom,
		CSharedSocket *pSharedSock,
		const SteamNetworkingIdentity &identityRemote,
		uint32 unConnectionIDRemote,
		const CMsgSteamDatagramCertificateSigned &msgCert,
		const CMsgSteamDatagramSessionCryptInfoSigned &msgSessionInfo,
		SteamDatagramErrMsg &errMsg
	);
protected:
	virtual ~CSteamNetworkConnectionUDP(); // hidden destructor, don't call directly.  Use ConnectionDestroySelfNow
};

/// A connection over loopback
class CSteamNetworkConnectionlocalhostLoopback final : public CSteamNetworkConnectionUDP
{
public:
	CSteamNetworkConnectionlocalhostLoopback( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, const SteamNetworkingIdentity &identity );

	/// Setup two connections to be talking to each other
	static bool APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionlocalhostLoopback *pConn[2], const SteamNetworkingIdentity pIdentity[2] );

	/// Base class overrides
	virtual EUnsignedCert AllowRemoteUnsignedCert() override;
	virtual EUnsignedCert AllowLocalUnsignedCert() override;
};

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_UDP_H
