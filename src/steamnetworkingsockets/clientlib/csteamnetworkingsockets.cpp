//====== Copyright Valve Corporation, All rights reserved. ====================

#include "csteamnetworkingsockets.h"
#include "steamnetworkingsockets_lowlevel.h"
#include "steamnetworkingsockets_connections.h"
#include "steamnetworkingsockets_udp.h"
#include "crypto.h"

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
#include <steam/steamnetworkingsockets.h>
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

DEFINE_GLOBAL_CONFIGVAL( float, FakePacketLoss_Send, 0.0f );
DEFINE_GLOBAL_CONFIGVAL( float, FakePacketLoss_Recv, 0.0f );
DEFINE_GLOBAL_CONFIGVAL( int32, FakePacketLag_Send, 0 );
DEFINE_GLOBAL_CONFIGVAL( int32, FakePacketLag_Recv, 0 );
DEFINE_GLOBAL_CONFIGVAL( float, FakePacketReorder_Send, 0.0f );
DEFINE_GLOBAL_CONFIGVAL( float, FakePacketReorder_Recv, 0.0f );
DEFINE_GLOBAL_CONFIGVAL( int32, FakePacketReorder_Time, 15 );
DEFINE_GLOBAL_CONFIGVAL( float, FakePacketDup_Send, 0.0f );
DEFINE_GLOBAL_CONFIGVAL( float, FakePacketDup_Recv, 0.0f );
DEFINE_GLOBAL_CONFIGVAL( int32, FakePacketDup_TimeMax, 10 );

DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, TimeoutInitial, 10000 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, TimeoutConnected, 10000 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, SendBufferSize, 512*1024 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, SendRateMin, 128*1024 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, SendRateMax, 1024*1024 );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, NagleTime, 5000 );
#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
	// We don't have a trusted third party, so allow this by default.
	DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, IP_AllowWithoutAuth, 1 );
#else
	DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, IP_AllowWithoutAuth, 0 );
#endif
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_AckRTT, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_PacketDecode, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_Message, k_ESteamNetworkingSocketsDebugOutputType_Everything );
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_PacketGaps, k_ESteamNetworkingSocketsDebugOutputType_Debug );
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SDR
DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, LogLevel_P2PRendezvous, k_ESteamNetworkingSocketsDebugOutputType_Verbose );
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

static CSteamNetworkListenSocketBase *GetListenSocketByHandle( HSteamListenSocket sock )
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
{
	m_connectionConfig.Init( nullptr );
}

CSteamNetworkingSockets::~CSteamNetworkingSockets()
{
	SteamDatagramTransportLock scopeLock;
	KillBase();
}

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

	// Dummy AppID.  (Zero is a reserved value, don't use that.)
	// If we want to be able to interop with the Steam code,
	// we're going to need a way to set this, probably.
	m_nAppID = 1;

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

time_t CSteamNetworkingSockets::GetTimeSecure()
{
	// Trusting local user's clock!
	return time(nullptr);
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
	if ( m_connectionConfig.m_IP_AllowWithoutAuth.Get() == 0 )
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
	CSteamNetworkListenSocketBase *pSock = GetListenSocketByHandle( hSocket );
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

EResult CSteamNetworkingSockets::SendMessageToConnection( HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags )
{
	SteamDatagramTransportLock scopeLock;
	CSteamNetworkConnectionBase *pConn = GetConnectionByHandle( hConn );
	if ( !pConn )
		return k_EResultInvalidParam;
	return pConn->APISendMessageToConnection( pData, cbData, nSendFlags );
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
	CSteamNetworkListenSocketBase *pSock = GetListenSocketByHandle( hSocket );
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
	CSteamNetworkListenSocketBase *pSock = GetListenSocketByHandle( hSocket );
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

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkingUtils
//
/////////////////////////////////////////////////////////////////////////////

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
	return AssignConfigValueTyped( pVal, eDataType, pArg );
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

	SteamDatagramTransportLock scopeLock;

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

	GlobalConfigValueEntry *pEntry = FindConfigValueEntry( eValue );
	if ( pEntry == nullptr )
		return k_ESteamNetworkingGetConfigValue_BadValue;

	if ( pOutDataType )
		*pOutDataType = pEntry->m_eDataType;

	SteamDatagramTransportLock scopeLock;

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
		if ( pVal->m_pNextEntry )
			*pOutNextValue = pVal->m_pNextEntry->m_eValue;
		else
			*pOutNextValue = k_ESteamNetworkingConfig_Invalid;
	}

	return true;
}

ESteamNetworkingConfigValue CSteamNetworkingUtils::GetFirstConfigValue()
{
	EnsureConfigValueTableInitted();
	return s_vecConfigValueTable[0]->m_eValue;
}


void CSteamNetworkingUtils::SteamNetworkingIPAddr_ToString( const SteamNetworkingIPAddr &addr, char *buf, size_t cbBuf, bool bWithPort )
{
	SteamAPI_SteamNetworkingIPAddr_ToString( &addr, buf, cbBuf, bWithPort );
}

bool CSteamNetworkingUtils::SteamNetworkingIPAddr_ParseString( SteamNetworkingIPAddr *pAddr, const char *pszStr )
{
	return SteamAPI_SteamNetworkingIPAddr_ParseString( pAddr, pszStr );
}

void CSteamNetworkingUtils::SteamNetworkingIdentity_ToString( const SteamNetworkingIdentity &identity, char *buf, size_t cbBuf )
{
	return SteamAPI_SteamNetworkingIdentity_ToString( identity, buf, cbBuf );
}

bool CSteamNetworkingUtils::SteamNetworkingIdentity_ParseString( SteamNetworkingIdentity *pIdentity, const char *pszStr )
{
	return SteamAPI_SteamNetworkingIdentity_ParseString( pIdentity, sizeof(SteamNetworkingIdentity), pszStr );
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
	SteamDatagramTransportLock lock;

	// Already initted?
	if ( s_pSteamNetworkingSockets )
	{
		AssertMsg( false, "GameNetworkingSockets_init called multiple times?" );
		return true;
	}

	// Init basic functionality
	CSteamNetworkingSockets *pSteamNetworkingSockets = new CSteamNetworkingSockets;
	if ( !pSteamNetworkingSockets->BInitGameNetworkingSockets( pIdentity, errMsg ) )
	{
		delete pSteamNetworkingSockets;
		return false;
	}

	s_pSteamNetworkingSockets = pSteamNetworkingSockets;
	return true;
}

STEAMNETWORKINGSOCKETS_INTERFACE void GameNetworkingSockets_Kill()
{
	SteamDatagramTransportLock lock;
	if ( s_pSteamNetworkingSockets )
	{
		delete s_pSteamNetworkingSockets;
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
