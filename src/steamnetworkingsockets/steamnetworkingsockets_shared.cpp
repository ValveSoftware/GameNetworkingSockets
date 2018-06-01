//====== Copyright Valve Corporation, All rights reserved. ====================

#include <atomic>
#include <tier1/utlbuffer.h>
#include "steamnetworking_statsutils.h"

// Must be the last include
#include <tier0/memdbgon.h>

using namespace SteamNetworkingSocketsLib;

/// What universe are we running in?
EUniverse SteamNetworkingSocketsLib::g_eUniverse = k_EUniverseInvalid;

///////////////////////////////////////////////////////////////////////////////
//
// LinkStatsTracker
//
///////////////////////////////////////////////////////////////////////////////


void SteamDatagramLinkInstantaneousStats::Clear()
{
	memset( this, 0, sizeof(*this) );
	m_nPingMS = -1;
	m_flPacketsDroppedPct = -1.0f;
	m_flPacketsWeirdSequenceNumberPct = -1.0f;
	m_usecMaxJitter = -1;
	m_nSendRate = -1;
	m_nPendingBytes = 0;
}

void SteamDatagramLinkLifetimeStats::Clear()
{
	memset( this, 0, sizeof(*this) );
	m_nPingNtile5th = -1;
	m_nPingNtile50th = -1;
	m_nPingNtile75th = -1;
	m_nPingNtile95th = -1;
	m_nPingNtile98th = -1;
	m_nQualityNtile2nd = -1;
	m_nQualityNtile5th = -1;
	m_nQualityNtile25th = -1;
	m_nQualityNtile50th = -1;
	m_nTXSpeedNtile5th = -1;
	m_nTXSpeedNtile50th = -1;
	m_nTXSpeedNtile75th = -1;
	m_nTXSpeedNtile95th = -1;
	m_nTXSpeedNtile98th = -1;
	m_nRXSpeedNtile5th = -1;
	m_nRXSpeedNtile50th = -1;
	m_nRXSpeedNtile75th = -1;
	m_nRXSpeedNtile95th = -1;
	m_nRXSpeedNtile98th = -1;
}

void SteamDatagramLinkStats::Clear()
{
	m_latest.Clear();
	//m_peak.Clear();
	m_lifetime.Clear();
	m_latestRemote.Clear();
	m_flAgeLatestRemote = -1.0f;
	m_lifetimeRemote.Clear();
	m_flAgeLifetimeRemote = -1.0f;
}


void PingTracker::Reset()
{
	memset( m_arPing, 0, sizeof(m_arPing) );
	m_nValidPings = 0;
	m_nSmoothedPing = -1;
	m_usecTimeLastSentPingRequest = 0;
	m_sample.Clear();
	m_nHistogram25 = 0;
	m_nHistogram50 = 0;
	m_nHistogram75 = 0;
	m_nHistogram100 = 0;
	m_nHistogram125 = 0;
	m_nHistogram150 = 0;
	m_nHistogram200 = 0;
	m_nHistogram300 = 0;
	m_nHistogramMax = 0;
}

void PingTracker::ReceivedPing( int nPingMS, SteamNetworkingMicroseconds usecNow )
{
	Assert( nPingMS >= 0 );
	COMPILE_TIME_ASSERT( V_ARRAYSIZE(m_arPing) == 3 );

	// Collect sample
	m_sample.AddSample( Min( nPingMS, 0xffff ) );

	// Discard oldest, insert new sample at head
	m_arPing[2] = m_arPing[1];
	m_arPing[1] = m_arPing[0];
	m_arPing[0].m_nPingMS = nPingMS;
	m_arPing[0].m_usecTimeRecv = usecNow;

	// Compute smoothed ping and update sample count based on existing sample size
	switch ( m_nValidPings )
	{
		case 0:
			// First sample.  Smoothed value is simply the same thing as the sample
			m_nValidPings = 1;
			m_nSmoothedPing = nPingMS;
			break;

		case 1:
			// Second sample.  Smoothed value is the average
			m_nValidPings = 2;
			m_nSmoothedPing = ( m_arPing[0].m_nPingMS + m_arPing[1].m_nPingMS ) >> 1;
			break;

		default:
			AssertMsg1( false, "Unexpected valid ping count %d", m_nValidPings );
		case 2:
			// Just received our final sample to complete the sample
			m_nValidPings = 3;
		case 3:
		{
			// Full sample.  Take the average of the two best.  Hopefully this strategy ignores a single
			// ping spike, but without being too optimistic and underestimating the sustained latency too
			// much.  (Another option might be to use the median?)
			int nMax = Max( m_arPing[0].m_nPingMS, m_arPing[1].m_nPingMS );
			nMax = Max( nMax, m_arPing[2].m_nPingMS );
			m_nSmoothedPing = ( m_arPing[0].m_nPingMS + m_arPing[1].m_nPingMS + m_arPing[2].m_nPingMS - nMax ) >> 1;
			break;
		}
	}

	// Update histogram using hand-rolled sort-of-binary-search, optimized
	// for the expectation that most pings will be reasonable
	if ( nPingMS <= 100 )
	{
		if ( nPingMS <= 50 )
		{
			if ( nPingMS <= 25 )
				++m_nHistogram25;
			else
				++m_nHistogram50;
		}
		else
		{
			if ( nPingMS <= 75 )
				++m_nHistogram75;
			else
				++m_nHistogram100;
		}
	}
	else
	{
		if ( nPingMS <= 150 )
		{
			if ( nPingMS <= 125 )
				++m_nHistogram125;
			else
				++m_nHistogram150;
		}
		else
		{
			if ( nPingMS <= 200 )
				++m_nHistogram200;
			else if ( nPingMS <= 300 )
				++m_nHistogram300;
			else
				++m_nHistogramMax;
		}
	}

}

int PingTracker::PessimisticPingEstimate() const
{
	if ( m_nValidPings < 1 )
	{
		AssertMsg( false, "Tried to make a pessimistic ping estimate without any ping data at all!" );
		return 500;
	}
	int nResult = m_arPing[0].m_nPingMS;
	for ( int i = 1 ; i < m_nValidPings ; ++i )
		nResult = Max( nResult, m_arPing[i].m_nPingMS );
	return nResult;
}

int PingTracker::OptimisticPingEstimate() const
{
	if ( m_nValidPings < 1 )
	{
		AssertMsg( false, "Tried to make an optimistic ping estimate without any ping data at all!" );
		return 50;
	}
	int nResult = m_arPing[0].m_nPingMS;
	for ( int i = 1 ; i < m_nValidPings ; ++i )
		nResult = Min( nResult, m_arPing[i].m_nPingMS );
	return nResult;
}

void LinkStatsTracker::InitBaseLinkStatsTracker( SteamNetworkingMicroseconds usecNow, bool bStartDisconnected )
{
	m_nPeerProtocolVersion = 0;
	m_bDisconnected = false;
	m_sent.Reset();
	m_recv.Reset();
	m_recvExceedRateLimit.Reset();
	m_ping.Reset();
	m_nNextSendSequenceNumber = 1;
	m_usecTimeLastSentSeq = 0;
	m_nLastRecvSequenceNumber = 0;
	m_flInPacketsDroppedPct = -1.0f;
	m_usecMaxJitterPreviousInterval = -1;
	m_flInPacketsWeirdSequencePct = -1.0f;
	m_nPktsRecvSequenced = 0;
	m_nPktsRecvDropped = 0;
	m_nPktsRecvOutOfOrder = 0;
	m_nPktsRecvDuplicate = 0;
	m_nPktsRecvSequenceNumberLurch = 0;
	m_usecTimeLastRecv = 0;
	m_usecTimeLastRecvSeq = 0;
	memset( &m_latestRemote, 0, sizeof(m_latestRemote) );
	m_usecTimeRecvLatestRemote = 0;
	memset( &m_lifetimeRemote, 0, sizeof(m_lifetimeRemote) );
	m_usecTimeRecvLifetimeRemote = 0;
	//m_seqnumUnackedSentLifetime = -1;
	//m_seqnumPendingAckRecvTimelife = -1;
	m_nQualityHistogram100 = 0;
	m_nQualityHistogram99 = 0;
	m_nQualityHistogram97 = 0;
	m_nQualityHistogram95 = 0;
	m_nQualityHistogram90 = 0;
	m_nQualityHistogram75 = 0;
	m_nQualityHistogram50 = 0;
	m_nQualityHistogram1 = 0;
	m_nQualityHistogramDead = 0;
	m_qualitySample.Clear();

	m_nJitterHistogramNegligible = 0;
	m_nJitterHistogram1 = 0;
	m_nJitterHistogram2 = 0;
	m_nJitterHistogram5 = 0;
	m_nJitterHistogram10 = 0;
	m_nJitterHistogram20 = 0;

	InternalSetDisconnected( bStartDisconnected, usecNow );
}

void LinkStatsTracker::InternalSetDisconnected( bool bFlag, SteamNetworkingMicroseconds usecNow )
{
	m_bDisconnected = bFlag;

	m_seqNumInFlight = 0;
	m_bInFlightInstantaneous = false;
	m_bInFlightLifetime = false;
	PeerAckedInstantaneous( usecNow );
	PeerAckedLifetime( usecNow );

	// Clear acks we expect, on either state change.
	m_usecInFlightReplyTimeout = 0;
	m_usecLastSendPacketExpectingImmediateReply = 0;
	m_nReplyTimeoutsSinceLastRecv = 0;
	m_usecWhenTimeoutStarted = 0;
	m_expectedAcks.Clear();
	m_nPendingOutgoingAcks = 0;
	m_bPendingAckImmediate = false;

	if ( !bFlag )
	{
		StartNextInterval( usecNow );
	}
}

void LinkStatsTracker::StartNextInterval( SteamNetworkingMicroseconds usecNow )
{
	m_nPktsRecvSequencedCurrentInterval = 0;
	m_nPktsRecvDroppedCurrentInterval = 0;
	m_nPktsRecvWeirdSequenceCurrentInterval = 0;
	m_usecMaxJitterCurrentInterval = -1;
	m_usecIntervalStart = usecNow;
}

