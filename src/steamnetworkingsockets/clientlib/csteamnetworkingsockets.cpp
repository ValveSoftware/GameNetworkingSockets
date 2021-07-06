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

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
#include "csteamnetworkingmessages.h"
#endif

// Needed for the platform checks below
#if defined(__APPLE__)
	#include "AvailabilityMacros.h"
	#include "TargetConditionals.h"
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
DEFINE_GLOBAL_CONFIGVAL( int32, PacketTraceMaxBytes, -1, -1, 99999 );
DEFINE_GLOBAL_CONFIGVAL( int32, FakeRateLimit_Send_Rate, 0, 0, 1024*1024*1024 );
DEFINE_GLOBAL_CONFIGVAL( int32, FakeRateLimit_Send_Burst, 16*1024, 0, 1024*1024 );
DEFINE_GLOBAL_CONFIGVAL( int32, FakeRateLimit_Recv_Rate, 0, 0, 1024*1024*1024 );
DEFINE_GLOBAL_CONFIGVAL( int32, FakeRateLimit_Recv_Burst, 16*1024, 0, 1024*1024 );

DEFINE_GLOBAL_CONFIGVAL( void *, Callback_AuthStatusChanged, nullptr );
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
DEFINE_GLOBAL_CONFIGVAL( void*, Callback_MessagesSessionRequest, nullptr );
DEFINE_GLOBAL_CONFIGVAL( void*, Callback_MessagesSessionFailed, nullptr );
#endif
DEFINE_GLOBAL_CONFIGVAL( void *, Callback_CreateConnectionSignaling, nullptr );
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
DEFINE_GLOBAL_CONFIGVAL( void *, Callback_FakeIPResult, nullptr );
#endif

DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, TimeoutInitial, 10000, 0, INT32_MAX );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, TimeoutConnected, 10000, 0, INT32_MAX );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, SendBufferSize, 512*1024, 0, 0x10000000 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int64, ConnectionUserData, -1 ); // no limits here
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
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, SymmetricConnect, 0, 0, 1 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LocalVirtualPort, -1, -1, 65535 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_AckRTT, k_ESteamNetworkingSocketsDebugOutputType_Warning, k_ESteamNetworkingSocketsDebugOutputType_Error, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_PacketDecode, k_ESteamNetworkingSocketsDebugOutputType_Warning, k_ESteamNetworkingSocketsDebugOutputType_Error, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_Message, k_ESteamNetworkingSocketsDebugOutputType_Warning, k_ESteamNetworkingSocketsDebugOutputType_Error, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_PacketGaps, k_ESteamNetworkingSocketsDebugOutputType_Warning, k_ESteamNetworkingSocketsDebugOutputType_Error, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_P2PRendezvous, k_ESteamNetworkingSocketsDebugOutputType_Warning, k_ESteamNetworkingSocketsDebugOutputType_Error, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( void *, Callback_ConnectionStatusChanged, nullptr );

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ICE
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( std::string, P2P_STUN_ServerList, "" );

COMPILE_TIME_ASSERT( k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Default == -1 );
COMPILE_TIME_ASSERT( k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable == 0 );
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
	// There is no such thing as "default" if we don't have some sort of platform
	DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All );
#else
	DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Default, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Default, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All );
#endif

DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, P2P_Transport_ICE_Penalty, 0, 0, INT_MAX );
#endif

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( std::string, SDRClient_DebugTicketAddress, "" );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, P2P_Transport_SDR_Penalty, 0, 0, INT_MAX );
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
	SteamNetworkingGlobalLock scopeLock;
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
CUtlHashMap<int, CSteamNetworkPollGroup *, std::equal_to<int>, Identity<int> > g_mapPollGroups;
TableLock g_tables_lock;

// Table of active listen sockets.  Listen sockets and this table are protected
// by the global lock.
CUtlHashMap<int, CSteamNetworkListenSocketBase *, std::equal_to<int>, Identity<int> > g_mapListenSockets; 

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

static CSteamNetworkConnectionBase *InternalGetConnectionByHandle( HSteamNetConnection sock, ConnectionScopeLock &scopeLock, const char *pszLockTag, bool bForAPI )
{
	if ( sock == 0 )
		return nullptr;
	TableScopeLock tableScopeLock( g_tables_lock );
	int idx = g_mapConnections.Find( uint16( sock ) );
	if ( idx == g_mapConnections.InvalidIndex() )
		return nullptr;
	CSteamNetworkConnectionBase *pResult = g_mapConnections[ idx ];
	if ( !pResult )
	{
		AssertMsg( false, "g_mapConnections corruption!" );
		return nullptr;
	}
	if ( uint16( pResult->m_hConnectionSelf ) != uint16( sock ) )
	{
		AssertMsg( false, "Connection map corruption!" );
		return nullptr;
	}

	// Make sure connection is not in the process of being self-destructed
	bool bLocked = false;
	for (;;)
	{

		// Fetch the state of the connection.  This is OK to do
		// even if we don't have the lock.
		ESteamNetworkingConnectionState s = pResult->GetState();
		if ( s == k_ESteamNetworkingConnectionState_Dead )
			break;
		if ( bForAPI )
		{
			if ( !BConnectionStateExistsToAPI( s ) )
				break;
		}

		// Have we locked already?  Then we're good
		if ( bLocked )
		{
			// NOTE: We unlock the table lock here, OUT OF ORDER!
			return pResult; 
		}

		// State looks good, try to lock the connection.
		// NOTE: we still (briefly) hold the table lock!
		// We *should* be able to totally block here
		// without creating a deadlock, but looping here
		// isn't so bad
		bLocked = scopeLock.TryLock( *pResult->m_pLock, 5, pszLockTag );
	}

	// Connection found in table, but should not be returned to the caller.
	// Unlock the connection, if we locked it
	if ( bLocked )
		scopeLock.Unlock();
	
	return nullptr;
}

CSteamNetworkConnectionBase *GetConnectionByHandle( HSteamNetConnection sock, ConnectionScopeLock &scopeLock )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	return InternalGetConnectionByHandle( sock, scopeLock, nullptr, false );
}

inline CSteamNetworkConnectionBase *GetConnectionByHandleForAPI( HSteamNetConnection sock, ConnectionScopeLock &scopeLock, const char *pszLockTag )
{
	return InternalGetConnectionByHandle( sock, scopeLock, pszLockTag, true );
}

static CSteamNetworkListenSocketBase *GetListenSocketByHandle( HSteamListenSocket sock )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread(); // listen sockets are protected by the global lock!
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

