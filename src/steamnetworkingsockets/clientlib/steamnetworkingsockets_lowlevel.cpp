//====== Copyright Valve Corporation, All rights reserved. ====================

#if defined( _MSC_VER ) && ( _MSC_VER <= 1800 )
	#pragma warning( disable: 4244 )
	// 1>C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\include\chrono(749): warning C4244: '=' : conversion from '__int64' to 'time_t', possible loss of data (steamnetworkingsockets_lowlevel.cpp)
#endif

#ifdef __GNUC__
	// src/public/tier0/basetypes.h:104:30: error: assuming signed overflow does not occur when assuming that (X + c) < X is always false [-Werror=strict-overflow]
	// current steamrt:scout gcc "g++ (SteamRT 4.8.4-1ubuntu15~12.04+steamrt1.2+srt1) 4.8.4" requires this at the top due to optimizations
	#pragma GCC diagnostic ignored "-Wstrict-overflow"
#endif

#include <thread>
#include <mutex>
#include <atomic>

#include "steamnetworkingsockets_lowlevel.h"
#include "../steamnetworkingsockets_internal.h"
#include <vstdlib/random.h>
#include <tier1/utlpriorityqueue.h>
#include <tier1/utllinkedlist.h>
#include "crypto.h"

// Ugggggggggg MSVC VS2013 STL bug: try_lock_for doesn't actually respect the timeout, it always ends up using an infinite timeout.
// And even in 2015, the code is calling the timer and to convert a relative time to an absolute time, and waiting until that time,
// which then calls the timer again, and subtracts it back off....It's really bad. Just go directly to the underlying Win32
// primitives
#ifdef _MSC_VER
	#define MSVC_STL_MUTEX_WORKAROUND
	#include <Windows.h>
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace SteamNetworkingSocketsLib {

int g_nSteamDatagramSocketBufferSize = 128*1024;

/// Global lock for all local data structures
#ifdef MSVC_STL_MUTEX_WORKAROUND
	HANDLE s_hSteamDatagramTransportMutex = INVALID_HANDLE_VALUE; 
#else
	static std::recursive_timed_mutex s_steamDatagramTransportMutex;
#endif
int SteamDatagramTransportLock::s_nLocked;
static SteamNetworkingMicroseconds s_usecWhenLocked;
static std::thread::id s_threadIDLockOwner;
static int64 s_usecLongLockWarningThreshold;

void SteamDatagramTransportLock::OnLocked()
{
	++s_nLocked;
	if ( s_nLocked == 1 )
	{
		s_usecWhenLocked = SteamNetworkingSockets_GetLocalTimestamp();
		s_threadIDLockOwner = std::this_thread::get_id();

		// By default, complain if we hold the lock for more than this long
		s_usecLongLockWarningThreshold = 20*1000;
	}
}

void SteamDatagramTransportLock::Lock()
{
	#ifdef MSVC_STL_MUTEX_WORKAROUND
		if ( s_hSteamDatagramTransportMutex == INVALID_HANDLE_VALUE ) // This is not actually threadsafe, but we assume that client code will call (and wait for the return of) some Init() call before invoking any API calls.
			s_hSteamDatagramTransportMutex = ::CreateMutex( NULL, FALSE, NULL );
		DWORD res = ::WaitForSingleObject( s_hSteamDatagramTransportMutex, INFINITE );
		Assert( res == WAIT_OBJECT_0 );
	#else
		s_steamDatagramTransportMutex.lock();
	#endif
	OnLocked();
}

bool SteamDatagramTransportLock::TryLock( int msTimeout )
{
	#ifdef MSVC_STL_MUTEX_WORKAROUND
		if ( ::WaitForSingleObject( s_hSteamDatagramTransportMutex, msTimeout ) != WAIT_OBJECT_0 )
			return false;
	#else
		if ( !s_steamDatagramTransportMutex.try_lock_for( std::chrono::milliseconds( msTimeout ) ) )
			return false;
	#endif
	OnLocked();
	return true;
}

void SteamDatagramTransportLock::Unlock()
{
	AssertHeldByCurrentThread();
	SteamNetworkingMicroseconds usecElapsedTooLong = 0;
	if ( s_nLocked == 1 )
	{

		// We're about to do the final release.  How long did we hold the lock?
		usecElapsedTooLong = SteamNetworkingSockets_GetLocalTimestamp() - s_usecWhenLocked;

		// If that duration is acceptable, then clear it.  We need to check the
		// threshold here because the threshold could change by another thread
		// immediately after we release the lock.  Also, if we're debugging, all bets are
		// off.  They could have hit a breakpoint, and we don't want to create a bunch
		// of confusing spew with spurious asserts
		if ( usecElapsedTooLong < s_usecLongLockWarningThreshold || Plat_IsInDebugSession() )
			usecElapsedTooLong = 0;
	}
	--s_nLocked;
	#ifdef MSVC_STL_MUTEX_WORKAROUND
		DbgVerify( ReleaseMutex( s_hSteamDatagramTransportMutex ) );
	#else
		s_steamDatagramTransportMutex.unlock();
	#endif

	// Yelp if we held the lock for longer than the threshold.
	AssertMsg1( usecElapsedTooLong == 0, "SteamDatagramTransportLock held for %.1fms!", usecElapsedTooLong*1e-3 );
}

void SteamDatagramTransportLock::SetLongLockWarningThresholdMS( int msWarningThreshold )
{
	AssertHeldByCurrentThread();
	SteamNetworkingMicroseconds usecWarningThreshold = SteamNetworkingMicroseconds{msWarningThreshold}*1000;
	if ( s_usecLongLockWarningThreshold < usecWarningThreshold )
		s_usecLongLockWarningThreshold = usecWarningThreshold;
}

void SteamDatagramTransportLock::AssertHeldByCurrentThread()
{
	Assert( s_nLocked > 0 ); // NOTE: This could succeed even if another thread has the lock
	Assert( s_threadIDLockOwner == std::this_thread::get_id() );
}

static void SeedWeakRandomGenerator()
{

	// Seed cheesy random number generator using true source of entropy
	int temp;
	CCrypto::GenerateRandomBlock( &temp, sizeof(temp) );
	WeakRandomSeed( temp );
}

static std::atomic<long long> s_usecTimeLastReturned;

// Start with an offset so that a timestamp of zero is always pretty far in the past.
// But round it up to nice round number, so that looking at timestamps in the debugger
// is easy to read.
const long long k_nInitialTimestampMin = k_nMillion*24*3600*30;
const long long k_nInitialTimestamp = 3000000000000ll;
COMPILE_TIME_ASSERT( 2000000000000ll < k_nInitialTimestampMin );
COMPILE_TIME_ASSERT( k_nInitialTimestampMin < k_nInitialTimestamp );
static std::atomic<long long> s_usecTimeOffset( k_nInitialTimestamp );

static int s_nLowLevelSupportRefCount = 0;

/////////////////////////////////////////////////////////////////////////////
//
// Raw sockets
//
/////////////////////////////////////////////////////////////////////////////

inline IRawUDPSocket::IRawUDPSocket() {}
inline IRawUDPSocket::~IRawUDPSocket() {}

class CRawUDPSocketImpl : public IRawUDPSocket
{
public:
	~CRawUDPSocketImpl()
	{
		closesocket( m_socket );
		#ifdef WIN32
			WSACloseEvent( m_event );
		#endif
	}

	/// Descriptor from the OS
	SOCKET m_socket;

	/// What address families are supported by this socket?
	int m_nAddressFamilies;

	/// Who to notify when we receive a packet on this socket.
	/// This is set to null when we are asked to close the socket.
	CRecvPacketCallback m_callback;

	/// An event that will be set when the socket has data that we can read.
	#ifdef WIN32
		WSAEVENT m_event = INVALID_HANDLE_VALUE;
	#endif

	//// Send a packet, for really realz right now.  (No checking for fake loss or lag.)
	inline bool BReallySendRawPacket( int nChunks, const iovec *pChunks, const netadr_t &adrTo ) const
	{
		Assert( m_socket != INVALID_SOCKET );

		// Convert address to BSD interface
		struct sockaddr_storage destAddress;
		socklen_t addrSize;
		if ( m_nAddressFamilies & k_nAddressFamily_IPv6 )
		{
			addrSize = sizeof(sockaddr_in6);
			adrTo.ToSockadrIPV6( &destAddress );
		}
		else
		{
			addrSize = (socklen_t)adrTo.ToSockadr( &destAddress );
		}

		//const uint8 *pbPkt = (const uint8 *)pPkt;
		//Log_Detailed( LOG_STEAMDATAGRAM_CLIENT, "%4db -> %s %02x %02x %02x %02x %02x ...\n",
		//	cbPkt, CUtlNetAdrRender( adrTo ).String(), pbPkt[0], pbPkt[1], pbPkt[2], pbPkt[3], pbPkt[4] );

		#ifdef WIN32
			// Confirm that iovec and WSABUF are indeed bitwise equivalent
			COMPILE_TIME_ASSERT( sizeof( iovec ) == sizeof( WSABUF ) );
			COMPILE_TIME_ASSERT( offsetof( iovec, iov_len ) == offsetof( WSABUF, len ) );
			COMPILE_TIME_ASSERT( offsetof( iovec, iov_base ) == offsetof( WSABUF, buf ) );

			DWORD numberOfBytesSent;
			int r = WSASendTo(
				m_socket,
				(WSABUF *)pChunks,
				(DWORD)nChunks,
				&numberOfBytesSent,
				0, // flags
				(const sockaddr *)&destAddress,
				addrSize,
				nullptr, // lpOverlapped
				nullptr // lpCompletionRoutine
			);
			return ( r == 0 );
		#else
			msghdr msg;
			msg.msg_name = (sockaddr *)&destAddress;
			msg.msg_namelen = addrSize;
			msg.msg_iov = const_cast<struct iovec *>( pChunks );
			msg.msg_iovlen = nChunks;
			msg.msg_control = nullptr;
			msg.msg_controllen = 0;
			msg.msg_flags = 0;

			int r = ::sendmsg( m_socket, &msg, 0 );
			return ( r >= 0 ); // just check for -1 for error, since we don't want to take the time here to scan the iovec and sum up the expected total number of bytes sent
		#endif
	}
};

/// We don't expect to have enough sockets, and open and close them frequently
/// enough, such that an occasional linear search will kill us.
static CUtlVector<CRawUDPSocketImpl *> s_vecRawSockets;

/// List of raw sockets pending actual destruction.
static CUtlVector<CRawUDPSocketImpl *> s_vecRawSocketsPendingDeletion;

/// Track packets that have fake lag applied and are pending to be sent/received
class CPacketLagger : private IThinker
{
public:
	~CPacketLagger() { Clear(); }

	void LagPacket( bool bSend, const CRawUDPSocketImpl *pSock, const netadr_t &adr, int msDelay, int nChunks, const iovec *pChunks )
	{
		int cbPkt = 0;
		for ( int i = 0 ; i < nChunks ; ++i )
			cbPkt += pChunks[i].iov_len;
		if ( cbPkt > k_cbSteamNetworkingSocketsMaxUDPMsgLen )
		{
			AssertMsg( false, "Tried to lag a packet that w as too big!" );
			return;
		}

		// Make sure we never queue a packet that is queued for destruction!
		if ( pSock->m_socket == INVALID_SOCKET || !pSock->m_callback.m_fnCallback )
		{
			AssertMsg( false, "Tried to lag a packet on a socket that has already been closed and is pending destruction!" );
			return;
		}

		if ( msDelay < 1 )
		{
			AssertMsg( false, "Packet lag time must be positive!" );
			msDelay = 1;
		}

		// Limit to something sane
		msDelay = Min( msDelay, 5000 );
		const SteamNetworkingMicroseconds usecTime = SteamNetworkingSockets_GetLocalTimestamp() + msDelay * 1000;

		// Find the right place to insert the packet.
		LaggedPacket *pkt = nullptr;
		for ( CUtlLinkedList< LaggedPacket >::IndexType_t i = m_list.Head(); i != m_list.InvalidIndex(); i = m_list.Next( i ) )
		{
			if ( usecTime < m_list[ i ].m_usecTime )
			{
				pkt = &m_list[ m_list.InsertBefore( i ) ];
				break;
			}
		}
		if ( pkt == nullptr )
		{
			pkt = &m_list[ m_list.AddToTail() ];
		}
	
		pkt->m_bSend = bSend;
		pkt->m_pSockOwner = pSock;
		pkt->m_adrRemote = adr;
		pkt->m_usecTime = usecTime;
		pkt->m_cbPkt = cbPkt;

		// Gather them into buffer
		char *d = pkt->m_pkt;
		for ( int i = 0 ; i < nChunks ; ++i )
		{
			int cbChunk = pChunks[i].iov_len;
			memcpy( d, pChunks[i].iov_base, cbChunk );
			d += cbChunk;
		}

		Schedule();
	}

	/// Periodic processing
	virtual void Think( SteamNetworkingMicroseconds usecNow ) OVERRIDE
	{

		// Just always process packets in queue order.  This means there could be some
		// weird burst or jankiness if the delay time is changed, but that's OK.
		while ( !m_list.IsEmpty() )
		{
			LaggedPacket &pkt = m_list[ m_list.Head() ];
			if ( pkt.m_usecTime > usecNow )
				break;

			// Make sure socket is still in good shape.
			const CRawUDPSocketImpl *pSock = pkt.m_pSockOwner;
			if ( pSock->m_socket == INVALID_SOCKET || !pSock->m_callback.m_fnCallback )
			{
				AssertMsg( false, "Lagged packet remains in queue after socket destroyed or queued for destruction!" );
			}
			else
			{

				// Sending, or receiving?
				if ( pkt.m_bSend )
				{
					iovec temp;
					temp.iov_len = pkt.m_cbPkt;
					temp.iov_base = pkt.m_pkt;
					pSock->BReallySendRawPacket( 1, &temp, pkt.m_adrRemote );
				}
				else
				{
					// Copy data out of queue into local variables, just in case a
					// packet is queued while we're in this function.  We don't want
					// our list to shift in memory, and the pointer we pass to the
					// caller to dangle.
					char temp[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
					memcpy( temp, pkt.m_pkt, pkt.m_cbPkt );
					netadr_t adr( pkt.m_adrRemote );
					pSock->m_callback( temp, pkt.m_cbPkt, adr );
				}
			}
			m_list.RemoveFromHead();
		}

		Schedule();
	}

	/// Nuke everything
	void Clear()
	{
		m_list.RemoveAll();
		IThinker::ClearNextThinkTime();
	}

	/// Called when we're about to destroy a socket
	void AboutToDestroySocket( const CRawUDPSocketImpl *pSock )
	{
		// Just do a dumb linear search.  This list should be empty in
		// production situations, and socket destruction is relatively rare,
		// so its not worth making this complicated.
		int idx = m_list.Head();
		while ( idx != m_list.InvalidIndex() )
		{
			int idxNext = m_list.Next( idx );
			if ( m_list[idx].m_pSockOwner == pSock )
				m_list.Remove( idx );
			idx = idxNext;
		}

		Schedule();
	}

private:

	/// Set the next think time as appropriate
	void Schedule()
	{
		if ( m_list.IsEmpty() )
			ClearNextThinkTime();
		else
			SetNextThinkTime( m_list[ m_list.Head() ].m_usecTime );
	}

	struct LaggedPacket
	{
		bool m_bSend; // true for outbound, false for inbound
		const CRawUDPSocketImpl *m_pSockOwner;
		netadr_t m_adrRemote;
		SteamNetworkingMicroseconds m_usecTime; /// Time when it should be sent or received
		int m_cbPkt;
		char m_pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	};
	CUtlLinkedList<LaggedPacket> m_list;

};

static CPacketLagger s_packetLagQueue;

/// Object used to wake our background thread efficiently
#ifdef WIN32
	static HANDLE s_hEventWakeThread = INVALID_HANDLE_VALUE;
#else
	static SOCKET s_hSockWakeThreadRead = INVALID_SOCKET;
	static SOCKET s_hSockWakeThreadWrite = INVALID_SOCKET;
#endif

static std::thread *s_pThreadSteamDatagram = nullptr;

static void WakeSteamDatagramThread()
{
	#ifdef _WIN32
		if ( s_hEventWakeThread != INVALID_HANDLE_VALUE )
			SetEvent( s_hEventWakeThread );
	#else
		if ( s_hSockWakeThreadWrite != INVALID_SOCKET )
		{
			char buf[1] = {0};
			send( s_hSockWakeThreadWrite, buf, 1, 0 );
		}
	#endif
}

bool IRawUDPSocket::BSendRawPacket( const void *pPkt, int cbPkt, const netadr_t &adrTo ) const
{
	iovec temp;
	temp.iov_len = cbPkt;
	temp.iov_base = (void *)pPkt;
	return BSendRawPacketGather( 1, &temp, adrTo );
}

bool IRawUDPSocket::BSendRawPacketGather( int nChunks, const iovec *pChunks, const netadr_t &adrTo ) const
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	// Silently ignore a request to send a packet anytime we're in the process of shutting down the system
	if ( !g_bWantThreadRunning )
		return true;

	// Fake loss?
	if ( RandomBoolWithOdds( g_Config_FakePacketLoss_Send.Get() ) )
		return true;

	const CRawUDPSocketImpl *self = static_cast<const CRawUDPSocketImpl *>( this );

	// Fake lag?
	int32 nPacketFakeLagTotal = g_Config_FakePacketLag_Send.Get();

	// Check for simulating random packet reordering
	if ( RandomBoolWithOdds( g_Config_FakePacketReorder_Send.Get() ) )
	{
		nPacketFakeLagTotal += g_Config_FakePacketReorder_Time.Get();
	}

	// Check for simulating random packet duplication
	if ( RandomBoolWithOdds( g_Config_FakePacketDup_Send.Get() ) )
	{
		int32 nDupLag = nPacketFakeLagTotal + WeakRandomInt( 0, g_Config_FakePacketDup_TimeMax.Get() );
		nDupLag = std::max( 1, nDupLag );
		s_packetLagQueue.LagPacket( true, self, adrTo, nDupLag, nChunks, pChunks );
	}

	// Lag the original packet?
	if ( nPacketFakeLagTotal > 0 )
	{
		s_packetLagQueue.LagPacket( true, self, adrTo, nPacketFakeLagTotal, nChunks, pChunks );
		return true;
	}

	// Now really send it
	return self->BReallySendRawPacket( nChunks, pChunks, adrTo );
}

void IRawUDPSocket::Close()
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();
	CRawUDPSocketImpl *self = static_cast<CRawUDPSocketImpl *>( this );

	/// Clear the callback, to ensure that no further callbacks will be executed.
	/// This marks the socket as pending destruction.
	Assert( self->m_callback.m_fnCallback );
	self->m_callback.m_fnCallback = nullptr;
	Assert( self->m_socket != INVALID_SOCKET );

	DbgVerify( s_vecRawSockets.FindAndFastRemove( self ) );
	DbgVerify( !s_vecRawSocketsPendingDeletion.FindAndFastRemove( self ) );
	s_vecRawSocketsPendingDeletion.AddToTail( self );

	// Clean up lagged packets, if any
	s_packetLagQueue.AboutToDestroySocket( self );

	// Make sure we don't delay doing this too long
	if ( s_pThreadSteamDatagram && s_pThreadSteamDatagram->get_id() != std::this_thread::get_id() )
	{
		WakeSteamDatagramThread();
	}
	else
	{
		ProcessPendingDestroyClosedRawUDPSockets();
	}
}

