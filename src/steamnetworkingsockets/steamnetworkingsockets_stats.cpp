//====== Copyright Valve Corporation, All rights reserved. ====================

#include <atomic>
#include <tier1/utlbuffer.h>
#include "steamnetworking_statsutils.h"

// !KLUDGE! For SteamNetworkingSockets_GetLocalTimestamp
#ifdef IS_STEAMDATAGRAMROUTER
	#include "router/sdr.h"
#else
	#include "clientlib/steamnetworkingsockets_lowlevel.h"
#endif

// Must be the last include
#include <tier0/memdbgon.h>

using namespace SteamNetworkingSocketsLib;

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
	m_nConnectedSeconds = -1;
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
}

void PingTracker::ReceivedPing( int nPingMS, SteamNetworkingMicroseconds usecNow )
{
	Assert( nPingMS >= 0 );
	COMPILE_TIME_ASSERT( V_ARRAYSIZE(m_arPing) == 3 );

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
			// FALLTHROUGH
		case 2:
			// Just received our final sample to complete the sample
			m_nValidPings = 3;
			// FALLTHROUGH
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
}

int PingTracker::WorstPingInRecentSample() const
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

void LinkStatsTrackerBase::InitInternal( SteamNetworkingMicroseconds usecNow )
{
	m_nPeerProtocolVersion = 0;
	m_bPassive = false;
	m_sent.Reset();
	m_recv.Reset();
	m_recvExceedRateLimit.Reset();
	m_ping.Reset();
	m_nNextSendSequenceNumber = 1;
	m_usecTimeLastSentSeq = 0;
	InitMaxRecvPktNum( 0 );
	m_seqPktCounters.Reset();
	m_flInPacketsDroppedPct = -1.0f;
	m_flInPacketsWeirdSequencePct = -1.0f;
	m_usecMaxJitterPreviousInterval = -1;
	m_nPktsRecvSequenced = 0;
	m_nDebugPktsRecvInOrder = 0;
	m_nPktsRecvDroppedAccumulator = 0;
	m_nPktsRecvOutOfOrderAccumulator = 0;
	m_nPktsRecvDuplicateAccumulator = 0;
	m_nPktsRecvLurchAccumulator = 0;
	m_usecTimeLastRecv = 0;
	m_usecTimeLastRecvSeq = 0;
	memset( &m_latestRemote, 0, sizeof(m_latestRemote) );
	m_usecTimeRecvLatestRemote = 0;
	memset( &m_lifetimeRemote, 0, sizeof(m_lifetimeRemote) );
	m_usecTimeRecvLifetimeRemote = 0;
	//m_seqnumUnackedSentLifetime = -1;
	//m_seqnumPendingAckRecvTimelife = -1;
	m_qualityHistogram.Reset();
	m_qualitySample.Clear();
	m_jitterHistogram.Reset();
}

void LinkStatsTrackerBase::SetPassiveInternal( bool bFlag, SteamNetworkingMicroseconds usecNow )
{
	m_bPassive = bFlag;

	m_pktNumInFlight = 0;
	m_bInFlightInstantaneous = false;
	m_bInFlightLifetime = false;
	PeerAckedInstantaneous( usecNow );
	PeerAckedLifetime( usecNow );

	// Clear acks we expect, on either state change.
	m_usecInFlightReplyTimeout = 0;
	m_usecLastSendPacketExpectingImmediateReply = 0;
	m_nReplyTimeoutsSinceLastRecv = 0;
	m_usecWhenTimeoutStarted = 0;

	if ( !bFlag )
	{
		StartNextInterval( usecNow );
	}
}

void LinkStatsTrackerBase::StartNextInterval( SteamNetworkingMicroseconds usecNow )
{
	m_nPktsRecvDroppedAccumulator += m_seqPktCounters.m_nDropped;
	m_nPktsRecvOutOfOrderAccumulator += m_seqPktCounters.m_nOutOfOrder;
	m_nPktsRecvDuplicateAccumulator += m_seqPktCounters.m_nDuplicate;
	m_nPktsRecvLurchAccumulator += m_seqPktCounters.m_nLurch;
	m_seqPktCounters.Reset();
	m_usecIntervalStart = usecNow;
}

