//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_p2p.h"
#include "csteamnetworkingsockets.h"
#include "../steamnetworkingsockets_certstore.h"
#include "crypto.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
	#include "sdr/steamnetworkingsockets_sdr_p2p.h"
	#include "sdr/steamnetworkingsockets_sdr_client.h"
	#ifdef SDR_ENABLE_HOSTED_SERVER
		#include "sdr/steamnetworkingsockets_sdr_hostedserver.h"
	#endif
#endif

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
	#include "steamnetworkingsockets_p2p_ice.h"
#endif

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
	#include "csteamnetworkingmessages.h"
#endif

#include "tier0/memdbgoff.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI
	#include "../../common/steammessages_gamenetworkingui.pb.h"
#endif

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
	#include <steam/steamnetworkingfakeip.h>
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

// This table is protected by the global lock
CUtlHashMap<RemoteConnectionKey_t,CSteamNetworkConnectionP2P*, std::equal_to<RemoteConnectionKey_t>, RemoteConnectionKey_t::Hash > g_mapP2PConnectionsByRemoteInfo;

constexpr SteamNetworkingMicroseconds k_usecWaitForControllingAgentBeforeSelectingNonNominatedTransport = 1*k_nMillion;

// Retry timeout for reliable messages in P2P signals
constexpr SteamNetworkingMicroseconds k_usecP2PSignalReliableRTO = k_nMillion;

VirtualPortRender::VirtualPortRender( int nVirtualPort )
{
	if ( nVirtualPort == -1 )
	{
		V_strcpy_safe( m_buf, "vport ?" );
	}
	else if ( nVirtualPort == k_nVirtualPort_Messages )
	{
		V_strcpy_safe( m_buf, "msg vport" );
	}
	else if ( IsVirtualPortEphemeralFakePort( nVirtualPort ) )
	{
		V_sprintf_safe( m_buf, "eph fakeport #%d", nVirtualPort-k_nVirtualPort_EphemeralFakePort0 );
	}
	else if ( IsVirtualPortGlobalFakePort( nVirtualPort ) )
	{
		V_sprintf_safe( m_buf, "fakeport #%d", nVirtualPort-k_nVirtualPort_GlobalFakePort0  );
	}
	else
	{
		V_sprintf_safe( m_buf, "vport %d", nVirtualPort  );
	}
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketP2P
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkListenSocketP2P::CSteamNetworkListenSocketP2P( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkListenSocketBase( pSteamNetworkingSocketsInterface )
{
}

CSteamNetworkListenSocketP2P::~CSteamNetworkListenSocketP2P()
{
	// Remove from virtual port map
	if ( m_connectionConfig.LocalVirtualPort.IsSet() )
	{
		int h = m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.Find( LocalVirtualPort() );
		if ( h != m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.InvalidIndex() && m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort[h] == this )
		{
			m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort[h] = nullptr; // just for grins
			m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.RemoveAt( h );
		}
		else
		{
			AssertMsg( false, "Bookkeeping bug!" );
		}
	}
}

bool CSteamNetworkListenSocketP2P::BInit( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	Assert( nLocalVirtualPort >= 0 );

	if ( m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.HasElement( nLocalVirtualPort ) )
	{
		V_sprintf_safe( errMsg, "Already have a listen socket on P2P %s", VirtualPortRender( nLocalVirtualPort ).c_str() );
		return false;
	}
	m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.Insert( nLocalVirtualPort, this );

	// Lock in virtual port into connection config map.
	m_connectionConfig.LocalVirtualPort.Set( nLocalVirtualPort );
	m_connectionConfig.LocalVirtualPort.Lock();

	// Set options, add us to the global table
	if ( !BInitListenSocketCommon( nOptions, pOptions, errMsg ) )
		return false;

	return true;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkConnectionP2P
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkConnectionP2P::CSteamNetworkConnectionP2P( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, ConnectionScopeLock &scopeLock )
: CSteamNetworkConnectionBase( pSteamNetworkingSocketsInterface, scopeLock )
{
	m_nRemoteVirtualPort = -1;
	m_idxMapP2PConnectionsByRemoteInfo = -1;
	m_pSignaling = nullptr;
	m_usecWhenStartedFindingRoute = 0;
	m_usecNextEvaluateTransport = k_nThinkTime_ASAP;
	m_bTransportSticky = false;
	m_bAppConnectHandshakePacketsInRSVP = false;
	m_bNeedToSendConnectOKSignal = false;
	m_bWaitForInitialRoutingReady = true;

	m_pszNeedToSendSignalReason = nullptr;
	m_usecSendSignalDeadline = k_nThinkTime_Never;
	m_usecWhenSentLastSignal = 0; // A very long time ago
	m_nLastSendRendesvousMessageID = 0;
	m_nLastRecvRendesvousMessageID = 0;
	m_pPeerSelectedTransport = nullptr;

	m_pCurrentTransportP2P = nullptr;
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		m_pTransportP2PSDR = nullptr;
		#ifdef SDR_ENABLE_HOSTED_CLIENT
			m_pTransportToSDRServer = nullptr;
		#endif
		#ifdef SDR_ENABLE_HOSTED_SERVER
			m_pTransportFromSDRClient = nullptr;
		#endif
	#endif
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		m_pTransportICE = nullptr;
		m_pTransportICEPendingDelete = nullptr;
		m_szICECloseMsg[ 0 ] = '\0';
	#endif
}

CSteamNetworkConnectionP2P::~CSteamNetworkConnectionP2P()
{
	Assert( m_idxMapP2PConnectionsByRemoteInfo == -1 );
}

void CSteamNetworkConnectionP2P::GetConnectionTypeDescription_GetP2PType( ConnectionTypeDescription_t &szDescription ) const
{
	if ( IsSDRHostedServerClient() )
		V_strcpy_safe( szDescription, "SDR server" );
	else if ( m_pCurrentTransportP2P )
		V_sprintf_safe( szDescription, "P2P %s", m_pCurrentTransportP2P->m_pszP2PTransportDebugName );
	else
		V_strcpy_safe( szDescription, "P2P" );
}

void CSteamNetworkConnectionP2P::GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const
{
	// !SPEED! This could be done faster, but this code is
	// simple and isn't run very often

	GetConnectionTypeDescription_GetP2PType( szDescription );

	// If current remote identity is a FakeIP, that means we don't
	// really know who they are yet.
	if ( m_identityRemote.IsFakeIP() )
	{
		V_strcat_safe( szDescription, " ?@" );
		V_strcat_safe( szDescription, SteamNetworkingIPAddrRender( m_identityRemote.m_ip ).c_str() );
	}
	else
	{

		// Do we have a real identity?
		if ( !m_identityRemote.IsInvalid() && !m_identityRemote.IsLocalHost() )
		{
			V_strcat_safe( szDescription, " " );
			V_strcat_safe( szDescription, SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
		}

		// If we have a FakeIP, also include that.
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
			SteamNetworkingIPAddr fakeIP;
			if ( m_fakeIPRefRemote.GetInfo( nullptr, &fakeIP ) )
			{
				V_strcat_safe( szDescription, "@" );
				V_strcat_safe( szDescription, SteamNetworkingIPAddrRender( fakeIP ).c_str() );
			}
		#endif
	}

	//
	// Also include virtual port info, depending on the situation
	//

	int nLocalVirtualPort = LocalVirtualPort();

	// Local ephemeral ports are never interesting, act like they don't exist here
	if ( IsVirtualPortEphemeralFakePort(nLocalVirtualPort) )
		nLocalVirtualPort = -1;
	Assert( !IsVirtualPortEphemeralFakePort( m_nRemoteVirtualPort ) ); // Remote ephemeral ports are not a thing.

	if ( nLocalVirtualPort == m_nRemoteVirtualPort )
	{
		if ( nLocalVirtualPort >= 0 )
		{
			// Common symmetric situation, or where only one vport is really relevant
			V_strcat_safe( szDescription, " " );
			V_strcat_safe( szDescription, VirtualPortRender( nLocalVirtualPort ).c_str() );
		}
	}
	else if ( nLocalVirtualPort >= 0 && m_bConnectionInitiatedRemotely && !BSymmetricMode() )
	{
		// Common "server" situation
		V_strcat_safe( szDescription, " " );
		V_strcat_safe( szDescription, VirtualPortRender( nLocalVirtualPort ).c_str() );
	}
	else if ( m_nRemoteVirtualPort >= 0 && !m_bConnectionInitiatedRemotely && !BSymmetricMode() )
	{
		// Common "client" situation
		V_strcat_safe( szDescription, " " );
		V_strcat_safe( szDescription, VirtualPortRender( m_nRemoteVirtualPort ).c_str() );
	}
	else
	{
		// Weird situation
		if ( nLocalVirtualPort >= 0 )
		{
			V_strcat_safe( szDescription, " loc " );
			V_strcat_safe( szDescription, VirtualPortRender( nLocalVirtualPort ).c_str() );
		}
		if ( m_nRemoteVirtualPort >= 0 )
		{
			V_strcat_safe( szDescription, " rem " );
			V_strcat_safe( szDescription, VirtualPortRender( m_nRemoteVirtualPort ).c_str() );
		}
	}
}

bool CSteamNetworkConnectionP2P::BInitConnect(
	ISteamNetworkingConnectionSignaling *pSignaling,
	const SteamNetworkingIdentity *pIdentityRemote, int nRemoteVirtualPort,
	int nOptions, const SteamNetworkingConfigValue_t *pOptions,
	CSteamNetworkConnectionP2P **pOutMatchingSymmetricConnection,
	SteamDatagramErrMsg &errMsg
)
{
	Assert( !m_pTransport );

	if ( pOutMatchingSymmetricConnection )
		*pOutMatchingSymmetricConnection = nullptr;

	// Remember who we're talking to
	Assert( m_pSignaling == nullptr );
	m_pSignaling = pSignaling;
	if ( pIdentityRemote )
		m_identityRemote = *pIdentityRemote;
	m_nRemoteVirtualPort = nRemoteVirtualPort;

	// Can only initiate FakeIP connections to global addresses
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		if ( m_identityRemote.IsFakeIP() )
		{
			if ( m_identityRemote.GetFakeIPType() != k_ESteamNetworkingFakeIPType_GlobalIPv4 )
			{
				V_sprintf_safe( errMsg, "Can only initiate connection to global FakeIP" );
				AssertMsg( false, errMsg );
				return false;
			}
		}
	#endif

	// Remember when we started finding a session
	//m_usecTimeStartedFindingSession = usecNow;

	// Reset end-to-end state
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	if ( !BInitP2PConnectionCommon( usecNow, nOptions, pOptions, errMsg ) )
		return false;

	if ( !m_identityRemote.IsInvalid() && LocalVirtualPort() >= 0 )
	{
		bool bOnlySymmetricConnections = !BSymmetricMode();
		CSteamNetworkConnectionP2P *pMatchingConnection = CSteamNetworkConnectionP2P::FindDuplicateConnection( m_pSteamNetworkingSocketsInterface, LocalVirtualPort(), m_identityRemote, m_nRemoteVirtualPort, bOnlySymmetricConnections, this );
		if ( pMatchingConnection )
		{
			if ( pOutMatchingSymmetricConnection )
				*pOutMatchingSymmetricConnection = pMatchingConnection;
			V_sprintf_safe( errMsg, "Existing symmetric connection [%s]", pMatchingConnection->GetDescription() );
			return false;
		}
	}
	else
	{
		if ( BSymmetricMode() )
		{
			Assert( LocalVirtualPort() >= 0 );
			V_strcpy_safe( errMsg, "To use symmetric connect, remote identity must be specified" );
			return false;
		}
	}

	if ( !BInitSDRTransport( errMsg ) )
		return false;

	// Check if we should try ICE.
	CheckInitICE();

	// No available transports?
	Assert( GetState() == k_ESteamNetworkingConnectionState_None );
	if ( m_pTransport == nullptr && m_vecAvailableTransports.empty() )
	{
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
			ESteamNetConnectionEnd ignoreReason;
			ConnectionEndDebugMsg closeDebugMsg;
			GuessICEFailureReason( ignoreReason, closeDebugMsg, usecNow );
			V_strcpy_safe( errMsg, closeDebugMsg );
		#else
			Assert( false ); // We shouldn't compile if we don't have either SDR or ICE transport enabled.  And if SDR fails, we fail above
			V_strcpy_safe( errMsg, "No available P2P transports" );
		#endif
		return false;
	}

	// Start the connection state machine
	return BConnectionState_Connecting( usecNow, errMsg );
}

bool CSteamNetworkConnectionP2P::BInitP2PConnectionCommon( SteamNetworkingMicroseconds usecNow, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	// Let base class do some common initialization
	if ( !CSteamNetworkConnectionBase::BInitConnection( usecNow, nOptions, pOptions, errMsg ) )
		return false;

	// Check for defaulting the local virtual port to be the same as the remote virtual port
	if ( LocalVirtualPort() < 0 && m_nRemoteVirtualPort >= 0 )
	{
		Assert( m_nRemoteVirtualPort <= 0xffff || m_nRemoteVirtualPort == k_nVirtualPort_Messages ); // Regular P2P virtual ports
		m_connectionConfig.LocalVirtualPort.Set( m_nRemoteVirtualPort );
	}

	// Local virtual port cannot be changed henceforth
	m_connectionConfig.LocalVirtualPort.Lock();
	int nLocalVirtualPort = LocalVirtualPort();

	// Check for activating symmetric mode based on listen socket on the same local virtual port
	// But don't do this for FakeUDP ports -- only certain connections will be symmetric.
	if ( nLocalVirtualPort >= 0 && !BSymmetricMode() && !IsVirtualPortGlobalFakePort( nLocalVirtualPort ) )
	{

		// Are we listening on that virtual port?
		int idxListenSock = m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.Find( nLocalVirtualPort );
		if ( idxListenSock != m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.InvalidIndex() )
		{

			// Really, they should match.  App code should be all-or-nothing.  It should not mix.
			if ( m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort[ idxListenSock ]->BSymmetricMode() )
			{
				SpewWarning( "[%s] Setting SymmetricConnect=1 because it is enabled on listen socket on %s.  To avoid this warning, specify the option on connection creation\n",
					GetDescription(), VirtualPortRender( nLocalVirtualPort ).c_str() );
				Assert( !m_connectionConfig.SymmetricConnect.IsLocked() );
				m_connectionConfig.SymmetricConnect.Unlock();
				m_connectionConfig.SymmetricConnect.Set( 1 );
			}
		}
	}

	// Once symmetric mode is activated, it cannot be turned off!
	if ( BSymmetricMode() )
		m_connectionConfig.SymmetricConnect.Lock();

	// We must know our own identity to initiate or receive this kind of connection
	if ( m_identityLocal.IsInvalid() )
	{
		V_strcpy_safe( errMsg, "Unable to determine local identity.  Not logged in?" );
		return false;
	}

	// Check for connecting to self.
	if ( m_identityRemote == m_identityLocal )
	{
		// Spew a warning when P2P connecting to self
		// 1.) We should special case this and automatically create a pipe instead.
		//     But right now the pipe connection class assumes we want to be immediately
		//     connected.  We should fix that, for now I'll just spew.
		// 2.) It's not just connecting to self.  If we are connecting to an identity of
		//     another local CSteamNetworkingSockets interface, we should use a pipe.
		//     But we'd probably have to make a special flag to force it to relay,
		//     for tests.
		SpewWarning( "Connecting P2P socket to self (%s).  Traffic will be relayed over the network", SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
	}

	// If we know the remote connection ID already, then  put us in the map
	if ( m_unConnectionIDRemote )
	{
		if ( !BEnsureInP2PConnectionMapByRemoteInfo( errMsg ) )
			return false;
	}

	return true;
}

void CSteamNetworkConnectionP2P::RemoveP2PConnectionMapByRemoteInfo()
{
	AssertLocksHeldByCurrentThread();

	if ( m_idxMapP2PConnectionsByRemoteInfo >= 0 )
	{
		if ( g_mapP2PConnectionsByRemoteInfo.IsValidIndex( m_idxMapP2PConnectionsByRemoteInfo ) && g_mapP2PConnectionsByRemoteInfo[ m_idxMapP2PConnectionsByRemoteInfo ] == this )
		{
			g_mapP2PConnectionsByRemoteInfo[ m_idxMapP2PConnectionsByRemoteInfo ] = nullptr; // just for grins
			g_mapP2PConnectionsByRemoteInfo.RemoveAt( m_idxMapP2PConnectionsByRemoteInfo );
		}
		else
		{
			AssertMsg( false, "g_mapIncomingP2PConnections bookkeeping mismatch" );
		}
		m_idxMapP2PConnectionsByRemoteInfo = -1;
	}
}

bool CSteamNetworkConnectionP2P::BEnsureInP2PConnectionMapByRemoteInfo( SteamDatagramErrMsg &errMsg )
{
	Assert( !m_identityRemote.IsInvalid() );
	Assert( m_unConnectionIDRemote );

	RemoteConnectionKey_t key{ m_identityRemote, m_unConnectionIDRemote };
	if ( m_idxMapP2PConnectionsByRemoteInfo >= 0 )
	{
		Assert( g_mapP2PConnectionsByRemoteInfo.Key( m_idxMapP2PConnectionsByRemoteInfo ) == key );
		Assert( g_mapP2PConnectionsByRemoteInfo[ m_idxMapP2PConnectionsByRemoteInfo ] == this );
	}
	else
	{
		if ( g_mapP2PConnectionsByRemoteInfo.HasElement( key ) )
		{
			// "should never happen"
			V_sprintf_safe( errMsg, "Duplicate P2P connection %s %u!", SteamNetworkingIdentityRender( m_identityRemote ).c_str(), m_unConnectionIDRemote );
			AssertMsg1( false, "%s", errMsg );
			return false;
		}
		m_idxMapP2PConnectionsByRemoteInfo = g_mapP2PConnectionsByRemoteInfo.InsertOrReplace( key, this );
	}

	return true;
}

bool CSteamNetworkConnectionP2P::BBeginAcceptFromSignal(
	const CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest,
	SteamDatagramErrMsg &errMsg,
	SteamNetworkingMicroseconds usecNow
) {
	m_bConnectionInitiatedRemotely = true;

	// Let base class do some common initialization
	if ( !BInitP2PConnectionCommon( usecNow, 0, nullptr, errMsg ) )
		return false;

	// Initialize SDR transport
	if ( !BInitSDRTransport( errMsg ) )
		return false;

	// Process crypto handshake now
	if ( RecvCryptoHandshake( msgConnectRequest.cert(), msgConnectRequest.crypt(), true, errMsg ) != k_ESteamNetConnectionEnd_Invalid )
		return false;

	// Add to connection map
	if ( !BEnsureInP2PConnectionMapByRemoteInfo( errMsg ) )
		return false;

	// Start the connection state machine
	return BConnectionState_Connecting( usecNow, errMsg );
}

void CSteamNetworkConnectionP2P::ChangeRoleToServerAndAccept( const CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	int nLogLevel = LogLevel_P2PRendezvous();

	// Our connection should be the server.  We should change the role of this connection.
	// But we can only do that if we are still trying to connect!
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting )
	{
		SpewWarningGroup( nLogLevel, "[%s] Symmetric role resolution for connect request remote cxn ID #%u says we should act as server.  But we cannot change our role, since we are already in state %d!  Dropping incoming request\n", GetDescription(), msg.from_connection_id(), GetState() );
		return;
	}

	// We should currently be the client, and we should not already know anything about the remote host
	if ( m_bConnectionInitiatedRemotely )
	{
		AssertMsg( false, "[%s] Symmetric role resolution for connect request remote cxn ID #%u says we should act as server.  But we are already the server!  Why haven't we transitioned out of connecting state.  Dropping incoming request\n", GetDescription(), msg.from_connection_id() );
		return;
	}

	SpewVerboseGroup( nLogLevel, "[%s] Symmetric role resolution for connect request remote cxn ID #%u says we should act as server.  Changing role\n", GetDescription(), msg.from_connection_id() );

	// !KLUDGE! If we already started ICE, then we have to nuke it and restart.
	// It'd be better if we could just ask ICE to change the role.
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		bool bRestartICE = false;
		CheckCleanupICE();

		// Already failed?
		if ( GetICEFailureCode() != 0 )
		{
			SpewVerboseGroup( nLogLevel, "[%s] ICE already failed (%d %s) while changing role to server.  We won't try again.",
				GetDescription(), GetICEFailureCode(), m_szICECloseMsg );
		}
		else if ( m_pTransportICE )
		{
			SpewVerboseGroup( nLogLevel, "[%s] Destroying ICE while changing role to server.\n", GetDescription() );
			DestroyICENow();
			bRestartICE = true;
			Assert( GetICEFailureCode() == 0 );
		}
	#endif

	// We should not have done the crypto handshake yet
	Assert( !m_unConnectionIDRemote );
	Assert( m_idxMapP2PConnectionsByRemoteInfo < 0 );
	Assert( !m_bCryptKeysValid );
	Assert( m_sCertRemote.empty() );
	Assert( m_sCryptRemote.empty() );

	// We're changing the remote connection ID.
	// If we're in the remote info -> connection map,
	// we need to get out.  We'll add ourselves back
	// correct when we accept the connection
	RemoveP2PConnectionMapByRemoteInfo();

	// Change role
	m_bConnectionInitiatedRemotely = true;
	m_unConnectionIDRemote = msg.from_connection_id();

	// Clear crypt stuff that we usually do immediately as the client, but have
	// to defer when we're the server.  We need to redo it, now that our role has
	// changed
	ClearLocalCrypto();

	// Process crypto handshake now -- acting as the "server"
	const CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest = msg.connect_request();
	if ( !BRecvCryptoHandshake( msgConnectRequest.cert(), msgConnectRequest.crypt(), true ) )
	{
		Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
		return;
	}

	// Add to connection map
	SteamNetworkingErrMsg errMsg;
	if ( !BEnsureInP2PConnectionMapByRemoteInfo( errMsg ) )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "%s", errMsg );
		return;
	}

	// Restart ICE if necessary
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( bRestartICE )
		{
			Assert( GetICEFailureCode() == 0 );
			CheckInitICE();
		}
	#endif

	// Accept the connection
	EResult eAcceptResult = APIAcceptConnection();
	if ( eAcceptResult == k_EResultOK )
	{
		Assert( GetState() == k_ESteamNetworkingConnectionState_FindingRoute );
	}
	else
	{
		Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
	}
}

CSteamNetworkConnectionP2P *CSteamNetworkConnectionP2P::AsSteamNetworkConnectionP2P()
{
	return this;
}

bool CSteamNetworkConnectionP2P::BInitSDRTransport( SteamNetworkingErrMsg &errMsg )
{
	// Create SDR transport, if SDR is enabled
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR

		// Make sure SDR client functionality is ready
		CSteamNetworkingSocketsSDR *pSteamNetworkingSocketsSDR = assert_cast< CSteamNetworkingSocketsSDR *>( m_pSteamNetworkingSocketsInterface );
		if ( !pSteamNetworkingSocketsSDR->BSDRClientInit( errMsg ) )
			return false;

		// Create SDR transport
		Assert( !m_pTransportP2PSDR );
		m_pTransportP2PSDR = new CConnectionTransportP2PSDR( *this );
		Assert( !has_element( g_vecSDRClients, m_pTransportP2PSDR ) );
		g_vecSDRClients.push_back( m_pTransportP2PSDR );
		m_vecAvailableTransports.push_back( m_pTransportP2PSDR );
	#endif

	return true;
}

void CSteamNetworkConnectionP2P::FreeResources()
{
	AssertLocksHeldByCurrentThread();

	// Remove from global map, if we're in it
	RemoveP2PConnectionMapByRemoteInfo();

	// Release signaling
	if ( m_pSignaling )
	{	
		m_pSignaling->Release();
		m_pSignaling = nullptr;
	}

	// Base class cleanup
	CSteamNetworkConnectionBase::FreeResources();
}

void CSteamNetworkConnectionP2P::DestroyTransport()
{
	AssertLocksHeldByCurrentThread();

	// We're about to nuke all of our transports, don't point at any of them.
	m_pTransport = nullptr;
	m_pCurrentTransportP2P = nullptr;

	// Destroy ICE first
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		DestroyICENow();
	#endif

	// Nuke all other P2P transports
	for ( int i = len( m_vecAvailableTransports )-1 ; i >= 0 ; --i )
	{
		m_vecAvailableTransports[i]->m_pSelfAsConnectionTransport->TransportDestroySelfNow();
		Assert( len( m_vecAvailableTransports ) == i );
	}

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		Assert( m_pTransportP2PSDR == nullptr ); // Should have been nuked above

		#ifdef SDR_ENABLE_HOSTED_CLIENT
			if ( m_pTransportToSDRServer )
			{
				m_pTransportToSDRServer->TransportDestroySelfNow();
				m_pTransportToSDRServer = nullptr;
			}
		#endif

		#ifdef SDR_ENABLE_HOSTED_SERVER
			if ( m_pTransportFromSDRClient )
			{
				m_pTransportFromSDRClient->TransportDestroySelfNow();
				m_pTransportFromSDRClient = nullptr;
			}
		#endif
	#endif
}

CSteamNetworkConnectionP2P *CSteamNetworkConnectionP2P::FindDuplicateConnection( CSteamNetworkingSockets *pInterfaceLocal, int nLocalVirtualPort, const SteamNetworkingIdentity &identityRemote, int nRemoteVirtualPort, bool bOnlySymmetricConnections, CSteamNetworkConnectionP2P *pIgnore )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	Assert( nLocalVirtualPort >= 0 );

	// Check FakeIP
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		switch ( identityRemote.GetFakeIPType() )
		{
			case k_ESteamNetworkingFakeIPType_Invalid:
				// Typical P2P connection
				break;

			case k_ESteamNetworkingFakeIPType_GlobalIPv4:
				if ( !IsVirtualPortGlobalFakePort( nLocalVirtualPort ) )
				{
					Assert( IsVirtualPortEphemeralFakePort( nLocalVirtualPort ) );
					return nullptr;
				}
				break;

			default:
				AssertMsg( false, "Bad FakeIP type %d", identityRemote.GetFakeIPType() );
				return nullptr;
		}
	#endif

	for ( CSteamNetworkConnectionBase *pConn: g_mapConnections.IterValues() )
	{
		if ( pConn->m_pSteamNetworkingSocketsInterface != pInterfaceLocal )
			continue;
		if ( !(  pConn->m_identityRemote == identityRemote ) )
			continue;

		// Check state
		switch ( pConn->GetState() )
		{
			default:
				Assert( false );
			case k_ESteamNetworkingConnectionState_Dead: // NOTE: Dead connections do stay in the map for a brief time while we wait for a safe point to destroy them
			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_FinWait:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
				// Connection no longer alive, we could create a new one
				continue;

			case k_ESteamNetworkingConnectionState_None:
				// Not finished initializing.  But that should only possibly be the case
				// for one connection, one we are in the process of creating.  And so we
				// should be ignoring it.
				Assert( pConn == pIgnore );
				continue;

			case k_ESteamNetworkingConnectionState_Connecting:
			case k_ESteamNetworkingConnectionState_FindingRoute:
			case k_ESteamNetworkingConnectionState_Connected:
			case k_ESteamNetworkingConnectionState_Linger:
				// Yes, it's a possible duplicate
				break;
		}
		if ( bOnlySymmetricConnections && !pConn->BSymmetricMode() )
			continue;
		if ( pConn == pIgnore )
			continue;
		CSteamNetworkConnectionP2P *pConnP2P = pConn->AsSteamNetworkConnectionP2P();
		if ( !pConnP2P )
			continue;
		if ( pConnP2P->m_nRemoteVirtualPort != nRemoteVirtualPort )
			continue;
		if ( pConnP2P->LocalVirtualPort() != nLocalVirtualPort )
			continue;
		return pConnP2P;
	}

	return nullptr;
}

EResult CSteamNetworkConnectionP2P::AcceptConnection( SteamNetworkingMicroseconds usecNow )
{
	AssertLocksHeldByCurrentThread( "P2P::AcceptConnection" );

	// Calling code shouldn't call us unless this is true
	Assert( m_bConnectionInitiatedRemotely );
	Assert( GetState() == k_ESteamNetworkingConnectionState_Connecting );

	// Check symmetric mode.  Note that if they are using the API properly, we should
	// have already detected this earlier!
	if ( BSymmetricMode() )
	{
		if ( CSteamNetworkConnectionP2P::FindDuplicateConnection( m_pSteamNetworkingSocketsInterface, LocalVirtualPort(), m_identityRemote, m_nRemoteVirtualPort, false, this ) )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Cannot accept connection, duplicate symmetric connection already exists" );
			return k_EResultFail;
		}
	}

	return P2PInternalAcceptConnection( usecNow );
}

