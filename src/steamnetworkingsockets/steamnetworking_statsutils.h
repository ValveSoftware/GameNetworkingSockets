//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Utils for calculating networking stats
//
//=============================================================================

#ifndef STEAMNETWORKING_STATSUTILS_H
#define STEAMNETWORKING_STATSUTILS_H
#pragma once

#include <tier0/basetypes.h>
#include <tier0/t0constants.h>
#include "percentile_generator.h"
#include "steamnetworking_stats.h"
#include "steamnetworkingsockets_internal.h"
#include "steamnetworkingsockets_thinker.h"

//#include <google/protobuf/repeated_field.h> // FIXME - should only need this!
#include <tier0/memdbgoff.h>
#include <steamnetworkingsockets_messages.pb.h>
#include <tier0/memdbgon.h>

class CMsgSteamDatagramConnectionQuality;

// Internal stuff goes in a private namespace
namespace SteamNetworkingSocketsLib {

/// Default interval for link stats rate measurement
const SteamNetworkingMicroseconds k_usecSteamDatagramLinkStatsDefaultInterval = 5 * k_nMillion;

/// Default interval for speed stats rate measurement
const SteamNetworkingMicroseconds k_usecSteamDatagramSpeedStatsDefaultInterval = 1 * k_nMillion;

/// We should send tracer ping requests in our packets on approximately
/// this interval.  (Tracer pings and their replies are relatively cheap.)
/// These serve both as latency measurements, and also as keepalives, if only
/// one side or the other is doing most of the talking, to make sure the other side
/// always does a minimum amount of acking.
const SteamNetworkingMicroseconds k_usecLinkStatsMinPingRequestInterval = 5 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsMaxPingRequestInterval = 7 * k_nMillion;

/// Client should send instantaneous connection quality stats
/// at approximately this interval
const SteamNetworkingMicroseconds k_usecLinkStatsInstantaneousReportInterval = 20 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsInstantaneousReportMaxInterval = 30 * k_nMillion;

/// Client will report lifetime connection stats at approximately this interval
const SteamNetworkingMicroseconds k_usecLinkStatsLifetimeReportInterval = 120 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsLifetimeReportMaxInterval = 140 * k_nMillion;

/// If we are timing out, ping the peer on this interval
const SteamNetworkingMicroseconds k_usecAggressivePingInterval = 200*1000;

/// If we haven't heard from the peer in a while, send a keepalive
const SteamNetworkingMicroseconds k_usecKeepAliveInterval = 10*k_nMillion;

/// Track the rate that something is happening
struct Rate_t
{
	void Reset() { memset( this, 0, sizeof(*this) ); }

	int64	m_nCurrentInterval;
	int64	m_nAccumulator; // does not include the currentinterval
	float	m_flRate;

	int64 Total() const { return m_nAccumulator + m_nCurrentInterval; }

	inline void Process( int64 nIncrement )
	{
		m_nCurrentInterval += nIncrement;
	}

	inline void UpdateInterval( float flIntervalDuration )
	{
		m_flRate = float(m_nCurrentInterval) / flIntervalDuration;
		m_nAccumulator += m_nCurrentInterval;
		m_nCurrentInterval = 0;
	}

	inline void operator+=( const Rate_t &x )
	{
		m_nCurrentInterval += x.m_nCurrentInterval;
		m_nAccumulator += x.m_nAccumulator;
		m_flRate += x.m_flRate;
	}
};

/// Track flow rate (number and bytes)
struct PacketRate_t
{
	void Reset() { memset( this, 0, sizeof(*this) ); }

	Rate_t m_packets;
	Rate_t m_bytes;

	inline void ProcessPacket( int sz )
	{
		m_packets.Process( 1 );
		m_bytes.Process( sz );
	}

	void UpdateInterval( float flIntervalDuration )
	{
		m_packets.UpdateInterval( flIntervalDuration );
		m_bytes.UpdateInterval( flIntervalDuration );
	}

	inline void operator+=( const PacketRate_t &x )
	{
		m_packets += x.m_packets;
		m_bytes += x.m_bytes;
	}
};

/// Class used to track ping values
struct PingTracker
{

	struct Ping
	{
		int m_nPingMS;
		SteamNetworkingMicroseconds m_usecTimeRecv;
	};

	/// Recent ping measurements.  The most recent one is at entry 0.
	Ping m_arPing[ 3 ];

	/// Number of valid entries in m_arPing.
	int m_nValidPings;

	/// Time when the most recent ping was received
	SteamNetworkingMicroseconds TimeRecvMostRecentPing() const { return m_arPing[0].m_usecTimeRecv; }

	/// Return the worst of the pings in the small sample of recent pings
	int WorstPingInRecentSample() const;

	/// Estimate a conservative (i.e. err on the large side) timeout for the connection
	SteamNetworkingMicroseconds CalcConservativeTimeout() const
	{
		constexpr SteamNetworkingMicroseconds k_usecMaxTimeout = 1250000;
		if ( m_nSmoothedPing < 0 )
			return k_usecMaxTimeout;
		return std::min( SteamNetworkingMicroseconds{ WorstPingInRecentSample()*2000 + 250000 }, k_usecMaxTimeout );
	}

	/// Smoothed ping value
	int m_nSmoothedPing;

	/// Time when we last sent a message, for which we expect a reply (possibly delayed)
	/// that we could use to measure latency.  (Possibly because the reply contains
	/// a simple timestamp, or possibly because it will contain a sequence number, and
	/// we will be able to look up that sequence number and remember when we sent it.)
	SteamNetworkingMicroseconds m_usecTimeLastSentPingRequest;
protected:
	void Reset();

	/// Called when we receive a ping measurement
	void ReceivedPing( int nPingMS, SteamNetworkingMicroseconds usecNow );
};

/// Ping tracker that tracks detailed lifetime stats
struct PingTrackerDetailed : PingTracker
{
	void Reset()
	{
		PingTracker::Reset();
		m_sample.Clear();
		m_histogram.Reset();
	}
	void ReceivedPing( int nPingMS, SteamNetworkingMicroseconds usecNow )
	{
		PingTracker::ReceivedPing( nPingMS, usecNow );
		m_sample.AddSample( std::min( nPingMS, 0xffff ) );
		m_histogram.AddSample( nPingMS );
	}

	/// Track sample of pings received so we can generate percentiles.
	/// Also tracks how many pings we have received total
	PercentileGenerator<uint16> m_sample;

	/// Counts by bucket
	PingHistogram m_histogram;

