//====== Copyright Valve Corporation, All rights reserved. ====================

#pragma once

#include "../steamnetworkingsockets_internal.h"
#include <vector>
#include <map>
#include <set>

struct P2PSessionState_t;

namespace SteamNetworkingSocketsLib {

class CSteamNetworkConnectionBase;

//
// Constants
//
const int TFRC_NDUPACK = 3; // Number of packets to wait after a missing packet (RFC 4342, 6.1)

const int NINTERVAL = 8; // Number of loss intervals (RFC 4342, 8.6.1). 
const int LIH_SIZE = (NINTERVAL + 1); // The history size is one more than NINTERVAL, 
										 // since the `open' interval I_0 is always 
										 // stored as the first entry.
const SteamNetworkingMicroseconds kSNPInitialNagle = 100000; // 100ms default nagle, the nagle timeout 
															 // will get changed to 2*R once we have rtt

// Linux uses 200ms RTO minimum, let's use that
const SteamNetworkingMicroseconds TCP_RTO_MIN = k_nMillion / 5;

// Note this shouldn't be more then TCP_RTO_MIN since that's used for re-transmission and feedback pump
const SteamNetworkingMicroseconds kSNPMinThink = TCP_RTO_MIN; 

const SteamNetworkingMicroseconds TFRC_INITIAL_TIMEOUT = 2 * k_nMillion;

const int k_nSteamDatagramGlobalMinRate = 5000;
const int k_nSteamDatagramGlobalMaxRate = 4*k_nMillion;

const int k_nBurstMultiplier = 4; // how much we should allow for burst, TFRC defaults to 2 but we need more

const int k_nMaxPacketsPerThink = 16;

const float k_flSendRateBurstOverageAllowance = k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend;

struct SNPRange_t
{
	/// Byte or sequence number range
	int64 m_nBegin;
	int64 m_nEnd; // STL-style.  It's one past the end

	inline int64 length() const
	{
		// In general, allow zero-length ranges, but not negative ones
		Assert( m_nEnd >= m_nBegin );
		return m_nEnd - m_nBegin;
	}

	/// Strict comparison function.  This is used in situations where
	/// ranges must not overlap, AND we also never search for
	/// a range that might overlap.
	struct NonOverlappingLess
	{
		inline bool operator ()(const SNPRange_t &l, const SNPRange_t &r ) const
		{
			if ( l.m_nBegin < r.m_nBegin ) return true;
			AssertMsg( l.m_nBegin > r.m_nBegin || l.m_nEnd == r.m_nEnd, "Ranges should not overlap in this map!" );
			return false;
		}
	};
};

/// A packet that has been sent but we don't yet know if was received
/// or dropped.  These are kept in an ordered map keyed by packet number.
/// (Hence the packet number not being a member)  When we receive an ACK,
/// we remove packets from this list.
struct SNPInFlightPacket_t
{
	/// Local timestamp when we sent it
	SteamNetworkingMicroseconds m_usecWhenSent;

	/// Did we get an ack block from peer that explicitly marked this
	/// packet as being skipped?  Note that we might subsequently get an
	/// an ack for this same packet, that's OK!
	bool m_bNack;

	/// List of reliable segments.  Ignoring retransmission,
	/// there really is no reason why we we would need to have
	/// more than 1 in a packet, even if there are multiple
	/// reliable messages.  If we need to retry, we might
	/// be fragmented.  But usually it will only be a few.
	vstd::small_vector<SNPRange_t,1> m_vecReliableSegments;
};

/// Track an outbound message in various states
struct SNPSendMessage_t
{
	// FIXME Probably should provide an optimized allocator for this,
	// since these are small and are created and destroyed often.

	~SNPSendMessage_t()
	{
		delete [] m_pData;
	}

	/// Message number.
	int64 m_nMsgNum;

	/// Messages are kept in doubly-linked lists, ordered by message number
	SNPSendMessage_t *m_pNext;
	SNPSendMessage_t *m_pPrev;

	/// OK to delay sending this message until this time.  Set to zero to explicitly force
	/// Nagle timer to expire and send now (but this should behave the same as if the
	/// timer < usecNow).  If the timer is cleared, then all messages with lower message numbers
	/// are also cleared.
	SteamNetworkingMicroseconds m_usecNagle;

