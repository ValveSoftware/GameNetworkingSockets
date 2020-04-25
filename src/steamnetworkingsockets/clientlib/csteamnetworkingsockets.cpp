//====== Copyright Valve Corporation, All rights reserved. ====================

#include "csteamnetworkingsockets.h"
#include "steamnetworkingsockets_lowlevel.h"
#include "steamnetworkingsockets_connections.h"
#include "steamnetworkingsockets_udp.h"
#include "../steamnetworkingsockets_certstore.h"
#include "crypto.h"

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
#include <steam/steamnetworkingsockets.h>
#endif

#ifdef STEAMNETWORKINGSOCKETS_HAS_DEFAULT_P2P_SIGNALING
#include "csteamnetworkingmessages.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ISteamNetworkingSockets::~ISteamNetworkingSockets() {}
ISteamNetworkingUtils::~ISteamNetworkingUtils() {}

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// Configuration Variables
//
/////////////////////////////////////////////////////////////////////////////

DEFINE_GLOBAL_CONFIGVAL( float, FakePacketLoss_Send, 0.0f, 0.0f, 100.0f );
DEFINE_GLOBAL_CONFIGVAL( float, FakePacketLoss_Recv, 0.0f, 0.0f, 100.0f );
DEFINE_GLOBAL_CONFIGVAL( int32, FakePacketLag_Send, 0, 0, 5000 );
DEFINE_GLOBAL_CONFIGVAL( int32, FakePacketLag_Recv, 0, 0, 5000 );
DEFINE_GLOBAL_CONFIGVAL( float, FakePacketReorder_Send, 0.0f, 0.0f, 100.0f );
DEFINE_GLOBAL_CONFIGVAL( float, FakePacketReorder_Recv, 0.0f, 0.0f, 100.0f );
DEFINE_GLOBAL_CONFIGVAL( int32, FakePacketReorder_Time, 15, 0, 5000 );
DEFINE_GLOBAL_CONFIGVAL( float, FakePacketDup_Send, 0.0f, 0.0f, 100.0f );
DEFINE_GLOBAL_CONFIGVAL( float, FakePacketDup_Recv, 0.0f, 0.0f, 100.0f );
DEFINE_GLOBAL_CONFIGVAL( int32, FakePacketDup_TimeMax, 10, 0, 5000 );
DEFINE_GLOBAL_CONFIGVAL( int32, EnumerateDevVars, 0, 0, 1 );

DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, TimeoutInitial, 10000, 0, INT32_MAX );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, TimeoutConnected, 10000, 0, INT32_MAX );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, SendBufferSize, 512*1024, 0, 0x10000000 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, SendRateMin, 128*1024, 1024, 0x10000000 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, SendRateMax, 1024*1024, 1024, 0x10000000 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, NagleTime, 5000, 0, 20000 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, MTU_PacketSize, 1300, k_cbSteamNetworkingSocketsMinMTUPacketSize, k_cbSteamNetworkingSocketsMaxUDPMsgLen );
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
	// We don't have a trusted third party, so allow this by default,
	// and don't warn about it
	DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, IP_AllowWithoutAuth, 2, 0, 2 );
#else
	DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, IP_AllowWithoutAuth, 0, 0, 2 );
#endif
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, Unencrypted, 0, 0, 3 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_AckRTT, k_ESteamNetworkingSocketsDebugOutputType_Everything, k_ESteamNetworkingSocketsDebugOutputType_Error, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_PacketDecode, k_ESteamNetworkingSocketsDebugOutputType_Everything, k_ESteamNetworkingSocketsDebugOutputType_Error, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_Message, k_ESteamNetworkingSocketsDebugOutputType_Everything, k_ESteamNetworkingSocketsDebugOutputType_Error, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_PacketGaps, k_ESteamNetworkingSocketsDebugOutputType_Debug, k_ESteamNetworkingSocketsDebugOutputType_Error, k_ESteamNetworkingSocketsDebugOutputType_Everything );

DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_P2PRendezvous, k_ESteamNetworkingSocketsDebugOutputType_Verbose, k_ESteamNetworkingSocketsDebugOutputType_Error, k_ESteamNetworkingSocketsDebugOutputType_Everything );

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( std::string, P2P_STUN_ServerList, "" );
#endif

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( std::string, SDRClient_DebugTicketAddress, "" );
#endif

static GlobalConfigValueEntry *s_pFirstGlobalConfigEntry = nullptr;
static bool s_bConfigValueTableInitted = false;
static std::vector<GlobalConfigValueEntry *> s_vecConfigValueTable; // Sorted by value
static std::vector<GlobalConfigValueEntry *> s_vecConnectionConfigValueTable; // Sorted by offset

GlobalConfigValueEntry::GlobalConfigValueEntry(
	ESteamNetworkingConfigValue eValue,
	const char *pszName,
	ESteamNetworkingConfigDataType eDataType,
	ESteamNetworkingConfigScope eScope,
	int cbOffsetOf
) : m_eValue{ eValue }
, m_pszName{ pszName }
, m_eDataType{ eDataType }
, m_eScope{ eScope }
, m_cbOffsetOf{cbOffsetOf}
, m_pNextEntry( s_pFirstGlobalConfigEntry )
{
	s_pFirstGlobalConfigEntry = this;
	AssertMsg( !s_bConfigValueTableInitted, "Attempt to register more config values after table is already initialized" );
	s_bConfigValueTableInitted = false;
}

static void EnsureConfigValueTableInitted()
{
	if ( s_bConfigValueTableInitted )
		return;
	SteamDatagramTransportLock scopeLock;
	if ( s_bConfigValueTableInitted )
		return;

	for ( GlobalConfigValueEntry *p = s_pFirstGlobalConfigEntry ; p ; p = p->m_pNextEntry )
	{
		s_vecConfigValueTable.push_back( p );
		if ( p->m_eScope == k_ESteamNetworkingConfig_Connection )
			s_vecConnectionConfigValueTable.push_back( p );
	}

	// Sort in ascending order by value, so we can binary search
	std::sort( s_vecConfigValueTable.begin(), s_vecConfigValueTable.end(),
		[]( GlobalConfigValueEntry *a, GlobalConfigValueEntry *b ) { return a->m_eValue < b->m_eValue; } );

	// Sort by struct offset, so that ConnectionConfig::Init will access memory in a sane way.
	// This doesn't really matter, though.
	std::sort( s_vecConnectionConfigValueTable.begin(), s_vecConnectionConfigValueTable.end(),
		[]( GlobalConfigValueEntry *a, GlobalConfigValueEntry *b ) { return a->m_cbOffsetOf < b->m_cbOffsetOf; } );

	// Rebuild linked list, in order, and safety check for duplicates
	int N = len( s_vecConfigValueTable );
	for ( int i = 1 ; i < N ; ++i )
	{
		s_vecConfigValueTable[i-1]->m_pNextEntry = s_vecConfigValueTable[i];
		AssertMsg1( s_vecConfigValueTable[i-1]->m_eValue < s_vecConfigValueTable[i]->m_eValue, "Registered duplicate config value %d", s_vecConfigValueTable[i]->m_eValue );
	}
	s_vecConfigValueTable[N-1]->m_pNextEntry = nullptr;

	s_pFirstGlobalConfigEntry = nullptr;
	s_bConfigValueTableInitted = true;
}

