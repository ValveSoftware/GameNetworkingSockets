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
#include <tier1/utllinkedlist.h>

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
const SteamNetworkingMicroseconds k_usecLinkStatsPingRequestInterval = 5 * k_nMillion;

/// Client should send instantaneous connection quality stats
/// at approximately this interval
const SteamNetworkingMicroseconds k_usecLinkStatsInstantaneousReportMinInterval = 17 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsInstantaneousReportInterval = 20 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsInstantaneousReportMaxInterval = 30 * k_nMillion;

/// Client will report lifetime connection stats at approximately this interval
const SteamNetworkingMicroseconds k_usecLinkStatsLifetimeReportMinInterval = 102 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsLifetimeReportInterval = 120 * k_nMillion;
const SteamNetworkingMicroseconds k_usecLinkStatsLifetimeReportMaxInterval = 140 * k_nMillion;

/// If we are timing out, ping the peer on this interval
const SteamNetworkingMicroseconds k_usecAggressivePingInterval = 200*1000;

/// If we haven't heard from the peer in a while, send a keepalive
const SteamNetworkingMicroseconds k_usecKeepAliveInterval = 10*k_nMillion;

/// Track the rate that something is happening
struct Rate_t
{
	Rate_t()  { memset( this, 0, sizeof(*this) ); }

	int64	m_nTotal;
	int64	m_nCurrentInterval;
	//int64	m_nCurrentLongInterval;
	float	m_flRate;
	float	m_flPeakRate;

	inline void Process( int64 nIncrement )
	{
		m_nTotal += nIncrement;
		m_nCurrentInterval += nIncrement;
		//m_nCurrentLongInterval += nIncrement;
	}

	inline void UpdateInterval( float flIntervalDuration )
	{
		m_flRate = float(m_nCurrentInterval) / flIntervalDuration;
		m_flPeakRate = Max( m_flPeakRate, m_flRate );
		m_nCurrentInterval = 0;
	}

	inline void operator+=( const Rate_t &x )
	{
		m_nTotal += x.m_nTotal;
		m_nCurrentInterval += x.m_nCurrentInterval;
		//m_nCurrentLongInterval += x.m_nCurrentLongInterval;
		m_flRate += x.m_flRate;
		// !NOTE: Don't aggregate peak.  It's ambiguous whether we should take the sum or max.
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
	void Reset();

	/// Called when we receive a ping measurement
	void ReceivedPing( int nPingMS, SteamNetworkingMicroseconds usecNow );

	struct Ping
	{
		int m_nPingMS;
		SteamNetworkingMicroseconds m_usecTimeRecv;
	};

	/// Recent ping measurements.  The most recent one is at entry 0.
	Ping m_arPing[ 3 ];

	/// Number of valid entries in m_arPing.
	int m_nValidPings;

	/// Do we have a full sample?
	bool HasFullSample() const { return m_nValidPings >= V_ARRAYSIZE(m_arPing); }

	/// Time when the most recent ping was received
	SteamNetworkingMicroseconds TimeRecvMostRecentPing() const { return m_arPing[0].m_usecTimeRecv; }

	/// Time when the oldest ping was received
	SteamNetworkingMicroseconds TimeRecvOldestPing() const { return ( m_nValidPings > 0 ) ? m_arPing[m_nValidPings-1].m_usecTimeRecv : 0; }

	/// Ping estimate, being pessimistic
	int PessimisticPingEstimate() const;

	/// Ping estimate, being optimistic
	int OptimisticPingEstimate() const;

	/// Smoothed ping value
	int m_nSmoothedPing;

	/// Time when we last sent a message, for which we expect a reply (possibly delayed)
	/// that we could use to measure latency.  (Possibly because the reply contains
	/// a simple timestamp, or possibly because it will contain a sequence number, and
	/// we will be able to look up that sequence number and remember when we sent it.)
	SteamNetworkingMicroseconds m_usecTimeLastSentPingRequest;

	/// Total number of pings we have received
	inline int TotalPingsReceived() const { return m_sample.NumSamplesTotal(); }

	/// Should match CMsgSteamDatagramLinkLifetimeStats
	int m_nHistogram25;
	int m_nHistogram50;
	int m_nHistogram75;
	int m_nHistogram100;
	int m_nHistogram125;
	int m_nHistogram150;
	int m_nHistogram200;
	int m_nHistogram300;
	int m_nHistogramMax;

	/// Track sample of pings received so we can generate a histogram.
	/// Also tracks how many pings we have received total
	PercentileGenerator<uint16> m_sample;
};

/// An outgoing sequence number that we sent, and are expecting an ack of some kind
/// -or- An ack that we need to send, but are pending it briefly.
#pragma pack( push, 1 )
struct PacketAck
{
	// These guys shouldn't live every long, and so assume that
	// the difference in both the sequence number and the the timestamp
	// can be captured using only the lower bits of each
	uint16 m_nWireSeqNum : 16;
	uint64 m_usecTimestamp : 48; /// When did we sent the message for which we are expecting an ack, or when did we receive the message for which we are pending an ack?