	// Memory buffer that we own.  NOTE: for reliable messages, the
	// reliable header is generated and saved at the time we accept the
	// message from the app.
	int m_cbSize;
	byte *m_pData;

	/// Offset in reliable stream of the header byte.  0 if we're not reliable.
	int64 m_nReliableStreamPos;
};

struct SSNPSendMessageList
{
	SNPSendMessage_t *m_pFirst = nullptr;
	SNPSendMessage_t *m_pLast = nullptr;

	/// Return true if the list is empty
	inline bool empty() const
	{
		if ( m_pFirst == nullptr )
		{
			Assert( m_pLast == nullptr );
			return true;
		}
		Assert( m_pLast != nullptr );
		return false;
	}

	/// Unlink the message at the head, if any and return it.
	/// Unlike STL pop_front, this will return nullptr if the
	/// list is empty
	SNPSendMessage_t *pop_front()
	{
		SNPSendMessage_t *pResult = m_pFirst;
		if ( pResult )
		{
			Assert( m_pLast );
			Assert( pResult->m_pPrev == nullptr );
			m_pFirst = pResult->m_pNext;
			if ( m_pFirst )
			{
				Assert( m_pFirst->m_pPrev == pResult );
				Assert( m_pFirst->m_nMsgNum > pResult->m_nMsgNum );
				m_pFirst->m_pPrev = nullptr;
			}
			else
			{
				Assert( m_pLast == pResult );
				m_pLast = nullptr;
			}
			pResult->m_pNext = nullptr;
		}
		return pResult;
	}

	/// Optimized insertion when we know it goes at the end
	void push_back( SNPSendMessage_t *pMsg )
	{
		if ( m_pFirst == nullptr )
		{
			Assert( m_pLast == nullptr );
			m_pFirst = pMsg;
		}
		else
		{
			// Messages are always kept in message number order
			Assert( pMsg->m_nMsgNum > m_pLast->m_nMsgNum );
			Assert( m_pLast->m_pNext == nullptr );
			m_pLast->m_pNext = pMsg;
		}
		pMsg->m_pNext = nullptr;
		pMsg->m_pPrev = m_pLast;
		m_pLast = pMsg;
	}

};

struct SSNPSenderState
{

	// Sender TFRC control values and timers

	// Current sending rate in bytes per second, RFC 3448 4.2 states default
	// is one packet per second, but that is insane and we're not doing that.
	// In most cases we will set a default based on initial ping, so this is
	// only rarely used.
	int m_n_x = 32*1024;

	int m_n_minRate = 0; // Minimum send rate, if 0 defaults to using the global config setting
	int m_n_maxRate = 0; // Maximum send rate, if 0 defaults to using the global config setting
	//int m_n_x_recv = 0; // Received rate transmitted by receiver
	int m_n_x_calc = 0;	// Calculated rate in bytes per second
	//uint32 m_un_p = 0;		// Current loss event rate (0-1) scaled to 0-UINT_MAX
	int m_n_tx_s = k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend;	// Average packet size in bytes
	//SteamNetworkingMicroseconds m_usec_rto = 0; // RTO calc
	//SteamNetworkingMicroseconds m_usec_nfb = 0; // Nofeedback Timer
	//bool m_bSentPacketSinceNFB = false; // Set to false whenever we update nfb to detect sender idle periods

	/// If >=0, then we can send a full packet right now.  We allow ourselves to "store up"
	/// about 1 packet worth of "reserve".  In other words, if we have not sent any packets
	/// for a while, basically we allow ourselves to send two packets in rapid succession,
	/// thus "bursting" over the limit by 1 packet.  That long term rate will be clamped by
	/// the send rate.
	///
	/// If <0, then we are currently "over" our rate limit and need to wait before we can
	/// send a packet.
	///
	/// Provision for accumulating "credits" and burst allowance, to account for lossy
	/// kernel scheduler, etc is mentioned in RFC 5348, section 4.6.
	float m_flTokenBucket = 0;

	/// Last time that we added tokens to m_flTokenBucket
	SteamNetworkingMicroseconds m_usecTokenBucketTime = 0;

	void TokenBucket_Init( SteamNetworkingMicroseconds usecNow )
	{
		m_usecTokenBucketTime = usecNow;
		m_flTokenBucket = k_flSendRateBurstOverageAllowance;
	}