CSteamNetworkPollGroup *GetPollGroupByHandle( HSteamNetPollGroup hPollGroup, PollGroupScopeLock &scopeLock, const char *pszLockTag )
{
	if ( hPollGroup == k_HSteamNetPollGroup_Invalid )
		return nullptr;
	AssertMsg( (hPollGroup & 0x80000000), "A listen socket handle was used where a poll group handle was expected" );
	int idx = hPollGroup & 0xffff;
	TableScopeLock tableScopeLock( g_tables_lock );
	if ( !g_mapPollGroups.IsValidIndex( idx ) )
		return nullptr;
	CSteamNetworkPollGroup *pResult = g_mapPollGroups[ idx ];

	// Make sure poll group is the one they really asked for, and also
	// handle deletion race condition
	while ( pResult->m_hPollGroupSelf == hPollGroup )
	{
		if ( scopeLock.TryLock( pResult->m_lock, 1, pszLockTag ) )
			return pResult;
	}

	// Slot was reused, but this handle is now invalid,
	// or poll group deleted race condition
	return nullptr;
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
, m_bEverTriedToGetCert( false )
, m_bEverGotCert( false )
#ifdef STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT
, m_scheduleCheckRenewCert( this, &CSteamNetworkingSockets::CheckAuthenticationPrerequisites )
#endif
, m_mutexPendingCallbacks( "pending_callbacks" )
{
	m_connectionConfig.Init( nullptr );
	InternalInitIdentity();
}

void CSteamNetworkingSockets::InternalInitIdentity()
{
	m_identity.Clear();
	m_msgSignedCert.Clear();
	m_msgCert.Clear();
	m_keyPrivateKey.Wipe();

	#ifdef STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT
		m_CertStatus.m_eAvail = k_ESteamNetworkingAvailability_NeverTried;
		m_CertStatus.m_debugMsg[0] = '\0';
	#else
		m_CertStatus.m_eAvail = k_ESteamNetworkingAvailability_CannotTry;
		V_strcpy_safe( m_CertStatus.m_debugMsg, "No certificate authority" );
	#endif
	m_AuthenticationStatus = m_CertStatus;
	m_bEverTriedToGetCert = false;
	m_bEverGotCert = false;
}

CSteamNetworkingSockets::~CSteamNetworkingSockets()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
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
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingSockets::KillConnections" );
	TableScopeLock tableScopeLock( g_tables_lock );

	// Warn messages interface that it needs to clean up.  We need to do this
	// because that class has pointers to objects that we are about to destroy.
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
		if ( m_pSteamNetworkingMessages )
			m_pSteamNetworkingMessages->FreeResources();
	#endif

	// Destroy all of my connections
	CSteamNetworkConnectionBase::ProcessDeletionList();
	FOR_EACH_HASHMAP( g_mapConnections, idx )
	{
		CSteamNetworkConnectionBase *pConn = g_mapConnections[idx];
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
		{
			ConnectionScopeLock connectionLock( *pConn );
			pConn->ConnectionQueueDestroy();
		}
	}
	CSteamNetworkConnectionBase::ProcessDeletionList();

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
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CSteamNetworkingSockets::Destroy" );

	FreeResources();

	// Nuke messages interface, if we had one.
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
		if ( m_pSteamNetworkingMessages )
		{
			delete m_pSteamNetworkingMessages;
			Assert( m_pSteamNetworkingMessages == nullptr ); // Destructor should sever this link
			m_pSteamNetworkingMessages = nullptr; // Buuuuut we'll slam it, too, in case there's a bug
		}
	#endif

	// Remove from list of extant instances, if we are there
	find_and_remove_element( s_vecSteamNetworkingSocketsInstances, this );

	delete this;
}

void CSteamNetworkingSockets::FreeResources()
{

	KillConnections();

	// Clear identity and crypto stuff.
	// If we are re-initialized, we might get new ones
	InternalInitIdentity();

	// Mark us as no longer being setup
	if ( m_bHaveLowLevelRef )
	{
		m_bHaveLowLevelRef = false;
		SteamNetworkingSocketsLowLevelDecRef();
	}
}

bool CSteamNetworkingSockets::BHasAnyConnections() const
{
	TableScopeLock tableScopeLock( g_tables_lock );
	for ( CSteamNetworkConnectionBase *pConn: g_mapConnections.IterValues() )
	{
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
			return true;
	}
	return false;
}

bool CSteamNetworkingSockets::BHasAnyListenSockets() const
{
	TableScopeLock tableScopeLock( g_tables_lock );
	for ( CSteamNetworkListenSocketBase *pSock: g_mapListenSockets.IterValues() )
	{
		if ( pSock->m_pSteamNetworkingSocketsInterface == this )
			return true;
	}
	return false;
}

bool CSteamNetworkingSockets::GetIdentity( SteamNetworkingIdentity *pIdentity )
{
	SteamNetworkingGlobalLock scopeLock( "GetIdentity" );
	InternalGetIdentity();
	if ( pIdentity )
		*pIdentity = m_identity;
	return !m_identity.IsInvalid();
}

int CSteamNetworkingSockets::GetSecondsUntilCertExpiry() const
{
	if ( !m_msgSignedCert.has_cert() )
		return INT_MIN;

	Assert( m_msgSignedCert.has_ca_signature() ); // Connections may use unsigned certs in certain situations, but we never use them here
	Assert( m_msgCert.has_key_data() );
	Assert( m_msgCert.has_time_expiry() ); // We should never generate keys without an expiry!

	int nSeconduntilExpiry = (long)m_msgCert.time_expiry() - (long)m_pSteamNetworkingUtils->GetTimeSecure();
	return nSeconduntilExpiry;
}

bool CSteamNetworkingSockets::GetCertificateRequest( int *pcbBlob, void *pBlob, SteamNetworkingErrMsg &errMsg )
{
	SteamNetworkingGlobalLock scopeLock( "GetCertificateRequest" );

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

	SteamNetworkingGlobalLock scopeLock( "SetCertificate" );

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
	// If shouldn't already be expired.
	AssertMsg( GetSecondsUntilCertExpiry() > 0, "Cert already invalid / expired?" );

	// We've got a valid cert
	SetCertStatus( k_ESteamNetworkingAvailability_Current, "OK" );

	// Make sure we have everything else we need to do authentication.
	// This will also make sure we have renewal scheduled
	AuthenticationNeeded();

	// OK
	return true;
}

void CSteamNetworkingSockets::ResetIdentity( const SteamNetworkingIdentity *pIdentity )
{
#ifdef STEAMNETWORKINGSOCKETS_STEAM
	Assert( !"Not supported on steam" );
#else
	KillConnections();
	InternalInitIdentity();
	if ( pIdentity )
		m_identity = *pIdentity;
#endif
}

ESteamNetworkingAvailability CSteamNetworkingSockets::InitAuthentication()
{
	SteamNetworkingGlobalLock scopeLock( "InitAuthentication" );

	// Check/fetch prerequisites
	AuthenticationNeeded();

	// Return status
	return m_AuthenticationStatus.m_eAvail;
}