static GlobalConfigValueEntry *FindConfigValueEntry( ESteamNetworkingConfigValue eSearchVal )
{
	EnsureConfigValueTableInitted();

	// Binary search
	int l = 0;
	int r = len( s_vecConfigValueTable )-1;
	while ( l <= r )
	{
		int m = (l+r)>>1;
		GlobalConfigValueEntry *mp = s_vecConfigValueTable[m];
		if ( eSearchVal < mp->m_eValue )
			r = m-1;
		else if ( eSearchVal > mp->m_eValue )
			l = m+1;
		else
			return mp;
	}

	// Not found
	return nullptr;
}

void ConnectionConfig::Init( ConnectionConfig *pInherit )
{
	EnsureConfigValueTableInitted();

	for ( GlobalConfigValueEntry *pEntry : s_vecConnectionConfigValueTable )
	{
		ConfigValueBase *pVal = (ConfigValueBase *)((intptr_t)this + pEntry->m_cbOffsetOf );
		if ( pInherit )
		{
			pVal->m_pInherit = (ConfigValueBase *)((intptr_t)pInherit + pEntry->m_cbOffsetOf );
		}
		else
		{
			// Assume the relevant members are the same, no matter
			// what type T, so just use int32 arbitrarily
			pVal->m_pInherit = &( static_cast< GlobalConfigValueBase<int32> * >( pEntry ) )->m_value;
		}
	}
}

/////////////////////////////////////////////////////////////////////////////
//
// Table of active sockets
//
/////////////////////////////////////////////////////////////////////////////

CUtlHashMap<uint16, CSteamNetworkConnectionBase *, std::equal_to<uint16>, Identity<uint16> > g_mapConnections;
CUtlHashMap<int, CSteamNetworkListenSocketBase *, std::equal_to<int>, Identity<int> > g_mapListenSockets;
CUtlHashMap<int, CSteamNetworkPollGroup *, std::equal_to<int>, Identity<int> > g_mapPollGroups;

