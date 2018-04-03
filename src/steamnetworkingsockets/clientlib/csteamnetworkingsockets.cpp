//====== Copyright Valve Corporation, All rights reserved. ====================

#include "csteamnetworkingsockets.h"
#include "steamnetworkingsockets_lowlevel.h"
#include "steamnetworkingsockets_connections.h"
#include "steamnetworkingsockets_udp.h"
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include "steamnetworkingsockets_sdr_hostedserver.h"
#include "steamnetworkingsockets_sdr_client.h"
#include "steamnetworkingsockets_sdr_p2p.h"
#endif
#include "crypto.h"

#define DEFINE_CONFIG // instance the variables here

#include "steamnetworkingconfig.h"

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

	{ k_ESteamNetworkingConfigurationValue_SNP_DebugWindow,                            "SNP_DebugWindow",                            &steamdatagram_snp_debug_window },
	{ k_ESteamNetworkingConfigurationValue_SNP_SendBufferSize,                         "SNP_SendBufferSize",                         &steamdatagram_snp_send_buffer_size },
	{ k_ESteamNetworkingConfigurationValue_SNP_MaxRate,                                "SNP_MaxRate",                                &steamdatagram_snp_max_rate },
	{ k_ESteamNetworkingConfigurationValue_SNP_MinRate,                                "SNP_MinRate",                                &steamdatagram_snp_min_rate },
	{ k_ESteamNetworkingConfigurationValue_SNP_Nagle_Time,                             "SNP_Nagle_Time",                             &steamdatagram_snp_nagle_time },
	{ k_ESteamNetworkingConfigurationValue_SNP_Log_RTT,                                "SNP_Log_RTT",                                &steamdatagram_snp_log_rtt },
	{ k_ESteamNetworkingConfigurationValue_SNP_Log_Packet,                             "SNP_Log_Packet",                             &steamdatagram_snp_log_packet },
	{ k_ESteamNetworkingConfigurationValue_SNP_Log_Segments,                           "SNP_Log_Segments",                           &steamdatagram_snp_log_segments },
	{ k_ESteamNetworkingConfigurationValue_SNP_Log_Feedback,                           "SNP_Log_Feedback",                           &steamdatagram_snp_log_feedback },
	{ k_ESteamNetworkingConfigurationValue_SNP_Log_Reliable,                           "SNP_Log_Reliable",                           &steamdatagram_snp_log_reliable },
	{ k_ESteamNetworkingConfigurationValue_SNP_Log_Message,                            "SNP_Log_Message",                            &steamdatagram_snp_log_message },
	{ k_ESteamNetworkingConfigurationValue_SNP_Log_Loss,                               "SNP_Log_Loss",                               &steamdatagram_snp_log_loss },
	{ k_ESteamNetworkingConfigurationValue_SNP_Log_X,                                  "SNP_Log_X",                                  &steamdatagram_snp_log_x },
	{ k_ESteamNetworkingConfigurationValue_SNP_Log_Nagle,                              "SNP_Log_Nagle",                              &steamdatagram_snp_log_nagle },
	{ k_ESteamNetworkingConfigurationValue_ClientConsecutitivePingTimeoutsFailInitial, "ClientConsecutitivePingTimeoutsFailInitial", &steamdatagram_client_consecutitive_ping_timeouts_fail_initial },
	{ k_ESteamNetworkingConfigurationValue_ClientConsecutitivePingTimeoutsFail,        "ClientConsecutitivePingTimeoutsFail",        &steamdatagram_client_consecutitive_ping_timeouts_fail },
	{ k_ESteamNetworkingConfigurationValue_ClientMinPingsBeforePingAccurate,           "ClientMinPingsBeforePingAccurate",           &steamdatagram_client_min_pings_before_ping_accurate },
	{ k_ESteamNetworkingConfigurationValue_ClientSingleSocket,                         "ClientSingleSocket",                         &steamdatagram_client_single_socket },
	{ k_ESteamNetworkingConfigurationValue_IP_Allow_Without_Auth,                      "IpAllowWithoutAuth",                         &steamdatagram_ip_allow_connections_without_auth },
	{ k_ESteamNetworkingConfigurationValue_Timeout_Seconds_Initial,                    "TimeoutSecondsInitial",                      &steamdatagram_timeout_seconds_initial },
	{ k_ESteamNetworkingConfigurationValue_Timeout_Seconds_Connected,                  "TimeoutSecondsConnected",                    &steamdatagram_timeout_seconds_connected },
};
const int k_nDeprecated = 1;
COMPILE_TIME_ASSERT( sizeof( sConfigurationValueEntryList ) / sizeof( SConfigurationValueEntry ) == k_ESteamNetworkingConfigurationValue_Count - k_nDeprecated );

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

CUtlLinkedList<CSteamNetworkConnectionBase *> g_listConnections;
CUtlLinkedList<CSteamNetworkListenSocketBase *> g_listListenSockets;

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
	int idx = sock & 0xffff;
	if ( !g_listConnections.IsValidIndex( idx ) )
		return nullptr;
	CSteamNetworkConnectionBase *pResult = g_listConnections[ idx ];
	if ( pResult->m_hConnectionSelf != sock )
		return nullptr;
	if ( !BConnectionStateExistsToAPI( pResult->GetState() ) )
		return nullptr;
	return pResult;
}