EResult CSteamNetworkConnectionP2P::P2PInternalAcceptConnection( SteamNetworkingMicroseconds usecNow )
{
	Assert( !IsSDRHostedServer() ); // Those connections use a derived class that overrides this function

	// Spew
	SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Accepting connection, transitioning to 'finding route' state\n", GetDescription() );

	// Check for enabling ICE
	CheckInitICE();

	// At this point, we should have at least one possible transport.  If not, we are sunk.
	if ( m_vecAvailableTransports.empty() && m_pTransport == nullptr )
	{

		// The only way we should be able to get here is if ICE is
		// the only transport that is enabled in this configuration,
		// and it has failed.
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
			Assert( GetICEFailureCode() != 0 );
			ConnectionState_ProblemDetectedLocally( (ESteamNetConnectionEnd)GetICEFailureCode(), "%s", m_szICECloseMsg );
		#else
			Assert( false );
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Generic, "No available transports?" );
		#endif
		return k_EResultFail;
	}

	// Remember that we need to send them a "ConnectOK" message via signaling.
	// But we might wait just a bit before doing so.  Usually we can include
	// a bit more routing info in the message.
	QueueSendConnectOKSignal();

	// WE'RE NOT "CONNECTED" YET!
	// We need to do route negotiation first, which could take several route trips,
	// depending on what ping data we already had before we started.
	ConnectionState_FindingRoute( usecNow );

	// OK
	return k_EResultOK;
}

void CSteamNetworkConnectionP2P::ProcessSNPPing( int msPing, RecvPacketContext_t &ctx )
{
	if ( ctx.m_pTransport == m_pTransport || m_pTransport == nullptr )
		CSteamNetworkConnectionBase::ProcessSNPPing( msPing, ctx );

	// !KLUDGE! Because we cannot upcast.  This list should be short, though
	for ( CConnectionTransportP2PBase *pTransportP2P: m_vecAvailableTransports )
	{
		if ( pTransportP2P->m_pSelfAsConnectionTransport == ctx.m_pTransport )
		{
			pTransportP2P->m_pingEndToEnd.ReceivedPing( msPing, ctx.m_usecNow );
		}
	}
}

int64 CSteamNetworkConnectionP2P::_APISendMessageToConnection( CSteamNetworkingMessage *pMsg, SteamNetworkingMicroseconds usecNow, bool *pbThinkImmediately )
{
	int64 nResult = CSteamNetworkConnectionBase::_APISendMessageToConnection( pMsg, usecNow, pbThinkImmediately );
	if ( nResult > 0 && m_bAppConnectHandshakePacketsInRSVP )
		// FIXME - we probably need to rate limit this.
		ScheduleSendSignal( "ConnectHandshakePacketsInRSVP" );

	return nResult;
}

bool CSteamNetworkConnectionP2P::BSupportsSymmetricMode()
{
	return true;
}

void CSteamNetworkConnectionP2P::TransportEndToEndConnectivityChanged( CConnectionTransportP2PBase *pTransport, SteamNetworkingMicroseconds usecNow )
{
	AssertLocksHeldByCurrentThread( "P2P::TransportEndToEndConnectivityChanged" );

	if ( pTransport->m_bNeedToConfirmEndToEndConnectivity == ( pTransport == m_pCurrentTransportP2P ) )
	{
		// Connectivity was lost on the current transport, gained on a transport not currently selected.
		// This is an event that should cause us to take action
		// Clear any stickiness to current transport, and schedule us to wake up
		// immediately and re-evaluate the situation
		m_bTransportSticky = false;
		m_usecNextEvaluateTransport = k_nThinkTime_ASAP;
	}

	// Reset counter to make sure we collect a few more, either immediately if we can, or when
	// we come back alive.  Also, this makes sure that as soon as we get confirmed connectivity,
	// that we send something to the peer so they can get confirmation, too.
	pTransport->m_nKeepTryingToPingCounter = std::max( pTransport->m_nKeepTryingToPingCounter, 5 );

	// Wake up the connection immediately, either to evaluate transports, or to send packets
	SetNextThinkTimeASAP();

	// Check for recording the time when a transport first became available
	if ( !pTransport->m_bNeedToConfirmEndToEndConnectivity && BStateIsActive() )
	{

		SteamNetworkingMicroseconds usecWhenStartedNegotiation = m_usecWhenStartedFindingRoute;
		if ( usecWhenStartedNegotiation == 0 )
		{
			// It's actually possible for us to confirm end-to-end connectivity before
			// entering the routing finding state.  If we are initiating the connection,
			// we might have sent info to the peer through our connect request which they
			// use to get back to us over the transport, before their COnnectOK reply signal
			// reaches us!
			Assert( GetState() == k_ESteamNetworkingConnectionState_Connecting );
			usecWhenStartedNegotiation = GetTimeEnteredConnectionState();
		}

		// Round to nearest ms, and clamp to 1, to make sure that 0 is not interpreted
		// anywhere as "no data", instead of "incredibly fast", which is actually happening.
		int msNegotiationTime = std::max( 1, (int)( ( usecNow - usecWhenStartedNegotiation + 500 ) / 1000 ) );

		// Which transport?
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
			if ( pTransport == m_pTransportICE && !m_msgICESessionSummary.has_negotiation_ms() )
				m_msgICESessionSummary.set_negotiation_ms( msNegotiationTime );
		#endif
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
			if ( pTransport == m_pTransportP2PSDR && !m_msgSDRRoutingSummary.has_negotiation_ms() )
				m_msgSDRRoutingSummary.set_negotiation_ms( msNegotiationTime );
		#endif

		// Compiler warning if nothing enabled
		(void)msNegotiationTime;
	}
}