void LinkStatsTracker::ThinkBaseLinkStatsTracker( SteamNetworkingMicroseconds usecNow )
{
	// Check for ending the current QoS interval
	if ( !m_bDisconnected && m_usecIntervalStart + k_usecSteamDatagramLinkStatsDefaultInterval < usecNow )
	{
		UpdateInterval( usecNow );
	}
	
	// Check for reply timeout that we count.  (We intentionally only allow
	// one of this type of timeout to be in flight at a time, so that the max
	// rate that we accumulate them is based on the ping time, instead of the packet
	// rate.
	if ( m_usecInFlightReplyTimeout > 0 && m_usecInFlightReplyTimeout < usecNow )
	{
		m_usecInFlightReplyTimeout = 0;
		if ( m_usecWhenTimeoutStarted == 0 )
		{
			Assert( m_nReplyTimeoutsSinceLastRecv == 0 );
			m_usecWhenTimeoutStarted = usecNow;
		}
		++m_nReplyTimeoutsSinceLastRecv;
	}

	// Check for expiring expected acks.
	if ( !m_expectedAcks.m_listAcks.IsEmpty() )
	{
		SteamNetworkingMicroseconds usecExpiry = usecNow - CalcConservativeTimeout();
		PacketAck ackTimedOut;
		while ( m_expectedAcks.BRemoveOldestAckIfTimedOut( ackTimedOut, usecExpiry ) )
		{
			if ( m_seqNumInFlight == ackTimedOut.m_nWireSeqNum )
			{
				// They probably didn't receive these stats, we should send again
				m_seqNumInFlight = 0;
				m_bInFlightInstantaneous = m_bInFlightLifetime = false;
			}
		}
	}
}

void LinkStatsTracker::UpdateInterval( SteamNetworkingMicroseconds usecNow )
{
	float flElapsed = int64( usecNow - m_usecIntervalStart ) * 1e-6;
	flElapsed = Max( flElapsed, .001f ); // make sure math doesn't blow up

	// Check if enough happened in this interval to make a meaningful judgment about connection quality
	COMPILE_TIME_ASSERT( k_usecSteamDatagramLinkStatsDefaultInterval >= 5*k_nMillion );
	if ( flElapsed > 4.5f )
	{
		if ( m_nPktsRecvSequencedCurrentInterval > 5 )
		{
			int nBad = m_nPktsRecvDroppedCurrentInterval + m_nPktsRecvWeirdSequenceCurrentInterval;
			if ( nBad == 0 )
			{
				// Perfect connection.  This will hopefully be relatively common
				m_qualitySample.AddSample( 100 );
				++m_nQualityHistogram100;
			}
			else
			{

				// Less than perfect.  Compute quality metric.
				int nTotalSent = m_nPktsRecvSequencedCurrentInterval + m_nPktsRecvDroppedCurrentInterval;
				int nRecvGood = m_nPktsRecvSequencedCurrentInterval - m_nPktsRecvWeirdSequenceCurrentInterval;
				int nQuality = nRecvGood * 100 / nTotalSent;

				// Cap at 99, since 100 is reserved to mean "perfect",
				// I don't think it's possible for the calculation above to ever produce 100, but whatever.
				if ( nQuality >= 99 )
				{
					m_qualitySample.AddSample( 99 );
					++m_nQualityHistogram99;
				}
				else if ( nQuality <= 1 ) // in case accounting is hosed or every single packet was out of order, clamp.  0 means "totally dead connection"
				{
					m_qualitySample.AddSample( 1 );
					++m_nQualityHistogram1;
				}
				else
				{
					m_qualitySample.AddSample( nQuality );
					if ( nQuality >= 97 )
						++m_nQualityHistogram97;
					else if ( nQuality >= 95 )
						++m_nQualityHistogram95;
					else if ( nQuality >= 90 )
						++m_nQualityHistogram90;
					else if ( nQuality >= 75 )
						++m_nQualityHistogram75;
					else if ( nQuality >= 50 )
						++m_nQualityHistogram50;
					else
						++m_nQualityHistogram1;
				}
			}
		}
		else if ( m_recv.m_packets.m_nCurrentInterval == 0 && m_sent.m_packets.m_nCurrentInterval > (int64)( flElapsed ) && m_nReplyTimeoutsSinceLastRecv >= 2 )
		{
			COMPILE_TIME_ASSERT( k_usecSteamDatagramClientPingTimeout + k_usecSteamDatagramRouterPendClientPing < k_nMillion );

			// He's dead, Jim.  But we've been trying pretty hard to talk to him, so it probably isn't
			// because the connection is just idle or shutting down.  The connection has probably
			// dropped.
			m_qualitySample.AddSample(0);
			++m_nQualityHistogramDead;
		}
	}

	// PacketRate class does most of the work
	m_sent.UpdateInterval( flElapsed );
	m_recv.UpdateInterval( flElapsed );
	m_recvExceedRateLimit.UpdateInterval( flElapsed );

	// Calculate rate of packet flow imperfections
	Assert( m_nPktsRecvWeirdSequenceCurrentInterval <= m_nPktsRecvSequencedCurrentInterval );
	if ( m_nPktsRecvSequencedCurrentInterval <= 0 )
	{
		// No sequenced packets received during interval, so no data available
		m_flInPacketsDroppedPct = -1.0f;
		m_flInPacketsWeirdSequencePct = -1.0f;
	}
	else
	{
		float flToPct = 1.0f / float( m_nPktsRecvSequencedCurrentInterval + m_nPktsRecvDroppedCurrentInterval );
		m_flInPacketsDroppedPct = m_nPktsRecvDroppedCurrentInterval * flToPct;
		m_flInPacketsWeirdSequencePct = m_nPktsRecvWeirdSequenceCurrentInterval * flToPct;
	}

	// Peak jitter value
	m_usecMaxJitterPreviousInterval = m_usecMaxJitterCurrentInterval;

	// Reset for next time
	StartNextInterval( usecNow );
}

void LinkStatsTracker::TrackRecvSequencedPacket( uint16 unWireSequenceNumber, SteamNetworkingMicroseconds usecNow, int usecSenderTimeSincePrev )
{
	int16 nGap = unWireSequenceNumber - uint16( m_nLastRecvSequenceNumber );
	int64 nFullSequenceNumber = m_nLastRecvSequenceNumber + nGap;
	Assert( uint16( nFullSequenceNumber ) == unWireSequenceNumber );

	TrackRecvSequencedPacketGap( nGap, usecNow, usecSenderTimeSincePrev );
}

void LinkStatsTracker::TrackRecvSequencedPacketGap( int16 nGap, SteamNetworkingMicroseconds usecNow, int usecSenderTimeSincePrev )
{

	// Update stats
	++m_nPktsRecvSequencedCurrentInterval;
	++m_nPktsRecvSequenced;
	++m_nPktsRecvSeqSinceSentLifetime;
	++m_nPktsRecvSeqSinceSentInstantaneous;

	// Check for dropped packet.  Since we hope that by far the most common
	// case will be packets delivered in order, we optimize this logic
	// for that case.
	if ( nGap == 1 )
	{

		// We've received two packets, in order.  Did the sender supply the time between packets on his side?
		if ( usecSenderTimeSincePrev > 0 )
		{
			int usecJitter = ( usecNow - m_usecTimeLastRecvSeq ) - usecSenderTimeSincePrev;
			usecJitter = abs( usecJitter );
			if ( usecJitter < k_usecTimeSinceLastPacketMaxReasonable )
			{

				// Update max jitter for current interval
				m_usecMaxJitterCurrentInterval = Max( m_usecMaxJitterCurrentInterval, usecJitter );

				// Add to histogram
				if ( usecJitter < 1000 )
					++m_nJitterHistogramNegligible;
				else if ( usecJitter < 2000 )
					++m_nJitterHistogram1;
				else if ( usecJitter < 5000 )
					++m_nJitterHistogram2;
				else if ( usecJitter < 10000 )
					++m_nJitterHistogram5;
				else if ( usecJitter < 20000 )
					++m_nJitterHistogram10;
				else
					++m_nJitterHistogram20;
			}
			else
			{
				// Something is really, really off.  Discard measurement
			}
		}

	}
	else
	{
		// Classify imperfection based on gap size.
		if ( nGap < -10 || nGap >= 100 )
		{
			// Very weird.
			++m_nPktsRecvSequenceNumberLurch;
			++m_nPktsRecvWeirdSequenceCurrentInterval;

			// Continue to code below, reseting the sequence number
			// for packets going forward.
		}
		else if ( nGap > 0 )
		{
			// Probably the most common case, we just dropped a packet
			int nDropped = nGap-1;
			m_nPktsRecvDropped += nDropped;
			m_nPktsRecvDroppedCurrentInterval += nDropped;
		}
		else if ( nGap == 0 )
		{
			// Same sequence number as last time.
			// Packet was delivered multiple times.
			++m_nPktsRecvDuplicate;
			++m_nPktsRecvWeirdSequenceCurrentInterval;

			// NOTE: There is no mechanism in this layer
			// of the code to prevent the processing of
			// the duplicate by the application layer!
		}
		else
		{
			// Small negative gap.  Looks like packets were delivered
			// out of order (or multiple times).
			++m_nPktsRecvOutOfOrder;
			++m_nPktsRecvWeirdSequenceCurrentInterval;
		}
	}

	// Save highest known sequence number for next time.
	if ( nGap > 0 )
	{
		m_nLastRecvSequenceNumber += nGap;
		m_usecTimeLastRecvSeq = usecNow;
	}
}

bool LinkStatsTracker::BCheckHaveDataToSendInstantaneous( SteamNetworkingMicroseconds usecNow )
{
	Assert( !m_bDisconnected );

	// How many packets a second to we expect to send on an "active" connection?
	const int64 k_usecActiveConnectionSendInterval = 3*k_nMillion;
	COMPILE_TIME_ASSERT( k_usecSteamDatagramClientPingTimeout*2 < k_usecActiveConnectionSendInterval );
	COMPILE_TIME_ASSERT( k_usecSteamDatagramClientBackupRouterKeepaliveInterval > k_usecActiveConnectionSendInterval*5 ); // make sure backup keepalive interval isn't anywhere near close enough to trigger this.

	// Calculate threshold based on how much time has elapsed and a very low packet rate
	int64 usecElapsed = usecNow - m_usecPeerAckedInstaneous;
	Assert( usecElapsed >= k_usecLinkStatsInstantaneousReportMinInterval ); // don't call this unless you know it's been long enough!
	int nThreshold = usecElapsed / k_usecActiveConnectionSendInterval;

	// Have they been trying to talk to us?
	if ( m_nPktsRecvSeqSinceSentInstantaneous > nThreshold || m_nPktsSentSinceSentInstantaneous > nThreshold )
		return true;

	// Connection has been idle since the last time we sent instantaneous stats.
	// Don't actually send stats, but clear counters and timers and act like we did.
	PeerAckedInstantaneous( usecNow );

	// And don't send anything
	return false;
}

bool LinkStatsTracker::BCheckHaveDataToSendLifetime( SteamNetworkingMicroseconds usecNow )
{
	Assert( !m_bDisconnected );

	// Make sure we have something new to report since the last time we sent stats
	if ( m_nPktsRecvSeqSinceSentLifetime > 100 || m_nPktsSentSinceSentLifetime > 100 )
		return true;

	// Reset the timer.  But do NOT reset the packet counters.  So if the connection isn't
	// dropped, and we are sending keepalives very slowly, this will just send some stats
	// along about every 100 packets or so.  Typically we'll drop the session before that
	// happens.
	m_usecPeerAckedLifetime = usecNow;

	// Don't send anything now
	return false;
}

