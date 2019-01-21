//====== Copyright Valve Corporation, All rights reserved. ====================

#include "csteamnetworkingsockets.h"
#include <steam/steamnetworkingsockets.h>
#include "steamnetworkingsockets_lowlevel.h"
#include "steamnetworkingsockets_connections.h"
#include "steamnetworkingsockets_udp.h"
#include "crypto.h"

#define DEFINE_CONFIG // instance the variables here
#include "steamnetworkingconfig.h"

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
#include <steam/steamnetworkingsockets.h>
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ISteamNetworkingSockets::~ISteamNetworkingSockets() {}

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// Configuration Variables
//
/////////////////////////////////////////////////////////////////////////////

struct SConfigurationValueEntry
{
	ESteamNetworkingConfigurationValue eValue;
	const char *pName;
	int32 *pVar;
};

static SConfigurationValueEntry sConfigurationValueEntryList[] =
{
	{ k_ESteamNetworkingConfigurationValue_FakeMessageLoss_Send,                       "FakeMessageLoss_Send",                       &steamdatagram_fakemessageloss_send },
	{ k_ESteamNetworkingConfigurationValue_FakeMessageLoss_Recv,                       "FakeMessageLoss_Recv",                       &steamdatagram_fakemessageloss_recv },
	{ k_ESteamNetworkingConfigurationValue_FakePacketLoss_Send,                        "FakePacketLoss_Send",                        &steamdatagram_fakepacketloss_send },
	{ k_ESteamNetworkingConfigurationValue_FakePacketLoss_Recv,                        "FakePacketLoss_Recv",                        &steamdatagram_fakepacketloss_recv },
	{ k_ESteamNetworkingConfigurationValue_FakePacketLag_Send,                         "FakePacketLag_Send",                         &steamdatagram_fakepacketlag_send },
	{ k_ESteamNetworkingConfigurationValue_FakePacketLag_Recv,                         "FakePacketLag_Recv",                         &steamdatagram_fakepacketlag_recv },
	{ k_ESteamNetworkingConfigurationValue_FakePacketReorder_Send,                     "FakePacketReorder_Send",                     &steamdatagram_fakepacketreorder_send },
	{ k_ESteamNetworkingConfigurationValue_FakePacketReorder_Recv,                     "FakePacketReorder_Recv",                     &steamdatagram_fakepacketreorder_recv },
	{ k_ESteamNetworkingConfigurationValue_FakePacketReorder_Time,                     "FakePacketReorder_Time",                     &steamdatagram_fakepacketreorder_time },
	{ k_ESteamNetworkingConfigurationValue_FakePacketDup_Send,                         "FakePacketDup_Send",                         &steamdatagram_fakepacketdup_send },
	{ k_ESteamNetworkingConfigurationValue_FakePacketDup_Recv,                         "FakePacketDup_Recv",                         &steamdatagram_fakepacketdup_recv },
	{ k_ESteamNetworkingConfigurationValue_FakePacketDup_TimeMax,                      "FakePacketDup_Time",                         &steamdatagram_fakepacketdup_timemax },
	{ k_ESteamNetworkingConfigurationValue_SendBufferSize,                             "SendBufferSize",                             &steamdatagram_snp_send_buffer_size },
	{ k_ESteamNetworkingConfigurationValue_MaxRate,                                    "MaxRate",                                    &steamdatagram_snp_max_rate },
	{ k_ESteamNetworkingConfigurationValue_MinRate,                                    "MinRate",                                    &steamdatagram_snp_min_rate },
	{ k_ESteamNetworkingConfigurationValue_Nagle_Time,                                 "Nagle_Time",                                 &steamdatagram_snp_nagle_time },
	{ k_ESteamNetworkingConfigurationValue_LogLevel_AckRTT,                            "Log_AckRTT",                                 &steamdatagram_snp_log_ackrtt },
	{ k_ESteamNetworkingConfigurationValue_LogLevel_Packet,                            "Log_Packet",                                 &steamdatagram_snp_log_packet },
	{ k_ESteamNetworkingConfigurationValue_LogLevel_Message,                           "Log_Message",                                &steamdatagram_snp_log_message },
	{ k_ESteamNetworkingConfigurationValue_LogLevel_PacketGaps,                        "Log_PacketGaps",                             &steamdatagram_snp_log_packetgaps },
	{ k_ESteamNetworkingConfigurationValue_LogLevel_P2PRendezvous,                     "Log_p2prendezvous",                          &steamdatagram_snp_log_p2prendezvous },
	{ k_ESteamNetworkingConfigurationValue_LogLevel_RelayPings,                        "Log_RelayPings",                             &steamdatagram_snp_log_relaypings },
	{ k_ESteamNetworkingConfigurationValue_ClientConsecutitivePingTimeoutsFailInitial, "ClientConsecutitivePingTimeoutsFailInitial", &steamdatagram_client_consecutitive_ping_timeouts_fail_initial },
	{ k_ESteamNetworkingConfigurationValue_ClientConsecutitivePingTimeoutsFail,        "ClientConsecutitivePingTimeoutsFail",        &steamdatagram_client_consecutitive_ping_timeouts_fail },
	{ k_ESteamNetworkingConfigurationValue_ClientMinPingsBeforePingAccurate,           "ClientMinPingsBeforePingAccurate",           &steamdatagram_client_min_pings_before_ping_accurate },
	{ k_ESteamNetworkingConfigurationValue_ClientSingleSocket,                         "ClientSingleSocket",                         &steamdatagram_client_single_socket },
	{ k_ESteamNetworkingConfigurationValue_IP_Allow_Without_Auth,                      "IpAllowWithoutAuth",                         &steamdatagram_ip_allow_connections_without_auth },
	{ k_ESteamNetworkingConfigurationValue_Timeout_Seconds_Initial,                    "TimeoutSecondsInitial",                      &steamdatagram_timeout_seconds_initial },
	{ k_ESteamNetworkingConfigurationValue_Timeout_Seconds_Connected,                  "TimeoutSecondsConnected",                    &steamdatagram_timeout_seconds_connected },
};
COMPILE_TIME_ASSERT( sizeof( sConfigurationValueEntryList ) / sizeof( SConfigurationValueEntry ) == k_ESteamNetworkingConfigurationValue_Count );