static CSteamNetworkListenSocketBase *GetListenSockedByHandle( HSteamListenSocket sock )
{
	if ( sock == 0 )
		return nullptr;
	int idx = sock & 0xffff;
	if ( !g_listListenSockets.IsValidIndex( idx ) )
		return nullptr;
	CSteamNetworkListenSocketBase *pResult = g_listListenSockets[ idx ];
	Assert( pResult && pResult->m_hListenSocketSelf == sock );
	return pResult;
}

static HSteamListenSocket AddListenSocket( CSteamNetworkListenSocketBase *pSock )
{
	// Use upper 16 bits as a connection sequence number, so that listen socket handles
	// are not reused within a short time period.
	static uint32 s_nUpperBits = 0;
	s_nUpperBits += 0x10000;
	if ( s_nUpperBits == 0 )
		s_nUpperBits = 0x10000;

	// Add it to our table of listen sockets
	pSock->m_hListenSocketSelf = g_listListenSockets.AddToTail( pSock ) | s_nUpperBits;
	return pSock->m_hListenSocketSelf;
}

CSteamNetworkConnectionBase *FindConnectionByLocalID( uint32 nLocalConnectionID )
{
	// Dumb linear search
	for ( CSteamNetworkConnectionBase *pConn: g_listConnections )
		if ( pConn->m_unConnectionIDLocal == nLocalConnectionID )
			return pConn;
	return nullptr;
}


/////////////////////////////////////////////////////////////////////////////
//
// Callbacks
//
/////////////////////////////////////////////////////////////////////////////

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

ISteamUser *g_pSteamUser;
ISteamGameServer *g_pSteamGameServer;

int g_iPartnerMask = -1;

static FSteamAPI_RegisterCallback s_fnRegisterCallback;
static FSteamAPI_UnregisterCallback s_fnUnregisterCallback;
static FSteamAPI_RegisterCallResult s_fnRegisterCallResult;
static FSteamAPI_UnregisterCallResult s_fnUnregisterCallResult;

void CSteamNetworkingSocketsCallbackBase::Register( bool bGameServer )
{
	Unregister();
	if ( bGameServer )
		m_nCallbackFlags |= k_ECallbackFlagsGameServer;
	else
		m_nCallbackFlags &= ~k_ECallbackFlagsGameServer;
	s_fnRegisterCallback( this, m_iCallback );
}

void CSteamNetworkingSocketsCallbackBase::Unregister(void)
{
	if ( m_nCallbackFlags & k_ECallbackFlagsRegistered )
		s_fnUnregisterCallback( this );
}

void CSteamNetworkingSocketsCallResultBase::Set(SteamAPICall_t hAPICall )
{
	Cancel();
	if ( hAPICall == k_uAPICallInvalid )
	{
		Assert( false );
	}
	else
	{
		m_hAPICall = hAPICall;
		s_fnRegisterCallResult( this, hAPICall );
	}
}

void CSteamNetworkingSocketsCallResultBase::Cancel(void)
{
	if ( m_hAPICall != k_uAPICallInvalid )
	{
		s_fnUnregisterCallResult( this, m_hAPICall );
		m_hAPICall = k_uAPICallInvalid;
	}
}

#endif // #ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE


} // namespace SteamNetworkingSocketsLib
using namespace SteamNetworkingSocketsLib;

/////////////////////////////////////////////////////////////////////////////
//
// CSteamSocketNetworking
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkingSockets::CSteamNetworkingSockets( bool bGameServer )
: m_bGameServer( bGameServer )
, m_bInitted( false )
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
, m_pSteamNetworkingSocketsSerialized( nullptr )
, m_pSteamUtils( nullptr )
, m_nAppID( 0 )
, m_bSDRClientInitted( false )
, m_eLogonStatus( k_ELogonStatus_InitialConnecting )
#endif
{}

static int s_nSteamNetworkingSocketsInitted = 0;

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
bool CSteamNetworkingSockets::BInitNonSteam( SteamDatagramErrMsg &errMsg )
{
	AssertMsg( !m_bInitted, "Initted interface twice?" );

	// Make sure low level socket support is ready
	if ( !BSteamNetworkingSocketsInitCommon( errMsg) )
		return false;

	m_bInitted = true;
	++s_nSteamNetworkingSocketsInitted;

	return true;
}
#else