static SOCKET OpenUDPSocketBoundToSockAddr( const void *sockaddr, size_t len, SteamDatagramErrMsg &errMsg, int *pnIPv6AddressFamilies )
{
	unsigned int opt;

	const sockaddr_in *inaddr = (const sockaddr_in *)sockaddr;

	// Select socket type.  For linux, use the "close on exec" flag, so that the
	// socket will not be inherited by any child process that we spawn.
	int sockType = SOCK_DGRAM;
	#ifdef LINUX
		sockType |= SOCK_CLOEXEC;
	#endif

	// Try to create a UDP socket using the specified family
	SOCKET sock = socket( inaddr->sin_family, sockType, IPPROTO_UDP );
	if ( sock == INVALID_SOCKET )
	{
		V_sprintf_safe( errMsg, "socket() call failed.  Error code 0x%08x.", GetLastSocketError() );
		return INVALID_SOCKET;
	}

	// We always use nonblocking IO
	opt = 1;
	if ( ioctlsocket( sock, FIONBIO, (unsigned long*)&opt ) == -1 )
	{
		V_sprintf_safe( errMsg, "Failed to set socket nonblocking mode.  Error code 0x%08x.", GetLastSocketError() );
		closesocket( sock );
		return INVALID_SOCKET;
	}

	// Set buffer sizes
	opt = g_nSteamDatagramSocketBufferSize;
	if ( setsockopt( sock, SOL_SOCKET, SO_SNDBUF, (char *)&opt, sizeof(opt) ) )
	{
		V_sprintf_safe( errMsg, "Failed to set socket send buffer size.  Error code 0x%08x.", GetLastSocketError() );
		closesocket( sock );
		return INVALID_SOCKET;
	}
	opt = g_nSteamDatagramSocketBufferSize;
	if ( setsockopt( sock, SOL_SOCKET, SO_RCVBUF, (char *)&opt, sizeof(opt) ) == -1 )
	{
		V_sprintf_safe( errMsg, "Failed to set socket recv buffer size.  Error code 0x%08x.", GetLastSocketError() );
		closesocket( sock );
		return INVALID_SOCKET;
	}

	// Handle IP v6 dual stack?
	if ( pnIPv6AddressFamilies )
	{

		// Enable dual stack?
		opt = ( *pnIPv6AddressFamilies == k_nAddressFamily_IPv6 ) ? 1 : 0;
		if ( setsockopt( sock, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&opt, sizeof( opt ) ) != 0 )
		{
			if ( *pnIPv6AddressFamilies == k_nAddressFamily_IPv6 )
			{
				// Spew a warning, but continue
				SpewWarning( "Failed to set socket for IPv6 only (IPV6_V6ONLY=1).  Error code 0x%08X.  Continuing anyway.\n", GetLastSocketError() );
			}
			else
			{
				// Dual stack required, or only requested?
				if ( *pnIPv6AddressFamilies == k_nAddressFamily_DualStack )
				{
					V_sprintf_safe( errMsg, "Failed to set socket for dual stack (IPV6_V6ONLY=0).  Error code 0x%08X.", GetLastSocketError() );
					closesocket( sock );
					return INVALID_SOCKET;
				}

				// Let caller know we're IPv6 only, and spew about this.
				SpewWarning( "Failed to set socket for dual stack (IPV6_V6ONLY=0).  Error code 0x%08X.  Continuing using IPv6 only!\n", GetLastSocketError() );
				*pnIPv6AddressFamilies = k_nAddressFamily_IPv6;
			}
		}
		else
		{
			// Tell caller what they've got
			*pnIPv6AddressFamilies = opt ? k_nAddressFamily_IPv6 : k_nAddressFamily_DualStack;
		}
	}

	// Bind it to specific desired port and/or interfaces
	if ( bind( sock, (struct sockaddr *)sockaddr, (socklen_t)len ) == -1 )
	{
		V_sprintf_safe( errMsg, "Failed to bind socket.  Error code 0x%08X.", GetLastSocketError() );
		closesocket( sock );
		return INVALID_SOCKET;
	}

	// All good
	return sock;
}

