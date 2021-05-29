//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_THINKER_H
#define STEAMNETWORKINGSOCKETS_THINKER_H
#pragma once

#include "steamnetworkingsockets_internal.h"

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
	STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW
	virtual ~IThinker();

	/// Called to set when you next want to get your Think() callback.
	/// You should assume that, due to scheduler inaccuracy, you could
	/// get your callback 1 or 2 ms late.
	void SetNextThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime );

	/// Adjust schedule time to the earlier of the current schedule time,
	/// or the given time.
	inline void EnsureMinThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime )
	{
		if ( usecTargetThinkTime < m_usecNextThinkTime )
			InternalEnsureMinThinkTime( usecTargetThinkTime );
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

	/// Try to acquire our lock.  Returns false if we fail.
	virtual bool TryLock() const;

	static void Thinker_ProcessThinkers();
	static SteamNetworkingMicroseconds Thinker_GetNextScheduledThinkTime();
protected:
	IThinker();

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

private:
	SteamNetworkingMicroseconds m_usecNextThinkTime;
	int m_queueIndex;
	friend class ThinkerSetIndex;

	void InternalSetNextThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime );
	void InternalEnsureMinThinkTime( SteamNetworkingMicroseconds usecTargetThinkTime );
public:
	#ifdef DBGFLAG_VALIDATE
	virtual void Validate( CValidator &validator, const char *pchName );
	#endif
};
template <typename L>
class ILockableThinker : public IThinker
{
public:
	virtual bool TryLock() const final { return m_pLock->try_lock(); }
	inline void Unlock() { m_pLock->unlock(); }

	L *m_pLock;
protected:
	ILockableThinker( L &lock ) : IThinker(), m_pLock( &lock ) {}
};

#ifdef DBGFLAG_VALIDATE
extern void Thinker_ValidateStatics( CValidator &validator );
#endif

/// A thinker that calls a method
template<typename TOuter>
class ScheduledMethodThinker : private IThinker
{
public:

	/// Required method signature accepts the current time as the only argument.  (Other than implicit "this")
	typedef void (TOuter::*TMethod)( SteamNetworkingMicroseconds );

	/// Default constructor doesn't set outer object or method
	ScheduledMethodThinker() : m_pOuter( nullptr ), m_method( nullptr ) {}

	/// You can specify the object and method in the constructor, if that's more convenient
	ScheduledMethodThinker( TOuter *pOuter, TMethod method ) : m_pOuter( pOuter ), m_method( method ) {}

	/// Schedule to invoke the method at the specified time.  You must have previously specified
	/// the target object and method.
	inline void Schedule( SteamNetworkingMicroseconds usecWhen ) { Assert( m_pOuter && m_method ); IThinker::SetNextThinkTime( usecWhen ); }
	inline void ScheduleASAP() { Schedule( k_nThinkTime_ASAP ); }

	/// Schedule to invoke the specified method on the specified object, at the specified time.
	inline void Schedule( TOuter *pOuter, TMethod method, SteamNetworkingMicroseconds usecWhen )
	{
		Cancel(); // !SPEED! If we wrapped this whole thing with the thinker lock, we could avoid this
		m_pOuter = pOuter;
		m_method = method;
		Schedule( usecWhen );
	}
	inline void ScheduleASAP( TOuter *pOuter, TMethod method ) { Schedule( pOuter, method, k_nThinkTime_ASAP ); }

	/// Adjust schedule time to the earlier of the current schedule time,
	/// or the given time.
	inline void EnsureMinScheduleTime( SteamNetworkingMicroseconds usecWhen ) { Assert( m_pOuter && m_method ); EnsureMinThinkTime( usecWhen ); }
	inline void EnsureMinScheduleTime( TOuter *pOuter, TMethod method, SteamNetworkingMicroseconds usecWhen )
	{
		Cancel(); // !SPEED! If we wrapped this whole thing with the thinker lock, we could avoid this
		m_pOuter = pOuter;
		m_method = method;
		EnsureMinScheduleTime( usecWhen );
	}

	/// If currently scheduled, cancel it
	inline void Cancel() { IThinker::SetNextThinkTime( k_nThinkTime_Never ); }

	/// Return true if we are currently scheduled
	using IThinker::IsScheduled;

	/// Return current time that we are scheduled to be called.  (Returns k_nThinkTime_Never if not scheduled.)
	inline SteamNetworkingMicroseconds GetScheduleTime() const { return IThinker::GetNextThinkTime(); }

protected:
	TOuter *m_pOuter;
	TMethod m_method;

	// Think Thunk
	virtual void Think( SteamNetworkingMicroseconds usecNow )
	{
		if ( m_pOuter )
			(m_pOuter->*m_method)( usecNow );
	}
};

/// A thinker that calls a method on an object that can try to lock itself
template<typename TOuter>
class ScheduledMethodThinkerLockable : public ScheduledMethodThinker<TOuter>
{
public:
	using super=ScheduledMethodThinker<TOuter>;
	using super::ScheduledMethodThinker; // Inherit constructors

	virtual bool TryLock() const override
	{
		return !super::m_pOuter || super::m_pOuter->TryLock();
	}

	virtual void Think( SteamNetworkingMicroseconds usecNow ) override
	{
		if ( super::m_pOuter )
		{
			(super::m_pOuter->*super::m_method)( usecNow );
			// NOTE: We assume that caller will not self-destruct!
			// This is too complicated to untangle.  If you hit this,
			// use a different pattern.
			super::m_pOuter->Unlock();
		}
	}

};

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_THINKER_H