bool CSteamNetworkingSockets::BInit( ISteamClient *pClient, HSteamUser hSteamUser, HSteamPipe hSteamPipe, SteamDatagramErrMsg &errMsg )
{
	AssertMsg( !m_bInitted, "Initted interface twice?" );

	// Make sure low level socket support is ready
	if ( !BSteamNetworkingSocketsInitCommon( errMsg) )
		return false;

	CSteamNetworkingSocketsCallback<SteamServersConnected_t>::Register( m_bGameServer );
	CSteamNetworkingSocketsCallback<SteamServerConnectFailure_t>::Register( m_bGameServer );
	CSteamNetworkingSocketsCallback<SteamServersDisconnected_t>::Register( m_bGameServer );
	CSteamNetworkingSocketsCallback<SteamNetworkingSocketsRecvP2PRendezvous_t>::Register( m_bGameServer );
	CSteamNetworkingSocketsCallback<SteamNetworkingSocketsRecvP2PFailure_t>::Register( m_bGameServer );
	CSteamNetworkingSocketsCallback<SteamNetworkingSocketsConfigUpdated_t>::Register( m_bGameServer );

	m_pSteamUtils = pClient->GetISteamUtils( hSteamPipe, STEAMUTILS_INTERFACE_VERSION );
	if ( !m_pSteamUtils )
	{
		V_sprintf_safe( errMsg, "Can't get steam interface '%s'", STEAMUTILS_INTERFACE_VERSION );
		return false;
	}
	g_eUniverse = m_pSteamUtils->GetConnectedUniverse();

	m_pSteamNetworkingSocketsSerialized = (ISteamNetworkingSocketsSerialized*)pClient->GetISteamGenericInterface( hSteamUser, hSteamPipe, STEAMNETWORKINGSOCKETSSERIALIZED_INTERFACE_VERSION );
	if ( !m_pSteamNetworkingSocketsSerialized )
	{
		V_sprintf_safe( errMsg, "Can't get steam interface '%s'", STEAMNETWORKINGSOCKETSSERIALIZED_INTERFACE_VERSION );
		return false;
	}

	m_nAppID = m_pSteamUtils->GetAppID();

	if ( m_bGameServer )
	{
		if ( g_pSteamGameServer->BLoggedOn() )
			m_eLogonStatus = k_ELogonStatus_Connected;
		else
			m_eLogonStatus = k_ELogonStatus_InitialConnecting;
	}
	else
	{
		if ( g_pSteamUser->BLoggedOn() )
			m_eLogonStatus = k_ELogonStatus_Connected;
		else
			m_eLogonStatus = k_ELogonStatus_Disconnected; // Unlike gameservers, we assume that we're logged on when we boot
	}

	// Cache our SteamID, if we're online
	GetSteamID();

	m_bInitted = true;
	++s_nSteamNetworkingSocketsInitted;

	return true;
}
#endif

void CSteamNetworkingSockets::Kill()
{
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
	CSteamNetworkingSocketsCallback<SteamServersConnected_t>::Unregister();
	CSteamNetworkingSocketsCallback<SteamServerConnectFailure_t>::Unregister();
	CSteamNetworkingSocketsCallback<SteamServersDisconnected_t>::Unregister();
	CSteamNetworkingSocketsCallback<SteamNetworkingSocketsRecvP2PRendezvous_t>::Unregister();
	CSteamNetworkingSocketsCallback<SteamNetworkingSocketsRecvP2PFailure_t>::Unregister();
	CSteamNetworkingSocketsCallback<SteamNetworkingSocketsConfigUpdated_t>::Unregister();
	m_pSteamNetworkingSocketsSerialized = nullptr;
	m_pSteamUtils = nullptr;
#endif

	// Destroy all of my connections
	for ( int idx = g_listConnections.FirstInorder() ; idx != g_listConnections.InvalidIndex() ; )
	{
		int idxNext = g_listConnections.NextInorder( idx );
		CSteamNetworkConnectionBase *pConn = g_listConnections[idx];
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
		{
			pConn->Destroy();
			Assert( !g_listConnections.IsValidIndex( idx ) );
		}
		idx = idxNext;
	}

	// Destroy all of my listen sockets
	for ( int idx = g_listListenSockets.FirstInorder() ; idx != g_listListenSockets.InvalidIndex() ; )
	{
		int idxNext = g_listListenSockets.NextInorder( idx );
		CSteamNetworkListenSocketBase *pSock = g_listListenSockets[idx];
		if ( pSock->m_pSteamNetworkingSocketsInterface == this )
		{
			DbgVerify( CloseListenSocket( pSock->m_hListenSocketSelf, nullptr ) );
			Assert( !g_listListenSockets.IsValidIndex( idx ) );
		}
		idx = idxNext;
	}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
	SDRClientKill();
#endif

	// Mark us as no longer being setup
	if ( m_bInitted )
	{
		m_bInitted = false;

		// Are we the last extant interface?  If so, then do global cleanup
		Assert( s_nSteamNetworkingSocketsInitted > 0 );
		--s_nSteamNetworkingSocketsInitted;
		if ( s_nSteamNetworkingSocketsInitted <= 0 )
			SteamNetworkingSocketsKillCommon();
	}
}

bool CSteamNetworkingSockets::BHasAnyConnections() const
{
	for ( CSteamNetworkConnectionBase *pConn: g_listConnections )
	{
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
			return true;
	}
	return false;
}

bool CSteamNetworkingSockets::BHasAnyListenSockets() const
{
	for ( CSteamNetworkListenSocketBase *pSock: g_listListenSockets )
	{
		if ( pSock->m_pSteamNetworkingSocketsInterface == this )
			return true;
	}
	return false;
}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
void CSteamNetworkingSockets::OnCallback( SteamServersConnected_t *param )
{
	SteamDatagramTransportLock lock;
	m_eLogonStatus = k_ELogonStatus_Connected;
	GetSteamID();

	if ( m_bGameServer )
	{
		SpewMsg( "Gameserver logged on to Steam, assigned SteamID %s\n", GetSteamID().Render() );
	}

	// See if we should make a cert request now.  We only need to do this if we have
	// any listen sockets or connections
	if ( !m_msgSignedCert.has_cert() && ( BHasAnyConnections() || BHasAnyListenSockets() ) )
	{
		AsyncCertRequest();
	}

}

void CSteamNetworkingSockets::OnCallback( SteamServerConnectFailure_t *param )
{
	ELogonStatus eSaveStatus = m_eLogonStatus;
	m_eLogonStatus = k_ELogonStatus_Disconnected;
	if ( eSaveStatus == CSteamNetworkingSockets::k_ELogonStatus_InitialConnecting )
	{
		if ( BHasAnyListenSockets() | BHasAnyListenSockets() )
			CertRequestFailed( k_ESteamNetConnectionEnd_Misc_SteamConnectivity, "Failed to connect to Steam" );
	}
}

