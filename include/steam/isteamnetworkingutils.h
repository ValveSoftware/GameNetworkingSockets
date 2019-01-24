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
STEAMNETWORKINGSOCKETS_INTERFACE SteamNetworkingMicroseconds SteamNetworkingSockets_GetLocalTimestamp();
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetDebugOutputFunction( ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc );
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

	/// Set a function to receive network-related information that is useful for debugging.
	/// This can be very useful during development, but it can also be useful for troubleshooting
	/// problems with tech savvy end users.  If you have a console or other log that customers
	/// can examine, these log messages can often be helpful to troubleshoot network issues.
	/// (Especially any warning/error messages.)
	///
	/// The detail level indicates what message to invoke your callback on.  Lower numeric
	/// value means more important, and the value you pass is the lowest priority (highest
	/// numeric value) you wish to receive callbacks for.
	///
	/// Except when debugging, you should only use k_ESteamNetworkingSocketsDebugOutputType_Msg
	/// or k_ESteamNetworkingSocketsDebugOutputType_Warning.  For best performance, do NOT
	/// request a high detail level and then filter out messages in your callback.  Instead,
	/// call function function to adjust the desired level of detail.
	///
	/// IMPORTANT: This may be called from a service thread, while we own a mutex, etc.
	/// Your output function must be threadsafe and fast!  Do not make any other
	/// Steamworks calls from within the handler.
	inline static void SetDebugOutputFunction( ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc )
	{
		SteamNetworkingSockets_SetDebugOutputFunction( eDetailLevel, pfnFunc );
	}
};

/// Dummy.  This just returns something non-null.  (In case you have code that
/// checks if this function is returning null.)
inline ISteamNetworkingUtils *SteamNetworkingUtils() { return reinterpret_cast<ISteamNetworkingUtils*>( 1 ); }

#endif // ISTEAMNETWORKINGUTILS
