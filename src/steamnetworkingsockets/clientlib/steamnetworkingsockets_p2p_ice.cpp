//====== Copyright Valve Corporation, All rights reserved. ====================

#include "../steamnetworkingsockets_internal.h"
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE

#include "steamnetworkingsockets_p2p_ice.h"
#include "steamnetworkingsockets_udp.h"

#include "steamnetworkingsockets_stun.h"

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC
	#include "steamnetworkingsockets_p2p_webrtc.h"

	#ifdef STEAMWEBRTC_USE_STATIC_LIBS
		extern "C" IICESession *CreateWebRTCICESession( const ICESessionConfig &cfg, IICESessionDelegate *pDelegate, int nInterfaceVersion );
	#endif
#endif

#ifdef _WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	#undef min
	#undef max
#endif
#if IsPosix() && defined( STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC )
	#include <dlfcn.h>
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"


// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkConnectionP2P ICE-related functions
//
/////////////////////////////////////////////////////////////////////////////

void CSteamNetworkConnectionP2P::CheckInitICE()
{
	AssertLocksHeldByCurrentThread( "CSteamNetworkConnectionP2P::CheckInitICE" );

	// Did we already fail?
	if ( GetICEFailureCode() != 0 )
		return;

	// Already created?
	if ( m_pTransportICE )
		return;
	Assert( !m_pTransportICEPendingDelete );
	CheckCleanupICE();

	if ( IsSDRHostedServerClient() || IsSDRHostedServer() )
	{
		// Don't use ICEFailed() here.  We don't we don't want to spew and don't need anything else it does
		m_msgICESessionSummary.set_failure_reason_code( k_nICECloseCode_Local_Special );
		return;
	}

	// Fetch enabled option
	int P2P_Transport_ICE_Enable = m_connectionConfig.P2P_Transport_ICE_Enable.Get();
	if ( P2P_Transport_ICE_Enable < 0 )
	{

		// Ask platform if we should enable it for this peer
		int nUserFlags = -1;
		P2P_Transport_ICE_Enable = m_pSteamNetworkingSocketsInterface->GetP2P_Transport_ICE_Enable( m_identityRemote, &nUserFlags );
		if ( nUserFlags >= 0 )
		{
			m_msgICESessionSummary.set_user_settings( nUserFlags );
		}
	}

	// Burn it into the connection config, if we inherited it, since we cannot change it
	// after this point.  (Note in some cases we may be running this initialization
	// for a second time, restarting ICE, so it might already be locked.)
	if ( !m_connectionConfig.P2P_Transport_ICE_Enable.IsLocked() )
	{
		m_connectionConfig.P2P_Transport_ICE_Enable.Set( P2P_Transport_ICE_Enable );
		m_connectionConfig.P2P_Transport_ICE_Enable.Lock();
	}

	// Disabled?
	if ( P2P_Transport_ICE_Enable <= 0 )
	{
		ICEFailed( k_nICECloseCode_Local_UserNotEnabled, "ICE not enabled by local user options" );
		return;
	}

	m_msgICESessionSummary.set_ice_enable_var( P2P_Transport_ICE_Enable );

	//
	// Configure ICE client options
	//

	ICESessionConfig cfg;

	// Generate local ufrag and password
	std::string sUfragLocal = Base64EncodeLower30Bits( m_unConnectionIDLocal );
	uint32 nPwdFrag;
	CCrypto::GenerateRandomBlock( &nPwdFrag, sizeof(nPwdFrag) );
	std::string sPwdFragLocal = Base64EncodeLower30Bits( nPwdFrag );
	cfg.m_pszLocalUserFrag = sUfragLocal.c_str();
	cfg.m_pszLocalPwd = sPwdFragLocal.c_str();

	// Set role
	cfg.m_eRole = IsControllingAgent() ? k_EICERole_Controlling : k_EICERole_Controlled;

	cfg.m_nCandidateTypes = 0;
	if ( P2P_Transport_ICE_Enable & k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Private )
		cfg.m_nCandidateTypes |= k_EICECandidate_Any_HostPrivate;

	// Get the STUN server list
	std_vector<std::string> vecStunServers;
	std_vector<const char *> vecStunServersPsz;
	if ( P2P_Transport_ICE_Enable & k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Public )
	{
		cfg.m_nCandidateTypes |= k_EICECandidate_Any_HostPublic|k_EICECandidate_Any_Reflexive;

		{
			CUtlVectorAutoPurge<char *> tempStunServers;
			V_AllocAndSplitString( m_connectionConfig.P2P_STUN_ServerList.Get().c_str(), ",", tempStunServers );
			for ( const char *pszAddress: tempStunServers )
			{
				std::string server;

				// Add prefix, unless they already supplied it
				if ( V_strnicmp( pszAddress, "stun:", 5 ) != 0 )
					server = "stun:";
				server.append( pszAddress );

				vecStunServers.push_back( std::move( server ) );
				vecStunServersPsz.push_back( vecStunServers.rbegin()->c_str() );
			}
		}
		if ( vecStunServers.empty() )
			SpewWarningGroup( LogLevel_P2PRendezvous(), "[%s] Reflexive candidates enabled by P2P_Transport_ICE_Enable, but P2P_STUN_ServerList is empty\n", GetDescription() );
		else
			SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Using STUN server list: %s\n", GetDescription(), m_connectionConfig.P2P_STUN_ServerList.Get().c_str() );
	}
	else
	{
		SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] Not using STUN servers as per P2P_Transport_ICE_Enable\n", GetDescription() );
	}
	cfg.m_nStunServers = len( vecStunServersPsz );
	cfg.m_pStunServers = vecStunServersPsz.data();

	// Get the TURN server list
	std_vector<std::string> vecTurnServerAddrs;
	CUtlVectorAutoPurge<char*> vecTurnUsers;
	CUtlVectorAutoPurge<char*> vecTurnPasses;
	std_vector<ICESessionConfig::TurnServer> vecTurnServers;
	if ( P2P_Transport_ICE_Enable & k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay )
	{
		cfg.m_nCandidateTypes |= k_EICECandidate_Any_Relay;

		{
			CUtlVectorAutoPurge<char*> tempTurnServers;
			V_AllocAndSplitString( m_connectionConfig.P2P_TURN_ServerList.Get().c_str(), ",", tempTurnServers, true );
			for (const char* pszAddress : tempTurnServers)
			{
				std::string server;

				// Add prefix, unless they already supplied it
				if (V_strnicmp(pszAddress, "turn:", 5) != 0)
					server = "turn:";
				server.append(pszAddress);

				vecTurnServerAddrs.push_back(std::move(server));
			}
		}

		if (vecTurnServerAddrs.empty())
		{
			SpewWarningGroup(LogLevel_P2PRendezvous(), "[%s] Relay candidates enabled by P2P_Transport_ICE_Enable, but P2P_TURN_ServerList is empty\n", GetDescription());
		}
		else
		{
			SpewVerboseGroup(LogLevel_P2PRendezvous(), "[%s] Using TURN server list: %s\n", GetDescription(), m_connectionConfig.P2P_TURN_ServerList.Get().c_str());
			cfg.m_nTurnServers = len(vecTurnServerAddrs);

			// populate usernames
			V_AllocAndSplitString( m_connectionConfig.P2P_TURN_UserList.Get().c_str(), ",", vecTurnUsers, true) ;

			// populate passwords
			V_AllocAndSplitString( m_connectionConfig.P2P_TURN_PassList.Get().c_str(), ",", vecTurnPasses, true );

			// If turn arrays lengths (servers, users and passes) are not match, treat all TURN servers as unauthenticated
			if ( !vecTurnUsers.IsEmpty() || !vecTurnPasses.IsEmpty() )
			{
				if ( cfg.m_nTurnServers != vecTurnUsers.Count() || cfg.m_nTurnServers != vecTurnPasses.Count() )
				{
					vecTurnUsers.PurgeAndDeleteElements();
					vecTurnPasses.PurgeAndDeleteElements();
					SpewWarningGroup(LogLevel_P2PRendezvous(), "[%s] TURN user/pass list is not same length as address list.  Treating all servers as unauthenticated!\n", GetDescription() );
				}
			}

			// Populate TurnServers configs
			for (int i = 0; i < cfg.m_nTurnServers; i++)
			{
				ICESessionConfig::TurnServer* turn = push_back_get_ptr( vecTurnServers );
				turn->m_pszHost = vecTurnServerAddrs[i].c_str();

				if ( vecTurnUsers.Count() > i)
					turn->m_pszUsername = vecTurnUsers[i];
				else
					turn->m_pszUsername = "";

				if ( vecTurnPasses.Count() > i)
					turn->m_pszPwd = vecTurnPasses[i];
				else
					turn->m_pszPwd = "";
			}

			cfg.m_pTurnServers = vecTurnServers.data();
		}
	}
	else
	{
		SpewVerboseGroup(LogLevel_P2PRendezvous(), "[%s] Not using TURN servers as per P2P_Transport_ICE_Enable\n", GetDescription());
	}


	if ( cfg.m_nStunServers == 0 )
		cfg.m_nCandidateTypes &= ~k_EICECandidate_Any_Reflexive;
	if ( cfg.m_nTurnServers == 0 )
		cfg.m_nCandidateTypes &= ~k_EICECandidate_Any_Relay;

	m_msgICESessionSummary.set_local_candidate_types_allowed( cfg.m_nCandidateTypes );
	SpewVerboseGroup( LogLevel_P2PRendezvous(), "[%s] P2P_Transport_ICE_Enable=0x%x, AllowedCandidateTypes=0x%x\n", GetDescription(), P2P_Transport_ICE_Enable, cfg.m_nCandidateTypes );

	// No candidates possible?
	if ( cfg.m_nCandidateTypes == 0 )
	{
		ICEFailed( k_nICECloseCode_Local_UserNotEnabled, "No local candidate types are allowed by user settings and configured servers" );
		return;
	}

	//
	// Select ICE client implementation and create the transport
	// WARNING: if we fail, the ICE transport will call ICEFailed, which sets m_pTransportICE=NULL
	//
	int ICE_Implementation = m_connectionConfig.P2P_Transport_ICE_Implementation.Get();

	// Apply default
	if ( ICE_Implementation == 0 )
	{
		// Current default is WebRTC=2
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC
			ICE_Implementation = 2;
		#else
			ICE_Implementation = 1;
		#endif
	}

	// Lock it in
	m_connectionConfig.P2P_Transport_ICE_Implementation.Set( ICE_Implementation );
	m_connectionConfig.P2P_Transport_ICE_Implementation.Lock();

	// "Native" ICE client?
	if ( ICE_Implementation == 1 ) 
	{
		auto pICEValve = new CConnectionTransportP2PICE_Valve( *this );
		m_pTransportICE = pICEValve;
		pICEValve->Init( cfg );
	}
	else if ( ICE_Implementation == 2 )
	{
		#ifndef STEAMNETWORKINGSOCKETS_ENABLE_WEBRTC
			ICEFailed( k_nICECloseCode_Local_NotCompiled, "WebRTC support not enabled" );
			return;
		#else

			// Make sure we have an interface to the WebRTC code, which might
			// live in another DLL
			#ifdef STEAMWEBRTC_USE_STATIC_LIBS
				// Static linkage, just set the pointer
				g_SteamNetworkingSockets_CreateICESessionFunc = (CreateICESession_t)CreateWebRTCICESession;
			#else

				// Try to load Load up the DLL the first time we need this
				if ( !g_SteamNetworkingSockets_CreateICESessionFunc )
				{

					// Only try one time
					static bool tried;
					if ( !tried )
					{
						SteamNetworkingErrMsg errMsg;
						tried = true;
						SteamNetworkingGlobalLock::SetLongLockWarningThresholdMS( "LoadICEDll", 500 );
						static const char pszExportFunc[] = "CreateWebRTCICESession";

						#if defined( _WINDOWS )
							#ifdef _WIN64
								static const char pszModule[] = "steamwebrtc64.dll";
							#else
								static const char pszModule[] = "steamwebrtc.dll";
							#endif
							HMODULE h = ::LoadLibraryA( pszModule );
							if ( h == NULL )
							{
								V_sprintf_safe( errMsg, "Failed to load %s.", pszModule ); // FIXME - error code?  Debugging DLL issues is so busted on Windows
								ICEFailed( k_nICECloseCode_Local_NotCompiled, errMsg );
								return;
							}
							g_SteamNetworkingSockets_CreateICESessionFunc = (CreateICESession_t)::GetProcAddress( h, pszExportFunc );
						#elif IsPosix()
							#if IsOSX() || defined( IOS ) || defined( TVOS )
								static const char pszModule[] = "libsteamwebrtc.dylib";
							#else
								static const char pszModule[] = "libsteamwebrtc.so";
							#endif
							void* h = dlopen(pszModule, RTLD_LAZY);
							if ( h == NULL )
							{
								V_sprintf_safe( errMsg, "Failed to dlopen %s.  %s", pszModule, dlerror() );
								ICEFailed( k_nICECloseCode_Local_NotCompiled, errMsg );
								return;
							}
							g_SteamNetworkingSockets_CreateICESessionFunc = (CreateICESession_t)dlsym( h, pszExportFunc );
						#else
							#error Need steamwebrtc for this platform
						#endif
						if ( !g_SteamNetworkingSockets_CreateICESessionFunc )
						{
							V_sprintf_safe( errMsg, "%s not found in %s.", pszExportFunc, pszModule );
							ICEFailed( k_nICECloseCode_Local_NotCompiled, errMsg );
							return;
						}
					}
					if ( !g_SteamNetworkingSockets_CreateICESessionFunc )
					{
						ICEFailed( k_nICECloseCode_Local_NotCompiled, "No ICE session factory" );
						return;
					}
				}
			#endif

			// Initialize WebRTC ICE client
			auto pICEWebRTC = new CConnectionTransportP2PICE_WebRTC( *this );
			m_pTransportICE = pICEWebRTC;
			pICEWebRTC->Init( cfg );
		#endif

	}
	else
	{
		ICEFailed( k_ESteamNetConnectionEnd_Misc_Generic, "Invalid P2P_Transport_ICE_Implementation value" );
		return;
	}

	// Queue a message to inform peer about our auth credentials.  It should
	// go out in the first signal.
	if ( m_pTransportICE )
	{
		CMsgSteamNetworkingP2PRendezvous_ReliableMessage msg;
		*msg.mutable_ice()->mutable_auth()->mutable_pwd_frag() = std::move( sPwdFragLocal );
		QueueSignalReliableMessage( std::move( msg ), "Initial ICE auth" );
	}

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Process any rendezvous messages that were pended
	for ( int i = 0 ; i < len( m_vecPendingICEMessages ) && m_pTransportICE ; ++i )
		m_pTransportICE->RecvRendezvous( m_vecPendingICEMessages[i], usecNow );
	m_vecPendingICEMessages.clear();

	// If we have failed here, go ahead and cleanup now
	CheckCleanupICE();

	// If we're still all good, then add it to the list of options
	if ( m_pTransportICE )
	{
		m_vecAvailableTransports.push_back( m_pTransportICE );

		// Set a field in the ice session summary message,
		// which is how we will remember that we did attempt to use ICE
		m_msgICESessionSummary.set_local_candidate_types( 0 );
	}
}