	static const uint64 k_nTimestampMask = 0xffffffffffffull;

	inline void SetTimestamp( SteamNetworkingMicroseconds usecNow )
	{
		Assert( ( usecNow & ~k_nTimestampMask ) == 0 ); // Assume local timestamps start near zero with a process starts, and we won't run for NINE YEARS
		m_usecTimestamp = usecNow;
	}

	SteamNetworkingMicroseconds Timestamp( SteamNetworkingMicroseconds usecRef ) const
	{
		Assert( ( usecRef & ~k_nTimestampMask ) == 0 ); // Assume local timestamps start near zero with a process starts, and we won't run for NINE YEARS
		return m_usecTimestamp;
	}

	SteamNetworkingMicroseconds MicrosecondsAge( SteamNetworkingMicroseconds usecRef ) const
	{
		SteamNetworkingMicroseconds usecDiff = usecRef - Timestamp( usecRef );
		Assert( usecDiff >= -10*k_nMillion && usecDiff <= 10*k_nMillion ); // All of our timestamps should be operating within a reasonably narrow sliding window.
		return usecDiff;
	}
};
#pragma pack( pop )
//COMPILE_TIME_ASSERT( sizeof(ExpectedAck) == 8 ); // FIXME PERF: this is firing, which is not good.

/// Track outgoing sequence numbers for which we expect an ack of some sort, and the time we sent it
struct ExpectedAcksTracker
{
	// FIXME Should add a way to allow for fixed size, because we don't want the relay doing any
	// dynamic memory allocation or attempting to track a bunch of acks anyway.  Also a doubly-linked
	// list is probably overkill here, since 95% of accesses will be at the head and tail, so a
	// dequeue would be better.
	CUtlLinkedList<PacketAck> m_listAcks;

	/// Reset state
	void Clear() { m_listAcks.RemoveAll(); }

	/// Record an outgoing packet for which we expect an ack
	void AddExpectedAck( uint16 nWireSeqNum, SteamNetworkingMicroseconds usecNow )
	{
		// Compare this against the last one we queued
		int t = m_listAcks.Tail();
		if ( t != m_listAcks.InvalidIndex() )
		{
			const PacketAck &last = m_listAcks[t];

			// Harmlessly allow duplicates
			if ( last.m_nWireSeqNum == nWireSeqNum )
			{
				Assert( last.m_usecTimestamp == uint64( usecNow&PacketAck::k_nTimestampMask) );
				return;
			}

			// Otherwise, make sure the sequence number doesn't lurch too much.
			// We really need to be confirming flow of packets with our peer (much)
			// more frequently than this!
			int16 nDiff = (int16)( nWireSeqNum - last.m_nWireSeqNum );
			Assert( nDiff > 0 && nDiff < 0x4000 );
			Assert( last.MicrosecondsAge( usecNow ) < k_nMillion*10 ); // We really should have already timed it out if it's older than this!
		}

		PacketAck a;
		a.m_nWireSeqNum = nWireSeqNum;
		a.m_usecTimestamp = usecNow;
		m_listAcks.AddToTail( a );

		// these shouldn't be allowed to stack up --- regardless of the behaviour of the remote host!
		AssertMsg( m_listAcks.Count() < 32, "Too many expected acks!  Either we're sending packts requiring acks too fast, or else we're not expiring them properly" );
	}