static CRawUDPSocketImpl *OpenRawUDPSocketInternal( CRecvPacketCallback callback, SteamDatagramErrMsg &errMsg, const SteamNetworkingIPAddr *pAddrLocal, int *pnAddressFamilies )
{
	// Creating a socket *should* be fast, but sometimes the OS might need to do some work.
	// We shouldn't do this too often, give it a little extra time.
	SteamDatagramTransportLock::SetLongLockWarningThresholdMS( 100 );

	// Make sure have been initialized
	if ( s_nLowLevelSupportRefCount <= 0 )
	{
		V_strcpy_safe( errMsg, "Internal order of operations bug.  Can't create socket, because low level systems not initialized" );
		AssertMsg( false, errMsg );
		return nullptr;
	}

	// Supply defaults
	int nAddressFamilies = pnAddressFamilies ? *pnAddressFamilies : k_nAddressFamily_Auto;
	SteamNetworkingIPAddr addrLocal;
	if ( pAddrLocal )
		addrLocal = *pAddrLocal;
	else
		addrLocal.Clear();

	// Check that the request makes sense
	if ( addrLocal.IsIPv4() )
	{
		// Only IPv4 family allowed, don't even try IPv6
		if ( nAddressFamilies == k_nAddressFamily_Auto )
		{
			nAddressFamilies = k_nAddressFamily_IPv4;
		}
		else if ( nAddressFamilies != k_nAddressFamily_IPv4 )
		{
			V_strcpy_safe( errMsg, "Invalid address family request when binding to IPv4 address" );
			return nullptr;
		}
	}
	else if ( addrLocal.IsIPv6AllZeros() )
	{
		// We can try IPv6 dual stack, and fallback to IPv4 if requested.
		// Just make sure they didn't request a totally bogus value
		if ( nAddressFamilies == 0 )
		{
			V_strcpy_safe( errMsg, "Invalid address families" );
			return nullptr;
		}
	}
	else
	{
		// Only IPv6 family allowed, cannot try IPv4
		if ( nAddressFamilies == k_nAddressFamily_Auto )
		{
			nAddressFamilies = k_nAddressFamily_IPv6;
		}
		else if ( nAddressFamilies != k_nAddressFamily_IPv6 )
		{
			V_strcpy_safe( errMsg, "Invalid address family request when binding to IPv6 address" );
			return nullptr;
		}
	}

	// Try IPv6?
	SOCKET sock = INVALID_SOCKET;
	if ( nAddressFamilies & k_nAddressFamily_IPv6 )
	{
		sockaddr_in6 address6;
		memset( &address6, 0, sizeof(address6) );
		address6.sin6_family = AF_INET6;
		memcpy( address6.sin6_addr.s6_addr, addrLocal.m_ipv6, 16 );
		address6.sin6_port = BigWord( addrLocal.m_port );

		// Try to get socket
		int nIPv6AddressFamilies = nAddressFamilies;
		sock = OpenUDPSocketBoundToSockAddr( &address6, sizeof(address6), errMsg, &nIPv6AddressFamilies );

		if ( sock == INVALID_SOCKET )
		{
			// Allowing fallback to IPv4?
			if ( nAddressFamilies != k_nAddressFamily_Auto )
				return nullptr;

			// Continue below, we'll try IPv4
		}
		else
		{
			nAddressFamilies = nIPv6AddressFamilies;
		}
	}

	// Try IPv4?
	if ( sock == INVALID_SOCKET )
	{
		Assert( nAddressFamilies & k_nAddressFamily_IPv4 ); // Otherwise, we should have already failed above

		sockaddr_in address4;
		memset( &address4, 0, sizeof(address4) );
		address4.sin_family = AF_INET;
		address4.sin_addr.s_addr = BigDWord( addrLocal.GetIPv4() );
		address4.sin_port = BigWord( addrLocal.m_port );

		// Try to get socket
		sock = OpenUDPSocketBoundToSockAddr( &address4, sizeof(address4), errMsg, nullptr );

		// If we failed, well, we have no other options left to try.
		if ( sock == INVALID_SOCKET )
			return nullptr;

		// We re IPv4 only
		nAddressFamilies = k_nAddressFamily_IPv4;
	}

	// Read back address we actually bound to.
	sockaddr_storage addrBound;
	socklen_t cbAddress = sizeof(addrBound);
	if ( getsockname( sock, (struct sockaddr *)&addrBound, &cbAddress ) != 0 )
	{
		V_sprintf_safe( errMsg, "getsockname failed.  Error code 0x%08X.", GetLastSocketError() );
		closesocket( sock );
		return nullptr;
	}
	if ( addrBound.ss_family == AF_INET )
	{
		const sockaddr_in *boundaddr4 = (const sockaddr_in *)&addrBound;
		addrLocal.SetIPv4( BigDWord( boundaddr4->sin_addr.s_addr ), BigWord( boundaddr4->sin_port ) );
	}
	else if ( addrBound.ss_family == AF_INET6 )
	{
		const sockaddr_in6 *boundaddr6 = (const sockaddr_in6 *)&addrBound;
		addrLocal.SetIPv6( boundaddr6->sin6_addr.s6_addr, BigWord( boundaddr6->sin6_port ) );
	}
	else
	{
		Assert( false );
		V_sprintf_safe( errMsg, "getsockname returned address with unexpected family %d", addrBound.ss_family );
		closesocket( sock );
		return nullptr;
	}

	// Allocate a bookkeeping structure
	CRawUDPSocketImpl *pSock = new CRawUDPSocketImpl;
	pSock->m_socket = sock;
	pSock->m_boundAddr = addrLocal;
	pSock->m_callback = callback;
	pSock->m_nAddressFamilies = nAddressFamilies;

	// On windows, create an event used to poll efficiently
	#ifdef _WIN32
		pSock->m_event = WSACreateEvent();
		if ( WSAEventSelect( pSock->m_socket, pSock->m_event, FD_READ ) != 0 )
		{
			delete pSock;
			V_sprintf_safe( errMsg, "WSACreateEvent() or WSAEventSelect() failed.  Error code 0x%08X.", GetLastSocketError() );
			return nullptr;
		}
	#endif

	// Add to master list.  (Hopefully we usually won't have that many.)
	s_vecRawSockets.AddToTail( pSock );

	// Wake up background thread so we can start receiving packets on this socket immediately
	WakeSteamDatagramThread();

	// Give back info on address families
	if ( pnAddressFamilies )
		*pnAddressFamilies = nAddressFamilies;

	// Give them something they can use
	return pSock;
}