struct SConfigurationStringEntry
{
	ESteamNetworkingConfigurationString eString;
	const char *pName;
	std::string *pVar;
};

static SConfigurationStringEntry sConfigurationStringEntryList[] =
{
	{ k_ESteamNetworkingConfigurationString_ClientForceRelayCluster,  "ClientForceRelayCluster",  &steamdatagram_client_force_relay_cluster },
	{ k_ESteamNetworkingConfigurationString_ClientDebugTicketAddress, "ClientDebugTicketAddress", &steamdatagram_client_debugticket_address }, 
	{ k_ESteamNetworkingConfigurationString_ClientForceProxyAddr,     "ClientForceProxyAddr",     &steamdatagram_client_forceproxyaddr       }, 
};
COMPILE_TIME_ASSERT( sizeof( sConfigurationStringEntryList ) / sizeof( SConfigurationStringEntry ) == k_ESteamNetworkingConfigurationString_Count );

/////////////////////////////////////////////////////////////////////////////
//
// Table of active sockets
//
/////////////////////////////////////////////////////////////////////////////

CUtlHashMap<uint16, CSteamNetworkConnectionBase *, std::equal_to<uint16>, Identity<uint16> > g_mapConnections;
CUtlHashMap<int, CSteamNetworkListenSocketBase *, std::equal_to<int>, Identity<int> > g_mapListenSockets;

static bool BConnectionStateExistsToAPI( ESteamNetworkingConnectionState eState )
{
	switch ( eState )
	{
		default:
			Assert( false );
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_Linger:
			return false;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			return true;
	}

}