void LinkStatsTrackerBase::UpdateInterval( SteamNetworkingMicroseconds usecNow )
{
	float flElapsed = int64( usecNow - m_usecIntervalStart ) * 1e-6;
	flElapsed = Max( flElapsed, .001f ); // make sure math doesn't blow up

	// Check if enough happened in this interval to make a meaningful judgment about connection quality
	COMPILE_TIME_ASSERT( k_usecSteamDatagramLinkStatsDefaultInterval >= 5*k_nMillion );
	if ( flElapsed > 4.5f )
	{
		if ( m_seqPktCounters.m_nRecv > 5 )
		{
			int nWeird = m_seqPktCounters.Weird();
			int nBad = m_seqPktCounters.m_nDropped + nWeird;
			if ( nBad == 0 )
			{
				// Perfect connection.  This will hopefully be relatively common
				m_qualitySample.AddSample( 100 );
				++m_qualityHistogram.m_n100;
			}
			else
			{

				// Less than perfect.  Compute quality metric.
				int nTotalSent = m_seqPktCounters.m_nRecv + m_seqPktCounters.m_nDropped;
				int nRecvGood = m_seqPktCounters.m_nRecv - nWeird;
				int nQuality = nRecvGood * 100 / nTotalSent;

				// Cap at 99, since 100 is reserved to mean "perfect",
				// I don't think it's possible for the calculation above to ever produce 100, but whatever.
				if ( nQuality >= 99 )
				{
					m_qualitySample.AddSample( 99 );
					++m_qualityHistogram.m_n99;
				}
				else if ( nQuality <= 1 ) // in case accounting is hosed or every single packet was out of order, clamp.  0 means "totally dead connection"
				{
					m_qualitySample.AddSample( 1 );
					++m_qualityHistogram.m_n1;
				}
				else
				{
					m_qualitySample.AddSample( nQuality );
					if ( nQuality >= 97 )
						++m_qualityHistogram.m_n97;
					else if ( nQuality >= 95 )
						++m_qualityHistogram.m_n95;
					else if ( nQuality >= 90 )
						++m_qualityHistogram.m_n90;
					else if ( nQuality >= 75 )
						++m_qualityHistogram.m_n75;
					else if ( nQuality >= 50 )
						++m_qualityHistogram.m_n50;
					else
						++m_qualityHistogram.m_n1;
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
			++m_qualityHistogram.m_nDead;
		}
	}

	// PacketRate class does most of the work
	m_sent.UpdateInterval( flElapsed );
	m_recv.UpdateInterval( flElapsed );
	m_recvExceedRateLimit.UpdateInterval( flElapsed );

	int nWeirdSequenceCurrentInterval = m_seqPktCounters.Weird();
	Assert( nWeirdSequenceCurrentInterval <= m_seqPktCounters.m_nRecv );
	if ( m_seqPktCounters.m_nRecv <= 0 )
	{
		// No sequenced packets received during interval, so no data available
		m_flInPacketsDroppedPct = -1.0f;
		m_flInPacketsWeirdSequencePct = -1.0f;
	}
	else
	{
		float flToPct = 1.0f / float( m_seqPktCounters.m_nRecv + m_seqPktCounters.m_nDropped );
		m_flInPacketsDroppedPct = m_seqPktCounters.m_nDropped * flToPct;
		m_flInPacketsWeirdSequencePct = nWeirdSequenceCurrentInterval * flToPct;
	}

	// Peak jitter value
	m_usecMaxJitterPreviousInterval = m_seqPktCounters.m_usecMaxJitter;

	// Reset for next time
	StartNextInterval( usecNow );
}

void LinkStatsTrackerBase::InitMaxRecvPktNum( int64 nPktNum )
{
	Assert( nPktNum >= 0 );
	m_nMaxRecvPktNum = nPktNum;

	// Set bits, to mark that all values <= this packet number have been
	// received.
	m_recvPktNumberMask[0] = ~(uint64)0;
	unsigned nBitsToSet = (unsigned)( nPktNum & 63 ) + 1;
	if ( nBitsToSet == 64 )
		m_recvPktNumberMask[1] = ~(uint64)0;
	else
		m_recvPktNumberMask[1] = ( (uint64)1 << nBitsToSet ) - 1;

	m_nDebugLastInitMaxRecvPktNum = nPktNum;
}

std::string LinkStatsTrackerBase::RecvPktNumStateDebugString() const
{
	char buf[256];
	V_sprintf_safe( buf,
		"maxrecv=%lld, init=%lld, inorder=%lld, mask=%llx,%llx",
		(long long)m_nMaxRecvPktNum, (long long)m_nDebugLastInitMaxRecvPktNum, (long long)m_nDebugPktsRecvInOrder,
		(unsigned long long)m_recvPktNumberMask[0], (unsigned long long)m_recvPktNumberMask[1] );
	return std::string(buf);
}

std::string LinkStatsTrackerBase::HistoryRecvSeqNumDebugString( int nMaxPkts ) const
{
	constexpr int N = V_ARRAYSIZE( m_arDebugHistoryRecvSeqNum );
	COMPILE_TIME_ASSERT( ( N & (N-1) ) == 0 );
	nMaxPkts = std::min( nMaxPkts, N );

	std::string result;
	int64 idx = m_nPktsRecvSequenced;
	while ( --nMaxPkts >= 0 && --idx >= 0 )
	{
		char buf[32];
		V_sprintf_safe( buf, "%s%lld", result.empty() ? "" : ",", (long long)m_arDebugHistoryRecvSeqNum[ idx & (N-1) ] );
		result.append( buf );
	}

	return result;
}

void LinkStatsTrackerBase::InternalProcessSequencedPacket_OutOfOrder( int64 nPktNum )
{

	// We should have previously counted this packet as dropped.
	if ( PktsRecvDropped() == 0 )
	{
		// This is weird.
		// !TEST! Only assert if we can provide more detailed info to debug.
		// Also note that on the relay, old peers are using a single sequence
		// number stream, shred across multiple sessions, and we are not
		// tracking this properly, because we don't know which session we
		// marked the "drop" in.
		if ( m_nPktsRecvSequenced < 256 && m_nPeerProtocolVersion >= 9 )
		{
			AssertMsg( false,
				"No dropped packets, pkt num %lld, dup bit not set?  recvseq=%lld inorder=%lld, dup=%lld, lurch=%lld, ooo=%lld, %s.  (%s)",
				(long long)nPktNum, (long long)m_nPktsRecvSequenced,
				(long long)m_nDebugPktsRecvInOrder, (long long)PktsRecvDuplicate(),
				(long long)PktsRecvLurch(), (long long)PktsRecvOutOfOrder(),
				RecvPktNumStateDebugString().c_str(),
				Describe().c_str()
			);
	#ifdef IS_STEAMDATAGRAMROUTER
			int64 idx = m_nPktsRecvSequenced-1;
			while ( idx >= 0 )
			{
				CUtlBuffer buf( 0, 1024, CUtlBuffer::TEXT_BUFFER );
				switch ( idx )
				{
				default: buf.Printf( "%7lld", (long long)m_arDebugHistoryRecvSeqNum[ idx-- & 255 ] ); 
				case  6: buf.Printf( "%7lld", (long long)m_arDebugHistoryRecvSeqNum[ idx-- & 255 ] ); 
				case  5: buf.Printf( "%7lld", (long long)m_arDebugHistoryRecvSeqNum[ idx-- & 255 ] ); 
				case  4: buf.Printf( "%7lld", (long long)m_arDebugHistoryRecvSeqNum[ idx-- & 255 ] ); 
				case  3: buf.Printf( "%7lld", (long long)m_arDebugHistoryRecvSeqNum[ idx-- & 255 ] ); 
				case  2: buf.Printf( "%7lld", (long long)m_arDebugHistoryRecvSeqNum[ idx-- & 255 ] ); 
				case  1: buf.Printf( "%7lld", (long long)m_arDebugHistoryRecvSeqNum[ idx-- & 255 ] ); 
				case  0: buf.Printf( "%7lld", (long long)m_arDebugHistoryRecvSeqNum[ idx-- & 255 ] );
				}
				buf.PutChar( '\n' );
				g_pLogger->Write( buf.Base(), buf.TellPut() );
			}
	#endif
		}
	}

	m_seqPktCounters.OnOutOfOrder();
}

bool LinkStatsTrackerBase::BCheckHaveDataToSendInstantaneous( SteamNetworkingMicroseconds usecNow )
{
	Assert( !m_bPassive );

	// How many packets a second to we expect to send on an "active" connection?
	const int64 k_usecActiveConnectionSendInterval = 3*k_nMillion;
	COMPILE_TIME_ASSERT( k_usecSteamDatagramClientPingTimeout*2 < k_usecActiveConnectionSendInterval );
	COMPILE_TIME_ASSERT( k_usecSteamDatagramClientBackupRouterKeepaliveInterval > k_usecActiveConnectionSendInterval*5 ); // make sure backup keepalive interval isn't anywhere near close enough to trigger this.

	// Calculate threshold based on how much time has elapsed and a very low packet rate
	int64 usecElapsed = usecNow - m_usecPeerAckedInstaneous;
	Assert( usecElapsed >= k_usecLinkStatsInstantaneousReportInterval ); // don't call this unless you know it's been long enough!
	int nThreshold = usecElapsed / k_usecActiveConnectionSendInterval;

	// Has there been any traffic worth reporting on in this interval?
	if ( m_nPktsRecvSeqWhenPeerAckInstantaneous + nThreshold < m_nPktsRecvSequenced || m_nPktsSentWhenPeerAckInstantaneous + nThreshold < m_sent.m_packets.Total() )
		return true;

	// Connection has been idle since the last time we sent instantaneous stats.
	// Don't actually send stats, but clear counters and timers and act like we did.
	PeerAckedInstantaneous( usecNow );

	// And don't send anything
	return false;
}

bool LinkStatsTrackerBase::BCheckHaveDataToSendLifetime( SteamNetworkingMicroseconds usecNow )
{
	Assert( !m_bPassive );

	// Make sure we have something new to report since the last time we sent stats
	if ( m_nPktsRecvSeqWhenPeerAckLifetime + 100 < m_nPktsRecvSequenced || m_nPktsSentWhenPeerAckLifetime + 100 < m_sent.m_packets.Total() )
		return true;

	// Reset the timer.  But do NOT reset the packet counters.  So if the connection isn't
	// dropped, and we are sending keepalives very slowly, this will just send some stats
	// along about every 100 packets or so.  Typically we'll drop the session before that
	// happens.
	m_usecPeerAckedLifetime = usecNow;

	// Don't send anything now
	return false;
}

int LinkStatsTrackerBase::GetStatsSendNeed( SteamNetworkingMicroseconds usecNow )
{
	int nResult = 0;

	// Message already in flight?
	if ( m_pktNumInFlight == 0 && !m_bPassive )
	{
		if ( m_usecPeerAckedInstaneous + k_usecLinkStatsInstantaneousReportInterval < usecNow && BCheckHaveDataToSendInstantaneous( usecNow ) )
		{
			if ( m_usecPeerAckedInstaneous + k_usecLinkStatsInstantaneousReportMaxInterval < usecNow )
				nResult |= k_nSendStats_Instantanous_Due;
			else
				nResult |= k_nSendStats_Instantanous_Ready;
		}

		if ( m_usecPeerAckedLifetime + k_usecLinkStatsLifetimeReportInterval < usecNow && BCheckHaveDataToSendLifetime( usecNow ) )
		{
			if ( m_usecPeerAckedInstaneous + k_usecLinkStatsLifetimeReportMaxInterval < usecNow )
				nResult |= k_nSendStats_Lifetime_Due;
			else
				nResult |= k_nSendStats_Lifetime_Ready;
		}
	}

	return nResult;
}

const char *LinkStatsTrackerBase::InternalGetSendStatsReasonOrUpdateNextThinkTime( SteamNetworkingMicroseconds usecNow, const char *const arpszReasonStrings[4], SteamNetworkingMicroseconds &inOutNextThinkTime )
{
	if ( m_bPassive )
		return nullptr;
	if ( m_usecInFlightReplyTimeout > 0 && m_usecInFlightReplyTimeout < inOutNextThinkTime )
		inOutNextThinkTime = m_usecInFlightReplyTimeout;

	// Message already in flight?
	if ( m_pktNumInFlight )
		return nullptr;

	int n = 0;
	if ( m_usecPeerAckedInstaneous + k_usecLinkStatsInstantaneousReportMaxInterval < usecNow && BCheckHaveDataToSendInstantaneous( usecNow ) )
	{
		n |= 1;
	}
	else
	{
		SteamNetworkingMicroseconds usecNextCheck = m_usecPeerAckedInstaneous + k_usecLinkStatsInstantaneousReportMaxInterval;
		if ( usecNextCheck < inOutNextThinkTime )
			inOutNextThinkTime = usecNextCheck;
	}
	if ( m_usecPeerAckedLifetime + k_usecLinkStatsLifetimeReportMaxInterval < usecNow && BCheckHaveDataToSendLifetime( usecNow ) )
	{
		n |= 2;
	}
	else
	{
		SteamNetworkingMicroseconds usecNextCheck = m_usecPeerAckedLifetime + k_usecLinkStatsLifetimeReportMaxInterval;
		if ( usecNextCheck < inOutNextThinkTime )
			inOutNextThinkTime = usecNextCheck;
	}
	return arpszReasonStrings[n];
}

void LinkStatsTrackerBase::PopulateMessage( int nNeedFlags, CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow )
{
	Assert( m_pktNumInFlight == 0 && !m_bPassive );

	// Ready to send instantaneous stats?
	if ( nNeedFlags & k_nSendStats_Instantanous )
	{
		// !KLUDGE! Go through public struct as intermediary to keep code simple.
		SteamDatagramLinkInstantaneousStats sInstant;
		GetInstantaneousStats( sInstant );
		LinkStatsInstantaneousStructToMsg( sInstant, *msg.mutable_instantaneous() );
	}

	// Ready to send lifetime stats?
	if ( nNeedFlags & k_nSendStats_Lifetime )
	{
		PopulateLifetimeMessage( *msg.mutable_lifetime() );
	}
}

void LinkStatsTrackerBase::PopulateLifetimeMessage( CMsgSteamDatagramLinkLifetimeStats &msg )
{
	// !KLUDGE! Go through public struct as intermediary to keep code simple.
	SteamDatagramLinkLifetimeStats sLifetime;
	GetLifetimeStats( sLifetime );
	LinkStatsLifetimeStructToMsg( sLifetime, msg );
}

void LinkStatsTrackerBase::TrackSentMessageExpectingReply( SteamNetworkingMicroseconds usecNow, bool bAllowDelayedReply )
{
	if ( m_usecInFlightReplyTimeout == 0 )
	{
		m_usecInFlightReplyTimeout = usecNow + m_ping.CalcConservativeTimeout();
		if ( bAllowDelayedReply )
			m_usecInFlightReplyTimeout += k_usecSteamDatagramRouterPendClientPing;
	}
	if ( !bAllowDelayedReply )
		m_usecLastSendPacketExpectingImmediateReply = usecNow;
}

void LinkStatsTrackerBase::ProcessMessage( const CMsgSteamDatagramConnectionQuality &msg, SteamNetworkingMicroseconds usecNow )
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

void LinkStatsTrackerBase::GetInstantaneousStats( SteamDatagramLinkInstantaneousStats &s ) const
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

void LinkStatsTrackerBase::GetLifetimeStats( SteamDatagramLinkLifetimeStats &s ) const
{
	s.m_nPacketsSent = m_sent.m_packets.Total();
	s.m_nBytesSent = m_sent.m_bytes.Total();
	s.m_nPacketsRecv = m_recv.m_packets.Total();
	s.m_nBytesRecv = m_recv.m_bytes.Total();
	s.m_nPktsRecvSequenced = m_nPktsRecvSequenced;
	s.m_nPktsRecvDropped = PktsRecvDropped();
	s.m_nPktsRecvOutOfOrder = PktsRecvOutOfOrder();
	s.m_nPktsRecvDuplicate = PktsRecvDuplicate();
	s.m_nPktsRecvSequenceNumberLurch = PktsRecvLurch();

	s.m_qualityHistogram = m_qualityHistogram;

	s.m_nQualityNtile50th = m_qualitySample.NumSamples() <  2 ? -1 : m_qualitySample.GetPercentile( .50f );
	s.m_nQualityNtile25th = m_qualitySample.NumSamples() <  4 ? -1 : m_qualitySample.GetPercentile( .25f );
	s.m_nQualityNtile5th  = m_qualitySample.NumSamples() < 20 ? -1 : m_qualitySample.GetPercentile( .05f );
	s.m_nQualityNtile2nd  = m_qualitySample.NumSamples() < 50 ? -1 : m_qualitySample.GetPercentile( .02f );

	m_ping.GetLifetimeStats( s );

	s.m_jitterHistogram = m_jitterHistogram;

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

void LinkStatsTrackerBase::GetLinkStats( SteamDatagramLinkStats &s, SteamNetworkingMicroseconds usecNow ) const
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

void LinkStatsTrackerEndToEnd::InitInternal( SteamNetworkingMicroseconds usecNow )
{
	LinkStatsTrackerBase::InitInternal( usecNow );

	m_usecWhenStartedConnectedState = 0;
	m_usecWhenEndedConnectedState = 0;

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
	LinkStatsTrackerBase::GetLifetimeStats(s);

	if ( m_usecWhenStartedConnectedState == 0 || m_usecWhenStartedConnectedState == m_usecWhenEndedConnectedState )
	{
		s.m_nConnectedSeconds = 0;
	}
	else
	{
		SteamNetworkingMicroseconds usecWhenEnded = m_usecWhenEndedConnectedState ? m_usecWhenEndedConnectedState : SteamNetworkingSockets_GetLocalTimestamp();
		s.m_nConnectedSeconds = std::max( k_nMillion, usecWhenEnded - m_usecWhenStartedConnectedState + 500000 ) / k_nMillion;
	}

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
	if ( s.m_nConnectedSeconds >= 0 )
		msg.set_connected_seconds( s.m_nConnectedSeconds );

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

	SET_HISTOGRAM( s.m_qualityHistogram.m_n100 , quality_histogram_100  )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n99  , quality_histogram_99   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n97  , quality_histogram_97   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n95  , quality_histogram_95   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n90  , quality_histogram_90   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n75  , quality_histogram_75   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n50  , quality_histogram_50   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n1   , quality_histogram_1    )
	SET_HISTOGRAM( s.m_qualityHistogram.m_nDead, quality_histogram_dead )

	SET_NTILE( s.m_nQualityNtile50th, quality_ntile_50th )
	SET_NTILE( s.m_nQualityNtile25th, quality_ntile_25th )
	SET_NTILE( s.m_nQualityNtile5th , quality_ntile_5th  )
	SET_NTILE( s.m_nQualityNtile2nd , quality_ntile_2nd  )

	SET_HISTOGRAM( s.m_pingHistogram.m_n25 , ping_histogram_25  )
	SET_HISTOGRAM( s.m_pingHistogram.m_n50 , ping_histogram_50  )
	SET_HISTOGRAM( s.m_pingHistogram.m_n75 , ping_histogram_75  )
	SET_HISTOGRAM( s.m_pingHistogram.m_n100, ping_histogram_100 )
	SET_HISTOGRAM( s.m_pingHistogram.m_n125, ping_histogram_125 )
	SET_HISTOGRAM( s.m_pingHistogram.m_n150, ping_histogram_150 )
	SET_HISTOGRAM( s.m_pingHistogram.m_n200, ping_histogram_200 )
	SET_HISTOGRAM( s.m_pingHistogram.m_n300, ping_histogram_300 )
	SET_HISTOGRAM( s.m_pingHistogram.m_nMax, ping_histogram_max )

	SET_NTILE( s.m_nPingNtile5th , ping_ntile_5th  )
	SET_NTILE( s.m_nPingNtile50th, ping_ntile_50th )
	SET_NTILE( s.m_nPingNtile75th, ping_ntile_75th )
	SET_NTILE( s.m_nPingNtile95th, ping_ntile_95th )
	SET_NTILE( s.m_nPingNtile98th, ping_ntile_98th )

	SET_HISTOGRAM( s.m_jitterHistogram.m_nNegligible, jitter_histogram_negligible )
	SET_HISTOGRAM( s.m_jitterHistogram.m_n1,  jitter_histogram_1  )
	SET_HISTOGRAM( s.m_jitterHistogram.m_n2,  jitter_histogram_2  )
	SET_HISTOGRAM( s.m_jitterHistogram.m_n5,  jitter_histogram_5  )
	SET_HISTOGRAM( s.m_jitterHistogram.m_n10, jitter_histogram_10 )
	SET_HISTOGRAM( s.m_jitterHistogram.m_n20, jitter_histogram_20 )

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
	s.m_nConnectedSeconds = msg.has_connected_seconds() ? msg.connected_seconds() : -1;
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

	SET_HISTOGRAM( s.m_qualityHistogram.m_n100 , quality_histogram_100  )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n99  , quality_histogram_99   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n97  , quality_histogram_97   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n95  , quality_histogram_95   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n90  , quality_histogram_90   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n75  , quality_histogram_75   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n50  , quality_histogram_50   )
	SET_HISTOGRAM( s.m_qualityHistogram.m_n1   , quality_histogram_1    )
	SET_HISTOGRAM( s.m_qualityHistogram.m_nDead, quality_histogram_dead )

	SET_NTILE( s.m_nQualityNtile50th, quality_ntile_50th )
	SET_NTILE( s.m_nQualityNtile25th, quality_ntile_25th )
	SET_NTILE( s.m_nQualityNtile5th , quality_ntile_5th  )
	SET_NTILE( s.m_nQualityNtile2nd , quality_ntile_2nd  )

	SET_HISTOGRAM( s.m_pingHistogram.m_n25 , ping_histogram_25  )
	SET_HISTOGRAM( s.m_pingHistogram.m_n50 , ping_histogram_50  )
	SET_HISTOGRAM( s.m_pingHistogram.m_n75 , ping_histogram_75  )
	SET_HISTOGRAM( s.m_pingHistogram.m_n100, ping_histogram_100 )
	SET_HISTOGRAM( s.m_pingHistogram.m_n125, ping_histogram_125 )
	SET_HISTOGRAM( s.m_pingHistogram.m_n150, ping_histogram_150 )
	SET_HISTOGRAM( s.m_pingHistogram.m_n200, ping_histogram_200 )
	SET_HISTOGRAM( s.m_pingHistogram.m_n300, ping_histogram_300 )
	SET_HISTOGRAM( s.m_pingHistogram.m_nMax, ping_histogram_max )

	SET_NTILE( s.m_nPingNtile5th , ping_ntile_5th  )
	SET_NTILE( s.m_nPingNtile50th, ping_ntile_50th )
	SET_NTILE( s.m_nPingNtile75th, ping_ntile_75th )
	SET_NTILE( s.m_nPingNtile95th, ping_ntile_95th )
	SET_NTILE( s.m_nPingNtile98th, ping_ntile_98th )

	SET_HISTOGRAM( s.m_jitterHistogram.m_nNegligible, jitter_histogram_negligible )
	SET_HISTOGRAM( s.m_jitterHistogram.m_n1,  jitter_histogram_1  )
	SET_HISTOGRAM( s.m_jitterHistogram.m_n2,  jitter_histogram_2  )
	SET_HISTOGRAM( s.m_jitterHistogram.m_n5,  jitter_histogram_5  )
	SET_HISTOGRAM( s.m_jitterHistogram.m_n10, jitter_histogram_10 )
	SET_HISTOGRAM( s.m_jitterHistogram.m_n20, jitter_histogram_20 )

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
	char temp1[256];
	char temp2[256];
	char num[64];

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
		int nPingSamples = stats.m_pingHistogram.TotalCount();
		if ( nPingSamples >= 5 )
		{
			float flToPct = 100.0f / nPingSamples;
			buf.Printf( "%sPing histogram: (%d total samples)\n", pszLeader, nPingSamples );

			buf.Printf( "%s         0-25    25-50    50-75   75-100  100-125  125-150  150-200  200-300     300+\n", pszLeader );
			buf.Printf( "%s    %9d%9d%9d%9d%9d%9d%9d%9d%9d\n",
				pszLeader,
				stats.m_pingHistogram.m_n25,
				stats.m_pingHistogram.m_n50,
				stats.m_pingHistogram.m_n75,
				stats.m_pingHistogram.m_n100,
				stats.m_pingHistogram.m_n125,
				stats.m_pingHistogram.m_n150,
				stats.m_pingHistogram.m_n200,
				stats.m_pingHistogram.m_n300,
				stats.m_pingHistogram.m_nMax );
			buf.Printf( "%s    %8.1f%%%8.1f%%%8.1f%%%8.1f%%%8.1f%%%8.1f%%%8.1f%%%8.1f%%%8.1f%%\n",
				pszLeader,
				stats.m_pingHistogram.m_n25 *flToPct,
				stats.m_pingHistogram.m_n50 *flToPct,
				stats.m_pingHistogram.m_n75 *flToPct,
				stats.m_pingHistogram.m_n100*flToPct,
				stats.m_pingHistogram.m_n125*flToPct,
				stats.m_pingHistogram.m_n150*flToPct,
				stats.m_pingHistogram.m_n200*flToPct,
				stats.m_pingHistogram.m_n300*flToPct,
				stats.m_pingHistogram.m_nMax*flToPct );
			temp1[0] = '\0';
			temp2[0] = '\0';

			#define PING_NTILE( ntile, val ) \
				if ( val >= 0 ) \
				{ \
					V_sprintf_safe( num, "%7s", ntile ); V_strcat_safe( temp1, num ); \
					V_sprintf_safe( num, "%5dms", val ); V_strcat_safe( temp2, num ); \
				}

			PING_NTILE( "5th", stats.m_nPingNtile5th )
			PING_NTILE( "50th", stats.m_nPingNtile50th );
			PING_NTILE( "75th", stats.m_nPingNtile75th );
			PING_NTILE( "95th", stats.m_nPingNtile95th );
			PING_NTILE( "98th", stats.m_nPingNtile98th );

			#undef PING_NTILE

			if ( temp1[0] != '\0' )
			{
				buf.Printf( "%sPing distribution:\n", pszLeader );
				buf.Printf( "%s%s\n", pszLeader, temp1 );
				buf.Printf( "%s%s\n", pszLeader, temp2 );
			}
		}
		else
		{
			buf.Printf( "%sNo ping distribution available.  (%d samples)\n", pszLeader, nPingSamples );
		}
	}

	// Do we have enough quality samples such that the distribution might be interesting?
	{
		int nQualitySamples = stats.m_qualityHistogram.TotalCount();
		if ( nQualitySamples >= 5 )
		{
			float flToPct = 100.0f / nQualitySamples;

			buf.Printf( "%sConnection quality histogram: (%d measurement intervals)\n", pszLeader, nQualitySamples );

			buf.Printf( "%s    perfect    99+  97-99  95-97  90-95  75-90  50-75    <50   dead\n", pszLeader );
			buf.Printf( "%s    %7d%7d%7d%7d%7d%7d%7d%7d%7d\n",
				pszLeader,
				stats.m_qualityHistogram.m_n100,
				stats.m_qualityHistogram.m_n99,
				stats.m_qualityHistogram.m_n97,
				stats.m_qualityHistogram.m_n95,
				stats.m_qualityHistogram.m_n90,
				stats.m_qualityHistogram.m_n75,
				stats.m_qualityHistogram.m_n50,
				stats.m_qualityHistogram.m_n1,
				stats.m_qualityHistogram.m_nDead
			);
			buf.Printf( "%s    %6.1f%%%6.1f%%%6.1f%%%6.1f%%%6.1f%%%6.1f%%%6.1f%%%6.1f%%%6.1f%%\n",
				pszLeader,
				stats.m_qualityHistogram.m_n100 *flToPct,
				stats.m_qualityHistogram.m_n99  *flToPct,
				stats.m_qualityHistogram.m_n97  *flToPct,
				stats.m_qualityHistogram.m_n95  *flToPct,
				stats.m_qualityHistogram.m_n90  *flToPct,
				stats.m_qualityHistogram.m_n75  *flToPct,
				stats.m_qualityHistogram.m_n50  *flToPct,
				stats.m_qualityHistogram.m_n1   *flToPct,
				stats.m_qualityHistogram.m_nDead*flToPct
			);

			temp1[0] = '\0';
			temp2[0] = '\0';

			#define QUALITY_NTILE( ntile, val ) \
				if ( val >= 0 ) \
				{ \
					V_sprintf_safe( num, "%6s", ntile ); V_strcat_safe( temp1, num ); \
					V_sprintf_safe( num, "%5d%%", val ); V_strcat_safe( temp2, num ); \
				}

			QUALITY_NTILE( "50th", stats.m_nQualityNtile50th );
			QUALITY_NTILE( "25th", stats.m_nQualityNtile25th );
			QUALITY_NTILE(  "5th", stats.m_nQualityNtile5th  );
			QUALITY_NTILE(  "2nd", stats.m_nQualityNtile2nd  );

			#undef QUALITY_NTILE

			if ( temp1[0] != '\0' )
			{
				buf.Printf( "%sConnection quality distribution:\n", pszLeader );
				buf.Printf( "%s%s\n", pszLeader, temp1 );
				buf.Printf( "%s%s\n", pszLeader, temp2 );
			}
		}
		else
		{
			buf.Printf( "%sNo connection quality distribution available.  (%d measurement intervals)\n", pszLeader, nQualitySamples );
		}
	}

	// Do we have any jitter samples?
	{
		int nJitterSamples = stats.m_jitterHistogram.TotalCount();
		if ( nJitterSamples >= 1 )
		{
			float flToPct = 100.0f / nJitterSamples;

			buf.Printf( "%sLatency variance histogram: (%d total measurements)\n", pszLeader, nJitterSamples );
			buf.Printf( "%s          <1     1-2     2-5    5-10   10-20     >20\n", pszLeader );
			buf.Printf( "%s    %8d%8d%8d%8d%8d%8d\n", pszLeader,
				stats.m_jitterHistogram.m_nNegligible,
				stats.m_jitterHistogram.m_n1 , 
				stats.m_jitterHistogram.m_n2 , 
				stats.m_jitterHistogram.m_n5 , 
				stats.m_jitterHistogram.m_n10, 
				stats.m_jitterHistogram.m_n20 );
			buf.Printf( "%s    %7.1f%%%7.1f%%%7.1f%%%7.1f%%%7.1f%%%7.1f%%\n", pszLeader,
				stats.m_jitterHistogram.m_nNegligible*flToPct,
				stats.m_jitterHistogram.m_n1 *flToPct,
				stats.m_jitterHistogram.m_n2 *flToPct,
				stats.m_jitterHistogram.m_n5 *flToPct,
				stats.m_jitterHistogram.m_n10*flToPct,
				stats.m_jitterHistogram.m_n20*flToPct );
		}
		else
		{
			buf.Printf( "%sLatency variance histogram not available\n", pszLeader );
		}
	}

	// This is all bogus right now, just don't print it
	//// Do we have enough tx speed samples such that the distribution might be interesting?
	//{
	//	int nTXSpeedSamples = stats.TXSpeedHistogramTotalCount();
	//	if ( nTXSpeedSamples >= 5 )
	//	{
	//		float flToPct = 100.0f / nTXSpeedSamples;
	//		buf.Printf( "%sTX Speed histogram: (%d total samples)\n", pszLeader, nTXSpeedSamples );
	//		buf.Printf( "%s     0 - 16 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram16,   stats.m_nTXSpeedHistogram16  *flToPct );
	//		buf.Printf( "%s    16 - 32 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram32,   stats.m_nTXSpeedHistogram32  *flToPct );
	//		buf.Printf( "%s    32 - 64 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram64,   stats.m_nTXSpeedHistogram64  *flToPct );
	//		buf.Printf( "%s   64 - 128 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram128,  stats.m_nTXSpeedHistogram128 *flToPct );
	//		buf.Printf( "%s  128 - 256 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram256,  stats.m_nTXSpeedHistogram256 *flToPct );
	//		buf.Printf( "%s  256 - 512 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram512,  stats.m_nTXSpeedHistogram512 *flToPct );
	//		buf.Printf( "%s 512 - 1024 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogram1024, stats.m_nTXSpeedHistogram1024*flToPct );
	//		buf.Printf( "%s      1024+ KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nTXSpeedHistogramMax,  stats.m_nTXSpeedHistogramMax *flToPct );
	//		buf.Printf( "%sTransmit speed distribution:\n", pszLeader );
 	//		if ( stats.m_nTXSpeedNtile5th  >= 0 ) buf.Printf( "%s     5%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nTXSpeedNtile5th  );
	//		if ( stats.m_nTXSpeedNtile50th >= 0 ) buf.Printf( "%s    50%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nTXSpeedNtile50th );
	//		if ( stats.m_nTXSpeedNtile75th >= 0 ) buf.Printf( "%s    75%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nTXSpeedNtile75th );
	//		if ( stats.m_nTXSpeedNtile95th >= 0 ) buf.Printf( "%s    95%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nTXSpeedNtile95th );
	//		if ( stats.m_nTXSpeedNtile98th >= 0 ) buf.Printf( "%s    98%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nTXSpeedNtile98th );
	//	}
	//	else
	//	{
	//		buf.Printf( "%sNo connection transmit speed distribution available.  (%d measurement intervals)\n", pszLeader, nTXSpeedSamples );
	//	}
	//}
	//
	//// Do we have enough RX speed samples such that the distribution might be interesting?
	//{
	//	int nRXSpeedSamples = stats.RXSpeedHistogramTotalCount();
	//	if ( nRXSpeedSamples >= 5 )
	//	{
	//		float flToPct = 100.0f / nRXSpeedSamples;
	//		buf.Printf( "%sRX Speed histogram: (%d total samples)\n", pszLeader, nRXSpeedSamples );
	//		buf.Printf( "%s     0 - 16 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram16,   stats.m_nRXSpeedHistogram16  *flToPct );
	//		buf.Printf( "%s    16 - 32 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram32,   stats.m_nRXSpeedHistogram32  *flToPct );
	//		buf.Printf( "%s    32 - 64 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram64,   stats.m_nRXSpeedHistogram64  *flToPct );
	//		buf.Printf( "%s   64 - 128 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram128,  stats.m_nRXSpeedHistogram128 *flToPct );
	//		buf.Printf( "%s  128 - 256 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram256,  stats.m_nRXSpeedHistogram256 *flToPct );
	//		buf.Printf( "%s  256 - 512 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram512,  stats.m_nRXSpeedHistogram512 *flToPct );
	//		buf.Printf( "%s 512 - 1024 KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogram1024, stats.m_nRXSpeedHistogram1024*flToPct );
	//		buf.Printf( "%s      1024+ KB/s:%5d  %3.0f%%\n", pszLeader, stats.m_nRXSpeedHistogramMax,  stats.m_nRXSpeedHistogramMax *flToPct );
	//		buf.Printf( "%sReceive speed distribution:\n", pszLeader );
	//		if ( stats.m_nRXSpeedNtile5th  >= 0 ) buf.Printf( "%s     5%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nRXSpeedNtile5th  );
	//		if ( stats.m_nRXSpeedNtile50th >= 0 ) buf.Printf( "%s    50%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nRXSpeedNtile50th );
	//		if ( stats.m_nRXSpeedNtile75th >= 0 ) buf.Printf( "%s    75%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nRXSpeedNtile75th );
	//		if ( stats.m_nRXSpeedNtile95th >= 0 ) buf.Printf( "%s    95%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nRXSpeedNtile95th );
	//		if ( stats.m_nRXSpeedNtile98th >= 0 ) buf.Printf( "%s    98%% of speeds <= %4d KB/s\n", pszLeader, stats.m_nRXSpeedNtile98th );
	//	}
	//	else
	//	{
	//		buf.Printf( "%sNo connection recieve speed distribution available.  (%d measurement intervals)\n", pszLeader, nRXSpeedSamples );
	//	}
	//}

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
// SteamNetworkingDetailedConnectionStatus
//
///////////////////////////////////////////////////////////////////////////////

void SteamNetworkingDetailedConnectionStatus::Clear()
{
	V_memset( this, 0, sizeof(*this) );
	COMPILE_TIME_ASSERT( k_ESteamNetworkingAvailability_Unknown == 0 );
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
	if ( m_eAvailNetworkConfig != k_ESteamNetworkingAvailability_Current && m_eAvailNetworkConfig != k_ESteamNetworkingAvailability_Unknown )
	{
		buf.Printf( "Network configuration: %s\n", GetAvailabilityString( m_eAvailNetworkConfig ) );
		buf.Printf( "   Cannot communicate with relays without network config." );
	}

	// Unable to talk to any routers?
	if ( m_eAvailAnyRouterCommunication != k_ESteamNetworkingAvailability_Current && m_eAvailAnyRouterCommunication != k_ESteamNetworkingAvailability_Unknown )
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
		buf.Printf( "    Remote host is in data center '%s'\n", SteamNetworkingPOPIDRender( m_info.m_idPOPRemote ).c_str() );
	}

	// If we ever tried to send a packet end-to-end, dump end-to-end stats.
	if ( m_statsEndToEnd.m_lifetime.m_nPacketsSent > 0 )
	{
		LinkStatsPrintToBuf( "    ", m_statsEndToEnd, buf );
	}

	if ( m_szPrimaryRouterName[0] != '\0' )
	{
		buf.Printf( "Primary router: %s", m_szPrimaryRouterName );

		int nPrimaryFrontPing = m_statsPrimaryRouter.m_latest.m_nPingMS;
		if ( m_nPrimaryRouterBackPing >= 0 )
			buf.Printf( "  Ping = %d+%d=%d (front+back=total)\n", nPrimaryFrontPing, m_nPrimaryRouterBackPing,nPrimaryFrontPing+m_nPrimaryRouterBackPing );
		else
			buf.Printf( "  Ping to relay = %d\n", nPrimaryFrontPing );
		LinkStatsPrintToBuf( "    ", m_statsPrimaryRouter, buf );

		if ( m_szBackupRouterName[0] != '\0' )
		{
			buf.Printf( "Backup router: %s  Ping = %d+%d=%d (front+back=total)\n",
				m_szBackupRouterName,
				m_nBackupRouterFrontPing, m_nBackupRouterBackPing,m_nBackupRouterFrontPing+m_nBackupRouterBackPing
			);
		}
	}
	else if ( m_info.m_idPOPRelay )
	{
		buf.Printf( "Communicating via relay in '%s'\n", SteamNetworkingPOPIDRender( m_info.m_idPOPRelay ).c_str() );
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

} // namespace SteamNetworkingSocketsLib