void CSteamNetworkingSockets::OnCallback( SteamServersDisconnected_t *param )
{
	ELogonStatus eSaveStatus = m_eLogonStatus;
	m_eLogonStatus = k_ELogonStatus_Disconnected;
	if ( eSaveStatus == CSteamNetworkingSockets::k_ELogonStatus_InitialConnecting )
	{
		if ( BHasAnyListenSockets() || BHasAnyListenSockets() )
			CertRequestFailed( k_ESteamNetConnectionEnd_Misc_SteamConnectivity, "Lost connection to steam" );
	}
}

CSteamID CSteamNetworkingSockets::GetSteamID()
{
	if ( !m_steamID.IsValid() )
	{
		if ( m_bGameServer )
		{
			if ( g_pSteamGameServer )
				m_steamID = g_pSteamGameServer->GetSteamID();
		}
		else 
		{
			if ( g_pSteamUser )
				m_steamID = g_pSteamUser->GetSteamID();
		}
	}

	return m_steamID;
}
#endif

HSteamListenSocket CSteamNetworkingSockets::CreateListenSocket( int nSteamConnectVirtualPort, uint32 nIP, uint16 nPort )
{
	SteamDatagramTransportLock scopeLock;
	SteamDatagramErrMsg errMsg;

	// Might we want a cert?  If so, make sure async process to get one is in
	// progress (or try again if we tried earlier and failed)
	if ( nSteamConnectVirtualPort >= 0 || steamdatagram_ip_allow_connections_without_auth == 0 )
	{
		#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
			SpewError( "Need cert authority!" );
			return k_HSteamListenSocket_Invalid;
		#else
			AsyncCertRequest();
		#endif
	}

	// If they are asking fior P2P functionality, then we'll need SDR functionality, including the ability to
	// measure ping times to relays.  Start getting those resources ready now.
	if ( nSteamConnectVirtualPort != -1 )
	{
		#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
			SpewError( "Relayed connections require Steam!" );
			return k_HSteamListenSocket_Invalid;
		#else

			// Despite the API argument being an int, we'd like to reserve most of the address space.
			if ( nSteamConnectVirtualPort < 0 || nSteamConnectVirtualPort > 0xffff )
			{
				SpewError( "Virtual port number must be a small, positive number" );
				return k_HSteamListenSocket_Invalid;
			}

			// We need SDR functionality in order to support P2P
			if ( !BSDRClientInit( errMsg ) )
			{
				SpewError( "Cannot initialize SDR clientfunctionality to create P2P listen socket.  %s", errMsg );
				return k_HSteamListenSocket_Invalid;
			}
		#endif
	}

	CSteamNetworkListenSocketStandard *pSock = new CSteamNetworkListenSocketStandard( this );
	if ( !pSock )
		return k_HSteamListenSocket_Invalid;
	if ( !pSock->BInit( nSteamConnectVirtualPort, nIP, nPort, errMsg ) )
	{
		SpewError( "Cannot create listen socket.  %s", errMsg );
		delete pSock;
		return k_HSteamListenSocket_Invalid;
	}

	return AddListenSocket( pSock );
}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
HSteamNetConnection CSteamNetworkingSockets::ConnectBySteamID( CSteamID steamIDTarget, int nVirtualPort )
{
	if ( !steamIDTarget.IsValid() || !( steamIDTarget.BIndividualAccount() || steamIDTarget.BGameServerAccount() ) )
	{
		AssertMsg( false, "Invalid SteamID" );
		return k_HSteamNetConnection_Invalid;
	}

	// Despite the argument being an int, we actually restrict the range
	if ( nVirtualPort < 0 || nVirtualPort >= 0xffff )
	{
		AssertMsg( false, "Virtual port should be a small positive integer" );
		return k_HSteamNetConnection_Invalid;
	}

	SteamDatagramTransportLock scopeLock;

	CSteamNetworkConnectionP2PSDR *pConn = new CSteamNetworkConnectionP2PSDR( this );
	if ( !pConn )
		return k_HSteamNetConnection_Invalid;
	SteamDatagramErrMsg errMsg;
	if ( !pConn->BInitConnect( steamIDTarget, nVirtualPort, errMsg ) )
	{
		SpewError( "Cannot create P2P connection to %s.  %s", steamIDTarget.Render(), errMsg );
		delete pConn;
		return k_HSteamNetConnection_Invalid;
	}

	return pConn->m_hConnectionSelf;
}
#endif

HSteamNetConnection CSteamNetworkingSockets::ConnectByIPv4Address( uint32 nIP, uint16 nPort )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionIPv4 *pConn = new CSteamNetworkConnectionIPv4( this );
	if ( !pConn )
		return k_HSteamNetConnection_Invalid;
	SteamDatagramErrMsg errMsg;
	if ( !pConn->BInitConnect( netadr_t( nIP, nPort ), errMsg ) )
	{
		SpewError( "Cannot create IPv4 connection.  %s", errMsg );
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

bool CSteamNetworkingSockets::CloseListenSocket( HSteamListenSocket hSocket, const char *pszNotifyRemoteReason )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkListenSocketBase *pSock = GetListenSockedByHandle( hSocket );
	if ( !pSock )
		return false;
	int idx = hSocket & 0xffff;
	Assert( g_listListenSockets[ idx ] == pSock );

	// !FIXME! Need to handle putting connections into the linger state!
	//AssertMsg( !pszNotifyRemoteReason, "Closing connections gracefully not yet implemented!" );

	// Delete the socket itself
	// NOTE: If you change this, look at CSteamSocketNetworking::Kill()!
	pSock->Destroy();

	// Remove from our data structures
	g_listListenSockets[ idx ] = nullptr; // Just for grins
	g_listListenSockets.Remove( idx );
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
	pConn->SetName( pszName );
}