static CSteamNetworkConnectionBase *GetConnectionByHandle( HSteamNetConnection sock )
{
	if ( sock == 0 )
		return nullptr;
	int idx = g_mapConnections.Find( uint16( sock ) );
	if ( idx == g_mapConnections.InvalidIndex() )
		return nullptr;
	CSteamNetworkConnectionBase *pResult = g_mapConnections[ idx ];
	if ( !pResult || uint16( pResult->m_hConnectionSelf ) != uint16( sock ) )
	{
		AssertMsg( false, "g_mapConnections corruption!" );
		return nullptr;
	}
	if ( pResult->m_hConnectionSelf != sock )
		return nullptr;
	if ( !BConnectionStateExistsToAPI( pResult->GetState() ) )
		return nullptr;
	return pResult;
}

CSteamNetworkConnectionBase *FindConnectionByLocalID( uint32 nLocalConnectionID )
{
	// We use the wire connection ID as the API handle, so these two operations
	// are currently the same.
	return GetConnectionByHandle( HSteamNetConnection( nLocalConnectionID ) );
}

static CSteamNetworkListenSocketBase *GetListenSockedByHandle( HSteamListenSocket sock )
{
	if ( sock == 0 )
		return nullptr;
	int idx = sock & 0xffff;
	if ( !g_mapListenSockets.IsValidIndex( idx ) )
		return nullptr;
	CSteamNetworkListenSocketBase *pResult = g_mapListenSockets[ idx ];
	Assert( pResult && pResult->m_hListenSocketSelf == sock );
	return pResult;
}