void CSteamNetworkConnectionP2P::EnsureICEFailureReasonSet( SteamNetworkingMicroseconds usecNow )
{

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
}

void CSteamNetworkConnectionP2P::GuessICEFailureReason( ESteamNetConnectionEnd &nReasonCode, ConnectionEndDebugMsg &msg, SteamNetworkingMicroseconds usecNow )
{
	// Already have a reason?
	if ( m_msgICESessionSummary.failure_reason_code() )
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
	const int nAllowedTypes = m_msgICESessionSummary.local_candidate_types_allowed();
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
		if ( m_connectionConfig.P2P_STUN_ServerList.Get().empty() )
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

void CSteamNetworkConnectionP2P::DestroyICENow()
{
	AssertLocksHeldByCurrentThread( "P2P DestroyICENow" );

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
}

void CSteamNetworkConnectionP2P::ICEFailed( int nReasonCode, const char *pszReason )
{
	AssertLocksHeldByCurrentThread();

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

/////////////////////////////////////////////////////////////////////////////
//
// CConnectionTransportP2PICE
//
/////////////////////////////////////////////////////////////////////////////

CConnectionTransportP2PICE::CConnectionTransportP2PICE( CSteamNetworkConnectionP2P &connection )
: CConnectionTransportUDPBase( connection )
, CConnectionTransportP2PBase( "ICE", this )
{
	m_eCurrentRouteKind = k_ESteamNetTransport_Unknown;
	m_currentRouteRemoteAddress.Clear();
}

CConnectionTransportP2PICE::~CConnectionTransportP2PICE()
{
}

void CConnectionTransportP2PICE::TransportPopulateConnectionInfo( SteamNetConnectionInfo_t &info ) const
{
	CConnectionTransport::TransportPopulateConnectionInfo( info );

	info.m_addrRemote = m_currentRouteRemoteAddress;
	switch ( m_eCurrentRouteKind )
	{
		default:
		case k_ESteamNetTransport_SDRP2P:
		case k_ESteamNetTransport_Unknown:
			// Hm...
			Assert( false );
			break;

		case k_ESteamNetTransport_LocalHost:
			info.m_nFlags |= k_nSteamNetworkConnectionInfoFlags_Fast;
			break;

		case k_ESteamNetTransport_UDP:
			break;

		case k_ESteamNetTransport_UDPProbablyLocal:
		{
			int nPingMin, nPingMax;
			m_pingEndToEnd.GetPingRangeFromRecentBuckets( nPingMin, nPingMax, SteamNetworkingSockets_GetLocalTimestamp() );
			if ( nPingMin < k_nMinPingTimeLocalTolerance )
				info.m_nFlags |= k_nSteamNetworkConnectionInfoFlags_Fast;
			break;
		}

		case k_ESteamNetTransport_TURN:
			info.m_nFlags |= k_nSteamNetworkConnectionInfoFlags_Relayed;
			info.m_addrRemote.Clear();
			break;
	}
}

void CConnectionTransportP2PICE::GetDetailedConnectionStatus( SteamNetworkingDetailedConnectionStatus &stats, SteamNetworkingMicroseconds usecNow )
{
	CConnectionTransportUDPBase::GetDetailedConnectionStatus( stats, usecNow );
	stats.m_eTransportKind = m_eCurrentRouteKind;
	if ( stats.m_eTransportKind == k_ESteamNetTransport_UDPProbablyLocal && !( stats.m_info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Fast ) )
		stats.m_eTransportKind = k_ESteamNetTransport_UDP;
}

// Base-64 encode the least significant 30 bits.
// Returns a 5-character base-64 string
std::string Base64EncodeLower30Bits( uint32 nNum )
{
	static const char szBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	char result[6] = {
		szBase64Chars[ ( nNum >> 24 ) & 63 ],
		szBase64Chars[ ( nNum >> 18 ) & 63 ],
		szBase64Chars[ ( nNum >> 12 ) & 63 ],
		szBase64Chars[ ( nNum >>  6 ) & 63 ],
		szBase64Chars[ ( nNum       ) & 63 ],
		'\0'
	};
	return std::string( result );
}

void CConnectionTransportP2PICE::PopulateRendezvousMsg( CMsgSteamNetworkingP2PRendezvous &msg, SteamNetworkingMicroseconds usecNow )
{
	msg.set_ice_enabled( true );
}

void CConnectionTransportP2PICE::P2PTransportUpdateRouteMetrics( SteamNetworkingMicroseconds usecNow )
{
	if ( !BCanSendEndToEndData() || m_pingEndToEnd.m_nSmoothedPing < 0 )
	{
		m_routeMetrics.SetInvalid();
		return;
	}

	int nPingMin, nPingMax;
	m_routeMetrics.m_nBucketsValid = m_pingEndToEnd.GetPingRangeFromRecentBuckets( nPingMin, nPingMax, usecNow );
	m_routeMetrics.m_nTotalPenalty = 0;

	// Set ping as the score
	m_routeMetrics.m_nScoreCurrent = m_pingEndToEnd.m_nSmoothedPing;
	m_routeMetrics.m_nScoreMin = nPingMin;
	m_routeMetrics.m_nScoreMax = nPingMax;

	// Local route?
	if ( nPingMin < k_nMinPingTimeLocalTolerance && m_eCurrentRouteKind == k_ESteamNetTransport_UDPProbablyLocal )
	{

		// Whoo whoo!  Probably NAT punched LAN

	}
	else
	{
		// Update score based on the fraction that we are going over the Internet,
		// instead of dedicated backbone links.  (E.g. all of it)
		// This should match CalculateRoutePingScorein the SDR code
		m_routeMetrics.m_nScoreCurrent += m_pingEndToEnd.m_nSmoothedPing/10;
		m_routeMetrics.m_nScoreMin += nPingMin/10;
		m_routeMetrics.m_nScoreMax += nPingMax/10;

		// And add a penalty that everybody who is not LAN uses
		m_routeMetrics.m_nTotalPenalty += k_nRoutePenaltyNotLan;
	}

	// Debug penalty
	m_routeMetrics.m_nTotalPenalty += m_connection.m_connectionConfig.P2P_Transport_ICE_Penalty.Get();

	// Check for recording the initial scoring data used to make the initial decision
	CMsgSteamNetworkingICESessionSummary &ice_summary = Connection().m_msgICESessionSummary;
	uint32 nScore = m_routeMetrics.m_nScoreCurrent + m_routeMetrics.m_nTotalPenalty;
	if (
		ConnectionState() == k_ESteamNetworkingConnectionState_FindingRoute
		|| !ice_summary.has_initial_ping()
		|| ( nScore < ice_summary.initial_score() && usecNow < Connection().m_usecWhenCreated + 15*k_nMillion )
	) {
		ice_summary.set_initial_score( nScore );
		ice_summary.set_initial_ping( m_pingEndToEnd.m_nSmoothedPing );
		ice_summary.set_initial_route_kind( m_eCurrentRouteKind );
	}

	if ( !ice_summary.has_best_score() || nScore < ice_summary.best_score() )
	{
		ice_summary.set_best_score( nScore );
		ice_summary.set_best_ping( m_pingEndToEnd.m_nSmoothedPing );
		ice_summary.set_best_route_kind( m_eCurrentRouteKind );
		ice_summary.set_best_time( ( usecNow - Connection().m_usecWhenCreated + 500*1000 ) / k_nMillion );
	}
}

#define ParseProtobufBody( pvMsg, cbMsg, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	if ( !msgVar.ParseFromArray( pvMsg, cbMsg ) ) \
	{ \
		ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Protobuf parse failed." ); \
		return; \
	}

#define ParsePaddedPacket( pvPkt, cbPkt, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	{ \
		if ( cbPkt < k_cbSteamNetworkingMinPaddedPacketSize ) \
		{ \
			ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Packet is %d bytes, must be padded to at least %d bytes.", cbPkt, k_cbSteamNetworkingMinPaddedPacketSize ); \
			return; \
		} \
		const UDPPaddedMessageHdr *hdr =  (const UDPPaddedMessageHdr *)( pvPkt ); \
		int nMsgLength = LittleWord( hdr->m_nMsgLength ); \
		if ( nMsgLength <= 0 || int(nMsgLength+sizeof(UDPPaddedMessageHdr)) > cbPkt ) \
		{ \
			ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Invalid encoded message length %d.  Packet is %d bytes.", nMsgLength, cbPkt ); \
			return; \
		} \
		if ( !msgVar.ParseFromArray( hdr+1, nMsgLength ) ) \
		{ \
			ReportBadUDPPacketFromConnectionPeer( # CMsgCls, "Protobuf parse failed." ); \
			return; \
		} \
	}

void CConnectionTransportP2PICE::ProcessPacket( const uint8_t *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow )
{
	Assert( cbPkt >= 1 ); // Caller should have checked this
	//ETW_ICEProcessPacket( m_connection.m_hConnectionSelf, cbPkt );

	// Data packet is the most common, check for it first.  Also, does stat tracking.
	if ( *pPkt & 0x80 )
	{
		Received_Data( pPkt, cbPkt, usecNow );
		return;
	}

	// Track stats for other packet types.
	m_connection.m_statsEndToEnd.TrackRecvPacket( cbPkt, usecNow );

	if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectionClosed )
	{
		ParsePaddedPacket( pPkt, cbPkt, CMsgSteamSockets_UDP_ConnectionClosed, msg )
		Received_ConnectionClosed( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_NoConnection )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_NoConnection, msg )
		Received_NoConnection( msg, usecNow );
	}
	else
	{
		ReportBadUDPPacketFromConnectionPeer( "packet", "Lead byte 0x%02x not a known message ID", *pPkt );
	}
}