static bool BConnectionStateExistsToAPI( ESteamNetworkingConnectionState eState )
{
	switch ( eState )
	{
		default:
			Assert( false );
			return false;
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

CSteamNetworkConnectionBase *GetConnectionByHandle( HSteamNetConnection sock )
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
	return pResult;
}

static CSteamNetworkConnectionBase *GetConnectionByHandleForAPI( HSteamNetConnection sock )
{
	CSteamNetworkConnectionBase *pResult = GetConnectionByHandle( sock );
	if ( !pResult )
		return nullptr;
	if ( !BConnectionStateExistsToAPI( pResult->GetState() ) )
		return nullptr;
	return pResult;
}

static CSteamNetworkListenSocketBase *GetListenSocketByHandle( HSteamListenSocket sock )
{
	if ( sock == k_HSteamListenSocket_Invalid )
		return nullptr;
	AssertMsg( !(sock & 0x80000000), "A poll group handle was used where a listen socket handle was expected" );
	int idx = sock & 0xffff;
	if ( !g_mapListenSockets.IsValidIndex( idx ) )
		return nullptr;
	CSteamNetworkListenSocketBase *pResult = g_mapListenSockets[ idx ];
	if ( pResult->m_hListenSocketSelf != sock )
	{
		// Slot was reused, but this handle is now invalid
		return nullptr;
	}
	return pResult;
}

static CSteamNetworkPollGroup *GetPollGroupByHandle( HSteamNetPollGroup hPollGroup )
{
	if ( hPollGroup == k_HSteamNetPollGroup_Invalid )
		return nullptr;
	AssertMsg( (hPollGroup & 0x80000000), "A listen socket handle was used where a poll group handle was expected" );
	int idx = hPollGroup & 0xffff;
	if ( !g_mapPollGroups.IsValidIndex( idx ) )
		return nullptr;
	CSteamNetworkPollGroup *pResult = g_mapPollGroups[ idx ];
	if ( pResult->m_hPollGroupSelf != hPollGroup )
	{
		// Slot was reused, but this handle is now invalid
		return nullptr;
	}
	return pResult;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamSocketNetworkingBase
//
/////////////////////////////////////////////////////////////////////////////

std::vector<CSteamNetworkingSockets *> CSteamNetworkingSockets::s_vecSteamNetworkingSocketsInstances;

CSteamNetworkingSockets::CSteamNetworkingSockets( CSteamNetworkingUtils *pSteamNetworkingUtils )
: m_bHaveLowLevelRef( false )
, m_pSteamNetworkingUtils( pSteamNetworkingUtils )
, m_pSteamNetworkingMessages( nullptr )
{
	m_connectionConfig.Init( nullptr );
	m_identity.Clear();
}

CSteamNetworkingSockets::~CSteamNetworkingSockets()
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();
	Assert( !m_bHaveLowLevelRef ); // Called destructor directly?  Use Destroy()!
}

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
bool CSteamNetworkingSockets::BInitGameNetworkingSockets( const SteamNetworkingIdentity *pIdentity, SteamDatagramErrMsg &errMsg )
{
	AssertMsg( !m_bHaveLowLevelRef, "Initted interface twice?" );

	// Make sure low level socket support is ready
	if ( !BInitLowLevel( errMsg ) )
		return false;

	if ( pIdentity )
		m_identity = *pIdentity;
	else
		CacheIdentity();

	return true;
}
#endif

bool CSteamNetworkingSockets::BInitLowLevel( SteamNetworkingErrMsg &errMsg )
{
	if ( m_bHaveLowLevelRef )
		return true;
	if ( !BSteamNetworkingSocketsLowLevelAddRef( errMsg) )
		return false;

	// Add us to list of extant instances only after we have done some initialization
	if ( !has_element( s_vecSteamNetworkingSocketsInstances, this ) )
		s_vecSteamNetworkingSocketsInstances.push_back( this );

	m_bHaveLowLevelRef = true;
	return true;
}

void CSteamNetworkingSockets::KillConnections()
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread( "CSteamNetworkingSockets::KillConnections" );

	// Destroy all of my connections
	FOR_EACH_HASHMAP( g_mapConnections, idx )
	{
		CSteamNetworkConnectionBase *pConn = g_mapConnections[idx];
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
		{
			pConn->ConnectionDestroySelfNow();
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

	// Destroy all of my poll groups
	FOR_EACH_HASHMAP( g_mapPollGroups, idx )
	{
		CSteamNetworkPollGroup *pPollGroup = g_mapPollGroups[idx];
		if ( pPollGroup->m_pSteamNetworkingSocketsInterface == this )
		{
			DbgVerify( DestroyPollGroup( pPollGroup->m_hPollGroupSelf ) );
			Assert( !g_mapPollGroups.IsValidIndex( idx ) );
		}
	}

}

void CSteamNetworkingSockets::Destroy()
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread( "CSteamNetworkingSockets::Destroy" );

	// Nuke messages interface, if we had one
	#ifdef STEAMNETWORKINGSOCKETS_HAS_DEFAULT_P2P_SIGNALING
		if ( m_pSteamNetworkingMessages )
		{
			delete m_pSteamNetworkingMessages;

			// That destructor should clear our pointer (so we can be destroyed in either order)
			Assert( m_pSteamNetworkingMessages == nullptr );

			// But clear it just to be safe
			m_pSteamNetworkingMessages = nullptr;
		}
	#endif

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

	// Remove from list of extant instances, if we are there
	find_and_remove_element( s_vecSteamNetworkingSocketsInstances, this );

	// Self destruct
	delete this;
}

bool CSteamNetworkingSockets::BHasAnyConnections() const
{
	for ( CSteamNetworkConnectionBase *pConn: g_mapConnections.IterValues() )
	{
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
			return true;
	}
	return false;
}

bool CSteamNetworkingSockets::BHasAnyListenSockets() const
{
	for ( CSteamNetworkListenSocketBase *pSock: g_mapListenSockets.IterValues() )
	{
		if ( pSock->m_pSteamNetworkingSocketsInterface == this )
			return true;
	}
	return false;
}

bool CSteamNetworkingSockets::GetIdentity( SteamNetworkingIdentity *pIdentity )
{
	SteamDatagramTransportLock scopeLock( "GetIdentity" );
	InternalGetIdentity();
	if ( pIdentity )
		*pIdentity = m_identity;
	return !m_identity.IsInvalid();
}

bool CSteamNetworkingSockets::GetCertificateRequest( int *pcbBlob, void *pBlob, SteamNetworkingErrMsg &errMsg )
{
	SteamDatagramTransportLock scopeLock( "GetCertificateRequest" );

	// If we don't have a private key, generate one now.
	CECSigningPublicKey pubKey;
	if ( m_keyPrivateKey.IsValid() )
	{
		DbgVerify( m_keyPrivateKey.GetPublicKey( &pubKey ) );
	}
	else
	{
		CCrypto::GenerateSigningKeyPair( &pubKey, &m_keyPrivateKey );
	}

	// Fill out the request
	CMsgSteamDatagramCertificateRequest msgRequest;
	CMsgSteamDatagramCertificate &msgCert =*msgRequest.mutable_cert();

	// Our public key
	msgCert.set_key_type( CMsgSteamDatagramCertificate_EKeyType_ED25519 );
	DbgVerify( pubKey.GetRawDataAsStdString( msgCert.mutable_key_data() ) );

	// Our identity, if we know it
	InternalGetIdentity();
	if ( !m_identity.IsInvalid() && !m_identity.IsLocalHost() )
	{
		SteamNetworkingIdentityToProtobuf( m_identity, msgCert, identity_string, legacy_identity_binary, legacy_steam_id );
	}

	// Check size
	int cb = ProtoMsgByteSize( msgRequest );
	if ( !pBlob )
	{
		*pcbBlob = cb;
		return true;
	}
	if ( cb > *pcbBlob )
	{
		*pcbBlob = cb;
		V_sprintf_safe( errMsg, "%d byte buffer not big enough; %d bytes required", *pcbBlob, cb );
		return false;
	}

	*pcbBlob = cb;
	uint8 *p = (uint8 *)pBlob;
	DbgVerify( msgRequest.SerializeWithCachedSizesToArray( p ) == p + cb );
	return true;
}

bool CSteamNetworkingSockets::SetCertificate( const void *pCertificate, int cbCertificate, SteamNetworkingErrMsg &errMsg )
{
	// Crack the blob
	CMsgSteamDatagramCertificateSigned msgCertSigned;
	if ( !msgCertSigned.ParseFromArray( pCertificate, cbCertificate ) )
	{
		V_strcpy_safe( errMsg, "CMsgSteamDatagramCertificateSigned failed protobuf parse" );
		return false;
	}

	SteamDatagramTransportLock scopeLock( "SetCertificate" );

	// Crack the cert, and check the signature.  If *we* aren't even willing
	// to trust it, assume that our peers won't either
	CMsgSteamDatagramCertificate msgCert;
	time_t authTime = m_pSteamNetworkingUtils->GetTimeSecure();
	const CertAuthScope *pAuthScope = CertStore_CheckCert( msgCertSigned, msgCert, authTime, errMsg );
	if ( !pAuthScope )
	{
		SpewWarning( "SetCertificate: We are not currently able to verify our own cert!  %s.  Continuing anyway!", errMsg );
	}

	// Extract the identity from the cert
	SteamNetworkingErrMsg tempErrMsg;
	SteamNetworkingIdentity certIdentity;
	int r = SteamNetworkingIdentityFromCert( certIdentity, msgCert, tempErrMsg );
	if ( r < 0 )
	{
		V_sprintf_safe( errMsg, "Cert has invalid identity.  %s", tempErrMsg );
		return false;
	}

	// We currently only support one key type
	if ( msgCert.key_type() != CMsgSteamDatagramCertificate_EKeyType_ED25519 || msgCert.key_data().size() != 32 )
	{
		V_strcpy_safe( errMsg, "Cert has invalid public key" );
		return false;
	}

	// Does cert contain a private key?
	if ( msgCertSigned.has_private_key_data() )
	{
		// The degree to which the key is actually "private" is not
		// really known to us.  However there are some use cases where
		// we will accept a cert 
		const std::string &private_key_data = msgCertSigned.private_key_data();
		if ( m_keyPrivateKey.IsValid() )
		{

			// We already chose a private key, so the cert must match.
			// For the most common use cases, we choose a private
			// key and it never leaves the current process.
			if ( m_keyPrivateKey.GetRawDataSize() != private_key_data.length()
				|| memcmp( m_keyPrivateKey.GetRawDataPtr(), private_key_data.c_str(), private_key_data.length() ) != 0 )
			{
				V_strcpy_safe( errMsg, "Private key mismatch" );
				return false;
			}
		}
		else
		{
			// We haven't chosen a private key yet, so we'll accept this one.
			if ( !m_keyPrivateKey.SetRawDataFromStdString( private_key_data ) )
			{
				V_strcpy_safe( errMsg, "Invalid private key" );
				return false;
			}
		}
	}
	else if ( !m_keyPrivateKey.IsValid() )
	{
		// WAT
		V_strcpy_safe( errMsg, "Cannot set cert.  No private key?" );
		return false;
	}

	// Make sure the cert actually matches our public key.
	if ( memcmp( msgCert.key_data().c_str(), m_keyPrivateKey.GetPublicKeyRawData(), 32 ) != 0 )
	{
		V_strcpy_safe( errMsg, "Cert public key does not match our private key" );
		return false;
	}

	// Make sure the cert authorizes us for the App we think we are running
	AppId_t nAppID = m_pSteamNetworkingUtils->GetAppID();
	if ( !CheckCertAppID( msgCert, pAuthScope, nAppID, tempErrMsg ) )
	{
		V_sprintf_safe( errMsg, "Cert does not authorize us for App %u", nAppID );
		return false;
	}

	// If we don't know our identity, then set it now.  Otherwise,
	// it better match.
	if ( m_identity.IsInvalid() || m_identity.IsLocalHost() )
	{
		m_identity = certIdentity;
		SpewMsg( "Local identity established from certificate.  We are '%s'\n", SteamNetworkingIdentityRender( m_identity ).c_str() );
	}
	else if ( !( m_identity == certIdentity ) )
	{
		V_sprintf_safe( errMsg, "Cert is for identity '%s'.  We are '%s'", SteamNetworkingIdentityRender( certIdentity ).c_str(), SteamNetworkingIdentityRender( m_identity ).c_str() );
		return false;
	}

	// Save it off
	m_msgSignedCert = std::move( msgCertSigned );
	m_msgCert = std::move( msgCert );

	// Make sure we have everything else we need to do authentication
	InitAuthentication();

	// OK
	return true;
}

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
ESteamNetworkingAvailability CSteamNetworkingSockets::InitAuthentication()
{
	return k_ESteamNetworkingAvailability_CannotTry;
}

ESteamNetworkingAvailability CSteamNetworkingSockets::GetAuthenticationStatus( SteamNetAuthenticationStatus_t *pDetails )
{

	// We don't really have any mechanism right now for you to do your own PKI.
	// Do you want this feature?  Let us know on github!
	if ( pDetails )
	{
		pDetails->m_eAvail = k_ESteamNetworkingAvailability_CannotTry;
		V_strcpy_safe( pDetails->m_debugMsg, "No certificate authority" );
	}
	return k_ESteamNetworkingAvailability_CannotTry;
}
#endif

HSteamListenSocket CSteamNetworkingSockets::CreateListenSocketIP( const SteamNetworkingIPAddr &localAddr, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{
	SteamDatagramTransportLock scopeLock( "CreateListenSocketIP" );
	SteamDatagramErrMsg errMsg;

	CSteamNetworkListenSocketDirectUDP *pSock = new CSteamNetworkListenSocketDirectUDP( this );
	if ( !pSock )
		return k_HSteamListenSocket_Invalid;
	if ( !pSock->BInit( localAddr, nOptions, pOptions, errMsg ) )
	{
		SpewError( "Cannot create listen socket.  %s", errMsg );
		pSock->Destroy();
		return k_HSteamListenSocket_Invalid;
	}

	return pSock->m_hListenSocketSelf;
}

HSteamNetConnection CSteamNetworkingSockets::ConnectByIPAddress( const SteamNetworkingIPAddr &address, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{
	SteamDatagramTransportLock scopeLock( "ConnectByIPAddress" );
	CSteamNetworkConnectionUDP *pConn = new CSteamNetworkConnectionUDP( this );
	if ( !pConn )
		return k_HSteamNetConnection_Invalid;
	SteamDatagramErrMsg errMsg;
	if ( !pConn->BInitConnect( address, nOptions, pOptions, errMsg ) )
	{
		SpewError( "Cannot create IPv4 connection.  %s", errMsg );
		pConn->ConnectionDestroySelfNow();
		return k_HSteamNetConnection_Invalid;
	}

	return pConn->m_hConnectionSelf;
}


EResult CSteamNetworkingSockets::AcceptConnection( HSteamNetConnection hConn )
{
	SteamDatagramTransportLock scopeLock( "AcceptConnection" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn );
	if ( !pConn )
	{
		SpewError( "Cannot accept connection #%u; invalid connection handle", hConn );
		return k_EResultInvalidParam;
	}

	// Accept it
	return pConn->APIAcceptConnection();
}

bool CSteamNetworkingSockets::CloseConnection( HSteamNetConnection hConn, int nReason, const char *pszDebug, bool bEnableLinger )
{
	SteamDatagramTransportLock scopeLock( "CloseConnection" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn );
	if ( !pConn )
		return false;

	// Close it
	pConn->APICloseConnection( nReason, pszDebug, bEnableLinger );
	return true;
}

bool CSteamNetworkingSockets::CloseListenSocket( HSteamListenSocket hSocket )
{
	SteamDatagramTransportLock scopeLock( "CloseListenSocket" );
	CSteamNetworkListenSocketBase *pSock = GetListenSocketByHandle( hSocket );
	if ( !pSock )
		return false;

	// Delete the socket itself
	// NOTE: If you change this, look at CSteamSocketNetworking::Kill()!
	pSock->Destroy();
	return true;
}

bool CSteamNetworkingSockets::SetConnectionUserData( HSteamNetConnection hPeer, int64 nUserData )
{
	SteamDatagramTransportLock scopeLock( "SetConnectionUserData" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hPeer );
	if ( !pConn )
		return false;
	pConn->SetUserData( nUserData );
	return true;
}

int64 CSteamNetworkingSockets::GetConnectionUserData( HSteamNetConnection hPeer )
{
	SteamDatagramTransportLock scopeLock( "GetConnectionUserData" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hPeer );
	if ( !pConn )
		return -1;
	return pConn->GetUserData();
}

void CSteamNetworkingSockets::SetConnectionName( HSteamNetConnection hPeer, const char *pszName )
{
	SteamDatagramTransportLock scopeLock( "SetConnectionName" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hPeer );
	if ( !pConn )
		return;
	pConn->SetAppName( pszName );
}

bool CSteamNetworkingSockets::GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen )
{
	SteamDatagramTransportLock scopeLock( "GetConnectionName" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hPeer );
	if ( !pConn )
		return false;
	V_strncpy( pszName, pConn->GetAppName(), nMaxLen );
	return true;
}

EResult CSteamNetworkingSockets::SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber )
{
	SteamDatagramTransportLock scopeLock( "SendMessageToConnection" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn );
	if ( !pConn )
		return k_EResultInvalidParam;
	return pConn->APISendMessageToConnection( pData, cbData, nSendFlags, pOutMessageNumber );
}

void CSteamNetworkingSockets::SendMessages( int nMessages, SteamNetworkingMessage_t *const *pMessages, int64 *pOutMessageNumberOrResult )
{
	SteamDatagramTransportLock scopeLock( "SendMessages" );
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	vstd::small_vector<CSteamNetworkConnectionBase *,64 > vecConnectionsToCheck;

	for ( int i = 0 ; i < nMessages ; ++i )
	{

		// Sanity check that message is valid
		CSteamNetworkingMessage *pMsg = static_cast<CSteamNetworkingMessage*>( pMessages[i] );
		if ( !pMsg )
		{
			if ( pOutMessageNumberOrResult )
				pOutMessageNumberOrResult[i] = -k_EResultInvalidParam;
			continue;
		}

		// Locate connection
		CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( pMsg->m_conn );
		if ( !pConn )
		{
			if ( pOutMessageNumberOrResult )
				pOutMessageNumberOrResult[i] = -k_EResultInvalidParam;
			pMsg->Release();
			continue;
		}

		// Attempt to send
		bool bThinkImmediately = false;
		int64 result = pConn->APISendMessageToConnection( pMsg, usecNow, &bThinkImmediately );

		// Return result for this message if they asked for it
		if ( pOutMessageNumberOrResult )
			pOutMessageNumberOrResult[i] = result;

		if ( bThinkImmediately && !has_element( vecConnectionsToCheck, pConn ) )
			vecConnectionsToCheck.push_back( pConn );
	}

	// Now if any connections indicated that we should do the sending work immediately,
	// give them a chance to send immediately
	for ( CSteamNetworkConnectionBase *pConn: vecConnectionsToCheck )
		pConn->CheckConnectionStateAndSetNextThinkTime( usecNow );
}

EResult CSteamNetworkingSockets::FlushMessagesOnConnection( HSteamNetConnection hConn )
{
	SteamDatagramTransportLock scopeLock( "FlushMessagesOnConnection" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn );
	if ( !pConn )
		return k_EResultInvalidParam;
	return pConn->APIFlushMessageOnConnection();
}

int CSteamNetworkingSockets::ReceiveMessagesOnConnection( HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	SteamDatagramTransportLock scopeLock( "ReceiveMessagesOnConnection" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn );
	if ( !pConn )
		return -1;
	return pConn->APIReceiveMessages( ppOutMessages, nMaxMessages );
}

HSteamNetPollGroup CSteamNetworkingSockets::CreatePollGroup()
{
	SteamDatagramTransportLock scopeLock( "CreatePollGroup" );
	CSteamNetworkPollGroup *pPollGroup = new CSteamNetworkPollGroup( this );
	pPollGroup->AssignHandleAndAddToGlobalTable();
	return pPollGroup->m_hPollGroupSelf;
}

bool CSteamNetworkingSockets::DestroyPollGroup( HSteamNetPollGroup hPollGroup )
{
	SteamDatagramTransportLock scopeLock( "DestroyPollGroup" );
	CSteamNetworkPollGroup *pPollGroup = GetPollGroupByHandle( hPollGroup );
	if ( !pPollGroup )
		return false;
	delete pPollGroup;
	return true;
}

bool CSteamNetworkingSockets::SetConnectionPollGroup( HSteamNetConnection hConn, HSteamNetPollGroup hPollGroup )
{
	SteamDatagramTransportLock scopeLock( "SetConnectionPollGroup" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn );
	if ( !pConn )
		return false;

	// Special case for removing the poll group
	if ( hPollGroup == k_HSteamNetPollGroup_Invalid )
	{
		pConn->RemoveFromPollGroup();
		return true;
	}


	CSteamNetworkPollGroup *pPollGroup = GetPollGroupByHandle( hPollGroup );
	if ( !pPollGroup )
		return false;

	pConn->SetPollGroup( pPollGroup );

	return true;
}

int CSteamNetworkingSockets::ReceiveMessagesOnPollGroup( HSteamNetPollGroup hPollGroup, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	SteamDatagramTransportLock scopeLock( "ReceiveMessagesOnPollGroup" );
	CSteamNetworkPollGroup *pPollGroup = GetPollGroupByHandle( hPollGroup );
	if ( !pPollGroup )
		return -1;
	return pPollGroup->m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
}

#ifdef STEAMNETWORKINGSOCKETS_STEAMCLIENT
int CSteamNetworkingSockets::ReceiveMessagesOnListenSocketLegacyPollGroup( HSteamListenSocket hSocket, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	SteamDatagramTransportLock scopeLock( "ReceiveMessagesOnListenSocket" );
	CSteamNetworkListenSocketBase *pSock = GetListenSocketByHandle( hSocket );
	if ( !pSock )
		return -1;
	return pSock->m_legacyPollGroup.m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
}
#endif

bool CSteamNetworkingSockets::GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo )
{
	SteamDatagramTransportLock scopeLock( "GetConnectionInfo" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn );
	if ( !pConn )
		return false;
	if ( pInfo )
		pConn->ConnectionPopulateInfo( *pInfo );
	return true;
}