void CSteamNetworkConnectionP2P::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// NOTE: Do not call base class, because it it going to
	// call TransportConnectionStateChanged on whatever transport is active.
	// We don't want that here.

	// Take action at certain transitions
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			Assert( false );
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_FinWait:
			m_bNeedToSendConnectOKSignal = false;
			m_bWaitForInitialRoutingReady = false;
			EnsureICEFailureReasonSet( usecNow );
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			break;

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			m_bNeedToSendConnectOKSignal = false;
			m_bWaitForInitialRoutingReady = false;
			EnsureICEFailureReasonSet( usecNow );

			// If we fail during these states, send a signal to Steam, for analytics
			if ( eOldState == k_ESteamNetworkingConnectionState_Connecting || eOldState == k_ESteamNetworkingConnectionState_FindingRoute )
				SendConnectionClosedSignal( usecNow );
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:
			Assert( m_usecWhenStartedFindingRoute == 0 ); // Should only enter this state once
			m_usecWhenStartedFindingRoute = usecNow;
			// |
			// |
			// V
			// FALLTHROUGH
		case k_ESteamNetworkingConnectionState_Connecting:
			m_bTransportSticky = false; // Not sure how we could have set this flag, but make sure and clear it
			// |
			// |
			// V
			// FALLTHROUGH
		case k_ESteamNetworkingConnectionState_Connected:

			// Kick off thinking loop, perhaps taking action immediately
			m_usecNextEvaluateTransport = k_nThinkTime_ASAP;
			SetNextThinkTimeASAP();
			for ( CConnectionTransportP2PBase *pTransportP2P: m_vecAvailableTransports )
				pTransportP2P->EnsureP2PTransportThink( k_nThinkTime_ASAP );

			break;
	}

	// Clear this flag once we leave the handshake phase.  It keeps some logic elsewhere simpler
	if ( m_bAppConnectHandshakePacketsInRSVP
		&& GetState() != k_ESteamNetworkingConnectionState_FindingRoute
		&& GetState() != k_ESteamNetworkingConnectionState_Connecting )
	{
		m_bAppConnectHandshakePacketsInRSVP = false;
		m_bWaitForInitialRoutingReady = false;
	}

	// Inform transports.  If we have a selected transport (or are in a special case) do that one first
	#ifdef SDR_ENABLE_HOSTED_CLIENT
		Assert( !m_pTransportToSDRServer || m_pTransport == m_pTransportToSDRServer );
	#endif
	#ifdef SDR_ENABLE_HOSTED_SERVER
		Assert( !m_pTransportFromSDRClient || m_pTransport == m_pTransportFromSDRClient );
	#endif
	if ( m_pTransport )
		m_pTransport->TransportConnectionStateChanged( eOldState );
	for ( CConnectionTransportP2PBase *pTransportP2P: m_vecAvailableTransports )
	{
		if ( pTransportP2P->m_pSelfAsConnectionTransport != m_pTransport )
			pTransportP2P->m_pSelfAsConnectionTransport->TransportConnectionStateChanged( eOldState );
	}
}

// If nothing scheduled, check RTOs.  If we have something scheduled,
// wait for the timer. The timer is short and designed to avoid
// a blast, so let it do its job.
SteamNetworkingMicroseconds CSteamNetworkConnectionP2P::GetSignalReliableRTO()
{
	SteamNetworkingMicroseconds usecMinRTO = k_nThinkTime_Never;
	for ( const OutboundMessage &s: m_vecUnackedOutboundMessages )
	{
		if ( s.m_usecRTO < usecMinRTO )
			usecMinRTO = s.m_usecRTO;
	}

	return usecMinRTO;
}

void CSteamNetworkConnectionP2P::ThinkConnection( SteamNetworkingMicroseconds usecNow )
{
	CSteamNetworkConnectionBase::ThinkConnection( usecNow );

	CheckCleanupICE();

	// Process route selection if we're ready
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting )
	{
		ThinkSelectTransport( usecNow );
	}

	// Check for sending a signal.  Can't send signals?
	if ( !m_pSignaling )
		return;

	// We can't send our initial signals without certs, etc
	if ( GetState() == k_ESteamNetworkingConnectionState_Connecting )
	{
		if ( !BThinkCryptoReady( usecNow ) )
		{
			EnsureMinThinkTime( usecNow + k_nMillion/20 );
			return;
		}

		// If we're the server, then don't send any signals until
		// the connection is actually accepted.
		if ( m_bConnectionInitiatedRemotely )
			return;
	}

	// Time to send a signal?
	// Limit using really basic minimum spacing between successive calls
	SteamNetworkingMicroseconds usecReliableRTO = GetSignalReliableRTO();
	SteamNetworkingMicroseconds usecNextWantToSend = std::min( usecReliableRTO, m_usecSendSignalDeadline );
	SteamNetworkingMicroseconds usecNextSend = std::max( usecNextWantToSend, GetWhenCanSendNextP2PSignal() );
	if ( usecNextSend > usecNow )
	{
		EnsureMinThinkTime( usecNextSend );
		return;
	}

	// Check if we should delay sending a signal until
	// we collect a bit of initial routing info
	SteamNetworkingMicroseconds usecRoutingReady = CheckWaitForInitialRoutingReady( usecNow );
	if ( usecRoutingReady > usecNow )
	{
		EnsureMinThinkTime( usecRoutingReady );
		return;
	}

	// OK, we're gonna send something.  Is it because of reliable RTO,
	// then we might not have a reason set yet, so set one now.
	const char *pszDebugReason = m_pszNeedToSendSignalReason;
	if ( !pszDebugReason )
	{
		Assert( m_usecSendSignalDeadline == k_nThinkTime_Never );
		Assert( usecReliableRTO <= usecNow );
		pszDebugReason = "ReliableRTO";
	}

	// Send a signal
	CMsgSteamNetworkingP2PRendezvous msgRendezvous;
	if ( !SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, pszDebugReason ) )
		return;
	Assert( m_usecWhenSentLastSignal == usecNow );
	Assert( m_usecSendSignalDeadline == k_nThinkTime_Never );
}

void CSteamNetworkConnectionP2P::ThinkSelectTransport( SteamNetworkingMicroseconds usecNow )
{

	// If no available transports, then nothing to think about.
	// (This will be the case if we are using a special transport type that isn't P2P.)
	if ( m_vecAvailableTransports.empty() )
	{
		m_usecNextEvaluateTransport = k_nThinkTime_Never;
		m_bTransportSticky = true;
		return;
	}

	// Time to evaluate which transport to use?
	if ( usecNow < m_usecNextEvaluateTransport )
	{
		EnsureMinThinkTime( m_usecNextEvaluateTransport );
		return;
	}

	AssertLocksHeldByCurrentThread( "P2P::ThinkSelectTRansport" );

	// Reset timer to evaluate transport at certain times
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			Assert( false );
			// FALLTHROUGH

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_Connecting: // wait for signaling to complete
			m_usecNextEvaluateTransport = k_nThinkTime_Never;
			return;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_FindingRoute:
			m_usecNextEvaluateTransport = usecNow + k_nMillion; // Check back periodically
			break;
	}

	bool bEvaluateFrequently = false;

	// Make sure extreme penalty numbers make sense
	constexpr int k_nMaxReasonableScore = k_nRoutePenaltyNeedToConfirmConnectivity + k_nRoutePenaltyNotNominated + k_nRoutePenaltyNotSelectedOverride + 2000;
	COMPILE_TIME_ASSERT( k_nMaxReasonableScore >= 0 && k_nMaxReasonableScore*2 < k_nRouteScoreHuge/2 );

	// Scan all the options
	int nCurrentTransportScore = k_nRouteScoreHuge;
	int nBestTransportScore = k_nRouteScoreHuge;
	CConnectionTransportP2PBase *pBestTransport = nullptr;
	for ( CConnectionTransportP2PBase *t: m_vecAvailableTransports )
	{
		// Update metrics
		t->P2PTransportUpdateRouteMetrics( usecNow );

		// Add on a penalty if we need to confirm connectivity
		if ( t->m_bNeedToConfirmEndToEndConnectivity )
			t->m_routeMetrics.m_nTotalPenalty += k_nRoutePenaltyNeedToConfirmConnectivity;

		// If we are the controlled agent, add a penalty to non-nominated transports
		if ( !IsControllingAgent() && m_pPeerSelectedTransport != t )
			t->m_routeMetrics.m_nTotalPenalty += k_nRoutePenaltyNotNominated;

		// Calculate the total score
		int nScore = t->m_routeMetrics.m_nScoreCurrent + t->m_routeMetrics.m_nTotalPenalty;
		if ( t == m_pCurrentTransportP2P )
			nCurrentTransportScore = nScore;
		if ( nScore < nBestTransportScore )
		{
			nBestTransportScore = nScore;
			pBestTransport = t;
		}

		// Should not be using the special "force select this transport" score
		// if we have more than one transport
		Assert( nScore >= 0 || m_vecAvailableTransports.size() == 1 );
	}

	if ( pBestTransport == nullptr )
	{
		// No suitable transports at all?
		SelectTransport( nullptr, usecNow );
	}
	else if ( len( m_vecAvailableTransports ) == 1 )
	{
		// We only have one option.  No use waiting
		SelectTransport( pBestTransport, usecNow );
		m_bTransportSticky = true;
	}
	else if ( pBestTransport->m_bNeedToConfirmEndToEndConnectivity )
	{
		// Don't switch or activate a transport if we are not certain
		// about its connectivity and we might have other options
		m_bTransportSticky = false;
	}
	else if ( m_pCurrentTransportP2P == nullptr )
	{
		m_bTransportSticky = false;

		// We're making the initial decision, or we lost all transports.
		// If we're not the controlling agent, give the controlling agent
		// a bit of time
		if (
			IsControllingAgent() // we're in charge
			|| m_pPeerSelectedTransport == pBestTransport // we want to switch to what the other guy said
			|| GetTimeEnteredConnectionState() + k_usecWaitForControllingAgentBeforeSelectingNonNominatedTransport < usecNow // we've waited long enough
		) {

			// Select something as soon as it becomes available
			SelectTransport( pBestTransport, usecNow );
		}
		else
		{
			// Wait for the controlling agent to make a decision
			bEvaluateFrequently = true;
		}
	}
	else if ( m_pCurrentTransportP2P != pBestTransport )
	{

		const auto &GetStickyPenalizedScore = []( int nScore ) { return nScore * 11 / 10 + 5; };

		// Check for applying a sticky penalty, that the new guy has to
		// overcome to switch
		int nBestScoreWithStickyPenalty = nBestTransportScore;
		if ( m_bTransportSticky )
			nBestScoreWithStickyPenalty = GetStickyPenalizedScore( nBestTransportScore );

		// Still better?
		if ( nBestScoreWithStickyPenalty < nCurrentTransportScore )
		{

			// Make sure we have enough recent ping data to make
			// the switch
			bool bReadyToSwitch = true;
			if ( m_bTransportSticky )
			{

				// We don't have a particular reason to switch, so let's make sure the new option is
				// consistently better than the current option, over a sustained time interval
				if (
					GetStickyPenalizedScore( pBestTransport->m_routeMetrics.m_nScoreMax ) + pBestTransport->m_routeMetrics.m_nTotalPenalty
					 < m_pCurrentTransportP2P->m_routeMetrics.m_nScoreMin + m_pCurrentTransportP2P->m_routeMetrics.m_nTotalPenalty
				) {
					bEvaluateFrequently = true;

					// The new transport is consistently better within all recent samples.  But is that just because
					// we don't have many samples?  If so, let's make sure and collect some
					#define CHECK_READY_TO_SWITCH( pTransport ) \
						if ( pTransport->m_routeMetrics.m_nBucketsValid < k_nRecentValidTimeBucketsToSwitchRoute ) \
						{ \
							bReadyToSwitch = false; \
							SteamNetworkingMicroseconds usecNextPing = pTransport->m_pingEndToEnd.TimeToSendNextAntiFlapRouteCheckPingRequest(); \
							if ( usecNextPing > usecNow ) \
							{ \
								m_usecNextEvaluateTransport = std::min( m_usecNextEvaluateTransport, usecNextPing ); \
							} \
							else if ( pTransport->m_usecEndToEndInFlightReplyTimeout > 0 ) \
							{ \
								/* The transport *should* be scheduled to deal with the timeout */ \
								Assert( pTransport->GetP2PTransportThinkScheduleTime() <= pTransport->m_usecEndToEndInFlightReplyTimeout ); \
								/* In case not, use the max so we don't wake up and try to deal with it before it has taken care of it and get into a loop */ \
								SteamNetworkingMicroseconds usecAfterTimeOut = std::max( pTransport->GetP2PTransportThinkScheduleTime(), pTransport->m_usecEndToEndInFlightReplyTimeout ); \
								++usecAfterTimeOut; /* because we need to make sure we do our processing AFTER the transport has had a chance to deal with the timeout */ \
								m_usecNextEvaluateTransport = std::min( m_usecNextEvaluateTransport, usecAfterTimeOut ); \
							} \
							else \
							{ \
								SpewVerbose( "[%s] %s (%d+%d) appears preferable to current transport %s (%d+%d), but maybe transient.  Pinging via %s.", \
									GetDescription(), \
									pBestTransport->m_pszP2PTransportDebugName, \
									pBestTransport->m_routeMetrics.m_nScoreCurrent, pBestTransport->m_routeMetrics.m_nTotalPenalty, \
									m_pCurrentTransportP2P->m_pszP2PTransportDebugName, \
									m_pCurrentTransportP2P->m_routeMetrics.m_nScoreCurrent, m_pCurrentTransportP2P->m_routeMetrics.m_nTotalPenalty, \
									pTransport->m_pszP2PTransportDebugName \
								); \
								pTransport->m_pSelfAsConnectionTransport->SendEndToEndStatsMsg( k_EStatsReplyRequest_Immediate, usecNow, "TransportChangeConfirm" ); \
							} \
						}

					CHECK_READY_TO_SWITCH( pBestTransport )
					CHECK_READY_TO_SWITCH( m_pCurrentTransportP2P )

					#undef CHECK_READY_TO_SWITCH
				}
			}

			if ( bReadyToSwitch )
				SelectTransport( pBestTransport, usecNow );
			else
				bEvaluateFrequently = true;
		}
	}

	// Check for turning on the sticky flag if things look solid
	if (
		m_pCurrentTransportP2P
		&& m_pCurrentTransportP2P == pBestTransport
		&& !m_pCurrentTransportP2P->m_bNeedToConfirmEndToEndConnectivity // Never be sticky do a transport that we aren't sure we can communicate on!
		&& ( IsControllingAgent() || m_pPeerSelectedTransport == m_pCurrentTransportP2P ) // Don't be sticky to a non-nominated transport
	) {
		m_bTransportSticky = true;
	}

	// As soon as we have any viable transport, exit route finding.
	if ( GetState() == k_ESteamNetworkingConnectionState_FindingRoute )
	{
		if ( m_pCurrentTransportP2P && !m_pCurrentTransportP2P->m_bNeedToConfirmEndToEndConnectivity )
		{
			ConnectionState_Connected( usecNow );
		}
		else
		{
			bEvaluateFrequently = true;
		}
	}

	// If we're not settled, then make sure we're checking in more frequently
	if ( bEvaluateFrequently || !m_bTransportSticky || m_pCurrentTransportP2P == nullptr || pBestTransport == nullptr || m_pCurrentTransportP2P->m_bNeedToConfirmEndToEndConnectivity || pBestTransport->m_bNeedToConfirmEndToEndConnectivity )
		m_usecNextEvaluateTransport = std::min( m_usecNextEvaluateTransport, usecNow + k_nMillion/20 );

	EnsureMinThinkTime( m_usecNextEvaluateTransport );
}