	/// Populate structure
	void GetLifetimeStats( SteamDatagramLinkLifetimeStats &s ) const
	{
		s.m_pingHistogram  = m_histogram;

		s.m_nPingNtile5th  = m_sample.NumSamples() < 20 ? -1 : m_sample.GetPercentile( .05f );
		s.m_nPingNtile50th = m_sample.NumSamples() <  2 ? -1 : m_sample.GetPercentile( .50f );
		s.m_nPingNtile75th = m_sample.NumSamples() <  4 ? -1 : m_sample.GetPercentile( .75f );
		s.m_nPingNtile95th = m_sample.NumSamples() < 20 ? -1 : m_sample.GetPercentile( .95f );
		s.m_nPingNtile98th = m_sample.NumSamples() < 50 ? -1 : m_sample.GetPercentile( .98f );
	}
};

/// Before switching to a different route, we need to make sure that we have a ping
/// sample in at least N recent time buckets.  (See PingTrackerForRouteSelection)
const int k_nRecentValidTimeBucketsToSwitchRoute = 15;

/// Ping tracker that tracks samples over several intervals.  This is used
/// to make routing decisions in such a way to avoid route flapping when ping
/// times on different routes are fluctuating.
///
/// This class also has the concept of a user override, which is used to fake
/// a particular ping time for debugging.
struct PingTrackerForRouteSelection : PingTracker
{
	COMPILE_TIME_ASSERT( k_nRecentValidTimeBucketsToSwitchRoute == 15 );
	static constexpr int k_nTimeBucketCount = 17;
	static constexpr SteamNetworkingMicroseconds k_usecTimeBucketWidth = k_nMillion; // Desired width of each time bucket
	static constexpr int k_nPingOverride_None = -2; // Ordinary operation.  (-1 is a legit ping time, which means "ping failed")
	static constexpr SteamNetworkingMicroseconds k_usecAntiFlapRouteCheckPingInterval = 200*1000;

	struct TimeBucket
	{
		SteamNetworkingMicroseconds m_usecEnd; // End of this bucket.  The start of the bucket is m_usecEnd-k_usecTimeBucketWidth
		int m_nPingCount;
		int m_nMinPing; // INT_NAX if we have not received one
		int m_nMaxPing; // INT_MIN
	};
	TimeBucket m_arTimeBuckets[ k_nTimeBucketCount ];
	int m_idxCurrentBucket;
	int m_nTotalPingsReceived;
	int m_nPingOverride = k_nPingOverride_None;

	void Reset()
	{
		PingTracker::Reset();
		m_nTotalPingsReceived = 0;
		m_idxCurrentBucket = 0;
		for ( TimeBucket &b: m_arTimeBuckets )
		{
			b.m_usecEnd = 0;
			b.m_nPingCount = 0;
			b.m_nMinPing = INT_MAX;
			b.m_nMaxPing = INT_MIN;
		}
	}
	void ReceivedPing( int nPingMS, SteamNetworkingMicroseconds usecNow )
	{
		// Ping time override in effect?
		if ( m_nPingOverride > k_nPingOverride_None )
		{
			if ( m_nPingOverride == -1 )
				return;
			nPingMS = m_nPingOverride;
		}
		PingTracker::ReceivedPing( nPingMS, usecNow );
		++m_nTotalPingsReceived;

		SteamNetworkingMicroseconds usecCurrentBucketEnd = m_arTimeBuckets[ m_idxCurrentBucket ].m_usecEnd;
		if ( usecCurrentBucketEnd > usecNow )
		{
			TimeBucket &curBucket = m_arTimeBuckets[ m_idxCurrentBucket ];
			++curBucket.m_nPingCount;
			curBucket.m_nMinPing = std::min( curBucket.m_nMinPing, nPingMS );
			curBucket.m_nMaxPing = std::max( curBucket.m_nMaxPing, nPingMS );
		}
		else
		{
			++m_idxCurrentBucket;
			if ( m_idxCurrentBucket >= k_nTimeBucketCount )
				m_idxCurrentBucket = 0;
			TimeBucket &newBucket = m_arTimeBuckets[ m_idxCurrentBucket ];

			// If we are less than halfway into the new window, then start it immediately after
			// the previous one.
			if ( usecCurrentBucketEnd + (k_usecTimeBucketWidth/2) >= usecNow )
			{
				newBucket.m_usecEnd = usecCurrentBucketEnd + k_usecTimeBucketWidth;
			}
			else
			{
				// It's been more than half a window.  Start this window at the current time.
				newBucket.m_usecEnd = usecNow + k_usecTimeBucketWidth;
			}

			newBucket.m_nPingCount = 1;
			newBucket.m_nMinPing = nPingMS;
			newBucket.m_nMaxPing = nPingMS;
		}
	}

	void SetPingOverride( int nPing )
	{
		m_nPingOverride = nPing;
		if ( m_nPingOverride <= k_nPingOverride_None )
			return;
		if ( m_nPingOverride < 0 )
		{
			m_nValidPings = 0;
			m_nSmoothedPing = -1;
			return;
		}
		m_nSmoothedPing = nPing;
		for ( int i = 0 ; i < m_nValidPings ; ++i )
			m_arPing[i].m_nPingMS = nPing;
		TimeBucket &curBucket = m_arTimeBuckets[ m_idxCurrentBucket ];
		curBucket.m_nMinPing = nPing;
		curBucket.m_nMaxPing = nPing;
	}

	/// Return true if the next ping received will start a new bucket
	SteamNetworkingMicroseconds TimeToSendNextAntiFlapRouteCheckPingRequest() const
	{
		return std::min(
			m_arTimeBuckets[ m_idxCurrentBucket ].m_usecEnd, // time to start next bucket
			m_usecTimeLastSentPingRequest + k_usecAntiFlapRouteCheckPingInterval // and then send them at a given rate
		);
	}

	// Get the min/max ping value among recent buckets.
	// Returns the number of valid buckets used to collect the data.
	int GetPingRangeFromRecentBuckets( int &nOutMin, int &nOutMax, SteamNetworkingMicroseconds usecNow ) const
	{
		int nMin = m_nSmoothedPing;
		int nMax = m_nSmoothedPing;
		int nBucketsValid = 0;
		if ( m_nSmoothedPing >= 0 )
		{
			SteamNetworkingMicroseconds usecRecentEndThreshold = usecNow - ( (k_nTimeBucketCount-1) * k_usecTimeBucketWidth );
			for ( const TimeBucket &bucket: m_arTimeBuckets )
			{
				if ( bucket.m_usecEnd >= usecRecentEndThreshold )
				{
					Assert( bucket.m_nPingCount > 0 );
					Assert( 0 <= bucket.m_nMinPing );
					Assert( bucket.m_nMinPing <= bucket.m_nMaxPing );
					++nBucketsValid;
					nMin = std::min( nMin, bucket.m_nMinPing );
					nMax = std::max( nMax, bucket.m_nMaxPing );
				}
			}
		}
		nOutMin = nMin;
		nOutMax = nMax;
		return nBucketsValid;
	}
};

/// Token bucket rate limiter
/// https://en.wikipedia.org/wiki/Token_bucket
struct TokenBucketRateLimiter
{
	TokenBucketRateLimiter() { Reset(); }

	/// Mark the token bucket as full and reset internal timer
	void Reset() { m_usecLastTime = 0; m_flTokenDeficitFromFull = 0.0f; }