HSteamListenSocket AddListenSocket( CSteamNetworkListenSocketBase *pSock )
{
	// We actually don't do map "lookups".  We assume the number of listen sockets
	// is going to be reasonably small.
	static int s_nDummy;
	++s_nDummy;
	int idx = g_mapListenSockets.Insert( s_nDummy, pSock );
	Assert( idx < 0x1000 );

	// Use upper 16 bits as a connection sequence number, so that listen socket handles
	// are not reused within a short time period.
	static uint32 s_nUpperBits = 0;
	s_nUpperBits += 0x10000;
	if ( s_nUpperBits == 0 )
		s_nUpperBits = 0x10000;

	// Add it to our table of listen sockets
	pSock->m_hListenSocketSelf = HSteamListenSocket( idx | s_nUpperBits );
	return pSock->m_hListenSocketSelf;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamSocketNetworkingBase
//
/////////////////////////////////////////////////////////////////////////////

int CSteamNetworkingSockets::s_nSteamNetworkingSocketsInitted = 0;

CSteamNetworkingSockets::CSteamNetworkingSockets()
: m_bHaveLowLevelRef( false )
{}

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
bool CSteamNetworkingSockets::BInitGameNetworkingSockets( const SteamNetworkingIdentity *pIdentity, SteamDatagramErrMsg &errMsg )
{
	AssertMsg( !m_bHaveLowLevelRef, "Initted interface twice?" );

	// Make sure low level socket support is ready
	if ( !BSteamNetworkingSocketsLowLevelAddRef( errMsg ) )
		return false;
	m_bHaveLowLevelRef = true;

	++s_nSteamNetworkingSocketsInitted;

	if ( pIdentity )
		m_identity = *pIdentity;
	else
		CacheIdentity();

	return true;
}
#endif

void CSteamNetworkingSockets::KillConnections()
{

	// Destroy all of my connections
	FOR_EACH_HASHMAP( g_mapConnections, idx )
	{
		CSteamNetworkConnectionBase *pConn = g_mapConnections[idx];
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
		{
			pConn->Destroy();
			Assert( !g_mapConnections.IsValidIndex( idx ) );
		}
	}

	// Destroy all of my listen sockets
	FOR_EACH_HASHMAP( g_mapListenSockets, idx )
	{
		CSteamNetworkListenSocketBase *pSock = g_mapListenSockets[idx];
		if ( pSock->m_pSteamNetworkingSocketsInterface == this )
		{
			DbgVerify( CloseListenSocket( pSock->m_hListenSocketSelf ) );
			Assert( !g_mapListenSockets.IsValidIndex( idx ) );
		}
	}
}

void CSteamNetworkingSockets::KillBase()
{
	KillConnections();

	// Clear identity and crypto stuff.
	// If we are re-initialized, we might get new ones
	m_identity.Clear();
	m_msgSignedCert.Clear();
	m_msgCert.Clear();
	m_keyPrivateKey.Wipe();

	// Mark us as no longer being setup
	if ( m_bHaveLowLevelRef )
	{
		m_bHaveLowLevelRef = false;
		SteamNetworkingSocketsLowLevelDecRef();
	}
}

bool CSteamNetworkingSockets::BHasAnyConnections() const
{
	for ( const auto &item: g_mapConnections )
	{
		if ( item.elem->m_pSteamNetworkingSocketsInterface == this )
			return true;
	}
	return false;
}

bool CSteamNetworkingSockets::BHasAnyListenSockets() const
{
	for ( const auto &item: g_mapListenSockets )
	{
		if ( item.elem->m_pSteamNetworkingSocketsInterface == this )
			return true;
	}
	return false;
}

bool CSteamNetworkingSockets::GetIdentity( SteamNetworkingIdentity *pIdentity )
{
	SteamDatagramTransportLock scopeLock;
	InternalGetIdentity();
	if ( pIdentity )
		*pIdentity = m_identity;
	return !m_identity.IsInvalid();
}

HSteamListenSocket CSteamNetworkingSockets::CreateListenSocketIP( const SteamNetworkingIPAddr &localAddr )
{
	SteamDatagramTransportLock scopeLock;
	SteamDatagramErrMsg errMsg;

	// Might we want a cert?  If so, make sure async process to get one is in
	// progress (or try again if we tried earlier and failed)
	if ( steamdatagram_ip_allow_connections_without_auth == 0 )
	{
		#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
			SpewError( "Need cert authority!" );
			return k_HSteamListenSocket_Invalid;
		#else
			AsyncCertRequest();
		#endif
	}

	CSteamNetworkListenSocketDirectUDP *pSock = new CSteamNetworkListenSocketDirectUDP( this );
	if ( !pSock )
		return k_HSteamListenSocket_Invalid;
	if ( !pSock->BInit( localAddr, errMsg ) )
	{
		SpewError( "Cannot create listen socket.  %s", errMsg );
		delete pSock;
		return k_HSteamListenSocket_Invalid;
	}

	return AddListenSocket( pSock );
}

HSteamNetConnection CSteamNetworkingSockets::ConnectByIPAddress( const SteamNetworkingIPAddr &address )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionUDP *pConn = new CSteamNetworkConnectionUDP( this );
	if ( !pConn )
		return k_HSteamNetConnection_Invalid;
	SteamDatagramErrMsg errMsg;
	if ( !pConn->BInitConnect( address, errMsg ) )
	{
		SpewError( "Cannot create IPv4 connection.  %s", errMsg );
		pConn->FreeResources();
		delete pConn;
		return k_HSteamNetConnection_Invalid;
	}

	return pConn->m_hConnectionSelf;
}


EResult CSteamNetworkingSockets::AcceptConnection( HSteamNetConnection hConn )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return k_EResultInvalidParam;

	// Should only be called for connections accepted on listen socket.
	// (E.g., not connections initiated locally.)
	if ( pConn->m_pParentListenSocket == nullptr )
		return k_EResultInvalidParam;

	// Must be in in state ready to be accepted
	if ( pConn->GetState() != k_ESteamNetworkingConnectionState_Connecting )
		return k_EResultInvalidState;

	// Protocol-specific handling
	return pConn->APIAcceptConnection();
}

