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

//-----------------------------------------------------------------------------
/// Misc networking utilities for checking the local networking environment
/// and estimating pings.
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
	virtual SteamNetworkingMicroseconds GetLocalTimestamp() = 0;

	/// Check if the ping data of sufficient recency is available, and if
	/// it's too old, start refreshing it.
	/// 
	/// Games that use the ping location information will typically
	/// want to call this at boot time, to make sure all prerequisites
	/// are ready.  Especially since the first measurement might take
	/// slightly longer than subsequent measurements.
	///
	/// Returns true if sufficiently recent data is already available.
	///
	/// Returns false if sufficiently recent data is not available.  In this
	/// case, ping measurement is initiated, if it is not already active.
	/// (You cannot restart a measurement already in progress.)
	///
	/// A FIXME event will be posted when measurement is completed.
	virtual bool CheckPingDataUpToDate( float flMaxAgeSeconds ) = 0;

	/// Return location info for the current host.  Returns the approximate
	/// age of the data, in seconds, or -1 if no data is available.
	/// Note that this might return an age older than the age of your game's
	/// process, if the data was obtained before you game started.
	///
	/// This always return the most up-to-date information we have available
	/// right now, even if we are in the middle of re-calculating ping times.
	virtual float GetLocalPingLocation( SteamNetworkPingLocation_t &result ) = 0;

	/// Return true if we are taking ping measurements to update our ping
	/// location or select optimal routing.  Ping measurement typically takes
	/// a few seconds, perhaps up to 10 seconds.
	virtual bool IsPingMeasurementInProgress() = 0;

	/// Estimate the round-trip latency between two arbitrary locations, in
	/// milliseconds.  This is a conservative estimate, based on routing through
	/// the relay network.  For most basic connections based on SteamID,
	/// this ping time will be pretty accurate, since it will be based on the
	/// route likely to be actually used.
	///
	/// If a direct IP route is used (perhaps via NAT traversal), then the route
	/// will be different, and the ping time might be better.  Or it might actually
	/// be a bit worse!  Standard IP routing is frequently suboptimal!
	///
	/// But even in this case, the estimate obtained using this method is a
	/// reasonable upper bound on the ping time.  (Also it has the advantage
	/// of returning immediately and not sending any packets.)
	///
	/// In a few cases we might not able to estimate the route.  In this case
	/// a negative value is returned.  k_nSteamNetworkingPing_Failed means
	/// the reason was because of some networking difficulty.  (Failure to
	/// ping, etc)  k_nSteamNetworkingPing_Unknown is returned if we cannot
	/// currently answer the question for some other reason.
	virtual int EstimatePingTimeBetweenTwoLocations( const SteamNetworkPingLocation_t &location1, const SteamNetworkPingLocation_t &location2 ) = 0;

	/// Same as EstimatePingTime, but assumes that one location is the local host.
	/// This is a bit faster, especially if you need to calculate a bunch of
	/// these in a loop to find the fastest one.
	///
	/// In rare cases this might return a slightly different estimate than combining
	/// GetLocalPingLocation with EstimatePingTimeBetweenTwoLocations.  That's because
	/// this function uses a slightly more complete set of information about what
	/// route would be taken.
	virtual int EstimatePingTimeFromLocalHost( const SteamNetworkPingLocation_t &remoteLocation ) = 0;

	//
	// List of valve data centers, and ping times to them.
	//

	/// Fetch ping time of best available relayed route from this host to
	/// the specified data center.
	virtual int GetPingToDataCenter( SteamNetworkingPOPID popID, SteamNetworkingPOPID *pViaRelayPoP ) = 0;

	/// Get *direct* ping time to the relays at the data center.
	virtual int GetDirectPingToPOP( SteamNetworkingPOPID popID ) = 0;

	/// Get number of network points of presence in the config
	virtual int GetPOPCount() = 0;

	/// Get list of all POP IDs.  Returns the number of entries that were filled into
	/// your list.
	virtual int GetPOPList( SteamNetworkingPOPID *list, int nListSz ) = 0;
};
#define STEAMNETWORKINGUTILS_VERSION "SteamNetworkingUtils001"

/// Get ISteamNetworkingUtils object.  This will eventually go in Steam_api.h with all the rest of its kin
STEAMDATAGRAMLIB_INTERFACE ISteamNetworkingUtils *SteamNetworkingUtils();

#endif // ISTEAMNETWORKINGUTILS