	/// Attempt to spend a token.
	/// flMaxSteadyStateRate - the rate that tokens are added to the bucket, per second.
	///                        Over a long interval, tokens cannot be spent faster than this rate.  And if they are consumed
	///                        at this rate, there is no allowance for bursting higher.  Typically you'll set this to a bit
	///                        higher than the true steady-state rate, so that the bucket can fill back up to allow for
	///                        another burst.
	/// flMaxBurst - The max possible burst, in tokens.
	bool BCheck( SteamNetworkingMicroseconds usecNow, float flMaxSteadyStateRate, float flMaxBurst )
	{
		Assert( flMaxBurst >= 1.0f );
		Assert( flMaxSteadyStateRate > 0.0f );

		// Calculate elapsed time (in seconds) and advance timestamp
		float flElapsed = ( usecNow - m_usecLastTime ) * 1e-6f;
		m_usecLastTime = usecNow;

		// Add tokens to the bucket, but stop if it gets full
		m_flTokenDeficitFromFull = Max( m_flTokenDeficitFromFull - flElapsed*flMaxSteadyStateRate, 0.0f );

		// Burst rate currently being exceeded?
		if ( m_flTokenDeficitFromFull + 1.0f > flMaxBurst )
			return false;

		// We have a token.  Spend it
		m_flTokenDeficitFromFull += 1.0f;
		return true;
	}

private:

	/// Last time a token was spent
	SteamNetworkingMicroseconds m_usecLastTime;

	/// The degree to which the bucket is not full.  E.g. 0 is "full" and any higher number means they are less than full.
	/// Doing the accounting in this "inverted" way makes it easier to reset and adjust the limits dynamically.
	float m_flTokenDeficitFromFull;
};

// Bitmask returned by GetStatsSendNeed
constexpr int k_nSendStats_Instantanous_Due = 1;
constexpr int k_nSendStats_Instantanous_Ready = 2;
constexpr int k_nSendStats_Lifetime_Due = 4;
constexpr int k_nSendStats_Lifetime_Ready = 8;
constexpr int k_nSendStats_Instantanous = k_nSendStats_Instantanous_Due|k_nSendStats_Instantanous_Ready;
constexpr int k_nSendStats_Lifetime = k_nSendStats_Lifetime_Due|k_nSendStats_Lifetime_Ready;
constexpr int k_nSendStats_Due = k_nSendStats_Instantanous_Due|k_nSendStats_Lifetime_Due;
constexpr int k_nSendStats_Ready = k_nSendStats_Instantanous_Ready|k_nSendStats_Lifetime_Ready;

/// Track quality stats based on flow of sequence numbers
struct SequencedPacketCounters
{
	int m_nRecv; // packets successfully received containing a sequence number
	int m_nDropped; // packets assumed to be dropped in the current interval
	int m_nOutOfOrder; // any sequence number deviation other than a simple dropped packet.  (Most recent interval.)
	int m_nLurch; // any sequence number deviation other than a simple dropped packet.  (Most recent interval.)
	int m_nDuplicate; // any sequence number deviation other than a simple dropped packet.  (Most recent interval.)
	int m_usecMaxJitter;

	void Reset()
	{
		m_nRecv = 0;
		m_nDropped = 0;
		m_nOutOfOrder = 0;
		m_nLurch = 0;
		m_nDuplicate = 0;
		m_usecMaxJitter = -1;
	}

	void Accumulate( const SequencedPacketCounters &x )
	{
		m_nRecv += x.m_nRecv;
		m_nDropped += x.m_nDropped;
		m_nOutOfOrder += m_nOutOfOrder;
		m_nLurch += m_nLurch;
		m_nDuplicate += x.m_nDuplicate;
		m_usecMaxJitter = std::max( m_usecMaxJitter, x.m_usecMaxJitter );
	}

	inline int Weird() const { return m_nOutOfOrder + m_nLurch + m_nDuplicate; }

	static inline float CalculateQuality( int nRecv, int nDropped, int nWeird )
	{
		Assert( nRecv >= nWeird );
		int nSent = nRecv + nDropped;
		if ( nSent <= 0 )
			return -1.0f;
		return (float)(nRecv - nWeird) / (float)nSent;
	}

	inline float CalculateQuality() const
	{
		return CalculateQuality( m_nRecv, m_nDropped, Weird() );
	}

	inline void OnRecv()
	{
		++m_nRecv;
	}
	inline void OnDropped( int nDropped )
	{
		m_nDropped += nDropped;
	}
	inline void OnDuplicate()
	{
		++m_nDuplicate;
	}
	inline void OnLurch()
	{
		++m_nLurch;
	}
	inline void OnOutOfOrder()
	{
		++m_nOutOfOrder;

		// We previously marked this as dropped.  Undo that
		if ( m_nDropped > 0 ) // Might have marked it in the previous interval.  Our stats will be slightly off in this case.  Not worth it to try to get this exactly right.
			--m_nDropped;
	}

};


/// Base class used to handle link quality calculations.
///
/// All extant instantiations will actually be LinkStatsTracker<T>, where T is the specific,
/// derived type.  There are several functions that, if we cared more about simplicity and less
/// about perf, would be defined as virtual functions here.  But many of these functions are tiny
/// and called in inner loops, and we want to ensure that the compiler is able to expand everything
/// inline and does not use virtual function dispatch.
///
/// So, if a function needs to be "virtual", meaning it can be overridden by a derived class, then
/// we name it with "Internal" here, and place a small wrapper in LinkStatsTracker<T> that will call
/// the correct version.  We never call the "Internal" one directly, except when invoking the base
/// class.  In this way, we make sure that we always call the most derived class version.
///
/// If a base class needs to *call* a virtual function, then we have a problem.  With a traditional
/// member function, we have type erasure.  The type of this is always the type of the method, not the
/// actual derived type.  To work around this, all methods that need to call virtual functions
/// are declared as static, accepting "this" (named "pThis") as a template argument, thus the type
/// is not erased.
///
/// All of this is weird, no doubt, but it achieves the goal of ensuring that the compiler can inline
/// all of these small functions if appropriate, and no virtual function dispatch is used.
struct LinkStatsTrackerBase
{


	/// What version is the peer running?  It's 0 if we don't know yet.
	uint32 m_nPeerProtocolVersion;

	/// Ping
	PingTrackerDetailed m_ping;

	//
	// Outgoing stats
	//
	int64 m_nNextSendSequenceNumber;
	PacketRate_t m_sent;
	SteamNetworkingMicroseconds m_usecTimeLastSentSeq;

	/// Called when we sent a packet, with or without a sequence number
	inline void TrackSentPacket( int cbPktSize )
	{
		m_sent.ProcessPacket( cbPktSize );
	}

	/// Consume the next sequence number, and record the time at which
	/// we sent a sequenced packet.  (Don't call this unless you are sending
	/// a sequenced packet.)
	inline uint16 ConsumeSendPacketNumberAndGetWireFmt( SteamNetworkingMicroseconds usecNow )
	{
		m_usecTimeLastSentSeq = usecNow;
		return uint16( m_nNextSendSequenceNumber++ );
	}

	//
	// Incoming
	//