IRawUDPSocket *OpenRawUDPSocket( CRecvPacketCallback callback, SteamDatagramErrMsg &errMsg, SteamNetworkingIPAddr *pAddrLocal, int *pnAddressFamilies )
{
	return OpenRawUDPSocketInternal( callback, errMsg, pAddrLocal, pnAddressFamilies );
}

/// Poll all of our sockets, and dispatch the packets received.
/// This will return true if we own the lock, or false if we detected
/// a shutdown request and bailed without re-squiring the lock.
static bool PollRawUDPSockets( int nMaxTimeoutMS )
{
	// This should only ever be called from our one thread proc,
	// and we assume that it will have locked the lock exactly once.
	SteamDatagramTransportLock::AssertHeldByCurrentThread();
	Assert( SteamDatagramTransportLock::s_nLocked == 1 );

	#ifdef _WIN32
		HANDLE *pEvents = (HANDLE*)alloca( sizeof(HANDLE) * (s_vecRawSockets.Count()+1) );
		int nEvents = 0;
	#else
		pollfd *pPollFDs = (pollfd*)alloca( sizeof(pollfd) * (s_vecRawSockets.Count()+1) ); 
		int nPollFDs = 0;
	#endif

	CRawUDPSocketImpl **pSocketsToPoll = (CRawUDPSocketImpl **)alloca( sizeof(CRawUDPSocketImpl *) * s_vecRawSockets.Count() ); 
	int nSocketsToPoll = 0;

	for ( CRawUDPSocketImpl *pSock: s_vecRawSockets )
	{
		// Should be totally valid at this point
		Assert( pSock->m_callback.m_fnCallback );
		Assert( pSock->m_socket != INVALID_SOCKET );

		pSocketsToPoll[ nSocketsToPoll++ ] = pSock;

		#ifdef _WIN32
			pEvents[ nEvents++ ] = pSock->m_event;
		#else
			pollfd *p = &pPollFDs[ nPollFDs++ ];
			p->fd = pSock->m_socket;
			p->events = POLLRDNORM;
			p->revents = 0;
		#endif
	}

	#ifdef _WIN32
		Assert( s_hEventWakeThread != NULL && s_hEventWakeThread != INVALID_HANDLE_VALUE );
		pEvents[ nEvents++ ] = s_hEventWakeThread;
	#else
		Assert( s_hSockWakeThreadRead != INVALID_SOCKET );
		pollfd *p = &pPollFDs[ nPollFDs++ ];
		p->fd = s_hSockWakeThreadRead;
		p->events = POLLRDNORM;
		p->revents = 0;
	#endif

	// Release lock while we're asleep
	SteamDatagramTransportLock::Unlock();

	// Shutdown request?
	if ( !g_bWantThreadRunning )
		return false; // ABORT THREAD

	// Wait for data on one of the sockets, or for us to be asked to wake up
	#if defined( WIN32 )
		DWORD nWaitResult = WaitForMultipleObjects( nEvents, pEvents, FALSE, nMaxTimeoutMS );
	#else
		poll( pPollFDs, nPollFDs, nMaxTimeoutMS );
	#endif

	SteamNetworkingMicroseconds usecStartedLocking = SteamNetworkingSockets_GetLocalTimestamp();
	for (;;)
	{

		// Shutdown request?  We've potentially been waiting a long time.
		// Don't attempt to grab the lock again if we know we want to shutdown,
		// that is just a waste of time.
		if ( !g_bWantThreadRunning )
			return false;

		// Try to acquire the lock.  But don't wait forever, in case the other thread has the lock
		// and then makes a shutdown request while we're waiting on the lock here.
		if ( SteamDatagramTransportLock::TryLock( 250 ) )
			break;

		// The only time this really should happen is a relatively rare race condition
		// where the main thread is trying to shut us down.  (Or while debugging.)
		// However, note that try_lock_for is permitted to "fail" spuriously, returning
		// false even if no other thread holds the lock.  (For performance reasons.)
		// So we check how long we have actually been waiting.
		SteamNetworkingMicroseconds usecElapsed = SteamNetworkingSockets_GetLocalTimestamp() - usecStartedLocking;
		AssertMsg1( usecElapsed < 50*1000 || !g_bWantThreadRunning || Plat_IsInDebugSession(), "SDR service thread gave up on lock after waiting %dms.  This directly adds to delay of processing of network packets!", int( usecElapsed/1000 ) );
	}

	// Recv socket data from any sockets that might have data, and execute the callbacks.
	char buf[ k_cbSteamNetworkingSocketsMaxUDPMsgLen + 1024 ];
#ifdef _WIN32
	// Note that we assume we aren't polling a ton of sockets here.  We do at least skip ahead
	// to the first socket with data, based on the return value of WaitForMultipleObjects.  But
	// then we will check all sockets later in the array.
	//
	// Note that if we get a wake request, the event has already been reset, because we used an auto-reset event
	for ( int idx = nWaitResult - WAIT_OBJECT_0 ; (unsigned)idx < (unsigned)nSocketsToPoll ; ++idx )
	{

		CRawUDPSocketImpl *pSock = pSocketsToPoll[ idx ];

		// Check if this socket has anything, and clear the event
		WSANETWORKEVENTS wsaEvents;
		if ( WSAEnumNetworkEvents( pSock->m_socket, pSock->m_event, &wsaEvents ) != 0 )
		{
			AssertMsg1( false, "WSAEnumNetworkEvents failed.  Error code %08x", WSAGetLastError() );
			continue;
		}
		if ( !(wsaEvents.lNetworkEvents & FD_READ) )
			continue;
#else
	for ( int idx = 0 ; idx < nPollFDs ; ++idx )
	{
		if ( !( pPollFDs[ idx ].revents & POLLRDNORM ) )
			continue;
		if ( idx >= nSocketsToPoll )
		{
			// It's a wake request.  Pull a single packet out of the queue.
			// We want one wake request to always result in exactly one wake up.
			// Wake request are relatively rare, and we don't want to skip any
			// or combine them.  That would result in complicated race conditions
			// where we stay asleep a lot longer than we should.
			Assert( pPollFDs[idx].fd == s_hSockWakeThreadRead );
			::recv( s_hSockWakeThreadRead, buf, sizeof(buf), 0 );
			continue;
		}
		CRawUDPSocketImpl *pSock = pSocketsToPoll[ idx ];
#endif

		// Drain the socket.  But if the callback gets cleared, that
		// indicates that the socket is pending destruction and is
		// logically closed to the calling code.
		while ( pSock->m_callback.m_fnCallback )
		{
			if ( !g_bWantThreadRunning )
				return true; // current thread owns the lock

			sockaddr_storage from;
			socklen_t fromlen = sizeof(from);
			int ret = ::recvfrom( pSock->m_socket, buf, sizeof( buf ), 0, (sockaddr *)&from, &fromlen );

			// Negative value means nothing more to read.
			//
			// NOTE 1: We're not checking the cause of failure.  Usually it would be "EWOULDBLOCK",
			// meaning no more data.  However if there was some socket error (i.e. somebody did something
			// to reset the network stack, etc) we could make the code more robust by detecting this.
			// It would require us plumbing through this failure somehow, and all we have here is a callback
			// for processing packets.  Probably not worth the effort to handle this relatively common case.
			// It will just appear to the app that the cord is cut on this socket.
			//
			// NOTE 2: 0 byte datagram is possible, and in this case recvfrom will return 0.
			// (But all of our protocols enforce a minimum packet size, so if we get a zero byte packet,
			// it's a bogus.  We could drop it here but let's send it through the normal mechanism to
			// be handled/reported in the same way as any other bogus packet.)
			if ( ret < 0 )
				break;

			// Check for simulating random packet loss
			if ( RandomBoolWithOdds( g_Config_FakePacketLoss_Recv.Get() ) )
				continue;

			netadr_t adr;
			adr.SetFromSockadr( &from );

			// If we're dual stack, convert mapped IPv4 back to ordinary IPv4
			if ( pSock->m_nAddressFamilies == k_nAddressFamily_DualStack )
				adr.BConvertMappedToIPv4();

			int32 nPacketFakeLagTotal = g_Config_FakePacketLag_Recv.Get();

			// Check for simulating random packet reordering
			if ( RandomBoolWithOdds( g_Config_FakePacketReorder_Recv.Get() ) )
			{
				nPacketFakeLagTotal += g_Config_FakePacketReorder_Time.Get();
			}

			// Check for simulating random packet duplication
			if ( RandomBoolWithOdds( g_Config_FakePacketDup_Recv.Get() ) )
			{
				int32 nDupLag = nPacketFakeLagTotal + WeakRandomInt( 0, g_Config_FakePacketDup_TimeMax.Get() );
				nDupLag = std::max( 1, nDupLag );
				iovec temp;
				temp.iov_len = ret;
				temp.iov_base = buf;
				s_packetLagQueue.LagPacket( false, pSock, adr, nDupLag, 1, &temp );
			}

			// Check for simulating lag
			if ( nPacketFakeLagTotal > 0 )
			{
				iovec temp;
				temp.iov_len = ret;
				temp.iov_base = buf;
				s_packetLagQueue.LagPacket( false, pSock, adr, nPacketFakeLagTotal, 1, &temp );
			}
			else
			{

				//const uint8 *pbPkt = (const uint8 *)buf;
				//Log_Detailed( LOG_STEAMDATAGRAM_CLIENT, "%s -> %4db %02x %02x %02x %02x %02x ...\n",
				//	CUtlNetAdrRender( adr ).String(), ret, pbPkt[0], pbPkt[1], pbPkt[2], pbPkt[3], pbPkt[4] );

				pSock->m_callback( buf, ret, adr );
			}
		}
	}

	// We retained the lock
	return true;
}

void ProcessPendingDestroyClosedRawUDPSockets()
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	for ( CRawUDPSocketImpl *pSock: s_vecRawSocketsPendingDeletion )
	{
		Assert( pSock->m_callback.m_fnCallback == nullptr );
		delete pSock;
	}

	s_vecRawSocketsPendingDeletion.RemoveAll();
}

