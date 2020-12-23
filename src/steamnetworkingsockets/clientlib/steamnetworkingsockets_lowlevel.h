//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_LOWLEVEL_H
#define STEAMNETWORKINGSOCKETS_LOWLEVEL_H
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <steam/steamnetworkingtypes.h>
#include <tier1/netadr.h>
#include <tier1/utlhashmap.h>
#include "../steamnetworkingsockets_internal.h"

// Comment this in to enable Windows event tracing
//#ifdef _WINDOWS
//	#define STEAMNETWORKINGSOCKETS_ENABLE_ETW
//#endif

struct iovec;

namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// Low level sockets
//
/////////////////////////////////////////////////////////////////////////////

/// Store the callback and its context together
class CRecvPacketCallback
{
public:
	/// Prototype of the callback
	typedef void (*FCallbackRecvPacket)( const void *pPkt, int cbPkt, const netadr_t &adrFrom, void *pContext );

	/// Default constructor sets stuff to null
	inline CRecvPacketCallback() : m_fnCallback( nullptr ), m_pContext( nullptr ) {}

	/// A template constructor so you can use type safe context and avoid messy casting
	template< typename T >
	inline CRecvPacketCallback( void (*fnCallback)( const void *pPkt, int cbPkt, const netadr_t &adrFrom, T context ), T context )
	: m_fnCallback ( reinterpret_cast< FCallbackRecvPacket>( fnCallback ) )
	, m_pContext( reinterpret_cast< void * >( context ) )
	{
		COMPILE_TIME_ASSERT( sizeof(T) == sizeof(void*) );
	}

	FCallbackRecvPacket m_fnCallback;
	void *m_pContext;

	/// Shortcut notation to execute the callback
	inline void operator()( const void *pPkt, int cbPkt, const netadr_t &adrFrom ) const
	{
		if ( m_fnCallback )
			m_fnCallback( pPkt, cbPkt, adrFrom, m_pContext );
	}
};

/// Interface object for a low-level Berkeley socket.  We always use non-blocking, UDP sockets.
class IRawUDPSocket
{
public:
	/// A thin wrapper around ::sendto
	///
	/// Packets sent through this method are subject to fake loss (steamdatagram_fakepacketloss_send),
	/// lag (steamdatagram_fakepacketlag_send and steamdatagram_fakepacketreorder_send), and
	/// duplication (steamdatagram_fakepacketdup_send)
	bool BSendRawPacket( const void *pPkt, int cbPkt, const netadr_t &adrTo ) const;
	inline bool BSendRawPacket( const void *pPkt, int cbPkt, const SteamNetworkingIPAddr &adrTo ) const
	{
		netadr_t netadrTo;
		SteamNetworkingIPAddrToNetAdr( netadrTo, adrTo );
		return BSendRawPacket( pPkt, cbPkt, netadrTo );
	}

	/// Gather-based send.  Simulated lag, loss, etc are applied
	bool BSendRawPacketGather( int nChunks, const iovec *pChunks, const netadr_t &adrTo ) const;
	inline bool BSendRawPacketGather( int nChunks, const iovec *pChunks, const SteamNetworkingIPAddr &adrTo ) const
	{
		netadr_t netadrTo;
		SteamNetworkingIPAddrToNetAdr( netadrTo, adrTo );
		return BSendRawPacketGather( nChunks, pChunks, netadrTo );
	}

	/// Logically close the socket.  This might not actually close the socket IMMEDIATELY,
	/// there may be a slight delay.  (On the order of a few milliseconds.)  But you will not
	/// get any further callbacks.
	void Close();

	/// The local address we ended up binding to
	SteamNetworkingIPAddr m_boundAddr;

protected:
	IRawUDPSocket();
	~IRawUDPSocket();
};

const int k_nAddressFamily_Auto = -1; // Will try to use IPv6 dual stack if possible.  Falls back to IPv4 if necessary (and possible for your requested bind address)
const int k_nAddressFamily_IPv4 = 1;
const int k_nAddressFamily_IPv6 = 2;
const int k_nAddressFamily_DualStack = k_nAddressFamily_IPv4|k_nAddressFamily_IPv6;

