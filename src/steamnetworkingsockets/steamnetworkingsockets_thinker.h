//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_THINKER_H
#define STEAMNETWORKINGSOCKETS_THINKER_H
#pragma once

#include <steam/steamnetworkingtypes.h>

namespace SteamNetworkingSocketsLib {

/////////////////////////////////////////////////////////////////////////////
//
// Periodic processing
//
/////////////////////////////////////////////////////////////////////////////

const SteamNetworkingMicroseconds k_nThinkTime_Never = INT64_MAX;
const SteamNetworkingMicroseconds k_nThinkTime_ASAP = 1; // by convention, we do not allow setting a think time to 0, since 0 is often an uninitialized variable.
class ThinkerSetIndex;

class IThinker
{
public:
	virtual ~IThinker();

	/// Callback to do whatever periodic processing you need.  If you don't
	/// explicitly call SetNextThinkTime inside this function, then thinking
	/// will be disabled.
	///
	/// Think callbacks will always happen from the service thread,
	/// with the lock held.
	///
	/// Note that we assume a limited precision of the thread scheduler,
	/// and you won't get your callback exactly when you request.
	virtual void Think( SteamNetworkingMicroseconds usecNow ) = 0;

	/// Called to set when you next want to get your Think() callback.
	/// You should assume that, due to scheduler inaccuracy, you could
	/// get your callback 1 or 2 ms late.
	void SetNextThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime );

	/// Adjust schedule time to the earlier of the current schedule time,
	/// or the given time.
	inline void EnsureMinThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime )
	{
		if ( usecTargetThinkTime < m_usecNextThinkTime )
			SetNextThinkTime( usecTargetThinkTime );
	}

	/// Clear the next think time.  You won't get a callback.
	void ClearNextThinkTime() { SetNextThinkTime( k_nThinkTime_Never ); }

	/// Request an immediate wakeup.
	void SetNextThinkTimeASAP() { EnsureMinThinkTime( k_nThinkTime_ASAP ); }

	/// Fetch time when the next Think() call is currently scheduled to
	/// happen.
	inline SteamNetworkingMicroseconds GetNextThinkTime() const { return m_usecNextThinkTime; }

	/// Return true if we are scheduled to get our callback
	inline bool IsScheduled() const { return m_usecNextThinkTime != k_nThinkTime_Never; }

protected:
	IThinker();

private:
	SteamNetworkingMicroseconds m_usecNextThinkTime;
	int m_queueIndex;
	friend class ThinkerSetIndex;
};

extern IThinker *Thinker_GetNextScheduled();
extern void Thinker_ProcessThinkers();

#ifdef DBGFLAG_VALIDATE
extern void Thinker_ValidateStatics( CValidator &validator );
#endif

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_THINKER_H