bool CSteamNetworkingSockets::GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hPeer );
	if ( !pConn )
		return false;
	V_strncpy( pszName, pConn->GetName(), nMaxLen );
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
	
int CSteamNetworkingSockets::ReceiveMessagesOnConnection( HSteamNetConnection hConn, ISteamNetworkingMessage **ppOutMessages, int nMaxMessages )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return -1;
	return pConn->APIReceiveMessages( ppOutMessages, nMaxMessages );
}

int CSteamNetworkingSockets::ReceiveMessagesOnListenSocket( HSteamListenSocket hSocket, ISteamNetworkingMessage **ppOutMessages, int nMaxMessages )
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

bool CSteamNetworkingSockets::GetListenSocketInfo( HSteamListenSocket hSocket, uint32 *pnIP, uint16 *pnPort )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkListenSocketBase *pSock = GetListenSockedByHandle( hSocket );
	if ( !pSock )
		return false;
	if ( pnIP )
		*pnIP = pSock->m_unIP;
	if ( pnPort )
		*pnPort = pSock->m_unPort;
	return true;
}

bool CSteamNetworkingSockets::CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback )
{
	SteamDatagramTransportLock scopeLock;

	// Assume failure
	*pOutConnection1 = k_HSteamNetConnection_Invalid;
	*pOutConnection2 = k_HSteamNetConnection_Invalid;

	// Create network connections?
	if ( bUseNetworkLoopback )
	{
		// Create two connection objects
		CSteamNetworkConnectionlocalhostLoopback *pConn[2];
		if ( !CSteamNetworkConnectionlocalhostLoopback::APICreateSocketPair( this, pConn ) )
			return false;

		// Return their handles
		*pOutConnection1 = pConn[0]->m_hConnectionSelf;
		*pOutConnection2 = pConn[1]->m_hConnectionSelf;
	}
	else
	{
		// Create two connection objects
		CSteamNetworkConnectionPipe *pConn[2];
		if ( !CSteamNetworkConnectionPipe::APICreateSocketPair( this, pConn ) )
			return false;

		// Return their handles
		*pOutConnection1 = pConn[0]->m_hConnectionSelf;
		*pOutConnection2 = pConn[1]->m_hConnectionSelf;
	}
	return true;
}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
void CSteamNetworkingSockets::AsyncCertRequest()
{
	// If we already have a cert, then we're done
	if ( m_msgSignedCert.has_cert() )
	{
		Assert( m_msgSignedCert.has_ca_signature() );
		Assert( m_msgCert.has_key_data() );
		Assert( m_msgCert.has_time_expiry() ); // We should never generate keys without an expiry!

		// See if the cert is about to expire, then go ahead and request another one
		if ( !m_pSteamUtils )
		{
			Assert( false );
			return;
		}
		int nSeconduntilExpiry = (long)m_msgCert.time_expiry() - (long)m_pSteamUtils->GetServerRealTime();
		if ( nSeconduntilExpiry > 3600 )
		{
			SpewVerbose( "Cert expires in %d seconds.  Not requesting another\n", nSeconduntilExpiry );
			return;
		}
		if ( nSeconduntilExpiry < 0 )
		{
			SpewMsg( "Cert expired %d seconds ago.  Discarding and requesting another\n", -nSeconduntilExpiry );
			m_msgSignedCert.Clear();
			m_msgCert.Clear();
			m_keyPrivateKey.Wipe();
		}
		else
		{
			SpewMsg( "Cert expires in %d seconds.  Requesting another, but keeping current cert in case request fails\n", nSeconduntilExpiry );
		}
	}

	// If a request is already active, then we just need to wait for it to complete
	if ( CSteamNetworkingSocketsCallResult<SteamNetworkingSocketsCert_t>::m_hAPICall != k_uAPICallInvalid )
		return;

	// If connection attempt has already failed, we cannot get a cert right now
	if ( m_eLogonStatus == k_ELogonStatus_Disconnected )
	{
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_SteamConnectivity, "We're not logged into" );
		return;
	}

	// If we're not logged on, we can't do this right now.
	if ( m_eLogonStatus == k_ELogonStatus_InitialConnecting )
		return;

	// We must know our SteamID
	CSteamID steamID = GetSteamID();
	if ( !steamID.IsValid() )
	{
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "Cannot request a cert; we don't know our SteamID (yet?)." );
		return;
	}

	// Do we have a serialized interface we can use to make a request?
	if ( m_pSteamNetworkingSocketsSerialized == nullptr )
	{
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "No ISteamNetworkingSocketsSerialized; old steam client binaries" );
		return;
	}

	// Make a request for a cert
	SteamAPICall_t hCall = m_pSteamNetworkingSocketsSerialized->GetCertAsync();
	if ( hCall == k_uAPICallInvalid )
	{
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "ISteamNetworkingSocketsSerialized::GetCertAsync failed" );
		return;
	}

	// Await the result
	SpewVerbose( "Requesting cert for %s from Steam\n", steamID.Render() );
	CSteamNetworkingSocketsCallResult<SteamNetworkingSocketsCert_t>::Set( hCall );
}