/// Create a UDP socket, set all the socket options for non-blocking, etc, bind it to the desired interface and port, and
/// make sure we're setup to poll the socket efficiently and deliver packets received to the specified callback.
///
/// Local address is interpreted as follows:
/// - If a specific IPv6 or IPv4 address is present, we will try to bind to that interface,
///   and dual-stack will be disabled.
/// - If IPv4 0.0.0.0 is specified, only bind for IPv4
/// - If IPv6 ::0 is specified, consult pnAddressFamilies.
///
/// Address family is interpreted as follows:
/// - k_nAddressFamily_IPv4/k_nAddressFamily_IPv6: only bind for that protocol
/// - k_nAddressFamily_DualStack: Fail if we cannot get dual stack
/// - k_nAddressFamily_Auto (or null): Try dual stack if address is ::0 or null,
///   otherwise use single protocol.
///
/// Upon exit, the address and address families are modified to contain the actual bound
/// address (specifically, the port!) and available address families.
extern IRawUDPSocket *OpenRawUDPSocket( CRecvPacketCallback callback, SteamDatagramErrMsg &errMsg, SteamNetworkingIPAddr *pAddrLocal, int *pnAddressFamilies );

/// A single socket could, in theory, be used to communicate with every single remote host.
/// Or we may decide to open up one socket per remote host, to workaround weird firewall/NAT
/// bugs.  A IBoundUDPSocket abstracts this.  If you need to talk to a single remote host
/// over UDP, you can get one of these and not worry about whether you got your own socket
/// or are sharing a socket.  And you don't need to worry about polling the socket.  You'll
/// just get your callback when a packet is received.
class IBoundUDPSocket
{
public:

	/// Send a packet on this socket to the bound remote host
	inline bool BSendRawPacket( const void *pPkt, int cbPkt ) const
	{
		return m_pRawSock->BSendRawPacket( pPkt, cbPkt, m_adr );
	}

	/// Gather-based send to the bound remote host
	inline bool BSendRawPacketGather( int nChunks, const iovec *pChunks ) const
	{
		return m_pRawSock->BSendRawPacketGather( nChunks, pChunks, m_adr );
	}

	/// Close this socket and stop talking to the specified remote host
	virtual void Close() = 0;

	/// Who are we talking to?
	const netadr_t &GetRemoteHostAddr() const { return m_adr; }

	/// Access the underlying socket we are using (which might be shared)
	IRawUDPSocket *GetRawSock() const { return m_pRawSock; }

protected:
	inline IBoundUDPSocket( IRawUDPSocket *pRawSock, const netadr_t &adr ) : m_adr( adr ), m_pRawSock( pRawSock ) {}
	inline ~IBoundUDPSocket() {}

	/// Address of remote host
	netadr_t m_adr;

	/// The raw socket that is being shared
	IRawUDPSocket *m_pRawSock;
};

/// Get a socket to talk to a single host.  The underlying socket won't be
/// shared with anybody else.
extern IBoundUDPSocket *OpenUDPSocketBoundToHost( const netadr_t &adrRemote, CRecvPacketCallback callback, SteamDatagramErrMsg &errMsg );

/// Create a pair of sockets that are bound to talk to each other.
extern bool CreateBoundSocketPair( CRecvPacketCallback callback1, CRecvPacketCallback callback2, IBoundUDPSocket **ppOutSockets, SteamDatagramErrMsg &errMsg );

/// Manage a single underlying socket that is used to talk to multiple remote hosts
class CSharedSocket
{
public:
	STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW
	CSharedSocket();
	~CSharedSocket();

	/// Allocate a raw socket and setup bookkeeping structures so we can add
	/// clients that will talk using it.
	bool BInit( const SteamNetworkingIPAddr &localAddr, CRecvPacketCallback callbackDefault, SteamDatagramErrMsg &errMsg );

	/// Close all sockets and clean up all resources
	void Kill();

	/// Add a client to talk to a given remote address.  Use IBoundUDPSocket::Close when you
	/// are done.
	IBoundUDPSocket *AddRemoteHost( const netadr_t &adrRemote, CRecvPacketCallback callback );

	/// Send a packet to a remove host.  It doesn't matter if the remote host
	/// is in the client table a client already or not.
	bool BSendRawPacket( const void *pPkt, int cbPkt, const netadr_t &adrTo ) const
	{
		return m_pRawSock->BSendRawPacket( pPkt, cbPkt, adrTo );
	}

	const SteamNetworkingIPAddr *GetBoundAddr() const
	{
		if ( !m_pRawSock )
		{
			Assert( false );
			return nullptr;
		}
		return &m_pRawSock->m_boundAddr;
	}

private:

	/// Call this if we get a packet from somebody we don't recognize
	CRecvPacketCallback m_callbackDefault;

	/// The raw socket that is being shared
	IRawUDPSocket *m_pRawSock;

	class RemoteHost : public IBoundUDPSocket
	{
	private:
		friend class CSharedSocket;
		inline virtual ~RemoteHost() {}
	public:
		STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW
		inline RemoteHost( IRawUDPSocket *pRawSock, const netadr_t &adr ) : IBoundUDPSocket( pRawSock, adr ) {}
		CRecvPacketCallback m_callback;
		CSharedSocket *m_pOwner;
		virtual void Close() OVERRIDE;
	};
	friend class RemoteHost;

	/// List of remote hosts we're talking to.  It's sort of silly to use a map,
	/// which duplicates the address in the key as well as a member of the
	/// RemoteHost.
	/// Perhaps a better approach would be to use an RBTree, but then we'd
	/// need to be able to search the tree given an address, and RBTRee class
	/// doesn't have that interface yet.  Also, it's probably better to
	/// waste a tiny bit of space and put the keys close together in memory,
	/// anyway.
	CUtlHashMap<netadr_t, RemoteHost *, std::equal_to<netadr_t>, netadr_t::Hash > m_mapRemoteHosts;

	void CloseRemoteHostByIndex( int idx );

	static void CallbackRecvPacket( const void *pPkt, int cbPkt, const netadr_t &adrFrom, CSharedSocket *pSock );
};

/////////////////////////////////////////////////////////////////////////////
//
// Misc low level service thread stuff
//
/////////////////////////////////////////////////////////////////////////////

/// Called when we know it's safe to actually destroy sockets pending deletion.
/// This is when: 1.) We own the lock and 2.) we aren't polling in the service thread.
extern void ProcessPendingDestroyClosedRawUDPSockets();

/// Last time that we spewed something that was subject to rate limit 
extern SteamNetworkingMicroseconds g_usecLastRateLimitSpew;
extern int g_nRateLimitSpewCount;

/// Check for rate limiting spew (e.g. when spew could be triggered by malicious sender.)
inline bool BRateLimitSpew( SteamNetworkingMicroseconds usecNow )
{
	if ( g_nRateLimitSpewCount <= 0 )
	{
		if ( usecNow < g_usecLastRateLimitSpew + 300000 )
			return false;
		g_usecLastRateLimitSpew = usecNow;
		g_nRateLimitSpewCount = 3; // Allow a short burst, because sometimes we need messages from different levels on the call stack
	}
	--g_nRateLimitSpewCount;
	return true;
}

extern ESteamNetworkingSocketsDebugOutputType g_eDefaultGroupSpewLevel;
extern void ReallySpewTypeFmt( int eType, PRINTF_FORMAT_STRING const char *pFmt, ... ) FMTFUNCTION( 2, 3 );
extern void (*g_pfnPreFormatSpewHandler)( ESteamNetworkingSocketsDebugOutputType eType, bool bFmt, const char* pstrFile, int nLine, const char *pMsg, va_list ap );

#define SpewTypeGroup( eType, nGroup, ... ) ( ( (eType) <= (nGroup) ) ? ReallySpewTypeFmt( (eType), __VA_ARGS__ ) : (void)0 )
#define SpewMsgGroup( nGroup, ... ) SpewTypeGroup( k_ESteamNetworkingSocketsDebugOutputType_Msg, (nGroup), __VA_ARGS__ )
#define SpewVerboseGroup( nGroup, ... ) SpewTypeGroup( k_ESteamNetworkingSocketsDebugOutputType_Verbose, (nGroup), __VA_ARGS__ )
#define SpewDebugGroup( nGroup, ... ) SpewTypeGroup( k_ESteamNetworkingSocketsDebugOutputType_Debug, (nGroup), __VA_ARGS__ )
#define SpewImportantGroup( nGroup, ... ) SpewTypeGroup( k_ESteamNetworkingSocketsDebugOutputType_Important, (nGroup), __VA_ARGS__ )
#define SpewWarningGroup( nGroup, ... ) SpewTypeGroup( k_ESteamNetworkingSocketsDebugOutputType_Warning, (nGroup), __VA_ARGS__ )
#define SpewErrorGroup( nGroup, ... ) SpewTypeGroup( k_ESteamNetworkingSocketsDebugOutputType_Error, (nGroup), __VA_ARGS__ )
#define SpewBugGroup( nGroup, ... ) SpewTypeGroup( k_ESteamNetworkingSocketsDebugOutputType_Bug, (nGroup), __VA_ARGS__ )