/////////////////////////////////////////////////////////////////////////////
//
// Periodic processing
//
/////////////////////////////////////////////////////////////////////////////

struct ThinkerLess
{
	bool operator()( const IThinker *a, const IThinker *b ) const
	{
		return a->GetLatestThinkTime() > b->GetLatestThinkTime();
	}
};
class ThinkerSetIndex
{
public:
	static void SetIndex( IThinker *p, int idx ) { p->m_queueIndex = idx; }
};

static CUtlPriorityQueue<IThinker*,ThinkerLess,ThinkerSetIndex> s_queueThinkers;

IThinker::IThinker()
: m_usecNextThinkTimeTarget( k_nThinkTime_Never )
, m_usecNextThinkTimeEarliest( k_nThinkTime_Never )
, m_usecNextThinkTimeLatest( k_nThinkTime_Never )
, m_queueIndex( -1 )
{
}

IThinker::~IThinker()
{
	ClearNextThinkTime();
}

#ifdef __GNUC__
	// older steamrt:scout gcc requires this also, probably getting confused by unbalanced push/pop
	#pragma GCC diagnostic ignored "-Wstrict-overflow"
#endif

void IThinker::SetNextThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime, int nSlackMS )
{
	Assert( usecTargetThinkTime > 0 );

	// Clearing it?
	if ( usecTargetThinkTime == k_nThinkTime_Never )
	{
		if ( m_queueIndex >= 0 )
		{
			Assert( s_queueThinkers.Element( m_queueIndex ) == this );
			s_queueThinkers.RemoveAt( m_queueIndex );
			Assert( m_queueIndex == -1 );
		}

		m_usecNextThinkTimeTarget = k_nThinkTime_Never;
		m_usecNextThinkTimeEarliest = k_nThinkTime_Never;
		m_usecNextThinkTimeLatest = k_nThinkTime_Never;
		return;
	}

	if ( nSlackMS == 0 )
	{
		Assert( false );
		nSlackMS = +1;
	}
	SteamNetworkingMicroseconds usecLimit = usecTargetThinkTime + nSlackMS*1000;

	// Save current time when the next thinker wants service
	SteamNetworkingMicroseconds usecNextWake = ( s_queueThinkers.Count() > 0 ) ? s_queueThinkers.ElementAtHead()->GetLatestThinkTime() : k_nThinkTime_Never;

	// Not currently scheduled?
	if ( m_queueIndex < 0 )
	{
		Assert( m_usecNextThinkTimeTarget == k_nThinkTime_Never );
		m_usecNextThinkTimeTarget = usecTargetThinkTime;
		m_usecNextThinkTimeEarliest = Min( usecTargetThinkTime, usecLimit );
		m_usecNextThinkTimeLatest = Max( usecTargetThinkTime, usecLimit );
		s_queueThinkers.Insert( this );
	}
	else
	{

		// We're already scheduled.
		Assert( s_queueThinkers.Element( m_queueIndex ) == this );
		Assert( m_usecNextThinkTimeTarget != k_nThinkTime_Never );

		// Set the new schedule time
		m_usecNextThinkTimeTarget = usecTargetThinkTime;
		m_usecNextThinkTimeEarliest = Min( usecTargetThinkTime, usecLimit );
		m_usecNextThinkTimeLatest = Max( usecTargetThinkTime, usecLimit );

		// And update our position in the queue
		s_queueThinkers.RevaluateElement( m_queueIndex );
	}

	// Check that we know our place
	Assert( m_queueIndex >= 0 );
	Assert( s_queueThinkers.Element( m_queueIndex ) == this );

	// Do we need service before we were previously schedule to wake up?
	// If so, wake the thread now so that it can redo its schedule work
	if ( m_usecNextThinkTimeLatest < usecNextWake )
		WakeSteamDatagramThread();
}

void IThinker::EnsureMinThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime, int nSlackMS )
{
	Assert( usecTargetThinkTime < k_nThinkTime_Never );
	Assert( nSlackMS != 0 );

	if ( nSlackMS == 0 )
	{
		Assert( false );
		nSlackMS = +1;
	}

	SteamNetworkingMicroseconds usecLimit = usecTargetThinkTime + nSlackMS*1000;
	SteamNetworkingMicroseconds usecNextThinkTimeEarliest = Min( usecTargetThinkTime, usecLimit );
	SteamNetworkingMicroseconds usecNextThinkTimeLatest = Max( usecTargetThinkTime, usecLimit );

	// Save current time when the next thinker wants service
	SteamNetworkingMicroseconds usecNextWake = ( s_queueThinkers.Count() > 0 ) ? s_queueThinkers.ElementAtHead()->GetLatestThinkTime() : k_nThinkTime_Never;

	// Not currently scheduled?
	if ( m_queueIndex < 0 )
	{
		Assert( m_usecNextThinkTimeTarget == k_nThinkTime_Never );
		m_usecNextThinkTimeTarget = usecTargetThinkTime;
		m_usecNextThinkTimeEarliest = usecNextThinkTimeEarliest;
		m_usecNextThinkTimeLatest = usecNextThinkTimeLatest;
		s_queueThinkers.Insert( this );
	}
	else
	{

		// We're already scheduled.
		Assert( s_queueThinkers.Element( m_queueIndex ) == this );
		Assert( m_usecNextThinkTimeTarget != k_nThinkTime_Never );

		Assert( m_usecNextThinkTimeEarliest <= m_usecNextThinkTimeTarget );
		Assert( m_usecNextThinkTimeTarget <= m_usecNextThinkTimeLatest );
		Assert( m_usecNextThinkTimeEarliest+1000 <= m_usecNextThinkTimeLatest );

		// No change needed?
		if ( usecTargetThinkTime >= m_usecNextThinkTimeTarget && usecNextThinkTimeLatest >= m_usecNextThinkTimeLatest )
			return;

		// Push the scheduled time up
		m_usecNextThinkTimeTarget = Min( m_usecNextThinkTimeTarget, usecTargetThinkTime );
		m_usecNextThinkTimeEarliest = Min( m_usecNextThinkTimeEarliest, usecNextThinkTimeEarliest );
		m_usecNextThinkTimeLatest = Min( m_usecNextThinkTimeLatest, usecNextThinkTimeLatest );

		Assert( m_usecNextThinkTimeEarliest <= m_usecNextThinkTimeTarget );
		Assert( m_usecNextThinkTimeTarget <= m_usecNextThinkTimeLatest );

		// Make sure our window has enough tolerance.  If this code kicks in,
		// the sequence of requests has made it a bit ambiguous exactly
		// which request "earlier" than the other.
		if ( m_usecNextThinkTimeEarliest+1000 > m_usecNextThinkTimeLatest )
			m_usecNextThinkTimeEarliest = m_usecNextThinkTimeLatest-1000;

		// And update our position in the queue
		s_queueThinkers.RevaluateElement( m_queueIndex );
	}

	// Check that we know our place
	Assert( m_queueIndex >= 0 );
	Assert( s_queueThinkers.Element( m_queueIndex ) == this );

	// Do we need service before we were previously schedule to wake up?
	// If so, wake the thread now so that it can redo its schedule work
	if ( m_usecNextThinkTimeLatest < usecNextWake )
		WakeSteamDatagramThread();
}