bool LinkStatsTracker::BNeedToSendStatsOrAcks( SteamNetworkingMicroseconds usecNow )
{
	// Check if any acks need to be sent now
	if ( m_nPendingOutgoingAcks > 0 )
	{
		// Ack list getting full?
		if ( m_nPendingOutgoingAcks >= k_nMaxPendingAcks )
			return true;

		// Most recent ack was requested to be sent immediately?
		if ( m_bPendingAckImmediate )
			return true;

		// Is the oldest pending ack getting pretty stale?
		if ( m_arPendingOutgoingAck[0].MicrosecondsAge( usecNow ) > k_usecMaxAckStatsDelay )
			return true;
	}

	// Message already in flight?
	if ( m_seqNumInFlight != 0 || m_bDisconnected )
		return false;
	bool bNeedToSendInstantaneous = ( m_usecPeerAckedInstaneous + k_usecLinkStatsInstantaneousReportMaxInterval < usecNow ) && BCheckHaveDataToSendInstantaneous( usecNow );
	bool bNeedToSendLifetime = ( m_usecPeerAckedLifetime + k_usecLinkStatsLifetimeReportMaxInterval < usecNow ) && BCheckHaveDataToSendLifetime( usecNow );
	return bNeedToSendInstantaneous || bNeedToSendLifetime;
}

void LinkStatsTracker::PopulateMessage( CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow )
{
	if ( m_seqNumInFlight == 0 && !m_bDisconnected )
	{

		// Ready to send instantaneous stats?
		if ( m_usecPeerAckedInstaneous + k_usecLinkStatsInstantaneousReportMinInterval < usecNow && BCheckHaveDataToSendInstantaneous( usecNow ) )
		{
			// !KLUDGE! Go through public struct as intermediary to keep code simple.
			SteamDatagramLinkInstantaneousStats sInstant;
			GetInstantaneousStats( sInstant );
			LinkStatsInstantaneousStructToMsg( sInstant, *msg.mutable_instantaneous() );
		}

		// Ready to send lifetime stats?
		if ( m_usecPeerAckedLifetime + k_usecLinkStatsLifetimeReportMinInterval < usecNow && BCheckHaveDataToSendLifetime( usecNow ) )
		{
			// !KLUDGE! Go through public struct as intermediary to keep code simple.
			SteamDatagramLinkLifetimeStats sLifetime;
			GetLifetimeStats( sLifetime );
			LinkStatsLifetimeStructToMsg( sLifetime, *msg.mutable_lifetime() );
		}
	}
}