void CSteamNetworkingSockets::CheckAuthenticationPrerequisites( SteamNetworkingMicroseconds usecNow )
{
#ifdef STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

	// Check if we're in flight already.
	bool bInFlight = BCertRequestInFlight();

	// Do we already have a cert?
	if ( m_msgSignedCert.has_cert() )
	{
		//Assert( m_CertStatus.m_eAvail == k_ESteamNetworkingAvailability_Current );

		// How much more life does it have in it?
		int nSeconduntilExpiry = GetSecondsUntilCertExpiry();
		if ( nSeconduntilExpiry < 0 )
		{

			// It's already expired, we might as well discard it now.
			SpewMsg( "Cert expired %d seconds ago.  Discarding and requesting another\n", -nSeconduntilExpiry );
			m_msgSignedCert.Clear();
			m_msgCert.Clear();
			m_keyPrivateKey.Wipe();

			// Update cert status
			SetCertStatus( k_ESteamNetworkingAvailability_Previously, "Expired" );
		}
		else
		{

			// If request is already active, don't do any of the work below, and don't spam while we wait, since this function may be called frequently.
			if ( bInFlight )
				return;

			// Check if it's time to renew
			int nSecondsUntilRenew = nSeconduntilExpiry - k_nSecCertExpirySeekRenew;
			if ( nSecondsUntilRenew > 0 )
			{

				// Schedule a wakeup
				constexpr SteamNetworkingMicroseconds kFudge = k_nMillion*3/2;
				SteamNetworkingMicroseconds usecTargetCheck = std::min( usecNow + nSecondsUntilRenew*k_nMillion + kFudge, usecNow + 600*k_nMillion );
				SteamNetworkingMicroseconds usecScheduledCheck = m_scheduleCheckRenewCert.GetScheduleTime();
				if ( usecScheduledCheck <= usecTargetCheck + kFudge*2 )
				{
					// Currently scheduled time is good enough.  Don't constantly update the schedule time,
					// that involves a (small amount) of work.  Just wait for it
				}
				else
				{
					// Schedule a check later
					m_scheduleCheckRenewCert.Schedule( usecTargetCheck );

					// !TEST! Spew cert expiry
					long long expiry = m_msgCert.time_expiry(); Assert( expiry > 0 );
					long long now = m_pSteamNetworkingUtils->GetTimeSecure();
					long long nSecondsUntilExpiry = expiry - now;
					SpewMsg( "Certificate expires in %lldh%02lldm at %lld (current time %lld), will renew in %dh%02dm\n",
						nSecondsUntilExpiry/3600, ( nSecondsUntilExpiry/60 ) % 60, expiry, now,
						nSecondsUntilRenew/3600, (nSecondsUntilRenew/60)%60 );
				}
				return;
			}

			// Currently valid, but it's time to renew.  Spew about this.
			SpewMsg( "Cert expires in %d seconds.  Requesting another, but keeping current cert in case request fails\n", nSeconduntilExpiry );
		}
	}

	// If a request is already active, then we just need to wait for it to complete
	if ( bInFlight )
		return;

	// Invoke platform code to begin fetching a cert
	BeginFetchCertAsync();
#endif
}

void CSteamNetworkingSockets::SetCertStatus( ESteamNetworkingAvailability eAvail, const char *pszFmt, ... )
{
	char msg[ sizeof(m_CertStatus.m_debugMsg) ];
	va_list ap;
	va_start( ap, pszFmt );
	V_vsprintf_safe( msg, pszFmt, ap );
	va_end( ap );

	// Mark success or an attempt
	if ( eAvail == k_ESteamNetworkingAvailability_Current )
		m_bEverGotCert = true;
	if ( eAvail == k_ESteamNetworkingAvailability_Attempting || eAvail == k_ESteamNetworkingAvailability_Retrying )
		m_bEverTriedToGetCert = true;

	// If we failed, but we previously succeeded, convert to "previously"
	if ( eAvail == k_ESteamNetworkingAvailability_Failed && m_bEverGotCert )
		eAvail = k_ESteamNetworkingAvailability_Previously;

	// No change?
	if ( m_CertStatus.m_eAvail == eAvail && V_stricmp( m_CertStatus.m_debugMsg, msg ) == 0 )
		return;

	// Update
	m_CertStatus.m_eAvail = eAvail;
	V_strcpy_safe( m_CertStatus.m_debugMsg, msg );

	// Check if our high level authentication status changed
	DeduceAuthenticationStatus();
}

void CSteamNetworkingSockets::DeduceAuthenticationStatus()
{
	// For the base class, the overall authentication status is identical to the status of
	// our cert.  (Derived classes may add additional criteria)
	SetAuthenticationStatus( m_CertStatus );
}

void CSteamNetworkingSockets::SetAuthenticationStatus( const SteamNetAuthenticationStatus_t &newStatus )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

	// No change?
	bool bStatusChanged = newStatus.m_eAvail != m_AuthenticationStatus.m_eAvail;
	if ( !bStatusChanged && V_strcmp( m_AuthenticationStatus.m_debugMsg, newStatus.m_debugMsg ) == 0 )
		return;

	// Update
	m_AuthenticationStatus = newStatus;

	// Re-cache identity
	InternalGetIdentity();

	// Post a callback, but only if the high level status changed.  Don't post a callback just
	// because the message changed
	if ( bStatusChanged )
	{
		// Spew
		SpewMsg( "AuthStatus (%s):  %s  (%s)",
			SteamNetworkingIdentityRender( m_identity ).c_str(),
			GetAvailabilityString( m_AuthenticationStatus.m_eAvail ), m_AuthenticationStatus.m_debugMsg );

		QueueCallback( m_AuthenticationStatus, g_Config_Callback_AuthStatusChanged.Get() );
	}
}

#ifdef STEAMNETWORKINGSOCKETS_CAN_REQUEST_CERT
void CSteamNetworkingSockets::AsyncCertRequestFinished()
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "AsyncCertRequestFinished" );

	Assert( m_msgSignedCert.has_cert() );
	SetCertStatus( k_ESteamNetworkingAvailability_Current, "OK" );

	// Setup renewal check (this will also spew how much time until cert expires)
	m_scheduleCheckRenewCert.Cancel();
	CheckAuthenticationPrerequisites( SteamNetworkingSockets_GetLocalTimestamp() );

	// Check for any connections that we own that are waiting on a cert
	TableScopeLock tableScopeLock( g_tables_lock );
	for ( CSteamNetworkConnectionBase *pConn: g_mapConnections.IterValues() )
	{
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
			pConn->InterfaceGotCert();
	}
}

void CSteamNetworkingSockets::CertRequestFailed( ESteamNetworkingAvailability eCertAvail, ESteamNetConnectionEnd nConnectionEndReason, const char *pszMsg )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "CertRequestFailed" );

	SpewWarning( "Cert request for %s failed with reason code %d.  %s\n", SteamNetworkingIdentityRender( InternalGetIdentity() ).c_str(), nConnectionEndReason, pszMsg );

	// Schedule a retry.  Note that if we have active connections that need for a cert,
	// we may end up retrying sooner.  If we don't have any active connections, spamming
	// retries way too frequently may be really bad; we might end up DoS-ing ourselves.
	// Do we need to make this configurable?
	m_scheduleCheckRenewCert.Schedule( SteamNetworkingSockets_GetLocalTimestamp() + k_nMillion*30 );

	if ( m_msgSignedCert.has_cert() )
	{
		SpewMsg( "But we still have a valid cert, continuing with that one\n" );
		AsyncCertRequestFinished();
		return;
	}

	// Set generic cert status, so we will post a callback
	SetCertStatus( eCertAvail, "%s", pszMsg );

	TableScopeLock tableScopeLock( g_tables_lock );
	for ( CSteamNetworkConnectionBase *pConn: g_mapConnections.IterValues() )
	{
		if ( pConn->m_pSteamNetworkingSocketsInterface == this )
			pConn->CertRequestFailed( nConnectionEndReason, pszMsg );
	}

	// FIXME If we have any listen sockets, we might need to let them know about this as well?
}
#endif

ESteamNetworkingAvailability CSteamNetworkingSockets::GetAuthenticationStatus( SteamNetAuthenticationStatus_t *pDetails )
{
	SteamNetworkingGlobalLock scopeLock; // !SPEED! We could protect this with a more tightly scoped lock, if we think this is eomthing people might be polling

	// Return details, if requested
	if ( pDetails )
		*pDetails = m_AuthenticationStatus;

	// Return status
	return m_AuthenticationStatus.m_eAvail;
}