void CSteamNetworkConnectionP2P::SelectTransport( CConnectionTransportP2PBase *pTransportP2P, SteamNetworkingMicroseconds usecNow )
{
	CConnectionTransport *pTransport = pTransportP2P ? pTransportP2P->m_pSelfAsConnectionTransport : nullptr;

	// No change?
	if ( pTransportP2P == m_pCurrentTransportP2P )
	{
		return;
	}

	AssertLocksHeldByCurrentThread( "P2P::SelectTransport" );

	// Spew about this event
	const int nLogLevel = LogLevel_P2PRendezvous();
	if ( nLogLevel >= k_ESteamNetworkingSocketsDebugOutputType_Verbose )
	{
		if ( pTransportP2P == nullptr )
		{
			if ( BStateIsActive() ) // Don't spew about cleaning up
				ReallySpewTypeFmt( nLogLevel, "[%s] Deselected '%s' transport, no transport currently active!\n", GetDescription(), m_pCurrentTransportP2P->m_pszP2PTransportDebugName );
		}
		else if ( m_pCurrentTransportP2P == nullptr )
		{
			ReallySpewTypeFmt( nLogLevel, "[%s] Selected '%s' transport (ping=%d, score=%d+%d)\n", GetDescription(),
				pTransportP2P->m_pszP2PTransportDebugName, pTransportP2P->m_pingEndToEnd.m_nSmoothedPing, pTransportP2P->m_routeMetrics.m_nScoreCurrent, pTransportP2P->m_routeMetrics.m_nTotalPenalty );
		}
		else
		{
			ReallySpewTypeFmt( nLogLevel, "[%s] Switched to '%s' transport (ping=%d, score=%d+%d) from '%s' (ping=%d, score=%d+%d)\n", GetDescription(),
				pTransportP2P->m_pszP2PTransportDebugName, pTransportP2P->m_pingEndToEnd.m_nSmoothedPing, pTransportP2P->m_routeMetrics.m_nScoreCurrent, pTransportP2P->m_routeMetrics.m_nTotalPenalty,
				m_pCurrentTransportP2P->m_pszP2PTransportDebugName, m_pCurrentTransportP2P->m_pingEndToEnd.m_nSmoothedPing, m_pCurrentTransportP2P->m_routeMetrics.m_nScoreCurrent, m_pCurrentTransportP2P->m_routeMetrics.m_nTotalPenalty
			);
		}
	}

	// Slam the connection end-to-end ping data with values from the new transport
	if ( m_pCurrentTransportP2P )
	{
		static_cast< PingTracker &>( m_statsEndToEnd.m_ping ) = static_cast< const PingTracker &>( m_pCurrentTransportP2P->m_pingEndToEnd ); // Slice cast
		m_statsEndToEnd.m_ping.m_usecTimeLastSentPingRequest = 0;

		// Count up time we were selected
		Assert( m_pCurrentTransportP2P->m_usecWhenSelected );
		m_pCurrentTransportP2P->m_usecTimeSelectedAccumulator = m_pCurrentTransportP2P->CalcTotalTimeSelected( usecNow );
		m_pCurrentTransportP2P->m_usecWhenSelected = 0;
	}

	m_pCurrentTransportP2P = pTransportP2P;
	m_pTransport = pTransport;
	if ( m_pCurrentTransportP2P && len( m_vecAvailableTransports ) == 1 )
	{
		// Only one transport.  Might as well be sticky, and no use evaluating other transports
		m_bTransportSticky = true;
		m_usecNextEvaluateTransport = k_nThinkTime_Never;
	}
	else
	{
		// Assume we won't be sticky for now
		m_bTransportSticky = false;
		m_usecNextEvaluateTransport = k_nThinkTime_ASAP;
	}

	SetDescription();
	SetNextThinkTimeASAP(); // we might want to send packets ASAP

	// Remember when we became active
	if ( m_pCurrentTransportP2P )
	{
		Assert( m_pCurrentTransportP2P->m_usecWhenSelected == 0 );
		m_pCurrentTransportP2P->m_usecWhenSelected = usecNow;
	}

	// Make sure the summaries are updated with the current total time selected
	UpdateTransportSummaries( usecNow );

	// If we're the controlling agent, then send something on this transport ASAP
	if ( m_pTransport && IsControllingAgent() && !IsSDRHostedServerClient() )
	{
		m_pTransport->SendEndToEndStatsMsg( k_EStatsReplyRequest_NoReply, usecNow, "P2PNominate" );
	}
}

void CSteamNetworkConnectionP2P::UpdateTransportSummaries( SteamNetworkingMicroseconds usecNow )
{

	#define UPDATE_SECONDS_SELECTED( pTransport, msg ) \
		if ( pTransport ) \
		{ \
			SteamNetworkingMicroseconds usec = pTransport->CalcTotalTimeSelected( usecNow ); \
			msg.set_selected_seconds( usec <= 0 ? 0 : std::max( 1, (int)( ( usec + 500*1000 ) / k_nMillion ) ) ); \
		}

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		UPDATE_SECONDS_SELECTED( m_pTransportICE, m_msgICESessionSummary )
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		UPDATE_SECONDS_SELECTED( m_pTransportP2PSDR, m_msgSDRRoutingSummary )
	#endif

	#undef UPDATE_SECONDS_SELECTED
}

SteamNetworkingMicroseconds CSteamNetworkConnectionP2P::ThinkConnection_ClientConnecting( SteamNetworkingMicroseconds usecNow )
{
	Assert( !m_bConnectionInitiatedRemotely );
	Assert( m_pParentListenSocket == nullptr );

	// FIXME if we have LAN broadcast enabled, we should send those here.
	// (Do we even need crypto ready for that, if we are gonna allow them to
	// be unauthenticated anyway?)  If so, we will need to refactor the base
	// class to call this even if crypt is not ready.

	// SDR client to hosted dedicated server?  We don't use signaling to make these connect requests.
	if ( IsSDRHostedServerClient() )
	{

		// Base class behaviour, which uses the transport to send end-to-end connect
		// requests, is the right thing to do
		return CSteamNetworkConnectionBase::ThinkConnection_ClientConnecting( usecNow );
	}

	// No signaling?  This should only be possible if we are attempting P2P though LAN
	// broadcast only.
	if ( !m_pSignaling )
	{
		// LAN broadcasts not implemented, so this should currently not be possible.
		AssertMsg( false, "No signaling?" );
		return k_nThinkTime_Never;
	}

	SteamNetworkingMicroseconds usecRoutingReady = CheckWaitForInitialRoutingReady( usecNow );
	if ( usecRoutingReady > usecNow )
		return usecRoutingReady;

	// Time to send another connect request?
	// We always do this through signaling service rendezvous message.  We don't need to have
	// selected the transport (yet)
	SteamNetworkingMicroseconds usecRetry = m_usecWhenSentConnectRequest + k_usecConnectRetryInterval;
	usecRetry = std::min( usecRetry, m_usecSendSignalDeadline );
	if ( usecNow < usecRetry )
		return usecRetry;

	CMsgSteamNetworkingP2PRendezvous msgRendezvous;

	// Send through signaling service
	SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] Sending P2P ConnectRequest\n", GetDescription() );
	if ( SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, "ConnectRequest" ) )
	{
		Assert( m_usecWhenSentConnectRequest == usecNow );
	}

	// And set timeout for retry
	return usecNow + k_usecConnectRetryInterval;
}

SteamNetworkingMicroseconds CSteamNetworkConnectionP2P::CheckWaitForInitialRoutingReady( SteamNetworkingMicroseconds usecNow )
{
	// Check if we already waited or decided we were ready
	if ( !m_bWaitForInitialRoutingReady )
		return k_nThinkTime_ASAP;

	// If we are using SDR, then we want to wait until we have finished the initial ping probes.
	// This makes sure out initial connect message doesn't contain potentially inaccurate
	// routing information.  This delay should only happen very soon after initializing the
	// relay network.
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		if ( m_pTransportP2PSDR )
		{
			// SDR not ready?
			if ( !m_pTransportP2PSDR->BReady() )
			{
				// NOTE: It is actually possible for ICE to have already
				// succeeded here.  (If we are the server and have all of
				// the peer's info.)  So we might consider not waiting in
				// that case.  But this is a relatively fine point, let's
				// not worry about it right now.
				SteamNetworkingMicroseconds usecWaitForSDR = GetTimeEnteredConnectionState() + 2*k_nMillion;
				if ( usecNow < usecWaitForSDR )
					return std::min( usecNow + 20*1000, usecWaitForSDR );
			}

			// SDR is ready, but if the connection was initiated remotely, then
			// we might want to go ahead and establish sessions on the POPs
			// we expect to use.
			if (
				( GetState() == k_ESteamNetworkingConnectionState_FindingRoute )
				&& m_pTransportP2PSDR->m_vecAllRelaySessions.IsEmpty()
				&& m_pTransportP2PSDR->BHaveAnyPeerClusters()
			) {
				// Note that this logic assumes that SDR was ready immediately
				// when we entered the connection state.
				// That won't be true for the first connection, but let's not worry
				// about that.  We could fix this by recording the time
				// when SDR became available.
				SteamNetworkingMicroseconds usecWaitForSDR = GetTimeEnteredConnectionState() + 100*1000;
				if ( usecNow < usecWaitForSDR )
					return std::min( usecNow + 20*1000, usecWaitForSDR );
			}

		}
	#endif

	// When using ICE, it takes just a few milliseconds to collect the local candidates.
	// We'd like to send those in the initial connect request
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( m_pTransportICE && GetICEFailureCode() == 0 && !m_pTransportICE->BCanSendEndToEndData() )
		{
			SteamNetworkingMicroseconds usecWaitForICE = GetTimeEnteredConnectionState();
			uint32 nAllowed = m_msgICESessionSummary.local_candidate_types_allowed();
			uint32 nGathered = m_msgICESessionSummary.local_candidate_types();
			if ( ( nAllowed & k_EICECandidate_Any_Reflexive ) && !( nGathered & k_EICECandidate_Any_Reflexive ) )
			{
				usecWaitForICE += 100*1000;
			}
			else if (
				( nAllowed & (k_EICECandidate_Any_HostPrivate|k_EICECandidate_Any_HostPublic) )
				&& !( nGathered & (k_EICECandidate_Any_HostPrivate|k_EICECandidate_Any_HostPublic) )
			) {
				// Missing something we really ought to be able to immediately
				// determine by iterating adapters, etc.  This is worth waiting
				// for.  10ms is an extremely generous deadline.
				usecWaitForICE += 10*1000;
			}
			if ( usecNow < usecWaitForICE )
				return std::min( usecNow + 10*1000, usecWaitForICE );
		}
	#endif

	// We're ready.  Don't ever check again
	m_bWaitForInitialRoutingReady = false;
	return k_nThinkTime_ASAP;
}


SteamNetworkingMicroseconds CSteamNetworkConnectionP2P::ThinkConnection_FindingRoute( SteamNetworkingMicroseconds usecNow )
{
	#ifdef SDR_ENABLE_HOSTED_CLIENT
		if ( m_pTransportToSDRServer )
		{
			Assert( m_pTransport == m_pTransportToSDRServer );
			return m_pTransportToSDRServer->ThinkFindingRoute( usecNow );
		}
	#endif

	return CSteamNetworkConnectionBase::ThinkConnection_FindingRoute( usecNow );
}

void CSteamNetworkConnectionP2P::QueueSendConnectOKSignal()
{
	if ( !m_bNeedToSendConnectOKSignal )
		SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Queueing ConnectOK signal\n", GetDescription() );
	m_bNeedToSendConnectOKSignal = true;
	ScheduleSendSignal( "ConnectOK" );
}

void CSteamNetworkConnectionP2P::SendConnectionClosedSignal( SteamNetworkingMicroseconds usecNow )
{
	SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Sending graceful P2P ConnectionClosed, remote cxn %u\n", GetDescription(), m_unConnectionIDRemote );

	m_bNeedToSendConnectOKSignal = false;

	CMsgSteamNetworkingP2PRendezvous msgRendezvous;
	CMsgSteamNetworkingP2PRendezvous_ConnectionClosed &msgConnectionClosed = *msgRendezvous.mutable_connection_closed();
	msgConnectionClosed.set_reason_code( m_eEndReason );
	msgConnectionClosed.set_debug( m_szEndDebug );

	// NOTE: Not sending connection stats here.  Usually when a connection is closed through this mechanism,
	// it is because we have not been able to rendezvous, and haven't sent any packets end-to-end anyway

	SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, "ConnectionClosed" );
}

void CSteamNetworkConnectionP2P::SendNoConnectionSignal( SteamNetworkingMicroseconds usecNow )
{
	SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Sending P2P NoConnection signal, remote cxn %u\n", GetDescription(), m_unConnectionIDRemote );

	m_bNeedToSendConnectOKSignal = false;

	CMsgSteamNetworkingP2PRendezvous msgRendezvous;
	CMsgSteamNetworkingP2PRendezvous_ConnectionClosed &msgConnectionClosed = *msgRendezvous.mutable_connection_closed();
	msgConnectionClosed.set_reason_code( k_ESteamNetConnectionEnd_Internal_P2PNoConnection ); // Special reason code that means "do not reply"

	// NOTE: Not sending connection stats here.  Usually when a connection is closed through this mechanism,
	// it is because we have not been able to rendezvous, and haven't sent any packets end-to-end anyway

	SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, "NoConnection" );
}

bool CSteamNetworkConnectionP2P::SetRendezvousCommonFieldsAndSendSignal( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow, const char *pszDebugReason )
{
	if ( !m_pSignaling )
		return false;

	AssertLocksHeldByCurrentThread( "P2P::SetRendezvousCommonFieldsAndSendSignal" );

	// Check if we have a "ConnectOK" message we need to flush out
	if ( m_bNeedToSendConnectOKSignal )
	{
		Assert( m_bConnectionInitiatedRemotely );
		Assert( BStateIsActive() );
		Assert( BCryptKeysValid() );

		CMsgSteamNetworkingP2PRendezvous_ConnectOK &msgConnectOK = *msg.mutable_connect_ok();
		*msgConnectOK.mutable_cert() = m_msgSignedCertLocal;
		*msgConnectOK.mutable_crypt() = m_msgSignedCryptLocal;
		m_bNeedToSendConnectOKSignal = false;
	}

	// If we are the client connecting, then send a connect_request
	// message in every signal.  (That's really the only reason we
	// should be sending a signal.)
	if ( GetState() == k_ESteamNetworkingConnectionState_Connecting && !msg.has_connection_closed() && !m_bConnectionInitiatedRemotely )
	{
		CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest = *msg.mutable_connect_request();
		*msgConnectRequest.mutable_cert() = m_msgSignedCertLocal;
		*msgConnectRequest.mutable_crypt() = m_msgSignedCryptLocal;
		int nLocalVirtualPort = LocalVirtualPort();

		// Connecting via FakeIP?
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
			if ( m_identityRemote.IsFakeIP() )
			{
				Assert( m_nRemoteVirtualPort == -1 ); // We never use remote virtual port for FakeIP connections

				// If we are sending from a global FakeIP, then let them
				// know who we are.
				if ( IsVirtualPortGlobalFakePort( nLocalVirtualPort ) )
				{
					int idxGlobalPort = nLocalVirtualPort - k_nVirtualPort_GlobalFakePort0;

					SteamNetworkingFakeIPResult_t fakeIPLocal;
					m_pSteamNetworkingSocketsInterface->GetFakeIP( idxGlobalPort, &fakeIPLocal );

					// We don't have our fake IP yet.  Try again in a bit
					if ( fakeIPLocal.m_eResult != k_EResultOK )
					{
						if ( fakeIPLocal.m_eResult != k_EResultBusy )
							ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "GetFakeIP returned %d trying to send connect signal", fakeIPLocal.m_eResult );
						return usecNow + 100*1000;
					}

					Assert( fakeIPLocal.m_unIP );
					Assert( fakeIPLocal.m_unPorts[0] );
					msgConnectRequest.set_from_fakeip( CUtlNetAdrRender( fakeIPLocal.m_unIP, fakeIPLocal.m_unPorts[0] ).String() );
				}

				// Don't send any virtual ports in the message
				nLocalVirtualPort = -1;
			}
		#endif

		// put virtual ports into the message
		if ( nLocalVirtualPort >= 0 )
			msgConnectRequest.set_from_virtual_port( nLocalVirtualPort );
		if ( m_nRemoteVirtualPort >= 0 )
			msgConnectRequest.set_to_virtual_port( m_nRemoteVirtualPort );
	}

	Assert( !msg.has_to_connection_id() );
	if ( !msg.has_connect_request() )
	{
		if ( m_unConnectionIDRemote )
		{
			msg.set_to_connection_id( m_unConnectionIDRemote );
		}
		else
		{
			Assert( msg.has_connection_closed() );
		}
	}

	char szTempIdentity[ SteamNetworkingIdentity::k_cchMaxString ];
	if ( !m_identityRemote.IsInvalid() )
	{
		m_identityRemote.ToString( szTempIdentity, sizeof(szTempIdentity) );
		msg.set_to_identity( szTempIdentity );
	}
	m_identityLocal.ToString( szTempIdentity, sizeof(szTempIdentity) );
	msg.set_from_identity( szTempIdentity );
	msg.set_from_connection_id( m_unConnectionIDLocal );

	// Asks transport(s) to put routing info into the message
	PopulateRendezvousMsgWithTransportInfo( msg, usecNow );

	m_pszNeedToSendSignalReason = nullptr;
	m_usecSendSignalDeadline = INT64_MAX;

	// Reliable messages?
	if ( msg.has_connection_closed() )
	{
		// Once connection is closed, discard these, never send again
		m_vecUnackedOutboundMessages.clear();
	}
	else
	{
		bool bInitialHandshake = msg.has_connect_request() || msg.has_connect_ok();

		int nTotalMsgSize = 0;
		for ( OutboundMessage &s: m_vecUnackedOutboundMessages )
		{

			// Not yet ready to retry sending?
			if ( !msg.has_first_reliable_msg() )
			{

				// If we have sent recently, assume it's in flight,
				// and don't give up yet.  Just go ahead and move onto
				// the next once, speculatively sending them before
				// we get our ack for the previously sent ones.
				if ( s.m_usecRTO > usecNow )
				{
					if ( !bInitialHandshake ) // However, always start from the beginning in initial handshake packets
						continue;
				}

				// Try to keep individual signals relatively small.  If we have a lot
				// to say, break it up into multiple messages
				if ( nTotalMsgSize > 800 )
					break;

				// Start sending from this guy forward
				msg.set_first_reliable_msg( s.m_nID );
			}

			*msg.add_reliable_messages() = s.m_msg;
			nTotalMsgSize += s.m_cbSerialized;

			s.m_usecRTO = usecNow + k_usecP2PSignalReliableRTO; // Reset RTO
		}

		// Go ahead and always ack, even if we don't need to, because this is small
		msg.set_ack_reliable_msg( m_nLastRecvRendesvousMessageID );

		// Check for sending application data
		if ( m_bAppConnectHandshakePacketsInRSVP )
		{
			if ( GetState() == k_ESteamNetworkingConnectionState_Connecting || GetState() == k_ESteamNetworkingConnectionState_FindingRoute )
			{
				int cbRemaining = k_cbMaxSendMessagDataInRSVP;
				for (;;)
				{
					CSteamNetworkingMessage *pMsgSend = m_senderState.m_messagesQueued.m_pFirst;
					if ( !pMsgSend )
						break;
					if ( pMsgSend->SNPSend_IsReliable() )
					{
						AssertMsg( false, "[%s] Reliable messages can't be send in signals!", GetDescription() );
						break;
					}
					if ( pMsgSend->m_cbSize > cbRemaining )
					{
						AssertMsg( pMsgSend->m_cbSize <= k_cbMaxSendMessagDataInRSVP, "[%s] Can't send %d-byte message in signal", GetDescription(), pMsgSend->m_cbSize );
						break;
					}
					CMsgSteamNetworkingP2PRendezvous_ApplicationMessage *pAppMsgOut = msg.add_application_messages();
					pAppMsgOut->set_msg_num( pMsgSend->m_nMessageNumber );
					if ( pMsgSend->m_idxLane )
						pAppMsgOut->set_lane_idx( pMsgSend->m_idxLane );
					pAppMsgOut->set_data( pMsgSend->m_pData, pMsgSend->m_cbSize );

					m_senderState.m_cbPendingUnreliable -= pMsgSend->m_cbSize;
					Assert( m_senderState.m_cbPendingUnreliable >= 0 );

					SSNPSenderState::Lane &sendLane = m_senderState.m_vecLanes[ pMsgSend->m_idxLane ];
					sendLane.m_cbPendingUnreliable -= pMsgSend->m_cbSize;
					Assert( sendLane.m_cbPendingUnreliable >= 0 );

					cbRemaining -= pMsgSend->m_cbSize;

					pMsgSend->Unlink();
					pMsgSend->Release();
				}
			}
		}
	}

	// Spew
	int nLogLevel = LogLevel_P2PRendezvous();
	SpewVerboseGroup( nLogLevel, "[%s] Sending P2PRendezvous (%s)\n", GetDescription(), pszDebugReason );
	SpewDebugGroup( nLogLevel, "%s\n\n", Indent( msg.DebugString() ).c_str() );

	int cbMsg = ProtoMsgByteSize( msg );
	uint8 *pMsg = (uint8 *)alloca( cbMsg );
	DbgVerify( msg.SerializeWithCachedSizesToArray( pMsg ) == pMsg+cbMsg );

	// Get connection info to pass to the signal sender
	SteamNetConnectionInfo_t info;
	ConnectionPopulateInfo( info );

	// Send it
	if ( !m_pSignaling->SendSignal( m_hConnectionSelf, info, pMsg, cbMsg ) )
	{
		// NOTE: we might already be closed, either before this call,
		//       or the caller might have closed us!
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Failed to send P2P signal" );
		return false;
	}

	// Mark that we sent it
	m_usecWhenSentLastSignal = usecNow;

	// If we sent a connect request, remember that
	if ( msg.has_connect_request() )
		m_usecWhenSentConnectRequest = usecNow;

	// Check if we might need to schedule another signal
	SteamNetworkingMicroseconds usecNextCheck = std::max( GetSignalReliableRTO(), GetWhenCanSendNextP2PSignal() );
	Assert( usecNextCheck > usecNow );

	EnsureMinThinkTime( usecNextCheck );

	// Once we send our first signal for any reason, don't bother checking
	// to wait for routing info to be ready for the next one.
	m_bWaitForInitialRoutingReady = false;

	// OK, send a signal
	return true;
}

