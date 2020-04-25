//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_p2p.h"
#include "csteamnetworkingsockets.h"
#include "crypto.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
	#include "steamnetworkingsockets_sdr_p2p.h"
#endif

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
	#include "steamnetworkingsockets_p2p_ice.h"
#endif

#ifdef STEAMNETWORKINGSOCKETS_HAS_DEFAULT_P2P_SIGNALING
#include "csteamnetworkingmessages.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

CUtlHashMap<RemoteConnectionKey_t,CSteamNetworkConnectionP2P*, std::equal_to<RemoteConnectionKey_t>, RemoteConnectionKey_t::Hash > g_mapIncomingP2PConnections;

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketP2P
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkListenSocketP2P::CSteamNetworkListenSocketP2P( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkListenSocketBase( pSteamNetworkingSocketsInterface )
, m_nVirtualPort( -1 )
{
}

CSteamNetworkListenSocketP2P::~CSteamNetworkListenSocketP2P()
{
	// Remove from virtual port map
	if ( m_nVirtualPort >= 0 )
	{
		int h = m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.Find( m_nVirtualPort );
		if ( h != m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.InvalidIndex() && m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort[h] == this )
		{
			m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort[h] = nullptr; // just for grins
			m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.RemoveAt( h );
		}
		else
		{
			AssertMsg( false, "Bookkeeping bug!" );
		}
		m_nVirtualPort = -1;
	}
}

bool CSteamNetworkListenSocketP2P::BInit( int nVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	Assert( nVirtualPort >= 0 );

	// We need SDR functionality in order to support P2P
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		CSteamNetworkingSocketsSDR *pSteamNetworkingSocketsSDR = assert_cast< CSteamNetworkingSocketsSDR *>( m_pSteamNetworkingSocketsInterface );
		if ( !pSteamNetworkingSocketsSDR->BSDRClientInit( errMsg ) )
			return false;
	#endif

	if ( m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.HasElement( nVirtualPort ) )
	{
		V_sprintf_safe( errMsg, "Already have a listen socket on P2P virtual port %d", nVirtualPort );
		return false;
	}
	m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.Insert( nVirtualPort, this );
	m_nVirtualPort = nVirtualPort;

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

CSteamNetworkConnectionP2P::CSteamNetworkConnectionP2P( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkConnectionBase( pSteamNetworkingSocketsInterface )
{
	m_nRemoteVirtualPort = -1;
	m_idxMapIncomingP2PConnections = -1;
	m_pSignaling = nullptr;
	m_usecNextEvaluateTransport = 0;
	m_bTransportSticky = false;
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		m_pTransportP2PSDR = nullptr;
	#endif
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		m_pTransportICE = nullptr;
		m_pTransportICEPendingDelete = nullptr;
		m_nICECloseCode = 0;
		m_szICECloseMsg[ 0 ] = '\0';
	#endif
}

CSteamNetworkConnectionP2P::~CSteamNetworkConnectionP2P()
{
	Assert( m_idxMapIncomingP2PConnections == -1 );
}

void CSteamNetworkConnectionP2P::GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const
{
	V_sprintf_safe( szDescription, "P2P %s", SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
}

bool CSteamNetworkConnectionP2P::BInitConnect( ISteamNetworkingConnectionCustomSignaling *pSignaling, const SteamNetworkingIdentity *pIdentityRemote, int nVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	Assert( !m_pTransport );

	// Remember who we're talking to
	Assert( m_pSignaling == nullptr );
	m_pSignaling = pSignaling;
	if ( pIdentityRemote )
		m_identityRemote = *pIdentityRemote;
	m_nRemoteVirtualPort = nVirtualPort;

	// Remember when we started finding a session
	//m_usecTimeStartedFindingSession = usecNow;

	// Reset end-to-end state
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	if ( !BInitP2PConnectionCommon( usecNow, nOptions, pOptions, errMsg ) )
		return false;

	// Check if we should try ICE
	CheckInitICE();

	// Start the connection state machine, and send the first request packet.
	CheckConnectionStateAndSetNextThinkTime( usecNow );

	// Done
	return true;
}

bool CSteamNetworkConnectionP2P::BInitP2PConnectionCommon( SteamNetworkingMicroseconds usecNow, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	// Let base class do some common initialization
	if ( !CSteamNetworkConnectionBase::BInitConnection( usecNow, nOptions, pOptions, errMsg ) )
		return false;

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

		// Select SDR as the transport
		m_pTransport = m_pTransportP2PSDR;
	#endif

	return true;
}

bool CSteamNetworkConnectionP2P::BBeginAccept(
	const CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest,
	SteamDatagramErrMsg &errMsg,
	SteamNetworkingMicroseconds usecNow
) {
	m_bConnectionInitiatedRemotely = true;

	// Let base class do some common initialization
	if ( !BInitP2PConnectionCommon( usecNow, 0, nullptr, errMsg ) )
		return false;

	// Process crypto handshake now
	if ( !BRecvCryptoHandshake( msgConnectRequest.cert(), msgConnectRequest.crypt(), true ) )
	{
		Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
		V_sprintf_safe( errMsg, "Error with crypto.  %s", m_szEndDebug );
		return false;
	}

	// Add to connection map
	Assert( m_idxMapIncomingP2PConnections == -1 );
	RemoteConnectionKey_t key{ m_identityRemote, m_unConnectionIDRemote };
	if ( g_mapIncomingP2PConnections.HasElement( key ) )
	{
		// "should never happen"
		V_sprintf_safe( errMsg, "Duplicate P2P connection %s %u!", SteamNetworkingIdentityRender( m_identityRemote ).c_str(), m_unConnectionIDRemote );
		AssertMsg1( false, "%s", errMsg );
		return false;
	}
	m_idxMapIncomingP2PConnections = g_mapIncomingP2PConnections.InsertOrReplace( key, this );
	return true;
}

CSteamNetworkConnectionP2P *CSteamNetworkConnectionP2P::AsSteamNetworkConnectionP2P()
{
	return this;
}

void CSteamNetworkConnectionP2P::CheckInitICE()
{
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
	Assert( !m_pTransportICE );
	Assert( !m_pTransportICEPendingDelete );

	// Now ICE factory?
	if ( !g_SteamNetworkingSockets_CreateICESessionFunc )
		return;

	// !FIXME! Check permissions based on remote host!

	// For now, if we have no STUN servers, then no ICE,
	// because NAT-punching is the main reason to use ICE.
	//
	// But we might want to enable ICE even without STUN.
	// In some environments, maybe they want to only use TURN.
	// Or maybe they don't need STUN, and don't need to pierce NAT,
	// either because both IPs are public, or because they are on
	// the same LAN.  The LAN case is usually better handled by
	// broadcast, since that works without signaling.
	if ( m_connectionConfig.m_P2P_STUN_ServerList.Get().empty() )
		return;

	m_pTransportICE = new CConnectionTransportP2PICE( *this );
	m_pTransportICE->Init();

	// If we failed, go ahead and cleanup now
	CheckCleanupICE();
#endif
}

void CSteamNetworkConnectionP2P::CheckCleanupICE()
{
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
	if ( m_pTransportICEPendingDelete )
		DestroyICENow();
#endif
}

void CSteamNetworkConnectionP2P::DestroyICENow()
{
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

	// If transport was selected, then make sure and deselect, and force a re-evaluation ASAP
	if ( m_pTransport && ( m_pTransport == m_pTransportICEPendingDelete || m_pTransport == m_pTransportICE ) )
	{
		SelectTransport( nullptr );
		m_usecNextEvaluateTransport = 0;
		SetNextThinkTimeASAP();
	}

	// Destroy
	if ( m_pTransportICE )
	{
		Assert( m_pTransportICE != m_pTransportICEPendingDelete );
		m_pTransportICE->TransportDestroySelfNow();
		m_pTransportICE = nullptr;
	}
	if ( m_pTransportICEPendingDelete )
	{
		m_pTransportICEPendingDelete->TransportDestroySelfNow();
		m_pTransportICEPendingDelete = nullptr;
	}
#endif

}

void CSteamNetworkConnectionP2P::FreeResources()
{
	// Remove from global map, if we're in it
	if ( m_idxMapIncomingP2PConnections >= 0 )
	{
		if ( g_mapIncomingP2PConnections.IsValidIndex( m_idxMapIncomingP2PConnections ) && g_mapIncomingP2PConnections[ m_idxMapIncomingP2PConnections ] == this )
		{
			g_mapIncomingP2PConnections[ m_idxMapIncomingP2PConnections ] = nullptr; // just for grins
			g_mapIncomingP2PConnections.RemoveAt( m_idxMapIncomingP2PConnections );
		}
		else
		{
			AssertMsg( false, "g_mapIncomingP2PConnections bookkeeping mismatch" );
		}
		m_idxMapIncomingP2PConnections = -1;
	}

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
	// We're about to nuke all of our transports, don't point at any of them.
	m_pTransport = nullptr;

	// Nuke all known transports.
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		if ( m_pTransportP2PSDR )
		{
			m_pTransportP2PSDR->TransportDestroySelfNow();
			m_pTransportP2PSDR = nullptr;
		}
	#endif
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		DestroyICENow();
	#endif
}

EResult CSteamNetworkConnectionP2P::AcceptConnection( SteamNetworkingMicroseconds usecNow )
{
	// Calling code shouldn't call us unless this is true
	Assert( m_bConnectionInitiatedRemotely );
	Assert( GetState() == k_ESteamNetworkingConnectionState_Connecting );

	// Send them a reply, and include whatever info we have right now
	SendConnectOKSignal( usecNow );

	// WE'RE NOT "CONNECTED" YET!
	// We need to do route negotiation first, which could take several route trips,
	// depending on what ping data we already had before we started.
	ConnectionState_FindingRoute( usecNow );

	// OK
	return k_EResultOK;
}

void CSteamNetworkConnectionP2P::TransportEndToEndConnectivityChanged( CConnectionTransport *pTransport )
{
	// A major event has happened.  Don't be sticky to current transport.
	m_bTransportSticky = false;

	// Schedule us to wake up immediately and deal with it.
	m_usecNextEvaluateTransport = 0;
	SetNextThinkTimeASAP();
}

void CSteamNetworkConnectionP2P::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	// NOTE: Do not call base class, because it it going to
	// call TransportConnectionStateChanged on whatever transport is active.
	// We don't want that here.

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		if ( m_pTransportP2PSDR )
			m_pTransportP2PSDR->TransportConnectionStateChanged( eOldState );
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( m_pTransportICE )
			m_pTransportICE->TransportConnectionStateChanged( eOldState );
	#endif

	// Reset timer to evaluate transport at certain times
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		default:
			Assert( false );
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connected:
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:
			m_usecNextEvaluateTransport = 0;
			m_bTransportSticky = false; // Not sure how we could have set this flag, but make sure and clear it
			SetNextThinkTimeASAP();
			break;
	}
}

void CSteamNetworkConnectionP2P::ThinkConnection( SteamNetworkingMicroseconds usecNow )
{
	CSteamNetworkConnectionBase::ThinkConnection( usecNow );

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		CheckCleanupICE();
	#endif

	ThinkSelectTransport( usecNow );
}

void CSteamNetworkConnectionP2P::ThinkSelectTransport( SteamNetworkingMicroseconds usecNow )
{

	// Time to evaluate which transport to use?
	if ( usecNow < m_usecNextEvaluateTransport )
	{
		EnsureMinThinkTime( m_usecNextEvaluateTransport );
		return;
	}

	struct TransportChoice
	{
		CConnectionTransport *m_pTransport;
		int m_nScore;
	};
	vstd::small_vector<TransportChoice, 3 > vecTransports;

	const int k_nPenaltyNotLAN = 20; // LAN should be fast, so always use it if available.  If LAN ping is more than 20....we got problems
	const int k_nPenaltyNeedToConfirmEndToEndConnectivity = 10000;

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		if ( m_pTransportP2PSDR && m_pTransportP2PSDR->BCanSendEndToEndData() )
		{
			TransportChoice t;
			t.m_pTransport = m_pTransportP2PSDR;
			t.m_nScore = m_pTransportP2PSDR->GetScoreForActiveSession() + k_nPenaltyNotLAN;
			if ( m_pTransportP2PSDR->m_bNeedToConfirmEndToEndConnectivity )
				t.m_nScore += k_nPenaltyNeedToConfirmEndToEndConnectivity;
			vecTransports.push_back( t );
		}
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( m_pTransportICE && m_pTransportICE->BCanSendEndToEndData() && m_pTransportICE->m_ping.m_nSmoothedPing >= 0 )
		{
			TransportChoice t;
			t.m_pTransport = m_pTransportICE;
			t.m_nScore = m_pTransportICE->m_ping.m_nSmoothedPing + k_nPenaltyNotLAN;
			if ( m_pTransportICE->m_bNeedToConfirmEndToEndConnectivity )
				t.m_nScore += k_nPenaltyNeedToConfirmEndToEndConnectivity;
			vecTransports.push_back( t );
		}
	#endif

	// !FIXME! need to implementy LAN beacon connectivity.  Until then,
	// need this to silence a warning
	(void)k_nPenaltyNotLAN;

	// Locate best and current scores
	int nCurrentTransportScore = INT_MAX;
	int nBestTransportScore = INT_MAX;
	CConnectionTransport *pBestTransport = nullptr;
	for ( const TransportChoice &t: vecTransports )
	{
		if ( t.m_pTransport == m_pTransport )
			nCurrentTransportScore = t.m_nScore;
		if ( t.m_nScore < nBestTransportScore )
		{
			nBestTransportScore = t.m_nScore;
			pBestTransport = t.m_pTransport;
		}
	}

	// Finding route?
	if ( GetState() == k_ESteamNetworkingConnectionState_FindingRoute )
	{

		// As soon as the first transport says it's ready, let's go
		if ( nBestTransportScore < k_nPenaltyNeedToConfirmEndToEndConnectivity )
		{
			SelectTransport( pBestTransport );
			ConnectionState_Connected( usecNow );
		}
	}
	else if ( m_pTransport == nullptr )
	{
		SelectTransport( pBestTransport );
	}
	else if ( m_pTransport != pBestTransport )
	{

		// Check for applying a sticky penalty, that the new guy has to
		// overcome to switch
		int nBestScoreWithStickyPenalty = nBestTransportScore;
		if ( m_bTransportSticky )
			nBestScoreWithStickyPenalty = nBestTransportScore * 11 / 10 + 5;

		// Switch?
		if ( nBestScoreWithStickyPenalty < nCurrentTransportScore )
			SelectTransport( pBestTransport );
	}

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
			m_usecNextEvaluateTransport = usecNow + k_nMillion; // Check back periodically
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:
			m_usecNextEvaluateTransport = usecNow + k_nMillion/20; // Run the code below frequently
			break;
	}
	EnsureMinThinkTime( m_usecNextEvaluateTransport );
}

void CSteamNetworkConnectionP2P::SelectTransport( CConnectionTransport *pTransport )
{
	// No change?
	if ( pTransport == m_pTransport )
		return;
	m_pTransport = pTransport;
	m_bTransportSticky = ( m_pTransport != nullptr );
	SetDescription();
	SetNextThinkTimeASAP(); // we might want to send packets ASAP
}

SteamNetworkingMicroseconds CSteamNetworkConnectionP2P::ThinkConnection_ClientConnecting( SteamNetworkingMicroseconds usecNow )
{
	Assert( !m_bConnectionInitiatedRemotely );
	Assert( m_pParentListenSocket == nullptr );

	// FIXME if we have LAN broadcast enabled, we should send those here.
	// (Do we even need crypto ready for that, if we are gonna allow them to
	// be unauthenticated anyway?)  If so, we will need to refactor the base
	// class to call this even if crypt is not ready.

	// No signaling?  This should only be possible if we are attempting P2P though LAN
	// broadcast only.
	if ( !m_pSignaling )
	{
		// LAN broadcasts not implemented, so this should currently not be possible.
		AssertMsg( false, "No signaling?" );
		return k_nThinkTime_Never;
	}

	// If we are using SDR, then we want to wait until we have finished the initial ping probes.
	// This makes sure out initial connect message doesn't contain potentially inaccurate
	// routing information.  This delay should only happen very soon after initializing the
	// relay network.
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		if ( m_pTransportP2PSDR )
		{
			if ( !m_pTransportP2PSDR->BReady() )
				return usecNow + k_nMillion/20;
		}
	#endif

	// Time to send another connect request?
	// We always do this through signaling service rendezvous message.  We don't need to have
	// selected the transport (yet)
	SteamNetworkingMicroseconds usecRetry = m_usecWhenSentConnectRequest + k_usecConnectRetryInterval;
	if ( usecNow < usecRetry )
		return usecRetry;

	// Fill out the rendezvous message
	CMsgSteamNetworkingP2PRendezvous msgRendezvous;
	CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest = *msgRendezvous.mutable_connect_request();
	*msgConnectRequest.mutable_cert() = m_msgSignedCertLocal;
	*msgConnectRequest.mutable_crypt() = m_msgSignedCryptLocal;
	if ( m_nRemoteVirtualPort >= 0 )
		msgConnectRequest.set_virtual_port( m_nRemoteVirtualPort );

	// Send through signaling service
	SpewType( LogLevel_P2PRendezvous(), "[%s] Sending P2P ConnectRequest\n", GetDescription() );
	SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, "ConnectRequest" );

	// Remember when we send it
	m_usecWhenSentConnectRequest = usecNow;

	// And set timeout for retry
	return m_usecWhenSentConnectRequest + k_usecConnectRetryInterval;
}

