//====== Copyright Valve Corporation, All rights reserved. ====================

#include "csteamnetworkingsockets.h"
#include <steamnetworkingsockets/steamnetworkingsockets.h>
#include "steamnetworkingsockets_lowlevel.h"
#include "steamnetworkingsockets_connections.h"
#include "steamnetworkingsockets_udp.h"
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include "steamnetworkingsockets_sdr_hostedserver.h"
#include "steamnetworkingsockets_sdr_client.h"
#include "steamnetworkingsockets_sdr_p2p.h"
#endif
#include "crypto.h"

#ifdef LINUX
	#include <sys/types.h>
	#include <ifaddrs.h>
	#include <sys/ioctl.h>
	#include <net/if.h>
#endif

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

static HSteamListenSocket AddListenSocket( CSteamNetworkListenSocketBase *pSock )
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
// Callbacks
//
/////////////////////////////////////////////////////////////////////////////

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

ISteamUser *g_pSteamUser;
ISteamGameServer *g_pSteamGameServer;

std::string g_sLauncherPartner = "valve";

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
bool CSteamNetworkingSockets::BInitNonSteam( const SteamNetworkingIdentity *pIdentity, SteamDatagramErrMsg &errMsg )
{
	AssertMsg( !m_bInitted, "Initted interface twice?" );

	// Make sure low level socket support is ready
	if ( !BSteamNetworkingSocketsInitCommon( errMsg) )
		return false;

	if ( pIdentity )
		m_identity = *pIdentity;

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

	// Cache our identity, if we're online
	m_identity.Clear();
	InternalGetIdentity();

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

	// Kill relayed connection support
	#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
		SDRClientKill();
	#endif

	// Clear identity and crypto stuff.
	// If we are re-initialized, we might get new ones
	m_identity.Clear();
	#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
		m_eLogonStatus = k_ELogonStatus_InitialConnecting;
	#endif
	m_msgSignedCert.Clear();
	m_msgCert.Clear();
	m_keyPrivateKey.Wipe();

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

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
void CSteamNetworkingSockets::OnCallback( SteamServersConnected_t *param )
{
	SteamDatagramTransportLock lock;
	m_eLogonStatus = k_ELogonStatus_Connected;
	InternalGetIdentity();

	if ( m_bGameServer )
	{
		SpewMsg( "Gameserver logged on to Steam, assigned identity %s\n", SteamNetworkingIdentityRender( InternalGetIdentity() ).c_str() );
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
	SteamDatagramTransportLock scopeLock;
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
	SteamDatagramTransportLock scopeLock;
	ELogonStatus eSaveStatus = m_eLogonStatus;
	m_eLogonStatus = k_ELogonStatus_Disconnected;
	if ( eSaveStatus == CSteamNetworkingSockets::k_ELogonStatus_InitialConnecting )
	{
		if ( BHasAnyListenSockets() || BHasAnyListenSockets() )
			CertRequestFailed( k_ESteamNetConnectionEnd_Misc_SteamConnectivity, "Lost connection to steam" );
	}
}
#endif

const SteamNetworkingIdentity &CSteamNetworkingSockets::InternalGetIdentity()
{
	// FIXME SteamNetworkingIdentity
	// Should eventually support other types
	if ( m_identity.IsInvalid() )
	{
		#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
			if ( m_bGameServer )
			{
				if ( g_pSteamGameServer && g_pSteamGameServer->GetSteamID().IsValid() )
					m_identity.SetSteamID( g_pSteamGameServer->GetSteamID() );
			}
			else
			{
				if ( g_pSteamUser && g_pSteamUser->GetSteamID().IsValid() )
					m_identity.SetSteamID( g_pSteamUser->GetSteamID() );
			}
		#else
			m_identity.SetLocalHost();
		#endif
	}
	return m_identity;
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

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE

HSteamListenSocket CSteamNetworkingSockets::CreateListenSocketP2P( int nVirtualPort )
{
	SteamDatagramTransportLock scopeLock;
	SteamDatagramErrMsg errMsg;

	// We'll need a cert.  Start sure async process to get one is in
	// progress (or try again if we tried earlier and failed)
	AsyncCertRequest();

	// We'll need SDR functionality, including the ability to measure ping
	// times to relays.  Start getting those resources ready now.

	// Despite the API argument being an int, we'd like to reserve most of the address space.
	if ( nVirtualPort < 0 || nVirtualPort > 0xffff )
	{
		SpewError( "Virtual port number must be a small, positive number" );
		return k_HSteamListenSocket_Invalid;
	}

	CSteamNetworkListenSocketP2P *pSock = new CSteamNetworkListenSocketP2P( this );
	if ( !pSock )
		return k_HSteamListenSocket_Invalid;
	if ( !pSock->BInit( nVirtualPort, errMsg ) )
	{
		SpewError( "Cannot create listen socket.  %s", errMsg );
		delete pSock;
		return k_HSteamListenSocket_Invalid;
	}

	return AddListenSocket( pSock );
}

HSteamNetConnection CSteamNetworkingSockets::ConnectP2P( const SteamNetworkingIdentity &identityRemote, int nVirtualPort )
{
	if ( identityRemote.IsInvalid() )
	{
		AssertMsg( false, "Invalid identity" );
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
	if ( !pConn->BInitConnect( identityRemote, nVirtualPort, errMsg ) )
	{
		SpewError( "Cannot create P2P connection to %s.  %s", SteamNetworkingIdentityRender( identityRemote ).c_str(), errMsg );
		pConn->FreeResources();
		delete pConn;
		return k_HSteamNetConnection_Invalid;
	}

	return pConn->m_hConnectionSelf;
}
#endif

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
	// FIXME SteamNetworkingIdentity - only works for SteamIDs
	CSteamID steamID = InternalGetIdentity().GetSteamID();
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
	SpewVerbose( "Got cert for %s from Steam\n", SteamNetworkingIdentityRender( InternalGetIdentity() ).c_str() );
	AsyncCertRequestFinished();
}

void CSteamNetworkingSockets::AsyncCertRequestFinished()
{

	// Check for any connections that we own that are waiting on a cert
	for ( const auto &item: g_mapConnections )
	{
		if ( item.elem->m_pSteamNetworkingSocketsInterface == this )
			item.elem->InterfaceGotCert();
	}
}

void CSteamNetworkingSockets::CertRequestFailed( ESteamNetConnectionEnd nConnectionEndReason, const char *pszMsg )
{
	SpewWarning( "Cert request for %s failed with reason code %d.  %s\n", SteamNetworkingIdentityRender( InternalGetIdentity() ).c_str(), nConnectionEndReason, pszMsg );

	if ( m_msgSignedCert.has_cert() )
	{
		SpewMsg( "But we still have a valid cert, continuing with that one\n" );
		AsyncCertRequestFinished();
	}

	for ( const auto &item: g_mapConnections )
	{
		if ( item.elem->m_pSteamNetworkingSocketsInterface == this )
			item.elem->CertRequestFailed( nConnectionEndReason, pszMsg );
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

static SteamNetworkingPOPID s_nHostedDedicatedServerPOPID;
static SteamDatagramHostedAddress s_HostedDedicatedServerRouting;

uint16 CSteamNetworkingSockets::GetHostedDedicatedServerPort()
{
	SteamDatagramTransportLock scopeLock;
	static int s_nHostedDedicatedServerPort = -1;
	if ( s_nHostedDedicatedServerPort < 0 )
	{

		s_nHostedDedicatedServerPort = 0;

		// Check if we are a hosted dedicated server
		const char *SDR_LISTEN_PORT = getenv( "SDR_LISTEN_PORT" );
		if ( SDR_LISTEN_PORT )
		{
			SpewMsg( "SDR_LISTEN_PORT = %s\n", SDR_LISTEN_PORT );
			s_nHostedDedicatedServerPort = atoi( SDR_LISTEN_PORT );
			Assert( s_nHostedDedicatedServerPort );
		}

		const char *SDR_POPID = getenv( "SDR_POPID" );
		if ( SDR_POPID )
		{
			SpewMsg( "SDR_POPID = '%s'\n", SDR_POPID );
			s_nHostedDedicatedServerPOPID = CalculateSteamNetworkingPOPIDFromString( SDR_POPID );
		}
	}

	return s_nHostedDedicatedServerPort;
}

SteamNetworkingPOPID CSteamNetworkingSockets::GetHostedDedicatedServerPOPID()
{
	GetHostedDedicatedServerPort();
	return s_nHostedDedicatedServerPOPID;
}

bool CSteamNetworkingSockets::GetHostedDedicatedServerAddress( SteamDatagramHostedAddress *pRouting )
{
	if ( !m_bGameServer )
	{
		AssertMsg( false, "GetHostedDedicatedServerAddress should be called thorugh a gameserver's ISteamSocketNetworking" );
		return false;
	}

	if ( !m_bInitted )
	{
		AssertMsg( false, "GetHostedDedicatedServerAddress should not be called before calling SteamDatagramServer_Init." );
		return false;
	}

	// Make sure we're listening
	if ( GetHostedDedicatedServerPort() == 0 )
		return false;

	// Return routing to them
	if ( pRouting )
		*pRouting = s_HostedDedicatedServerRouting;

	return true;
}

HSteamListenSocket CSteamNetworkingSockets::CreateHostedDedicatedServerListenSocket( int nVirtualPort )
{
	SteamDatagramTransportLock scopeLock;
	if ( !m_bGameServer )
	{
		AssertMsg( false, "CreateHostedDedicatedServerListenSocket should be called thorugh a gameserver's ISteamSocketNetworking" );
		return k_HSteamListenSocket_Invalid;
	}
	CSteamNetworkListenSocketSDRServer *pSock = new CSteamNetworkListenSocketSDRServer( this );
	if ( !pSock )
		return k_HSteamListenSocket_Invalid;
	SteamDatagramErrMsg errMsg;
	if ( !pSock->BInit( nVirtualPort, errMsg ) )
	{
		SpewError( "Cannot create hosted dedicated server listen socket.  %s", errMsg );
		delete pSock;
		return k_HSteamListenSocket_Invalid;
	}

	return AddListenSocket( pSock );
}

HSteamNetConnection CSteamNetworkingSockets::ConnectToHostedDedicatedServer( const SteamNetworkingIdentity &identityTarget, int nVirtualPort )
{
	SteamDatagramTransportLock scopeLock;
	AssertMsg( !m_bGameServer, "ConnectToHostedDedicatedServer should not be called thorugh a gameserver's ISteamSocketNetworking" );
	CSteamNetworkConnectionToSDRServer *pConn = new CSteamNetworkConnectionToSDRServer( this );
	if ( !pConn )
		return k_HSteamNetConnection_Invalid;
	SteamDatagramErrMsg errMsg;
	if ( !pConn->BInitConnect( identityTarget, nVirtualPort, errMsg ) )
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

CSteamNetworkingSockets SteamNetworkingSocketsLib::g_SteamNetworkingSocketsUser(false);

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
CSteamNetworkingSockets SteamNetworkingSocketsLib::g_SteamNetworkingSocketsGameServer(true);

STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamNetworkingSocketsGameServer()
{
	return &g_SteamNetworkingSocketsGameServer;
}

#endif

/////////////////////////////////////////////////////////////////////////////
//
// Global API interface
//
/////////////////////////////////////////////////////////////////////////////

STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamNetworkingSockets()
{
	return &g_SteamNetworkingSocketsUser;
}

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE

STEAMNETWORKINGSOCKETS_INTERFACE bool GameNetworkingSockets_Init( const SteamNetworkingIdentity *pIdentity, SteamDatagramErrMsg &errMsg )
{
	SteamDatagramTransportLock lock;

	// Init basic functionality
	if ( !g_SteamNetworkingSocketsUser.BInitNonSteam( pIdentity, errMsg ) )
		return false;

	return true;
}

STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_Kill()
{
	SteamDatagramTransportLock lock;
	g_SteamNetworkingSocketsUser.Kill();
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

STEAMNETWORKINGSOCKETS_INTERFACE void SteamDatagramClient_SetLauncher( const char *pszLauncher )
{
	AssertMsg( !g_SteamNetworkingSocketsUser.BInitted(), "Called SteamDatagramClient_SetPartner too late!" );

	g_sLauncherPartner = pszLauncher;
	Assert( !g_sLauncherPartner.empty() );
}

STEAMNETWORKINGSOCKETS_INTERFACE bool SteamDatagramClient_Init_InternalV7( SteamDatagramErrMsg &errMsg, FSteamInternal_CreateInterface fnCreateInterface, HSteamUser hSteamUser, HSteamPipe hSteamPipe )
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

	//
	// Are we listening for SDR in a Valve data center
	//
	s_HostedDedicatedServerRouting.Clear();
	uint16 nSDR_PORT = g_SteamNetworkingSocketsGameServer.GetHostedDedicatedServerPort();
	SteamNetworkingPOPID nSDR_POPID = g_SteamNetworkingSocketsGameServer.GetHostedDedicatedServerPOPID();
	const char *SDR_POPID = getenv( "SDR_POPID" );
	if ( nSDR_PORT && SDR_POPID )
	{
		V_strcpy_safe( s_HostedDedicatedServerRouting.m_data, SDR_POPID );
		s_HostedDedicatedServerRouting.m_cbSize = sizeof(SteamNetworkingPOPID);
		Assert( s_HostedDedicatedServerRouting.GetPopID() == nSDR_POPID );

		// Did they give us specific routing string to use?
		// We really don't need to understand this -- actually only the relay neds to.
		const char *SDR_ROUTING = getenv( "SDR_ROUTING" );
		if ( SDR_ROUTING )
		{
			uint32 cubDecodedData = sizeof(s_HostedDedicatedServerRouting.m_data) - s_HostedDedicatedServerRouting.m_cbSize;
			if ( !CCrypto::HexDecode( SDR_ROUTING, (uint8 *)s_HostedDedicatedServerRouting.m_data + s_HostedDedicatedServerRouting.m_cbSize, &cubDecodedData ) )
			{
				V_strcpy_safe( errMsg, "SDR_ROUTING is invalid or too long" );
				return false;
			}
			s_HostedDedicatedServerRouting.m_cbSize += cubDecodedData;
		}
		else
		{
			// No?  We'll have to use plaintext
			// Figure out our IP
			uint32 nIP = 0;

			// Set via environment variable?
			const char *SDR_IP = getenv( "SDR_IP" );
			if ( SDR_IP )
			{
				SpewMsg( "SDR_IP = '%s'\n", SDR_IP );
				netadr_t adr;
				if ( !adr.SetFromString( SDR_IP ) )
				{
					V_sprintf_safe( errMsg, "SDR_IP='%s', which isn't a valid IP address", SDR_IP );
					return false;
				}
				nIP = adr.GetIP();
			}
			else
			{

				// Get list of IP addresses
				CUtlVector<netadr_t> vecIPs;

				// On linux, use getifaddr, so it doesn't matter how they have DNS resolved.  Basically
				// we want to make sure we don't end up resolving our own hostname back to the loopback.
				#ifdef LINUX
					ifaddrs *pMyAddrInfo = NULL;
					int r = getifaddrs( &pMyAddrInfo );
					if ( r != 0 )
						Plat_FatalError( "getifaddrs() failed, returned %d",  r );
					for ( ifaddrs *pAddr = pMyAddrInfo ; pAddr ; pAddr = pAddr->ifa_next )
					{
						if ( ( pAddr->ifa_flags & IFF_LOOPBACK ) != 0 || !pAddr->ifa_addr )
							continue;
						netadr_t adr;
						if ( !adr.SetFromSockadr( pAddr->ifa_addr ) )
							continue;
						vecIPs.AddToTail( adr );
					}
					freeifaddrs( pMyAddrInfo );
				#elif defined(WIN32)

					// On Windows resolve our hostname.
					char szHostName[ 256 ];
					if ( gethostname( szHostName, sizeof(szHostName) ) != 0 )
						Plat_FatalError( "gethostname failed, error code 0x%x",  GetLastSocketError() );
					addrinfo *pMyAddrInfo = NULL;
					int r = getaddrinfo( szHostName, NULL, NULL, &pMyAddrInfo );
					if ( r != 0 )
						Plat_FatalError( "getaddrinfo(%s) failed, returned %d",  szHostName, r );
					for ( addrinfo *pAddr = pMyAddrInfo ; pAddr ; pAddr = pAddr->ai_next )
					{
						if ( pAddr->ai_family != AF_INET || !pAddr->ai_addr )
							continue;
						netadr_t adr;
						if ( !adr.SetFromSockadr( pAddr->ai_addr ) )
							continue;
						vecIPs.AddToTail( adr );
					}
					freeaddrinfo( pMyAddrInfo );
				#endif

				// Scan list of IPs.  If we have a single public-IP, then we're probably good
				for ( netadr_t adr: vecIPs )
				{
					uint32 nCheckIP = adr.GetIP();
					if ( !IsPrivateIP( nCheckIP ) || ( g_eUniverse == k_EUniverseBeta && ( nCheckIP >> 24U ) == 172 ) ) // Allow 172.x.x.x.x address to count as "public" on beta universe
					{
						if ( nIP )
							SpewWarning( "Host is configured with multiple public IPs.  Using %s; ignoring %s\n", CUtlNetAdrRender( nIP ).String(), CUtlNetAdrRender( nCheckIP ).String() );
						else
							nIP = nCheckIP;
					}
				}
				if ( nIP == 0 )
				{
					V_strcpy_safe( errMsg, "Cannot deduce public IP.  Datacenter environment variables misconfigured!" );
					return false;
				}
				SpewMsg( "%s appears to be SDR public address.\n", CUtlNetAdrRender( nIP, nSDR_PORT ).String() );
			}

			// Put it in plaintext
			SteamDatagramHostedAddress_PlainText *pRoutingAsPlainText = (SteamDatagramHostedAddress_PlainText *)s_HostedDedicatedServerRouting.m_data;
			pRoutingAsPlainText->m_magic_0101 = 0x0101;
			pRoutingAsPlainText->m_ip = LittleDWord( nIP );
			pRoutingAsPlainText->m_port = LittleWord( nSDR_PORT );
			s_HostedDedicatedServerRouting.m_cbSize = sizeof(SteamDatagramHostedAddress_PlainText);
		}
	}

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

		if ( !SDR_POPID || !nSDR_POPID )
		{
			V_sprintf_safe( errMsg, "SDR_PRIVATE_KEY/SDR_CERT are set, but not SDR_POPID!  We don't know what data center we are in.\n" );
			return false;
		}

		// Make sure we are using a cert that is legit for us!
		bool bCertForThisPopID = false;
		for ( uint32 nCertPopID: g_SteamNetworkingSocketsGameServer.m_msgCert.gameserver_datacenter_ids() )
		{
			if ( nCertPopID == nSDR_POPID )
			{
				bCertForThisPopID  = true;
				break;
			}
		}
		if ( !bCertForThisPopID  )
		{
			V_sprintf_safe( errMsg, "SDR_POPID=%s, but our cert is not valid for that PoP ID!\n", SDR_POPID );
			return false;
		}
	}
	else
	{
		// Should either specify one, or both
		AssertMsg( ( pszPrivateKey == nullptr || *pszPrivateKey == '\0' )
			&& ( pszCert == nullptr || *pszCert == '\0' ), "Specified only one of SDR_PRIVATE_KEY and SDR_CERT" );

		// Make this fatal on production
		if ( SDR_POPID )
		{
			V_sprintf_safe( errMsg, "SDR_POPID is set, but not SDR_PRIVATE_KEY/SDR_CERT!  Certs are required in production data centers.\n" );
			return false;
		}

		// Otherwise, spew a message just to be clear
		if ( nSDR_PORT )
		{
			SpewMsg( "SDR_PORT is set, but not SDR_CERT & SDR_PRIVATE_KEY!  Clients will not be able to trust this server.  This is OK for dev, but should not happen in production!\n" );
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
		int r = g_SteamDatagramNetwork.SetupFromJSON( (const char *)buf.Base(), buf.TellPut(), msgConfig );
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