void CSteamNetworkConnectionP2P::PopulateRendezvousMsgWithTransportInfo( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		if ( m_pTransportP2PSDR )
			m_pTransportP2PSDR->PopulateRendezvousMsg( msg, usecNow );
	#endif
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( m_pTransportICE )
			m_pTransportICE->PopulateRendezvousMsg( msg, usecNow );
	#endif
}

bool CSteamNetworkConnectionP2P::ProcessSignal( const CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	AssertLocksHeldByCurrentThread( "P2P::ProcessSignal" );

	// SDR routing?
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR

		// Check for SDR hosted server telling us to contact them via the special protocol
		if ( msg.has_hosted_server_ticket() )
		{
			#ifdef SDR_ENABLE_HOSTED_CLIENT
				if ( !IsSDRHostedServerClient() )
				{
					SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] Peer sent hosted_server_ticket.  Switching to SDR client transport\n", GetDescription() );
					if ( !BSelectTransportToSDRServerFromSignal( msg ) )
						return false;
				}
			#else
				ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_P2P_Rendezvous, "Peer is a hosted dedicated server.  Not supported." );
				return false;
			#endif
		}

		// Go ahead and process the SDR P2P routes, if they are sending them
		if ( m_pTransportP2PSDR )
		{
			if ( msg.has_sdr_routes() )
				m_pTransportP2PSDR->RecvRoutes( msg.sdr_routes() );
			m_pTransportP2PSDR->CheckRecvRoutesAck( msg );
		}
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( msg.has_connection_closed() )
		{
			m_vecUnackedOutboundMessages.clear();
			m_nLastSendRendesvousMessageID = 0;
		}
		if ( !msg.ice_enabled() )
		{
			ICEFailed( k_nICECloseCode_Remote_NotEnabled, "Peer sent signal without ice_enabled set" );

			// An old peer doesn't understand how to ack our messages, so nuke them.
			// note that for a newer peer, we keep them in the queue, even though this is
			// useless.  That's because they are "reliable" messages, and we don't want
			// to add a complication of trying to remove "reliable" messages that cannot
			// be acked.  (Although we could make the optimization to empty them, since we
			// know the peer would discard them.)  At the time of this writing, old peers
			// do not even understand the concept of this reliable message queue, and
			// ICE messages are the only thing that uses it, and so clearing this makes sense.
			// For protocol version 10, we know that this field is ALWAYS set in every signal
			// other than ConnectionClosed.  But we don't want to make any commitments beyond
			// version 10.  (Maybe we want to be able to stop acking after a certain point.)
			if ( !msg.has_ack_reliable_msg() && m_statsEndToEnd.m_nPeerProtocolVersion < 10 )
			{
				Assert( m_nLastRecvRendesvousMessageID == 0 );
				Assert( m_nLastSendRendesvousMessageID == m_vecUnackedOutboundMessages.size() );
				m_vecUnackedOutboundMessages.clear();
				m_nLastSendRendesvousMessageID = 0;
			}
		}
	#endif

	// Closing the connection through rendezvous?
	// (Usually we try to close the connection through the
	// data transport, but in some cases that may not be possible.)
	if ( msg.has_connection_closed() )
	{
		const CMsgSteamNetworkingP2PRendezvous_ConnectionClosed &connection_closed = msg.connection_closed();

		// Give them a reply if appropriate
		if ( connection_closed.reason_code() != k_ESteamNetConnectionEnd_Internal_P2PNoConnection )
			SendNoConnectionSignal( usecNow );

		// Generic state machine take it from here.  (This call does the right
		// thing regardless of the current connection state.)
		if ( connection_closed.reason_code() == k_ESteamNetConnectionEnd_Internal_P2PNoConnection )
		{
			// If we were already closed, this won't actually be "unexpected".  The
			// error message and code we pass here are only used if we are not already
			// closed.
			ConnectionState_ClosedByPeer( k_ESteamNetConnectionEnd_Misc_PeerSentNoConnection, "Received unexpected P2P 'no connection' signal" ); 
		}
		else
		{
			ConnectionState_ClosedByPeer( connection_closed.reason_code(), connection_closed.debug().c_str() ); 
		}
		return true;
	}

	// Check for acking reliable messages
	if ( msg.ack_reliable_msg() > 0 )
	{

		// Remove messages that are being acked
		while ( !m_vecUnackedOutboundMessages.empty() && m_vecUnackedOutboundMessages[0].m_nID <= msg.ack_reliable_msg() )
			erase_at( m_vecUnackedOutboundMessages, 0 );
	}

	// Check if they sent reliable messages
	if ( msg.has_first_reliable_msg() )
	{

		// Send an ack, no matter what
		ScheduleSendSignal( "AckMessages" );

		// Do we have a gap?
		if ( msg.first_reliable_msg() > m_nLastRecvRendesvousMessageID+1 )
		{
			// Something got dropped.  They will need to re-transmit.
			// FIXME We could save these, though, so that if they
			// retransmit, but not everything here, we won't have to ask them
			// for these messages again.  Just discard for now
		}
		else
		{

			// Take the update
			for ( int i = m_nLastRecvRendesvousMessageID+1-msg.first_reliable_msg() ; i < msg.reliable_messages_size() ; ++i )
			{
				++m_nLastRecvRendesvousMessageID;
				const CMsgSteamNetworkingP2PRendezvous_ReliableMessage &reliable_msg = msg.reliable_messages(i);

				#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
					if ( reliable_msg.has_ice() )
					{
						if ( m_pTransportICE )
						{
							m_pTransportICE->RecvRendezvous( reliable_msg.ice(), usecNow );
						}
						else if ( GetState() == k_ESteamNetworkingConnectionState_Connecting && GetICEFailureCode() == 0 )
						{
							m_vecPendingICEMessages.push_back( reliable_msg.ice() );
						}
					}
				#endif

				(void)reliable_msg; // Avoid compiler warning, depending on what transports are available
			}
		}
	}

	// Already closed?
	switch ( GetState() )
	{
		default:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Dead: // shouldn't be in the map!
			Assert( false );
			// FALLTHROUGH
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedSignal( usecNow );
			return true;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			// Must be stray / out of order message, since we think they already closed
			// the connection.
			SendNoConnectionSignal( usecNow );
			return true;

		case k_ESteamNetworkingConnectionState_Connecting:

			if ( msg.has_connect_ok() )
			{
				if ( m_bConnectionInitiatedRemotely )
				{
					SpewWarningGroup( LogLevel_P2PRendezvous(), "[%s] Ignoring P2P connect_ok, since they initiated the connection\n", GetDescription() );
					return false;
				}

				SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] Received ConnectOK in P2P Rendezvous.\n", GetDescription() );
				ProcessSignal_ConnectOK( msg.connect_ok(), usecNow );
			}
			break;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:

			// Now that we know we still might want to talk to them,
			// check for redundant connection request.  (Our reply dropped.)
			if ( msg.has_connect_request() )
			{
				if ( m_bConnectionInitiatedRemotely )
				{
					// NOTE: We're assuming here that it actually is a redundant retry,
					//       meaning they specified all the same parameters as before!
					QueueSendConnectOKSignal();
				}
				else
				{
					AssertMsg( false, "Received ConnectRequest in P2P rendezvous message, but we are the 'client'!" );
				}
			}
			break;
	}

	// Check if they sent actual end-to-end data in the signal.
	for ( const CMsgSteamNetworkingP2PRendezvous_ApplicationMessage &msgAppMsg: msg.application_messages() )
	{
		int idxLane = 0; // (int)msgAppMsg.data().lane_idx() // FIXME - Need to handle growing the lanes and aborting the connection if they try to use too high of a lane number
		ReceivedMessageData( msgAppMsg.data().c_str(), (int)msgAppMsg.data().length(), idxLane, msgAppMsg.msg_num(), msgAppMsg.flags(), usecNow );
	}


	return true;
}

void CSteamNetworkConnectionP2P::ProcessSignal_ConnectOK( const CMsgSteamNetworkingP2PRendezvous_ConnectOK &msgConnectOK, SteamNetworkingMicroseconds usecNow )
{
	Assert( !m_bConnectionInitiatedRemotely );

	// Check the certs, save keys, etc
	if ( !BRecvCryptoHandshake( msgConnectOK.cert(), msgConnectOK.crypt(), false ) )
	{
		Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
		SpewWarning( "Failed crypto init in ConnectOK packet.  %s", m_szEndDebug );
		return;
	}

	// Mark that we received something.  Even though it was through the
	// signaling mechanism, not the channel used for data, and we ordinarily
	// don't count that.
	m_statsEndToEnd.m_usecTimeLastRecv = usecNow;

	// We're not fully connected.  Now we're doing rendezvous
	ConnectionState_FindingRoute( usecNow );
}

ESteamNetConnectionEnd CSteamNetworkConnectionP2P::CheckRemoteCert( const CertAuthScope *pCACertAuthScope, SteamNetworkingErrMsg &errMsg )
{
	// Standard base class connection checks
	ESteamNetConnectionEnd result = CSteamNetworkConnectionBase::CheckRemoteCert( pCACertAuthScope, errMsg );
	if ( result != k_ESteamNetConnectionEnd_Invalid )
		return result;

	// If ticket was bound to a data center, then make sure the cert chain authorizes
	// them to send us there.
	#ifdef SDR_ENABLE_HOSTED_CLIENT
		if ( m_pTransportToSDRServer )
		{
			SteamNetworkingPOPID popIDTicket = m_pTransportToSDRServer->m_authTicket.m_ticket.m_routing.GetPopID();
			if ( popIDTicket != 0 && popIDTicket != k_SteamDatagramPOPID_dev )
			{
				if ( !CheckCertPOPID( m_msgCertRemote, pCACertAuthScope, popIDTicket, errMsg ) )
					return k_ESteamNetConnectionEnd_Remote_BadCert;
			}
		}
	#endif

	return k_ESteamNetConnectionEnd_Invalid;
}

void CSteamNetworkConnectionP2P::QueueSignalReliableMessage( CMsgSteamNetworkingP2PRendezvous_ReliableMessage &&msg, const char *pszDebug )
{
	AssertLocksHeldByCurrentThread();

	SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Queue reliable signal message %s: { %s }\n", GetDescription(), pszDebug, msg.ShortDebugString().c_str() );
	OutboundMessage *p = push_back_get_ptr( m_vecUnackedOutboundMessages );
	p->m_nID = ++m_nLastSendRendesvousMessageID;
	p->m_usecRTO = 1;
	p->m_msg = std::move( msg );
	p->m_cbSerialized = ProtoMsgByteSize(p->m_msg);
	ScheduleSendSignal( pszDebug );
}

void CSteamNetworkConnectionP2P::ScheduleSendSignal( const char *pszReason )
{
	SteamNetworkingMicroseconds usecDeadline = SteamNetworkingSockets_GetLocalTimestamp() + 10*1000;
	if ( !m_pszNeedToSendSignalReason || m_usecSendSignalDeadline > usecDeadline )
	{
		m_pszNeedToSendSignalReason = pszReason;
		m_usecSendSignalDeadline = usecDeadline;
	}
	EnsureMinThinkTime( m_usecSendSignalDeadline );
}

void CSteamNetworkConnectionP2P::PeerSelectedTransportChanged()
{
	AssertLocksHeldByCurrentThread();

	// If we are not the controlling agent, then we probably need to switch
	if ( !IsControllingAgent() && m_pPeerSelectedTransport != m_pCurrentTransportP2P )
	{
		m_usecNextEvaluateTransport = k_nThinkTime_ASAP;
		m_bTransportSticky = false;
		SetNextThinkTimeASAP();
	}

	if ( m_pPeerSelectedTransport )
		SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] Peer appears to be using '%s' transport as primary\n", GetDescription(), m_pPeerSelectedTransport->m_pszP2PTransportDebugName );
}

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_DIAGNOSTICSUI

void CSteamNetworkConnectionP2P::ConnectionPopulateDiagnostics( ESteamNetworkingConnectionState eOldState, CGameNetworkingUI_ConnectionState &msgConnectionState, SteamNetworkingMicroseconds usecNow )
{
	AssertLocksHeldByCurrentThread();
	CSteamNetworkConnectionBase::ConnectionPopulateDiagnostics( eOldState, msgConnectionState, usecNow );

	CMsgSteamDatagramP2PRoutingSummary &p2p_routing = *msgConnectionState.mutable_p2p_routing();
	PopulateP2PRoutingSummary( p2p_routing );

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( m_pTransportICE )
		{
			if ( m_pTransportICE->m_pingEndToEnd.m_nSmoothedPing >= 0 )
			{
				msgConnectionState.set_ping_default_internet_route( m_pTransportICE->m_pingEndToEnd.m_nSmoothedPing );
			}
		}
		else
		{
			if ( p2p_routing.has_ice() )
			{
				const CMsgSteamNetworkingICESessionSummary &ice = p2p_routing.ice();
				if ( ice.has_initial_ping() )
					msgConnectionState.set_ping_default_internet_route( ice.initial_ping() );
			}
		}
	#endif
}

#endif

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingSockets CConnectionTransportP2PBase
//
/////////////////////////////////////////////////////////////////////////////

CConnectionTransportP2PBase::CConnectionTransportP2PBase( const char *pszDebugName, CConnectionTransport *pSelfBase )
: m_pszP2PTransportDebugName( pszDebugName )
, m_pSelfAsConnectionTransport( pSelfBase )
{
	m_pingEndToEnd.Reset();
	m_usecEndToEndInFlightReplyTimeout = 0;
	m_nReplyTimeoutsSinceLastRecv = 0;
	m_nKeepTryingToPingCounter = 5;
	m_usecWhenSelected = 0;
	m_usecTimeSelectedAccumulator = 0;
	m_bNeedToConfirmEndToEndConnectivity = true;
	m_routeMetrics.SetInvalid();
}