#define SpewTypeDefaultGroup( eType, ... ) SpewTypeGroup( eType, g_eDefaultGroupSpewLevel, __VA_ARGS__ )
#define SpewMsg( ... ) SpewTypeDefaultGroup( k_ESteamNetworkingSocketsDebugOutputType_Msg, __VA_ARGS__ )
#define SpewVerbose( ... ) SpewTypeDefaultGroup( k_ESteamNetworkingSocketsDebugOutputType_Verbose, __VA_ARGS__ )
#define SpewDebug( ... ) SpewTypeDefaultGroup( k_ESteamNetworkingSocketsDebugOutputType_Debug, __VA_ARGS__ )
#define SpewImportant( ... ) SpewTypeDefaultGroup( k_ESteamNetworkingSocketsDebugOutputType_Important, __VA_ARGS__ )
#define SpewWarning( ... ) SpewTypeDefaultGroup( k_ESteamNetworkingSocketsDebugOutputType_Warning, __VA_ARGS__ )
#define SpewError( ... ) SpewTypeDefaultGroup( k_ESteamNetworkingSocketsDebugOutputType_Error, __VA_ARGS__ )
#define SpewBug( ... ) SpewTypeDefaultGroup( k_ESteamNetworkingSocketsDebugOutputType_Bug, __VA_ARGS__ )

#define SpewTypeDefaultGroupRateLimited( usecNow, eType, ... ) ( ( (eType) <= g_eDefaultGroupSpewLevel && BRateLimitSpew( usecNow ) ) ? ReallySpewTypeFmt( (eType), __VA_ARGS__ ) : (void)0 )
#define SpewWarningRateLimited( usecNow, ... ) SpewTypeDefaultGroupRateLimited( usecNow, k_ESteamNetworkingSocketsDebugOutputType_Warning, __VA_ARGS__ )

/// Make sure stuff is initialized
extern bool BSteamNetworkingSocketsLowLevelAddRef( SteamDatagramErrMsg &errMsg );

/// Nuke common stuff
extern void SteamNetworkingSocketsLowLevelDecRef();

/////////////////////////////////////////////////////////////////////////////
//
// Locking
//
// Having fine-grained locks is utterly terrifying, frankly.  In order
// to make this work, while avoiding deadlocks, we protect *most* things
// with the global lock, and only certain frequently used API calls use more
// fine-grained locks.
//
// In general, the global lock will be held while the background is doing its work,
// and so we want to avoid API calls taking that lock when possible.  The most
// important API calls that are likely to conflict are:
//
// - sending messages on a connection
// - polling for incoming messages on a connection or poll group.
// - polling connection state
//
// These are the calls most likely to be called often, maybe from multiple threads at
// the same time.
//
// For less frequently-used API calls, we are less concerned about lock contention and
// prefer to keep the code simple until we have a proven example of bad performance.
//
// Here are the locks that are used:
// 
// - Global lock.  You must hold this lock while:
//   - Changing any data not specifically carved out below.
//   - Creating or destroying objects
//   - Changing connection state
//   - Changing links between multiple objects.  (E.g. assigning connections to poll groups.)
//   - Acquiring more than one "object" lock below at the same time.
// - Per-connection locks.  You must hold this lock to modify any property of the connection.
// - Per-poll-group locks.  You must hold this lock to modify any property of the poll group.
// - g_tables_lock.  Protects the connection and poll group global handle lookup tables.
//   You must hold the lock any time you want to read or write the connection or poll group
//   tables.  This is a very special lock with custom handling.
// - Other miscellaneous "leaf" locks that are only held very briefly to protect specific
//   data structures, such as callback lists.  (ShortDurationLock's)
//
// The rules for acquiring locks are as follows:
// - You may not acquire the global lock while already holding any other lock.  The global lock
//   must always be acquired *first*.
// - You may not acquire another lock while you already hold a ShortDurationLock.  These locks
//   are intended for extremely simple use cases where the lock is expected to be held for a brief
//   period of time and contention is expected to be low.
// - You may not acquire more than object lock (connection or poll group) unless already holding
//   the global lock.
// - The table lock must always be acquired before any object or poll group locks.  This is the flow
//   that happens for all API calls.  Also - note that API calls are special in that they release the
//   table lock out of order, while retaining the object lock.  (It is not a stack lock/unlock pattern.)
//   Object creation is special, and out-of-order locking is OK.  See the code for why.
//
// A sequence of lock acquisitions that violates the rules above *is* allowed, provided
// that the out-of-order acquisition is a "try" acquisition, tolerant of failing due to
// deadlock.
//
/////////////////////////////////////////////////////////////////////////////