void CSteamNetworkingSockets::OnCallback( SteamNetworkingSocketsCert_t *param, bool bIOFailure )
{
	// Grab lock so we don't step on our toes in the other thread.  Let's try not to assume too
	// much about the callback context
	SteamDatagramTransportLock scopeLock;

	char msg[ k_cchSteamNetworkingMaxConnectionCloseReason ];
	if ( bIOFailure )
	{
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_SteamConnectivity, "Failed to get cert from steam" );
		return;
	}
	switch ( param->m_eResult )
	{
		case k_EResultOK: break;

		default:
			// Hm.... any specific result codes we should handle with different codes?
			V_sprintf_safe( msg, "Cert failure %d: %s", param->m_eResult, param->m_certOrMsg );
			CertRequestFailed( k_ESteamNetConnectionEnd_Misc_Generic, msg );
			return;
	}

	//
	// Decode the cert
	//
	CMsgSteamDatagramCertificate msgCert;
	if (
		!msgCert.ParseFromArray( param->m_certOrMsg, param->m_cbCert )
		|| !msgCert.has_time_expiry()
		|| !msgCert.has_key_data()
	) {
		Assert( false );
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "Cert request returned invalid cert" );
		return;
	}
	if ( msgCert.key_type() != CMsgSteamDatagramCertificate_EKeyType_ED25519 )
	{
		Assert( false );
		V_sprintf_safe( msg, "Cert request returned invalid key type %d", msgCert.key_type() );
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_InternalError, msg);
		return;
	}

	// Make sure the signature makes sense.  We won't check it here.
	if ( param->m_cbSignature != sizeof(CryptoSignature_t))
	{
		Assert( false );
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "Cert request returned invalid signature" );
		return;
	}

	//
	// Decode the private key
	//
	CECSigningPrivateKey privKey;
	if ( !privKey.Set( param->m_privKey, param->m_cbPrivKey ) || !privKey.IsValid() )
	{
		Assert( false );
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "Cert request returned invalid private key" );
		return;
	}

	//
	// Make sure that the private key and the cert match!
	//
	CECSigningPublicKey pubKey;
	if ( !pubKey.Set( msgCert.key_data().c_str(), (uint32)msgCert.key_data().length() ) )
	{
		Assert( false );
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "Cert request returned invalid public key" );
		return;
	}
	if ( !privKey.MatchesPublicKey( pubKey ) )
	{
		Assert( false );
		CertRequestFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "Cert request returned public/private key mismatch" );
		return;
	}

	// OK, save into our data structures
	m_msgCert = msgCert;
	m_msgSignedCert.Clear();
	m_msgSignedCert.set_cert( param->m_certOrMsg, param->m_cbCert );
	m_msgSignedCert.set_ca_key_id( param->m_caKeyID );
	m_msgSignedCert.set_ca_signature( param->m_signature, param->m_cbSignature );
	m_keyPrivateKey.Set( param->m_privKey, param->m_cbPrivKey );

	// Notify connections, so they can advance their state machine
	SpewVerbose( "Got cert for %s from Steam\n", GetSteamID().Render() );
	AsyncCertRequestFinished();
}

void CSteamNetworkingSockets::AsyncCertRequestFinished()
{

	// Check for any connections that we own that are waiting on a cert
	for ( CSteamNetworkConnectionBase *pConn: g_listConnections )
	{
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
			pConn->InterfaceGotCert();
	}
}