HSteamListenSocket CSteamNetworkingSockets::CreateListenSocketIP( const SteamNetworkingIPAddr &localAddr, int nOptions, const SteamNetworkingConfigValue_t *pOptions )
{
	SteamNetworkingGlobalLock scopeLock( "CreateListenSocketIP" );
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
	SteamNetworkingGlobalLock scopeLock( "ConnectByIPAddress" );
	ConnectionScopeLock connectionLock;

	// Check if the IP address is "fake" and this is really a P2P connection
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		if ( address.IsFakeIP() )
		{
			SteamNetworkingIdentity identityRemote;
			identityRemote.SetIPAddr( address );
			int nRemoveVirtualPort = -1; // Ignored, we multiplex in this case based on the fake port
			CSteamNetworkConnectionBase *pConn = InternalConnectP2PDefaultSignaling( identityRemote, nRemoveVirtualPort, nOptions, pOptions, connectionLock );
			if ( !pConn )
				return k_HSteamNetConnection_Invalid;
			return pConn->m_hConnectionSelf;
		}
	#endif

	CSteamNetworkConnectionUDP *pConn = new CSteamNetworkConnectionUDP( this, connectionLock );
	if ( !pConn )
		return k_HSteamNetConnection_Invalid;
	SteamDatagramErrMsg errMsg;
	if ( !pConn->BInitConnect( address, nOptions, pOptions, errMsg ) )
	{
		SpewError( "Cannot create IPv4 connection.  %s", errMsg );
		pConn->ConnectionQueueDestroy();
		return k_HSteamNetConnection_Invalid;
	}

	return pConn->m_hConnectionSelf;
}


EResult CSteamNetworkingSockets::AcceptConnection( HSteamNetConnection hConn )
{
	SteamNetworkingGlobalLock scopeLock( "AcceptConnection" ); // Take global lock, since this will lead to connection state transition
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, nullptr );
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
	SteamNetworkingGlobalLock scopeLock( "CloseConnection" ); // Take global lock, we are going to change connection state and/or destroy objects
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, nullptr );
	if ( !pConn )
		return false;

	// Close it
	pConn->APICloseConnection( nReason, pszDebug, bEnableLinger );
	return true;
}

bool CSteamNetworkingSockets::CloseListenSocket( HSteamListenSocket hSocket )
{
	SteamNetworkingGlobalLock scopeLock( "CloseListenSocket" ); // Take global lock, we are going to destroy objects
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
	//SteamNetworkingGlobalLock scopeLock( "SetConnectionUserData" ); // NO, not necessary!
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hPeer, connectionLock, "SetConnectionUserData" );
	if ( !pConn )
		return false;
	pConn->SetUserData( nUserData );
	return true;
}

int64 CSteamNetworkingSockets::GetConnectionUserData( HSteamNetConnection hPeer )
{
	//SteamNetworkingGlobalLock scopeLock( "GetConnectionUserData" ); // NO, not necessary!
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hPeer, connectionLock, "GetConnectionUserData" );
	if ( !pConn )
		return -1;
	return pConn->GetUserData();
}

void CSteamNetworkingSockets::SetConnectionName( HSteamNetConnection hPeer, const char *pszName )
{
	SteamNetworkingGlobalLock scopeLock( "SetConnectionName" ); // NOTE: Yes, we must take global lock for this.  See CSteamNetworkConnectionBase::SetDescription
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hPeer, connectionLock, nullptr );
	if ( !pConn )
		return;
	pConn->SetAppName( pszName );
}

bool CSteamNetworkingSockets::GetConnectionName( HSteamNetConnection hPeer, char *pszName, int nMaxLen )
{
	//SteamNetworkingGlobalLock scopeLock( "GetConnectionName" ); // NO, not necessary!
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hPeer, connectionLock, "GetConnectionName" );
	if ( !pConn )
		return false;
	V_strncpy( pszName, pConn->GetAppName(), nMaxLen );
	return true;
}

EResult CSteamNetworkingSockets::SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber )
{
	//SteamNetworkingGlobalLock scopeLock( "SendMessageToConnection" ); // NO, not necessary!
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, "SendMessageToConnection" );
	if ( !pConn )
		return k_EResultInvalidParam;
	return pConn->APISendMessageToConnection( pData, cbData, nSendFlags, pOutMessageNumber );
}

void CSteamNetworkingSockets::SendMessages( int nMessages, SteamNetworkingMessage_t *const *pMessages, int64 *pOutMessageNumberOrResult )
{

	// Get list of messages, grouped by connection.
	// But within the connection, it is important that we
	// keep them in the same order!
	struct SortMsg_t
	{
		HSteamNetConnection m_hConn;
		int m_idx;
		inline bool operator<(const SortMsg_t &x ) const
		{
			if ( m_hConn < x.m_hConn ) return true;
			if ( m_hConn > x.m_hConn ) return false;
			return m_idx < x.m_idx;
		}
	};
	SortMsg_t *pSortMessages = (SortMsg_t *)alloca( nMessages * sizeof(SortMsg_t) );
	int nSortMessages = 0;

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

		if ( pMsg->m_conn == k_HSteamNetConnection_Invalid )
		{
			if ( pOutMessageNumberOrResult )
				pOutMessageNumberOrResult[i] = -k_EResultInvalidParam;
			pMsg->Release();
			continue;
		}

		pSortMessages[ nSortMessages ].m_hConn = pMsg->m_conn;
		pSortMessages[ nSortMessages ].m_idx = i;
		++nSortMessages;
	}

	if ( nSortMessages < 1 )
		return;

	SortMsg_t *const pSortEnd = pSortMessages+nSortMessages;
	std::sort( pSortMessages, pSortEnd );

	// OK, we are ready to begin

	// SteamNetworkingGlobalLock scopeLock( "SendMessages" ); // NO, not necessary!
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	CSteamNetworkConnectionBase *pConn = nullptr;
	HSteamNetConnection hConn = k_HSteamNetConnection_Invalid;
	ConnectionScopeLock connectionLock;
	bool bConnectionThinkImmediately = false;
	for ( SortMsg_t *pSort = pSortMessages ; pSort < pSortEnd ; ++pSort )
	{

		// Switched to a different connection?
		if ( hConn != pSort->m_hConn )
		{

			// Flush out previous connection, if any
			if ( pConn )
			{
				if ( bConnectionThinkImmediately )
					pConn->CheckConnectionStateOrScheduleWakeUp( usecNow );
				connectionLock.Unlock();
				bConnectionThinkImmediately = false;
			}

			// Locate the connection
			hConn = pSort->m_hConn;
			pConn = GetConnectionByHandleForAPI( hConn, connectionLock, "SendMessages" );
		}

		CSteamNetworkingMessage *pMsg = static_cast<CSteamNetworkingMessage*>( pMessages[pSort->m_idx] );

		// Current connection is valid?
		int64 result;
		if ( pConn )
		{

			// Attempt to send
			bool bThinkImmediately = false;
			result = pConn->APISendMessageToConnection( pMsg, usecNow, &bThinkImmediately );
			if ( bThinkImmediately )
				bConnectionThinkImmediately = true;
		}
		else
		{
			pMsg->Release();
			result = -k_EResultInvalidParam;
		}

		// Return result for this message if they asked for it
		if ( pOutMessageNumberOrResult )
			pOutMessageNumberOrResult[pSort->m_idx] = result;
	}

	// Flush out last connection, if any
	if ( bConnectionThinkImmediately )
		pConn->CheckConnectionStateOrScheduleWakeUp( usecNow );
}