// You can override these with more optimal platform-specific
// versions if you want
using ShortDurationMutexImpl = std::mutex; // No recursion, no timeout, should only be held for a short time, so expect low contention.  Good candidate for spinlock.
using RecursiveMutexImpl = std::recursive_mutex; // Need to able to lock recursively, but don't need to be able to wait with timeout.
using RecursiveTimedMutexImpl = std::recursive_timed_mutex; // Recursion, and need to be able to wait with timeout.  (Does this ability actually add any extra work on any OS we care about?)

/// Debug record for a lock.
struct LockDebugInfo
{
	static constexpr int k_nFlag_ShortDuration = (1<<0);
	static constexpr int k_nFlag_Connection = (1<<1);
	static constexpr int k_nFlag_PollGroup = (1<<2);
	static constexpr int k_nFlag_Table = (1<<4);

	const char *const m_pszName;
	const int m_nFlags;

	void AssertHeldByCurrentThread( const char *pszTag = nullptr ) const;

protected:
	LockDebugInfo( const char *pszName, int nFlags ) : m_pszName( pszName ), m_nFlags( nFlags ) {}
	~LockDebugInfo();

	void AboutToLock( bool bTry );
	void OnLocked( const char *pszTag );
	void AboutToUnlock();

	//volatile int m_nLockCount = 0;
	//std::thread::id m_threadIDLockOwner;

};

/// Wrapper for locks to make them somewhat debuggable.
template<typename TMutexImpl >
struct Lock : LockDebugInfo
{
	inline Lock( const char *pszName, int nFlags ) : LockDebugInfo( pszName, nFlags ) {}
	inline void lock( const char *pszTag = nullptr )
	{
		LockDebugInfo::AboutToLock( false );
		m_impl.lock();
		LockDebugInfo::OnLocked( pszTag );
	}
	inline void unlock()
	{
		LockDebugInfo::AboutToUnlock();
		m_impl.unlock();
	}
	inline bool try_lock( const char *pszTag = nullptr ) {
		LockDebugInfo::AboutToLock( true );
		if ( !m_impl.try_lock() )
			return false;
		LockDebugInfo::OnLocked( pszTag );
		return true;
	}
	inline bool try_lock_for( int msTimeout, const char *pszTag = nullptr )
	{
		LockDebugInfo::AboutToLock( true );
		if ( !m_impl.try_lock_for( std::chrono::milliseconds( msTimeout ) ) )
			return false;
		LockDebugInfo::OnLocked( pszTag );
		return true;
	}

private:
	TMutexImpl m_impl;
};

/// Object that automatically unlocks a lock when it goes out of scope using RIAA
template<typename TLock>
struct ScopeLock
{
	ScopeLock() : m_pLock( nullptr ) {}
	explicit ScopeLock( TLock &lock, const char *pszTag = nullptr ) : m_pLock(&lock) { lock.lock( pszTag ); }
	~ScopeLock() { if ( m_pLock ) m_pLock->unlock(); }
	void Lock( TLock &lock, const char *pszTag = nullptr ) { Assert( !m_pLock ); m_pLock = &lock; lock.lock( pszTag ); }
	bool TryLock( TLock &lock, int msTimeout, const char *pszTag ) { Assert( !m_pLock ); if ( !lock.try_lock_for( msTimeout, pszTag ) ) return false; m_pLock = &lock; return true; }
	void Unlock() { if ( !m_pLock ) return; m_pLock->unlock(); m_pLock = nullptr; }

	// If we have a lock, forget about it
	void Abandon() { m_pLock = nullptr; }
private:
	TLock *m_pLock;
};

// A very simple lock to protect short accesses to a small set of data.
// Used when:
// - We hold the lock for a brief period.
// - We don't need to take any additional locks while already holding this one.
//   (Including this lock -- e.g. we don't need to lock recursively.)
struct ShortDurationLock : Lock<ShortDurationMutexImpl>
{
	ShortDurationLock( const char *pszName ) : Lock<ShortDurationMutexImpl>( pszName, k_nFlag_ShortDuration ) {}
};
using ShortDurationScopeLock = ScopeLock<ShortDurationLock>;