CConnectionTransportP2PBase::~CConnectionTransportP2PBase()
{
	CSteamNetworkConnectionP2P &conn = Connection();
	conn.AssertLocksHeldByCurrentThread();

	// Detach from parent connection
	find_and_remove_element( conn.m_vecAvailableTransports, this );

	Assert( ( conn.m_pTransport == m_pSelfAsConnectionTransport ) == ( conn.m_pCurrentTransportP2P == this ) );
	if ( conn.m_pTransport == m_pSelfAsConnectionTransport || conn.m_pCurrentTransportP2P == this )
		conn.SelectTransport( nullptr, SteamNetworkingSockets_GetLocalTimestamp() );
	if ( conn.m_pPeerSelectedTransport == this )
		conn.m_pPeerSelectedTransport = nullptr;

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		if ( conn.m_pTransportP2PSDR == this )
			conn.m_pTransportP2PSDR = nullptr;
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( conn.m_pTransportICE == this )
			conn.m_pTransportICE = nullptr;
		if ( conn.m_pTransportICEPendingDelete == this )
			conn.m_pTransportICEPendingDelete = nullptr;
	#endif

	// Make sure we re-evaluate transport
	conn.m_usecNextEvaluateTransport = k_nThinkTime_ASAP;
	conn.SetNextThinkTimeASAP();
}

