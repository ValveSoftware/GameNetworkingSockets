//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Purpose: misc networking utilities
//
//=============================================================================

#ifndef ISTEAMNETWORKINGUTILS
#define ISTEAMNETWORKINGUTILS
#ifdef _WIN32
#pragma once
#endif

#include "steamnetworkingtypes.h"
struct SteamDatagramRelayAuthTicket;

extern "C" {

// Fetch local timestamp.  If you want to stay compatible with Steamworks SDK,
// don't call this directly.  Use the ISteamnetworkingUtils interface
STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingMicroseconds SteamNetworkingSockets_GetLocalTimestamp();

}

//-----------------------------------------------------------------------------
class ISteamNetworkingUtils
{
public:

	/// Fetch current timestamp.  This timer has the following properties:
	///
	/// - Monotonicity is guaranteed.
	/// - The initial value will be at least 24*3600*30*1e6, i.e. about
	///   30 days worth of milliseconds.  In this way, the timestamp value of
	///   0 will always be at least "30 days ago".  Also, negative numbers
	///   will never be returned.
	/// - Wraparound / overflow is not a practical concern.
	///
	/// If you are running under the debugger and stop the process, the clock
	/// might not advance the full wall clock time that has elapsed between
	/// calls.  If the process is not blocked from normal operation, the
	/// timestamp values will track wall clock time, even if you don't call
	/// the function frequently.
	///
	/// The value is only meaningful for this run of the process.  Don't compare
	/// it to values obtained on another computer, or other runs of the same process.
	inline static SteamNetworkingMicroseconds GetLocalTimestamp()
	{
		return SteamNetworkingSockets_GetLocalTimestamp();
	}
};

/// Dummy.  This just returns something non-null.  (In case you have code that
/// checks if this function is returning null,)
inline ISteamNetworkingUtils *SteamNetworkingUtils() { return reinterpret_cast<ISteamNetworkingUtils*>( 1 ); }

#endif // ISTEAMNETWORKINGUTILS