void LinkStatsTracker::TrackSentMessageExpectingReply( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
{
	if ( m_usecInFlightReplyTimeout == 0 )
	{
		m_usecInFlightReplyTimeout = usecNow + k_usecSteamDatagramClientPingTimeout;  // FIXME - we could be a lot smarter about this timeout using the ping estimate!
		if ( bAllowDelayedReply )
			m_usecInFlightReplyTimeout += k_usecSteamDatagramRouterPendClientPing;
	}
	if ( !bAllowDelayedReply )
		m_usecLastSendPacketExpectingImmediateReply = usecNow;
}

void LinkStatsTracker::TrackSentMessageExpectingSeqNumAck( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
{
	// This counts as a ping request
	TrackSentPingRequest( usecNow, bAllowDelayedReply );

	// Remember when we sent this, so that when we receive the ack we can use it as a latency estimate
	m_expectedAcks.AddExpectedAck( uint16( m_nNextSendSequenceNumber-1 ), usecNow );
}

bool LinkStatsTracker::RecvAck( uint16 nWireSeqNum, uint16 nPackedDelay, SteamNetworkingMicroseconds usecNow )
{

	// Acking stats that we sent?  Note that in general, we should also have an ack record
	if ( nWireSeqNum == m_seqNumInFlight )
	{
		if ( m_bInFlightInstantaneous )
			PeerAckedInstantaneous( usecNow );
		if ( m_bInFlightLifetime )
			PeerAckedLifetime( usecNow );
		m_seqNumInFlight = 0;
		m_bInFlightInstantaneous = m_bInFlightLifetime = false;
	}

	// Locate the ack
	SteamNetworkingMicroseconds usecSent = m_expectedAcks.GetTimeSentAndRemoveAck( nWireSeqNum, usecNow );
	if ( usecSent == 0 )
		return true;
	if ( usecSent < 0 )
		return false;

	SteamNetworkingMicroseconds usecTotalPing = usecNow - usecSent;
	if ( usecTotalPing > 0 )
	{

		// Unpack the delay
		SteamNetworkingMicroseconds usecDelay = SteamNetworkingMicroseconds(nPackedDelay) << k_usecAckDelayPacketSerializedPrecisionShift;

		int msPing = ( usecTotalPing - usecDelay ) / 1000;
		if ( msPing < -1 || msPing > 3000 )
		{
			// Hm - suspicious.  Let caller know so they can spew if they want to
			return false;
		}
		if ( msPing < 0 )
			msPing = 0;
		m_ping.ReceivedPing( msPing, usecNow );
	}

	return true;
}

void LinkStatsTracker::TrackSentStats( const CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
{

	// Check if we expect our peer to know how to acknowledge this
	if ( !m_bDisconnected )
	{
		m_seqNumInFlight = uint16( m_nNextSendSequenceNumber-1 );
		m_bInFlightInstantaneous = msg.has_instantaneous();
		m_bInFlightLifetime = msg.has_lifetime();

		// They should ack.  Make a note of the sequence number that we used,
		// so that we can measure latency when they reply, setup timeout bookkeeping, etc
		TrackSentMessageExpectingSeqNumAck( usecNow, bAllowDelayedReply );
	}
	else
	{
		// Peer can't ack.  Just mark them as acking immediately
		Assert( m_seqNumInFlight == 0 );
		m_seqNumInFlight = 0;
		m_bInFlightInstantaneous = false;
		m_bInFlightLifetime = false;
		if ( msg.has_instantaneous() )
			PeerAckedInstantaneous( usecNow );
		if ( msg.has_lifetime() )
			PeerAckedLifetime( usecNow );
	}
}

void LinkStatsTracker::ProcessMessage( const CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow )
{
	if ( msg.has_instantaneous() )
	{
		LinkStatsInstantaneousMsgToStruct( msg.instantaneous(), m_latestRemote );
		m_usecTimeRecvLatestRemote = usecNow;
	}
	if ( msg.has_lifetime() )
	{
		LinkStatsLifetimeMsgToStruct( msg.lifetime(), m_lifetimeRemote );
		m_usecTimeRecvLifetimeRemote = usecNow;
	}
}

void LinkStatsTracker::GetInstantaneousStats( SteamDatagramLinkInstantaneousStats &s ) const
{
	s.m_flOutPacketsPerSec = m_sent.m_packets.m_flRate;
	s.m_flOutBytesPerSec = m_sent.m_bytes.m_flRate;
	s.m_flInPacketsPerSec = m_recv.m_packets.m_flRate;
	s.m_flInBytesPerSec = m_recv.m_bytes.m_flRate;
	s.m_nPingMS = m_ping.m_nSmoothedPing;
	s.m_flPacketsDroppedPct = m_flInPacketsDroppedPct;
	s.m_flPacketsWeirdSequenceNumberPct = m_flInPacketsWeirdSequencePct;
	s.m_usecMaxJitter = m_usecMaxJitterPreviousInterval;
}

void LinkStatsTracker::GetLifetimeStats( SteamDatagramLinkLifetimeStats &s ) const
{
	s.m_nPacketsSent = m_sent.m_packets.m_nTotal;
	s.m_nBytesSent = m_sent.m_bytes.m_nTotal;
	s.m_nPacketsRecv = m_recv.m_packets.m_nTotal;
	s.m_nBytesRecv = m_recv.m_bytes.m_nTotal;
	s.m_nPktsRecvSequenced = m_nPktsRecvSequenced;
	s.m_nPktsRecvDropped = m_nPktsRecvDropped;
	s.m_nPktsRecvOutOfOrder = m_nPktsRecvOutOfOrder;
	s.m_nPktsRecvDuplicate = m_nPktsRecvDuplicate;
	s.m_nPktsRecvSequenceNumberLurch = m_nPktsRecvSequenceNumberLurch;

	s.m_nQualityHistogram100 = m_nQualityHistogram100;
	s.m_nQualityHistogram99 = m_nQualityHistogram99;
	s.m_nQualityHistogram97 = m_nQualityHistogram97;
	s.m_nQualityHistogram95 = m_nQualityHistogram95;
	s.m_nQualityHistogram90 = m_nQualityHistogram90;
	s.m_nQualityHistogram75 = m_nQualityHistogram75;
	s.m_nQualityHistogram50 = m_nQualityHistogram50;
	s.m_nQualityHistogram1 = m_nQualityHistogram1;
	s.m_nQualityHistogramDead = m_nQualityHistogramDead;

	s.m_nQualityNtile50th = m_qualitySample.NumSamples() <  2 ? -1 : m_qualitySample.GetPercentile( .50f );
	s.m_nQualityNtile25th = m_qualitySample.NumSamples() <  4 ? -1 : m_qualitySample.GetPercentile( .25f );
	s.m_nQualityNtile5th  = m_qualitySample.NumSamples() < 20 ? -1 : m_qualitySample.GetPercentile( .05f );
	s.m_nQualityNtile2nd  = m_qualitySample.NumSamples() < 50 ? -1 : m_qualitySample.GetPercentile( .02f );

	s.m_nPingHistogram25  = m_ping.m_nHistogram25;
	s.m_nPingHistogram50  = m_ping.m_nHistogram50;
	s.m_nPingHistogram75  = m_ping.m_nHistogram75;
	s.m_nPingHistogram100 = m_ping.m_nHistogram100;
	s.m_nPingHistogram125 = m_ping.m_nHistogram125;
	s.m_nPingHistogram150 = m_ping.m_nHistogram150;
	s.m_nPingHistogram200 = m_ping.m_nHistogram200;
	s.m_nPingHistogram300 = m_ping.m_nHistogram300;
	s.m_nPingHistogramMax = m_ping.m_nHistogramMax;

	s.m_nPingNtile5th  = m_ping.m_sample.NumSamples() < 20 ? -1 : m_ping.m_sample.GetPercentile( .05f );
	s.m_nPingNtile50th = m_ping.m_sample.NumSamples() <  2 ? -1 : m_ping.m_sample.GetPercentile( .50f );
	s.m_nPingNtile75th = m_ping.m_sample.NumSamples() <  4 ? -1 : m_ping.m_sample.GetPercentile( .75f );
	s.m_nPingNtile95th = m_ping.m_sample.NumSamples() < 20 ? -1 : m_ping.m_sample.GetPercentile( .95f );
	s.m_nPingNtile98th = m_ping.m_sample.NumSamples() < 50 ? -1 : m_ping.m_sample.GetPercentile( .98f );

	s.m_nJitterHistogramNegligible = m_nJitterHistogramNegligible;
	s.m_nJitterHistogram1 = m_nJitterHistogram1;
	s.m_nJitterHistogram2 = m_nJitterHistogram2;
	s.m_nJitterHistogram5 = m_nJitterHistogram5;
	s.m_nJitterHistogram10 = m_nJitterHistogram10;
	s.m_nJitterHistogram20 = m_nJitterHistogram20;

	//
	// Clear all end-to-end values
	//

	s.m_nTXSpeedMax           = -1;

	s.m_nTXSpeedHistogram16   = 0;
	s.m_nTXSpeedHistogram32   = 0;  
	s.m_nTXSpeedHistogram64   = 0;  
	s.m_nTXSpeedHistogram128  = 0;
	s.m_nTXSpeedHistogram256  = 0;
	s.m_nTXSpeedHistogram512  = 0;
	s.m_nTXSpeedHistogram1024 = 0;
	s.m_nTXSpeedHistogramMax  = 0;

	s.m_nTXSpeedNtile5th  = -1;
	s.m_nTXSpeedNtile50th = -1;
	s.m_nTXSpeedNtile75th = -1;
	s.m_nTXSpeedNtile95th = -1;
	s.m_nTXSpeedNtile98th = -1;

	s.m_nRXSpeedMax           = -1;

	s.m_nRXSpeedHistogram16   = 0;
	s.m_nRXSpeedHistogram32   = 0;
	s.m_nRXSpeedHistogram64   = 0;
	s.m_nRXSpeedHistogram128  = 0;
	s.m_nRXSpeedHistogram256  = 0;
	s.m_nRXSpeedHistogram512  = 0;
	s.m_nRXSpeedHistogram1024 = 0;
	s.m_nRXSpeedHistogramMax  = 0;

	s.m_nRXSpeedNtile5th  = -1;
	s.m_nRXSpeedNtile50th = -1;
	s.m_nRXSpeedNtile75th = -1;
	s.m_nRXSpeedNtile95th = -1;
	s.m_nRXSpeedNtile98th = -1;
}

void LinkStatsTracker::GetLinkStats( SteamDatagramLinkStats &s, SteamNetworkingMicroseconds usecNow ) const
{
	GetInstantaneousStats( s.m_latest );
	GetLifetimeStats( s.m_lifetime );

	if ( m_usecTimeRecvLatestRemote )
	{
		s.m_latestRemote = m_latestRemote;
		s.m_flAgeLatestRemote = ( usecNow - m_usecTimeRecvLatestRemote ) * 1e-6;
	}
	else
	{
		s.m_latestRemote.Clear();
		s.m_flAgeLatestRemote = -1.0f;
	}

	if ( m_usecTimeRecvLifetimeRemote )
	{
		s.m_lifetimeRemote = m_lifetimeRemote;
		s.m_flAgeLifetimeRemote = ( usecNow - m_usecTimeRecvLifetimeRemote ) * 1e-6;
	}
	else
	{
		s.m_lifetimeRemote.Clear();
		s.m_flAgeLifetimeRemote = -1.0f;
	}
}

void LinkStatsTrackerEndToEnd::Init( SteamNetworkingMicroseconds usecNow, bool bStartDisconnected )
{
	InitBaseLinkStatsTracker( usecNow, bStartDisconnected );

	m_TXSpeedSample.Clear();
	m_nTXSpeed = 0;
	m_nTXSpeedHistogram16 = 0; // Speed at kb/s
	m_nTXSpeedHistogram32 = 0; 
	m_nTXSpeedHistogram64 = 0;
	m_nTXSpeedHistogram128 = 0;
	m_nTXSpeedHistogram256 = 0;
	m_nTXSpeedHistogram512 = 0;
	m_nTXSpeedHistogram1024 = 0;
	m_nTXSpeedHistogramMax = 0;

	m_RXSpeedSample.Clear();
	m_nRXSpeed = 0;
	m_nRXSpeedHistogram16 = 0; // Speed at kb/s
	m_nRXSpeedHistogram32 = 0; 
	m_nRXSpeedHistogram64 = 0;
	m_nRXSpeedHistogram128 = 0;
	m_nRXSpeedHistogram256 = 0;
	m_nRXSpeedHistogram512 = 0;
	m_nRXSpeedHistogram1024 = 0;
	m_nRXSpeedHistogramMax = 0;

	StartNextSpeedInterval( usecNow );
}

void LinkStatsTrackerEndToEnd::Think( SteamNetworkingMicroseconds usecNow )
{
	ThinkBaseLinkStatsTracker( usecNow );

	if ( m_usecSpeedIntervalStart + k_usecSteamDatagramSpeedStatsDefaultInterval < usecNow )
	{
		UpdateSpeedInterval( usecNow );
	}
}

void LinkStatsTrackerEndToEnd::StartNextSpeedInterval( SteamNetworkingMicroseconds usecNow )
{
	m_usecSpeedIntervalStart = usecNow;
}

void LinkStatsTrackerEndToEnd::UpdateSpeedInterval( SteamNetworkingMicroseconds usecNow )
{
	float flElapsed = int64( usecNow - m_usecIntervalStart ) * 1e-6;
	flElapsed = Max( flElapsed, .001f ); // make sure math doesn't blow up

	int nTXKBs = ( m_nTXSpeed + 512 ) / 1024;
	m_TXSpeedSample.AddSample( nTXKBs );

	if ( nTXKBs <= 16 ) ++m_nTXSpeedHistogram16;
	else if ( nTXKBs <= 32 ) ++m_nTXSpeedHistogram32; 
	else if ( nTXKBs <= 64 ) ++m_nTXSpeedHistogram64;
	else if ( nTXKBs <= 128 ) ++m_nTXSpeedHistogram128;
	else if ( nTXKBs <= 256 ) ++m_nTXSpeedHistogram256;
	else if ( nTXKBs <= 512 ) ++m_nTXSpeedHistogram512;
	else if ( nTXKBs <= 1024 ) ++m_nTXSpeedHistogram1024;
	else ++m_nTXSpeedHistogramMax;
	
	int nRXKBs = ( m_nRXSpeed + 512 ) / 1024;
	m_RXSpeedSample.AddSample( nRXKBs );

	if ( nRXKBs <= 16 ) ++m_nRXSpeedHistogram16;
	else if ( nRXKBs <= 32 ) ++m_nRXSpeedHistogram32; 
	else if ( nRXKBs <= 64 ) ++m_nRXSpeedHistogram64;
	else if ( nRXKBs <= 128 ) ++m_nRXSpeedHistogram128;
	else if ( nRXKBs <= 256 ) ++m_nRXSpeedHistogram256;
	else if ( nRXKBs <= 512 ) ++m_nRXSpeedHistogram512;
	else if ( nRXKBs <= 1024 ) ++m_nRXSpeedHistogram1024;
	else ++m_nRXSpeedHistogramMax;

	// Reset for next time
	StartNextSpeedInterval( usecNow );
}

void LinkStatsTrackerEndToEnd::UpdateSpeeds( int nTXSpeed, int nRXSpeed )
{
	m_nTXSpeed = nTXSpeed;
	m_nRXSpeed = nRXSpeed;

	m_nTXSpeedMax = Max( m_nTXSpeedMax, nTXSpeed );
	m_nRXSpeedMax = Max( m_nRXSpeedMax, nRXSpeed );
}

void LinkStatsTrackerEndToEnd::GetLifetimeStats( SteamDatagramLinkLifetimeStats &s ) const
{
	LinkStatsTracker::GetLifetimeStats(s);

	s.m_nTXSpeedMax           = m_nTXSpeedMax;

	s.m_nTXSpeedHistogram16   = m_nTXSpeedHistogram16;
	s.m_nTXSpeedHistogram32   = m_nTXSpeedHistogram32;  
	s.m_nTXSpeedHistogram64   = m_nTXSpeedHistogram64;  
	s.m_nTXSpeedHistogram128  = m_nTXSpeedHistogram128; 
	s.m_nTXSpeedHistogram256  = m_nTXSpeedHistogram256; 
	s.m_nTXSpeedHistogram512  = m_nTXSpeedHistogram512; 
	s.m_nTXSpeedHistogram1024 = m_nTXSpeedHistogram1024;
	s.m_nTXSpeedHistogramMax  = m_nTXSpeedHistogramMax; 

	s.m_nTXSpeedNtile5th  = m_TXSpeedSample.NumSamples() < 20 ? -1 : m_TXSpeedSample.GetPercentile( .05f );
	s.m_nTXSpeedNtile50th = m_TXSpeedSample.NumSamples() <  2 ? -1 : m_TXSpeedSample.GetPercentile( .50f );
	s.m_nTXSpeedNtile75th = m_TXSpeedSample.NumSamples() <  4 ? -1 : m_TXSpeedSample.GetPercentile( .75f );
	s.m_nTXSpeedNtile95th = m_TXSpeedSample.NumSamples() < 20 ? -1 : m_TXSpeedSample.GetPercentile( .95f );
	s.m_nTXSpeedNtile98th = m_TXSpeedSample.NumSamples() < 50 ? -1 : m_TXSpeedSample.GetPercentile( .98f );

	s.m_nRXSpeedMax           = m_nRXSpeedMax;

	s.m_nRXSpeedHistogram16   = m_nRXSpeedHistogram16;
	s.m_nRXSpeedHistogram32   = m_nRXSpeedHistogram32;  
	s.m_nRXSpeedHistogram64   = m_nRXSpeedHistogram64;  
	s.m_nRXSpeedHistogram128  = m_nRXSpeedHistogram128; 
	s.m_nRXSpeedHistogram256  = m_nRXSpeedHistogram256; 
	s.m_nRXSpeedHistogram512  = m_nRXSpeedHistogram512; 
	s.m_nRXSpeedHistogram1024 = m_nRXSpeedHistogram1024;
	s.m_nRXSpeedHistogramMax  = m_nRXSpeedHistogramMax; 

	s.m_nRXSpeedNtile5th  = m_RXSpeedSample.NumSamples() < 20 ? -1 : m_RXSpeedSample.GetPercentile( .05f );
	s.m_nRXSpeedNtile50th = m_RXSpeedSample.NumSamples() <  2 ? -1 : m_RXSpeedSample.GetPercentile( .50f );
	s.m_nRXSpeedNtile75th = m_RXSpeedSample.NumSamples() <  4 ? -1 : m_RXSpeedSample.GetPercentile( .75f );
	s.m_nRXSpeedNtile95th = m_RXSpeedSample.NumSamples() < 20 ? -1 : m_RXSpeedSample.GetPercentile( .95f );
	s.m_nRXSpeedNtile98th = m_RXSpeedSample.NumSamples() < 50 ? -1 : m_RXSpeedSample.GetPercentile( .98f );
}

namespace SteamNetworkingSocketsLib
{

void LinkStatsInstantaneousStructToMsg( const SteamDatagramLinkInstantaneousStats &s, CMsgSteamDatagramLinkInstantaneousStats &msg )
{
	msg.set_out_packets_per_sec_x10( uint32( s.m_flOutPacketsPerSec * 10.0f ) );
	msg.set_out_bytes_per_sec( uint32( s.m_flOutBytesPerSec ) );
	msg.set_in_packets_per_sec_x10( uint32( s.m_flInPacketsPerSec * 10.0f ) );
	msg.set_in_bytes_per_sec( uint32( s.m_flInBytesPerSec ) );
	if ( s.m_nPingMS >= 0 )
		msg.set_ping_ms( uint32( s.m_nPingMS ) );
	if ( s.m_flPacketsDroppedPct >= 0.0f )
		msg.set_packets_dropped_pct( uint32( s.m_flPacketsDroppedPct * 100.0f ) );
	if ( s.m_flPacketsWeirdSequenceNumberPct >= 0.0f )
		msg.set_packets_weird_sequence_pct( uint32( s.m_flPacketsWeirdSequenceNumberPct * 100.0f ) );
	if ( s.m_usecMaxJitter >= 0 )
		msg.set_peak_jitter_usec( s.m_usecMaxJitter );
}

void LinkStatsInstantaneousMsgToStruct( const CMsgSteamDatagramLinkInstantaneousStats &msg, SteamDatagramLinkInstantaneousStats &s )
{
	s.m_flOutPacketsPerSec = msg.out_packets_per_sec_x10() * .1f;
	s.m_flOutBytesPerSec = msg.out_bytes_per_sec();
	s.m_flInPacketsPerSec = msg.in_packets_per_sec_x10() * .1f;
	s.m_flInBytesPerSec = msg.in_bytes_per_sec();
	if ( msg.has_ping_ms() )
		s.m_nPingMS = msg.ping_ms();
	else
		s.m_nPingMS = -1;

	if ( msg.has_packets_dropped_pct() )
		s.m_flPacketsDroppedPct = msg.packets_dropped_pct() * .01f;
	else
		s.m_flPacketsDroppedPct = -1.0f;

	if ( msg.has_packets_weird_sequence_pct() )
		s.m_flPacketsWeirdSequenceNumberPct = msg.packets_weird_sequence_pct() * .01f;
	else
		s.m_flPacketsWeirdSequenceNumberPct = -1.0f;

	if ( msg.has_peak_jitter_usec() )
		s.m_usecMaxJitter = msg.peak_jitter_usec();
	else
		s.m_usecMaxJitter = -1;

}

void LinkStatsLifetimeStructToMsg( const SteamDatagramLinkLifetimeStats &s, CMsgSteamDatagramLinkLifetimeStats &msg )
{
	msg.set_packets_sent( s.m_nPacketsSent );
	msg.set_kb_sent( ( s.m_nBytesSent + 512 ) / 1024 );
	msg.set_packets_recv( s.m_nPacketsRecv );
	msg.set_kb_recv( ( s.m_nBytesRecv + 512 ) / 1024 );
	msg.set_packets_recv_sequenced( s.m_nPktsRecvSequenced );
	msg.set_packets_recv_dropped( s.m_nPktsRecvDropped );
	msg.set_packets_recv_out_of_order( s.m_nPktsRecvOutOfOrder );
	msg.set_packets_recv_duplicate( s.m_nPktsRecvDuplicate );
	msg.set_packets_recv_lurch( s.m_nPktsRecvSequenceNumberLurch );

	#define SET_HISTOGRAM( mbr, field ) if ( mbr > 0 ) msg.set_ ## field( mbr );
	#define SET_NTILE( mbr, field ) if ( mbr >= 0 ) msg.set_ ## field( mbr );

	SET_HISTOGRAM( s.m_nQualityHistogram100 , quality_histogram_100  )
	SET_HISTOGRAM( s.m_nQualityHistogram99  , quality_histogram_99   )
	SET_HISTOGRAM( s.m_nQualityHistogram97  , quality_histogram_97   )
	SET_HISTOGRAM( s.m_nQualityHistogram95  , quality_histogram_95   )
	SET_HISTOGRAM( s.m_nQualityHistogram90  , quality_histogram_90   )
	SET_HISTOGRAM( s.m_nQualityHistogram75  , quality_histogram_75   )
	SET_HISTOGRAM( s.m_nQualityHistogram50  , quality_histogram_50   )
	SET_HISTOGRAM( s.m_nQualityHistogram1   , quality_histogram_1    )
	SET_HISTOGRAM( s.m_nQualityHistogramDead, quality_histogram_dead )

	SET_NTILE( s.m_nQualityNtile50th, quality_ntile_50th )
	SET_NTILE( s.m_nQualityNtile25th, quality_ntile_25th )
	SET_NTILE( s.m_nQualityNtile5th , quality_ntile_5th  )
	SET_NTILE( s.m_nQualityNtile2nd , quality_ntile_2nd  )

	SET_HISTOGRAM( s.m_nPingHistogram25 , ping_histogram_25  )
	SET_HISTOGRAM( s.m_nPingHistogram50 , ping_histogram_50  )
	SET_HISTOGRAM( s.m_nPingHistogram75 , ping_histogram_75  )
	SET_HISTOGRAM( s.m_nPingHistogram100, ping_histogram_100 )
	SET_HISTOGRAM( s.m_nPingHistogram125, ping_histogram_125 )
	SET_HISTOGRAM( s.m_nPingHistogram150, ping_histogram_150 )
	SET_HISTOGRAM( s.m_nPingHistogram200, ping_histogram_200 )
	SET_HISTOGRAM( s.m_nPingHistogram300, ping_histogram_300 )
	SET_HISTOGRAM( s.m_nPingHistogramMax, ping_histogram_max )

	SET_NTILE( s.m_nPingNtile5th , ping_ntile_5th  )
	SET_NTILE( s.m_nPingNtile50th, ping_ntile_50th )
	SET_NTILE( s.m_nPingNtile75th, ping_ntile_75th )
	SET_NTILE( s.m_nPingNtile95th, ping_ntile_95th )
	SET_NTILE( s.m_nPingNtile98th, ping_ntile_98th )

	SET_HISTOGRAM( s.m_nJitterHistogramNegligible, jitter_histogram_negligible )
	SET_HISTOGRAM( s.m_nJitterHistogram1,  jitter_histogram_1  )
	SET_HISTOGRAM( s.m_nJitterHistogram2,  jitter_histogram_2  )
	SET_HISTOGRAM( s.m_nJitterHistogram5,  jitter_histogram_5  )
	SET_HISTOGRAM( s.m_nJitterHistogram10, jitter_histogram_10 )
	SET_HISTOGRAM( s.m_nJitterHistogram20, jitter_histogram_20 )

	if ( s.m_nTXSpeedMax > 0 )
		msg.set_txspeed_max( s.m_nTXSpeedMax );

	SET_HISTOGRAM( s.m_nTXSpeedHistogram16,   txspeed_histogram_16   )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram32,   txspeed_histogram_32   )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram64,   txspeed_histogram_64   )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram128,  txspeed_histogram_128  )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram256,  txspeed_histogram_256  )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram512,  txspeed_histogram_512  )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram1024, txspeed_histogram_1024 )
	SET_HISTOGRAM( s.m_nTXSpeedHistogramMax,  txspeed_histogram_max  )

	SET_NTILE( s.m_nTXSpeedNtile5th,  txspeed_ntile_5th  )
	SET_NTILE( s.m_nTXSpeedNtile50th, txspeed_ntile_50th )
	SET_NTILE( s.m_nTXSpeedNtile75th, txspeed_ntile_75th )
	SET_NTILE( s.m_nTXSpeedNtile95th, txspeed_ntile_95th )
	SET_NTILE( s.m_nTXSpeedNtile98th, txspeed_ntile_98th )

	if ( s.m_nRXSpeedMax > 0 )
		msg.set_rxspeed_max( s.m_nRXSpeedMax );

	SET_HISTOGRAM( s.m_nRXSpeedHistogram16,   rxspeed_histogram_16   )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram32,   rxspeed_histogram_32   )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram64,   rxspeed_histogram_64   )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram128,  rxspeed_histogram_128  )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram256,  rxspeed_histogram_256  )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram512,  rxspeed_histogram_512  )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram1024, rxspeed_histogram_1024 )
	SET_HISTOGRAM( s.m_nRXSpeedHistogramMax,  rxspeed_histogram_max  )

	SET_NTILE( s.m_nRXSpeedNtile5th,  rxspeed_ntile_5th  )
	SET_NTILE( s.m_nRXSpeedNtile50th, rxspeed_ntile_50th )
	SET_NTILE( s.m_nRXSpeedNtile75th, rxspeed_ntile_75th )
	SET_NTILE( s.m_nRXSpeedNtile95th, rxspeed_ntile_95th )
	SET_NTILE( s.m_nRXSpeedNtile98th, rxspeed_ntile_98th )

	#undef SET_HISTOGRAM
	#undef SET_NTILE
}

