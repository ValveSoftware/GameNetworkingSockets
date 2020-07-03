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

constexpr SteamNetworkingMicroseconds k_usecWaitForControllingAgentBeforeSelectingNonNominatedTransport = 1*k_nMillion;

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketP2P
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkListenSocketP2P::CSteamNetworkListenSocketP2P( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkListenSocketBase( pSteamNetworkingSocketsInterface )
, m_nLocalVirtualPort( -1 )
{
}

CSteamNetworkListenSocketP2P::~CSteamNetworkListenSocketP2P()
{
	// Remove from virtual port map
	if ( m_nLocalVirtualPort >= 0 )
	{
		int h = m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.Find( m_nLocalVirtualPort );
		if ( h != m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.InvalidIndex() && m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort[h] == this )
		{
			m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort[h] = nullptr; // just for grins
			m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.RemoveAt( h );
		}
		else
		{
			AssertMsg( false, "Bookkeeping bug!" );
		}
		m_nLocalVirtualPort = -1;
	}
}

bool CSteamNetworkListenSocketP2P::BInit( int nLocalVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	Assert( nLocalVirtualPort >= 0 );

	// We need SDR functionality in order to support P2P
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		CSteamNetworkingSocketsSDR *pSteamNetworkingSocketsSDR = assert_cast< CSteamNetworkingSocketsSDR *>( m_pSteamNetworkingSocketsInterface );
		if ( !pSteamNetworkingSocketsSDR->BSDRClientInit( errMsg ) )
			return false;
	#endif

	if ( m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.HasElement( nLocalVirtualPort ) )
	{
		V_sprintf_safe( errMsg, "Already have a listen socket on P2P virtual port %d", nLocalVirtualPort );
		return false;
	}
	m_pSteamNetworkingSocketsInterface->m_mapListenSocketsByVirtualPort.Insert( nLocalVirtualPort, this );
	m_nLocalVirtualPort = nLocalVirtualPort;

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
	m_usecWhenStartedFindingRoute = 0;
	m_usecNextEvaluateTransport = k_nThinkTime_ASAP;
	m_bTransportSticky = false;

	m_pszNeedToSendSignalReason = nullptr;
	m_usecSendSignalDeadline = k_nThinkTime_Never;
	m_nLastSendRendesvousMessageID = 0;
	m_nLastRecvRendesvousMessageID = 0;
	m_pPeerSelectedTransport = nullptr;

	m_pCurrentTransportP2P = nullptr;
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		m_pTransportP2PSDR = nullptr;
	#endif
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		m_pTransportICE = nullptr;
		m_pTransportICEPendingDelete = nullptr;
		m_szICECloseMsg[ 0 ] = '\0';
	#endif
}

CSteamNetworkConnectionP2P::~CSteamNetworkConnectionP2P()
{
	Assert( m_idxMapIncomingP2PConnections == -1 );
}

void CSteamNetworkConnectionP2P::GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const
{
	if ( m_pCurrentTransportP2P )
		V_sprintf_safe( szDescription, "P2P %s %s", m_pCurrentTransportP2P->m_pszP2PTransportDebugName, SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
	else
		V_sprintf_safe( szDescription, "P2P %s", SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
}

bool CSteamNetworkConnectionP2P::BInitConnect( ISteamNetworkingConnectionCustomSignaling *pSignaling, const SteamNetworkingIdentity *pIdentityRemote, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	Assert( !m_pTransport );

	// Remember who we're talking to
	Assert( m_pSignaling == nullptr );
	m_pSignaling = pSignaling;
	if ( pIdentityRemote )
		m_identityRemote = *pIdentityRemote;
	m_nRemoteVirtualPort = nRemoteVirtualPort;

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
		m_vecAvailableTransports.push_back( m_pTransportP2PSDR );
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

	// Start the connection state machine
	return BConnectionState_Connecting( usecNow, errMsg );
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

	// Did we already fail?
	if ( GetICEFailureCode() != 0 )
		return;

	// Fetch enabled option
	int P2P_Transport_ICE_Enable = m_connectionConfig.m_P2P_Transport_ICE_Enable.Get();
	if ( P2P_Transport_ICE_Enable < 0 )
	{

		// Ask platform if we should enable it for this peer
		P2P_Transport_ICE_Enable = m_pSteamNetworkingSocketsInterface->GetP2P_Transport_ICE_Enable( m_identityRemote );
	}

	// Burn it into the connection config, if we inherited it, since we cannot change it
	// after this point
	m_connectionConfig.m_P2P_Transport_ICE_Enable.Set( P2P_Transport_ICE_Enable );

	// Disabled?
	if ( P2P_Transport_ICE_Enable <= 0 )
	{
		ICEFailed( k_nICECloseCode_Local_UserNotEnabled, "ICE not enabled by local user options" );
		return;
	}

	// No ICE factory?
	if ( !g_SteamNetworkingSockets_CreateICESessionFunc )
	{
		// !HACK! Just try to load up the dll directly
		#ifdef STEAMNETWORKINGSOCKETS_PARTNER
			static bool tried;
			if ( !tried )
			{
				tried = true;
				SteamDatagramTransportLock::SetLongLockWarningThresholdMS( "LoadICEDll", 500 );

				#if defined( _WIN32 )
					HMODULE h = ::LoadLibraryA( "steamwebrtc.dll" );
					if ( h != NULL )
					{
						g_SteamNetworkingSockets_CreateICESessionFunc = (CreateICESession_t)::GetProcAddress( h, "CreateWebRTCICESession" );
					}
				//#elif defined( OSX ) || defined( IOS ) || defined( TVOS )
				//	pszModule = "libsteamwebrtc.dylib";
				//#elif defined( LINUX ) || defined( ANDROID )
				//	pszModule = "libsteamwebrtc.so";
				//#else
				//	#error Need steamwebrtc for this platform
				#endif
			}
		#endif
		if ( !g_SteamNetworkingSockets_CreateICESessionFunc )
		{
			ICEFailed( k_ESteamNetConnectionEnd_Misc_InternalError, "No ICE session factory" );
			return;
		}
	}

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	m_pTransportICE = new CConnectionTransportP2PICE( *this );
	m_pTransportICE->Init();

	// Process rendezvous messages that were pended
	for ( int i = 0 ; i < len( m_vecPendingICEMessages ) && m_pTransportICE ; ++i )
		m_pTransportICE->RecvRendezvous( m_vecPendingICEMessages[i], usecNow );
	m_vecPendingICEMessages.clear();

	// If we failed, go ahead and cleanup now
	CheckCleanupICE();

	// If we're still all good, then add it to the list of options
	if ( m_pTransportICE )
	{
		m_vecAvailableTransports.push_back( m_pTransportICE );

		// Set a field in the ice session summary message,
		// which is how we will remember that we did attempt to use ICE
		Assert( !m_msgICESessionSummary.has_local_candidate_types() );
		m_msgICESessionSummary.set_local_candidate_types( 0 );
	}
#endif
}


void CSteamNetworkConnectionP2P::EnsureICEFailureReasonSet( SteamNetworkingMicroseconds usecNow )
{
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

	// Already have a reason?
	if ( m_msgICESessionSummary.has_failure_reason_code() )
		return;

	// If we never tried ICE, then there's no "failure"!
	if ( !m_msgICESessionSummary.has_local_candidate_types() )
		return;

	// Classify failure, and make it permanent
	ESteamNetConnectionEnd nReasonCode;
	GuessICEFailureReason( nReasonCode, m_szICECloseMsg, usecNow );
	m_msgICESessionSummary.set_failure_reason_code( nReasonCode );
	int nSeverity = ( nReasonCode != 0 && nReasonCode != k_nICECloseCode_Aborted ) ? k_ESteamNetworkingSocketsDebugOutputType_Msg : k_ESteamNetworkingSocketsDebugOutputType_Verbose;
	SpewTypeGroup( nSeverity, LogLevel_P2PRendezvous(), "[%s] Guessed ICE failure to be %d: %s\n",
		GetDescription(), nReasonCode, m_szICECloseMsg );

#endif
}

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
void CSteamNetworkConnectionP2P::GuessICEFailureReason( ESteamNetConnectionEnd &nReasonCode, ConnectionEndDebugMsg &msg, SteamNetworkingMicroseconds usecNow )
{
	// Already have a reason?
	if ( m_msgICESessionSummary.has_failure_reason_code() )
	{
		nReasonCode = ESteamNetConnectionEnd( m_msgICESessionSummary.failure_reason_code() );
		V_strcpy_safe( msg, m_szICECloseMsg );
		return;
	}

	// This should not be called if we never even tried
	Assert( m_msgICESessionSummary.has_local_candidate_types() );

	// This ought to be called before we cleanup and destroy the info we need
	Assert( m_pTransportICE );

	// If we are connected right now, then there is no problem!
	if ( m_pTransportICE && !m_pTransportICE->m_bNeedToConfirmEndToEndConnectivity )
	{
		nReasonCode = k_ESteamNetConnectionEnd_Invalid;
		V_strcpy_safe( msg, "OK" );
		return;
	}

	// Did we ever pierce NAT?  If so, then we just dropped connection.
	if ( m_msgICESessionSummary.has_negotiation_ms() )
	{
		nReasonCode = k_ESteamNetConnectionEnd_Misc_Timeout;
		V_strcpy_safe( msg, "ICE connection dropped after successful negotiation" );
		return;
	}

	// OK, looks like we never pierced NAT.  Try to figure out why.
	const int nAllowedTypes = m_pTransportICE ? m_pTransportICE->m_nAllowedCandidateTypes : 0;
	const int nGatheredTypes = m_msgICESessionSummary.local_candidate_types();
	const int nFailedToGatherTypes = nAllowedTypes & ~nGatheredTypes;
	const int nRemoteTypes = m_msgICESessionSummary.remote_candidate_types();

	// Terminated prematurely?  Presumably the higher level code hs a reason,
	// and so this will only be used for analytics.
	if ( m_usecWhenStartedFindingRoute == 0 || m_usecWhenStartedFindingRoute+5*k_nMillion > usecNow )
	{
		nReasonCode = ESteamNetConnectionEnd( k_nICECloseCode_Aborted );
		V_strcpy_safe( msg, "NAT traversal aborted" );
		return;
	}

	// If we enabled all host candidates, and failed to gather any, then we have a problem
	// on our end.  Note that if we only allow one or the other kind, or only IPv4, etc, that
	// there are network configurations where we may legit fail to gather candidates.  (E.g.
	// their IP address is public and they don't have a LAN IP.  Or they only have IPv6.)  But
	// every computer should have *some* IP, and if we enabled all host candidate types (which
	// will be a in important use case worth handling specifically), then we should gather some
	// host candidates.
	const int k_EICECandidate_Any_Host = k_EICECandidate_Any_HostPrivate | k_EICECandidate_Any_HostPublic;
	if ( ( nFailedToGatherTypes & k_EICECandidate_Any_Host ) == k_EICECandidate_Any_Host )
	{
		// We should always be able to collect these sorts of candidates!
		nReasonCode = k_ESteamNetConnectionEnd_Misc_InternalError;
		V_strcpy_safe( msg, "Never gathered *any* host candidates?" );
		return;
	}

	// Never received *any* candidates from them?
	if ( nRemoteTypes == 0 )
	{
		// FIXME - not we probably can detect if it's likely to be on their end.
		// If we are getting signals from them, just none with any candidates,
		// then it's very likely on their end, not just because they gathered
		// them but couldn't send them to us.
		nReasonCode = k_ESteamNetConnectionEnd_Misc_Generic;
		V_strcpy_safe( msg, "Never received any remote candidates" );
		return;
	}

	// We failed to STUN?
	if ( ( nAllowedTypes & k_EICECandidate_Any_Reflexive ) != 0 && ( nGatheredTypes & (k_EICECandidate_Any_Reflexive|k_EICECandidate_IPv4_HostPublic) ) == 0 )
	{
		if ( m_connectionConfig.m_P2P_STUN_ServerList.Get().empty() )
		{
			nReasonCode = k_ESteamNetConnectionEnd_Misc_InternalError;
			V_strcpy_safe( msg, "No configured STUN servers" );
			return;
		}
		nReasonCode = k_ESteamNetConnectionEnd_Local_P2P_ICE_NoPublicAddresses;
		V_strcpy_safe( msg, "Failed to determine our public address via STUN" );
		return;
	}

	// FIXME - we should probably handle this as a special case.  TURN candidates
	// should basically always work
	//if ( (nAllowedTypes|nGatheredTypes) | k_EICECandidate_Any_Relay )
	//{
	//}

	// Any candidates from remote host that we really ought to have been able to talk to?
	if ( !(nRemoteTypes & ( k_EICECandidate_IPv4_HostPublic|k_EICECandidate_Any_Reflexive|k_EICECandidate_Any_Relay) ) )
	{
		nReasonCode = k_ESteamNetConnectionEnd_Remote_P2P_ICE_NoPublicAddresses;
		V_strcpy_safe( msg, "No public or relay candidates from remote host" );
		return;
	}

	// NOTE: in theory, we could haveIPv4 vs IPv6 capabilities mismatch.  In practice
	// does that ever happen?

	// OK, both sides shared reflexive candidates, but we still failed?  This is probably
	// a firewall thing
	nReasonCode = k_ESteamNetConnectionEnd_Misc_P2P_NAT_Firewall;
	V_strcpy_safe( msg, "NAT traversal failed" );
}
#endif

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
		SelectTransport( nullptr, SteamNetworkingSockets_GetLocalTimestamp() );
		m_usecNextEvaluateTransport = k_nThinkTime_ASAP;
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

	m_vecPendingICEMessages.clear();
#endif

}

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
void CSteamNetworkConnectionP2P::ICEFailed( int nReasonCode, const char *pszReason )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	// Remember reason code, if we didn't already set one
	if ( GetICEFailureCode() == 0 )
	{
		SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] ICE failed %d %s\n", GetDescription(), nReasonCode, pszReason );
		m_msgICESessionSummary.set_failure_reason_code( nReasonCode );
		V_strcpy_safe( m_szICECloseMsg, pszReason );
	}

	// Queue for deletion
	if ( !m_pTransportICEPendingDelete )
	{
		m_pTransportICEPendingDelete = m_pTransportICE;
		m_pTransportICE = nullptr;

		// Make sure we clean ourselves up as soon as it is safe to do so
		SetNextThinkTimeASAP();
	}
}
#endif

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
	m_pCurrentTransportP2P = nullptr;
	m_vecAvailableTransports.clear();

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

	// Check for enabling ICE
	CheckInitICE();

	// Send them a reply, and include whatever info we have right now
	SendConnectOKSignal( usecNow );

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

