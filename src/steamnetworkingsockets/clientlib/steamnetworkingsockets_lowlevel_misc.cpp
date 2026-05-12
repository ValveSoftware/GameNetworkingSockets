//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Miscellaneous "low level" functionality for SteamNetworkingSockets.
//
// - Global lock (mutex) and lock debugging
// - Local timestamp clock
// - Memory allocator override
//
#include "steamnetworkingsockets_lowlevel.h"
#include "crypto.h"
#include <vstdlib/random.h>
#include <tier0/valve_tracelogging.h>

#if IsPosix()
	#include <pthread.h>
#endif

#include <tier0/memdbgon.h>

TRACELOGGING_DECLARE_PROVIDER( HTraceLogging_SteamNetworkingSockets );

using namespace SteamNetworkingSocketsLib;
namespace SteamNetworkingSocketsLib {

void ETW_LongOp( const char *opName, SteamNetworkingMicroseconds usec, const char *pszInfo )
{
	if ( !pszInfo )
		pszInfo = "";
	TraceLoggingWrite(
		HTraceLogging_SteamNetworkingSockets,
		"LongOp",
		TraceLoggingLevel( WINEVENT_LEVEL_WARNING ),
		TraceLoggingUInt64( usec, "Microseconds" ),
		TraceLoggingString( pszInfo, "ExtraInfo" )
	);
}

void SeedWeakRandomGenerator()
{

	// Seed cheesy random number generator using true source of entropy
	int temp;
	CCrypto::GenerateRandomBlock( &temp, sizeof(temp) );
	WeakRandomSeed( temp );
}

/////////////////////////////////////////////////////////////////////////////
//
// Lock debugging / hygiene
//
/////////////////////////////////////////////////////////////////////////////

/// Global lock for all local data structures
static Lock<RecursiveTimedMutexImpl> s_mutexGlobalLock( "global", 0, LockDebugInfo::k_nOrder_Global );

// Threshold for the service-thread-specific *assert*
#ifdef __SANITIZE_THREAD__
SteamNetworkingMicroseconds s_usecServiceThreadLockWaitWarning = 300*1000;
#else
SteamNetworkingMicroseconds s_usecServiceThreadLockWaitWarning = 50*1000;
#endif


#if STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL > 0

// By default, complain if we hold the lock for more than this long.
// TSan adds 10-20x overhead so the threshold is scaled up accordingly.
#ifdef __SANITIZE_THREAD__
constexpr SteamNetworkingMicroseconds k_usecDefaultLongLockHeldWarningThreshold = 5*1000*20;
#else
constexpr SteamNetworkingMicroseconds k_usecDefaultLongLockHeldWarningThreshold = 5*1000;
#endif

// Threshold for warning about waiting on any lock.
#ifdef __SANITIZE_THREAD__
static SteamNetworkingMicroseconds s_usecLockWaitWarningThreshold = 300*1000;
#else
static SteamNetworkingMicroseconds s_usecLockWaitWarningThreshold = 2*1000;
#endif

// Debug the locks active on the cu
struct ThreadLockDebugInfo
{
	static constexpr int k_nMaxHeldLocks = 8;
	static constexpr int k_nMaxTags = 32;

	int m_nHeldLocks = 0;
	int m_nTags = 0;

	SteamNetworkingMicroseconds m_usecLongLockWarningThreshold;
	SteamNetworkingMicroseconds m_usecIgnoreLongLockWaitTimeUntil;
	SteamNetworkingMicroseconds m_usecOuterLockStartTime; // Time when we started waiting on outermost lock (if we don't have it yet), or when we aquired the lock (if we have it)

	const LockDebugInfo *m_arHeldLocks[ k_nMaxHeldLocks ];
	struct Tag_t
	{
		const char *m_pszTag;
		int m_nCount;
	};
	Tag_t m_arTags[ k_nMaxTags ];