void LinkStatsLifetimeMsgToStruct( const CMsgSteamDatagramLinkLifetimeStats &msg, SteamDatagramLinkLifetimeStats &s )
{
	s.m_nPacketsSent = msg.packets_sent();
	s.m_nBytesSent = msg.kb_sent() * 1024;
	s.m_nPacketsRecv = msg.packets_recv();
	s.m_nBytesRecv = msg.kb_recv() * 1024;
	s.m_nPktsRecvSequenced = msg.packets_recv_sequenced();
	s.m_nPktsRecvDropped = msg.packets_recv_dropped();
	s.m_nPktsRecvOutOfOrder = msg.packets_recv_out_of_order();
	s.m_nPktsRecvDuplicate = msg.packets_recv_duplicate();
	s.m_nPktsRecvSequenceNumberLurch = msg.packets_recv_lurch();

	#define SET_HISTOGRAM( mbr, field ) mbr = msg.field();
	#define SET_NTILE( mbr, field ) mbr = ( msg.has_ ## field() ? msg.field() : -1 );

	SET_HISTOGRAM( s.m_nQualityHistogram100 , quality_histogram_100  )
	SET_HISTOGRAM( s.m_nQualityHistogram99  , quality_histogram_99   )
	SET_HISTOGRAM( s.m_nQualityHistogram97  , quality_histogram_97   )
	SET_HISTOGRAM( s.m_nQualityHistogram95  , quality_histogram_95   )
	SET_HISTOGRAM( s.m_nQualityHistogram90  , quality_histogram_90   )
	SET_HISTOGRAM( s.m_nQualityHistogram75  , quality_histogram_75   )
	SET_HISTOGRAM( s.m_nQualityHistogram50  , quality_histogram_50   )
	SET_HISTOGRAM( s.m_nQualityHistogram1   , quality_histogram_1    )
	SET_HISTOGRAM( s.m_nQualityHistogramDead, quality_histogram_dead )

	SET_NTILE( s.m_nQualityNtile50th, quality_ntile_50th )
	SET_NTILE( s.m_nQualityNtile25th, quality_ntile_25th )
	SET_NTILE( s.m_nQualityNtile5th , quality_ntile_5th  )
	SET_NTILE( s.m_nQualityNtile2nd , quality_ntile_2nd  )

	SET_HISTOGRAM( s.m_nPingHistogram25 , ping_histogram_25  )
	SET_HISTOGRAM( s.m_nPingHistogram50 , ping_histogram_50  )
	SET_HISTOGRAM( s.m_nPingHistogram75 , ping_histogram_75  )
	SET_HISTOGRAM( s.m_nPingHistogram100, ping_histogram_100 )
	SET_HISTOGRAM( s.m_nPingHistogram125, ping_histogram_125 )
	SET_HISTOGRAM( s.m_nPingHistogram150, ping_histogram_150 )
	SET_HISTOGRAM( s.m_nPingHistogram200, ping_histogram_200 )
	SET_HISTOGRAM( s.m_nPingHistogram300, ping_histogram_300 )
	SET_HISTOGRAM( s.m_nPingHistogramMax, ping_histogram_max )

	SET_NTILE( s.m_nPingNtile5th , ping_ntile_5th  )
	SET_NTILE( s.m_nPingNtile50th, ping_ntile_50th )
	SET_NTILE( s.m_nPingNtile75th, ping_ntile_75th )
	SET_NTILE( s.m_nPingNtile95th, ping_ntile_95th )
	SET_NTILE( s.m_nPingNtile98th, ping_ntile_98th )

	SET_HISTOGRAM( s.m_nJitterHistogramNegligible, jitter_histogram_negligible )
	SET_HISTOGRAM( s.m_nJitterHistogram1,  jitter_histogram_1  )
	SET_HISTOGRAM( s.m_nJitterHistogram2,  jitter_histogram_2  )
	SET_HISTOGRAM( s.m_nJitterHistogram5,  jitter_histogram_5  )
	SET_HISTOGRAM( s.m_nJitterHistogram10, jitter_histogram_10 )
	SET_HISTOGRAM( s.m_nJitterHistogram20, jitter_histogram_20 )

	s.m_nTXSpeedMax = msg.txspeed_max();

	SET_HISTOGRAM( s.m_nTXSpeedHistogram16,   txspeed_histogram_16   )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram32,   txspeed_histogram_32   )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram64,   txspeed_histogram_64   )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram128,  txspeed_histogram_128  )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram256,  txspeed_histogram_256  )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram512,  txspeed_histogram_512  )
	SET_HISTOGRAM( s.m_nTXSpeedHistogram1024, txspeed_histogram_1024 )
	SET_HISTOGRAM( s.m_nTXSpeedHistogramMax,  txspeed_histogram_max  )

	SET_NTILE( s.m_nTXSpeedNtile5th,  txspeed_ntile_5th  )
	SET_NTILE( s.m_nTXSpeedNtile50th, txspeed_ntile_50th )
	SET_NTILE( s.m_nTXSpeedNtile75th, txspeed_ntile_75th )
	SET_NTILE( s.m_nTXSpeedNtile95th, txspeed_ntile_95th )
	SET_NTILE( s.m_nTXSpeedNtile98th, txspeed_ntile_98th )

	s.m_nRXSpeedMax = msg.rxspeed_max();

	SET_HISTOGRAM( s.m_nRXSpeedHistogram16,   rxspeed_histogram_16   )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram32,   rxspeed_histogram_32   )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram64,   rxspeed_histogram_64   )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram128,  rxspeed_histogram_128  )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram256,  rxspeed_histogram_256  )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram512,  rxspeed_histogram_512  )
	SET_HISTOGRAM( s.m_nRXSpeedHistogram1024, rxspeed_histogram_1024 )
	SET_HISTOGRAM( s.m_nRXSpeedHistogramMax,  rxspeed_histogram_max  )

	SET_NTILE( s.m_nRXSpeedNtile5th,  rxspeed_ntile_5th  )
	SET_NTILE( s.m_nRXSpeedNtile50th, rxspeed_ntile_50th )
	SET_NTILE( s.m_nRXSpeedNtile75th, rxspeed_ntile_75th )
	SET_NTILE( s.m_nRXSpeedNtile95th, rxspeed_ntile_95th )
	SET_NTILE( s.m_nRXSpeedNtile98th, rxspeed_ntile_98th )

	#undef SET_HISTOGRAM
	#undef SET_NTILE
}

