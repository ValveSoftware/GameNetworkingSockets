//====== Copyright Valve Corporation, All rights reserved. ====================

#ifdef __GNUC__
	// src/public/tier0/basetypes.h:104:30: error: assuming signed overflow does not occur when assuming that (X + c) < X is always false [-Werror=strict-overflow]
	// current steamrt:scout gcc "g++ (SteamRT 4.8.4-1ubuntu15~12.04+steamrt1.2+srt1) 4.8.4" requires this at the top due to optimizations
	#pragma GCC diagnostic ignored "-Wstrict-overflow"
#endif

#include <tier1/utlpriorityqueue.h>

#include "steamnetworkingsockets_thinker.h"

#ifdef IS_STEAMDATAGRAMROUTER
	#include "router/sdr.h"
#else
	#include "clientlib/steamnetworkingsockets_lowlevel.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// Periodic processing
//
/////////////////////////////////////////////////////////////////////////////

struct ThinkerLess
{
	bool operator()( const IThinker *a, const IThinker *b ) const
	{
		return a->GetNextThinkTime() > b->GetNextThinkTime();
	}
};
class ThinkerSetIndex
{
public:
	static void SetIndex( IThinker *p, int idx ) { p->m_queueIndex = idx; }
};

static CUtlPriorityQueue<IThinker*,ThinkerLess,ThinkerSetIndex> s_queueThinkers;

IThinker::IThinker()
: m_usecNextThinkTime( k_nThinkTime_Never )
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

void IThinker::SetNextThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime )
{
	// Protect against us blowing up because of an invalid think time.
	// Zero is reserved (since it often means there is an uninitialized value),
	// and our initial time value is effectively infinite compared to the
	// intervals we deal with in this code, so we should never need to deal
	// with a timestamp that far in the past.  See k_nThinkTime_ASAP
	if ( unlikely( usecTargetThinkTime <= 0 ) )
	{
		AssertMsg1( false, "Attempt to set target think time to %lld", (long long)usecTargetThinkTime );
		usecTargetThinkTime = SteamNetworkingSockets_GetLocalTimestamp() + 2000;
	}

	// Clearing it?
	if ( usecTargetThinkTime == k_nThinkTime_Never )
	{
		if ( m_queueIndex >= 0 )
		{
			Assert( s_queueThinkers.Element( m_queueIndex ) == this );
			s_queueThinkers.RemoveAt( m_queueIndex );
			Assert( m_queueIndex == -1 );
		}

		m_usecNextThinkTime = k_nThinkTime_Never;
		return;
	}

	// Save current time when the next thinker wants service
	#ifndef IS_STEAMDATAGRAMROUTER
		SteamNetworkingMicroseconds usecNextWake = ( s_queueThinkers.Count() > 0 ) ? s_queueThinkers.ElementAtHead()->GetNextThinkTime() : k_nThinkTime_Never;
	#endif

	// Not currently scheduled?
	if ( m_queueIndex < 0 )
	{
		Assert( m_usecNextThinkTime == k_nThinkTime_Never );
		m_usecNextThinkTime = usecTargetThinkTime;
		s_queueThinkers.Insert( this );
	}
	else
	{

		// We're already scheduled.
		Assert( s_queueThinkers.Element( m_queueIndex ) == this );
		Assert( m_usecNextThinkTime != k_nThinkTime_Never );

		// Set the new schedule time
		m_usecNextThinkTime = usecTargetThinkTime;

		// And update our position in the queue
		s_queueThinkers.RevaluateElement( m_queueIndex );
	}

	// Check that we know our place
	Assert( m_queueIndex >= 0 );
	Assert( s_queueThinkers.Element( m_queueIndex ) == this );

	#ifndef IS_STEAMDATAGRAMROUTER
		// Do we need service before we were previously schedule to wake up?
		// If so, wake the thread now so that it can redo its schedule work
		// NOTE: On Windows we could use a waitable timer.  This would avoid
		// waking up the service thread just to re-schedule when it should
		// wake up for real.
		if ( m_usecNextThinkTime < usecNextWake )
			WakeSteamDatagramThread();
	#endif
}

IThinker *Thinker_GetNextScheduled()
{
	if ( s_queueThinkers.Count() == 0 )
		return nullptr;
	return s_queueThinkers.ElementAtHead();
}

void Thinker_ProcessThinkers()
{

	// Until the queue is empty
	int nIterations = 0;
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

		// Scheduled too far in the future?
		if ( pNextThinker->GetNextThinkTime() >= usecNow )
		{
			// Keep waiting
			break;
		}

		++nIterations;
		if ( nIterations > 10000 )
		{
			AssertMsg1( false, "Processed thinkers %d times -- probably one thinker keeps requesting an immediate wakeup call.", nIterations );
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

#ifdef DBGFLAG_VALIDATE
void Thinker_ValidateStatics( CValidator &validator )
{
	ValidateObj( s_queueThinkers );
}
#endif

} // namespace SteamNetworkingSocketsLib