EResult CSteamNetworkingSockets::FlushMessagesOnConnection( HSteamNetConnection hConn )
{
	//SteamNetworkingGlobalLock scopeLock( "FlushMessagesOnConnection" ); // NO, not necessary!
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, "FlushMessagesOnConnection" );
	if ( !pConn )
		return k_EResultInvalidParam;
	return pConn->APIFlushMessageOnConnection();
}

int CSteamNetworkingSockets::ReceiveMessagesOnConnection( HSteamNetConnection hConn, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	//SteamNetworkingGlobalLock scopeLock( "ReceiveMessagesOnConnection" ); // NO, not necessary!
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, "ReceiveMessagesOnConnection" );
	if ( !pConn )
		return -1;
	return pConn->APIReceiveMessages( ppOutMessages, nMaxMessages );
}

HSteamNetPollGroup CSteamNetworkingSockets::CreatePollGroup()
{
	SteamNetworkingGlobalLock scopeLock( "CreatePollGroup" ); // Take global lock, because we will be creating objects
	PollGroupScopeLock pollGroupScopeLock;
	CSteamNetworkPollGroup *pPollGroup = InternalCreatePollGroup( pollGroupScopeLock );
	return pPollGroup->m_hPollGroupSelf;
}

CSteamNetworkPollGroup *CSteamNetworkingSockets::InternalCreatePollGroup( PollGroupScopeLock &scopeLock )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();
	TableScopeLock tableScopeLock( g_tables_lock );
	CSteamNetworkPollGroup *pPollGroup = new CSteamNetworkPollGroup( this );
	scopeLock.Lock( pPollGroup->m_lock );
	pPollGroup->AssignHandleAndAddToGlobalTable();
	return pPollGroup;
}

bool CSteamNetworkingSockets::DestroyPollGroup( HSteamNetPollGroup hPollGroup )
{
	SteamNetworkingGlobalLock scopeLock( "DestroyPollGroup" ); // Take global lock, since we'll be destroying objects
	TableScopeLock tableScopeLock( g_tables_lock ); // We'll need to be able to remove the poll group from the tables list
	PollGroupScopeLock pollGroupLock;
	CSteamNetworkPollGroup *pPollGroup = GetPollGroupByHandle( hPollGroup, pollGroupLock, nullptr );
	if ( !pPollGroup )
		return false;
	pollGroupLock.Abandon(); // We're about to destroy the lock itself.  The Destructor will unlock -- we don't want to do it again.
	delete pPollGroup;
	return true;
}

bool CSteamNetworkingSockets::SetConnectionPollGroup( HSteamNetConnection hConn, HSteamNetPollGroup hPollGroup )
{
	SteamNetworkingGlobalLock scopeLock( "SetConnectionPollGroup" ); // Take global lock, since we'll need to take multiple object locks
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, nullptr );
	if ( !pConn )
		return false;

	// NOTE: We are allowed to take multiple locks here, in any order, because we have the global
	// lock.  Code that does not hold the global lock may only lock one object at a time

	// Special case for removing the poll group
	if ( hPollGroup == k_HSteamNetPollGroup_Invalid )
	{
		pConn->RemoveFromPollGroup();
		return true;
	}


	PollGroupScopeLock pollGroupLock;
	CSteamNetworkPollGroup *pPollGroup = GetPollGroupByHandle( hPollGroup, pollGroupLock, nullptr );
	if ( !pPollGroup )
		return false;

	pConn->SetPollGroup( pPollGroup );

	return true;
}

int CSteamNetworkingSockets::ReceiveMessagesOnPollGroup( HSteamNetPollGroup hPollGroup, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	//SteamNetworkingGlobalLock scopeLock( "ReceiveMessagesOnPollGroup" ); // NO, not necessary!
	PollGroupScopeLock pollGroupLock;
	CSteamNetworkPollGroup *pPollGroup = GetPollGroupByHandle( hPollGroup, pollGroupLock, "ReceiveMessagesOnPollGroup" );
	if ( !pPollGroup )
		return -1;
	g_lockAllRecvMessageQueues.lock();
	int nMessagesReceived = pPollGroup->m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
	g_lockAllRecvMessageQueues.unlock();
	return nMessagesReceived;
}

#ifdef STEAMNETWORKINGSOCKETS_STEAMCLIENT
int CSteamNetworkingSockets::ReceiveMessagesOnListenSocketLegacyPollGroup( HSteamListenSocket hSocket, SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages )
{
	SteamNetworkingGlobalLock scopeLock( "ReceiveMessagesOnListenSocket" );
	CSteamNetworkListenSocketBase *pSock = GetListenSocketByHandle( hSocket );
	if ( !pSock )
		return -1;
	if ( !pSock->m_pLegacyPollGroup )
		return 0;
	g_lockAllRecvMessageQueues.lock();
	int nMessagesReceived = pSock->m_pLegacyPollGroup->m_queueRecvMessages.RemoveMessages( ppOutMessages, nMaxMessages );
	g_lockAllRecvMessageQueues.unlock();
	return nMessagesReceived;
}
#endif

bool CSteamNetworkingSockets::GetConnectionInfo( HSteamNetConnection hConn, SteamNetConnectionInfo_t *pInfo )
{
	//SteamNetworkingGlobalLock scopeLock( "GetConnectionInfo" ); // NO, not necessary!
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, "GetConnectionInfo" );
	if ( !pConn )
		return false;
	if ( pInfo )
		pConn->ConnectionPopulateInfo( *pInfo );
	return true;
}

bool CSteamNetworkingSockets::GetQuickConnectionStatus( HSteamNetConnection hConn, SteamNetworkingQuickConnectionStatus *pStats )
{
	//SteamNetworkingGlobalLock scopeLock( "GetQuickConnectionStatus" ); // NO, not necessary!
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, "GetQuickConnectionStatus" );
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
		SteamNetworkingGlobalLock scopeLock( "GetDetailedConnectionStatus" ); // In some use cases (SDR), we need to touch some shared data structures.  It's easier to just protect this with the global lock than to try to sort that out
		ConnectionScopeLock connectionLock;
		CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, nullptr );
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
	SteamNetworkingGlobalLock scopeLock( "GetListenSocketAddress" );
	CSteamNetworkListenSocketBase *pSock = GetListenSocketByHandle( hSocket );
	if ( !pSock )
		return false;
	return pSock->APIGetAddress( pAddress );
}

