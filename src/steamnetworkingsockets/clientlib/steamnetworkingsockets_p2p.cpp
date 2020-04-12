//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_p2p.h"
#include "csteamnetworkingsockets.h"
#include "crypto.h"
#include "csteamnetworkingmessages.h"

#include "steamnetworkingsockets_sdr_p2p.h" // FIXME Refactor

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
	// FIXME should not be in base class.
	CSteamNetworkingSocketsSDR *pSteamNetworkingSocketsSDR = assert_cast< CSteamNetworkingSocketsSDR *>( m_pSteamNetworkingSocketsInterface );
	if ( !pSteamNetworkingSocketsSDR->BSDRClientInit( errMsg ) )
		return false;

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
	m_pTransportP2PSDR = nullptr;
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

	// FIXME SDR mentioned here

	Assert( !m_pTransport );
	CConnectionTransportP2PSDR *pTransport = new CConnectionTransportP2PSDR( *this );

	// FIXME - for now only one transport supported, relay over SDR
	m_pTransportP2PSDR = pTransport;
	m_pTransport = pTransport;

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

	// Start the connection state machine, and send the first request packet.
	CheckConnectionStateAndSetNextThinkTime( usecNow );

	// Done
	return true;
}

bool CSteamNetworkConnectionP2P::BInitP2PConnectionCommon( SteamNetworkingMicroseconds usecNow, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	CSteamNetworkingSocketsSDR *pSteamNetworkingSocketsSDR = assert_cast< CSteamNetworkingSocketsSDR *>( m_pSteamNetworkingSocketsInterface );
	Assert( m_pTransportP2PSDR );

	// Make sure SDR client functionality is ready
	if ( !pSteamNetworkingSocketsSDR->BSDRClientInit( errMsg ) )
		return false;

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
		SpewWarning( "Connecting P2P socket to self (%s).  Traffic will be relayed over the Internet", SteamNetworkingIdentityRender( m_identityRemote ).c_str() );
	}

	// Add to list of SDR clients
	g_vecSDRClients.push_back( m_pTransportP2PSDR );
	return true;
}

bool CSteamNetworkConnectionP2P::BBeginAccept(
	const CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest,
	SteamDatagramErrMsg &errMsg,
	SteamNetworkingMicroseconds usecNow
) {
	m_bConnectionInitiatedRemotely = true;

	// Create transport
	Assert( !m_pTransport );
	Assert( !m_pTransportP2PSDR );
	m_pTransportP2PSDR = new CConnectionTransportP2PSDR( *this );
	m_pTransport = m_pTransportP2PSDR;

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
	if ( m_pTransportP2PSDR )
	{
		m_pTransportP2PSDR->TransportDestroySelfNow();
		m_pTransportP2PSDR = nullptr;
	}
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

void CSteamNetworkConnectionP2P::ThinkConnection( SteamNetworkingMicroseconds usecNow )
{
	CSteamNetworkConnectionBase::ThinkConnection( usecNow );

	if ( m_pTransportP2PSDR )
		m_pTransportP2PSDR->ThinkSessions( usecNow );
}

void CSteamNetworkConnectionP2P::ConnectionSendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow )
{
	Assert( m_pParentListenSocket == nullptr );
	if ( m_bConnectionInitiatedRemotely )
	{
		AssertMsg( false, "Shouldn't be sending end-to-end connect request!  Connection was initiated remotely!" );
		return;
	}

	// We always do this through steam using a rendezvous message

	CMsgSteamNetworkingP2PRendezvous msgRendezvous;
	CMsgSteamNetworkingP2PRendezvous_ConnectRequest &msgConnectRequest = *msgRendezvous.mutable_connect_request();
	//msgConnectRequest.set_connection_id( m_unConnectionIDLocal ); // No, these fields are in the rendezvous envelope
	//msgConnectRequest.set_client_steam_id( .... );
	*msgConnectRequest.mutable_cert() = m_msgSignedCertLocal;
	*msgConnectRequest.mutable_crypt() = m_msgSignedCryptLocal;
	if ( m_nRemoteVirtualPort >= 0 )
		msgConnectRequest.set_virtual_port( m_nRemoteVirtualPort );
	// NOTE: Intentionally not setting the timestamp, since ping time via signaling mechanism is not relevant for data

	SpewType( LogLevel_P2PRendezvous(), "[%s] Sending P2P ConnectRequest\n", GetDescription() );
	SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, "ConnectRequest" );
}

void CSteamNetworkConnectionP2P::SendConnectOKSignal( SteamNetworkingMicroseconds usecNow )
{
	Assert( BCryptKeysValid() );

	CMsgSteamNetworkingP2PRendezvous msgRendezvous;
	CMsgSteamNetworkingP2PRendezvous_ConnectOK &msgConnectOK = *msgRendezvous.mutable_connect_ok();
	//msgConnectOK.set_server_connection_id( m_unConnectionIDLocal ); // No, these fields are in the rendezvous envelope
	//msgConnectOK.set_client_connection_id( m_unConnectionIDRemote );
	*msgConnectOK.mutable_cert() = m_msgSignedCertLocal;
	*msgConnectOK.mutable_crypt() = m_msgSignedCryptLocal;

	// NOTE: Intentionally not setting the timestamp field or doing ping time calculations.
	//       These messages are being sent through a totally separate channel than the actual
	//       data packets will go, so this ping time is not useful for that purpose.

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
	// it is because we have not need able to rendezvous, and haven't sent any packets end-to-end anyway

	SetRendezvousCommonFieldsAndSendSignal( msgRendezvous, usecNow, "ConnectionClosed" );
}

void CSteamNetworkConnectionP2P::SendNoConnectionSignal( SteamNetworkingMicroseconds usecNow )
{
	SpewType( LogLevel_P2PRendezvous(), "[%s] Sending graceful P2P ConnectionClosed, remote cxn %u\n", GetDescription(), m_unConnectionIDRemote );

	CMsgSteamNetworkingP2PRendezvous msgRendezvous;
	CMsgSteamNetworkingP2PRendezvous_ConnectionClosed &msgConnectionClosed = *msgRendezvous.mutable_connection_closed();
	msgConnectionClosed.set_reason_code( k_ESteamNetConnectionEnd_Internal_P2PNoConnection );

	// NOTE: Not sending connection stats here.  Usually when a connection is closed through this mechanism,
	// it is because we have not need able to rendezvous, and haven't sent any packets end-to-end anyway

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

	if ( m_pTransportP2PSDR )
	{
		m_pTransportP2PSDR->PopulateRendezvousMsg( msg, usecNow );
	}

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

bool CSteamNetworkConnectionP2P::BConnectionCanSendEndToEndConnectRequest() const
{
	if ( m_bConnectionInitiatedRemotely )
	{
		AssertMsg( false, "Why are we asking this?" );
		return false;
	}
	Assert( m_pParentListenSocket == nullptr );

	if ( !m_pSignaling )
		return false;

	//// The first messages go through Steam, and we need to be logged on to do that
	//// FIXME - Should ask the signaling interface, not SteamNetworkingSocketsInterface
	//if ( !SteamNetworkingSocketsInterface()->BCanSendP2PRendezvous() )
	//	return false;

	// Check if we are doing initial ping collection, then wait.
	if ( !g_bClusterPingDataGoodEnoughForRouting )
		return false;

	return true;
}

void CSteamNetworkConnectionP2P::ProcessSignal_ConnectOK( const CMsgSteamNetworkingP2PRendezvous_ConnectOK &msgConnectOK, SteamNetworkingMicroseconds usecNow )
{

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

} // namespace SteamNetworkingSocketsLib