static void PrintPct( char (&szBuf)[32], float flPct )
{
	flPct *= 100.0f;
	if ( flPct < 0.0f )
		V_strcpy_safe( szBuf, "???" );
	else if ( flPct < 9.5f )
		V_sprintf_safe( szBuf, "%.2f", flPct );
	else if ( flPct < 99.5f )
		V_sprintf_safe( szBuf, "%.1f", flPct );
	else
		V_sprintf_safe( szBuf, "%.0f", flPct );
}

void LinkStatsPrintInstantaneousToBuf( const char *pszLeader, const SteamDatagramLinkInstantaneousStats &stats, CUtlBuffer &buf )
{
	buf.Printf( "%sSent:%6.1f pkts/sec%6.1f K/sec\n", pszLeader, stats.m_flOutPacketsPerSec, stats.m_flOutBytesPerSec/1024.0f );
	buf.Printf( "%sRecv:%6.1f pkts/sec%6.1f K/sec\n", pszLeader, stats.m_flInPacketsPerSec, stats.m_flInBytesPerSec/1024.0f );

	if ( stats.m_nPingMS >= 0 || stats.m_usecMaxJitter >= 0 )
	{
		char szPing[ 32 ];
		if ( stats.m_nPingMS < 0 )
			V_strcpy_safe( szPing, "???" );
		else
			V_sprintf_safe( szPing, "%d", stats.m_nPingMS );

		char szPeakJitter[ 32 ];
		if ( stats.m_usecMaxJitter < 0 )
			V_strcpy_safe( szPeakJitter, "???" );
		else
			V_sprintf_safe( szPeakJitter, "%.1f", stats.m_usecMaxJitter*1e-3f );

		buf.Printf( "%sPing:%sms    Max latency variance: %sms\n", pszLeader, szPing, szPeakJitter );
	}

	if ( stats.m_flPacketsDroppedPct >= 0.0f && stats.m_flPacketsWeirdSequenceNumberPct >= 0.0f )
	{
		char szDropped[ 32 ];
		PrintPct( szDropped, stats.m_flPacketsDroppedPct );

		char szWeirdSeq[ 32 ];
		PrintPct( szWeirdSeq, stats.m_flPacketsWeirdSequenceNumberPct );

		char szQuality[32];
		PrintPct( szQuality, 1.0f - stats.m_flPacketsDroppedPct - stats.m_flPacketsWeirdSequenceNumberPct );
		buf.Printf( "%sQuality:%5s%%  (Dropped:%4s%%  WeirdSeq:%4s%%)\n", pszLeader, szQuality, szDropped, szWeirdSeq);
	}

	if ( stats.m_nSendRate > 0 )
		buf.Printf( "%sEst avail bandwidth: %.1fKB/s  \n", pszLeader, stats.m_nSendRate/1024.0f );
	if ( stats.m_nPendingBytes >= 0 )
		buf.Printf( "%sBytes buffered: %s\n", pszLeader, NumberPrettyPrinter( stats.m_nPendingBytes ).String() );
}