/// Special utilities for acquiring the global lock
struct SteamNetworkingGlobalLock
{
	inline SteamNetworkingGlobalLock( const char *pszTag = nullptr ) { Lock( pszTag ); }
	inline ~SteamNetworkingGlobalLock() { Unlock(); }
	static void Lock( const char *pszTag );
	static bool TryLock( const char *pszTag, int msTimeout );
	static void Unlock();
	static void AssertHeldByCurrentThread();
	static void AssertHeldByCurrentThread( const char *pszTag );
	static void SetLongLockWarningThresholdMS( const char *pszTag, int msWarningThreshold );
};

#ifdef DBGFLAG_VALIDATE
extern void SteamNetworkingSocketsLowLevelValidate( CValidator &validator );
#endif

/// Wake up the service thread ASAP.  Intended to be called from other threads,
/// but is safe to call from the service thread as well.
extern void WakeSteamDatagramThread();

/// Class used to take some action while we have the global thread locked,
/// perhaps later and in another thread if necessary.  Intended to be used
/// from callbacks and other contexts where we don't know what thread we are
/// in and cannot risk trying to wait on the lock, without risking creating
/// a deadlock.
///
/// Note: This code could have been a lot simpler with std::function, but
/// it was intentionally not used, to avoid adding that runtime dependency.
class ISteamNetworkingSocketsRunWithLock
{
public:
	virtual ~ISteamNetworkingSocketsRunWithLock();

	/// If we can run immediately, then do so, delete self, and return true.
	/// Otherwise, we are placed into a queue and false is returned.
	bool RunOrQueue( const char *pszTag );

	/// Don't check the global lock, just queue the item to be run.
	void Queue( const char *pszTag );

	/// Called from service thread while we hold the lock
	static void ServiceQueue();

	inline const char *Tag() const { return m_pszTag; }
private:
	const char *m_pszTag = nullptr;

protected:
	virtual void Run() = 0;

	inline ISteamNetworkingSocketsRunWithLock() {};
};

/////////////////////////////////////////////////////////////////////////////
//
// Misc
//
/////////////////////////////////////////////////////////////////////////////

/// Fetch current time
extern SteamNetworkingMicroseconds SteamNetworkingSockets_GetLocalTimestamp();

/// Set debug output hook
extern void SteamNetworkingSockets_SetDebugOutputFunction( ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc );

/// Return true if it looks like the address is a local address
extern bool IsRouteToAddressProbablyLocal( netadr_t addr );

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ETW
	extern void ETW_Init();
	extern void ETW_Kill();
	extern void ETW_LongOp( const char *opName, SteamNetworkingMicroseconds usec, const char *pszInfo = nullptr );
	extern void ETW_UDPSendPacket( const netadr_t &adrTo, int cbPkt );
	extern void ETW_UDPRecvPacket( const netadr_t &adrFrom, int cbPkt );
	extern void ETW_ICESendPacket( HSteamNetConnection hConn, int cbPkt );
	extern void ETW_ICERecvPacket( HSteamNetConnection hConn, int cbPkt );
	extern void ETW_ICEProcessPacket( HSteamNetConnection hConn, int cbPkt );
	extern void ETW_webrtc_setsockopt( int slevel, int sopt, int value );
	extern void ETW_webrtc_send( int length );
	extern void ETW_webrtc_sendto( void *addr, int length );
#else
	inline void ETW_Init() {}
	inline void ETW_Kill() {}
	inline void ETW_LongOp( const char *opName, SteamNetworkingMicroseconds usec, const char *pszInfo = nullptr ) {}
	inline void ETW_UDPSendPacket( const netadr_t &adrTo, int cbPkt ) {}
	inline void ETW_UDPRecvPacket( const netadr_t &adrFrom, int cbPkt ) {}
	inline void ETW_ICESendPacket( HSteamNetConnection hConn, int cbPkt ) {}
	inline void ETW_ICERecvPacket( HSteamNetConnection hConn, int cbPkt ) {}
	inline void ETW_ICEProcessPacket( HSteamNetConnection hConn, int cbPkt ) {}
#endif

} // namespace SteamNetworkingSocketsLib

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_DefaultPreFormatDebugOutputHandler( ESteamNetworkingSocketsDebugOutputType eType, bool bFmt, const char* pstrFile, int nLine, const char *pMsg, va_list ap );

#endif // STEAMNETWORKINGSOCKETS_LOWLEVEL_H