void CConnectionTransportP2PICE::TrackSentStats( UDPSendPacketContext_t &ctx )
{
	CConnectionTransportUDPBase::TrackSentStats( ctx );

	// Does this count as a ping request?
	if ( ctx.msg.has_stats() || ( ctx.msg.flags() & ctx.msg.ACK_REQUEST_E2E ) )
	{
		bool bAllowDelayedReply = ( ctx.msg.flags() & ctx.msg.ACK_REQUEST_IMMEDIATE ) == 0;
		P2PTransportTrackSentEndToEndPingRequest( ctx.m_usecNow, bAllowDelayedReply );
	}
}

void CConnectionTransportP2PICE::RecvValidUDPDataPacket( UDPRecvPacketContext_t &ctx )
{
	if ( !ctx.m_pStatsIn || !( ctx.m_pStatsIn->flags() & ctx.m_pStatsIn->NOT_PRIMARY_TRANSPORT_E2E ) )
		Connection().SetPeerSelectedTransport( this );
	P2PTransportTrackRecvEndToEndPacket( ctx.m_usecNow );
	if ( m_bNeedToConfirmEndToEndConnectivity && BCanSendEndToEndData() )
		P2PTransportEndToEndConnectivityConfirmed( ctx.m_usecNow );
}

void CConnectionTransportP2PICE::LocalCandidateGathered( EICECandidateType eType, CMsgICECandidate &&msgCandidate )
{
	CSteamNetworkConnectionP2P &conn = Connection();
	CMsgSteamNetworkingICESessionSummary &sum = conn.m_msgICESessionSummary;

	// Make sure candidate type makes sense and is allowed
	Assert( ( (int)eType & ((int)eType-1) ) == 0 ); // Should be a single bit set
	AssertMsg( eType & sum.local_candidate_types_allowed(), "We gathered candidate type 0x%x, but 0x%x is allowed", eType, sum.local_candidate_types_allowed() );

	// Update bookkeeping about what types of candidates we gathered
	sum.set_local_candidate_types( sum.local_candidate_types() | eType );

	// Queue a message to inform peer
	CMsgSteamNetworkingP2PRendezvous_ReliableMessage msg;
	*msg.mutable_ice()->mutable_add_candidate() = std::move( msgCandidate );
	Connection().QueueSignalReliableMessage( std::move(msg), "LocalCandidateAdded" );
}

} // namespace SteamNetworkingSocketsLib

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