void ProcessThinkers()
{

	// Until the queue is empty
	while ( s_queueThinkers.Count() > 0 )
	{

		// Grab the head element
		IThinker *pNextThinker = s_queueThinkers.ElementAtHead();

		// Refetch timestamp each time.  The reason is that certain thinkers
		// may pass through to other systems (e.g. fake lag) that fetch the time.
		// If we don't update the time here, that code may have used the newer
		// timestamp (e.g. to mark when a packet was received) and then
		// in our next iteration, we will use an older timestamp to process
		// a thinker.
		SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

		// Scheduled too far in the future?  Note that
		// there could be other items in the queue with
		// a greater tolerance, that we could go ahead
		// and process now.  However, since they are later
		// in the queue we know that we can wait to wake
		// them up.
		if ( pNextThinker->GetEarliestThinkTime() >= usecNow )
		{
			// Keep waiting
			break;
		}

		// Go ahead and clear his think time now and remove him
		// from the heap.  He needs to schedule a new think time
		// if heeds service again.  For thinkers that need frequent
		// service, removing them and then re-inserting them when
		// they reschedule is a bit of extra work that could be
		// optimized by trying to not remove them now, but adjusting
		// them once we know when they want to think.  But this
		// is probably just a bit too complicated for the expected
		// benefit.  If the number of total Thinkers is relatively
		// small (which it probably will be), the heap operations
		// are probably negligible.
		pNextThinker->ClearNextThinkTime();

		// Execute callback.  (Note: this could result
		// in self-destruction or essentially any change
		// to the rest of the queue.)
		pNextThinker->Think( usecNow );
	}
}

/////////////////////////////////////////////////////////////////////////////
//
// Service thread
//
/////////////////////////////////////////////////////////////////////////////

std::atomic<bool> g_bWantThreadRunning;

static void SteamDatagramThreadProc()
{

	// This is an "interrupt" thread.  When an incoming packet raises the event,
	// we need to take priority above normal threads and wake up immediately
	// to process the packet.  We should be asleep most of the time waiting
	// for packets to arrive.
	#if defined(_WIN32)
	DbgVerify( SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_HIGHEST ) );
	#endif

	#if defined(_WIN32) && !defined(GNU_COMPILER)
		typedef struct tagTHREADNAME_INFO
		{
			DWORD dwType;
			LPCSTR szName;
			DWORD dwThreadID;
			DWORD dwFlags;
		} THREADNAME_INFO;


		THREADNAME_INFO info;
		{
			info.dwType = 0x1000;
			info.szName = "SteamDatagram";
			info.dwThreadID = GetCurrentThreadId();
			info.dwFlags = 0;
		}
		__try
		{
			RaiseException( 0x406D1388, 0, sizeof(info)/sizeof(DWORD), (ULONG_PTR*)&info );
		}
		__except(EXCEPTION_CONTINUE_EXECUTION)
		{
		}

	#else
		// Help!  Really we should do this for all platforms.  Seems it's not
		// totally straightforward the correct way to do this on Linux.
	#endif

	// In the loop, we will always hold global lock while we're awake.
	// So go ahead and acquire it now.  But watch out for a race condition
	// where we want to shut down immediately after starting the thread
	do
	{
		if ( !g_bWantThreadRunning )
			return;
	} while ( !SteamDatagramTransportLock::TryLock( 10 ) );

	// Random number generator may be per thread!  Make sure and see it for
	// this thread, if so
	SeedWeakRandomGenerator();

	// Keep looping until we're asked to terminate
	for (;;)
	{
		SteamDatagramTransportLock::AssertHeldByCurrentThread(); // We should own the lock
		Assert( SteamDatagramTransportLock::s_nLocked == 1 ); // exactly once

		// Figure out how long to sleep
		int msWait = 100;
		if ( s_queueThinkers.Count() > 0 )
		{
			IThinker *pNextThinker = s_queueThinkers.ElementAtHead();

			// Calc wait time to wake up as late as possible,
			// routed up to the nearest millisecond.
			SteamNetworkingMicroseconds usecNextWakeTime = pNextThinker->GetLatestThinkTime();
			SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
			int64 usecUntilNextThinkTime = usecNextWakeTime - usecNow;

			if ( usecNow >= pNextThinker->GetEarliestThinkTime() )
			{
				// Earliest thinker in the queue is ready to go now.
				// There is no point in going to sleep
				msWait = 0;
			}
			else if ( usecUntilNextThinkTime <= 1000 )
			{
				// Less than 1ms until time to wake up?  But not yet reached the
				// time for him to think?  This guy is asking for more resolution
				// than we can provide.  Just squawk and sleep 1ms.  (We *do* need to
				// sleep some amount of time, because this guy isn't going to get
				// ejected from the queue until the earliest think time comes around,
				// so we'll just be in an infinite loop complaining about the same thing
				// over and over.)
				msWait = 1;
				AssertMsg( false, "Thinker requested submillisecond wait time precision." );
			}
			else
			{

				// Set wake time to wake up just at the last moment.
				msWait = usecUntilNextThinkTime/1000;
				Assert( msWait >= 1 );
				usecNextWakeTime = usecNow + msWait*1000;
				Assert( usecNextWakeTime <= pNextThinker->GetLatestThinkTime() );
				Assert( usecNextWakeTime >= pNextThinker->GetEarliestThinkTime() );

				// If we assume that the actual time we spend waiting might be
				// up to 1ms shorter or longer than we request (in reality it
				// could be much worse!), then attempting to wake up
				// "at the last minute" might actually be too late.  If we can,
				// back up to wake up 1ms earlier.  The reality is that if a thinker
				// is requesting 1ms precision, they might just be asking for more than
				// the underlying operating system can deliver.
				if ( usecNextWakeTime+1000 > pNextThinker->GetLatestThinkTime() && usecNextWakeTime-1000 < pNextThinker->GetEarliestThinkTime() )
				{
					--msWait;
					usecNextWakeTime -= 1000;
				}

				// But don't ever sleep for too long, just in case.  This timeout
				// is long enough so that if we have a bug where we really need to
				// be explicitly waking the thread for good perf, we will notice
				// the delay.  But not so long that a bug in some rare 
				// shutdown race condition (or the like) will be catastrophic
				msWait = Min( msWait, 5000 );
			}
		}

		// Poll sockets
		if ( !PollRawUDPSockets( msWait ) )
		{
			// Shutdown request, and they did NOT re-aquire the lock
			break;
		}

		SteamDatagramTransportLock::AssertHeldByCurrentThread(); // We should own the lock
		Assert( SteamDatagramTransportLock::s_nLocked == 1 ); // exactly once

		// Shutdown request?
		if ( !g_bWantThreadRunning )
		{
			SteamDatagramTransportLock::Unlock();
			break;
		}

		// Check for periodic processing
		ProcessThinkers();

		// Close any sockets pending delete, if we discarded a server
		// We can close the sockets safely now, because we know we're
		// not polling on them and we know we hold the lock
		ProcessPendingDestroyClosedRawUDPSockets();
	}
}

static bool BEnsureSteamDatagramThreadRunning( SteamDatagramErrMsg &errMsg )
{

	// Make sure and call time function at least once
	// just before we start up our thread, so we don't lurch
	// on our first reading after the thread is running and
	// take action to correct this.
	SteamNetworkingSockets_GetLocalTimestamp();

	if ( s_pThreadSteamDatagram )
	{
		Assert( g_bWantThreadRunning );
		return true;
	}
	Assert( !g_bWantThreadRunning );

	// Create thread communication object used to wake the background thread efficiently
	// in case a thinker priority changes or we want to shutdown
	#ifdef WIN32
		Assert( s_hEventWakeThread == INVALID_HANDLE_VALUE );

		// Note: Using "automatic reset" style event.
		s_hEventWakeThread = CreateEvent( nullptr, false, false, nullptr );
		if ( s_hEventWakeThread == NULL || s_hEventWakeThread == INVALID_HANDLE_VALUE )
		{
			s_hEventWakeThread = INVALID_HANDLE_VALUE;
			V_sprintf_safe( errMsg, "CreateEvent() call failed.  Error code 0x%08x.", GetLastError() );
			return false;
		}
	#else
		Assert( s_hSockWakeThreadRead == INVALID_SOCKET );
		Assert( s_hSockWakeThreadWrite == INVALID_SOCKET );
		int sockType = SOCK_DGRAM;
		#ifdef LINUX
			sockType |= SOCK_CLOEXEC;
		#endif
		int sock[2];
		if ( socketpair( AF_LOCAL, sockType, 0, sock ) != 0 )
		{
			V_sprintf_safe( errMsg, "socketpair() call failed.  Error code 0x%08x.", GetLastSocketError() );
			return false;
		}

		s_hSockWakeThreadRead = sock[0];
		s_hSockWakeThreadWrite = sock[1];

		unsigned int opt;
		opt = 1;
		if ( ioctlsocket( s_hSockWakeThreadRead, FIONBIO, (unsigned long*)&opt ) != 0 )
		{
			AssertMsg1( false, "Failed to set socket nonblocking mode.  Error code 0x%08x.", GetLastSocketError() );
		}
		opt = 1;
		if ( ioctlsocket( s_hSockWakeThreadWrite, FIONBIO, (unsigned long*)&opt ) != 0 )
		{
			AssertMsg1( false, "Failed to set socket nonblocking mode.  Error code 0x%08x.", GetLastSocketError() );
		}
	#endif

	// Create the thread and start socket processing
	g_bWantThreadRunning = true;

	s_pThreadSteamDatagram = new std::thread( SteamDatagramThreadProc );

	return true;
}