void CSteamNetworkConnectionP2P::SendConnectOKSignal( SteamNetworkingMicroseconds usecNow )
{
	Assert( BCryptKeysValid() );

	CMsgSteamNetworkingP2PRendezvous msgRendezvous;
	CMsgSteamNetworkingP2PRendezvous_ConnectOK &msgConnectOK = *msgRendezvous.mutable_connect_ok();
	*msgConnectOK.mutable_cert() = m_msgSignedCertLocal;
	*msgConnectOK.mutable_crypt() = m_msgSignedCryptLocal;
	SpewType( LogLevel_P2PRendezvous(), "[%s] Sending P2P ConnectOK via Steam, remote cxn %u\n", GetDescription(), m_unConnectionIDRemote );
	SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, "ConnectOK" );
}

void CSteamNetworkConnectionP2P::SendConnectionClosedSignal( SteamNetworkingMicroseconds usecNow )
{
	SpewType( LogLevel_P2PRendezvous(), "[%s] Sending graceful P2P ConnectionClosed, remote cxn %u\n", GetDescription(), m_unConnectionIDRemote );

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
	SpewType( LogLevel_P2PRendezvous(), "[%s] Sending graceful P2P ConnectionClosed, remote cxn %u\n", GetDescription(), m_unConnectionIDRemote );

	CMsgSteamNetworkingP2PRendezvous msgRendezvous;
	CMsgSteamNetworkingP2PRendezvous_ConnectionClosed &msgConnectionClosed = *msgRendezvous.mutable_connection_closed();
	msgConnectionClosed.set_reason_code( k_ESteamNetConnectionEnd_Internal_P2PNoConnection ); // Special reason code that means "do not reply"

	// NOTE: Not sending connection stats here.  Usually when a connection is closed through this mechanism,
	// it is because we have not been able to rendezvous, and haven't sent any packets end-to-end anyway

	SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, "NoConnection" );
}