	/// Check the oldest entry (if any).  If it's timed out, return true and return to caller.
	/// Otherwise, return false.
	bool BRemoveOldestAckIfTimedOut( PacketAck &result, SteamNetworkingMicroseconds usecExpiry )
	{
		// Empty?
		int h = m_listAcks.Head();
		if ( h == m_listAcks.InvalidIndex() )
			return false;

		// Timed out?
		const PacketAck &a = m_listAcks[ h ];
		if ( a.MicrosecondsAge( usecExpiry ) < 0 )
			return false;

		// It's timed out
		result = a;
		m_listAcks.Remove( h );
		return true;
	}

	/// Called when receive an ack.  Removes the record from the list, and returns
	/// the timestamp when it was sent.  Returns 0 if we don't have a record
	/// of it being sent
	///
	/// Returns <0 if something looks fishy or out of whack, and the client might
	/// be sending us bad stuff we should squawk about
	SteamNetworkingMicroseconds GetTimeSentAndRemoveAck( uint16 nWireSeqNum, SteamNetworkingMicroseconds usecNow )
	{
		// Walk list from oldest to newest.  We assume that sequence numbers are in this list in order!
		int idx = m_listAcks.Head();
		while ( idx != m_listAcks.InvalidIndex() )
		{
			PacketAck a = m_listAcks[idx];
			int16 nSeqNumDiff = (int16)( a.m_nWireSeqNum - nWireSeqNum );
			if ( nSeqNumDiff == 0 )
			{
				m_listAcks.Remove( idx );
				return a.Timestamp( usecNow );
			}

			// Make sure the size of the sliding window of packets doesn't get too large
			if ( nSeqNumDiff < -0x4000 )
			{
				Assert( a.MicrosecondsAge( usecNow ) < k_nMillion*10 ); // We really should have already timed it out if it's older than this!
				return -1;
			}

			// All remaining acks have a subsequent sequence number?
			if ( nSeqNumDiff > 0 )
			{
				if ( nSeqNumDiff > 0x4000 )
				{
					Assert( a.MicrosecondsAge( usecNow ) < k_nMillion*10 ); // We really should have already timed it out if it's older than this!
					return -1;
				}
				break;
			}

			// Hm, ack dropped or out of order.  Scan forward and look for the
			// one we received.  This hopefully doesn't iterate too many times.
			idx = m_listAcks.Next( idx );
		}

		// Not found
		return 0;
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

/// Class used to handle link quality calculations.
struct LinkStatsTracker
{

	/// Estimate a conservative (i.e. err on the large side) timeout for the connection
	SteamNetworkingMicroseconds CalcConservativeTimeout() const
	{
		return ( m_ping.m_nSmoothedPing >= 0 ) ? ( m_ping.m_nSmoothedPing*2 + 500000 ) : k_nMillion;
	}

	/// What version is the peer running?  It's 0 if we don't know yet.
	uint32 m_nPeerProtocolVersion;

	inline void SetDisconnected( bool bFlag, SteamNetworkingMicroseconds usecNow ) { if ( m_bDisconnected != bFlag ) InternalSetDisconnected( bFlag, usecNow ); }
	inline bool IsDisconnected() const { return m_bDisconnected; }

	/// Ping
	PingTracker m_ping;

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
		++m_nPktsSentSinceSentInstantaneous;
		++m_nPktsSentSinceSentLifetime;
	}

	/// Consume the next sequence number, and record the time at which
	/// we sent a sequenced packet.  (Don't call this unless you are sending
	/// a sequenced packet.)
	inline uint16 GetNextSendSequenceNumber( SteamNetworkingMicroseconds usecNow )
	{
		m_usecTimeLastSentSeq = usecNow;
		return uint16( m_nNextSendSequenceNumber++ );
	}

	// Track acks outstanding that we expect to receive
	ExpectedAcksTracker m_expectedAcks;

	// List of acks that we are pending, seeking an opportunity to piggy back it with some other packet
	static const int k_nMaxPendingAcks = 5;
	bool m_bPendingAckImmediate;
	int m_nPendingOutgoingAcks;
	PacketAck m_arPendingOutgoingAck[k_nMaxPendingAcks];

	//
	// Incoming
	//
	int64 m_nLastRecvSequenceNumber;
	PacketRate_t m_recv;

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

	/// Called when we receive a packet with a sequence number, to update estimated
	/// number of dropped packets, etc.  Returns the full 64-bit sequence number
	/// for the flow.
	void TrackRecvSequencedPacket( uint16 unWireSequenceNumber, SteamNetworkingMicroseconds usecNow, int usecSenderTimeSincePrev );
	void TrackRecvSequencedPacketGap( int16 nGap, SteamNetworkingMicroseconds usecNow, int usecSenderTimeSincePrev );

	//
	// Instantaneous stats
	//

	// Accumulators for current interval
	int m_nPktsRecvSequencedCurrentInterval; // packets successfully received containing a sequence number
	int m_nPktsRecvDroppedCurrentInterval; // packets assumed to be dropped in the current interval
	int m_nPktsRecvWeirdSequenceCurrentInterval; // any sequence number deviation other than a simple dropped packet.  (Most recent interval.)
	int m_usecMaxJitterCurrentInterval;

	// Instantaneous rates, calculated from most recent completed interval
	float m_flInPacketsDroppedPct;
	float m_flInPacketsWeirdSequencePct;
	int m_usecMaxJitterPreviousInterval;

	//
	// Lifetime stats
	//

	// Lifetime counters
	int64 m_nPktsRecvSequenced;
	int64 m_nPktsRecvDropped;
	int64 m_nPktsRecvOutOfOrder;
	int64 m_nPktsRecvDuplicate;
	int64 m_nPktsRecvSequenceNumberLurch; // sequence number had a really large discontinuity

	/// Lifetime quality statistics
	PercentileGenerator<uint8> m_qualitySample;

	/// Histogram of quality intervals
	int m_nQualityHistogram100;
	int m_nQualityHistogram99;
	int m_nQualityHistogram97;
	int m_nQualityHistogram95;
	int m_nQualityHistogram90;
	int m_nQualityHistogram75;
	int m_nQualityHistogram50;
	int m_nQualityHistogram1;
	int m_nQualityHistogramDead;

	// Histogram of incoming latency variance
	int m_nJitterHistogramNegligible; // <1ms
	int m_nJitterHistogram1; // 1--2ms
	int m_nJitterHistogram2; // 2--5ms
	int m_nJitterHistogram5; // 5--10ms
	int m_nJitterHistogram10; // 10--20ms
	int m_nJitterHistogram20; // 20ms or more

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
	inline bool BReadyToSendTracerPing( SteamNetworkingMicroseconds usecNow ) const
	{
		return m_ping.m_usecTimeLastSentPingRequest + k_usecLinkStatsPingRequestInterval < usecNow;
	}

	/// Check if we appear to be timing out and need to send an "aggressive" ping, meaning send it right
	/// now, request that the reply not be delayed, and also request that the relay (if any) confirm its
	/// connectivity as well.
	inline bool BNeedToSendPingImmediate( SteamNetworkingMicroseconds usecNow ) const
	{
		return
			m_nReplyTimeoutsSinceLastRecv > 0 // We're timing out
			&& m_usecLastSendPacketExpectingImmediateReply+k_usecAggressivePingInterval < usecNow; // we haven't just recently sent an agreeeisve ping.
	}

	/// Check if we should send a keepalive ping.  In this case we haven't heard from the peer in a while,
	/// but we don't have any reason to think there are any problems.
	inline bool BNeedToSendKeepalive( SteamNetworkingMicroseconds usecNow ) const
	{
		return
			m_usecInFlightReplyTimeout == 0 // not already tracking some other message for which we expect a reply (and which would confirm that the connection is alive)
			&& m_usecTimeLastRecv + k_usecKeepAliveInterval < usecNow; // haven't heard from the peer recently
	}

	/// Check if we have data worth sending, if we have a good
	/// opportunity (inline in a data packet) to do it.
	inline bool BReadyToSendStats( SteamNetworkingMicroseconds usecNow )
	{
		bool bResult = false;
		if ( m_seqNumInFlight == 0 && !m_bDisconnected )
		{
			if ( m_usecPeerAckedInstaneous + k_usecLinkStatsInstantaneousReportInterval < usecNow && BCheckHaveDataToSendInstantaneous( usecNow ) )
				bResult = true ;
			if ( m_usecPeerAckedLifetime + k_usecLinkStatsLifetimeReportInterval < usecNow && BCheckHaveDataToSendLifetime( usecNow ) )
				bResult = true;
		}

		return bResult;
	}

	/// Check if we really need to send some stats out, even if it means using
	/// a less efficient standalone message type.
	bool BNeedToSendStatsOrAcks( SteamNetworkingMicroseconds usecNow );

	/// Fill out message with everything we'd like to send.  We don't assume that we will
	/// actually send it.  (We might be looking for a good opportunity, and the data we want
	/// to send doesn't fit.)
	void PopulateMessage( CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow );

	/// Called after we actually send connection data.  Note that we must have consumed the outgoing sequence
	/// for that packet (using GetNextSendSequenceNumber), but must *NOT* have consumed any more!
	void TrackSentStats( const CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply );

	/// Called after we send a packet for which we expect an ack.  Note that we must have consumed the outgoing sequence
	/// for that packet (using GetNextSendSequenceNumber), but must *NOT* have consumed any more!
	/// This call implies TrackSentPingRequest, since we will be able to match up the ack'd sequence
	/// number with the time sent to get a latency estimate.
	void TrackSentMessageExpectingSeqNumAck( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply );

	/// Called when we send a packet for which we expect a reply and
	/// for which we expect to get latency info.
	/// This implies TrackSentMessageExpectingReply.
	void TrackSentPingRequest( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
	{
		TrackSentMessageExpectingReply( usecNow, bAllowDelayedReply );
		m_ping.m_usecTimeLastSentPingRequest = usecNow;
	}

	/// Called when we send any message for which we expect some sort of reply.  (But maybe not an ack.)
	void TrackSentMessageExpectingReply( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply );

	/// Called when we receive stats from remote host
	void ProcessMessage( const CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow );

	/// Called when we receive an ack.  Returns false if something seems fishy about the times.
	bool RecvAck( uint16 nWireSeqNum, uint16 nPackedDelay, SteamNetworkingMicroseconds usecNow );
	bool RecvPackedAcks( const google::protobuf::RepeatedField<google::protobuf::uint32> &msgField, SteamNetworkingMicroseconds usecNow )
	{
		bool bResult = true;
		for ( uint32 nPackedAck: msgField )
		{
			if ( !RecvAck( nPackedAck>>16U, ( nPackedAck & 0xffff ), usecNow ) )
				bResult = false;
		}
		return bResult;
	}

	/// Called when we send acks packed in repeated protobuf field
	void TrackSentAck( uint16 nWireSeqNum )
	{
		for ( int i = 0 ; i < m_nPendingOutgoingAcks ; ++i )
		{
			if ( nWireSeqNum == m_arPendingOutgoingAck[i].m_nWireSeqNum )
			{
				--m_nPendingOutgoingAcks;
				if ( m_nPendingOutgoingAcks == 0 )
					m_bPendingAckImmediate = false;
				else
					memmove( &m_arPendingOutgoingAck[i+1], &m_arPendingOutgoingAck[i], sizeof(m_arPendingOutgoingAck[0]) * (m_nPendingOutgoingAcks-i) );
				return;
			}
		}
		AssertMsg( false, "We sent an ack that wasn't pending!" );
	}

	inline void TrackSentPackedAcks( const google::protobuf::RepeatedField<google::protobuf::uint32> &msgField )
	{
		if ( msgField.size() == 0 )
			return;
		if ( msgField.size() == m_nPendingOutgoingAcks )
		{
			m_nPendingOutgoingAcks = 0;
			m_bPendingAckImmediate = false;
		}
		else
		{
			for ( uint32 nPackedAck: msgField )
				TrackSentAck( nPackedAck>>16U );
		}
	}

	inline void QueueOutgoingAck( uint16 nWireSeqNum, bool bImmediate, SteamNetworkingMicroseconds usecNow )
	{
		// Ignore redundant request to ack the same packet twice.
		if ( m_nPendingOutgoingAcks == 0 || m_arPendingOutgoingAck[m_nPendingOutgoingAcks-1].m_nWireSeqNum != nWireSeqNum )
		{
			if ( m_nPendingOutgoingAcks >= k_nMaxPendingAcks )
			{
				if ( !bImmediate )
					return;
				--m_nPendingOutgoingAcks;
			}
			m_arPendingOutgoingAck[m_nPendingOutgoingAcks].m_nWireSeqNum = nWireSeqNum;
			m_arPendingOutgoingAck[m_nPendingOutgoingAcks].SetTimestamp( usecNow );
			++m_nPendingOutgoingAcks;
		}
		if ( bImmediate )
			m_bPendingAckImmediate = true;
	}

	/// Received from remote host
	SteamDatagramLinkInstantaneousStats m_latestRemote;
	SteamNetworkingMicroseconds m_usecTimeRecvLatestRemote;
	SteamDatagramLinkLifetimeStats m_lifetimeRemote;
	SteamNetworkingMicroseconds m_usecTimeRecvLifetimeRemote;

	/// Local time when peer last acknowledged instantaneous stats.
	SteamNetworkingMicroseconds m_usecPeerAckedInstaneous;
	uint16 m_seqNumInFlight;
	bool m_bInFlightInstantaneous;
	bool m_bInFlightLifetime;

	/// Number of sequenced packets received since we last sent instantaneous stats
	int m_nPktsRecvSeqSinceSentInstantaneous;

	/// Number of packets we have sent, since we last sent instantaneous stats
	int m_nPktsSentSinceSentInstantaneous;

	/// Local time when we last sent lifetime stats.
	SteamNetworkingMicroseconds m_usecPeerAckedLifetime;

	/// Number of sequenced packets received since we last sent lifetime stats
	int m_nPktsRecvSeqSinceSentLifetime;
	int m_nPktsSentSinceSentLifetime;

	/// We sent lifetime stats on this seq number, has not been acknowledged.  <0 if none
	//int m_seqnumUnackedSentLifetime;

	/// We received lifetime stats at this sequence number, and should ack it soon.  <0 if none
	//int m_seqnumPendingAckRecvTimelife;

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
		m_nPktsRecvSeqSinceSentInstantaneous = 0;
		m_nPktsSentSinceSentInstantaneous = 0;
	}
	inline void PeerAckedLifetime( SteamNetworkingMicroseconds usecNow )
	{
		m_usecPeerAckedLifetime = usecNow;
		m_nPktsRecvSeqSinceSentLifetime = 0;
		m_nPktsSentSinceSentLifetime = 0;
	}

protected:
	// Make sure it's used as abstract base.  Note that we require you to call Init()
	// with a timestamp value, so the constructor is empty by default.
	inline LinkStatsTracker() {}

	/// Initialize the stats tracking object
	/// We don't do this as a virtual function, since it's easy to factor the code
	/// where outside code will just call the derived class Init() version directly,
	/// and also give it a really specific name so we don't forget that this isn't doing any
	/// derived class work and call it internally.
	void InitBaseLinkStatsTracker( SteamNetworkingMicroseconds usecNow, bool bStartDisconnected );

	/// Check if it's time to update, and if so, do it.
	/// This is another one we don't implement as a virtual function,
	/// and this is called frequently so giving the optimizer a bit more
	/// visibility can'thurt.
	void ThinkBaseLinkStatsTracker( SteamNetworkingMicroseconds usecNow );

	void GetInstantaneousStats( SteamDatagramLinkInstantaneousStats &s ) const;

private:

	bool BCheckHaveDataToSendInstantaneous( SteamNetworkingMicroseconds usecNow );
	bool BCheckHaveDataToSendLifetime( SteamNetworkingMicroseconds usecNow );

	/// Called to force interval to roll forward now
	void UpdateInterval( SteamNetworkingMicroseconds usecNow );

	void StartNextInterval( SteamNetworkingMicroseconds usecNow );


	/// When the connection is terminated, we set this flag.  At that point we will no
	/// longer expect the peer to ack, or request to flush stats, etc.  (Although we
	/// might indicate that we need to send an ack.)
	bool m_bDisconnected;

	void InternalSetDisconnected( bool bFlag, SteamNetworkingMicroseconds usecNow );
};

struct LinkStatsTrackerRelay : public LinkStatsTracker
{