	/// Accumulate "tokens" into our bucket base on the current calculated send rate
	void TokenBucket_Accumulate( SteamNetworkingMicroseconds usecNow )
	{
		float flElapsed = ( usecNow - m_usecTokenBucketTime ) * 1e-6;
		m_flTokenBucket += (float)m_n_x * flElapsed;
		m_usecTokenBucketTime = usecNow;

		// If we don't currently have any packets ready to send right now,
		// then go ahead and limit the tokens.  If we do have packets ready
		// to send right now, then we must assume that we would be trying to
		// wakeup as soon as we are ready to send the next packet, and thus
		// any excess tokens we accumulate are because the scheduler woke
		// us up late, and we are not actually bursting
		if ( TimeWhenWantToSendNextPacket() > usecNow )
			TokenBucket_Limit();
	}

	/// Return timestamp when we will *want* to send the next packet.
	/// (Ignoring rate limiting.)
	/// 0=ASAP
	/// INT64_MAX: Idle, nothing to send
	/// Other - Nagle timer of next packet
	inline SteamNetworkingMicroseconds TimeWhenWantToSendNextPacket() const
	{
		if ( !m_listReadyRetryReliableRange.empty() )
			return 0;
		if ( m_messagesQueued.empty() )
		{
			Assert( PendingBytesTotal() == 0 );
			return INT64_MAX;
		}

		// FIXME acks, stop_waiting?

		// Have we got at least a full packet ready to go?
		if ( PendingBytesTotal() >= k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend )
			// Send it ASAP
			return 0;

		// We have less than a full packet's worth of data.  Wait until
		// the Nagle time, if we have one
		return m_messagesQueued.m_pFirst->m_usecNagle;
	}

	/// Limit our token bucket to the max reserve amount
	void TokenBucket_Limit()
	{
		if ( m_flTokenBucket > k_flSendRateBurstOverageAllowance )
			m_flTokenBucket = k_flSendRateBurstOverageAllowance;
	}

	/// Calculate time until we could send our next packet, checking our token
	/// bucket and the current send rate
	SteamNetworkingMicroseconds CalcTimeUntilNextSend() const
	{
		// Do we have tokens to burn right now?
		if ( m_flTokenBucket >= 0.0f )
			return 0;

		return SteamNetworkingMicroseconds( m_flTokenBucket * -1e6 / (float)m_n_x ) + 1; // +1 to make sure that if we don't have any tokens, we never return 0, since zero means "ready right now"
	}

	/// Nagle timer on all pending messages
	void ClearNagleTimers()
	{
		SNPSendMessage_t *pMsg = m_messagesQueued.m_pLast;
		while ( pMsg && pMsg->m_usecNagle )
		{
			pMsg->m_usecNagle = 0;
			pMsg = pMsg->m_pPrev;
		}
	}

	// Current message number, we ++ when adding a message
	int64 m_nReliableStreamPos = 1;
	int64 m_nLastSentMsgNum = 0; // Will increment to 1 with first message
	int64 m_nLastSendMsgNumReliable = 0;

	/// List of messages that we have not yet finished putting on the wire the first time.
	/// The Nagle timer may be active on one or more, but if so, it is only on messages
	/// at the END of the list.  The first message may be partially sent.
	SSNPSendMessageList m_messagesQueued;

	/// How many bytes into the first message in the queue have we put on the wire?
	int m_cbCurrentSendMessageSent = 0;

	/// List of reliable messages that have been fully placed on the wire at least once,
	/// but we're hanging onto because of the potential need to retry.  (Note that if we get
	/// packet loss, it's possible that we hang onto a message even after it's been fully
	/// acked, because a prior message is still needed.  We always operate on this list
	/// like a queue, rather than seeking into the middle of the list and removing messages
	/// as soon as they are no longer needed.)
	SSNPSendMessageList m_unackedReliableMessages;

	// Buffered data counters.  See SteamNetworkingQuickConnectionStatus for more info
	int m_cbPendingUnreliable = 0;
	int m_cbPendingReliable = 0;
	int m_cbSentUnackedReliable = 0;
	inline int PendingBytesTotal() const { return m_cbPendingUnreliable + m_cbPendingReliable; }

	// Stats.  FIXME - move to LinkStatsEndToEnd and track rate counters
	int64 m_nMessagesSentReliable = 0;
	int64 m_nMessagesSentUnreliable = 0;