void CSteamNetworkConnectionP2P::SetRendezvousCommonFieldsAndSendSignal( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow, const char *pszDebugReason )
{
	if ( !m_pSignaling )
		return;

	Assert( !msg.has_to_connection_id() );
	if ( !msg.has_connect_request() )
	{
		Assert( m_unConnectionIDRemote != 0 );
		msg.set_to_connection_id( m_unConnectionIDRemote );
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

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		if ( m_pTransportP2PSDR )
			m_pTransportP2PSDR->PopulateRendezvousMsg( msg, usecNow );
	#endif
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( m_pTransportICE && m_nICECloseCode == 0 )
			m_pTransportICE->PopulateRendezvousMsg( msg, usecNow );
	#endif

	// Spew
	int nLogLevel = LogLevel_P2PRendezvous();
	SpewType( nLogLevel, "[%s] Sending P2PRendezvous (%s)\n", GetDescription(), pszDebugReason );
	SpewType( nLogLevel+1, "%s\n\n", Indent( msg.DebugString() ).c_str() );

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
	}
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

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingSockets P2P stuff
//
/////////////////////////////////////////////////////////////////////////////

HSteamNetConnection CSteamNetworkingSockets::ConnectP2PCustomSignaling( ISteamNetworkingConnectionCustomSignaling *pSignaling, const SteamNetworkingIdentity *pPeerIdentity, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{
	if ( !pSignaling )
		return k_HSteamNetConnection_Invalid;

	SteamDatagramTransportLock scopeLock( "ConnectP2PCustomSignaling" );
	return InternalConnectP2P( pSignaling, pPeerIdentity, -1, nOptions, pOptions );
}

HSteamNetConnection CSteamNetworkingSockets::InternalConnectP2P( ISteamNetworkingConnectionCustomSignaling *pSignaling, const SteamNetworkingIdentity *pPeerIdentity, int nVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{

	CSteamNetworkConnectionP2P *pConn = new CSteamNetworkConnectionP2P( this );
	if ( !pConn )
	{
		pSignaling->Release();
		return k_HSteamNetConnection_Invalid;
	}

	SteamDatagramErrMsg errMsg;
	if ( !pConn->BInitConnect( pSignaling, pPeerIdentity, nVirtualPort, nOptions, pOptions, errMsg ) )
	{
		if ( pPeerIdentity )
			SpewError( "Cannot create P2P connection to %s.  %s", SteamNetworkingIdentityRender( *pPeerIdentity ).c_str(), errMsg );
		else
			SpewError( "Cannot create P2P connection.  %s", errMsg );
		pConn->ConnectionDestroySelfNow();
		return k_HSteamNetConnection_Invalid;
	}

	return pConn->m_hConnectionSelf;
}

static void SendP2PRejection( ISteamNetworkingCustomSignalingRecvContext *pContext, SteamNetworkingIdentity &identityPeer, const CMsgSteamNetworkingP2PRendezvous &msg, int nEndReason, const char *fmt, ... )
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

bool CSteamNetworkingSockets::ReceivedP2PCustomSignal( const void *pMsg, int cbMsg, ISteamNetworkingCustomSignalingRecvContext *pContext )
{
	// Deserialize the message
	CMsgSteamNetworkingP2PRendezvous msg;
	if ( !msg.ParseFromArray( pMsg, cbMsg ) )
	{
		SpewWarning( "P2P signal failed protobuf parse\n" );
		return false;
	}

	// Parse remote identity
	if ( *msg.from_identity().c_str() == '\0' )
	{
		SpewWarning( "Bad P2P signal: no from_identity\n" );
		return false;
	}
	SteamNetworkingIdentity identityRemote;
	if ( !identityRemote.ParseString( msg.from_identity().c_str() ) )
	{
		SpewWarning( "Bad P2P signal: invalid from_identity '%s'\n", msg.from_identity().c_str() );
		return false;
	}

	int nLogLevel = m_connectionConfig.m_LogLevel_P2PRendezvous.Get();

	// Grab the lock now
	SteamDatagramTransportLock lock( "ReceivedP2PCustomSignal" );

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Locate the connection, if we already have one
	CSteamNetworkConnectionP2P *pConn = nullptr;
	if ( msg.has_to_connection_id() )
	{
		CSteamNetworkConnectionBase *pConnBase = FindConnectionByLocalID( msg.to_connection_id() );

		// Didn't find them?  Then just drop it.  Otherwise, we are susceptible to leaking the player's online state
		// any time we receive random message.
		if ( pConnBase == nullptr )
		{
			SpewType( nLogLevel, "Ignoring P2PRendezvous from %s to unknown connection #%u\n", SteamNetworkingIdentityRender( identityRemote ).c_str(), msg.to_connection_id() );
			return true;
		}

		SpewType( nLogLevel, "[%s] Recv P2PRendezvous\n", pConnBase->GetDescription() );
		SpewType( nLogLevel+1, "%s\n\n", Indent( msg.DebugString() ).c_str() );

		pConn = pConnBase->AsSteamNetworkConnectionP2P();
		if ( !pConn )
		{
			SpewWarning( "[%s] Got P2P signal from %s.  Wrong connection type!\n", msg.from_identity().c_str(), pConn->GetDescription() );
			return false;
		}

		// Connection already shutdown?
		if ( pConn->m_pSignaling == nullptr || pConn->GetState() == k_ESteamNetworkingConnectionState_Dead )
		{
			// How was the connection found by FindConnectionByLocalID then?
			Assert( false );
			return false;
		}

		// We might not know who the other guy is yet
		if ( pConn->GetState() == k_ESteamNetworkingConnectionState_Connecting && ( pConn->m_identityRemote.IsInvalid() || pConn->m_identityRemote.IsLocalHost() ) )
		{
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
				SpewWarning( "Ignoring P2P signal from %s.  For our cxn #%u, they first used remote cxn #%u, not using #%u",
					msg.from_identity().c_str(), msg.to_connection_id(), pConn->m_unConnectionIDRemote, msg.from_connection_id() );
				return false;
			}
		}
		else
		{
			pConn->m_unConnectionIDRemote = msg.from_connection_id();
		}

		// Closing the connection gracefully through rendezvous?
		// (Usually we try to close the connection through the
		// relay network.)
		if ( msg.has_connection_closed() )
		{
			const CMsgSteamNetworkingP2PRendezvous_ConnectionClosed &connection_closed = msg.connection_closed();

			// Give them a reply if appropriate
			if ( connection_closed.reason_code() != k_ESteamNetConnectionEnd_Internal_P2PNoConnection )
				pConn->SendNoConnectionSignal( usecNow );

			// If connection is already closed, then we're good.
			if ( pConn->GetState() == k_ESteamNetworkingConnectionState_FinWait )
			{

				// No need to hang around any more, we know that we're cleaned up on both ends
				pConn->QueueDestroy();
			}
			else
			{

				// Generic state machine take it from here.  (This call does the right
				// thing regardless of the current connection state.)
				if ( connection_closed.reason_code() == k_ESteamNetConnectionEnd_Internal_P2PNoConnection )
				{
					pConn->ConnectionState_ClosedByPeer( k_ESteamNetConnectionEnd_Misc_Generic, "Received unexpected P2P 'non connection' signal" ); 
				}
				else
				{
					pConn->ConnectionState_ClosedByPeer( connection_closed.reason_code(), connection_closed.debug().c_str() ); 
				}
			}
			return true;
		}
	}
	else if ( !msg.has_connect_request() || !msg.from_connection_id() )
	{
		SpewWarning( "Bad P2P signal from '%s': no connect_request or (\n", msg.from_identity().c_str() );
		return false;
	}
	else
	{
		Assert( !msg.has_connection_closed() ); // Not a local code bug, but fishy for them to send both

		RemoteConnectionKey_t key{ identityRemote, msg.from_connection_id() };
		int idxMapP2P = g_mapIncomingP2PConnections.Find( key );
		if ( idxMapP2P != g_mapIncomingP2PConnections.InvalidIndex() )
		{
			pConn = g_mapIncomingP2PConnections[ idxMapP2P ];
			Assert( pConn->m_idxMapIncomingP2PConnections == idxMapP2P );
			Assert( pConn->m_identityRemote == identityRemote );
			Assert( pConn->m_unConnectionIDRemote == msg.from_connection_id() );
		}
		else
		{

			// Make sure we have a recent cert.  Start requesting another if needed.
			#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
				AsyncCertRequest();
			#endif

			// If we don't have a signed cert now, then we cannot accept this connection!
			// P2P connections always require certs issued by Steam!
			if ( !m_msgSignedCert.has_ca_signature() )
			{
				SpewWarning( "Ignoring P2P connection request from %s.  We cannot accept it since we don't have a cert from Steam yet!\n", SteamNetworkingIdentityRender( identityRemote ).c_str() );
				return true; // Return true because the signal is valid, we just cannot do anything with it right now
			}

			const CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest = msg.connect_request();
			if ( !msgConnectRequest.has_cert() || !msgConnectRequest.has_crypt() )
			{
				AssertMsg1( false, "Ignoring P2P CMsgSteamDatagramConnectRequest from %s; missing required fields", SteamNetworkingIdentityRender( identityRemote ).c_str() );
				return false;
			}

			// Create a connection
			SteamDatagramErrMsg errMsg;
			pConn = new CSteamNetworkConnectionP2P( this );
			pConn->m_identityRemote = identityRemote;
			pConn->m_unConnectionIDRemote = msg.from_connection_id();

			// Suppress state change notifications for now
			pConn->m_bSupressStateChangeCallbacks = true;

			// Is this a higher-level "P2P sessions"-style request?  Then we
			// need to setup the messages session relationship now
			if ( msgConnectRequest.has_virtual_port() )
			{
				#ifdef STEAMNETWORKINGSOCKETS_HAS_DEFAULT_P2P_SIGNALING
					if ( msgConnectRequest.virtual_port() == k_nVirtualPort_P2P )
					{
						CSteamNetworkingMessages *pSteamNetworkingMessages = GetSteamNetworkingMessages();
						if ( !pSteamNetworkingMessages )
						{
							AssertMsg1( false, "Ignoring P2P CMsgSteamDatagramConnectRequest from %s; can't get NetworkingMessages interface!", SteamNetworkingIdentityRender( pConn->m_identityRemote ).c_str() );
							pConn->ConnectionDestroySelfNow();
							SendP2PRejection( pContext, identityRemote, msg, k_ESteamNetConnectionEnd_Misc_Generic, "Internal error accepting connection.  Can't get NetworkingMessages interface" );
							return false;
						}

						if ( !pSteamNetworkingMessages->BAcceptConnection( pConn, usecNow, errMsg ) )
						{
							pConn->ConnectionDestroySelfNow();
							SendP2PRejection( pContext, identityRemote, msg, k_ESteamNetConnectionEnd_Misc_Generic, "Internal error accepting connection.  %s", errMsg );
							return false;
						}
					}
					else
					{
						// Locate the listen socket
						int idxListenSock = m_mapListenSocketsByVirtualPort.Find( msgConnectRequest.virtual_port() );
						if ( idxListenSock == m_mapListenSocketsByVirtualPort.InvalidIndex() )
						{

							// Totally ignore it.  We don't want this to be able to be used as a way to
							// tell if you are online or not.
							SpewDebug( "Ignoring P2P CMsgSteamDatagramConnectRequest from %s; we're not listening on virtual port %d\n", SteamNetworkingIdentityRender( pConn->m_identityRemote ).c_str(), msgConnectRequest.virtual_port() );

							pConn->ConnectionDestroySelfNow();
							return false;
						}
						CSteamNetworkListenSocketP2P *pListenSock = m_mapListenSocketsByVirtualPort[ idxListenSock ];
						if ( !pListenSock->BAddChildConnection( pConn, errMsg ) )
						{
							SpewDebug( "Ignoring P2P CMsgSteamDatagramConnectRequest from %s on virtual port %d; %s\n", SteamNetworkingIdentityRender( pConn->m_identityRemote ).c_str(), msgConnectRequest.virtual_port(), errMsg );
							pConn->ConnectionDestroySelfNow();
							return false;
						}
					}
				#else
					SpewDebug( "Ignoring P2P CMsgSteamDatagramConnectRequest from %s on virtual port %d; no default signaling\n", SteamNetworkingIdentityRender( pConn->m_identityRemote ).c_str(), msgConnectRequest.virtual_port() );
					pConn->ConnectionDestroySelfNow();
					return false;
				#endif
			}

			// OK, start setting up the connection
			if ( !pConn->BBeginAccept( 
				msgConnectRequest,
				errMsg,
				usecNow
			) ) {
				pConn->ConnectionDestroySelfNow();
				SendP2PRejection( pContext, identityRemote, msg, k_ESteamNetConnectionEnd_Misc_Generic, "Internal error accepting connection.  %s", errMsg );
				return false;
			}

			// Inform app about the incoming request, see what they want to do
			pConn->m_pSignaling = pContext->OnConnectRequest( pConn->m_hConnectionSelf, identityRemote );

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
					SendP2PRejection( pContext, identityRemote, msg, k_ESteamNetConnectionEnd_Misc_Generic, "Internal error accepting connection.  %s", errMsg );
					pConn->ConnectionDestroySelfNow();
					return true;

				case k_ESteamNetworkingConnectionState_Connecting:

					// If they returned null, that means they want to totally ignore it.
					if ( !pConn->m_pSignaling )
					{
						// They decided to ignore it, by just returning null
						pConn->ConnectionDestroySelfNow();
						return true;
					}

					// They return signaling, which means that they will consider accepting it.
					// But they didn't accept it, so they want to go through the normal
					// callback mechanism.
					if ( !pConn->m_pMessagesInterface )
						pConn->PostConnectionStateChangedCallback( k_ESteamNetworkingConnectionState_None, k_ESteamNetworkingConnectionState_Connecting );
					break;

				case k_ESteamNetworkingConnectionState_Connected:
					AssertMsg( false, "How did we already get connected?  We should be finding route?");
				case k_ESteamNetworkingConnectionState_FindingRoute:
					// They accepted the request already.
					break;
			}

			// Fire up ICE
			// FIXME Really we should wait until the app accepts the connection, if it hasn't already
			if ( msg.has_webrtc() )
				pConn->CheckInitICE();
		}

		// Stop suppressing state change notifications
		pConn->m_bSupressStateChangeCallbacks = false;
	}

	// Go ahead and process the SDR routes, if they are sending them
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		if ( pConn->m_pTransportP2PSDR )
		{
			if ( msg.has_sdr_routes() )
				pConn->m_pTransportP2PSDR->RecvRoutes( msg.sdr_routes() );
			pConn->m_pTransportP2PSDR->CheckRecvRoutesAck( msg );
		}
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( pConn->m_pTransportICE )
		{
			// If they send rendezvous, then process it
			if ( msg.has_webrtc() )
			{
				pConn->m_pTransportICE->RecvRendezvous( msg.webrtc(), usecNow );
			}
			else
			{
				// The lack of any message at all (even an empty one) means that they
				// will not support ICE, so we can destroy our transport
				SpewType( nLogLevel, "[%s] Destroying ICE transport, peer rendezvous indicates they will not use it\n", pConn->GetDescription() );
				pConn->DestroyICENow();
			}
		}
	#endif

	// Already closed?
	switch ( pConn->GetState() )
	{
		default:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Dead: // shouldn't be in the map!
			Assert( false );
			// FALLTHROUGH
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			pConn->SendConnectionClosedSignal( usecNow );
			return true;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			// Must be stray / out of order message, since we think they already closed
			// the connection.
			pConn->SendNoConnectionSignal( usecNow );
			return true;

		case k_ESteamNetworkingConnectionState_Connecting:

			if ( msg.has_connect_ok() )
			{
				if ( pConn->m_bConnectionInitiatedRemotely )
				{
					SpewWarning( "Ignoring P2P connect_ok from %s, since they initiated the connection\n", SteamNetworkingIdentityRender( identityRemote ).c_str() );
					return false;
				}

				SpewType( nLogLevel, "[%s] Received ConnectOK in P2P Rendezvous from %s.\n", pConn->GetDescription(), SteamNetworkingIdentityRender( identityRemote ).c_str() );
				pConn->ProcessSignal_ConnectOK( msg.connect_ok(), usecNow );
			}
			break;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_FindingRoute:
		case k_ESteamNetworkingConnectionState_Connected:

			// Now that we know we still might want to talk to them,
			// check for redundant connection request.  (Our reply dropped.)
			if ( msg.has_connect_request() )
			{
				if ( pConn->m_bConnectionInitiatedRemotely )
				{
					// NOTE: We're assuming here that it actually is a redundant retry,
					//       meaning they specified all the same parameters as before!
					pConn->SendConnectOKSignal( usecNow );
				}
				else
				{
					AssertMsg( false, "Received ConnectRequest in P2P rendezvous message, but we are the 'client'!" );
				}
			}
			break;
	}

	return true;
}

} // namespace SteamNetworkingSocketsLib