bool CSteamNetworkingSockets::CreateSocketPair( HSteamNetConnection *pOutConnection1, HSteamNetConnection *pOutConnection2, bool bUseNetworkLoopback, const SteamNetworkingIdentity *pIdentity1, const SteamNetworkingIdentity *pIdentity2 )
{
	SteamNetworkingGlobalLock scopeLock( "CreateSocketPair" );

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

bool CSteamNetworkingSockets::BMatchesIdentity( const SteamNetworkingIdentity &identity )
{
	if ( identity == InternalGetIdentity() )
		return true;

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		if ( identity.IsFakeIP() )
		{
			if ( GetFakePortIndex( identity.m_ip ) >= 0 )
				return true;
		}
	#endif

	return false;
}

bool CSteamNetworkingSockets::SetCertificateAndPrivateKey( const void *pCert, int cbCert, void *pPrivateKey, int cbPrivateKey )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread( "SetCertificateAndPrivateKey" );

	m_msgCert.Clear();
	m_msgSignedCert.Clear();
	m_keyPrivateKey.Wipe();

	//
	// Decode the private key
	//
	if ( !m_keyPrivateKey.LoadFromAndWipeBuffer( pPrivateKey, cbPrivateKey ) )
	{
		SetCertStatus( k_ESteamNetworkingAvailability_Failed, "Invalid private key" );
		return false;
	}

	//
	// Decode the cert
	//
	SteamNetworkingErrMsg parseErrMsg;
	if ( !ParseCertFromPEM( pCert, cbCert, m_msgSignedCert, parseErrMsg ) )
	{
		SetCertStatus( k_ESteamNetworkingAvailability_Failed, parseErrMsg );
		return false;
	}

	if (
		!m_msgSignedCert.has_cert()
		|| !m_msgCert.ParseFromString( m_msgSignedCert.cert() )
		|| !m_msgCert.has_time_expiry()
		|| !m_msgCert.has_key_data()
	) {
		SetCertStatus( k_ESteamNetworkingAvailability_Failed, "Invalid cert" );
		return false;
	}
	if ( m_msgCert.key_type() != CMsgSteamDatagramCertificate_EKeyType_ED25519 )
	{
		SetCertStatus( k_ESteamNetworkingAvailability_Failed, "Invalid cert or unsupported public key type" );
		return false;
	}

	//
	// Make sure that the private key and the cert match!
	//

	CECSigningPublicKey pubKey;
	if ( !pubKey.SetRawDataWithoutWipingInput( m_msgCert.key_data().c_str(), m_msgCert.key_data().length() ) )
	{
		SetCertStatus( k_ESteamNetworkingAvailability_Failed, "Invalid public key" );
		return false;
	}
	if ( !m_keyPrivateKey.MatchesPublicKey( pubKey ) )
	{
		SetCertStatus( k_ESteamNetworkingAvailability_Failed, "Private key doesn't match public key from cert" );
		return false;
	}

	SetCertStatus( k_ESteamNetworkingAvailability_Current, "OK" );

	return true;
}

int CSteamNetworkingSockets::GetP2P_Transport_ICE_Enable( const SteamNetworkingIdentity &identityRemote, int *pOutUserFlags )
{
	// We really shouldn't get here, because this is only a question that makes sense
	// to ask if we have also overridden this function in a derived class, or slammed
	// it before making the connection
	Assert( false );
	if ( pOutUserFlags )
		*pOutUserFlags = 0;
	return k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable;
}

void CSteamNetworkingSockets::RunCallbacks()
{

	// Swap into a temp, so that we only hold lock for
	// a brief period.
	std_vector<QueuedCallback> listTemp;
	m_mutexPendingCallbacks.lock();
	listTemp.swap( m_vecPendingCallbacks );
	m_mutexPendingCallbacks.unlock();

	// Dispatch the callbacks
	for ( QueuedCallback &x: listTemp )
	{
		// NOTE: this switch statement is probably not necessary, if we are willing to make
		// some (almost certainly reasonable in practice) assumptions about the parameter
		// passing ABI.  All of these function calls basically have the same signature except
		// for the actual type of the argument being pointed to.

		#define DISPATCH_CALLBACK( structType, fnType ) \
			case structType::k_iCallback: \
				COMPILE_TIME_ASSERT( sizeof(structType) <= sizeof(x.data) ); \
				((fnType)x.fnCallback)( (structType*)x.data ); \
				break; \

		switch ( x.nCallback )
		{
			DISPATCH_CALLBACK( SteamNetConnectionStatusChangedCallback_t, FnSteamNetConnectionStatusChanged )
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
			DISPATCH_CALLBACK( SteamNetAuthenticationStatus_t, FnSteamNetAuthenticationStatusChanged )
			DISPATCH_CALLBACK( SteamRelayNetworkStatus_t, FnSteamRelayNetworkStatusChanged )
		#endif
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_STEAMNETWORKINGMESSAGES
			DISPATCH_CALLBACK( SteamNetworkingMessagesSessionRequest_t, FnSteamNetworkingMessagesSessionRequest )
			DISPATCH_CALLBACK( SteamNetworkingMessagesSessionFailed_t, FnSteamNetworkingMessagesSessionFailed )
		#endif
		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
			DISPATCH_CALLBACK( SteamNetworkingFakeIPResult_t, FnSteamNetworkingFakeIPResult )
		#endif
			default:
				AssertMsg1( false, "Unknown callback type %d!", x.nCallback );
		}

		#undef DISPATCH_CALLBACK
	}
}

void CSteamNetworkingSockets::InternalQueueCallback( int nCallback, int cbCallback, const void *pvCallback, void *fnRegisteredFunctionPtr )
{
	SteamNetworkingGlobalLock::AssertHeldByCurrentThread();

	if ( !fnRegisteredFunctionPtr )
		return;
	if ( cbCallback > sizeof( ((QueuedCallback*)0)->data ) )
	{
		AssertMsg( false, "Callback doesn't fit!" );
		return;
	}
	AssertMsg( len( m_vecPendingCallbacks ) < 100, "Callbacks backing up and not being checked.  Need to check them more frequently!" );

	m_mutexPendingCallbacks.lock();
	QueuedCallback &q = *push_back_get_ptr( m_vecPendingCallbacks );
	q.nCallback = nCallback;
	q.fnCallback = fnRegisteredFunctionPtr;
	memcpy( q.data, pvCallback, cbCallback );
	m_mutexPendingCallbacks.unlock();
}

bool CSteamNetworkingSockets::BeginAsyncRequestFakeIP( int nNumPorts )
{
	AssertMsg( false, "FakeIP allocation requires Steam" );
	return false;
}

void CSteamNetworkingSockets::GetFakeIP( int idxFirstPort, SteamNetworkingFakeIPResult_t *pInfo )
{
	// Not supported by base class
	if ( pInfo )
	{
		memset( pInfo, 0, sizeof(*pInfo) );
		GetIdentity( &pInfo->m_identity );
		pInfo->m_eResult = k_EResultDisabled;
	}
}

EResult CSteamNetworkingSockets::GetRemoteFakeIPForConnection( HSteamNetConnection hConn, SteamNetworkingIPAddr *pOutAddr )
{
	ConnectionScopeLock connectionLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandleForAPI( hConn, connectionLock, "GetRemoteFakeIPForConnection" );
	if ( !pConn )
		return k_EResultInvalidParam;
	return pConn->APIGetRemoteFakeIPForConnection( pOutAddr );
}

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
	intptr_t scopeObj,
	ConnectionScopeLock &connectionLock // Lock this, if it's a connection
)
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
			CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( HSteamNetConnection( scopeObj ), connectionLock );
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

static bool AssignConfigValueTyped( int32 *pVal, ESteamNetworkingConfigDataType eDataType, const void *pArg )
{
	switch ( eDataType )
	{
		case k_ESteamNetworkingConfig_Int32:
			*pVal = *(int32*)pArg;
			break;

		case k_ESteamNetworkingConfig_Int64:
		{
			int64 arg = *(int64*)pArg;
			if ( (int32)arg != arg )
				return false; // Cannot truncate!
			*pVal = *(int32*)arg;
			break;
		}

		case k_ESteamNetworkingConfig_Float:
			*pVal = (int32)floor( *(float*)pArg + .5f );
			break;

		case k_ESteamNetworkingConfig_String:
		{
			int x;
			if ( sscanf( (const char *)pArg, "%d", &x ) != 1 )
				return false;
			*pVal = x;
			break;
		}

		default:
			return false;
	}

	return true;
}