static void StopSteamDatagramThread()
{

	// Should only be called while we have the lock
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	// We don't want the thread running
	g_bWantThreadRunning = false;

	if ( s_pThreadSteamDatagram )
	{
		// Send wake up signal
		WakeSteamDatagramThread();

		// Wait for thread to finish
		s_pThreadSteamDatagram->join();

		// Clean up
		delete s_pThreadSteamDatagram;
		s_pThreadSteamDatagram = nullptr;
	}

	// Destory wake communication objects
	#ifdef WIN32
		if ( s_hEventWakeThread != INVALID_HANDLE_VALUE )
		{
			CloseHandle( s_hEventWakeThread );
			s_hEventWakeThread = INVALID_HANDLE_VALUE;
		}
	#else
		if ( s_hSockWakeThreadRead != INVALID_SOCKET )
		{
			closesocket( s_hSockWakeThreadRead );
			s_hSockWakeThreadRead = INVALID_SOCKET;
		}
		if ( s_hSockWakeThreadRead != INVALID_SOCKET )
		{
			closesocket( s_hSockWakeThreadWrite );
			s_hSockWakeThreadWrite = INVALID_SOCKET;
		}
	#endif

	// Make sure we don't leak any sockets.
	ProcessPendingDestroyClosedRawUDPSockets();
}

/////////////////////////////////////////////////////////////////////////////
//
// Bound sockets / socket sharing
//
/////////////////////////////////////////////////////////////////////////////

class CDedicatedBoundSocket : public IBoundUDPSocket
{
private:
	inline virtual ~CDedicatedBoundSocket() {}
public:
	CDedicatedBoundSocket( IRawUDPSocket *pRawSock, const netadr_t &adr )
	: IBoundUDPSocket( pRawSock, adr ) {}

	CRecvPacketCallback m_callback;

	virtual void Close() OVERRIDE
	{
		m_pRawSock->Close();
		m_pRawSock = nullptr;
		delete this;
	}
};

static void DedicatedBoundSocketCallback( const void *pPkt, int cbPkt, const netadr_t &adrFrom, CDedicatedBoundSocket *pSock )
{

	// Make sure that it's from the guy we are supposed to be talking to.
	if ( adrFrom != pSock->GetRemoteHostAddr() )
	{
		// Packets from random internet hosts happen all the time,
		// especially on a LAN where all sorts of people have broadcast
		// discovery protocols.  So this probably isn't a bug or a problem.
		SpewVerbose( "Ignoring stray packet from %s received on port %d.  Should only be talking to %s on that port.\n", CUtlNetAdrRender( adrFrom ).String(), pSock->GetRawSock()->m_boundAddr.m_port, CUtlNetAdrRender( pSock->GetRemoteHostAddr() ).String() );
		return;
	}

	// Now execute their callback.
	// Passing the address in this context is sort of superfluous.
	// Should we use a different signature here so that the user
	// of our API doesn't write their own useless code to check
	// the from address?
	pSock->m_callback( pPkt, cbPkt, adrFrom );
}

IBoundUDPSocket *OpenUDPSocketBoundToHost( const netadr_t &adrRemote, CRecvPacketCallback callback, SteamDatagramErrMsg &errMsg )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	// Select local address to use.
	// Since we know the remote host, let's just always use a single-stack socket
	// with the specified family
	int nAddressFamilies = ( adrRemote.GetType() == NA_IPV6 ) ? k_nAddressFamily_IPv6 : k_nAddressFamily_IPv4;

	// Create a socket, bind it to the desired local address
	CDedicatedBoundSocket *pTempContext = nullptr; // don't yet know the context
	CRawUDPSocketImpl *pRawSock = OpenRawUDPSocketInternal( CRecvPacketCallback( DedicatedBoundSocketCallback, pTempContext ), errMsg, nullptr, &nAddressFamilies );
	if ( !pRawSock )
		return nullptr;

	// Return wrapper interface that can only talk to this remote host
	CDedicatedBoundSocket *pBoundSock = new CDedicatedBoundSocket( pRawSock, adrRemote );
	pRawSock->m_callback.m_pContext = pBoundSock;
	pBoundSock->m_callback = callback;

	return pBoundSock;
}

bool CreateBoundSocketPair( CRecvPacketCallback callback1, CRecvPacketCallback callback2, IBoundUDPSocket **ppOutSockets, SteamDatagramErrMsg &errMsg )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	SteamNetworkingIPAddr localAddr;

	// Create two socket UDP sockets, bound to (IPv4) loopback IP, but allow OS to choose ephemeral port
	CRawUDPSocketImpl *pRawSock[2];
	uint32 nLocalIP = 0x7f000001; // 127.0.0.1
	CDedicatedBoundSocket *pTempContext = nullptr; // don't yet know the context
	localAddr.SetIPv4( nLocalIP, 0 );
	pRawSock[0] = OpenRawUDPSocketInternal( CRecvPacketCallback( DedicatedBoundSocketCallback, pTempContext ), errMsg, &localAddr, nullptr );
	if ( !pRawSock[0] )
		return false;
	localAddr.SetIPv4( nLocalIP, 0 );
	pRawSock[1] = OpenRawUDPSocketInternal( CRecvPacketCallback( DedicatedBoundSocketCallback, pTempContext ), errMsg, &localAddr, nullptr );
	if ( !pRawSock[1] )
	{
		delete pRawSock[0];
		return false;
	}

	// Return wrapper interfaces that can only talk to each other
	for ( int i = 0 ; i < 2 ; ++i )
	{
		auto s = new CDedicatedBoundSocket( pRawSock[i], netadr_t( nLocalIP, pRawSock[1-i]->m_boundAddr.m_port ) );
		pRawSock[i]->m_callback.m_pContext = s;
		s->m_callback = (i == 0 ) ? callback1 : callback2;
		ppOutSockets[i] = s;
	}

	return true;
}

CSharedSocket::CSharedSocket()
{
	m_pRawSock = nullptr;
}

CSharedSocket::~CSharedSocket()
{
	Kill();
}

void CSharedSocket::CallbackRecvPacket( const void *pPkt, int cbPkt, const netadr_t &adrFrom, CSharedSocket *pSock )
{
	// Locate the client
	int idx = pSock->m_mapRemoteHosts.Find( adrFrom );

	// Select the callback to invoke, ether client-specific, or the default
	const CRecvPacketCallback &callback = ( idx == pSock->m_mapRemoteHosts.InvalidIndex() ) ? pSock->m_callbackDefault : pSock->m_mapRemoteHosts[ idx ]->m_callback;

	// Execute the callback
	callback( pPkt, cbPkt, adrFrom );
}

bool CSharedSocket::BInit( const SteamNetworkingIPAddr &localAddr, CRecvPacketCallback callbackDefault, SteamDatagramErrMsg &errMsg )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	Kill();

	SteamNetworkingIPAddr bindAddr = localAddr;
	m_pRawSock = OpenRawUDPSocket( CRecvPacketCallback( CallbackRecvPacket, this ), errMsg, &bindAddr, nullptr );
	if ( m_pRawSock == nullptr )
		return false;

	m_callbackDefault = callbackDefault;
	return true;
}

void CSharedSocket::Kill()
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	m_callbackDefault.m_fnCallback = nullptr;
	if ( m_pRawSock )
	{
		m_pRawSock->Close();
		m_pRawSock = nullptr;
	}
	FOR_EACH_HASHMAP( m_mapRemoteHosts, idx )
	{
		CloseRemoteHostByIndex( idx );
	}
}

void CSharedSocket::CloseRemoteHostByIndex( int idx )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	delete m_mapRemoteHosts[ idx ];
	m_mapRemoteHosts[idx] = nullptr; // just for grins
	m_mapRemoteHosts.RemoveAt( idx );
}

IBoundUDPSocket *CSharedSocket::AddRemoteHost( const netadr_t &adrRemote, CRecvPacketCallback callback )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	if ( m_mapRemoteHosts.HasElement( adrRemote ) )
	{
		AssertMsg1( false, "Already talking to %s on this shared socket, cannot add another remote host!", CUtlNetAdrRender( adrRemote ).String() );
		return nullptr;
	}
	RemoteHost *pRemoteHost = new RemoteHost( m_pRawSock, adrRemote );
	pRemoteHost->m_pOwner = this;
	pRemoteHost->m_callback = callback;
	m_mapRemoteHosts.Insert( adrRemote, pRemoteHost );

	return pRemoteHost;
}

void CSharedSocket::RemoteHost::Close()
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	int idx = m_pOwner->m_mapRemoteHosts.Find( m_adr );
	if ( idx == m_pOwner->m_mapRemoteHosts.InvalidIndex() || m_pOwner->m_mapRemoteHosts[idx] != this )
	{
		AssertMsg( false, "CSharedSocket client table corruption!" );
		delete this;
	}
	else
	{
		m_pOwner->CloseRemoteHostByIndex( idx );
	}
}