void LinkStatsPrintLifetimeToBuf( const char *pszLeader, const SteamDatagramLinkLifetimeStats &stats, CUtlBuffer &buf )
{
	buf.Printf( "%sTotals\n", pszLeader );
	buf.Printf( "%s    Sent:%11s pkts %15s bytes\n", pszLeader, NumberPrettyPrinter( stats.m_nPacketsSent ).String(), NumberPrettyPrinter( stats.m_nBytesSent ).String() );
	buf.Printf( "%s    Recv:%11s pkts %15s bytes\n", pszLeader, NumberPrettyPrinter( stats.m_nPacketsRecv ).String(), NumberPrettyPrinter( stats.m_nBytesRecv ).String() );
	if ( stats.m_nPktsRecvSequenced > 0 )
	{
		buf.Printf( "%s    Recv w seq:%11s pkts\n", pszLeader, NumberPrettyPrinter( stats.m_nPktsRecvSequenced ).String() );
		float flToPct = 100.0f / ( stats.m_nPktsRecvSequenced + stats.m_nPktsRecvDropped );
		buf.Printf( "%s    Dropped   :%11s pkts%7.2f%%\n", pszLeader, NumberPrettyPrinter( stats.m_nPktsRecvDropped ).String(), stats.m_nPktsRecvDropped * flToPct );
		buf.Printf( "%s    OutOfOrder:%11s pkts%7.2f%%\n", pszLeader, NumberPrettyPrinter( stats.m_nPktsRecvOutOfOrder ).String(), stats.m_nPktsRecvOutOfOrder * flToPct );
		buf.Printf( "%s    Duplicate :%11s pkts%7.2f%%\n", pszLeader, NumberPrettyPrinter( stats.m_nPktsRecvDuplicate ).String(), stats.m_nPktsRecvDuplicate * flToPct );
		buf.Printf( "%s    SeqLurch  :%11s pkts%7.2f%%\n", pszLeader, NumberPrettyPrinter( stats.m_nPktsRecvSequenceNumberLurch ).String(), stats.m_nPktsRecvSequenceNumberLurch * flToPct );
	}

	// Do we have enough ping samples such that the distribution might be interesting
	{
		int nPingSamples = stats.PingHistogramTotalCount();
		if ( nPingSamples >= 5 )
		{
			float flToPct = 100.0f / nPingSamples;
			buf.Printf( "%sPing histogram: (%d total samples)\n", pszLeader, nPingSamples );
			buf.Printf( "%s      0-25  :%5d  %3.0f%%\n", pszLeader, stats.m_nPingHistogram25 , stats.m_nPingHistogram25 *flToPct );
			buf.Printf( "%s     25-50  :%5d  %3.0f%%\n", pszLeader, stats.m_nPingHistogram50 , stats.m_nPingHistogram50 *flToPct );
			buf.Printf( "%s     50-75  :%5d  %3.0f%%\n", pszLeader, stats.m_nPingHistogram75 , stats.m_nPingHistogram75 *flToPct );
			buf.Printf( "%s     75-100 :%5d  %3.0f%%\n", pszLeader, stats.m_nPingHistogram100, stats.m_nPingHistogram100*flToPct );
			buf.Printf( "%s    100-125 :%5d  %3.0f%%\n", pszLeader, stats.m_nPingHistogram125, stats.m_nPingHistogram125*flToPct );
			buf.Printf( "%s    125-150 :%5d  %3.0f%%\n", pszLeader, stats.m_nPingHistogram150, stats.m_nPingHistogram150*flToPct );
			buf.Printf( "%s    150-200 :%5d  %3.0f%%\n", pszLeader, stats.m_nPingHistogram200, stats.m_nPingHistogram200*flToPct );
			buf.Printf( "%s    200-300 :%5d  %3.0f%%\n", pszLeader, stats.m_nPingHistogram300, stats.m_nPingHistogram300*flToPct );
			buf.Printf( "%s      300+  :%5d  %3.0f%%\n", pszLeader, stats.m_nPingHistogramMax, stats.m_nPingHistogramMax*flToPct );
			buf.Printf( "%sPing distribution:\n", pszLeader );
			if ( stats.m_nPingNtile5th  >= 0 ) buf.Printf( "%s     5%% of pings <= %4dms\n", pszLeader, stats.m_nPingNtile5th  );
			if ( stats.m_nPingNtile50th >= 0 ) buf.Printf( "%s    50%% of pings <= %4dms\n", pszLeader, stats.m_nPingNtile50th );
			if ( stats.m_nPingNtile75th >= 0 ) buf.Printf( "%s    75%% of pings <= %4dms\n", pszLeader, stats.m_nPingNtile75th );
			if ( stats.m_nPingNtile95th >= 0 ) buf.Printf( "%s    95%% of pings <= %4dms\n", pszLeader, stats.m_nPingNtile95th );
			if ( stats.m_nPingNtile98th >= 0 ) buf.Printf( "%s    98%% of pings <= %4dms\n", pszLeader, stats.m_nPingNtile98th );
		}
		else
		{
			buf.Printf( "%sNo ping distribution available.  (%d samples)\n", pszLeader, nPingSamples );
		}
	}

	// Do we have enough quality samples such that the distribution might be interesting?
	{
		int nQualitySamples = stats.QualityHistogramTotalCount();
		if ( nQualitySamples >= 5 )
		{
			float flToPct = 100.0f / nQualitySamples;

			buf.Printf( "%sConnection quality histogram: (%d measurement intervals)\n", pszLeader, nQualitySamples );
			buf.Printf( "%s     100  :%5d  %3.0f%%   (All packets received in order)\n", pszLeader, stats.m_nQualityHistogram100, stats.m_nQualityHistogram100*flToPct );
			buf.Printf( "%s     99+  :%5d  %3.0f%%\n", pszLeader, stats.m_nQualityHistogram99, stats.m_nQualityHistogram99*flToPct );
			buf.Printf( "%s    97-99 :%5d  %3.0f%%\n", pszLeader, stats.m_nQualityHistogram97, stats.m_nQualityHistogram97*flToPct );
			buf.Printf( "%s    95-97 :%5d  %3.0f%%\n", pszLeader, stats.m_nQualityHistogram95, stats.m_nQualityHistogram95*flToPct );
			buf.Printf( "%s    90-95 :%5d  %3.0f%%\n", pszLeader, stats.m_nQualityHistogram90, stats.m_nQualityHistogram90*flToPct );
			buf.Printf( "%s    75-90 :%5d  %3.0f%%\n", pszLeader, stats.m_nQualityHistogram75, stats.m_nQualityHistogram75*flToPct );
			buf.Printf( "%s    50-75 :%5d  %3.0f%%\n", pszLeader, stats.m_nQualityHistogram50, stats.m_nQualityHistogram50*flToPct );
			buf.Printf( "%s     <50  :%5d  %3.0f%%\n", pszLeader, stats.m_nQualityHistogram1, stats.m_nQualityHistogram1*flToPct );
			buf.Printf( "%s    dead  :%5d  %3.0f%%   (Expected to receive something but didn't)\n", pszLeader, stats.m_nQualityHistogramDead, stats.m_nQualityHistogramDead*flToPct );
			buf.Printf( "%sConnection quality distribution:\n", pszLeader );
			if ( stats.m_nQualityNtile50th >= 0 ) buf.Printf( "%s    50%% of intervals >= %3d%%\n", pszLeader, stats.m_nQualityNtile50th );
			if ( stats.m_nQualityNtile25th >= 0 ) buf.Printf( "%s    75%% of intervals >= %3d%%\n", pszLeader, stats.m_nQualityNtile25th );
			if ( stats.m_nQualityNtile5th  >= 0 ) buf.Printf( "%s    95%% of intervals >= %3d%%\n", pszLeader, stats.m_nQualityNtile5th  );
			if ( stats.m_nQualityNtile2nd  >= 0 ) buf.Printf( "%s    98%% of intervals >= %3d%%\n", pszLeader, stats.m_nQualityNtile2nd  );
		}
		else
		{
			buf.Printf( "%sNo connection quality distribution available.  (%d measurement intervals)\n", pszLeader, nQualitySamples );
		}
	}

	// Do we have any jitter samples?
	{
		int nJitterSamples = stats.JitterHistogramTotalCount();
		if ( nJitterSamples >= 1 )
		{
			float flToPct = 100.0f / nJitterSamples;

			buf.Printf( "%sLatency variance histogram: (%d total measurements)\n", pszLeader, nJitterSamples );
			buf.Printf( "%s     <1  :%7d  %3.0f%%\n", pszLeader, stats.m_nJitterHistogramNegligible, stats.m_nJitterHistogramNegligible*flToPct );
			buf.Printf( "%s    1-2  :%7d  %3.0f%%\n", pszLeader, stats.m_nJitterHistogram1 , stats.m_nJitterHistogram1 *flToPct );
			buf.Printf( "%s    2-5  :%7d  %3.0f%%\n", pszLeader, stats.m_nJitterHistogram2 , stats.m_nJitterHistogram2 *flToPct );
			buf.Printf( "%s    5-10 :%7d  %3.0f%%\n", pszLeader, stats.m_nJitterHistogram5 , stats.m_nJitterHistogram5 *flToPct );
			buf.Printf( "%s   10-20 :%7d  %3.0f%%\n", pszLeader, stats.m_nJitterHistogram10, stats.m_nJitterHistogram10*flToPct );
			buf.Printf( "%s    >20  :%7d  %3.0f%%\n", pszLeader, stats.m_nJitterHistogram20, stats.m_nJitterHistogram20*flToPct );
		}
		else
		{
			buf.Printf( "%sLatency variance histogram not available\n", pszLeader );
		}
	}

	// Do we have enough tx speed samples such that the distribution might be interesting?
	{
		int nTXSpeedSamples = stats.TXSpeedHistogramTotalCount();
		if ( nTXSpeedSamples >= 5 )
		{
			float flToPct = 100.0f / nTXSpeedSamples;
			buf.Printf( "%sTX Speed histogram: (%d total samples)\n", pszLeader, nTXSpeedSamples );
			buf.Printf( "%s     0 - 16 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram16,   stats.m_nTXSpeedHistogram16  *flToPct );
			buf.Printf( "%s    16 - 32 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram32,   stats.m_nTXSpeedHistogram32  *flToPct );
			buf.Printf( "%s    32 - 64 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram64,   stats.m_nTXSpeedHistogram64  *flToPct );
			buf.Printf( "%s   64 - 128 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram128,  stats.m_nTXSpeedHistogram128 *flToPct );
			buf.Printf( "%s  128 - 256 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram256,  stats.m_nTXSpeedHistogram256 *flToPct );
			buf.Printf( "%s  256 - 512 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram512,  stats.m_nTXSpeedHistogram512 *flToPct );
			buf.Printf( "%s 512 - 1024 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram1024, stats.m_nTXSpeedHistogram1024*flToPct );
			buf.Printf( "%s      1024+ KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogramMax,  stats.m_nTXSpeedHistogramMax *flToPct );
			buf.Printf( "%sTransmit speed distribution:\n", pszLeader );
 			if ( stats.m_nTXSpeedNtile5th  >= 0 ) buf.Printf( "%s     5%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nTXSpeedNtile5th  );
			if ( stats.m_nTXSpeedNtile50th >= 0 ) buf.Printf( "%s    50%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nTXSpeedNtile50th );
			if ( stats.m_nTXSpeedNtile75th >= 0 ) buf.Printf( "%s    75%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nTXSpeedNtile75th );
			if ( stats.m_nTXSpeedNtile95th >= 0 ) buf.Printf( "%s    95%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nTXSpeedNtile95th );
			if ( stats.m_nTXSpeedNtile98th >= 0 ) buf.Printf( "%s    98%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nTXSpeedNtile98th );
		}
		else
		{
			buf.Printf( "%sNo connection transmit speed distribution available.  (%d measurement intervals)\n", pszLeader, nTXSpeedSamples );
		}
	}

	// Do we have enough RX speed samples such that the distribution might be interesting?
	{
		int nRXSpeedSamples = stats.RXSpeedHistogramTotalCount();
		if ( nRXSpeedSamples >= 5 )
		{
			float flToPct = 100.0f / nRXSpeedSamples;
			buf.Printf( "%sRX Speed histogram: (%d total samples)\n", pszLeader, nRXSpeedSamples );
			buf.Printf( "%s     0 - 16 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram16,   stats.m_nRXSpeedHistogram16  *flToPct );
			buf.Printf( "%s    16 - 32 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram32,   stats.m_nRXSpeedHistogram32  *flToPct );
			buf.Printf( "%s    32 - 64 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram64,   stats.m_nRXSpeedHistogram64  *flToPct );
			buf.Printf( "%s   64 - 128 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram128,  stats.m_nRXSpeedHistogram128 *flToPct );
			buf.Printf( "%s  128 - 256 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram256,  stats.m_nRXSpeedHistogram256 *flToPct );
			buf.Printf( "%s  256 - 512 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram512,  stats.m_nRXSpeedHistogram512 *flToPct );
			buf.Printf( "%s 512 - 1024 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram1024, stats.m_nRXSpeedHistogram1024*flToPct );
			buf.Printf( "%s      1024+ KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogramMax,  stats.m_nRXSpeedHistogramMax *flToPct );
			buf.Printf( "%sReceive speed distribution:\n", pszLeader );
			if ( stats.m_nRXSpeedNtile5th  >= 0 ) buf.Printf( "%s     5%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nRXSpeedNtile5th  );
			if ( stats.m_nRXSpeedNtile50th >= 0 ) buf.Printf( "%s    50%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nRXSpeedNtile50th );
			if ( stats.m_nRXSpeedNtile75th >= 0 ) buf.Printf( "%s    75%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nRXSpeedNtile75th );
			if ( stats.m_nRXSpeedNtile95th >= 0 ) buf.Printf( "%s    95%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nRXSpeedNtile95th );
			if ( stats.m_nRXSpeedNtile98th >= 0 ) buf.Printf( "%s    98%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nRXSpeedNtile98th );
		}
		else
		{
			buf.Printf( "%sNo connection recieve speed distribution available.  (%d measurement intervals)\n", pszLeader, nRXSpeedSamples );
		}
	}

}

void LinkStatsPrintToBuf( const char *pszLeader, const SteamDatagramLinkStats &stats, CUtlBuffer &buf )
{
	std::string sIndent( pszLeader ); sIndent.append( "    " );

	buf.Printf( "%sCurrent rates:\n", pszLeader );
	LinkStatsPrintInstantaneousToBuf( sIndent.c_str(), stats.m_latest, buf );
	buf.Printf( "%sLifetime stats:\n", pszLeader );
	LinkStatsPrintLifetimeToBuf( sIndent.c_str(), stats.m_lifetime, buf );

	if ( stats.m_flAgeLatestRemote < 0.0f )
	{
		buf.Printf( "%sNo rate stats received from remote host\n", pszLeader );
	}
	else
	{
		buf.Printf( "%sRate stats received from remote host %.1fs ago:\n", pszLeader, stats.m_flAgeLatestRemote );
		LinkStatsPrintInstantaneousToBuf( sIndent.c_str(), stats.m_latestRemote, buf );
	}

	if ( stats.m_flAgeLifetimeRemote < 0.0f )
	{
		buf.Printf( "%sNo lifetime stats received from remote host\n", pszLeader );
	}
	else
	{
		buf.Printf( "%sLifetime stats received from remote host %.1fs ago:\n", pszLeader, stats.m_flAgeLifetimeRemote );
		LinkStatsPrintLifetimeToBuf( sIndent.c_str(), stats.m_lifetimeRemote, buf );
	}
}

///////////////////////////////////////////////////////////////////////////////
//
// SipHash, used for challenge generation
//
// http://en.wikipedia.org/wiki/SipHash
//
// Code was copied from here:
// https://github.com/veorq/SipHash/blob/1b85a33b71f0fdd49942037a503b6798d67ef765/siphash24.c
//
///////////////////////////////////////////////////////////////////////////////

/* default: SipHash-2-4 */
#define cROUNDS 2
#define dROUNDS 4

#define ROTL(x,b) (uint64_t)( ((x) << (b)) | ( (x) >> (64 - (b))) )
#define U8TO64_LE(p) LittleQWord( *(const uint64*)(p) );

#define SIPROUND                                        \
  do {                                                  \
    v0 += v1; v1=ROTL(v1,13); v1 ^= v0; v0=ROTL(v0,32); \
    v2 += v3; v3=ROTL(v3,16); v3 ^= v2;                 \
    v0 += v3; v3=ROTL(v3,21); v3 ^= v0;                 \
    v2 += v1; v1=ROTL(v1,17); v1 ^= v2; v2=ROTL(v2,32); \
  } while(0)

uint64_t siphash( const uint8_t *in, uint64_t inlen, const uint8_t *k )
{
  /* "somepseudorandomlygeneratedbytes" */
  uint64_t v0 = 0x736f6d6570736575ULL;
  uint64_t v1 = 0x646f72616e646f6dULL;
  uint64_t v2 = 0x6c7967656e657261ULL;
  uint64_t v3 = 0x7465646279746573ULL;
  uint64_t b;
  uint64_t k0 = U8TO64_LE( k );
  uint64_t k1 = U8TO64_LE( k + 8 );
  uint64_t m;
  int i;
  const uint8_t *end = in + inlen - ( inlen % sizeof( uint64_t ) );
  const int left = inlen & 7;
  b = ( ( uint64_t )inlen ) << 56;
  v3 ^= k1;
  v2 ^= k0;
  v1 ^= k1;
  v0 ^= k0;

  for ( ; in != end; in += 8 )
  {
    m = U8TO64_LE( in );
    v3 ^= m;

    //TRACE;
    for( i=0; i<cROUNDS; ++i ) SIPROUND;

    v0 ^= m;
  }

  switch( left )
  {
  case 7: b |= ( ( uint64_t )in[ 6] )  << 48;
  case 6: b |= ( ( uint64_t )in[ 5] )  << 40;
  case 5: b |= ( ( uint64_t )in[ 4] )  << 32;
  case 4: b |= ( ( uint64_t )in[ 3] )  << 24;
  case 3: b |= ( ( uint64_t )in[ 2] )  << 16;
  case 2: b |= ( ( uint64_t )in[ 1] )  <<  8;
  case 1: b |= ( ( uint64_t )in[ 0] ); break;
  case 0: break;
  }


  v3 ^= b;

  //TRACE;
  for( i=0; i<cROUNDS; ++i ) SIPROUND;

  v0 ^= b;

  v2 ^= 0xff;

  //TRACE;
  for( i=0; i<dROUNDS; ++i ) SIPROUND;

  b = v0 ^ v1 ^ v2  ^ v3;

  return b;
}

std::string Indent( const char *s )
{
	if ( s == nullptr || *s == '\0' )
		return std::string();

	// Make one pass through, and count up how long the result will be
	int l = 2; // initial tab, plus terminating '\0';
	for ( const char *p = s ; *p ; ++p )
	{
		++l;
		if ( *p == '\n' || *p == '\r' )
		{
			if ( p[1] != '\n' && p[1] != '\r' && p[1] != '\0' )
			{
				++l;
			}
		}
	}

	std::string result;
	result.reserve( l );
	result += '\t';
	for ( const char *p = s ; *p ; ++p )
	{
		result += *p;
		if ( *p == '\n' || *p == '\r' )
		{
			if ( p[1] != '\n' && p[1] != '\r' && p[1] != '\0' )
			{
				result += '\t';
			}
		}
	}

	return result;
}

} // namespace SteamNetworkingSocketsLib

const char *GetAvailabilityString( ESteamDatagramAvailability a )
{
	switch ( a )
	{
		case k_ESteamDatagramAvailability_CannotTry: return "Dependency unavailable";
		case k_ESteamDatagramAvailability_Failed: return "Failed";
		case k_ESteamDatagramAvailability_Previously: return "Lost";
		case k_ESteamDatagramAvailability_NeverTried: return "Unknown";
		case k_ESteamDatagramAvailability_Attempting: return "Working...";
		case k_ESteamDatagramAvailability_Current: return "OK";
	}

	Assert( false );
	return "???";
}

void SteamNetworkingDetailedConnectionStatus::Clear()
{
	V_memset( this, 0, sizeof(*this) );
	COMPILE_TIME_ASSERT( k_ESteamDatagramAvailability_Unknown == 0 );
	m_statsEndToEnd.Clear();
	m_statsPrimaryRouter.Clear();
	m_nPrimaryRouterBackPing = -1;
	m_nBackupRouterFrontPing = -1;
	m_nBackupRouterBackPing = -1;
}

int SteamNetworkingDetailedConnectionStatus::Print( char *pszBuf, int cbBuf )
{
	CUtlBuffer buf( 0, 8*1024, CUtlBuffer::TEXT_BUFFER );

	// If we don't have network, there's nothing else we can really do
	if ( m_eAvailNetworkConfig != k_ESteamDatagramAvailability_Current && m_eAvailNetworkConfig != k_ESteamDatagramAvailability_Unknown )
	{
		buf.Printf( "Network configuration: %s\n", GetAvailabilityString( m_eAvailNetworkConfig ) );
		buf.Printf( "   Cannot communicate with relays without network config." );
	}

	// Unable to talk to any routers?
	if ( m_eAvailAnyRouterCommunication != k_ESteamDatagramAvailability_Current && m_eAvailAnyRouterCommunication != k_ESteamDatagramAvailability_Unknown )
	{
		buf.Printf( "Router network: %s\n", GetAvailabilityString( m_eAvailAnyRouterCommunication ) );
	}

	switch ( m_info.m_eState )
	{
		case k_ESteamNetworkingConnectionState_Connecting:
			buf.Printf( "End-to-end connection: connecting\n" );
			break;

		case k_ESteamNetworkingConnectionState_FindingRoute:
			buf.Printf( "End-to-end connection: performing rendezvous\n" );
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			buf.Printf( "End-to-end connection: connected\n" );
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			buf.Printf( "End-to-end connection: closed by remote host, reason code %d.  (%s)\n", m_info.m_eEndReason, m_info.m_szEndDebug );
			break;

		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			buf.Printf( "End-to-end connection: closed due to problem detected locally, reason code %d.  (%s)\n", m_info.m_eEndReason, m_info.m_szEndDebug );
			break;

		case k_ESteamNetworkingConnectionState_None:
			buf.Printf( "End-to-end connection: closed, reason code %d.  (%s)\n", m_info.m_eEndReason, m_info.m_szEndDebug );
			break;

		default:
			buf.Printf( "End-to-end connection: BUG: invalid state %d!\n", m_info.m_eState );
			break;
	}

	if ( m_info.m_idPOPRemote )
	{
		char szRemotePOP[ 8 ];
		GetSteamNetworkingLocationPOPStringFromID( m_info.m_idPOPRemote, szRemotePOP );
		buf.Printf( "    Remote host is in data center '%s'\n", szRemotePOP );
	}

	// If we ever tried to send a packet end-to-end, dump end-to-end stats.
	if ( m_statsEndToEnd.m_lifetime.m_nPacketsSent > 0 )
	{
		LinkStatsPrintToBuf( "    ", m_statsEndToEnd, buf );
	}

	if ( m_unPrimaryRouterIP )
	{
		buf.Printf( "Primary router: %s", m_szPrimaryRouterName );

		int nPrimaryFrontPing = m_statsPrimaryRouter.m_latest.m_nPingMS;
		if ( m_nPrimaryRouterBackPing >= 0 )
			buf.Printf( "  Ping = %d+%d=%d (front+back=total)\n", nPrimaryFrontPing, m_nPrimaryRouterBackPing,nPrimaryFrontPing+m_nPrimaryRouterBackPing );
		else
			buf.Printf( "  Ping to relay = %d\n", nPrimaryFrontPing );
		LinkStatsPrintToBuf( "    ", m_statsPrimaryRouter, buf );

		if ( m_unBackupRouterIP == 0 )
		{
			// Probably should only print this if we have reason to expect that a backup relay is expected
			//buf.Printf( "No backup router selected\n" );
		}
		else
		{
			buf.Printf( "Backup router: %s  Ping = %d+%d=%d (front+back=total)\n",
				m_szBackupRouterName,
				m_nBackupRouterFrontPing, m_nBackupRouterBackPing,m_nBackupRouterFrontPing+m_nBackupRouterBackPing
			);
		}
	}
	else if ( m_info.m_idPOPRelay )
	{
		char szRelayPOP[ 8 ];
		GetSteamNetworkingLocationPOPStringFromID( m_info.m_idPOPRelay, szRelayPOP );
		buf.Printf( "Communicating via relay in '%s'\n", szRelayPOP );
	}

	int sz = buf.TellPut()+1;
	if ( pszBuf && cbBuf > 0 )
	{
		int l = Min( sz, cbBuf ) - 1;
		V_memcpy( pszBuf, buf.Base(), l );
		pszBuf[l] = '\0';
		if ( cbBuf >= sz )
			return 0;
	}

	return sz;
}