	/// Highest (valid!) packet number we have ever processed
	int64 m_nMaxRecvPktNum;

	/// Packet and data rate trackers for inbound flow
	PacketRate_t m_recv;

	// TEMP delete this once I track down the accounting bug
	int64 m_nDebugLastInitMaxRecvPktNum;
	int64 m_nDebugPktsRecvInOrder;
	int64 m_arDebugHistoryRecvSeqNum[ 256 ];
	std::string HistoryRecvSeqNumDebugString( int nMaxPkts ) const;

	/// Setup state to expect the next packet to be nPktNum+1,
	/// and discard all packets <= nPktNum
	void InitMaxRecvPktNum( int64 nPktNum );
	void ResetMaxRecvPktNumForIncomingWirePktNum( uint16 nPktNum )
	{
		InitMaxRecvPktNum( (int64)(uint16)( nPktNum - 1 ) );
	}

	/// Bitmask of recently received packets, used to reject duplicate packets.
	/// (Important for preventing replay attacks.)
	///
	/// Let B be m_nMaxRecvPktNum & ~63.  (The largest multiple of 64
	/// that is <= m_nMaxRecvPktNum.)   Then m_recvPktNumberMask[1] bit n
	/// corresponds to B + n.  (Some of these bits may represent packet numbers
	/// higher than m_nMaxRecvPktNum.)  m_recvPktNumberMask[0] bit n
	/// corresponds to B - 64 + n.
	uint64 m_recvPktNumberMask[2];

	/// Get string describing state of recent packets received.
	std::string RecvPktNumStateDebugString() const;

	/// Packets that we receive that exceed the rate limit.
	/// (We might drop these, or we might just want to be interested in how often it happens.)
	PacketRate_t m_recvExceedRateLimit;

	/// Time when we last received anything
	SteamNetworkingMicroseconds m_usecTimeLastRecv;

	/// Time when we last received a sequenced packet
	SteamNetworkingMicroseconds m_usecTimeLastRecvSeq;

	/// Called when we receive any packet, with or without a sequence number.
	/// Does not perform any rate limiting checks
	inline void TrackRecvPacket( int cbPktSize, SteamNetworkingMicroseconds usecNow )
	{
		m_recv.ProcessPacket( cbPktSize );
		m_usecTimeLastRecv = usecNow;
		m_usecInFlightReplyTimeout = 0;
		m_nReplyTimeoutsSinceLastRecv = 0;
		m_usecWhenTimeoutStarted = 0;
	}

	//
	// Quality metrics stats
	//

	// Track instantaneous rate of number of sequence number anomalies
	SequencedPacketCounters m_seqPktCounters;

	// Instantaneous rates, calculated from most recent completed interval
	float m_flInPacketsDroppedPct;
	float m_flInPacketsWeirdSequencePct;
	int m_usecMaxJitterPreviousInterval;

	// Lifetime counters.  The "accumulator" values do not include the current interval -- use the accessors to get those
	int64 m_nPktsRecvSequenced;
	int64 m_nPktsRecvDroppedAccumulator;
	int64 m_nPktsRecvOutOfOrderAccumulator;
	int64 m_nPktsRecvDuplicateAccumulator;
	int64 m_nPktsRecvLurchAccumulator;
	inline int64 PktsRecvDropped() const { return m_nPktsRecvDroppedAccumulator + m_seqPktCounters.m_nDropped; }
	inline int64 PktsRecvOutOfOrder() const { return m_nPktsRecvOutOfOrderAccumulator + m_seqPktCounters.m_nOutOfOrder; }
	inline int64 PktsRecvDuplicate() const { return m_nPktsRecvDuplicateAccumulator + m_seqPktCounters.m_nDuplicate; }
	inline int64 PktsRecvLurch() const { return m_nPktsRecvLurchAccumulator + m_seqPktCounters.m_nLurch; }

	/// Lifetime quality statistics
	PercentileGenerator<uint8> m_qualitySample;

	/// Histogram of quality intervals
	QualityHistogram m_qualityHistogram;

	// Histogram of incoming latency variance
	JitterHistogram m_jitterHistogram;

	//
	// Misc stats bookkeeping
	//

	/// Check if it's been long enough since the last time we sent a ping,
	/// and we'd like to try to sneak one in if possible.
	///
	/// Note that in general, tracer pings are the only kind of pings that the relay
	/// ever sends.  It assumes that the endpoints will take care of any keepalives,
	/// etc that need to happen, and the relay can merely observe this process and take
	/// note of the outcome.
	///
	/// Returns:
	/// 0 - Not needed right now
	/// 1 - Opportunistic, but don't send by itself
	/// 2 - Yes, send one if possible
	inline int ReadyToSendTracerPing( SteamNetworkingMicroseconds usecNow ) const
	{
		if ( m_bPassive )
			return 0;
		SteamNetworkingMicroseconds usecTimeSince = usecNow - std::max( m_ping.m_usecTimeLastSentPingRequest, m_ping.TimeRecvMostRecentPing() );
		if ( usecTimeSince > k_usecLinkStatsMaxPingRequestInterval )
			return 2;
		if ( usecTimeSince > k_usecLinkStatsMinPingRequestInterval )
			return 1;
		return 0;
	}

	/// Check if we appear to be timing out and need to send an "aggressive" ping, meaning send it right
	/// now, request that the reply not be delayed, and also request that the relay (if any) confirm its
	/// connectivity as well.
	inline bool BNeedToSendPingImmediate( SteamNetworkingMicroseconds usecNow ) const
	{
		return
			!m_bPassive
			&& m_nReplyTimeoutsSinceLastRecv > 0 // We're timing out
			&& m_usecLastSendPacketExpectingImmediateReply+k_usecAggressivePingInterval <= usecNow; // we haven't just recently sent an aggressive ping.
	}

	/// Check if we should send a keepalive ping.  In this case we haven't heard from the peer in a while,
	/// but we don't have any reason to think there are any problems.
	inline bool BNeedToSendKeepalive( SteamNetworkingMicroseconds usecNow ) const
	{
		return
			!m_bPassive
			&& m_usecInFlightReplyTimeout == 0 // not already tracking some other message for which we expect a reply (and which would confirm that the connection is alive)
			&& m_usecTimeLastRecv + k_usecKeepAliveInterval <= usecNow; // haven't heard from the peer recently
	}

	/// Fill out message with everything we'd like to send.  We don't assume that we will
	/// actually send it.  (We might be looking for a good opportunity, and the data we want
	/// to send doesn't fit.)
	void PopulateMessage( int nNeedFlags, CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow );
	void PopulateLifetimeMessage( CMsgSteamDatagramLinkLifetimeStats &msg );
	/// Called when we send any message for which we expect some sort of reply.  (But maybe not an ack.)
	void TrackSentMessageExpectingReply( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply );

	/// Called when we receive stats from remote host
	void ProcessMessage( const CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow );

	/// Received from remote host
	SteamDatagramLinkInstantaneousStats m_latestRemote;
	SteamNetworkingMicroseconds m_usecTimeRecvLatestRemote;
	SteamDatagramLinkLifetimeStats m_lifetimeRemote;
	SteamNetworkingMicroseconds m_usecTimeRecvLifetimeRemote;

	int64 m_pktNumInFlight;
	bool m_bInFlightInstantaneous;
	bool m_bInFlightLifetime;

	/// Time when the current interval started
	SteamNetworkingMicroseconds m_usecIntervalStart;

	//
	// Reply timeout
	//

	/// If we have a message in flight for which we expect a reply (possibly delayed)
	/// and we haven't heard ANYTHING back, then this is the time when we should
	/// declare a timeout (and increment m_nReplyTimeoutsSinceLastRecv)
	SteamNetworkingMicroseconds m_usecInFlightReplyTimeout;

	/// Time when we last sent some sort of packet for which we expect
	/// an immediate reply.  m_stats.m_ping and m_usecInFlightReplyTimeout both
	/// remember when we send requests that expect replies, but both include
	/// ones that we allow the reply to be delayed.  This timestamp only includes
	/// ones that we do not allow to be delayed.
	SteamNetworkingMicroseconds m_usecLastSendPacketExpectingImmediateReply;

	/// Number of consecutive times a reply from this guy has timed out, since
	/// the last time we got valid communication from him.  This is reset basically
	/// any time we get a packet from the peer.
	int m_nReplyTimeoutsSinceLastRecv;

	/// Time when the current timeout (if any) was first detected.  This is not
	/// the same thing as the time we last heard from them.  For a mostly idle
	/// connection, the keepalive interval is relatively sparse, and so we don't
	/// know if we didn't hear from them, was it because there was a problem,
	/// or just they had nothing to say.  This timestamp measures the time when
	/// we expected to heard something but didn't.
	SteamNetworkingMicroseconds m_usecWhenTimeoutStarted;

	//
	// Populate public interface structure
	//
	void GetLinkStats( SteamDatagramLinkStats &s, SteamNetworkingMicroseconds usecNow ) const;

	/// This is the only function we needed to make virtual.  To factor this one
	/// out is really awkward, and this isn't called very often anyway.
	virtual void GetLifetimeStats( SteamDatagramLinkLifetimeStats &s ) const;

	inline void PeerAckedInstantaneous( SteamNetworkingMicroseconds usecNow )
	{
		m_usecPeerAckedInstaneous = usecNow;
		m_nPktsRecvSeqWhenPeerAckInstantaneous = m_nPktsRecvSequenced;
		m_nPktsSentWhenPeerAckInstantaneous = m_sent.m_packets.Total();
	}
	inline void PeerAckedLifetime( SteamNetworkingMicroseconds usecNow )
	{
		m_usecPeerAckedLifetime = usecNow;
		m_nPktsRecvSeqWhenPeerAckLifetime = m_nPktsRecvSequenced;
		m_nPktsSentWhenPeerAckLifetime = m_sent.m_packets.Total();
	}

	void InFlightPktAck( SteamNetworkingMicroseconds usecNow )
	{
		if ( m_bInFlightInstantaneous )
			PeerAckedInstantaneous( usecNow );
		if ( m_bInFlightLifetime )
			PeerAckedLifetime( usecNow );
		m_pktNumInFlight = 0;
		m_bInFlightInstantaneous = m_bInFlightLifetime = false;
	}

	void InFlightPktTimeout()
	{
		m_pktNumInFlight = 0;
		m_bInFlightInstantaneous = m_bInFlightLifetime = false;
	}

	/// Get urgency level to send instantaneous/lifetime stats.
	int GetStatsSendNeed( SteamNetworkingMicroseconds usecNow );

	/// Describe this stats tracker, for debugging, asserts, etc
	virtual std::string Describe() const = 0;

protected:
	// Make sure it's used as abstract base.  Note that we require you to call Init()
	// with a timestamp value, so the constructor is empty by default.
	inline LinkStatsTrackerBase() {}

	/// Initialize the stats tracking object
	void InitInternal( SteamNetworkingMicroseconds usecNow );

	/// Check if it's time to update, and if so, do it.
	template <typename TLinkStatsTracker>
	inline static void ThinkInternal( TLinkStatsTracker *pThis, SteamNetworkingMicroseconds usecNow )
	{
		// Check for ending the current QoS interval
		if ( !pThis->m_bPassive && pThis->m_usecIntervalStart + k_usecSteamDatagramLinkStatsDefaultInterval < usecNow )
		{
			pThis->UpdateInterval( usecNow );
		}
	
		// Check for reply timeout.
		if ( pThis->m_usecInFlightReplyTimeout > 0 && pThis->m_usecInFlightReplyTimeout < usecNow )
		{
			pThis->InFlightReplyTimeout( usecNow );
		}
	}

	/// Called when m_usecInFlightReplyTimeout is reached.  We intentionally only allow
	/// one of this type of timeout to be in flight at a time, so that the max
	/// rate that we accumulate them is based on the ping time, instead of the packet
	/// rate.
	template <typename TLinkStatsTracker>
	inline static void InFlightReplyTimeoutInternal( TLinkStatsTracker *pThis, SteamNetworkingMicroseconds usecNow )
	{
		pThis->m_usecInFlightReplyTimeout = 0;
		if ( pThis->m_usecWhenTimeoutStarted == 0 )
		{
			Assert( pThis->m_nReplyTimeoutsSinceLastRecv == 0 );
			pThis->m_usecWhenTimeoutStarted = usecNow;
		}
		++pThis->m_nReplyTimeoutsSinceLastRecv;
	}

	void GetInstantaneousStats( SteamDatagramLinkInstantaneousStats &s ) const;