/////////////////////////////////////////////////////////////////////////////
//
// Spew
//
/////////////////////////////////////////////////////////////////////////////

SteamNetworkingMicroseconds g_usecLastRateLimitSpew;
ESteamNetworkingSocketsDebugOutputType g_eSteamDatagramDebugOutputDetailLevel;
static FSteamNetworkingSocketsDebugOutput s_pfnDebugOutput = nullptr;

void ReallySpewType( ESteamNetworkingSocketsDebugOutputType eType, const char *pMsg, ... )
{
	// Save callback.  Paranoia for unlikely but possible race condition,
	// if we spew from more than one place in our code and stuff changes
	// while we are formatting.
	FSteamNetworkingSocketsDebugOutput pfnDebugOutput = s_pfnDebugOutput;

	// Filter, just in case.  (We really shouldn't get here, though.)
	if ( !pfnDebugOutput || eType > g_eSteamDatagramDebugOutputDetailLevel )
		return;
	
	// Do the formatting
	char buf[ 2048 ];
	va_list ap;
	va_start( ap, pMsg );
	V_vsprintf_safe( buf, pMsg, ap );
	va_end( ap );

	// Gah, some, but not all, of our code has newlines on the end
	V_StripTrailingWhitespaceASCII( buf );

	// Invoke callback
	pfnDebugOutput( eType, buf );
}

#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
static SpewRetval_t SDRSpewFunc( SpewType_t type, tchar const *pMsg )
{
	V_StripTrailingWhitespaceASCII( const_cast<tchar*>( pMsg ) );

	switch ( type )
	{
		case SPEW_LOG:
		case SPEW_INPUT:
			// No idea what these are, so....
			// |
			// V
		case SPEW_MESSAGE:
			if ( s_pfnDebugOutput && g_eSteamDatagramDebugOutputDetailLevel >= k_ESteamNetworkingSocketsDebugOutputType_Msg )
				s_pfnDebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Msg, pMsg );
			break;

		case SPEW_WARNING:
			if ( s_pfnDebugOutput && g_eSteamDatagramDebugOutputDetailLevel >= k_ESteamNetworkingSocketsDebugOutputType_Warning )
				s_pfnDebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Warning, pMsg );
			break;

		case SPEW_ASSERT:
			if ( s_pfnDebugOutput && g_eSteamDatagramDebugOutputDetailLevel >= k_ESteamNetworkingSocketsDebugOutputType_Error )
				s_pfnDebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Bug, pMsg );

			// Ug, for some reason this is crashing, because it's trying to generate a breakpoint
			// even when it's not being run under the debugger.  Probably the best thing to do is just rely
			// on the app hook asserting on an error condition.
			//return SPEW_DEBUGGER;
			break;

		case SPEW_ERROR:
			if ( s_pfnDebugOutput && g_eSteamDatagramDebugOutputDetailLevel >= k_ESteamNetworkingSocketsDebugOutputType_Error )
				s_pfnDebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Error, pMsg );
			return SPEW_ABORT;

		case SPEW_BOLD_MESSAGE:
			if ( s_pfnDebugOutput && g_eSteamDatagramDebugOutputDetailLevel >= k_ESteamNetworkingSocketsDebugOutputType_Important )
				s_pfnDebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Important, pMsg );
	}
	
	return SPEW_CONTINUE;
}
#endif

bool BSteamNetworkingSocketsLowLevelAddRef( SteamDatagramErrMsg &errMsg )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	// First time init?
	if ( s_nLowLevelSupportRefCount == 0 )
	{
		CCrypto::Init();

		// Give us a extra time here.  This is a one-time init function and the OS might
		// need to load up libraries and stuff.
		SteamDatagramTransportLock::SetLongLockWarningThresholdMS( 500 );

		// Init sockets
		#ifdef _WIN32
			WSAData wsaData;
			if ( ::WSAStartup( MAKEWORD(2, 2), &wsaData ) != 0 ) 
			{
				V_strcpy_safe( errMsg, "WSAStartup failed" );
				return false;
			}
		#endif

		// Latch Steam codebase's logging system so we get spew and asserts
		#ifdef STEAMNETWORKINGSOCKETS_STANDALONELIB
			SpewOutputFunc( SDRSpewFunc );
		#endif

		// Make sure random number generator is seeded
		SeedWeakRandomGenerator();
	}

	//extern void KludgePrintPublicKey();
	//KludgePrintPublicKey();

	++s_nLowLevelSupportRefCount;

	// Fire up the thread
	if ( !BEnsureSteamDatagramThreadRunning( errMsg ) )
	{
		SteamNetworkingSocketsLowLevelDecRef();
		return false;
	}

	return true;
}

void SteamNetworkingSocketsLowLevelDecRef()
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();
	Assert( s_nLowLevelSupportRefCount > 0 );

	// Last user is now done?
	--s_nLowLevelSupportRefCount;
	if ( s_nLowLevelSupportRefCount > 0 )
		return;

	// Give us a extra time here.  This is a one-time shutdown function.
	// There is a potential race condition / deadlock with the service thread,
	// that might cause us to have to wait for it to timeout.  And the OS
	// might need to do stuff when we close a bunch of sockets (and WSACleanup)
	SteamDatagramTransportLock::SetLongLockWarningThresholdMS( 500 );

	if ( s_vecRawSockets.IsEmpty() )
	{
		s_vecRawSockets.Purge();
	}
	else
	{
		AssertMsg( false, "Trying to close low level socket support, but we still have sockets open!" );
	}

	// Shutdown the thread
	StopSteamDatagramThread();

	// Make sure we actually destroy socket objects.  It's safe to do so now.
	ProcessPendingDestroyClosedRawUDPSockets();

	Assert( s_vecRawSocketsPendingDeletion.IsEmpty() );
	s_vecRawSocketsPendingDeletion.Purge();

	// Nuke sockets
	#ifdef _WIN32
		::WSACleanup();
	#endif
}

#ifdef DBGFLAG_VALIDATE
void SteamNetworkingSocketsLowLevelValidate( CValidator &validator )
{
	ValidateRecursive( s_vecRawSockets );
	ValidateObj( s_queueThinkers );
}
#endif

void SteamNetworkingSockets_SetDebugOutputFunction( ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc )
{
	if ( pfnFunc && eDetailLevel > k_ESteamNetworkingSocketsDebugOutputType_None )
	{
		SteamNetworkingSocketsLib::s_pfnDebugOutput = pfnFunc;
		SteamNetworkingSocketsLib::g_eSteamDatagramDebugOutputDetailLevel = ESteamNetworkingSocketsDebugOutputType( eDetailLevel );
	}
	else
	{
		SteamNetworkingSocketsLib::s_pfnDebugOutput = nullptr;
		SteamNetworkingSocketsLib::g_eSteamDatagramDebugOutputDetailLevel = k_ESteamNetworkingSocketsDebugOutputType_None;
	}
}

SteamNetworkingMicroseconds SteamNetworkingSockets_GetLocalTimestamp()
{
	SteamNetworkingMicroseconds usecResult;
	long long usecLastReturned;
	for (;;)
	{
		// Fetch values into locals (probably registers)
		usecLastReturned = SteamNetworkingSocketsLib::s_usecTimeLastReturned;
		long long usecOffset = SteamNetworkingSocketsLib::s_usecTimeOffset;

		// Read raw timer
		uint64 usecRaw = Plat_USTime();

		// Add offset to get value in "SteamNetworkingMicroseconds" time
		usecResult = usecRaw + usecOffset;

		// How much raw timer time (presumed to be wall clock time) has elapsed since
		// we read the timer?
		SteamNetworkingMicroseconds usecElapsed = usecResult - usecLastReturned;
		Assert( usecElapsed >= 0 ); // Our raw timer function is not monotonic!  We assume this never happens!
		const SteamNetworkingMicroseconds k_usecMaxTimestampDelta = k_nMillion; // one second
		if ( usecElapsed <= k_usecMaxTimestampDelta )
		{
			// Should be the common case - only a relatively small of time has elapsed
			break;
		}
		if ( !SteamNetworkingSocketsLib::g_bWantThreadRunning )
		{
			// We don't have any expectation that we should be updating the timer frequently,
			// so  a big jump in the value just means they aren't calling it very often
			break;
		}

		// NOTE: We should only rarely get here, and probably as a result of running under the debugger

		// Adjust offset so that delta between timestamps is limited
		long long usecNewOffset = usecOffset - ( usecElapsed - k_usecMaxTimestampDelta );
		usecResult = usecRaw + usecNewOffset;

		// Save the new offset.
		if ( SteamNetworkingSocketsLib::s_usecTimeOffset.compare_exchange_strong( usecOffset, usecNewOffset ) )
			break;

		// Race condition which should be extremely rare.  Some other thread changed the offset, in the time
		// between when we fetched it and now.  (So, a really small race window!)  Just start all over from
		// the beginning.
	}

	// Save the last value returned.  Unless another thread snuck in there while we were busy.
	// If so, that's OK.
	SteamNetworkingSocketsLib::s_usecTimeLastReturned.compare_exchange_strong( usecLastReturned, usecResult );

	return usecResult;
}

} // namespace SteamNetworkingSocketsLib

