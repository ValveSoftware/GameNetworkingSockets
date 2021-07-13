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

// Set STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL.
// NOTE: Currently only 0 or 1 is allowed.  Later we might add more flexibility
#ifndef STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL
	#ifdef DBGFLAG_ASSERT
		#define STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL 1
	#else
		#define STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL 0
	#endif
#endif

struct iovec;

namespace SteamNetworkingSocketsLib {

class IRawUDPSocket;

/////////////////////////////////////////////////////////////////////////////
//
// Low level sockets
//
/////////////////////////////////////////////////////////////////////////////

/// Info about an incoming packet passed to the CRecvPacketCallback
struct RecvPktInfo_t
{
	const void *m_pPkt;
	int m_cbPkt;
	SteamNetworkingMicroseconds m_usecNow; // Current time
	// FIXME - coming soon!
	//SteamNetworkingMicroseconds m_usecRecvMin; // Earliest possible time when the packet might have actually arrived
	//SteamNetworkingMicroseconds m_usecRecvMax; // Latest possible time when the packet might have actually arrived
	netadr_t m_adrFrom;
	IRawUDPSocket *m_pSock;
};

/// Store the callback and its context together
class CRecvPacketCallback
{
public:
	/// Prototype of the callback
	typedef void (*FCallbackRecvPacket)( const RecvPktInfo_t &info, void *pContext );

	/// Default constructor sets stuff to null
	inline CRecvPacketCallback() : m_fnCallback( nullptr ), m_pContext( nullptr ) {}

	/// A template constructor so you can use type safe context and avoid messy casting
	template< typename T >
	inline CRecvPacketCallback( void (*fnCallback)( const RecvPktInfo_t &info, T context ), T context )
	: m_fnCallback ( reinterpret_cast< FCallbackRecvPacket>( fnCallback ) )
	, m_pContext( reinterpret_cast< void * >( context ) )
	{
		COMPILE_TIME_ASSERT( sizeof(T) == sizeof(void*) );
	}

	FCallbackRecvPacket m_fnCallback;
	void *m_pContext;