void CSteamNetworkingSockets::CertRequestFailed( ESteamNetConnectionEnd nConnectionEndReason, const char *pszMsg )
{
	SpewWarning( "Cert request for %s failed with reason code %d.  %s\n", GetSteamID().Render(), nConnectionEndReason, pszMsg );

	if ( m_msgSignedCert.has_cert() )
	{
		SpewMsg( "But we still have a valid cert, continuing with that one\n" );
		AsyncCertRequestFinished();
	}

	for ( CSteamNetworkConnectionBase *pConn: g_listConnections )
	{
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
			pConn->CertRequestFailed( nConnectionEndReason, pszMsg );
	}

	// FIXME If we have any listen sockets, we might need to let them know about this as well.
	// We probably want to keep trying until we get one.
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
	if ( !pubKey.Set( m_msgCert.key_data().c_str(), (uint32)m_msgCert.key_data().length() ) )
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

uint16 CSteamNetworkingSockets::GetHostedDedicatedServerListenPort()
{
	static int nPort = -1;
	if ( nPort == -1 )
	{
		const char *SDR_LISTEN_PORT = getenv( "SDR_LISTEN_PORT" );
		nPort = SDR_LISTEN_PORT ? atoi( SDR_LISTEN_PORT ) : 0;
	}
	return nPort;
}

HSteamListenSocket CSteamNetworkingSockets::CreateHostedDedicatedServerListenSocket( int nVirtualPort )
{
	SteamDatagramTransportLock scopeLock;
	if ( !m_bGameServer )
	{
		AssertMsg( false, "CreateHostedDedicatedServerListenSocket should be called thorugh a gameserver's ISteamSocketNetworking" );
		return k_HSteamListenSocket_Invalid;
	}
	uint16 nPhysicalPort = GetHostedDedicatedServerListenPort();
	if ( nPhysicalPort == 0 )
	{
		AssertMsg( false, "SDR_LISTEN_PORT not set, should not call CreateHostedDedicatedServerListenSocket" );
		return k_HSteamListenSocket_Invalid;
	}
	CSteamNetworkListenSocketSDRServer *pSock = new CSteamNetworkListenSocketSDRServer( this );
	if ( !pSock )
		return k_HSteamListenSocket_Invalid;
	SteamDatagramErrMsg errMsg;
	if ( !pSock->BInit( nPhysicalPort, nVirtualPort, errMsg ) )
	{
		SpewError( "Cannot create hosted dedicated server listen socket.  %s", errMsg );
		delete pSock;
		return k_HSteamListenSocket_Invalid;
	}

	return AddListenSocket( pSock );
}

HSteamNetConnection CSteamNetworkingSockets::ConnectToHostedDedicatedServer( CSteamID steamIDTarget, int nVirtualPort )
{
	SteamDatagramTransportLock scopeLock;
	AssertMsg( !m_bGameServer, "ConnectToHostedDedicatedServer should not be called thorugh a gameserver's ISteamSocketNetworking" );
	CSteamNetworkConnectionToSDRServer *pConn = new CSteamNetworkConnectionToSDRServer( this );
	if ( !pConn )
		return k_HSteamNetConnection_Invalid;
	SteamDatagramErrMsg errMsg;
	if ( !pConn->BInitConnect( steamIDTarget, nVirtualPort, errMsg ) )
	{
		SpewError( "Cannot create SDR connection to hosted dedicated server.  %s", errMsg );
		pConn->Destroy();
		return k_HSteamNetConnection_Invalid;
	}

	return pConn->m_hConnectionSelf;
}

#endif

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

void CSteamNetworkingSockets::RunCallbacks( ISteamNetworkingSocketsCallbacks *pCallbacks )
{
	// !KLUDGE! If in special debug mode, do work now
	if ( g_bThreadInMainThread )
	{
		CallDatagramThreadProc();
	}

	// Only hold lock for a brief period
	CUtlLinkedList<QueuedCallback> listTemp;
	{
		SteamDatagramTransportLock scopeLock;
		UpdateSNPDebugWindow();

		// Swap list with the temp one
		listTemp.Swap( m_listPendingCallbacks );

		// Release the lock
	}

	// Dispatch the callbacks
	FOR_EACH_LL( listTemp, idx )
	{
		QueuedCallback &x = listTemp[ idx ];
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
	AssertMsg( m_listPendingCallbacks.Count() < 100, "Callbacks backing up and not being checked.  Need to check them more frequently!" );

	QueuedCallback &q = m_listPendingCallbacks[ m_listPendingCallbacks.AddToTail() ];
	q.nCallback = nCallback;
	memcpy( q.data, pvCallback, cbCallback );
}

CSteamNetworkingSockets SteamNetworkingSocketsLib::g_SteamNetworkingSocketsUser(false);
CSteamNetworkingSockets SteamNetworkingSocketsLib::g_SteamNetworkingSocketsGameServer(true);

/////////////////////////////////////////////////////////////////////////////
//
// Global API interface
//
/////////////////////////////////////////////////////////////////////////////

STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamNetworkingSockets()
{
	return &g_SteamNetworkingSocketsUser;
}

STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamNetworkingSocketsGameServer()
{
	return &g_SteamNetworkingSocketsGameServer;
}

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE

STEAMNETWORKINGSOCKETS_INTERFACE bool GameNetworkingSockets_Init( SteamDatagramErrMsg &errMsg )
{
	SteamDatagramTransportLock lock;

	// Init basic functionality
	if ( !g_SteamNetworkingSocketsUser.BInitNonSteam( errMsg ) )
		return false;
	if ( !g_SteamNetworkingSocketsGameServer.BInitNonSteam( errMsg ) )
		return false;

	return true;
}

STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_Kill()
{
	SteamDatagramTransportLock lock;
	g_SteamNetworkingSocketsUser.Kill();
	g_SteamNetworkingSocketsGameServer.Kill();
}

#else

STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagramClient_Kill()
{
	SteamDatagramTransportLock lock;

	// Interfaces might be destroyed soon, nuke them
	g_pSteamUser = nullptr;
	g_SteamNetworkingSocketsUser.Kill();
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagramClient_Internal_SteamAPIKludge( FSteamAPI_RegisterCallback fnRegisterCallback, FSteamAPI_UnregisterCallback fnUnregisterCallback, FSteamAPI_RegisterCallResult fnRegisterCallResult, FSteamAPI_UnregisterCallResult fnUnregisterCallResult )
{
	s_fnRegisterCallback = fnRegisterCallback;
	s_fnUnregisterCallback = fnUnregisterCallback;
	s_fnRegisterCallResult = fnRegisterCallResult;
	s_fnUnregisterCallResult = fnUnregisterCallResult;
}

// !KLUDGE! For testing on steam main branch, which has newer interface versions than are publicly released
//#undef STEAMCLIENT_INTERFACE_VERSION
//#define STEAMCLIENT_INTERFACE_VERSION		"SteamClient017"

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamDatagramClient_Init_InternalV4( int iPartnerMask, SteamDatagramErrMsg &errMsg, FSteamInternal_CreateInterface fnCreateInterface, HSteamUser hSteamUser, HSteamPipe hSteamPipe )
{
	SteamDatagramTransportLock lock;
	if ( g_pSteamUser )
	{
		AssertMsg( false, "SteamDatagram_InitClient called more than once." );
		return true;
	}

	//
	// Locate interfaces
	//

	ISteamClient *pClient = (ISteamClient*)(*fnCreateInterface)( STEAMCLIENT_INTERFACE_VERSION );
	if ( pClient == nullptr )
	{
		V_sprintf_safe( errMsg, "Can't get Steam interface '%s'", STEAMCLIENT_INTERFACE_VERSION );
		return false;
	}

	g_pSteamUser = pClient->GetISteamUser( hSteamUser, hSteamPipe, STEAMUSER_INTERFACE_VERSION );
	if ( !g_pSteamUser )
	{
		V_sprintf_safe( errMsg, "Can't get steam interface '%s'", STEAMUSER_INTERFACE_VERSION );
		return false;
	}

	// Save partner mask
	g_iPartnerMask = iPartnerMask;
	Assert( g_iPartnerMask != 0 );

	// Init basic functionality
	if ( !g_SteamNetworkingSocketsUser.BInit( pClient, hSteamUser, hSteamPipe, errMsg ) )
		return false;

	// For now, we assume that clients will always want to use SDR functionality, even if all it
	// is for is measurement to the relays.  Eventually we might need to expose a separate entry
	// point to the app so they can express exactly what functionality will be needed.
	if ( !g_SteamNetworkingSocketsUser.BSDRClientInit( errMsg ) )
		return false;

	// And for now assume that they will always want a cert.
	g_SteamNetworkingSocketsUser.AsyncCertRequest();

	return true;
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamDatagramServer_Init_Internal( SteamDatagramErrMsg &errMsg, FSteamInternal_CreateInterface fnCreateInterface, HSteamUser hSteamUser, HSteamPipe hSteamPipe )
{
	SteamDatagramTransportLock lock;

	//
	// Local interfaces
	//

	ISteamClient *pClient = (ISteamClient*)(*fnCreateInterface)( STEAMCLIENT_INTERFACE_VERSION );
	if ( pClient == nullptr )
	{
		V_sprintf_safe( errMsg, "Can't get Steam interface '%s'", STEAMCLIENT_INTERFACE_VERSION );
		return false;
	}

	g_pSteamGameServer = pClient->GetISteamGameServer( hSteamUser, hSteamPipe, STEAMGAMESERVER_INTERFACE_VERSION );
	if ( !g_pSteamGameServer )
	{
		V_sprintf_safe( errMsg, "Can't get steam interface '%s'", STEAMGAMESERVER_INTERFACE_VERSION );
		return false;
	}

	// Init basic functionality
	if ( !g_SteamNetworkingSocketsGameServer.BInit( pClient, hSteamUser, hSteamPipe, errMsg ) )
		return false;

	// Check environment variables, see if we are hosed in our data center
	char *pszPrivateKey = getenv( "SDR_PRIVATE_KEY" );
	char *pszCert = getenv( "SDR_CERT" );
	if ( pszPrivateKey && *pszPrivateKey && pszCert && *pszCert )
	{
		SteamDatagramErrMsg certErrMsg;
		if ( !g_SteamNetworkingSocketsGameServer.SetCertificate( pszCert, V_strlen(pszCert), pszPrivateKey, V_strlen(pszPrivateKey), certErrMsg ) )
		{
			V_sprintf_safe( errMsg, "Invalid SDR_PRIVATE_KEY or SDR_CERT.  %s", certErrMsg );
			return false;
		}
		SpewMsg( "Using cert from SDR_PRIVATE_KEY and SDR_CERT environment vars\n" );
	}
	else
	{
		// Should either specify one, or both
		AssertMsg( ( pszPrivateKey == nullptr || *pszPrivateKey == '\0' )
			&& ( pszCert == nullptr || *pszCert == '\0' ), "Specified only one of SDR_PRIVATE_KEY and SDR_CERT" );

		if ( g_SteamNetworkingSocketsGameServer.GetHostedDedicatedServerListenPort() )
		{
			SpewWarning( "SDR_LISTEN_PORT is set, but not SDR_CERT & SDR_PRIVATE_KEY!  Clients will not be able to trust this server.  This is OK for dev, but should not happen in production!\n" );
		}
	}

	char *pszNetworkConfigFile = getenv( "SDR_NETWORK_CONFIG" );
	if ( pszNetworkConfigFile && *pszNetworkConfigFile )
	{
		// See if a cache file already exists, then try to load it
		CUtlBuffer buf;
		if ( !LoadFileIntoBuffer( pszNetworkConfigFile, buf ) )
		{
			V_sprintf_safe( errMsg, "Can't open '%s' as per SDR_NETWORK_CONFIG", pszNetworkConfigFile );
			return false;
		}

		// Parse
		SteamDatagramErrMsg msgConfig;
		int r = g_SteamDatagramNetwork.SetupFromJSON( (const char *)buf.Base(), buf.TellPut(), msgConfig, g_iPartnerMask );
		if ( r < 0 )
		{
			V_sprintf_safe( errMsg, "Failed to parse '%s' as per SDR_NETWORK_CONFIG.  %s", pszNetworkConfigFile, msgConfig );
			return false;
		}
		g_eAvailNetworkConfig = k_ESteamDatagramAvailability_Current;
		SpewMsg( "Loaded network config revision %d from '%s' as per SDR_NETWORK_CONFIG\n", g_SteamDatagramNetwork.m_nRevision, pszNetworkConfigFile );

		// Init shared cluster stuff
		CreateSharedClusterData();
	}

	return true;
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagramServer_Kill()
{
	SteamDatagramTransportLock lock;
	g_pSteamGameServer = nullptr;
	g_SteamNetworkingSocketsGameServer.Kill();
}

#endif
