//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_UDP_H
#define STEAMNETWORKINGSOCKETS_UDP_H
#pragma once

#include "steamnetworkingsockets_connections.h"
#include <steamnetworkingsockets_messages_udp.pb.h>

namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// IP connections
//
/////////////////////////////////////////////////////////////////////////////

/// A connection over raw UDPv4
class CSteamNetworkConnectionIPv4 : public CSteamNetworkConnectionBase
{
public:
	CSteamNetworkConnectionIPv4( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );
	virtual ~CSteamNetworkConnectionIPv4();

	virtual void FreeResources() OVERRIDE;

	/// Convenience wrapper to do the upcast, since we know what sort of
	/// listen socket we were connected on.
	inline CSteamNetworkListenSocketStandard *ListenSocket() const { return assert_cast<CSteamNetworkListenSocketStandard *>( m_pParentListenSocket ); }

	/// Implements CSteamNetworkConnectionBase
	virtual int SendEncryptedDataChunk( const void *pChunk, int cbChunk, SteamNetworkingMicroseconds usecNow, void *pConnectionContext ) OVERRIDE;
	virtual EResult APIAcceptConnection() OVERRIDE;
	virtual bool BCanSendEndToEndConnectRequest() const OVERRIDE;
	virtual bool BCanSendEndToEndData() const OVERRIDE;
	virtual void SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow ) OVERRIDE;
	virtual void SendEndToEndPing( bool bUrgent, SteamNetworkingMicroseconds usecNow ) OVERRIDE;
	virtual void ThinkConnection( SteamNetworkingMicroseconds usecNow ) OVERRIDE;

	/// Initiate a connection
	bool BInitConnect( const netadr_t &netadrRemote, SteamDatagramErrMsg &errMsg );

	/// Accept a connection that has passed the handshake phase
	bool BBeginAccept(
		CSteamNetworkListenSocketStandard *pParent,
		const netadr_t &adrFrom,
		CSharedSocket *pSharedSock,
		CSteamID steamID,
		uint32 unConnectionIDRemote,
		uint32 nPeerProtocolVersion,
		const CMsgSteamDatagramCertificateSigned &msgCert,
		const CMsgSteamDatagramSessionCryptInfoSigned &msgSessionInfo,
		SteamDatagramErrMsg &errMsg
	);

protected:

	/// Interface used to talk to the remote host
	IBoundUDPSocket *m_pSocket;

	// We need to customize our thinking to handle the connection state machine
	virtual void ConnectionStateChanged( ESteamNetworkingConnectionState eOldState ) OVERRIDE;

	static void PacketReceived( const void *pPkt, int cbPkt, const netadr_t &adrFrom, CSteamNetworkConnectionIPv4 *pSelf );

	void Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow );
	void Received_ChallengeReply( const CMsgSteamSockets_UDP_ChallengeReply &msg, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectOK( const CMsgSteamSockets_UDP_ConnectOK &msg, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, SteamNetworkingMicroseconds usecNow );
	void Received_NoConnection( const CMsgSteamSockets_UDP_NoConnection &msg, SteamNetworkingMicroseconds usecNow );
	void Received_ChallengeOrConnectRequest( const char *pszDebugPacketType, uint32 unPacketConnectionID, SteamNetworkingMicroseconds usecNow );

	void SendPaddedMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg );
	void SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg );
	void SendPacket( const void *pkt, int cbPkt );
	void SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal );

	void SendConnectOK( SteamNetworkingMicroseconds usecNow );
	void SendConnectionClosedOrNoConnection();
	void SendNoConnection( uint32 unFromConnectionID, uint32 unToConnectionID );

	/// Process stats message, either inline or standalone
	void RecvStats( const CMsgSteamSockets_UDP_Stats &msgStatsIn, bool bInline, SteamNetworkingMicroseconds usecNow );
	void SendStatsMsg( EStatsReplyRequest eReplyRequested, SteamNetworkingMicroseconds usecNow );
	void TrackSentStats( const CMsgSteamSockets_UDP_Stats &msgStatsOut, bool bInline, SteamNetworkingMicroseconds usecNow );
};

/// A connection over loopback
class CSteamNetworkConnectionlocalhostLoopback : public CSteamNetworkConnectionIPv4
{
public:
	CSteamNetworkConnectionlocalhostLoopback( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );

	/// Setup two connections to be talking to each other
	static bool APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionlocalhostLoopback *pConn[2] );

	/// Base class overrides
	virtual bool BAllowRemoteUnsignedCert() OVERRIDE;
	virtual void PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState ) OVERRIDE;
	virtual void InitConnectionCrypto( SteamNetworkingMicroseconds usecNow ) OVERRIDE;
};

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_UDP_H