void CSteamNetworkConnectionP2P::TransportEndToEndConnectivityChanged( CConnectionTransportP2PBase *pTransport, SteamNetworkingMicroseconds usecNow )
{
	// A major event has happened.  Don't be sticky to current transport.
	m_bTransportSticky = false;

	// Schedule us to wake up immediately and deal with it.
	m_usecNextEvaluateTransport = k_nThinkTime_ASAP;
	SetNextThinkTimeASAP();

	// Reset counter to make sure we collect a few more, either immediately if we can, or when
	// we come back alive.  Also, this makes sure that as soon as we get confirmed connectivity,
	// that we send something to the peer so they can get confirmation, too.
	pTransport->m_nKeepTryingToPingCounter = std::max( pTransport->m_nKeepTryingToPingCounter, 5 );

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

		// Wwich transport?
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
			EnsureICEFailureReasonSet( usecNow );
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			break;

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
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
				pTransportP2P->m_pSelfAsThinker->SetNextThinkTimeASAP();

			break;
	}

	// Inform transports
	for ( CConnectionTransportP2PBase *pTransportP2P: m_vecAvailableTransports )
		pTransportP2P->m_pSelfAsConnectionTransport->TransportConnectionStateChanged( eOldState );
}

void CSteamNetworkConnectionP2P::ThinkConnection( SteamNetworkingMicroseconds usecNow )
{
	CSteamNetworkConnectionBase::ThinkConnection( usecNow );

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		CheckCleanupICE();
	#endif

	// Check for sending signals pending for RTO or Nagle.
	// (If we have gotten far enough along where we know where
	// to send them.  Some messages can be queued very early, and
	// do not depend on who the peer it.)
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting )
	{

		// Process route selection
		ThinkSelectTransport( usecNow );

		// If nothing scheduled, check RTOs.  If we have something scheduled,
		// wait for the timer. The timer is short and designed to avoid
		// a blast, so let it do its job.
		if ( m_usecSendSignalDeadline == k_nThinkTime_Never )
		{
			for ( const OutboundMessage &s: m_vecUnackedOutboundMessages )
			{
				if ( s.m_usecRTO < m_usecSendSignalDeadline )
				{
					m_usecSendSignalDeadline = s.m_usecRTO;
					m_pszNeedToSendSignalReason = "MessageRTO";
					// Keep scanning the list.  we want to collect
					// the minimum RTO.
				}
			}
		}

		if ( usecNow >= m_usecSendSignalDeadline )
		{
			Assert( m_pszNeedToSendSignalReason );

			// Send a signal
			CMsgSteamNetworkingP2PRendezvous msgRendezvous;
			SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, m_pszNeedToSendSignalReason );
		}

		Assert( m_usecSendSignalDeadline > usecNow );

		EnsureMinThinkTime( m_usecSendSignalDeadline );
	}
}