bool CSteamNetworkingSockets::GetQuickConnectionStatus( HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats )
{
	SteamDatagramTransportLock scopeLock( "GetQuickConnectionStatus" );
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn );
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
		SteamDatagramTransportLock scopeLock( "GetDetailedConnectionStatus" );
		CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn );
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
	SteamDatagramTransportLock scopeLock( "GetListenSocketAddress" );
	CSteamNetworkListenSocketBase *pSock = GetListenSocketByHandle( hSocket );
	if ( !pSock )
		return false;
	return pSock->APIGetAddress( pAddress );
}

bool CSteamNetworkingSockets::CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 )
{
	SteamDatagramTransportLock scopeLock( "CreateSocketPair" );

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
	return m_msgCert.has_identity_string() || m_msgCert.has_legacy_identity_binary() || m_msgCert.has_legacy_steam_id();
}


bool CSteamNetworkingSockets::SetCertificateAndPrivateKey( const void *pCert, int cbCert, void *pPrivateKey, int cbPrivateKey, SteamDatagramErrMsg &errMsg )
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
	if ( !ParseCertFromPEM( pCert, cbCert, m_msgSignedCert, errMsg ) )
	{
		return false;
	}

	if (
		!m_msgSignedCert.has_cert()
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
		#ifdef STEAMNETWORKINGSOCKETS_STEAM
			case P2PSessionRequest_t::k_iCallback:
				COMPILE_TIME_ASSERT( sizeof(P2PSessionRequest_t) <= sizeof(x.data) );
				pCallbacks->OnP2PSessionRequest( (P2PSessionRequest_t*)x.data );
				break;
			case P2PSessionConnectFail_t::k_iCallback:
				COMPILE_TIME_ASSERT( sizeof(P2PSessionConnectFail_t) <= sizeof(x.data) );
				pCallbacks->OnP2PSessionConnectFail( (P2PSessionConnectFail_t*)x.data );
				break;
		#endif
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
			case SteamNetAuthenticationStatus_t::k_iCallback:
				COMPILE_TIME_ASSERT( sizeof(SteamNetAuthenticationStatus_t) <= sizeof(x.data) );
				pCallbacks->OnAuthenticationStatusChanged( (SteamNetAuthenticationStatus_t *)x.data );
				break;
			case SteamRelayNetworkStatus_t::k_iCallback:
				COMPILE_TIME_ASSERT( sizeof(SteamRelayNetworkStatus_t) <= sizeof(x.data) );
				pCallbacks->OnRelayNetworkStatusChanged( (SteamRelayNetworkStatus_t*)x.data );
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

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingUtils
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkingUtils::~CSteamNetworkingUtils() {}

SteamNetworkingMessage_t *CSteamNetworkingUtils::AllocateMessage( int cbAllocateBuffer )
{
	return CSteamNetworkingMessage::New( cbAllocateBuffer );
}

SteamNetworkingMicroseconds CSteamNetworkingUtils::GetLocalTimestamp()
{
	return SteamNetworkingSockets_GetLocalTimestamp();
}

void CSteamNetworkingUtils::SetDebugOutputFunction( ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc )
{
	SteamNetworkingSockets_SetDebugOutputFunction( eDetailLevel, pfnFunc );
}


template<typename T>
static ConfigValue<T> *GetConnectionVar( const GlobalConfigValueEntry *pEntry, ConnectionConfig *pConnectionConfig )
{
	Assert( pEntry->m_eScope == k_ESteamNetworkingConfig_Connection );
	intptr_t ptr = intptr_t( pConnectionConfig );
	return (ConfigValue<T> *)( ptr + pEntry->m_cbOffsetOf );
}

template<typename T>
static ConfigValue<T> *EvaluateScopeConfigValue( GlobalConfigValueEntry *pEntry,
	ESteamNetworkingConfigScope eScopeType,
	intptr_t scopeObj )
{
	switch ( eScopeType )
	{
		case k_ESteamNetworkingConfig_Global:
		{
			auto *pGlobalVal = static_cast< GlobalConfigValueBase<T> * >( pEntry );
			return &pGlobalVal->m_value;
		}

		case k_ESteamNetworkingConfig_SocketsInterface:
		{
			CSteamNetworkingSockets *pInterface = (CSteamNetworkingSockets *)scopeObj;
			if ( pEntry->m_eScope == k_ESteamNetworkingConfig_Connection )
			{
				return GetConnectionVar<T>( pEntry, &pInterface->m_connectionConfig );
			}
			break;
		}

		case k_ESteamNetworkingConfig_ListenSocket:
		{
			CSteamNetworkListenSocketBase *pSock = GetListenSocketByHandle( HSteamListenSocket( scopeObj ) );
			if ( pSock )
			{
				if ( pEntry->m_eScope == k_ESteamNetworkingConfig_Connection )
				{
					return GetConnectionVar<T>( pEntry, &pSock->m_connectionConfig );
				}
			}
			break;
		}

		case k_ESteamNetworkingConfig_Connection:
		{
			// NOTE: Not using GetConnectionByHandleForAPI here.  In a few places in the code,
			// we need to be able to set config options for connections that are being created.
			// Really, we ought to plumb through these calls to an internal interface, so that
			// we would know that they should be given access.  Right now they are coming in
			// the "front door".  So this means if the app tries to set a config option on a
			// connection that technically no longer exists, we will actually allow that, when
			// we probably should fail the call.
			CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( HSteamNetConnection( scopeObj ) );
			if ( pConn )
			{
				if ( pEntry->m_eScope == k_ESteamNetworkingConfig_Connection )
				{
					return GetConnectionVar<T>( pEntry, &pConn->m_connectionConfig );
				}
			}
			break;
		}

	}

	// Bad scope argument
	return nullptr;
}

static bool AssignConfigValueTyped( ConfigValue<int32> *pVal, ESteamNetworkingConfigDataType eDataType, const void *pArg )
{
	switch ( eDataType )
	{
		case k_ESteamNetworkingConfig_Int32:
			pVal->m_data = *(int32*)pArg;
			break;

		case k_ESteamNetworkingConfig_Int64:
		{
			int64 arg = *(int64*)pArg;
			if ( (int32)arg != arg )
				return false; // Cannot truncate!
			pVal->m_data = *(int32*)arg;
			break;
		}

		case k_ESteamNetworkingConfig_Float:
			pVal->m_data = (int32)floor( *(float*)pArg + .5f );
			break;

		case k_ESteamNetworkingConfig_String:
		{
			int x;
			if ( sscanf( (const char *)pArg, "%d", &x ) != 1 )
				return false;
			pVal->m_data = x;
			break;
		}

		default:
			return false;
	}

	pVal->m_bValueSet = true;
	return true;
}

static bool AssignConfigValueTyped( ConfigValue<int64> *pVal, ESteamNetworkingConfigDataType eDataType, const void *pArg )
{
	switch ( eDataType )
	{
		case k_ESteamNetworkingConfig_Int32:
			pVal->m_data = *(int32*)pArg;
			break;

		case k_ESteamNetworkingConfig_Int64:
		{
			pVal->m_data = *(int64*)pArg;
			break;
		}

		case k_ESteamNetworkingConfig_Float:
			pVal->m_data = (int64)floor( *(float*)pArg + .5f );
			break;

		case k_ESteamNetworkingConfig_String:
		{
			long long x;
			if ( sscanf( (const char *)pArg, "%lld", &x ) != 1 )
				return false;
			pVal->m_data = (int64)x;
			break;
		}

		default:
			return false;
	}

	pVal->m_bValueSet = true;
	return true;
}

static bool AssignConfigValueTyped( ConfigValue<float> *pVal, ESteamNetworkingConfigDataType eDataType, const void *pArg )
{
	switch ( eDataType )
	{
		case k_ESteamNetworkingConfig_Int32:
			pVal->m_data = (float)( *(int32*)pArg );
			break;

		case k_ESteamNetworkingConfig_Int64:
		{
			pVal->m_data = (float)( *(int64*)pArg );
			break;
		}

		case k_ESteamNetworkingConfig_Float:
			pVal->m_data = *(float*)pArg;
			break;

		case k_ESteamNetworkingConfig_String:
		{
			float x;
			if ( sscanf( (const char *)pArg, "%f", &x ) != 1 )
				return false;
			pVal->m_data = x;
			break;
		}

		default:
			return false;
	}

	pVal->m_bValueSet = true;
	return true;
}

static bool AssignConfigValueTyped( ConfigValue<std::string> *pVal, ESteamNetworkingConfigDataType eDataType, const void *pArg )
{
	char temp[64];

	switch ( eDataType )
	{
		case k_ESteamNetworkingConfig_Int32:
			V_sprintf_safe( temp, "%d", *(int32*)pArg );
			pVal->m_data = temp;
			break;

		case k_ESteamNetworkingConfig_Int64:
			V_sprintf_safe( temp, "%lld", (long long)*(int64*)pArg );
			pVal->m_data = temp;
			break;

		case k_ESteamNetworkingConfig_Float:
			V_sprintf_safe( temp, "%g", *(float*)pArg );
			pVal->m_data = temp;
			break;

		case k_ESteamNetworkingConfig_String:
			pVal->m_data = (const char *)pArg;
			break;

		default:
			return false;
	}

	pVal->m_bValueSet = true;
	return true;
}

static bool AssignConfigValueTyped( ConfigValue<void *> *pVal, ESteamNetworkingConfigDataType eDataType, const void *pArg )
{
	switch ( eDataType )
	{
		case k_ESteamNetworkingConfig_FunctionPtr:
			pVal->m_data = (void **)pArg;
			break;

		default:
			return false;
	}

	pVal->m_bValueSet = true;
	return true;
}

template<typename T>
bool SetConfigValueTyped(
	GlobalConfigValueEntry *pEntry,
	ESteamNetworkingConfigScope eScopeType,
	intptr_t scopeObj,
	ESteamNetworkingConfigDataType eDataType,
	const void *pArg
) {
	ConfigValue<T> *pVal = EvaluateScopeConfigValue<T>( pEntry, eScopeType, scopeObj );
	if ( !pVal )
		return false;

	// Clearing the value?
	if ( pArg == nullptr )
	{
		if ( eScopeType == k_ESteamNetworkingConfig_Global )
		{
			auto *pGlobal = (typename GlobalConfigValueBase<T>::Value *)( pVal );
			Assert( pGlobal->m_pInherit == nullptr );
			Assert( pGlobal->m_bValueSet );
			pGlobal->m_data = pGlobal->m_defaultValue;
		}
		else
		{
			Assert( pVal->m_pInherit );
			pVal->m_bValueSet = false;
		}
		return true;
	}

	// Call type-specific method to set it
	if ( !AssignConfigValueTyped( pVal, eDataType, pArg ) )
		return false;

	// Apply limits
	pEntry->Clamp<T>( pVal->m_data );

	// OK
	return true;
}

template<typename T>
ESteamNetworkingGetConfigValueResult ReturnConfigValueTyped( const T &data, void *pData, size_t *cbData )
{
	ESteamNetworkingGetConfigValueResult eResult;
	if ( !pData || *cbData < sizeof(T) )
	{
		eResult = k_ESteamNetworkingGetConfigValue_BufferTooSmall;
	}
	else
	{
		*(T*)pData = data;
		eResult = k_ESteamNetworkingGetConfigValue_OK;
	}
	*cbData = sizeof(T);
	return eResult;
}

template<>
ESteamNetworkingGetConfigValueResult ReturnConfigValueTyped<std::string>( const std::string &data, void *pData, size_t *cbData )
{
	size_t l = data.length() + 1;
	ESteamNetworkingGetConfigValueResult eResult;
	if ( !pData || *cbData < l )
	{
		eResult = k_ESteamNetworkingGetConfigValue_BufferTooSmall;
	}
	else
	{
		memcpy( pData, data.c_str(), l );
		eResult = k_ESteamNetworkingGetConfigValue_OK;
	}
	*cbData = l;
	return eResult;
}

template<typename T>
ESteamNetworkingGetConfigValueResult GetConfigValueTyped(
	GlobalConfigValueEntry *pEntry,
	ESteamNetworkingConfigScope eScopeType,
	intptr_t scopeObj,
	void *pResult, size_t *cbResult
) {
	ConfigValue<T> *pVal = EvaluateScopeConfigValue<T>( pEntry, eScopeType, scopeObj );
	if ( !pVal )
	{
		*cbResult = 0;
		return k_ESteamNetworkingGetConfigValue_BadScopeObj;
	}

	// Remember if it was set at this level
	bool bValWasSet = pVal->m_bValueSet;

	// Find the place where the actual value comes from
	while ( !pVal->m_bValueSet )
	{
		Assert( pVal->m_pInherit );
		pVal = static_cast<ConfigValue<T> *>( pVal->m_pInherit );
	}

	// Call type-specific method to return it
	ESteamNetworkingGetConfigValueResult eResult = ReturnConfigValueTyped( pVal->m_data, pResult, cbResult );
	if ( eResult == k_ESteamNetworkingGetConfigValue_OK && !bValWasSet )
		eResult = k_ESteamNetworkingGetConfigValue_OKInherited;
	return eResult;
}

bool CSteamNetworkingUtils::SetConfigValue( ESteamNetworkingConfigValue eValue,
	ESteamNetworkingConfigScope eScopeType, intptr_t scopeObj,
	ESteamNetworkingConfigDataType eDataType, const void *pValue )
{
	GlobalConfigValueEntry *pEntry = FindConfigValueEntry( eValue );
	if ( pEntry == nullptr )
		return false;

	if ( eValue == k_ESteamNetworkingConfig_MTU_DataSize )
	{
		SpewWarning( "MTU_DataSize is readonly" );
		return false;
	}

	SteamDatagramTransportLock scopeLock( "SetConfigValue" );

	switch ( pEntry->m_eDataType )
	{
		case k_ESteamNetworkingConfig_Int32: return SetConfigValueTyped<int32>( pEntry, eScopeType, scopeObj, eDataType, pValue );
		case k_ESteamNetworkingConfig_Int64: return SetConfigValueTyped<int64>( pEntry, eScopeType, scopeObj, eDataType, pValue );
		case k_ESteamNetworkingConfig_Float: return SetConfigValueTyped<float>( pEntry, eScopeType, scopeObj, eDataType, pValue );
		case k_ESteamNetworkingConfig_String: return SetConfigValueTyped<std::string>( pEntry, eScopeType, scopeObj, eDataType, pValue );
		case k_ESteamNetworkingConfig_FunctionPtr: return SetConfigValueTyped<void *>( pEntry, eScopeType, scopeObj, eDataType, pValue );
	}

	Assert( false );
	return false;
}

ESteamNetworkingGetConfigValueResult CSteamNetworkingUtils::GetConfigValue(
	ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType,
	intptr_t scopeObj, ESteamNetworkingConfigDataType *pOutDataType,
	void *pResult, size_t *cbResult )
{

	if ( eValue == k_ESteamNetworkingConfig_MTU_DataSize )
	{
		int32 MTU_packetsize;
		size_t cbMTU_packetsize = sizeof(MTU_packetsize);
		ESteamNetworkingGetConfigValueResult rFetch = GetConfigValueTyped<int32>( &g_ConfigDefault_MTU_PacketSize, eScopeType, scopeObj, &MTU_packetsize, &cbMTU_packetsize );
		if ( rFetch < 0 )
			return rFetch;

		int32 MTU_DataSize = std::max( 0, MTU_packetsize - k_cbSteamNetworkingSocketsNoFragmentHeaderReserve );
		ESteamNetworkingGetConfigValueResult rStore = ReturnConfigValueTyped<int32>( MTU_DataSize, pResult, cbResult );
		if ( rStore != k_ESteamNetworkingGetConfigValue_OK )
			return rStore;
		return rFetch;
	}

	GlobalConfigValueEntry *pEntry = FindConfigValueEntry( eValue );
	if ( pEntry == nullptr )
		return k_ESteamNetworkingGetConfigValue_BadValue;

	if ( pOutDataType )
		*pOutDataType = pEntry->m_eDataType;

	SteamDatagramTransportLock scopeLock( "GetConfigValue" );

	switch ( pEntry->m_eDataType )
	{
		case k_ESteamNetworkingConfig_Int32: return GetConfigValueTyped<int32>( pEntry, eScopeType, scopeObj, pResult, cbResult );
		case k_ESteamNetworkingConfig_Int64: return GetConfigValueTyped<int64>( pEntry, eScopeType, scopeObj, pResult, cbResult );
		case k_ESteamNetworkingConfig_Float: return GetConfigValueTyped<float>( pEntry, eScopeType, scopeObj, pResult, cbResult );
		case k_ESteamNetworkingConfig_String: return GetConfigValueTyped<std::string>( pEntry, eScopeType, scopeObj, pResult, cbResult );
		case k_ESteamNetworkingConfig_FunctionPtr: return GetConfigValueTyped<void *>( pEntry, eScopeType, scopeObj, pResult, cbResult );
	}

	Assert( false ); // FIXME
	return k_ESteamNetworkingGetConfigValue_BadValue;
}

bool IsDevConfigValue( ESteamNetworkingConfigValue eVal )
{
	switch  ( eVal )
	{
		case k_ESteamNetworkingConfig_IP_AllowWithoutAuth:
		case k_ESteamNetworkingConfig_Unencrypted:
		case k_ESteamNetworkingConfig_EnumerateDevVars:
		case k_ESteamNetworkingConfig_SDRClient_FakeClusterPing:
			return true;
	}

	return false;
}

bool CSteamNetworkingUtils::GetConfigValueInfo( ESteamNetworkingConfigValue eValue,
	const char **pOutName, ESteamNetworkingConfigDataType *pOutDataType,
	ESteamNetworkingConfigScope *pOutScope, ESteamNetworkingConfigValue *pOutNextValue )
{
	const GlobalConfigValueEntry *pVal = FindConfigValueEntry( eValue );
	if ( pVal == nullptr )
		return false;

	if ( pOutName )
		*pOutName = pVal->m_pszName;
	if ( pOutDataType )
		*pOutDataType = pVal->m_eDataType;
	if ( pOutScope )
		*pOutScope = pVal->m_eScope;

	if ( pOutNextValue )
	{
		const GlobalConfigValueEntry *pNext = pVal;
		for (;;)
		{
			pNext = pNext->m_pNextEntry;
			if ( !pNext )
			{
				*pOutNextValue = k_ESteamNetworkingConfig_Invalid;
				break;
			}
			if ( g_Config_EnumerateDevVars.Get() || !IsDevConfigValue( pNext->m_eValue ) )
			{
				*pOutNextValue = pNext->m_eValue;
				break;
			}
		};
	}

	return true;
}

ESteamNetworkingConfigValue CSteamNetworkingUtils::GetFirstConfigValue()
{
	EnsureConfigValueTableInitted();
	Assert( !IsDevConfigValue( s_vecConfigValueTable[0]->m_eValue ) );
	return s_vecConfigValueTable[0]->m_eValue;
}


void CSteamNetworkingUtils::SteamNetworkingIPAddr_ToString( const SteamNetworkingIPAddr &addr, char *buf, size_t cbBuf, bool bWithPort )
{
	::SteamNetworkingIPAddr_ToString( &addr, buf, cbBuf, bWithPort );
}

bool CSteamNetworkingUtils::SteamNetworkingIPAddr_ParseString( SteamNetworkingIPAddr *pAddr, const char *pszStr )
{
	return ::SteamNetworkingIPAddr_ParseString( pAddr, pszStr );
}

void CSteamNetworkingUtils::SteamNetworkingIdentity_ToString( const SteamNetworkingIdentity &identity, char *buf, size_t cbBuf )
{
	return ::SteamNetworkingIdentity_ToString( &identity, buf, cbBuf );
}

bool CSteamNetworkingUtils::SteamNetworkingIdentity_ParseString( SteamNetworkingIdentity *pIdentity, const char *pszStr )
{
	return ::SteamNetworkingIdentity_ParseString( pIdentity, sizeof(SteamNetworkingIdentity), pszStr );
}

AppId_t CSteamNetworkingUtils::GetAppID()
{
	return m_nAppID;
}

time_t CSteamNetworkingUtils::GetTimeSecure()
{
	// Trusting local user's clock!
	return time(nullptr);
}

} // namespace SteamNetworkingSocketsLib
using namespace SteamNetworkingSocketsLib;