	/// List of packets that we have sent but don't know whether they were received or not.
	/// We keep a dummy sentinel at the head of the list, with a negative packet number.
	/// This vastly simplifies the processing.
	std::map<int64,SNPInFlightPacket_t> m_mapInFlightPacketsByPktNum;

	/// The next unacked packet that should be timed out and implicitly NACKed,
	/// if we don't receive an ACK in time.  Will be m_mapInFlightPacketsByPktNum.end()
	/// if we don't have any in flight packets that we are waiting on.
	std::map<int64,SNPInFlightPacket_t>::iterator m_itNextInFlightPacketToTimeout;

	/// Ordered list of reliable ranges that we have recently sent
	/// in a packet.  These should be non-overlapping, and furthermore
	/// should not overlap with with any range in m_listReadyReliableRange
	///
	/// The "value" portion of the map is the message that has the first bit of
	/// reliable data we need for this message
	std::map<SNPRange_t,SNPSendMessage_t*,SNPRange_t::NonOverlappingLess> m_listInFlightReliableRange;

	/// Ordered list of ranges that have been put on the wire,
	/// but have been detected as dropped, and now need to be retried.
	std::map<SNPRange_t,SNPSendMessage_t*,SNPRange_t::NonOverlappingLess> m_listReadyRetryReliableRange;

	/// Oldest packet sequence number that we are still asking peer
	/// to send acks for.
	int64 m_nMinPktWaitingOnAck = 0;

	/// Time when this was updated
	//SteamNetworkingMicroseconds m_usecWhenAdvancedMinPktWaitingOnAck = 0;

	/* TFRC sender states */
	enum ETFRCSenderStates {
		TFRC_SSTATE_NO_SENT = 1,
		TFRC_SSTATE_NO_FBACK,
		TFRC_SSTATE_FBACK,
	};
	ETFRCSenderStates m_e_tx_state = TFRC_SSTATE_NO_SENT; // Sender state
	SteamNetworkingMicroseconds m_usec_no_feedback_timer = 0; // No feedback timer
	SteamNetworkingMicroseconds m_usec_ld = 0; // Time last doubled during slow start

//	// Set when receiver determines we need to send a feedback packet
//	/* TFRC feedback sender states */
//	enum ETFRCSenderFeedbackStates {
//		TFRC_SSTATE_FBACK_NONE = 0, //
//		TFRC_SSTATE_FBACK_REQ = 1, // Set if we should send a feedback without data (parm change)
//		TFRC_SSTATE_FBACK_PERODIC = 2, // When periodic feedback is set, we piggy pack on a data packet
//	};
//	ETFRCSenderFeedbackStates m_sendFeedbackState = TFRC_SSTATE_FBACK_NONE;

	// Remove messages from m_unackedReliableMessages that have been fully acked.
	void RemoveAckedReliableMessageFromUnackedList();

	void SetNoFeedbackTimer( SteamNetworkingMicroseconds usecNow )
	{
		// FIXME
		//if ( m_usec_rto == 0 )
		//{
		//	m_usec_nfb = usecNow + TFRC_INITIAL_TIMEOUT;
		//}
		//else
		//{
		//	// Calculate inter-packet-interval ("ipi")
		//	SteamNetworkingMicroseconds ipi = k_nMillion * (int64)m_n_tx_s / (int64)m_n_x;
		//
		//	// Expect feedback within the RTO timeout, or 2 sent packets,
		//	// whichever is greater
		//	//
		//	// Hm, this isn't what the RFC said to do.  I wonder where this logic came from.
		//	m_usec_nfb = usecNow + Max( m_usec_rto, 2 * ipi );
		//}
	}
};

struct SSNPRecvUnreliableSegmentKey
{
	int64 m_nMsgNum;
	int m_nOffset;