static bool AssignConfigValueTyped( int64 *pVal, ESteamNetworkingConfigDataType eDataType, const void *pArg )
{
	switch ( eDataType )
	{
		case k_ESteamNetworkingConfig_Int32:
			*pVal = *(int32*)pArg;
			break;

		case k_ESteamNetworkingConfig_Int64:
		{
			*pVal = *(int64*)pArg;
			break;
		}

		case k_ESteamNetworkingConfig_Float:
			*pVal = (int64)floor( *(float*)pArg + .5f );
			break;

		case k_ESteamNetworkingConfig_String:
		{
			long long x;
			if ( sscanf( (const char *)pArg, "%lld", &x ) != 1 )
				return false;
			*pVal = (int64)x;
			break;
		}

		default:
			return false;
	}

	return true;
}

static bool AssignConfigValueTyped( float *pVal, ESteamNetworkingConfigDataType eDataType, const void *pArg )
{
	switch ( eDataType )
	{
		case k_ESteamNetworkingConfig_Int32:
			*pVal = (float)( *(int32*)pArg );
			break;

		case k_ESteamNetworkingConfig_Int64:
		{
			*pVal = (float)( *(int64*)pArg );
			break;
		}

		case k_ESteamNetworkingConfig_Float:
			*pVal = *(float*)pArg;
			break;

		case k_ESteamNetworkingConfig_String:
		{
			float x;
			if ( sscanf( (const char *)pArg, "%f", &x ) != 1 )
				return false;
			*pVal = x;
			break;
		}

		default:
			return false;
	}

	return true;
}

static bool AssignConfigValueTyped( std::string *pVal, ESteamNetworkingConfigDataType eDataType, const void *pArg )
{
	char temp[64];

	switch ( eDataType )
	{
		case k_ESteamNetworkingConfig_Int32:
			V_sprintf_safe( temp, "%d", *(int32*)pArg );
			*pVal = temp;
			break;

		case k_ESteamNetworkingConfig_Int64:
			V_sprintf_safe( temp, "%lld", (long long)*(int64*)pArg );
			*pVal = temp;
			break;

		case k_ESteamNetworkingConfig_Float:
			V_sprintf_safe( temp, "%g", *(float*)pArg );
			*pVal = temp;
			break;

		case k_ESteamNetworkingConfig_String:
			*pVal = (const char *)pArg;
			break;

		default:
			return false;
	}

	return true;
}

static bool AssignConfigValueTyped( void **pVal, ESteamNetworkingConfigDataType eDataType, const void *pArg )
{
	switch ( eDataType )
	{
		case k_ESteamNetworkingConfig_Ptr:
			*pVal = *(void **)pArg;
			break;

		default:
			return false;
	}

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
	ConnectionScopeLock connectionLock;
	ConfigValue<T> *pVal = EvaluateScopeConfigValue<T>( pEntry, eScopeType, scopeObj, connectionLock );
	if ( !pVal )
		return false;

	// Locked values cannot be changed
	if ( pVal->IsLocked() )
		return false;

	// Clearing the value?
	if ( pArg == nullptr )
	{
		if ( eScopeType == k_ESteamNetworkingConfig_Global )
		{
			auto *pGlobal = (typename GlobalConfigValueBase<T>::Value *)( pVal );
			Assert( pGlobal->m_pInherit == nullptr );
			Assert( pGlobal->IsSet() );
			pGlobal->m_data = pGlobal->m_defaultValue;
		}
		else if ( eScopeType == k_ESteamNetworkingConfig_Connection && pEntry->m_eValue == k_ESteamNetworkingConfig_ConnectionUserData )
		{
			// Once this is set, we cannot clear it or inherit it.
			SpewError( "Cannot clear connection user data\n" );
			return false;
		}
		else
		{
			Assert( pVal->m_pInherit );
			pVal->m_eState = ConfigValueBase::kENotSet;
		}
		return true;
	}

	// Call type-specific method to set it
	if ( !AssignConfigValueTyped( &pVal->m_data, eDataType, pArg ) )
		return false;

	// Mark it as set
	pVal->m_eState = ConfigValueBase::kESet;

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
	ConnectionScopeLock connectionLock;
	ConfigValue<T> *pVal = EvaluateScopeConfigValue<T>( pEntry, eScopeType, scopeObj, connectionLock );
	if ( !pVal )
	{
		*cbResult = 0;
		return k_ESteamNetworkingGetConfigValue_BadScopeObj;
	}

	// Remember if it was set at this level
	bool bValWasSet = pVal->IsSet();

	// Find the place where the actual value comes from
	while ( !pVal->IsSet() )
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

	// Check for special values
	switch ( eValue )
	{
		case k_ESteamNetworkingConfig_MTU_DataSize:
			SpewWarning( "MTU_DataSize is readonly" );
			return false;

		case k_ESteamNetworkingConfig_ConnectionUserData:
		{

			// We only need special handling when modifying a connection
			if ( eScopeType != k_ESteamNetworkingConfig_Connection )
				break;

			// Process the user argument, maybe performing type conversion
			int64 newData;
			if ( !AssignConfigValueTyped( &newData, eDataType, pValue ) )
				return false;

			// Lookup the connection
			ConnectionScopeLock connectionLock;
			CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( HSteamNetConnection( scopeObj ), connectionLock );
			if ( !pConn )
				return false;

			// Set the data, possibly fixing up existing queued messages, etc
			pConn->SetUserData( pConn->m_connectionConfig.m_ConnectionUserData.m_data );
			return true;
		}

	}

	GlobalConfigValueEntry *pEntry = FindConfigValueEntry( eValue );
	if ( pEntry == nullptr )
		return false;

	SteamNetworkingGlobalLock scopeLock( "SetConfigValue" );

	switch ( pEntry->m_eDataType )
	{
		case k_ESteamNetworkingConfig_Int32: return SetConfigValueTyped<int32>( pEntry, eScopeType, scopeObj, eDataType, pValue );
		case k_ESteamNetworkingConfig_Int64: return SetConfigValueTyped<int64>( pEntry, eScopeType, scopeObj, eDataType, pValue );
		case k_ESteamNetworkingConfig_Float: return SetConfigValueTyped<float>( pEntry, eScopeType, scopeObj, eDataType, pValue );
		case k_ESteamNetworkingConfig_String: return SetConfigValueTyped<std::string>( pEntry, eScopeType, scopeObj, eDataType, pValue );
		case k_ESteamNetworkingConfig_Ptr: return SetConfigValueTyped<void *>( pEntry, eScopeType, scopeObj, eDataType, pValue );
	}

	Assert( false );
	return false;
}