	/// Shortcut notation to execute the callback
	inline void operator()( const RecvPktInfo_t &info ) const
	{
		if ( m_fnCallback )
			m_fnCallback( info, m_pContext );
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
	inline bool BSendRawPacket( const void *pPkt, int cbPkt, const netadr_t &adrTo ) const
	{
		iovec temp;
		temp.iov_len = cbPkt;
		temp.iov_base = (void *)pPkt;
		return BSendRawPacketGather( 1, &temp, adrTo );
	}
	inline bool BSendRawPacket( const void *pPkt, int cbPkt, const SteamNetworkingIPAddr &adrTo ) const
	{
		netadr_t netadrTo;
		SteamNetworkingIPAddrToNetAdr( netadrTo, adrTo );
		return BSendRawPacket( pPkt, cbPkt, netadrTo );
	}

	/// Gather-based send.  Simulated lag, loss, etc are applied
	virtual bool BSendRawPacketGather( int nChunks, const iovec *pChunks, const netadr_t &adrTo ) const = 0;
	inline bool BSendRawPacketGather( int nChunks, const iovec *pChunks, const SteamNetworkingIPAddr &adrTo ) const
	{
		netadr_t netadrTo;
		SteamNetworkingIPAddrToNetAdr( netadrTo, adrTo );
		return BSendRawPacketGather( nChunks, pChunks, netadrTo );
	}

	/// Logically close the socket.  This might not actually close the socket IMMEDIATELY,
	/// there may be a slight delay.  (On the order of a few milliseconds.)  But you will not
	/// get any further callbacks.
	virtual void Close() = 0;

	/// The local address we ended up binding to
	SteamNetworkingIPAddr m_boundAddr;

protected:
	IRawUDPSocket();
	virtual ~IRawUDPSocket();
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

	static void CallbackRecvPacket( const RecvPktInfo_t &info, CSharedSocket *pSock );
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
extern FSteamNetworkingSocketsDebugOutput g_pfnDebugOutput;

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

	#if STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL > 0
		void _AssertHeldByCurrentThread( const char *pszFile, int line, const char *pszTag = nullptr ) const;
	#else
		inline void _AssertHeldByCurrentThread( const char *pszFile, int line, const char *pszTag = nullptr ) const {}
	#endif

protected:
	LockDebugInfo( const char *pszName, int nFlags ) : m_pszName( pszName ), m_nFlags( nFlags ) {}

	#if STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL > 0
		void AboutToLock( bool bTry );
		void OnLocked( const char *pszTag );
		void AboutToUnlock();
		~LockDebugInfo();
	#else
		void AboutToLock( bool bTry ) {}
		void OnLocked( const char *pszTag ) {}
		void AboutToUnlock() {}
	#endif
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
	bool IsLocked() const { return m_pLock != nullptr; } // Return true if we hold any lock
	bool BHoldsLock( const TLock &lock ) const { return m_pLock == &lock; } // Return true if we hold the specific lock
	void Lock( TLock &lock, const char *pszTag = nullptr )
	{
		if ( m_pLock )
		{
			AssertMsg( false, "Scopelock already holding %s, while locking %s!  tag=%s",
				m_pLock->m_pszName, lock.m_pszName, pszTag ? pszTag : "???" );
			m_pLock->unlock();
		}
		m_pLock = &lock;
		lock.lock( pszTag );
	}
	bool TryLock( TLock &lock, int msTimeout, const char *pszTag )
	{
		if ( m_pLock )
		{
			AssertMsg( false, "Scopelock already holding %s, while trylock %s!  tag=%s",
				m_pLock->m_pszName, lock.m_pszName, pszTag ? pszTag : "???" );
			m_pLock->unlock();
			m_pLock = nullptr;
		}
		if ( !lock.try_lock_for( msTimeout, pszTag ) )
			return false;
		m_pLock = &lock;
		return true;
	}
	void Unlock() { if ( !m_pLock ) return; m_pLock->unlock(); m_pLock = nullptr; }

	// If we have a lock, forget about it
	void Abandon() { m_pLock = nullptr; }

	// Take ownership of a lock (that must already be locked by the current thread),
	// so that we will unlock it when we are destructed
	void _TakeLockOwnership( TLock *pLock, const char *pszFile, int line, const char *pszTag = nullptr )
	{
		pLock->_AssertHeldByCurrentThread( pszFile, line, pszTag );

		if ( m_pLock )
		{
			AssertMsg( false, "Scopelock already holding %s, while assuming ownership %s!  tag=%s",
				m_pLock->m_pszName, pLock->m_pszName, pszTag ? pszTag : "???" );
			m_pLock->unlock();
		}
		m_pLock = pLock;
	}
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

#if STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL > 0
	#define AssertHeldByCurrentThread( ... ) _AssertHeldByCurrentThread( __FILE__, __LINE__ ,## __VA_ARGS__ )
	#define AssertLocksHeldByCurrentThread( ... ) _AssertLocksHeldByCurrentThread( __FILE__, __LINE__,## __VA_ARGS__ )
	#define TakeLockOwnership( pLock, ... ) _TakeLockOwnership( (pLock), __FILE__, __LINE__,## __VA_ARGS__ )
#else
	#define AssertHeldByCurrentThread( ... ) _AssertHeldByCurrentThread( nullptr, 0,## __VA_ARGS__ )
	#define AssertLocksHeldByCurrentThread( ... ) _AssertLocksHeldByCurrentThread( nullptr, 0,## __VA_ARGS__ )
	#define TakeLockOwnership( pLock, ... ) _TakeLockOwnership( (pLock), nullptr, 0,## __VA_ARGS__ )
#endif

/// Special utilities for acquiring the global lock
struct SteamNetworkingGlobalLock
{
	inline SteamNetworkingGlobalLock( const char *pszTag = nullptr ) { Lock( pszTag ); }
	inline ~SteamNetworkingGlobalLock() { Unlock(); }
	static void Lock( const char *pszTag );
	static bool TryLock( const char *pszTag, int msTimeout );
	static void Unlock();

	#if STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL > 0
		static void _AssertHeldByCurrentThread( const char *pszFile, int line );
		static void _AssertHeldByCurrentThread( const char *pszFile, int line, const char *pszTag );
		static void SetLongLockWarningThresholdMS( const char *pszTag, int msWarningThreshold );
	#else
		static void _AssertHeldByCurrentThread( const char *pszFile, int line ) {}
		static void _AssertHeldByCurrentThread( const char *pszFile, int line, const char *pszTag ) {}
		inline static void SetLongLockWarningThresholdMS( const char *pszTag, int msWarningThreshold ) {}
	#endif
};

#ifdef DBGFLAG_VALIDATE
extern void SteamNetworkingSocketsLowLevelValidate( CValidator &validator );
#endif

/// Wake up the service thread ASAP.  Intended to be called from other threads,
/// but is safe to call from the service thread as well.
extern void WakeSteamDatagramThread();

class CQueuedTask;

/// The target of a task that can be locked and can be safely deleted while
/// tasks are still queued against the target
class CTaskTarget
{
public:

protected:

	/// Destructor will cancel tasks.  You need to make sure any relevant
	/// locks have been acquired!
	~CTaskTarget();

	/// Cancel any tasks that are queued for us.
	/// This MUST be called while relevant locks are held,
	/// if any tasks are queued with a locking mechanism.
	void CancelQueuedTasks();

private:
	friend class CTaskList;

	/// Doubly-linked list of tasks that need to be canceled
	/// if we get deleted.  These are stored in "reverse"
	/// order, or more accurately, the order of the list
	/// should not be relevant
	CQueuedTask *m_pFirstTask = nullptr;
};

/// Abstract class for a task that is in a queue.  Optionally, you can set
/// a target that may need to be locked before the task is run or may be
/// deleted while tasks are queued
class CQueuedTask
{
public:

	// Target accessors
	void SetTarget( CTaskTarget *pTarget );
	CTaskTarget *Target() const { return m_pTarget; }

	/// If we can acquire the global lock immediately, then do so, delete self, and return true.
	/// Otherwise, we are placed into a queue and false is returned.
	bool RunWithGlobalLockOrQueue( const char *pszTag );

	/// Queue the item to run while we have the global lock held.
	void QueueToRunWithGlobalLock( const char *pszTag );

	/// Queue the item to run in the background when we have some time,
	/// on no particular thread and with no locks held
	void QueueToRunInBackground();

	/// Function call used to try to take the lock
	typedef bool (*FTryLockFunc)( void *lock, int msTimeOut, const char *pszTag );

	/// Set the locking mechanism that we should acquire before trying to
	/// run the task.  You must use this if the target might be deleted
	/// while the task is run.  If there is no chance of the target being
	/// deleted while the tasks are being run, then this is not necessary.
	inline void SetLockFunc( FTryLockFunc func, void *lock, const char *pszTag = nullptr )
	{
		m_lockFunc = func;
		m_lockFuncArg = lock;
		if ( pszTag )
			m_pszTag = pszTag;
	}

	/// Adapter for typed locks
	template<typename TLock>
	inline void SetLock( TLock &lock, const char *pszTag = nullptr )
	{
		m_lockFunc = []( void *pLock, int msTimeOut, const char *pszTagArg ) -> bool
		{
			return ((TLock *)pLock)->try_lock_for( msTimeOut, pszTagArg );
		};
		m_lockFuncArg = &lock;
		if ( pszTag )
			m_pszTag = pszTag;
	}

protected:
	virtual void Run() = 0;
	CQueuedTask() {}
	CQueuedTask( CTaskTarget *pTarget ) { SetTarget( pTarget ); }
	CTaskTarget *m_pTarget = nullptr;

	virtual ~CQueuedTask();

	enum ETaskState
	{
		k_ETaskState_Init,
		k_ETaskState_Queued,
		k_ETaskState_Running,
		k_ETaskState_ReadyToDelete,
	};

private:
	friend class CTaskList;
	friend class CTaskTarget;
	CQueuedTask *m_pNextTaskInQueue = nullptr;
	CQueuedTask *m_pPrevTaskForTarget = nullptr;
	CQueuedTask *m_pNextTaskForTarget = nullptr;
	FTryLockFunc m_lockFunc = nullptr;
	void *m_lockFuncArg = nullptr;
	const char *m_pszTag = nullptr;
	volatile ETaskState m_eTaskState = k_ETaskState_Init;
};

/// Helper class for a class that takes a target of a specific
/// type that is derived from CTaskTarget
template<typename TTarget>
class CQueuedTaskOnTarget : public CQueuedTask
{
public:
	CQueuedTaskOnTarget( TTarget *pTarget = nullptr ) : CQueuedTask( pTarget ) {}

	/// Upcast
	void SetTarget( TTarget *pTarget ) { CQueuedTask::SetTarget( pTarget ); }
	TTarget *Target() const { return static_cast<TTarget*>( m_pTarget ); }
};

/// A list of queued tasks
class CTaskList
{
public:

	// Add a task to the queue
	void QueueTask( CQueuedTask *pTask );

	// Run the queued tasks
	void RunTasks();

private:
	// List of queued tasks
	CQueuedTask *m_pFirstTask;
	CQueuedTask *m_pLastTask;
};

/// Tasks that we want to run while we hold the global lock
extern CTaskList g_taskListRunWithGlobalLock;

/// Tasks that we want to run when we are "idle", while
/// we do NOT hold the global lock.
extern CTaskList g_taskListRunInBackground;

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