	inline bool operator<(const SSNPRecvUnreliableSegmentKey &x) const
	{
		if ( m_nMsgNum < x.m_nMsgNum ) return true;
		if ( m_nMsgNum > x.m_nMsgNum ) return false;
		return m_nOffset < x.m_nOffset;
	}
};

struct SSNPRecvUnreliableSegmentData
{
	int m_cbSize = -1;
	bool m_bLast = false;
	char m_buf[ k_cbSteamNetworkingSocketsMaxPlaintextPayloadRecv ];
};

struct SSNPPacketGap
{
	int64 m_nEnd; // just after the last packet received
	SteamNetworkingMicroseconds m_usecWhenReceivedPktBefore;
};

struct SSNPReceiverState
{
	/// Unreliable message segments that we have received.  When an unreliable message
	/// needs to be fragmented, we store the pieces here.  NOTE: it might be more efficient
	/// to use a simpler container, with worse O(), since this should ordinarily be
	/// a pretty small list.
	std::map<SSNPRecvUnreliableSegmentKey,SSNPRecvUnreliableSegmentData> m_mapUnreliableSegments;

	/// Stream position of the first byte in m_bufReliableData.  Remember that the first byte
	/// in the reliable stream is actually at position 1, not 0
	int64 m_nReliableStreamPos = 1;

	/// The highest message number we have seen so far.
	int64 m_nHighestSeenMsgNum = 0;

	/// The message number of the most recently received reliable message
	int64 m_nLastRecvReliableMsgNum = 0;

	/// Reliable data stream that we have received.  This might have gaps in it!
	std::vector<byte> m_bufReliableStream;

	/// Gaps in the reliable data.  These are created when we receive reliable data that
	/// is beyond what we expect next.  Since these must never overlap, we store them
	/// using begin as the key and end as the value.
	///
	/// !SPEED! We should probably use a small fixed-sized, sorted vector here,
	/// since in most cases the list will be small, and the cost of dynamic memory
	/// allocation will be way worse than O(n) insertion/removal.
	std::map<int64,int64> m_mapReliableStreamGaps;

	/// List of gaps in the packet sequence numbers we have received.
	/// Since these must never overlap, we store them using begin as the
	/// key and the end in the value.
	///
	/// !SPEED! We should probably use a small fixed-sized, sorted vector here,
	/// since in most cases the list will be small, and the cost of dynamic memory
	/// allocation will be way worse than O(n) insertion/removal.
	std::map<int64,SSNPPacketGap> m_mapPacketGaps;

	/// Oldest packet sequence number we need to ack to our peer
	int64 m_nMinPktNumToSendAcks = 0;

	/// Packet number when we received the value of m_nMinPktNumToSendAcks
	int64 m_nPktNumUpdatedMinPktNumToSendAcks = 0;

	/// Timeout for when we need to flush out acks, if no other opportunity
	/// comes along (piggy on top of outbound data packet) to do this.
	SteamNetworkingMicroseconds m_usecWhenFlushAck = INT64_MAX;

	inline void SentAcks()
	{
		m_usecWhenFlushAck = INT64_MAX;
	}
	inline void MarkNeedToSendAck( SteamNetworkingMicroseconds usecNow )
	{
		m_usecWhenFlushAck = std::min( m_usecWhenFlushAck, usecNow + k_usecMaxDataAckDelay );
	}

	// Stats.  FIXME - move to LinkStatsEndToEnd and track rate counters
	int64 m_nMessagesRecvReliable = 0;
	int64 m_nMessagesRecvUnreliable = 0;



	enum ETFRCFeedbackType {
		TFRC_FBACK_NONE = 0,
		TFRC_FBACK_INITIAL,
		TFRC_FBACK_PERIODIC,
		TFRC_FBACK_PARAM_CHANGE
	};

	/* TFRC receiver states */
	enum ETFRCReceiverStates {
		TFRC_RSTATE_NO_DATA = 1,
		TFRC_RSTATE_DATA,
	};

	ETFRCReceiverStates m_e_rx_state = TFRC_RSTATE_NO_DATA; // Receiver state
	int m_n_x_recv = 0; // Receiver estimate of send rate (RFC 3448, sec. 4.3)
	SteamNetworkingMicroseconds m_usec_tstamp_last_feedback = 0; // Time at which last feedback was sent
	int m_n_rx_s = 0;	// Average packet size in bytes
	SteamNetworkingMicroseconds m_usec_next_feedback = 0; // Time we should send next feedback,
														  // its usually now + rtt, but can be set earlier 
														  // if a param change happense.

	uint32 m_li_i_mean = 0; // Current Average Loss Interval [RFC 3448, 5.4] scaled to 0-UINT_MAX
};

} // SteamNetworkingSocketsLib