ESteamNetworkingGetConfigValueResult CSteamNetworkingUtils::GetConfigValue(
	ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigScope eScopeType,
	intptr_t scopeObj, ESteamNetworkingConfigDataType *pOutDataType,
	void *pResult, size_t *cbResult )
{
	// Take the global lock.
	SteamNetworkingGlobalLock scopeLock( "GetConfigValue" );

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

	switch ( pEntry->m_eDataType )
	{
		case k_ESteamNetworkingConfig_Int32: return GetConfigValueTyped<int32>( pEntry, eScopeType, scopeObj, pResult, cbResult );
		case k_ESteamNetworkingConfig_Int64: return GetConfigValueTyped<int64>( pEntry, eScopeType, scopeObj, pResult, cbResult );
		case k_ESteamNetworkingConfig_Float: return GetConfigValueTyped<float>( pEntry, eScopeType, scopeObj, pResult, cbResult );
		case k_ESteamNetworkingConfig_String: return GetConfigValueTyped<std::string>( pEntry, eScopeType, scopeObj, pResult, cbResult );
		case k_ESteamNetworkingConfig_Ptr: return GetConfigValueTyped<void *>( pEntry, eScopeType, scopeObj, pResult, cbResult );
	}

	Assert( false ); // FIXME
	return k_ESteamNetworkingGetConfigValue_BadValue;
}

static bool BEnumerateConfigValue( const GlobalConfigValueEntry *pVal, bool bEnumerateDevVars )
{
	if ( pVal->m_eDataType == k_ESteamNetworkingConfig_Ptr )
		return false;

	switch  ( pVal->m_eValue )
	{
		// Never enumerate these
		case k_ESteamNetworkingConfig_SymmetricConnect:
		case k_ESteamNetworkingConfig_LocalVirtualPort:
		case k_ESteamNetworkingConfig_ConnectionUserData:
		//case k_ESteamNetworkingConfig_EnumerateDevVars:
			return false;

		// Dev var?
		case k_ESteamNetworkingConfig_IP_AllowWithoutAuth:
		case k_ESteamNetworkingConfig_Unencrypted:
		case k_ESteamNetworkingConfig_SDRClient_FakeClusterPing:
			return bEnumerateDevVars;
	}

	return true;
}

const char *CSteamNetworkingUtils::GetConfigValueInfo( ESteamNetworkingConfigValue eValue, ESteamNetworkingConfigDataType *pOutDataType, ESteamNetworkingConfigScope *pOutScope )
{
	const GlobalConfigValueEntry *pVal = FindConfigValueEntry( eValue );
	if ( pVal == nullptr )
		return nullptr;

	if ( pOutDataType )
		*pOutDataType = pVal->m_eDataType;
	if ( pOutScope )
		*pOutScope = pVal->m_eScope;

	return pVal->m_pszName;
}

ESteamNetworkingConfigValue CSteamNetworkingUtils::IterateGenericEditableConfigValues( ESteamNetworkingConfigValue eCurrent, bool bEnumerateDevVars )
{
	const GlobalConfigValueEntry *p;
	if ( eCurrent == k_ESteamNetworkingConfig_Invalid )
	{
		p = s_vecConfigValueTable[0];
	}
	else
	{
		p = FindConfigValueEntry( eCurrent );
		if ( p )
			p = p->m_pNextEntry;
	}

	while ( p )
	{
		if ( BEnumerateConfigValue( p, bEnumerateDevVars ) )
			return p->m_eValue;
		p = p->m_pNextEntry;
	}
	return k_ESteamNetworkingConfig_Invalid;
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

ESteamNetworkingFakeIPType CSteamNetworkingUtils::GetIPv4FakeIPType( uint32 nIPv4 )
{
	return SteamNetworkingSocketsLib::GetIPv4FakeIPType( nIPv4 );
}

EResult CSteamNetworkingUtils::GetRealIdentityForFakeIP( const SteamNetworkingIPAddr &fakeIP, SteamNetworkingIdentity *pOutRealIdentity )
{
	// Not supported without Steam
	return k_EResultDisabled;
}

ESteamNetworkingFakeIPType CSteamNetworkingUtils::SteamNetworkingIPAddr_GetFakeIPType( const SteamNetworkingIPAddr &addr )
{
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_FAKEIP
		return ::SteamNetworkingIPAddr_GetFakeIPType( &addr );
	#else
		return k_ESteamNetworkingFakeIPType_NotFake;
	#endif
}

AppId_t CSteamNetworkingUtils::GetAppID()
{
	return m_nAppID;
}

void CSteamNetworkingUtils::TEST_ResetSelf()
{
	m_nAppID = 0;
}

time_t CSteamNetworkingUtils::GetTimeSecure()
{
	// Trusting local user's clock!
	return time(nullptr);
}

const char *CSteamNetworkingUtils::GetBuildString()
{
	#if defined( STEAMNETWORKINGSOCKETS_OPENSOURCE )
		return "opensource " __DATE__ " " __TIME__;
	#elif defined( STEAMNETWORKINGSOCKETS_PARTNER )
		return "partner " __DATE__ " " __TIME__;
	#elif defined( STEAMNETWORKINGSOCKETS_STANDALONELIB )
		return "lib " __DATE__ " " __TIME__;
	#elif defined( STEAMNETWORKINGSOCKETS_STEAMCLIENT )
		return "steam "
		#ifdef BRANCH_MAIN
			"(main) "
		#elif !defined( BRANCH_REL_CLIENT )
			"(branch?) "
		#endif
		__DATE__ " " __TIME__;
	#elif defined( STEAMNETWORKINGSOCKETS_STREAMINGCLIENT )
		return "stream "
		#ifdef BRANCH_MAIN
			"(main) "
		#elif !defined( BRANCH_REL_CLIENT )
			"(branch?) "
		#endif
		__DATE__ " " __TIME__;
	#else
		#error "Huh?"
	#endif
}

const char *CSteamNetworkingUtils::GetPlatformString()
{
	#if defined( NN_NINTENDO_SDK )
		return "nswitch";
	#elif defined( _GAMECORE )
		// Is this right?  This might actually require a system call.
		return "xboxx";
	#elif defined( _STADIA )
		// Not sure if this works.
		return "stadia";
	#elif defined( _XBOX_ONE )
		return "xbone";
	#elif defined( _PS4 )
		return "ps4";
	#elif defined( _PS5 )
		return "ps5";
	#elif defined( TVOS ) || defined( __TVOS__ )
		return "tvos";
	#elif defined( __APPLE__ )
		#if TARGET_OS_TV
			return "tvos";
		#elif TARGET_OS_IPHONE
			return "ios";
		#else
			return "osx";
		#endif
	#elif defined( OSX )
		return "osx";
	#elif defined( ANDROID ) || defined( __ANDROID__ )
		return "android";
	#elif defined( _WINDOWS )
		return "windows";
	#elif defined( LINUX ) || defined( __LINUX__ ) || defined(linux) || defined(__linux) || defined(__linux__)
		return "linux";
	#elif defined( FREEBSD ) || defined( __FreeBSD__ )
		return "freebsd";
	#else
		#error "Unknown platform"
	#endif
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
	SteamNetworkingGlobalLock lock( "GameNetworkingSockets_Init" );

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
	SteamNetworkingGlobalLock lock( "GameNetworkingSockets_Kill" );
	if ( s_pSteamNetworkingSockets )
	{
		s_pSteamNetworkingSockets->Destroy();
		s_pSteamNetworkingSockets = nullptr;
	}
}

STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingSockets *SteamNetworkingSockets_LibV11()
{
	return s_pSteamNetworkingSockets;
}

STEAMNETWORKINGSOCKETS_INTERFACE ISteamNetworkingUtils *SteamNetworkingUtils_LibV4()
{
	static CSteamNetworkingUtils s_utils;
	return &s_utils;
}

#endif