bool CSteamNetworkingSockets::CloseConnection( HSteamNetConnection hConn, int nReason, const char *pszDebug, bool bEnableLinger )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return false;

	// Close it
	pConn->APICloseConnection( nReason, pszDebug, bEnableLinger );
	return true;
}

bool CSteamNetworkingSockets::CloseListenSocket( HSteamListenSocket hSocket )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkListenSocketBase *pSock = GetListenSockedByHandle( hSocket );
	if ( !pSock )
		return false;
	int idx = hSocket & 0xffff;
	Assert( g_mapListenSockets.IsValidIndex( idx ) && g_mapListenSockets[ idx ] == pSock );

	// Delete the socket itself
	// NOTE: If you change this, look at CSteamSocketNetworking::Kill()!
	pSock->Destroy();

	// Remove from our data structures
	g_mapListenSockets[ idx ] = nullptr; // Just for grins
	g_mapListenSockets.RemoveAt( idx );
	return true;
}

bool CSteamNetworkingSockets::SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hPeer );
	if ( !pConn )
		return false;
	pConn->SetUserData( nUserData );
	return true;
}

int64 CSteamNetworkingSockets::GetConnectionUserData( HSteamNetConnection hPeer )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hPeer );
	if ( !pConn )
		return -1;
	return pConn->GetUserData();
}

void CSteamNetworkingSockets::SetConnectionName( HSteamNetConnection hPeer, const char *pszName )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hPeer );
	if ( !pConn )
		return;
	pConn->SetAppName( pszName );
}

bool CSteamNetworkingSockets::GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hPeer );
	if ( !pConn )
		return false;
	V_strncpy( pszName, pConn->GetAppName(), nMaxLen );
	return true;
}

EResult CSteamNetworkingSockets::SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return k_EResultInvalidParam;
	return pConn->APISendMessageToConnection( pData, cbData, eSendType );
}

EResult CSteamNetworkingSockets::FlushMessagesOnConnection( HSteamNetConnection hConn )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return k_EResultInvalidParam;
	return pConn->APIFlushMessageOnConnection();
}
	
int CSteamNetworkingSockets::ReceiveMessagesOnConnection( HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return -1;
	return pConn->APIReceiveMessages( ppOutMessages, nMaxMessages );
}

int CSteamNetworkingSockets::ReceiveMessagesOnListenSocket( HSteamListenSocket hSocket, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkListenSocketBase *pSock = GetListenSockedByHandle( hSocket );
	if ( !pSock )
		return -1;
	return pSock->APIReceiveMessages( ppOutMessages, nMaxMessages );
}

bool CSteamNetworkingSockets::GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return false;
	if ( pInfo )
		pConn->PopulateConnectionInfo( *pInfo );
	return true;
}

bool CSteamNetworkingSockets::GetQuickConnectionStatus( HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return false;
	if ( pStats )
		pConn->APIGetQuickConnectionStatus( *pStats );
	return true;
}

int CSteamNetworkingSockets::GetDetailedConnectionStatus( HSteamNetConnection hConn, char *pszBuf, int cbBuf )
{
	SteamNetworkingDetailedConnectionStatus stats;

	// Only hold the lock for as long as we need.
	{
		SteamDatagramTransportLock scopeLock;
		CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
		if ( !pConn )
			return -1;

		pConn->APIGetDetailedConnectionStatus( stats, SteamNetworkingSockets_GetLocalTimestamp() );

	} // Release lock.  We don't need it, and printing can take a while!
	int r = stats.Print( pszBuf, cbBuf );

	/// If just asking for buffer size, pad it a bunch
	/// because connection status can change at any moment.
	if ( r > 0 )
		r += 1024;
	return r;
}

bool CSteamNetworkingSockets::GetListenSocketAddress( HSteamListenSocket hSocket, SteamNetworkingIPAddr *pAddress )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkListenSocketBase *pSock = GetListenSockedByHandle( hSocket );
	if ( !pSock )
		return false;
	return pSock->APIGetAddress( pAddress );
}