/////////////////////////////////////////////////////////////////////////////
//
// Global API interface
//
/////////////////////////////////////////////////////////////////////////////

#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE

static CSteamNetworkingSockets *s_pSteamNetworkingSockets = nullptr;

STEAMNETWORKINGSOCKETS_INTERFACE bool GameNetworkingSockets_Init( const SteamNetworkingIdentity *pIdentity, SteamNetworkingErrMsg &errMsg )
{
	SteamDatagramTransportLock lock( "GameNetworkingSockets_Init" );

	// Already initted?
	if ( s_pSteamNetworkingSockets )
	{
		AssertMsg( false, "GameNetworkingSockets_init called multiple times?" );
		return true;
	}

	// Init basic functionality
	CSteamNetworkingSockets *pSteamNetworkingSockets = new CSteamNetworkingSockets( ( CSteamNetworkingUtils *)SteamNetworkingUtils() );
	if ( !pSteamNetworkingSockets->BInitGameNetworkingSockets( pIdentity, errMsg ) )
	{
		pSteamNetworkingSockets->Destroy();
		return false;
	}

	s_pSteamNetworkingSockets = pSteamNetworkingSockets;
	return true;
}

STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_Kill()
{
	SteamDatagramTransportLock lock( "GameNetworkingSockets_Kill" );
	if ( s_pSteamNetworkingSockets )
	{
		s_pSteamNetworkingSockets->Destroy();
		s_pSteamNetworkingSockets = nullptr;
	}
}

STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamNetworkingSockets()
{
	return s_pSteamNetworkingSockets;
}

STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingUtils *SteamNetworkingUtils()
{
	static CSteamNetworkingUtils s_utils;
	return &s_utils;
}

#endif