void CSteamNetworkConnectionP2P::ThinkSelectTransport( SteamNetworkingMicroseconds usecNow )
{

	// Time to evaluate which transport to use?
	if ( usecNow < m_usecNextEvaluateTransport )
	{
		EnsureMinThinkTime( m_usecNextEvaluateTransport );
		return;
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
		case k_ESteamNetworkingConnectionState_FindingRoute:
			m_usecNextEvaluateTransport = usecNow + k_nMillion; // Check back periodically
			break;
	}

	bool bEvaluateFrequently = false;

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
								m_usecNextEvaluateTransport = std::min( m_usecNextEvaluateTransport, pTransport->m_usecEndToEndInFlightReplyTimeout ); \
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

	// Spew about this event
	const int nLogLevel = LogLevel_P2PRendezvous();
	if ( nLogLevel >= k_ESteamNetworkingSocketsDebugOutputType_Verbose )
	{
		if ( pTransportP2P == nullptr )
		{
			if ( BStateIsActive() ) // Don't spew about cleaning up
				ReallySpewType( nLogLevel, "[%s] Deselected '%s' transport, no transport currently active!\n", GetDescription(), m_pCurrentTransportP2P->m_pszP2PTransportDebugName );
		}
		else if ( m_pCurrentTransportP2P == nullptr )
		{
			ReallySpewType( nLogLevel, "[%s] Selected '%s' transport (ping=%d, score=%d+%d)\n", GetDescription(),
				pTransportP2P->m_pszP2PTransportDebugName, pTransportP2P->m_pingEndToEnd.m_nSmoothedPing, pTransportP2P->m_routeMetrics.m_nScoreCurrent, pTransportP2P->m_routeMetrics.m_nTotalPenalty );
		}
		else
		{
			ReallySpewType( nLogLevel, "[%s] Switched to '%s' transport (ping=%d, score=%d=%d) from '%s' (ping=%d, score=%d+%d)\n", GetDescription(),
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
	m_bTransportSticky = false; // Assume we won't be sticky for now
	m_usecNextEvaluateTransport = k_nThinkTime_ASAP;
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
	if ( m_pTransport && IsControllingAgent() )
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

	// When using ICE, it takes just a few milliseconds to collect the local candidates.
	// We'd like to send those in the initial connect request
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( m_pTransportICE )
		{
			SteamNetworkingMicroseconds usecWaitForICE = GetTimeEnteredConnectionState() + 5*1000;
			if ( usecNow < usecWaitForICE )
				return usecWaitForICE;
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
	SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] Sending P2P ConnectRequest\n", GetDescription() );
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
	SpewMsgGroup( LogLevel_P2PRendezvous(), "[%s] Sending P2P ConnectOK via Steam, remote cxn %u\n", GetDescription(), m_unConnectionIDRemote );
	SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, "ConnectOK" );
}

void CSteamNetworkConnectionP2P::SendConnectionClosedSignal( SteamNetworkingMicroseconds usecNow )
{
	SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Sending graceful P2P ConnectionClosed, remote cxn %u\n", GetDescription(), m_unConnectionIDRemote );

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
		if ( m_pTransportICE )
			m_pTransportICE->PopulateRendezvousMsg( msg, usecNow );
	#endif

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
				{
					if ( !msg.has_connect_request() )
						ScheduleSendSignal( "ContinueLargeSignal" );
					break;
				}

				// Start sending from this guy forward
				msg.set_first_reliable_msg( s.m_nID );
			}

			*msg.add_reliable_messages() = s.m_msg;
			nTotalMsgSize += s.m_cbSerialized;

			s.m_usecRTO = usecNow + k_nMillion/2; // Reset RTO
		}

		// Go ahead and always ack, even if we don't need to, because this is small
		msg.set_ack_reliable_msg( m_nLastRecvRendesvousMessageID );
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
	}
}