bool CSteamNetworkingSockets::CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 )
{
	SteamDatagramTransportLock scopeLock;

	// Assume failure
	*pOutConnection1 = k_HSteamNetConnection_Invalid;
	*pOutConnection2 = k_HSteamNetConnection_Invalid;
	SteamNetworkingIdentity identity[2];
	if ( pIdentity1 )
		identity[0] = *pIdentity1;
	else
		identity[0].SetLocalHost();
	if ( pIdentity2 )
		identity[1] = *pIdentity2;
	else
		identity[1].SetLocalHost();

	// Create network connections?
	if ( bUseNetworkLoopback )
	{
		// Create two connection objects
		CSteamNetworkConnectionlocalhostLoopback *pConn[2];
		if ( !CSteamNetworkConnectionlocalhostLoopback::APICreateSocketPair( this, pConn, identity ) )
			return false;

		// Return their handles
		*pOutConnection1 = pConn[0]->m_hConnectionSelf;
		*pOutConnection2 = pConn[1]->m_hConnectionSelf;
	}
	else
	{
		// Create two connection objects
		CSteamNetworkConnectionPipe *pConn[2];
		if ( !CSteamNetworkConnectionPipe::APICreateSocketPair( this, pConn, identity ) )
			return false;

		// Return their handles
		*pOutConnection1 = pConn[0]->m_hConnectionSelf;
		*pOutConnection2 = pConn[1]->m_hConnectionSelf;
	}
	return true;
}

bool CSteamNetworkingSockets::BCertHasIdentity() const
{
	// We should actually have a cert, otherwise this question cannot be answered
	Assert( m_msgSignedCert.has_cert() );
	Assert( m_msgCert.has_key_data() );
	return m_msgCert.has_identity() || m_msgCert.has_legacy_steam_id();
}


bool CSteamNetworkingSockets::SetCertificate( const void *pCert, int cbCert, void *pPrivateKey, int cbPrivateKey, SteamDatagramErrMsg &errMsg )
{
	m_msgCert.Clear();
	m_msgSignedCert.Clear();
	m_keyPrivateKey.Wipe();

	//
	// Decode the private key
	//
	if ( !m_keyPrivateKey.LoadFromAndWipeBuffer( pPrivateKey, cbPrivateKey ) )
	{
		V_strcpy_safe( errMsg, "Invalid private key" );
		return false;
	}

	//
	// Decode the cert
	//
	uint32 cbCertBody = cbCert;
	const char *pszCertBody = CCrypto::LocatePEMBody( (const char *)pCert, &cbCertBody, "STEAMDATAGRAM CERT" );
	if ( !pszCertBody )
	{
		V_strcpy_safe( errMsg, "Cert isn't a valid PEM-like text block" );
		return false;
	}
	CUtlVector<uint8> buf;
	uint32 cbDecoded = CCrypto::Base64DecodeMaxOutput( cbCertBody );
	buf.SetCount( cbDecoded );
	if ( !CCrypto::Base64Decode( pszCertBody, cbCertBody, buf.Base(), &cbDecoded, false ) )
	{
		V_strcpy_safe( errMsg, "Failed to Base64 decode cert" );
		return false;
	}

	if (
		!m_msgSignedCert.ParseFromArray( buf.Base(), cbDecoded )
		|| !m_msgSignedCert.has_cert()
		|| !m_msgCert.ParseFromString( m_msgSignedCert.cert() )
		|| !m_msgCert.has_time_expiry()
		|| !m_msgCert.has_key_data()
	) {
		V_strcpy_safe( errMsg, "Invalid cert" );
		return false;
	}
	if ( m_msgCert.key_type() != CMsgSteamDatagramCertificate_EKeyType_ED25519 )
	{
		V_strcpy_safe( errMsg, "Invalid cert or unsupported public key type" );
		return false;
	}

	//
	// Make sure that the private key and the cert match!
	//

	CECSigningPublicKey pubKey;
	if ( !pubKey.SetRawDataWithoutWipingInput( m_msgCert.key_data().c_str(), m_msgCert.key_data().length() ) )
	{
		V_strcpy_safe( errMsg, "Invalid public key" );
		return false;
	}
	if ( !m_keyPrivateKey.MatchesPublicKey( pubKey ) )
	{
		V_strcpy_safe( errMsg, "Private key doesn't match public key from cert" );
		return false;
	}

	return true;
}