	// LinkStatsTracker "overrides"
	void Init( SteamNetworkingMicroseconds usecNow, bool bStartDisconnected ) { InitBaseLinkStatsTracker( usecNow, bStartDisconnected ); }
	void Think( SteamNetworkingMicroseconds usecNow ) { ThinkBaseLinkStatsTracker( usecNow ); }

};

struct LinkStatsTrackerEndToEnd : public LinkStatsTracker
{

	// LinkStatsTracker "overrides"
	void Init( SteamNetworkingMicroseconds usecNow, bool bStartDisconnected = false );
	void Think( SteamNetworkingMicroseconds usecNow );
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

private:

	void UpdateSpeedInterval( SteamNetworkingMicroseconds usecNow );
	void StartNextSpeedInterval( SteamNetworkingMicroseconds usecNow );
};


//
// Pack/unpack C struct <-> protobuf message
//
extern void LinkStatsInstantaneousStructToMsg( const SteamDatagramLinkInstantaneousStats &s, CMsgSteamDatagramLinkInstantaneousStats &msg );
extern void LinkStatsInstantaneousMsgToStruct( const CMsgSteamDatagramLinkInstantaneousStats &msg, SteamDatagramLinkInstantaneousStats &s );
extern void LinkStatsLifetimeStructToMsg( const SteamDatagramLinkLifetimeStats &s, CMsgSteamDatagramLinkLifetimeStats &msg );
extern void LinkStatsLifetimeMsgToStruct( const CMsgSteamDatagramLinkLifetimeStats &msg, SteamDatagramLinkLifetimeStats &s );

inline void PutAcksIntoRepeatedField( google::protobuf::RepeatedField<uint32> &msgField, const LinkStatsTracker &stats, SteamNetworkingMicroseconds usecNow, const char *pszDebug )
{
	msgField.Reserve( stats.m_nPendingOutgoingAcks );
	for ( int i = 0 ; i < stats.m_nPendingOutgoingAcks ; ++i )
	{
		const PacketAck &ack = stats.m_arPendingOutgoingAck[i];

		SteamNetworkingMicroseconds usecThen = ack.Timestamp(usecNow);
		SteamNetworkingMicroseconds usecDelay = usecNow - usecThen;
		uint64 nDelayBits = usecDelay >> k_usecAckDelayPacketSerializedPrecisionShift;
		if ( nDelayBits & 0xffffffffffff0000ull )
		{
			AssertMsg5( false, "%s ack was pended for %lld usec, cannot pack delay properly!  usecNow=%llx, timestamp=%llx, usecThen=%llx",
				pszDebug, (long long)usecDelay, (unsigned long long)usecNow, (unsigned long long)ack.m_usecTimestamp, (unsigned long long)usecThen );
			nDelayBits = 0xffff;
		}
		uint32 unPacked = ( uint32(ack.m_nWireSeqNum)<<16 ) | (uint16)nDelayBits;

		msgField.Add( unPacked );
	}
}

template <typename MsgType >
void PutRelayAcksIntoMessage( MsgType &msg, const LinkStatsTracker &stats, SteamNetworkingMicroseconds usecNow )
{
	if ( stats.m_nPendingOutgoingAcks > 0 )
		PutAcksIntoRepeatedField( *msg.mutable_ack_relay(), stats, usecNow, "relay" );
}
template <typename MsgType >
void PutEndToEndAcksIntoMessage( MsgType &msg, const LinkStatsTracker &stats, SteamNetworkingMicroseconds usecNow )
{
	if ( stats.m_nPendingOutgoingAcks > 0 )
		PutAcksIntoRepeatedField( *msg.mutable_ack_e2e(), stats, usecNow, "e2e" );
}


} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKING_STATSUTILS_H