bool CSteamNetworkConnectionP2P::ProcessSignal( const CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{

	// Go ahead and process the SDR routes, if they are sending them
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
		if ( m_pTransportP2PSDR )
		{
			if ( msg.has_sdr_routes() )
				m_pTransportP2PSDR->RecvRoutes( msg.sdr_routes() );
			m_pTransportP2PSDR->CheckRecvRoutesAck( msg );
		}
	#endif

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
		if ( !msg.ice_enabled() )
			ICEFailed( k_nICECloseCode_Remote_NotEnabled, "Peer sent signal without ice_enabled set" );
	#endif

	// Check for acking reliable messages
	if ( msg.ack_reliable_msg() > 0 )
	{

		// Remove messages that are being acked
		while ( !m_vecUnackedOutboundMessages.empty() && m_vecUnackedOutboundMessages[0].m_nID <= msg.ack_reliable_msg() )
			erase_at( m_vecUnackedOutboundMessages, 0 );

		// If anything ready to retry now, schedule wakeup
		if ( m_usecSendSignalDeadline == k_nThinkTime_Never )
		{
			SteamNetworkingMicroseconds usecNextRTO = k_nThinkTime_Never;
			for ( const OutboundMessage &s: m_vecUnackedOutboundMessages )
				usecNextRTO = std::min( usecNextRTO, s.m_usecRTO );
			EnsureMinThinkTime( usecNextRTO );
		}
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
					SendConnectOKSignal( usecNow );
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

void CSteamNetworkConnectionP2P::QueueSignalReliableMessage( CMsgSteamNetworkingP2PRendezvous_ReliableMessage &&msg, const char *pszDebug )
{
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

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingSockets CConnectionTransportP2PBase
//
/////////////////////////////////////////////////////////////////////////////

CConnectionTransportP2PBase::CConnectionTransportP2PBase( const char *pszDebugName, CConnectionTransport *pSelfBase, IThinker *pSelfThinker )
: m_pszP2PTransportDebugName( pszDebugName )
, m_pSelfAsConnectionTransport( pSelfBase )
, m_pSelfAsThinker( pSelfThinker )
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

		m_pSelfAsThinker->EnsureMinThinkTime( m_usecEndToEndInFlightReplyTimeout );
	}
}

void CConnectionTransportP2PBase::P2PTransportThink( SteamNetworkingMicroseconds usecNow )
{
	CSteamNetworkConnectionP2P &conn = Connection();

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
		if ( m_usecEndToEndInFlightReplyTimeout < usecNow )
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
	m_pSelfAsThinker->EnsureMinThinkTime( usecNextThink );
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

HSteamNetConnection CSteamNetworkingSockets::ConnectP2PCustomSignaling( ISteamNetworkingConnectionCustomSignaling *pSignaling, const SteamNetworkingIdentity *pPeerIdentity, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{
	if ( !pSignaling )
		return k_HSteamNetConnection_Invalid;

	SteamDatagramTransportLock scopeLock( "ConnectP2PCustomSignaling" );
	return InternalConnectP2P( pSignaling, pPeerIdentity, -1, nOptions, pOptions );
}

HSteamNetConnection CSteamNetworkingSockets::InternalConnectP2P( ISteamNetworkingConnectionCustomSignaling *pSignaling, const SteamNetworkingIdentity *pPeerIdentity, int nRemoteVirtualPort, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{

	CSteamNetworkConnectionP2P *pConn = new CSteamNetworkConnectionP2P( this );
	if ( !pConn )
	{
		pSignaling->Release();
		return k_HSteamNetConnection_Invalid;
	}

	SteamDatagramErrMsg errMsg;
	if ( !pConn->BInitConnect( pSignaling, pPeerIdentity, nRemoteVirtualPort, nOptions, pOptions, errMsg ) )
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
			Assert( pConn->m_nSupressStateChangeCallbacks == 0 );
			pConn->m_nSupressStateChangeCallbacks = 1;

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
		}

		// Stop suppressing state change notifications
		Assert( pConn->m_nSupressStateChangeCallbacks == 1 );
		pConn->m_nSupressStateChangeCallbacks = 0;
	}

	// Process the message
	return pConn->ProcessSignal( msg, usecNow );
}

} // namespace SteamNetworkingSocketsLib