bool CSteamNetworkingSockets::GetConnectionDebugText( HSteamNetConnection hConn, char *pszOut, int nOutCCH )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return false;
	pConn->GetDebugText( pszOut, nOutCCH );
	return true;
}

int32 CSteamNetworkingSockets::GetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue )
{
	for ( int i = 0; i < V_ARRAYSIZE( sConfigurationValueEntryList ); ++i )
	{
		if ( sConfigurationValueEntryList[ i ].eValue == eConfigValue )
		{
			return *sConfigurationValueEntryList[ i ].pVar;
		}
	}

	return -1;
}

bool CSteamNetworkingSockets::SetConfigurationValue( ESteamNetworkingConfigurationValue eConfigValue, int32 nValue )
{
	for ( int i = 0; i < V_ARRAYSIZE( sConfigurationValueEntryList ); ++i )
	{
		if ( sConfigurationValueEntryList[ i ].eValue == eConfigValue )
		{
			*sConfigurationValueEntryList[ i ].pVar = nValue;
			return true;
		}
	}
	return false;
}

const char *CSteamNetworkingSockets::GetConfigurationValueName( ESteamNetworkingConfigurationValue eConfigValue )
{
	for ( int i = 0; i < V_ARRAYSIZE( sConfigurationValueEntryList ); ++i )
	{
		if ( sConfigurationValueEntryList[ i ].eValue == eConfigValue )
			return sConfigurationValueEntryList[ i ].pName;
	}
	return nullptr;
}

int32 CSteamNetworkingSockets::GetConfigurationString( ESteamNetworkingConfigurationString eConfigString, char *pDest, int32 destSize )
{
	for ( int i = 0; i < V_ARRAYSIZE( sConfigurationStringEntryList ); ++i )
	{
		if ( sConfigurationStringEntryList[ i ].eString == eConfigString )
		{
			int32 len = (int32) sConfigurationStringEntryList[ i ].pVar->size();
			if ( pDest && destSize > 0 )
			{
				V_strncpy( pDest, sConfigurationStringEntryList[ i ].pVar->c_str(), destSize );
			}
			return len;
		}
	}

	return -1;
}

bool CSteamNetworkingSockets::SetConfigurationString( ESteamNetworkingConfigurationString eConfigString, const char *pString )
{
	for ( int i = 0; i < V_ARRAYSIZE( sConfigurationStringEntryList ); ++i )
	{
		if ( sConfigurationStringEntryList[ i ].eString == eConfigString )
		{
			*sConfigurationStringEntryList[ i ].pVar = pString;
			return true;
		}
	}
	return false;
}

const char *CSteamNetworkingSockets::GetConfigurationStringName( ESteamNetworkingConfigurationString eConfigString )
{
	for ( int i = 0; i < V_ARRAYSIZE( sConfigurationStringEntryList ); ++i )
	{
		if ( sConfigurationStringEntryList[ i ].eString == eConfigString )
			return sConfigurationStringEntryList[ i ].pName;
	}
	return nullptr;
}

int32 CSteamNetworkingSockets::GetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return -1;

	switch ( eConfigValue )
	{
	case k_ESteamNetworkingConnectionConfigurationValue_SNP_MaxRate :
		return pConn->GetMaximumRate();

	case k_ESteamNetworkingConnectionConfigurationValue_SNP_MinRate :
		return pConn->GetMinimumRate();
	}
	return -1;
}