	/// Called after we send a packet for which we expect an ack.  Note that we must have consumed the outgoing sequence
	/// for that packet (using GetNextSendSequenceNumber), but must *NOT* have consumed any more!
	/// This call implies TrackSentPingRequest, since we will be able to match up the ack'd sequence
	/// number with the time sent to get a latency estimate.
	template <typename TLinkStatsTracker>
	inline static void TrackSentMessageExpectingSeqNumAckInternal( TLinkStatsTracker *pThis, SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
	{
		pThis->TrackSentPingRequest( usecNow, bAllowDelayedReply );
	}

	/// Are we in "passive" state?  When we are "active", we expect that our peer is awake
	/// and will reply to our messages, and that we should be actively sending our peer
	/// connection quality statistics and keepalives.  When we are passive, we still measure
	/// statistics and can receive messages from the peer, and send acknowledgments as necessary.
	/// but we will indicate that keepalives or stats need to be sent to the peer.
	bool m_bPassive;

	/// Called to switch the passive state.  (Should only be called on an actual state change.)
	void SetPassiveInternal( bool bFlag, SteamNetworkingMicroseconds usecNow );

	/// Check if we really need to flush out stats now.  Derived class should provide the reason strings.
	/// (See the code.)
	const char *InternalGetSendStatsReasonOrUpdateNextThinkTime( SteamNetworkingMicroseconds usecNow, const char *const arpszReasonStrings[4], SteamNetworkingMicroseconds &inOutNextThinkTime );

	/// Called when we send a packet for which we expect a reply and
	/// for which we expect to get latency info.
	/// This implies TrackSentMessageExpectingReply.
	template <typename TLinkStatsTracker>
	inline static void TrackSentPingRequestInternal( TLinkStatsTracker *pThis, SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
	{
		pThis->TrackSentMessageExpectingReply( usecNow, bAllowDelayedReply );
		pThis->m_ping.m_usecTimeLastSentPingRequest = usecNow;
	}

	/// Called when we receive a reply from which we are able to calculate latency information
	template <typename TLinkStatsTracker>
	inline static void ReceivedPingInternal( TLinkStatsTracker *pThis, int nPingMS, SteamNetworkingMicroseconds usecNow )
	{
		pThis->m_ping.ReceivedPing( nPingMS, usecNow );
	}

	inline bool BInternalNeedToSendPingImmediate( SteamNetworkingMicroseconds usecNow, SteamNetworkingMicroseconds &inOutNextThinkTime )
	{
		if ( m_nReplyTimeoutsSinceLastRecv == 0 )
			return false;
		SteamNetworkingMicroseconds usecUrgentPing = m_usecLastSendPacketExpectingImmediateReply+k_usecAggressivePingInterval;
		if ( usecUrgentPing <= usecNow )
			return true;
		if ( usecUrgentPing < inOutNextThinkTime )
			inOutNextThinkTime = usecUrgentPing;
		return false;
	}

	inline bool BInternalNeedToSendKeepAlive( SteamNetworkingMicroseconds usecNow, SteamNetworkingMicroseconds &inOutNextThinkTime )
	{
		if ( m_usecInFlightReplyTimeout == 0 )
		{
			SteamNetworkingMicroseconds usecKeepAlive = m_usecTimeLastRecv + k_usecKeepAliveInterval;
			if ( usecKeepAlive <= usecNow )
				return true;
			if ( usecKeepAlive < inOutNextThinkTime )
				inOutNextThinkTime = usecKeepAlive;
		}
		else
		{
			if ( m_usecInFlightReplyTimeout < inOutNextThinkTime )
				inOutNextThinkTime = m_usecInFlightReplyTimeout;
		}
		return false;
	}

	// Hooks that derived classes may override when we process a packet
	// and it meets certain characteristics
	inline void InternalProcessSequencedPacket_Count()
	{
		m_seqPktCounters.OnRecv();
		++m_nPktsRecvSequenced;
	}
	void InternalProcessSequencedPacket_OutOfOrder( int64 nPktNum );
	inline void InternalProcessSequencedPacket_Duplicate()
	{
		m_seqPktCounters.OnDuplicate();
	}
	inline void InternalProcessSequencedPacket_Lurch()
	{
		m_seqPktCounters.OnLurch();
	}
	inline void InternalProcessSequencedPacket_Dropped( int nDropped )
	{
		m_seqPktCounters.OnDropped( nDropped );
	}

private:

	// Number of lifetime sequenced packets received, and overall packets sent,
	// the last time the peer acked stats
	int64 m_nPktsRecvSeqWhenPeerAckInstantaneous;
	int64 m_nPktsSentWhenPeerAckInstantaneous;
	int64 m_nPktsRecvSeqWhenPeerAckLifetime;
	int64 m_nPktsSentWhenPeerAckLifetime;

	/// Local time when peer last acknowledged lifetime stats.
	SteamNetworkingMicroseconds m_usecPeerAckedLifetime;

	/// Local time when peer last acknowledged instantaneous stats.
	SteamNetworkingMicroseconds m_usecPeerAckedInstaneous;

	bool BCheckHaveDataToSendInstantaneous( SteamNetworkingMicroseconds usecNow );
	bool BCheckHaveDataToSendLifetime( SteamNetworkingMicroseconds usecNow );

	/// Called to force interval to roll forward now
	void UpdateInterval( SteamNetworkingMicroseconds usecNow );

	void StartNextInterval( SteamNetworkingMicroseconds usecNow );
};

struct LinkStatsTrackerEndToEnd : public LinkStatsTrackerBase
{

	// LinkStatsTrackerBase "overrides"
	virtual void GetLifetimeStats( SteamDatagramLinkLifetimeStats &s ) const OVERRIDE;

	/// Calculate retry timeout the sender will use
	SteamNetworkingMicroseconds CalcSenderRetryTimeout() const
	{
		if ( m_ping.m_nSmoothedPing < 0 )
			return k_nMillion;
		// 3 x RTT + max delay, plus some slop.
		// If the receiver hands on to it for the max duration and
		// our RTT is very low
		return m_ping.m_nSmoothedPing*3000 + ( k_usecMaxDataAckDelay + 10000 );
	}

	/// Time when the connection entered the connection state
	SteamNetworkingMicroseconds m_usecWhenStartedConnectedState;

	/// Time when the connection ended
	SteamNetworkingMicroseconds m_usecWhenEndedConnectedState;

	/// Time when the current interval started
	SteamNetworkingMicroseconds m_usecSpeedIntervalStart;

	/// TX Speed, should match CMsgSteamDatagramLinkLifetimeStats 
	int m_nTXSpeed; 
	int m_nTXSpeedMax; 
	PercentileGenerator<int> m_TXSpeedSample;
	int m_nTXSpeedHistogram16; // Speed at kb/s
	int m_nTXSpeedHistogram32; 
	int m_nTXSpeedHistogram64;
	int m_nTXSpeedHistogram128;
	int m_nTXSpeedHistogram256;
	int m_nTXSpeedHistogram512;
	int m_nTXSpeedHistogram1024;
	int m_nTXSpeedHistogramMax;

	/// RX Speed, should match CMsgSteamDatagramLinkLifetimeStats 
	int m_nRXSpeed;
	int m_nRXSpeedMax;
	PercentileGenerator<int> m_RXSpeedSample;
	int m_nRXSpeedHistogram16; // Speed at kb/s
	int m_nRXSpeedHistogram32; 
	int m_nRXSpeedHistogram64;
	int m_nRXSpeedHistogram128;
	int m_nRXSpeedHistogram256;
	int m_nRXSpeedHistogram512;
	int m_nRXSpeedHistogram1024;
	int m_nRXSpeedHistogramMax;

	/// Called when we get a speed sample
	void UpdateSpeeds( int nTXSpeed, int nRXSpeed );

	/// Do we need to send any stats?
	inline const char *GetSendReasonOrUpdateNextThinkTime( SteamNetworkingMicroseconds usecNow, EStatsReplyRequest &eReplyRequested, SteamNetworkingMicroseconds &inOutNextThinkTime )
	{
		if ( m_bPassive )
		{
			if ( m_usecInFlightReplyTimeout > 0 && m_usecInFlightReplyTimeout < inOutNextThinkTime )
				inOutNextThinkTime = m_usecInFlightReplyTimeout;
			eReplyRequested = k_EStatsReplyRequest_NothingToSend;
			return nullptr;
		}

		// Urgent ping?
		if ( BInternalNeedToSendPingImmediate( usecNow, inOutNextThinkTime ) )
		{
			eReplyRequested = k_EStatsReplyRequest_Immediate;
			return "E2EUrgentPing";
		}

		// Keepalive?
		if ( BInternalNeedToSendKeepAlive( usecNow, inOutNextThinkTime ) )
		{
			eReplyRequested = k_EStatsReplyRequest_DelayedOK;
			return "E2EKeepAlive";
		}

		// Connection stats?
		static const char *arpszReasons[4] =
		{
			nullptr,
			"E2EInstantaneousStats",
			"E2ELifetimeStats",
			"E2EAllStats"
		};
		const char *pszReason = LinkStatsTrackerBase::InternalGetSendStatsReasonOrUpdateNextThinkTime( usecNow, arpszReasons, inOutNextThinkTime );
		if ( pszReason )
		{
			eReplyRequested = k_EStatsReplyRequest_DelayedOK;
			return pszReason;
		}

		eReplyRequested = k_EStatsReplyRequest_NothingToSend;
		return nullptr;
	}

	/// Describe this stats tracker, for debugging, asserts, etc
	virtual std::string Describe() const override { return "EndToEnd"; }

protected:
	void InitInternal( SteamNetworkingMicroseconds usecNow );

	template <typename TLinkStatsTracker>
	inline static void ThinkInternal( TLinkStatsTracker *pThis, SteamNetworkingMicroseconds usecNow )
	{
		LinkStatsTrackerBase::ThinkInternal( pThis, usecNow );

		if ( pThis->m_usecSpeedIntervalStart + k_usecSteamDatagramSpeedStatsDefaultInterval < usecNow )
		{
			pThis->UpdateSpeedInterval( usecNow );
		}
	}

private:

	void UpdateSpeedInterval( SteamNetworkingMicroseconds usecNow );
	void StartNextSpeedInterval( SteamNetworkingMicroseconds usecNow );
};

/// The conceptual "abstract base class" for all link stats trackers.  See the comments
/// on LinkSTatsTrackerBase for why this wackiness
template <typename TLinkStatsTracker>
struct LinkStatsTracker final : public TLinkStatsTracker
{

	// "Virtual functions" that we are "overriding" at compile time
	// by the template argument
	inline void Init( SteamNetworkingMicroseconds usecNow, bool bStartDisconnected = false )
	{
		TLinkStatsTracker::InitInternal( usecNow );
		TLinkStatsTracker::SetPassiveInternal( bStartDisconnected, usecNow );
	}
	inline void Think( SteamNetworkingMicroseconds usecNow ) { TLinkStatsTracker::ThinkInternal( this, usecNow ); }
	inline void SetPassive( bool bFlag, SteamNetworkingMicroseconds usecNow ) { if ( TLinkStatsTracker::m_bPassive != bFlag ) TLinkStatsTracker::SetPassiveInternal( bFlag, usecNow ); }
	inline bool IsPassive() const { return TLinkStatsTracker::m_bPassive; }
	inline void TrackSentMessageExpectingSeqNumAck( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply ) { TLinkStatsTracker::TrackSentMessageExpectingSeqNumAckInternal( this, usecNow, bAllowDelayedReply ); }
	inline void TrackSentPingRequest( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply ) { TLinkStatsTracker::TrackSentPingRequestInternal( this, usecNow, bAllowDelayedReply ); }
	inline void ReceivedPing( int nPingMS, SteamNetworkingMicroseconds usecNow ) { TLinkStatsTracker::ReceivedPingInternal( this, nPingMS, usecNow ); }
	inline void InFlightReplyTimeout( SteamNetworkingMicroseconds usecNow ) { TLinkStatsTracker::InFlightReplyTimeoutInternal( this, usecNow ); }

	/// Called after we actually send connection data.  Note that we must have consumed the outgoing sequence
	/// for that packet (using GetNextSendSequenceNumber), but must *NOT* have consumed any more!
	void TrackSentStats( const CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
	{

		// Check if we expect our peer to know how to acknowledge this
		if ( !TLinkStatsTracker::m_bPassive )
		{
			TLinkStatsTracker::m_pktNumInFlight = TLinkStatsTracker::m_nNextSendSequenceNumber-1;
			TLinkStatsTracker::m_bInFlightInstantaneous = msg.has_instantaneous();
			TLinkStatsTracker::m_bInFlightLifetime = msg.has_lifetime();

			// They should ack.  Make a note of the sequence number that we used,
			// so that we can measure latency when they reply, setup timeout bookkeeping, etc
			TrackSentMessageExpectingSeqNumAck( usecNow, bAllowDelayedReply );
		}
		else
		{
			// Peer can't ack.  Just mark them as acking immediately
			Assert( TLinkStatsTracker::m_pktNumInFlight == 0 );
			TLinkStatsTracker::m_pktNumInFlight = 0;
			TLinkStatsTracker::m_bInFlightInstantaneous = false;
			TLinkStatsTracker::m_bInFlightLifetime = false;
			if ( msg.has_instantaneous() )
				TLinkStatsTracker::PeerAckedInstantaneous( usecNow );
			if ( msg.has_lifetime() )
				TLinkStatsTracker::PeerAckedLifetime( usecNow );
		}
	}

	inline bool RecvPackedAcks( const google::protobuf::RepeatedField<google::protobuf::uint32> &msgField, SteamNetworkingMicroseconds usecNow )
	{
		bool bResult = true;
		for ( uint32 nPackedAck: msgField )
		{
			if ( !TLinkStatsTracker::RecvPackedAckInternal( this, nPackedAck, usecNow ) )
				bResult = false;
		}
		return bResult;
	}

	// Shortcut when we know that we aren't going to send now, but we want to know when to wakeup and do so
	inline SteamNetworkingMicroseconds GetNextThinkTime( SteamNetworkingMicroseconds usecNow )
	{
		SteamNetworkingMicroseconds usecNextThink = k_nThinkTime_Never;
		EStatsReplyRequest eReplyRequested;
		if ( TLinkStatsTracker::GetSendReasonOrUpdateNextThinkTime( usecNow, eReplyRequested, usecNextThink ) )
			return k_nThinkTime_ASAP;
		return usecNextThink;
	}

	/// Called when we receive a packet with a sequence number.
	/// This expands the wire packet number to its full value,
	/// and checks if it is a duplicate or out of range.
	/// Stats are also updated
	int64 ExpandWirePacketNumberAndCheck( uint16 nWireSeqNum )
	{
		int16 nGap = (int16)( nWireSeqNum - (uint16)TLinkStatsTracker::m_nMaxRecvPktNum );
		int64 nPktNum = TLinkStatsTracker::m_nMaxRecvPktNum + nGap;

		// We've received a packet with a sequence number.
		// Update stats
		TLinkStatsTracker::m_arDebugHistoryRecvSeqNum[ TLinkStatsTracker::m_nPktsRecvSequenced & 255 ] = nPktNum;
		TLinkStatsTracker::InternalProcessSequencedPacket_Count();

		// Packet number is increasing?
		// (Maybe by a lot -- we don't handle that here.)
		if ( likely( nPktNum > TLinkStatsTracker::m_nMaxRecvPktNum ) )
			return nPktNum;

		// Which block of 64-bit packets is it in?
		int64 B = TLinkStatsTracker::m_nMaxRecvPktNum & ~int64{63};
		int64 idxRecvBitmask = ( ( nPktNum - B ) >> 6 ) + 1;
		Assert( idxRecvBitmask < 2 );
		if ( idxRecvBitmask < 0 )
		{
			// Too old (at least 64 packets old, maybe up to 128).
			TLinkStatsTracker::InternalProcessSequencedPacket_Lurch(); // Should we track "very old" under a different stat than "lurch"?
			return 0;
		}
		uint64 bit = uint64{1} << ( nPktNum & 63 );
		if ( TLinkStatsTracker::m_recvPktNumberMask[ idxRecvBitmask ] & bit )
		{
			// Duplicate
			TLinkStatsTracker::InternalProcessSequencedPacket_Duplicate();
			return 0;
		}

		// We have an out of order packet.  We'll update that
		// stat in TrackProcessSequencedPacket
		Assert( nPktNum > 0 && nPktNum < TLinkStatsTracker::m_nMaxRecvPktNum );
		return nPktNum;
	}

	/// Same as ExpandWirePacketNumberAndCheck, but if this is the first sequenced
	/// packet we have ever received, initialize the packet number
	int64 ExpandWirePacketNumberAndCheckMaybeInitialize( uint16 nWireSeqNum )
	{
		if ( unlikely( TLinkStatsTracker::m_nMaxRecvPktNum == 0 ) )
			TLinkStatsTracker::ResetMaxRecvPktNumForIncomingWirePktNum( nWireSeqNum );
		return ExpandWirePacketNumberAndCheck( nWireSeqNum );
	}

	/// Called when we have processed a packet with a sequence number, to update estimated
	/// number of dropped packets, etc.  This MUST only be called after we have
	/// called ExpandWirePacketNumberAndCheck, to ensure that the packet number is not a
	/// duplicate or out of range.
	inline void TrackProcessSequencedPacket( int64 nPktNum, SteamNetworkingMicroseconds usecNow, int usecSenderTimeSincePrev )
	{
		Assert( nPktNum > 0 );

		// Update bitfield of received packets
		int64 B = TLinkStatsTracker::m_nMaxRecvPktNum & ~int64{63};
		int64 idxRecvBitmask = ( ( nPktNum - B ) >> 6 ) + 1;
		Assert( idxRecvBitmask >= 0 ); // We should have discarded very old packets already
		if ( idxRecvBitmask >= 2 ) // Most common case is 0 or 1
		{
			if ( idxRecvBitmask == 2 )
			{
				// Crossed to the next 64-packet block.  Shift bitmasks forward by one.
				TLinkStatsTracker::m_recvPktNumberMask[0] = TLinkStatsTracker::m_recvPktNumberMask[1];
			}
			else
			{
				// Large packet number jump, we skipped a whole block
				TLinkStatsTracker::m_recvPktNumberMask[0] = 0;
			}
			TLinkStatsTracker::m_recvPktNumberMask[1] = 0;
			idxRecvBitmask = 1;
		}
		uint64 bit = uint64{1} << ( nPktNum & 63 );
		Assert( !( TLinkStatsTracker::m_recvPktNumberMask[ idxRecvBitmask ] & bit ) ); // Should not have already been marked!  We should have already discarded duplicates
		TLinkStatsTracker::m_recvPktNumberMask[ idxRecvBitmask ] |= bit;

		// Check for dropped packet.  Since we hope that by far the most common
		// case will be packets delivered in order, we optimize this logic
		// for that case.
		int64 nGap = nPktNum - TLinkStatsTracker::m_nMaxRecvPktNum;
		if ( likely( nGap == 1 ) )
		{
			++TLinkStatsTracker::m_nDebugPktsRecvInOrder;

			// We've received two packets, in order.  Did the sender supply the time between packets on his side?
			if ( usecSenderTimeSincePrev > 0 )
			{
				int usecJitter = ( usecNow - TLinkStatsTracker::m_usecTimeLastRecvSeq ) - usecSenderTimeSincePrev;
				usecJitter = abs( usecJitter );
				if ( usecJitter < k_usecTimeSinceLastPacketMaxReasonable )
				{

					// Update max jitter for current interval
					TLinkStatsTracker::m_seqPktCounters.m_usecMaxJitter = std::max( TLinkStatsTracker::m_seqPktCounters.m_usecMaxJitter, usecJitter );
					TLinkStatsTracker::m_jitterHistogram.AddSample( usecJitter );
				}
				else
				{
					// Something is really, really off.  Discard measurement
				}
			}

		}
		else if ( unlikely( nGap <= 0 ) )
		{
			// Packet number moving backward
			// We should have already rejected duplicates
			Assert( nGap != 0 );

			// Packet number moving in reverse.
			// It should be a *small* negative step, e.g. packets delivered out of order.
			// If the packet is really old, we should have already discarded it earlier.
			Assert( nGap >= -8 * (int64)sizeof(TLinkStatsTracker::m_recvPktNumberMask) );

			// out of order
			TLinkStatsTracker::InternalProcessSequencedPacket_OutOfOrder( nPktNum );
			return;
		}
		else 
		{
			// Packet number moving forward, i.e. a dropped packet
			// Large gap?
			if ( unlikely( nGap >= 100 ) )
			{
				// Very weird.
				TLinkStatsTracker::InternalProcessSequencedPacket_Lurch();

				// Reset the sequence number for packets going forward.
				TLinkStatsTracker::InitMaxRecvPktNum( nPktNum );
				return;
			}

			// Probably the most common case (after a perfect packet stream), we just dropped a packet or two
			TLinkStatsTracker::InternalProcessSequencedPacket_Dropped( nGap-1 );
		}

		// Save highest known sequence number for next time.
		TLinkStatsTracker::m_nMaxRecvPktNum = nPktNum;
		TLinkStatsTracker::m_usecTimeLastRecvSeq = usecNow;
	}
};


//
// Pack/unpack C struct <-> protobuf message
//
extern void LinkStatsInstantaneousStructToMsg( const SteamDatagramLinkInstantaneousStats &s, CMsgSteamDatagramLinkInstantaneousStats &msg );
extern void LinkStatsInstantaneousMsgToStruct( const CMsgSteamDatagramLinkInstantaneousStats &msg, SteamDatagramLinkInstantaneousStats &s );
extern void LinkStatsLifetimeStructToMsg( const SteamDatagramLinkLifetimeStats &s, CMsgSteamDatagramLinkLifetimeStats &msg );
extern void LinkStatsLifetimeMsgToStruct( const CMsgSteamDatagramLinkLifetimeStats &msg, SteamDatagramLinkLifetimeStats &s );

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKING_STATSUTILS_H