void CConnectionTransportP2PBase::P2PTransportTrackSentEndToEndPingRequest( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
{
	m_pingEndToEnd.m_usecTimeLastSentPingRequest = usecNow;
	if ( m_usecEndToEndInFlightReplyTimeout == 0 )
	{
		if ( m_nKeepTryingToPingCounter > 0 )
			--m_nKeepTryingToPingCounter;
		m_usecEndToEndInFlightReplyTimeout = usecNow + m_pingEndToEnd.CalcConservativeTimeout();
		if ( bAllowDelayedReply )
			m_usecEndToEndInFlightReplyTimeout += k_usecSteamDatagramRouterPendClientPing; // Is this the appropriate constant to use?

		EnsureP2PTransportThink( m_usecEndToEndInFlightReplyTimeout );
	}
}

void CConnectionTransportP2PBase::P2PTransportThink( SteamNetworkingMicroseconds usecNow )
{
	CSteamNetworkConnectionP2P &conn = Connection();
	conn.AssertLocksHeldByCurrentThread( "P2PTransportThink" );

	// We only need to take action while connecting, or trying to connect
	switch ( conn.GetState() )
	{

		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_Linger:
			break;

		default:

			// We'll have to wait until we get a callback
			return;
	}

	// Check for reply timeout
	if ( m_usecEndToEndInFlightReplyTimeout )
	{
		if ( m_usecEndToEndInFlightReplyTimeout <= usecNow )
		{
			m_usecEndToEndInFlightReplyTimeout = 0;
			++m_nReplyTimeoutsSinceLastRecv;
			if ( m_nReplyTimeoutsSinceLastRecv > 2 && !m_bNeedToConfirmEndToEndConnectivity )
			{
				SpewMsg( "[%s] %s: %d consecutive end-to-end timeouts\n",
					conn.GetDescription(), m_pszP2PTransportDebugName, m_nReplyTimeoutsSinceLastRecv );
				P2PTransportEndToEndConnectivityNotConfirmed( usecNow );
				conn.TransportEndToEndConnectivityChanged( this, usecNow );
			}
		}
	}

	// Check back in periodically
	SteamNetworkingMicroseconds usecNextThink = usecNow + 2*k_nMillion;

	// Check for sending ping requests
	if ( m_usecEndToEndInFlightReplyTimeout == 0 && m_pSelfAsConnectionTransport->BCanSendEndToEndData() )
	{

		// Check for pinging as fast as possible until we get an initial ping sample.
		CConnectionTransportP2PBase *pCurrentP2PTransport = Connection().m_pCurrentTransportP2P;
		if ( m_nKeepTryingToPingCounter > 0 )
		{
			m_pSelfAsConnectionTransport->SendEndToEndStatsMsg( k_EStatsReplyRequest_Immediate, usecNow, "End-to-end ping sample" );
		}
		else if ( 
			pCurrentP2PTransport == this // they have selected us
			|| pCurrentP2PTransport == nullptr // They haven't selected anybody
			|| pCurrentP2PTransport->m_bNeedToConfirmEndToEndConnectivity // current transport is not in good shape
		) {

			// We're a viable option right now, not just a backup
			if ( 
				// Some reason to establish connectivity or collect more data?
				m_bNeedToConfirmEndToEndConnectivity
				|| m_nReplyTimeoutsSinceLastRecv > 0
				|| m_pingEndToEnd.m_nSmoothedPing < 0
				|| m_pingEndToEnd.m_nValidPings < V_ARRAYSIZE(m_pingEndToEnd.m_arPing)
				|| m_pingEndToEnd.m_nTotalPingsReceived < 10
			) {
				m_pSelfAsConnectionTransport->SendEndToEndStatsMsg( k_EStatsReplyRequest_Immediate, usecNow, "Connectivity check" );
			}
			else
			{
				// We're the current transport and everything looks good.  We will let
				// the end-to-end keepalives handle things, no need to take our own action here.
			}
		}
		else
		{
			// They are using some other transport.  Just ping every now and then
			// so that if conditions change, we could discover that we are better
			SteamNetworkingMicroseconds usecNextPing = m_pingEndToEnd.m_usecTimeLastSentPingRequest + 10*k_nMillion;
			if ( usecNextPing <= usecNow )
			{
				m_pSelfAsConnectionTransport->SendEndToEndStatsMsg( k_EStatsReplyRequest_DelayedOK, usecNow, "P2PGrassGreenerCheck" );
			}
			else
			{
				usecNextThink = std::min( usecNextThink, usecNextPing );
			}
		}
	}

	if ( m_usecEndToEndInFlightReplyTimeout )
		usecNextThink = std::min( usecNextThink, m_usecEndToEndInFlightReplyTimeout );
	EnsureP2PTransportThink( usecNextThink );
}

void CConnectionTransportP2PBase::P2PTransportEndToEndConnectivityNotConfirmed( SteamNetworkingMicroseconds usecNow )
{
	if ( !m_bNeedToConfirmEndToEndConnectivity )
		return;
	CSteamNetworkConnectionP2P &conn = Connection();
	SpewWarningGroup( conn.LogLevel_P2PRendezvous(), "[%s] %s end-to-end connectivity lost\n", conn.GetDescription(), m_pszP2PTransportDebugName );
	m_bNeedToConfirmEndToEndConnectivity = true;
	conn.TransportEndToEndConnectivityChanged( this, usecNow );
}

void CConnectionTransportP2PBase::P2PTransportEndToEndConnectivityConfirmed( SteamNetworkingMicroseconds usecNow )
{
	CSteamNetworkConnectionP2P &conn = Connection();

	if ( !m_pSelfAsConnectionTransport->BCanSendEndToEndData() )
	{
		AssertMsg2( false, "[%s] %s trying to mark connectivity as confirmed, but !BCanSendEndToEndData!", conn.GetDescription(), m_pszP2PTransportDebugName );
		return;
	}

	if ( m_bNeedToConfirmEndToEndConnectivity )
	{
		SpewVerboseGroup( conn.LogLevel_P2PRendezvous(), "[%s] %s end-to-end connectivity confirmed\n", conn.GetDescription(), m_pszP2PTransportDebugName );
		m_bNeedToConfirmEndToEndConnectivity = false;
		conn.TransportEndToEndConnectivityChanged( this, usecNow );
	}
}

SteamNetworkingMicroseconds CConnectionTransportP2PBase::CalcTotalTimeSelected( SteamNetworkingMicroseconds usecNow ) const
{
	SteamNetworkingMicroseconds result = m_usecTimeSelectedAccumulator;
	if ( m_usecWhenSelected > 0 )
	{
		SteamNetworkingMicroseconds whenEnded = Connection().m_statsEndToEnd.m_usecWhenEndedConnectedState;
		if ( whenEnded == 0 )
			whenEnded = usecNow;
		Assert( whenEnded >= m_usecWhenSelected );
		result += usecNow - m_usecWhenSelected;
	}
	return result;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingSockets P2P stuff
//
/////////////////////////////////////////////////////////////////////////////

HSteamListenSocket CSteamNetworkingSockets::CreateListenSocketP2P( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{
	// Despite the API argument being an int, we'd like to reserve most of the address space.
	if ( nLocalVirtualPort < 0 || nLocalVirtualPort > 0xffff )
	{
		SpewError( "Virtual port number must be a small, positive number" );
		return k_HSteamListenSocket_Invalid;
	}

	SteamNetworkingGlobalLock scopeLock( "CreateListenSocketP2P" );

	CSteamNetworkListenSocketP2P *pSock = InternalCreateListenSocketP2P( nLocalVirtualPort, nOptions, pOptions );
	if ( pSock )
		return pSock->m_hListenSocketSelf;
	return k_HSteamListenSocket_Invalid;
}

HSteamListenSocket CSteamNetworkingSockets::CreateListenSocketP2PFakeIP( int idxFakePort, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		SteamNetworkingGlobalLock scopeLock( "CreateListenSocketP2PFakeIP" );

		if ( idxFakePort < 0 || idxFakePort >= m_nFakeIPPortsRequested )
		{
			SpewBug( "CreateListenSocketP2PFakeIP: Invalid fake port index %d (%d requested)", idxFakePort, m_nFakeIPPortsRequested );
			return k_HSteamListenSocket_Invalid;
		}

		int nLocalVirtualPort = k_nVirtualPort_GlobalFakePort0 + idxFakePort;
		Assert( nLocalVirtualPort >= k_nVirtualPort_GlobalFakePort0 && nLocalVirtualPort <= k_nVirtualPort_GlobalFakePortMax );
		CSteamNetworkListenSocketP2P *pSock = InternalCreateListenSocketP2P( nLocalVirtualPort, nOptions, pOptions );
		if ( pSock )
			return pSock->m_hListenSocketSelf;
	#else
		AssertMsg( false, "FakeIP allocation requires Steam" );
	#endif
	return k_HSteamListenSocket_Invalid;
}

CSteamNetworkListenSocketP2P *CSteamNetworkingSockets::InternalCreateListenSocketP2P( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "InternalCreateListenSocketP2P" );

	SteamDatagramErrMsg errMsg;

	// We'll need a cert.  Start sure async process to get one is in
	// progress (or try again if we tried earlier and failed)
	AuthenticationNeeded();

	// Figure out what kind of socket to create.
	// Hosted dedicated server?
	CSteamNetworkListenSocketP2P *pSock = nullptr;
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		CSteamNetworkingSocketsSDR *pSteamNetworkingSocketsSDR = assert_cast< CSteamNetworkingSocketsSDR *>( this );

		#ifdef SDR_ENABLE_HOSTED_SERVER
			if ( pSteamNetworkingSocketsSDR->GetHostedDedicatedServerPort() != 0 )
			{
				if ( !pSteamNetworkingSocketsSDR->m_bGameServer )
				{
					// It's totally possible that this works fine.  But it's weird and untested, and
					// almost certainly a bug somewhere, so let's just disallow it until we know what
					// the use case is.
					AssertMsg( false, "Can't create a P2P listen socket on a 'user' interface in a hosted dedicated server" );
					return nullptr;
				}
				pSock = new CSteamNetworkListenSocketSDRServer( pSteamNetworkingSocketsSDR );
			}
		#endif

		if ( !pSock )
		{
			// We're not in a hosted dedicated server, so it's the usual P2P stuff.
			if ( !pSteamNetworkingSocketsSDR->BSDRClientInit( errMsg ) )
				return nullptr;
		}
	#endif

	// Ordinary case where we are not at known data center?
	if ( !pSock )
	{
		pSock = new CSteamNetworkListenSocketP2P( this );
		if ( !pSock )
			return nullptr;
	}

	// Create listen socket
	if ( !pSock->BInit( nLocalVirtualPort, nOptions, pOptions, errMsg ) )
	{
		SpewError( "Cannot create listen socket.  %s", errMsg );
		pSock->Destroy();
		return nullptr;
	}

	return pSock;
}

HSteamNetConnection CSteamNetworkingSockets::ConnectP2P( const SteamNetworkingIdentity &identityRemote, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{

	// Check remote virtual port
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
	if ( identityRemote.IsFakeIP() )
	{
		if ( nRemoteVirtualPort != -1 )
		{
			SpewBug( "Must specify remote virtual port -1 when connecting by FakeIP!" );
			return k_HSteamNetConnection_Invalid;
		}

		// Cannot specify a local port
		for ( int idxOpt = 0 ; idxOpt < nOptions ; ++idxOpt )
		{
			if ( pOptions[idxOpt].m_eValue == k_ESteamNetworkingConfig_LocalVirtualPort )
			{
				SpewBug( "Cannot specify LocalVirtualPort when connecting by FakeIP" );
				return k_HSteamNetConnection_Invalid;
			}
		}
	}
	else
	#endif
	{
		// Despite the API argument being an int, we'd like to reserve most of the address space.
		if ( nRemoteVirtualPort < 0 || nRemoteVirtualPort > 0xffff )
		{
			SpewBug( "Virtual port number should be a small, non-negative number\n" );
			return k_HSteamNetConnection_Invalid;
		}
	}

	SteamNetworkingGlobalLock scopeLock( "ConnectP2P" );
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = InternalConnectP2PDefaultSignaling( identityRemote, nRemoteVirtualPort, nOptions, pOptions, connectionLock );
	if ( pConn )
		return pConn->m_hConnectionSelf;
	return k_HSteamNetConnection_Invalid;
}

CSteamNetworkConnectionBase *CSteamNetworkingSockets::InternalConnectP2PDefaultSignaling(
	const SteamNetworkingIdentity &identityRemote,
	int nRemoteVirtualPort,
	int nOptions, const SteamNetworkingConfigValue_t *pOptions,
	ConnectionScopeLock &scopeLock
)
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "InternalConnectP2PDefaultSignaling" );
	if ( identityRemote.IsInvalid() )
	{
		AssertMsg( false, "Invalid identity" );
		return nullptr;
	}

	SteamDatagramErrMsg errMsg;

	// Check for connecting to an identity in this process.  In some test environments we may intentionally
	// disable this optimization to force two clients to talk to each other through the relay
	if ( m_TEST_bEnableP2PLoopbackOptimization )
	{
		for ( CSteamNetworkingSockets *pServerInstance: CSteamNetworkingSockets::s_vecSteamNetworkingSocketsInstances )
		{
			if ( pServerInstance->BMatchesIdentity( identityRemote ) )
			{

				CSteamNetworkListenSocketP2P *pListenSocket = nullptr;

				// This is the guy we want to talk to.  Locate the listen socket

				#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
				if ( identityRemote.IsFakeIP() )
				{
					Assert( identityRemote.GetFakeIPType() == k_ESteamNetworkingFakeIPType_GlobalIPv4 ); // We cannot initiate connections to ephemeral addresses!
					Assert( nRemoteVirtualPort == -1 );
					int idxFakePort = pServerInstance->GetFakePortIndex( identityRemote.m_ip );
					Assert( idxFakePort >= 0 ); // Else why did BMatchesIdentity return true?

					int nRemoteVirtualPortToSearch = k_nVirtualPort_GlobalFakePort0 + idxFakePort;
					int idx = pServerInstance->m_mapListenSocketsByVirtualPort.Find( nRemoteVirtualPortToSearch );
					if ( idx == pServerInstance->m_mapListenSocketsByVirtualPort.InvalidIndex() )
					{
						SpewBug( "Cannot create P2P connection to local identity %s.  That is our FakeIP, but we aren't listening on fake port %d",
							SteamNetworkingIdentityRender( identityRemote ).c_str(), identityRemote.m_ip.m_port );
						return nullptr;
					}
					pListenSocket = pServerInstance->m_mapListenSocketsByVirtualPort[ idx ];

				}
				else
				#endif
				{
					int idx = pServerInstance->m_mapListenSocketsByVirtualPort.Find( nRemoteVirtualPort );
					if ( idx == pServerInstance->m_mapListenSocketsByVirtualPort.InvalidIndex() )
					{
						SpewBug( "Cannot create P2P connection to local identity %s.  We are not listening on %s",
							SteamNetworkingIdentityRender( identityRemote ).c_str(), VirtualPortRender( nRemoteVirtualPort ).c_str() );
						return nullptr;
					}
					pListenSocket = pServerInstance->m_mapListenSocketsByVirtualPort[ idx ];
				}

				// Create a loopback connection
				CSteamNetworkConnectionPipe *pConn = CSteamNetworkConnectionPipe::CreateLoopbackConnection(
					this,
					nOptions, pOptions,
					pListenSocket, identityRemote,
					errMsg, scopeLock );
				if ( pConn )
				{
					SpewVerbose( "[%s] Using loopback for P2P connection to local identity %s on %s.  Partner is [%s]\n",
						pConn->GetDescription(),
						SteamNetworkingIdentityRender( identityRemote ).c_str(), VirtualPortRender( nRemoteVirtualPort ).c_str(),
						pConn->m_pPartner->GetDescription() );
					return pConn;
				}

				// Failed?
				SpewBug( "P2P connection to local identity %s on %s; FAILED to create loopback.  %s\n",
					SteamNetworkingIdentityRender( identityRemote ).c_str(), VirtualPortRender( nRemoteVirtualPort ).c_str(), errMsg );
				return nullptr;
			}
		}
	}

	// What local virtual port will be used?
	int nLocalVirtualPort = nRemoteVirtualPort;
	for ( int idxOpt = 0 ; idxOpt < nOptions ; ++idxOpt )
	{
		if ( pOptions[idxOpt].m_eValue == k_ESteamNetworkingConfig_LocalVirtualPort )
		{
			if ( pOptions[idxOpt].m_eDataType == k_ESteamNetworkingConfig_Int32 )
			{
				nLocalVirtualPort = pOptions[idxOpt].m_val.m_int32;
			}
			else
			{
				SpewBug( "LocalVirtualPort must be Int32" );
				return nullptr;
			}
		}
	}

	// Make sure local virtual port is reasonable
	if ( nLocalVirtualPort == -1 )
	{
		// OK, unspecified
	}
	else if ( nLocalVirtualPort <= 0xffff )
	{
		// Ordinary P2P virtual port
		Assert( nRemoteVirtualPort >= -1 && nRemoteVirtualPort <= 0xffff );
	}
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
	else if ( nLocalVirtualPort == k_nVirtualPort_Messages )
	{
		Assert( nRemoteVirtualPort == k_nVirtualPort_Messages );
	}
	#endif
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
	else if ( IsVirtualPortFakePort( nLocalVirtualPort ) )
	{
		if ( !identityRemote.IsFakeIP() )
		{
			// Should have rejected this earlier
			AssertMsg( false, "vport 0x%x only valid when connecting to FakeIP", nLocalVirtualPort );
			return nullptr;
		}
	}
	#endif
	else
	{
		SpewBug( "Invalid LocalVirtualPort %d", nLocalVirtualPort );
		return nullptr;
	}

	// Check local virtual port and FakeIP
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		if ( identityRemote.IsFakeIP() )
		{
			Assert( nRemoteVirtualPort == -1 );

			COMPILE_TIME_ASSERT( k_nVirtualPort_EphemeralFakePortMax+1 == k_nVirtualPort_GlobalFakePort0 );
			if ( nLocalVirtualPort == -1 )
			{
				// OK, unspecified
			}
			else if ( IsVirtualPortFakePort( nLocalVirtualPort ) )
			{
				// We probably could add additional checks here,
				// but this is good enough for now
			}
			else
			{
				// User shouldn't be able to trigger this -- it's our bug
				AssertMsg( false, "Bad vport 0x%x connecting to FakeIP", nLocalVirtualPort );
				return nullptr;
			}
		}
	#endif

	// Create signaling
	FnSteamNetworkingSocketsCreateConnectionSignaling fnCreateConnectionSignaling = (FnSteamNetworkingSocketsCreateConnectionSignaling)GlobalConfig::Callback_CreateConnectionSignaling.Get();
	if ( fnCreateConnectionSignaling == nullptr )
	{
		SpewBug( "Cannot use P2P connectivity.  CreateConnectionSignaling callback not set" );
		return nullptr;
	}
	ISteamNetworkingConnectionSignaling *pSignaling = (*fnCreateConnectionSignaling)( this, identityRemote, nLocalVirtualPort, nRemoteVirtualPort );
	if ( !pSignaling )
		return nullptr;

	// Use the generic path
	CSteamNetworkConnectionBase *pResult = InternalConnectP2P( pSignaling, &identityRemote, nRemoteVirtualPort, nOptions, pOptions, scopeLock );

	// Confirm that we properly knew what the local virtual port would be
	Assert( !pResult || pResult->m_connectionConfig.LocalVirtualPort.Get() == nLocalVirtualPort );

	// Done
	return pResult;
}

HSteamNetConnection CSteamNetworkingSockets::ConnectP2PCustomSignaling( ISteamNetworkingConnectionSignaling *pSignaling, const SteamNetworkingIdentity *pPeerIdentity, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{
	if ( !pSignaling )
		return k_HSteamNetConnection_Invalid;

	SteamNetworkingGlobalLock scopeLock( "ConnectP2PCustomSignaling" );
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = InternalConnectP2P( pSignaling, pPeerIdentity, nRemoteVirtualPort, nOptions, pOptions, connectionLock );
	if ( pConn )
		return pConn->m_hConnectionSelf;
	return k_HSteamNetConnection_Invalid;
}

CSteamNetworkConnectionBase *CSteamNetworkingSockets::InternalConnectP2P(
	ISteamNetworkingConnectionSignaling *pSignaling,
	const SteamNetworkingIdentity *pPeerIdentity,
	int nRemoteVirtualPort,
	int nOptions, const SteamNetworkingConfigValue_t *pOptions,
	ConnectionScopeLock &scopeLock
)
{
	CSteamNetworkConnectionP2P *pConn = new CSteamNetworkConnectionP2P( this, scopeLock );
	if ( !pConn )
	{
		pSignaling->Release();
		return nullptr;
	}

	SteamDatagramErrMsg errMsg;
	CSteamNetworkConnectionP2P *pMatchingConnection = nullptr;
	if ( pConn->BInitConnect( pSignaling, pPeerIdentity, nRemoteVirtualPort, nOptions, pOptions, &pMatchingConnection, errMsg ) )
		return pConn;

	// Failed.  Destroy the failed connection
	pConn->ConnectionQueueDestroy();
	scopeLock.Unlock();
	pConn = nullptr;

	// Did we fail because we found an existing matching connection?
	if ( pMatchingConnection )
	{
		scopeLock.Lock( *pMatchingConnection, "InternalConnectP2P Matching Accept" );

		// If connection is inbound, then we can just implicitly accept it.
		if ( !pMatchingConnection->m_bConnectionInitiatedRemotely || pMatchingConnection->GetState() != k_ESteamNetworkingConnectionState_Connecting )
		{
			V_sprintf_safe( errMsg, "Found existing connection [%s].  Only one symmetric connection can be active at a time.", pMatchingConnection->GetDescription() );
		}
		else
		{
			SpewVerbose( "[%s] Accepting inbound connection implicitly, based on matching outbound connect request\n", pMatchingConnection->GetDescription() );

			// OK, we can try to accept this connection.  HOWEVER, first let's apply
			// any connection options the caller is passing in.
			if ( pOptions )
			{
				for ( int i = 0 ; i < nOptions ; ++i )
				{
					const SteamNetworkingConfigValue_t &opt = pOptions[i];

					// Skip these, they are locked
					if ( opt.m_eValue == k_ESteamNetworkingConfig_LocalVirtualPort || opt.m_eValue == k_ESteamNetworkingConfig_SymmetricConnect )
						continue;

					// Set the option
					if ( !m_pSteamNetworkingUtils->SetConfigValueStruct( opt, k_ESteamNetworkingConfig_Connection, pMatchingConnection->m_hConnectionSelf ) )
					{
						// Spew, but keep going!
						SpewBug( "[%s] Failed to set option %d while implicitly accepting.  Ignoring failure!", pMatchingConnection->GetDescription(), opt.m_eValue );
					}
				}
			}
			else
			{
				Assert( nOptions == 0 );
			}

			// Implicitly accept connection
			EResult eAcceptResult = pMatchingConnection->AcceptConnection( SteamNetworkingSockets_GetLocalTimestamp() );
			if ( eAcceptResult == k_EResultOK )
			{

				// All good!  Return the incoming connection that was accepted
				return pMatchingConnection;
			}

			V_sprintf_safe( errMsg, "Failed to implicitly accept [%s], return code %d", pMatchingConnection->GetDescription(), eAcceptResult );
		}
	}

	// Failed
	if ( pPeerIdentity )
		SpewError( "Cannot create P2P connection to %s.  %s", SteamNetworkingIdentityRender( *pPeerIdentity ).c_str(), errMsg );
	else
		SpewError( "Cannot create P2P connection.  %s", errMsg );
	return nullptr;
}

static void SendP2PRejection( ISteamNetworkingSignalingRecvContext *pContext, SteamNetworkingIdentity &identityPeer, const CMsgSteamNetworkingP2PRendezvous &msg, int nEndReason, const char *fmt, ... )
{
	if ( !msg.from_connection_id() || msg.from_identity().empty() )
		return;

	char szDebug[ 256 ];
	va_list ap;
	va_start( ap, fmt );
	V_vsnprintf( szDebug, sizeof(szDebug), fmt, ap );
	va_end( ap );

	CMsgSteamNetworkingP2PRendezvous msgReply;
	msgReply.set_to_connection_id( msg.from_connection_id() );
	msgReply.set_to_identity( msg.from_identity() );
	msgReply.mutable_connection_closed()->set_reason_code( nEndReason );
	msgReply.mutable_connection_closed()->set_debug( szDebug );

	int cbReply = ProtoMsgByteSize( msgReply );
	uint8 *pReply = (uint8*)alloca( cbReply );
	DbgVerify( msgReply.SerializeWithCachedSizesToArray( pReply ) == pReply + cbReply );
	pContext->SendRejectionSignal( identityPeer, pReply, cbReply );
}

bool CSteamNetworkingSockets::ReceivedP2PCustomSignal( const void *pMsg, int cbMsg, ISteamNetworkingSignalingRecvContext *pContext )
{
	// Deserialize the message
	CMsgSteamNetworkingP2PRendezvous msg;
	if ( !msg.ParseFromArray( pMsg, cbMsg ) )
	{
		SpewWarning( "P2P signal failed protobuf parse\n" );
		return false;
	}

	SteamNetworkingGlobalLock scopeLock( "ReceivedP2PCustomSignal" );
	return InternalReceivedP2PSignal( msg, pContext, false );
}

// Compare connections initiated by two peers, and make a decision
// which one should take priority.  We use the Connection IDs as the
// first discriminator, and we do it in a "rock-paper-scissors" sort
// of a way, such that all IDs are equally likely to win if you don't
// know the other ID, and a malicious client has no strategy for
// influencing the outcome to achieve any particular end.
//
// <0: A should be the "client"
// >0: B should be the "client"
// 0: cannot choose.  (*exceedingly* rare)
static int CompareSymmetricConnections( uint32 nConnectionIDA, const std::string &sTieBreakerA, uint32 nConnectionIDB, const std::string &sTieBreakerB )
{

	// Start by select
	int result;
	if ( nConnectionIDA < nConnectionIDB ) result = -1;
	else if ( nConnectionIDA > nConnectionIDB ) result = +1;
	else
	{
		// This is exceedingly rare.  We go ahead and write some code to handle it, because
		// why not?  It would probably be acceptable to punt here and fail the connection.
		// But assert, because if we do hit this case, it is almost certainly more likely
		// to be a bug in our code than an actual collision.
		//
		// Also note that it is possible to make a connection to "yourself"
		AssertMsg( false, "Symmetric connections with connection IDs!  Odds are 1:2e32!" );

		// Compare a secondary source of entropy.  Even if encryption is disabled, we still create a key per connection.
		int n = std::min( len( sTieBreakerA ), len( sTieBreakerB ) );
		Assert( n >= 32 );
		result = memcmp( sTieBreakerA.c_str(), sTieBreakerB.c_str(), n );
		Assert( result != 0 );
	}

	// Check parity of lowest bit and flip result
	if ( ( nConnectionIDA ^ nConnectionIDB ) & 1 )
		result = -result;

	return result;
}

bool CSteamNetworkingSockets::InternalReceivedP2PSignal( const CMsgSteamNetworkingP2PRendezvous &msg, ISteamNetworkingSignalingRecvContext *pContext, bool bDefaultSignaling )
{
	SteamDatagramErrMsg errMsg;

	// Caller must take the lock.
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "InternalReceivedP2PSignal" );

	// Parse remote identity
	if ( *msg.from_identity().c_str() == '\0' )
	{
		SpewWarning( "Bad P2P signal: no from_identity\n" );
		return false;
	}
	SteamNetworkingIdentity identityRemote;
	if ( !identityRemote.ParseString( msg.from_identity().c_str() ) || identityRemote.IsFakeIP() )
	{
		SpewWarning( "Bad P2P signal: invalid from_identity '%s'\n", msg.from_identity().c_str() );
		return false;
	}

	// Parse to identity, if one was given.  note that we sort of assume at this point
	// that the message has been properly routed and it's intended for *us*.  But
	// we may have more than one identity (e.g. FakeIP) and this tells us how the
	// peer is routing messages to us
	// NOTE this might legit fail on an empty string.  Ignore that.
	SteamNetworkingIdentity toLocalIdentity;
	toLocalIdentity.ParseString( msg.to_identity().c_str() );

	int nLogLevel = m_connectionConfig.LogLevel_P2PRendezvous.Get();

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Locate the connection, if we already have one
	CSteamNetworkConnectionP2P *pConn = nullptr;
	ConnectionScopeLock connectionLock;
	if ( msg.has_to_connection_id() )
	{
		CSteamNetworkConnectionBase *pConnBase = FindConnectionByLocalID( msg.to_connection_id(), connectionLock );

		// Didn't find them?  Then just drop it.  Otherwise, we are susceptible to leaking the player's online state
		// any time we receive random message.
		if ( pConnBase == nullptr )
		{
			SpewMsgGroup( nLogLevel, "Ignoring P2PRendezvous from %s to unknown connection #%u\n", SteamNetworkingIdentityRender( identityRemote ).c_str(), msg.to_connection_id() );
			return true;
		}

		SpewVerboseGroup( nLogLevel, "[%s] Recv P2PRendezvous\n", pConnBase->GetDescription() );
		SpewDebugGroup( nLogLevel, "%s\n\n", Indent( msg.DebugString() ).c_str() );

		pConn = pConnBase->AsSteamNetworkConnectionP2P();
		if ( !pConn )
		{
			SpewWarning( "[%s] Got P2P signal from %s.  Wrong connection type!\n", msg.from_identity().c_str(), pConn->GetDescription() );
			return false;
		}

		// Connection already shutdown?
		if ( pConn->GetState() == k_ESteamNetworkingConnectionState_Dead )
		{
			// How was the connection found by FindConnectionByLocalID then?
			Assert( false );
			return false;
		}

		// We might not know who the other guy is yet
		if ( pConn->GetState() == k_ESteamNetworkingConnectionState_Connecting && ( pConn->m_identityRemote.IsInvalid() || pConn->m_identityRemote.IsLocalHost() || pConn->m_identityRemote.IsFakeIP() ) )
		{
			Assert( !pConn->m_bConnectionInitiatedRemotely ); // We don't let people try to connect without telling us who they are
			#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
				if ( pConn->m_identityRemote.IsFakeIP() )
				{
					AssertMsg( !pConn->m_fakeIPRefRemote.IsValid(), "%s Setting up FakeIP ref twice?", pConn->GetDescription() );
					pConn->m_fakeIPRefRemote.Setup( pConn->m_identityRemote.m_ip, identityRemote );
				}
			#endif
			pConn->m_identityRemote = identityRemote;
			pConn->SetDescription();
		}
		else if ( !( pConn->m_identityRemote == identityRemote ) )
		{
			SpewWarning( "[%s] Got P2P signal from wrong remote identity '%s'\n", pConn->GetDescription(), msg.from_identity().c_str() );
			return false;
		}

		// They should always send their connection ID, unless they never established a connection.
		if ( pConn->m_unConnectionIDRemote )
		{
			if ( pConn->m_unConnectionIDRemote != msg.from_connection_id() )
			{
				SpewWarning( "Ignoring P2P signal from %s.  For our cxn #%u, they first used remote cxn #%u, now using #%u",
					msg.from_identity().c_str(), msg.to_connection_id(), pConn->m_unConnectionIDRemote, msg.from_connection_id() );
				return false;
			}
		}
		else
		{
			pConn->m_unConnectionIDRemote = msg.from_connection_id();
		}
		if ( !pConn->BEnsureInP2PConnectionMapByRemoteInfo( errMsg ) )
			return false;
	}
	else
	{

		// They didn't know our connection ID (yet).  But we might recognize their connection ID.
		if ( !msg.from_connection_id() )
		{
			SpewWarning( "Bad P2P signal from '%s': neither from/to connection IDs present\n", msg.from_identity().c_str() );
			return false;
		}
		RemoteConnectionKey_t key{ identityRemote, msg.from_connection_id() };
		int idxMapP2P = g_mapP2PConnectionsByRemoteInfo.Find( key );
		if ( idxMapP2P != g_mapP2PConnectionsByRemoteInfo.InvalidIndex() )
		{
			pConn = g_mapP2PConnectionsByRemoteInfo[ idxMapP2P ];
			connectionLock.Lock( *pConn );
			Assert( pConn->m_idxMapP2PConnectionsByRemoteInfo == idxMapP2P );
			Assert( pConn->m_identityRemote == identityRemote );
			Assert( pConn->m_unConnectionIDRemote == msg.from_connection_id() );
		}
		else
		{

			// Only other legit case is a new connect request.
			if ( !msg.has_connect_request() )
			{
				SpewWarning( "Ignoring P2P signal from '%s', unknown remote connection #%u\n", msg.from_identity().c_str(), msg.from_connection_id() );

				// We unfortunately must not reply in this case.  If we do reply,
				// then all you need to do to tell if somebody is online is send a
				// signal with a random connection ID.  If we did have such a
				// connection, but it is deleted now, then hopefully we cleaned it
				// up properly, handling potential for dropped cleanup messages,
				// in the FinWait state
				return true;
			}

			// We must know who we are.
			if ( m_identity.IsInvalid() )
			{
				SpewWarning( "Ignoring P2P connect request signal from '%s', no local identity?\n", msg.from_identity().c_str() );
				return false;
			}

			const CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest = msg.connect_request();
			if ( !msgConnectRequest.has_cert() || !msgConnectRequest.has_crypt() )
			{
				SpewWarning( "Ignoring P2P CMsgSteamDatagramConnectRequest from %s; missing required fields", SteamNetworkingIdentityRender( identityRemote ).c_str() );
				return false;
			}

			// Are we ready with authentication?
			// This is actually not really correct to use a #define here.  Really, we ought
			// to create a connection and check AllowLocalUnsignedCert/AllowRemoteUnsignedCert.
			if ( BCanRequestCert() )
			{

				// Make sure we have a recent cert.  Start requesting another if needed.
				AuthenticationNeeded();

				// If we don't have a signed cert now, then we cannot accept this connection!
				// P2P connections always require certs issued by Steam!
				if ( !m_msgSignedCert.has_ca_signature() )
				{
					SpewWarning( "Ignoring P2P connection request from %s.  We cannot accept it since we don't have a cert yet!\n",
						SteamNetworkingIdentityRender( identityRemote ).c_str() );
					return true; // Return true because the signal is valid, we just cannot do anything with it right now
				}
			}

			// Determine virtual ports, and locate the listen socket, if any
			// Connecting by FakeIP?
			int nLocalVirtualPort = msgConnectRequest.has_to_virtual_port() ? msgConnectRequest.to_virtual_port() : -1;
			int nRemoteVirtualPort = -1;
			if ( msgConnectRequest.has_from_virtual_port() )
				nRemoteVirtualPort = msgConnectRequest.from_virtual_port();
			else
				nRemoteVirtualPort = nLocalVirtualPort;
			int nUseSymmetricConnection = -1;
			CSteamNetworkListenSocketP2P *pListenSock = nullptr;

			#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
				SteamNetworkingIPAddr fromFakeIP;
				fromFakeIP.Clear();
				if ( toLocalIdentity.IsFakeIP() )
				{
					int idxFakePort = GetFakePortIndex( toLocalIdentity.m_ip );
					if ( idxFakePort < 0 )
					{
						SpewWarning( "Ignoring P2P CMsgSteamDatagramConnectRequest from %s to FakeIP %s.  We don't that's one of our ports!  (Why did it get routed to us?)",
							SteamNetworkingIdentityRender( identityRemote ).c_str(), SteamNetworkingIPAddrRender( toLocalIdentity.m_ip ).c_str() );
						return false;
					}

					// If they indicated that they have a global FakeIP, then use it.
					if ( msgConnectRequest.has_from_fakeip() )
					{
						if ( !fromFakeIP.ParseString( msgConnectRequest.from_fakeip().c_str() ) || fromFakeIP.GetFakeIPType() != k_ESteamNetworkingFakeIPType_GlobalIPv4 )
						{
							SpewWarning( "Ignoring P2P CMsgSteamDatagramConnectRequest from %s to FakeIP %s.  Invalid from_fake_ip '%s'",
								SteamNetworkingIdentityRender( identityRemote ).c_str(), SteamNetworkingIPAddrRender( toLocalIdentity.m_ip ).c_str(), msgConnectRequest.from_fakeip().c_str() );
							return false;
						}
					}

					// Ignore any remote virtual port they set.
					nRemoteVirtualPort = -1;

					// Set local virtual port from fake port they are sending to
					if ( nLocalVirtualPort >= 0 )
						SpewWarning( "%s set to_virtual_port in rendezvous when connecting by FakeIP; ignored", SteamNetworkingIdentityRender( identityRemote ).c_str() );
					nLocalVirtualPort = k_nVirtualPort_GlobalFakePort0 + idxFakePort;
					Assert( nLocalVirtualPort >= 0 && nLocalVirtualPort <= k_nVirtualPort_GlobalFakePortMax );
				}
				else
				{
					if ( msgConnectRequest.has_from_fakeip() )
					{
						SpewWarning( "Ignoring P2P CMsgSteamDatagramConnectRequest.from_fakeip from %s, not sending to a FakeIP!",
							SteamNetworkingIdentityRender( identityRemote ).c_str() );
					}
				}
			#endif

			if ( nLocalVirtualPort >= 0 )
			{

				// Special case for SteamNetworkingMessages.  We can get a message
				// before this interface is ever created, so the lookup below won't
				// work.
				if ( nLocalVirtualPort == k_nVirtualPort_Messages )
				{
					#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
						if ( !GetSteamNetworkingMessages() )
						{
							SpewBug( "Ignoring P2P CMsgSteamDatagramConnectRequest from %s; can't get ISteamNetworkingNetworkingMessages interface!", SteamNetworkingIdentityRender( identityRemote ).c_str() );
							//SendP2PRejection( pContext, identityRemote, msg, k_ESteamNetConnectionEnd_Misc_Generic, "Internal error accepting connection.  Can't get NetworkingMessages interface" );
							return false;
						}
					#else
						SpewWarning( "Ignoring P2P CMsgSteamDatagramConnectRequest from %s; ISteamNetworkingNetworkingMessages not supported", SteamNetworkingIdentityRender( identityRemote ).c_str() );
						//SendP2PRejection( pContext, identityRemote, msg, k_ESteamNetConnectionEnd_Misc_Generic, "Internal error accepting connection.  Can't get NetworkingMessages interface" );
						return false;
					#endif
				}

				// Locate the listen socket
				int idxListenSock = m_mapListenSocketsByVirtualPort.Find( nLocalVirtualPort );
				if ( idxListenSock == m_mapListenSocketsByVirtualPort.InvalidIndex() )
				{

					// If default signaling, then it must match up to a listen socket.
					// If custom signaling, then they need not have created one.
					if ( bDefaultSignaling )
					{

						// Totally ignore it.  We don't want this to be able to be used as a way to
						// tell if you are online or not.
						SpewMsgGroup( nLogLevel, "Ignoring P2P CMsgSteamDatagramConnectRequest from %s; we're not listening on %s\n",
							SteamNetworkingIdentityRender( identityRemote ).c_str(), VirtualPortRender( nLocalVirtualPort ).c_str() );
						return false;
					}
				}
				else
				{
					pListenSock = m_mapListenSocketsByVirtualPort[ idxListenSock ];
					if ( pListenSock->BSymmetricMode() )
						nUseSymmetricConnection = 1;
				}

				// Check for matching symmetric connections
				if ( nLocalVirtualPort >= 0 )
				{

					bool bSearchDuplicateConnections = true;
					if ( IsVirtualPortFakePort( nLocalVirtualPort ) )
					{
						#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
							Assert( IsVirtualPortGlobalFakePort( nLocalVirtualPort ) );

							// Check for matching symmetric connection if both
							// sides are using global addresses
							if ( fromFakeIP.GetFakeIPType() == k_ESteamNetworkingFakeIPType_GlobalIPv4 )
							{
								// Use symmetric mode if the listen socket is opened in symmetric mode
							}
							else
							{
								// Can't use symmetric mode, since we wouldn't have a way to
								// send back out to them
								Assert( fromFakeIP.IsIPv6AllZeros() );
								nUseSymmetricConnection = 0;
								bSearchDuplicateConnections = false;
							}
						#else
							Assert( false );
							return false;
						#endif
					}

					if ( bSearchDuplicateConnections )
					{

						// If this connection is symmetric, then we want to match any other connection.
						// (Although it really ought to also be symmetric if they are using the API
						// properly.)  If this connection is NOT symmetric, then only match another
						// symmetric connection.  (Again, this is not the best practices use of the API.)
						bool bMatchOnlySymmetricConnections = ( nUseSymmetricConnection <= 0 );

						CSteamNetworkConnectionP2P *pMatchingConnection = CSteamNetworkConnectionP2P::FindDuplicateConnection( this, nLocalVirtualPort, identityRemote, nRemoteVirtualPort, nUseSymmetricConnection <= 0, nullptr );
						if ( pMatchingConnection )
						{
							ConnectionScopeLock lockMatchingConnection( *pMatchingConnection );
							Assert( pMatchingConnection->m_pParentListenSocket == nullptr ); // This conflict should only happen for connections we initiate!

							// Check if they are mixing symmetric and asymmetric connections.
							// That's not good.
							if ( bMatchOnlySymmetricConnections )
							{
								SpewWarning( "[%s] Outbound symmetric connection (local vport %d, remote vport %d) and matched to incoming connect request remote cxn ID #%u.  You should configure the listen socket in symmetric mode\n",
									pMatchingConnection->GetDescription(), pMatchingConnection->LocalVirtualPort(), pMatchingConnection->m_nRemoteVirtualPort, msg.from_connection_id() );
							}

							int cmp = CompareSymmetricConnections( pMatchingConnection->m_unConnectionIDLocal, pMatchingConnection->GetSignedCertLocal().cert(), msg.from_connection_id(), msgConnectRequest.cert().cert() );

							// Check if we prefer for our connection to act as the "client"
							if ( cmp <= 0 )
							{
								SpewVerboseGroup( nLogLevel, "[%s] Symmetric role resolution for connect request remote cxn ID #%u says we should act as client.  Dropping incoming request, we will wait for them to accept ours\n", pMatchingConnection->GetDescription(), msg.from_connection_id() );
								Assert( !pMatchingConnection->m_bConnectionInitiatedRemotely );
								return true;
							}

							pMatchingConnection->ChangeRoleToServerAndAccept( msg, usecNow );
							return true;
						}
					}
				}

			}
			else
			{
				// Old client using custom signaling that previously did not specify virtual ports.
				// This is OK.  Otherwise, this is weird.
				AssertMsg( !bDefaultSignaling, "P2P connect request with no to_virtual_port? /+/ %s", msgConnectRequest.ShortDebugString().c_str() );
			}

			// Special case for servers in known POPs
			#ifdef SDR_ENABLE_HOSTED_SERVER
				if ( pListenSock )
				{
					switch ( pListenSock->m_eHostedDedicatedServer )
					{
						case CSteamNetworkListenSocketP2P::k_EHostedDedicatedServer_Not:
							// Normal P2P connectivity
							break;

						case CSteamNetworkListenSocketP2P::k_EHostedDedicatedServer_TicketsOnly:
							SpewMsgGroup( nLogLevel, "Ignoring P2P CMsgSteamDatagramConnectRequest from %s; we're listening on %s, but only for ticket-based connections, not for connections requiring P2P signaling\n",
								SteamNetworkingIdentityRender( identityRemote ).c_str(), VirtualPortRender( nLocalVirtualPort ).c_str() );
							return false;

						case CSteamNetworkListenSocketP2P::k_EHostedDedicatedServer_Auto:
							SpewMsgGroup( nLogLevel, "P2P CMsgSteamDatagramConnectRequest from %s; we're listening on %s, hosted server connection\n",
								SteamNetworkingIdentityRender( identityRemote ).c_str(), VirtualPortRender( nLocalVirtualPort ).c_str() );
							pConn = new CSteamNetworkAcceptedConnectionFromSDRClient( this, connectionLock );
							break;

						default:
							Assert( false );
							return false;
					}
				}
			#endif

			// Create a connection
			if ( pConn == nullptr )
				pConn = new CSteamNetworkConnectionP2P( this, connectionLock );
			pConn->m_identityRemote = identityRemote;
			pConn->m_unConnectionIDRemote = msg.from_connection_id();
			pConn->m_nRemoteVirtualPort = nRemoteVirtualPort;
			pConn->m_connectionConfig.LocalVirtualPort.Set( nLocalVirtualPort );
			if ( nUseSymmetricConnection >= 0 )
			{
				pConn->m_connectionConfig.SymmetricConnect.Set( nUseSymmetricConnection );
				pConn->m_connectionConfig.SymmetricConnect.Lock();
			}

			// Suppress state change notifications for now
			Assert( pConn->m_nSupressStateChangeCallbacks == 0 );
			pConn->m_nSupressStateChangeCallbacks = 1;

			// If this is a FakeIP connection, we need to remember or assign the
			// FakeIP of the remote host
			#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
				if ( toLocalIdentity.IsFakeIP() )
				{
					Assert( !pConn->m_fakeIPRefRemote.IsValid() );
					if ( fromFakeIP.IsIPv6AllZeros() )
					{
						if ( !pConn->m_fakeIPRefRemote.SetupNewLocalIP( pConn->m_identityRemote, &fromFakeIP ) )
						{
							SpewWarning( "Failed to start accepting P2P FakeIP connect request from %s; cannot assign ephemeral IP\n",
								SteamNetworkingIdentityRender( pConn->m_identityRemote ).c_str() );
							pConn->ConnectionQueueDestroy();
							return false;
						}
					}
					else
					{
						pConn->m_fakeIPRefRemote.Setup( fromFakeIP, pConn->m_identityRemote );
					}
				}
			#endif

			// Add it to the listen socket, if any
			if ( pListenSock )
			{
				if ( !pListenSock->BAddChildConnection( pConn, errMsg ) )
				{
					SpewWarning( "Failed to start accepting P2P connect request from %s on %s; %s\n",
						SteamNetworkingIdentityRender( pConn->m_identityRemote ).c_str(), VirtualPortRender( nLocalVirtualPort ).c_str(), errMsg );
					pConn->ConnectionQueueDestroy();
					return false;
				}
			}

			// OK, start setting up the connection
			if ( !pConn->BBeginAcceptFromSignal( 
				msgConnectRequest,
				errMsg,
				usecNow
			) ) {
				SpewWarning( "Failed to start accepting P2P connect request from %s on %s; %s\n",
					SteamNetworkingIdentityRender( pConn->m_identityRemote ).c_str(), VirtualPortRender( nLocalVirtualPort ).c_str(), errMsg );
				pConn->ConnectionQueueDestroy();
				SendP2PRejection( pContext, identityRemote, msg, k_ESteamNetConnectionEnd_Misc_Generic, "Internal error accepting connection.  %s", errMsg );
				return false;
			}

			// Mark that we received something.  Even though it was through the
			// signaling mechanism, not the channel used for data, and we ordinarily
			// don't count that.
			pConn->m_statsEndToEnd.m_usecTimeLastRecv = usecNow;

			// Inform app about the incoming request, see what they want to do
			pConn->m_pSignaling = pContext->OnConnectRequest( pConn->m_hConnectionSelf, identityRemote, nLocalVirtualPort );

			// Already closed?
			switch ( pConn->GetState() )
			{
				default:
				case k_ESteamNetworkingConnectionState_ClosedByPeer: // Uhhhhhh
				case k_ESteamNetworkingConnectionState_Dead:
				case k_ESteamNetworkingConnectionState_Linger:
				case k_ESteamNetworkingConnectionState_None:
				case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
					Assert( false );
					// FALLTHROUGH
				case k_ESteamNetworkingConnectionState_FinWait:

					// app context closed the connection.  This means that they want to send
					// a rejection
					SpewVerboseGroup( nLogLevel, "[%s] P2P connect request actively rejected by app, sending rejection (%s)\n",
						pConn->GetDescription(), pConn->GetConnectionEndDebugString() );
					SendP2PRejection( pContext, identityRemote, msg, pConn->GetConnectionEndReason(), "%s", pConn->GetConnectionEndDebugString() );
					pConn->ConnectionQueueDestroy();
					return true;

				case k_ESteamNetworkingConnectionState_Connecting:
				{

					// If they returned null, that means they want to totally ignore it.
					if ( !pConn->m_pSignaling )
					{
						// They decided to ignore it, by just returning null
						SpewVerboseGroup( nLogLevel, "App ignored P2P connect request from %s on %s\n",
							SteamNetworkingIdentityRender( pConn->m_identityRemote ).c_str(), VirtualPortRender( nLocalVirtualPort ).c_str() );
						pConn->ConnectionQueueDestroy();
						return true;
					}

					// They return signaling, which means that they will consider accepting it.
					// But they didn't accept it, so they want to go through the normal
					// callback mechanism.

					#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
					CMessagesEndPoint *pMessagesEndPoint = pListenSock ? pListenSock->m_pMessagesEndPointOwner : nullptr;
					if ( pMessagesEndPoint )
					{
						SpewVerboseGroup( nLogLevel, "[%s] Received incoming P2P connect request on ad-hoc style end point\n",
							pConn->GetDescription() );
						if ( !pMessagesEndPoint->BHandleNewIncomingConnection( pConn, connectionLock ) )
						{
							pConn->ConnectionQueueDestroy();
							return false;
						}
					} else
					#endif
					{
						SpewVerboseGroup( nLogLevel, "[%s] Received incoming P2P connect request; awaiting app to accept connection\n",
							pConn->GetDescription() );
					}
					pConn->PostConnectionStateChangedCallback( k_ESteamNetworkingConnectionState_None, k_ESteamNetworkingConnectionState_Connecting );
				} break;

				case k_ESteamNetworkingConnectionState_Connected:
					AssertMsg( false, "How did we already get connected?  We should be finding route?");
				case k_ESteamNetworkingConnectionState_FindingRoute:
					// They accepted the request already.
					break;
			}

			// Stop suppressing state change notifications
			Assert( pConn->m_nSupressStateChangeCallbacks == 1 );
			pConn->m_nSupressStateChangeCallbacks = 0;
		}
	}

	// Process the message
	return pConn->ProcessSignal( msg, usecNow );
}

} // namespace SteamNetworkingSocketsLib