bool CSteamNetworkingSockets::SetConnectionConfigurationValue( HSteamNetConnection hConn, ESteamNetworkingConnectionConfigurationValue eConfigValue, int32 nValue )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return false;

	switch ( eConfigValue )
	{
	case k_ESteamNetworkingConnectionConfigurationValue_SNP_MaxRate :
		pConn->SetMaximumRate( nValue );
		return true;

	case k_ESteamNetworkingConnectionConfigurationValue_SNP_MinRate :
		pConn->SetMinimumRate( nValue );
		return true;
	}

	return false;
}

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
void CSteamNetworkingSockets::RunCallbacks( ISteamNetworkingSocketsCallbacks *pCallbacks )
{

	// Only hold lock for a brief period
	std::vector<QueuedCallback> listTemp;
	{
		SteamDatagramTransportLock scopeLock;

		// Swap list with the temp one
		listTemp.swap( m_vecPendingCallbacks );

		// Release the lock
	}

	// Dispatch the callbacks
	for ( QueuedCallback &x: listTemp )
	{
		switch ( x.nCallback )
		{
			case SteamNetConnectionStatusChangedCallback_t::k_iCallback:
				COMPILE_TIME_ASSERT( sizeof(SteamNetConnectionStatusChangedCallback_t) <= sizeof(x.data) );
				pCallbacks->OnSteamNetConnectionStatusChanged( (SteamNetConnectionStatusChangedCallback_t*)x.data );
				break;
		#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
			case P2PSessionRequest_t::k_iCallback:
				COMPILE_TIME_ASSERT( sizeof(P2PSessionRequest_t) <= sizeof(x.data) );
				pCallbacks->OnP2PSessionRequest( (P2PSessionRequest_t*)x.data );
				break;
			case P2PSessionConnectFail_t::k_iCallback:
				COMPILE_TIME_ASSERT( sizeof(P2PSessionConnectFail_t) <= sizeof(x.data) );
				pCallbacks->OnP2PSessionConnectFail( (P2PSessionConnectFail_t*)x.data );
				break;
		#endif
			default:
				AssertMsg1( false, "Unknown callback type %d!", x.nCallback );
		}
	}
}

void CSteamNetworkingSockets::InternalQueueCallback( int nCallback, int cbCallback, const void *pvCallback )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	if ( cbCallback > sizeof( ((QueuedCallback*)0)->data ) )
	{
		AssertMsg( false, "Callback doesn't fit!" );
		return;
	}
	AssertMsg( len( m_vecPendingCallbacks ) < 100, "Callbacks backing up and not being checked.  Need to check them more frequently!" );

	QueuedCallback &q = *push_back_get_ptr( m_vecPendingCallbacks );
	q.nCallback = nCallback;
	memcpy( q.data, pvCallback, cbCallback );
}
#else
void CSteamNetworkingSockets::InternalQueueCallback( int nCallback, int cbCallback, const void *pvCallback )
{
	AssertMsg( false, "Should never be used" );
}
#endif

} // namespace SteamNetworkingSocketsLib
using namespace SteamNetworkingSocketsLib;

/////////////////////////////////////////////////////////////////////////////
//
// Global API interface
//
/////////////////////////////////////////////////////////////////////////////

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE

namespace SteamNetworkingSocketsLib {
CSteamNetworkingSockets g_SteamNetworkingSocketsUser;
}

STEAMNETWORKINGSOCKETS_INTERFACE bool GameNetworkingSockets_Init( const SteamNetworkingIdentity *pIdentity, SteamNetworkingErrMsg &errMsg )
{
	SteamDatagramTransportLock lock;

	// Init basic functionality
	if ( !g_SteamNetworkingSocketsUser.BInitGameNetworkingSockets( pIdentity, errMsg ) )
	{
		g_SteamNetworkingSocketsUser.Kill();
		return false;
	}

	return true;
}

STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_Kill()
{
	SteamDatagramTransportLock lock;
	g_SteamNetworkingSocketsUser.Kill();
}

STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamNetworkingSockets()
{
	return &g_SteamNetworkingSocketsUser;
}

#endif