	inline void AddTag( const char *pszTag );
};

static void (*s_fLockAcquiredCallback)( const char *tags, SteamNetworkingMicroseconds usecWaited );
static void (*s_fLockHeldCallback)( const char *tags, SteamNetworkingMicroseconds usecWaited );

/// Get the per-thread debug info
static ThreadLockDebugInfo &GetThreadDebugInfo()
{
	// Apple seems to hate thread_local.  Is there some sort of feature
	// define a can check here?  It's a shame because it's really very
	// efficient on MSVC, gcc, and clang on Windows and linux.
    //
    // Apple seems to support thread_local starting with Xcode 8.0
	#if defined(__APPLE__) && __clang_major__ < 8

		static pthread_key_t key;
		static pthread_once_t key_once = PTHREAD_ONCE_INIT;

		// One time init the TLS key
		pthread_once( &key_once,
			[](){ // Initialization code to run once
				pthread_key_create(
					&key,
					[](void *ptr) { free(ptr); } // Destructor function
				);
			}
		);

		// Get current object
		void *result = pthread_getspecific(key);
		if ( unlikely( result == nullptr ) )
		{
			result = malloc( sizeof(ThreadLockDebugInfo) );
			memset( result, 0, sizeof(ThreadLockDebugInfo) );
			pthread_setspecific(key, result);
		}
		return *static_cast<ThreadLockDebugInfo *>( result );
	#else

		// Use thread_local
		thread_local ThreadLockDebugInfo tls_lockDebugInfo;
		return tls_lockDebugInfo;
	#endif
}

/// If non-NULL, add a "tag" to the lock journal for the current thread.
/// This is useful so that if we hold a lock for a long time, we can get
/// an idea what sorts of operations were taking a long time.
inline void ThreadLockDebugInfo::AddTag( const char *pszTag )
{
	if ( !pszTag )
		return;

	Assert( m_nHeldLocks > 0 ); // Can't add a tag unless we are locked!

	for ( int i = 0 ; i < m_nTags ; ++i )
	{
		if ( m_arTags[i].m_pszTag == pszTag )
		{
			++m_arTags[i].m_nCount;
			return;
		}
	}

	if ( m_nTags >= ThreadLockDebugInfo::k_nMaxTags )
		return;

	m_arTags[ m_nTags ].m_pszTag = pszTag;
	m_arTags[ m_nTags ].m_nCount = 1;
	++m_nTags;
}

LockDebugInfo::~LockDebugInfo()
{
	// We should not be locked!  If we are, remove us
	ThreadLockDebugInfo &t = GetThreadDebugInfo();
	for ( int i = t.m_nHeldLocks-1 ; i >= 0 ; --i )
	{
		if ( t.m_arHeldLocks[i] == this )
		{
			AssertMsg( false, "Lock '%s' being destroyed while it is held!", m_pszName );
			AboutToUnlock();
		}
	}
}

void LockDebugInfo::AboutToLock( bool bTry )
{
	ThreadLockDebugInfo &t = GetThreadDebugInfo();

	// First lock held by this thread?
	if ( t.m_nHeldLocks == 0 )
	{
		// Remember when we started trying to lock
		t.m_usecOuterLockStartTime = SteamNetworkingSockets_GetLocalTimestamp();
		return;
	}

	// We already hold a lock.  Check for taking locks in such a way
	// that might lead to deadlocks.
	const LockDebugInfo *pTopLock = t.m_arHeldLocks[ t.m_nHeldLocks-1 ];

	// Taking locks in increasing order is always allowed
	if ( likely( pTopLock->m_nOrder < m_nOrder ) )
		return;

	// Global lock *must* always be the outermost lock.  (It is legal to take other locks in
	// between and then lock the global lock recursively.)
	const bool bHoldGlobalLock = t.m_arHeldLocks[ 0 ] == &s_mutexGlobalLock;
	AssertMsg(
		bHoldGlobalLock || this != &s_mutexGlobalLock,
		"Taking global lock while already holding lock '%s'", t.m_arHeldLocks[ 0 ]->m_pszName
	);

	// If they are only "trying", we allow out-of-order behaviour.
	if ( bTry )
		return;

	// It's always OK to lock recursively.
	//
	// (Except for "short duration" locks, which are allowed to
	// use a mutex implementation that does not support this.)
	if ( !( m_nFlags & k_nFlag_ShortDuration ) )
	{
		for ( int i = 0 ; i < t.m_nHeldLocks ; ++i )
		{
			if ( t.m_arHeldLocks[i] == this )
				return;
		}
	}

	// Taking multiple object locks?  This is allowed under certain circumstances
	if ( likely( pTopLock->m_nOrder == m_nOrder && m_nOrder == k_nOrder_ObjectOrTable ) )
	{

		// If we hold the global lock, it's OK
		if ( bHoldGlobalLock )
			return;

		// If the global lock isn't held, then no more than one
		// object lock is allowed, since two different threads
		// might take them in different order.
		constexpr int k_nObjectFlags = LockDebugInfo::k_nFlag_Connection | LockDebugInfo::k_nFlag_PollGroup;
		if (
			( ( m_nFlags & k_nObjectFlags ) != 0 )
			//|| ( m_nFlags & k_nFlag_Table ) // We actually do this in one place when we know it's OK.  Not worth it right now to get this situation exempted from the checking.
		) {
			// We must not already hold any existing object locks (except perhaps this one)
			for ( int i = 0 ; i < t.m_nHeldLocks ; ++i )
			{
				const LockDebugInfo *pOtherLock = t.m_arHeldLocks[ i ];
				AssertMsg( pOtherLock == this || !( pOtherLock->m_nFlags & k_nObjectFlags ),
					"Taking lock '%s' and then '%s', while not holding the global lock", pOtherLock->m_pszName, m_pszName );
			}
		}

		// Usage is OK if we didn't find any problems above
		return;
	}

	AssertMsg( false, "Taking lock '%s' while already holding lock '%s'", m_pszName, pTopLock->m_pszName );
}

void LockDebugInfo::OnLocked( const char *pszTag )
{
	ThreadLockDebugInfo &t = GetThreadDebugInfo();

	Assert( t.m_nHeldLocks < ThreadLockDebugInfo::k_nMaxHeldLocks );
	t.m_arHeldLocks[ t.m_nHeldLocks++ ] = this;

	if ( t.m_nHeldLocks == 1 )
	{
		SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
		SteamNetworkingMicroseconds usecTimeSpentWaitingOnLock = usecNow - t.m_usecOuterLockStartTime;
		t.m_usecLongLockWarningThreshold = k_usecDefaultLongLockHeldWarningThreshold;
		t.m_nTags = 0;

		if ( usecTimeSpentWaitingOnLock > s_usecLockWaitWarningThreshold && usecNow > t.m_usecIgnoreLongLockWaitTimeUntil )
		{
			if ( pszTag )
				SpewWarning( "Waited %.1fms for SteamNetworkingSockets lock [%s]", usecTimeSpentWaitingOnLock*1e-3, pszTag );
			else
				SpewWarning( "Waited %.1fms for SteamNetworkingSockets lock", usecTimeSpentWaitingOnLock*1e-3 );
			ETW_LongOp( "lock wait", usecTimeSpentWaitingOnLock, pszTag );
		}

		auto callback = s_fLockAcquiredCallback; // save to temp, to prevent very narrow race condition where variable is cleared after we null check it, and we call null
		if ( callback )
			callback( pszTag, usecTimeSpentWaitingOnLock );

		t.m_usecOuterLockStartTime = usecNow;
	}

	t.AddTag( pszTag );
}

void LockDebugInfo::AboutToUnlock()
{
	char tags[ 256 ];

	SteamNetworkingMicroseconds usecElapsed = 0;
	SteamNetworkingMicroseconds usecElapsedTooLong = 0;
	auto lockHeldCallback = s_fLockHeldCallback;

	ThreadLockDebugInfo &t = GetThreadDebugInfo();
	Assert( t.m_nHeldLocks > 0 );

	// Unlocking the last lock?
	if ( t.m_nHeldLocks == 1 )
	{

		// We're about to do the final release.  How long did we hold the lock?
		usecElapsed = SteamNetworkingSockets_GetLocalTimestamp() - t.m_usecOuterLockStartTime;

		// Too long?  We need to check the threshold here because the threshold could
		// change by another thread immediately after we release the lock.  Also, if
		// we're debugging, all bets are off.  They could have hit a breakpoint, and
		// we don't want to create a bunch of confusing spew with spurious asserts
		if ( usecElapsed >= t.m_usecLongLockWarningThreshold && !Plat_IsInDebugSession() )
		{
			usecElapsedTooLong = usecElapsed;
		}

		if ( usecElapsedTooLong > 0 || lockHeldCallback )
		{
			char *p = tags;
			char *end = tags + sizeof(tags) - 1;
			for ( int i = 0 ; i < t.m_nTags && p+5 < end ; ++i )
			{
				if ( p > tags )
					*(p++) = ',';

				const ThreadLockDebugInfo::Tag_t &tag = t.m_arTags[i];
				int taglen = std::min( int(end-p), (int)V_strlen( tag.m_pszTag ) );
				memcpy( p, tag.m_pszTag, taglen );
				p += taglen;

				if ( tag.m_nCount > 1 )
				{
					int l = end-p;
					if ( l <= 5 )
						break;
					p += V_snprintf( p, l, "(x%d)", tag.m_nCount );
				}
			}
			*p = '\0';
		}

		t.m_nTags = 0;
		t.m_usecOuterLockStartTime = 0; // Just for grins.
	}

	if ( usecElapsed > 0 && lockHeldCallback )
	{
		lockHeldCallback(tags, usecElapsed);
	}

	// Yelp if we held the lock for longer than the threshold.
	if ( usecElapsedTooLong != 0 )
	{
		SpewWarning(
			"SteamNetworkingSockets lock held for %.1fms.  (Performance warning.)  %s\n"
			"This is usually a symptom of a general performance problem such as thread starvation.",
			usecElapsedTooLong*1e-3, tags
		);
		ETW_LongOp( "lock held", usecElapsedTooLong, tags );
	}

	// NOTE: We are allowed to unlock out of order!  We specifically
	// do this with the table lock!
	for ( int i = t.m_nHeldLocks-1 ; i >= 0 ; --i )
	{
		if ( t.m_arHeldLocks[i] == this )
		{
			--t.m_nHeldLocks;
			if ( i < t.m_nHeldLocks ) // Don't do the memmove in the common case of stack pop
				memmove( &t.m_arHeldLocks[i], &t.m_arHeldLocks[i+1], (t.m_nHeldLocks-i) * sizeof(t.m_arHeldLocks[0]) );
			t.m_arHeldLocks[t.m_nHeldLocks] = nullptr; // Just for grins
			return;
		}
	}

	AssertMsg( false, "Unlocked a lock '%s' that wasn't held?", m_pszName );
}

void LockDebugInfo::_AssertHeldByCurrentThread( const char *pszFile, int line, const char *pszTag ) const
{
	ThreadLockDebugInfo &t = GetThreadDebugInfo();
	for ( int i = t.m_nHeldLocks-1 ; i >= 0 ; --i )
	{
		if ( t.m_arHeldLocks[i] == this )
		{
			t.AddTag( pszTag );
			return;
		}
	}

	AssertMsg( false, "%s(%d): Lock '%s' not held", pszFile, line, m_pszName );
}

void SteamNetworkingGlobalLock::SetLongLockWarningThresholdMS( const char *pszTag, int msWarningThreshold )
{
	AssertHeldByCurrentThread( pszTag );
	SteamNetworkingMicroseconds usecWarningThreshold = SteamNetworkingMicroseconds{msWarningThreshold}*1000;
	ThreadLockDebugInfo &t = GetThreadDebugInfo();
	if ( t.m_usecLongLockWarningThreshold < usecWarningThreshold )
	{
		t.m_usecLongLockWarningThreshold = usecWarningThreshold;
		t.m_usecIgnoreLongLockWaitTimeUntil = SteamNetworkingSockets_GetLocalTimestamp() + t.m_usecLongLockWarningThreshold;
	}
}

void SteamNetworkingGlobalLock::_AssertHeldByCurrentThread( const char *pszFile, int line )
{
	s_mutexGlobalLock._AssertHeldByCurrentThread( pszFile, line, nullptr );
}

void SteamNetworkingGlobalLock::_AssertHeldByCurrentThread( const char *pszFile, int line, const char *pszTag )
{
	s_mutexGlobalLock._AssertHeldByCurrentThread( pszFile, line, pszTag );
}

void AssertGlobalLockHeldExactlyOnce()
{
	ThreadLockDebugInfo &t = GetThreadDebugInfo();
	Assert( t.m_nHeldLocks == 1 && t.m_arHeldLocks[0] == &s_mutexGlobalLock );
}

#endif // #if STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL > 0

void SteamNetworkingGlobalLock::Lock( const char *pszTag )
{
	s_mutexGlobalLock.lock( pszTag );
}

bool SteamNetworkingGlobalLock::TryLock( const char *pszTag, int msTimeout )
{
	return s_mutexGlobalLock.try_lock_for( msTimeout, pszTag );
}

void SteamNetworkingGlobalLock::Unlock()
{
	s_mutexGlobalLock.unlock();
}

} // namespace SteamNetworkingSocketsLib

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockWaitWarningThreshold( SteamNetworkingMicroseconds usecTheshold )
{
	#if STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL > 0
		s_usecLockWaitWarningThreshold = usecTheshold;
	#endif
	s_usecServiceThreadLockWaitWarning = usecTheshold;
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockAcquiredCallback( void (*callback)( const char *tags, SteamNetworkingMicroseconds usecWaited ) )
{
	#if STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL > 0
		s_fLockAcquiredCallback = callback;
	#else
		// Should we assert here?
	#endif
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetLockHeldCallback( void (*callback)( const char *tags, SteamNetworkingMicroseconds usecWaited ) )
{
	#if STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL > 0
		s_fLockHeldCallback = callback;
	#else
		// Should we assert here?
	#endif
}

/////////////////////////////////////////////////////////////////////////////
//
// Local timestamp
//
/////////////////////////////////////////////////////////////////////////////

namespace SteamNetworkingSocketsLib {

static std::atomic<long long> s_usecTimeLastReturned;

// Start with an offset so that a timestamp of zero is always pretty far in the past.
// But round it up to nice round number, so that looking at timestamps in the debugger
// is easy to read.
const long long k_nInitialTimestampMin = k_nMillion*24*3600*30;
const long long k_nInitialTimestamp = 3000000000000ll;
COMPILE_TIME_ASSERT( 2000000000000ll < k_nInitialTimestampMin );
COMPILE_TIME_ASSERT( k_nInitialTimestampMin < k_nInitialTimestamp );
static std::atomic<long long> s_usecTimeOffset( k_nInitialTimestamp );

// Max delta allowed between GetLocalTimestamp calls before we clamp.
// Matches the service thread's max poll interval (k_msMaxPollWait * 1100us).
constexpr SteamNetworkingMicroseconds k_usecMaxTimestampDelta = 1100 * 1000;

// Defined in steamnetworkingsockets_socketthread.cpp
extern std::atomic<int> s_nLowLevelSupportRefCount;

} // namespace SteamNetworkingSocketsLib

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
		if ( usecElapsed <= SteamNetworkingSocketsLib::k_usecMaxTimestampDelta )
		{
			// Should be the common case - only a relatively small of time has elapsed
			break;
		}
		if ( SteamNetworkingSocketsLib::s_nLowLevelSupportRefCount.load(std::memory_order_acquire) <= 0 )
		{
			// We don't have any expectation that we should be updating the timer frequently,
			// so  a big jump in the value just means they aren't calling it very often
			break;
		}

		// NOTE: We should only rarely get here, and probably as a result of running under the debugger

		// Adjust offset so that delta between timestamps is limited
		long long usecNewOffset = usecOffset - ( usecElapsed - SteamNetworkingSocketsLib::k_usecMaxTimestampDelta );
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

/////////////////////////////////////////////////////////////////////////////
//
// memory override
//
/////////////////////////////////////////////////////////////////////////////

#include <tier0/memdbgoff.h>

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_MEM_OVERRIDE

namespace SteamNetworkingSocketsLib {
static bool s_bHasAllocatedMemory = false;

static void* (*s_pfn_malloc)( size_t s ) = malloc;
static void (*s_pfn_free)( void *p ) = free;
static void* (*s_pfn_realloc)( void *p, size_t s ) = realloc;
}

void *SteamNetworkingSockets_Malloc( size_t s )
{
	s_bHasAllocatedMemory = true;
	return (*s_pfn_malloc)( s );
}

void *SteamNetworkingSockets_Realloc( void *p, size_t s )
{
	s_bHasAllocatedMemory = true;
	return (*s_pfn_realloc)( p, s );
}

void SteamNetworkingSockets_Free( void *p )
{
	(*s_pfn_free)( p );
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetCustomMemoryAllocator(
	void* (*pfn_malloc)( size_t s ),
	void (*pfn_free)( void *p ),
	void* (*pfn_realloc)( void *p, size_t s )
) {
	Assert( !s_bHasAllocatedMemory ); // Too late!

	s_pfn_malloc = pfn_malloc;
	s_pfn_free = pfn_free;
	s_pfn_realloc = pfn_realloc;
}
#endif
