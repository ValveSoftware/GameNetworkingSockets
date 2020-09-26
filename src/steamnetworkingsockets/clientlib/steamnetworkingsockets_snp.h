//====== Copyright Valve Corporation, All rights reserved. ====================

#pragma once

#include "../steamnetworkingsockets_internal.h"
#include <vector>
#include <map>
#include <set>

struct P2PSessionState_t;

namespace SteamNetworkingSocketsLib {

// Acks may be delayed.  This controls the precision used on the wire to encode the delay time.
constexpr int k_nAckDelayPrecisionShift = 5;
constexpr SteamNetworkingMicroseconds k_usecAckDelayPrecision = (1 << k_nAckDelayPrecisionShift );

// When a receiver detects a dropped packet, wait a bit before NACKing it, to give it time
// to arrive out of order.  This is really important for many different types of connections
// that send on different channels, e.g. DSL, Wifi.
// Here we really could be smarter, by tracking how often dropped
// packets really do arrive out of order.  If the rate is low, then it's
// probably best to go ahead and send a NACK now, rather than waiting.
// But if dropped packets do often arrive out of order, then waiting
// to NACK will probably save some retransmits.  In fact, instead
// of learning the rate, we should probably try to learn the delay.
// E.g. a probability distribution P(t), which describes the odds
// that a dropped packet will have arrived at time t.  Then you
// adjust the NACK delay such that P(nack_delay) gives the best
// balance between false positive and false negative rates.
constexpr SteamNetworkingMicroseconds k_usecNackFlush = 3*1000;

// Max size of a message that we are wiling to *receive*.
constexpr int k_cbMaxMessageSizeRecv = k_cbMaxSteamNetworkingSocketsMessageSizeSend*2;

// The max we will look ahead and allocate data, ahead of the reliable
// messages we have been able to decode.  We limit this to make sure that
// a malicious sender cannot exploit us.
constexpr int k_cbMaxBufferedReceiveReliableData = k_cbMaxMessageSizeRecv + 64*1024;
constexpr int k_nMaxReliableStreamGaps_Extend = 30; // Discard reliable data past the end of the stream, if it would cause us to get too many gaps
constexpr int k_nMaxReliableStreamGaps_Fragment = 20; // Discard reliable data that is filling in the middle of a hole, if it would cause the number of gaps to exceed this number
constexpr int k_nMaxPacketGaps = 62; // Don't bother tracking more than N gaps.  Instead, we will end up NACKing some packets that we actually did receive.  This should not break the protocol, but it protects us from malicious sender

// Hang on to at most N unreliable segments.  When packets are dropping
// and unreliable messages being fragmented, we will accumulate old pieces
// of unreliable messages that we retain in hopes that we will get the
// missing piece and reassemble the whole message.  At a certain point we
// must give up and discard them.  We use a simple strategy of just limiting
// the max total number.  In reality large unreliable messages are just a very bad
// idea, since the odds of the message dropping increase exponentially with the
// number of packets.  With 20 packets, even 1% packet loss becomes ~80% message
// loss.  (Assuming naive fragmentation and reassembly and no forward
// error correction.)
constexpr int k_nMaxBufferedUnreliableSegments = 20;

// If app tries to send a message larger than N bytes unreliably,
// complain about it, and automatically convert to reliable.
// About 15 segments.
constexpr int k_cbMaxUnreliableMsgSizeSend = 15*1100;

// Max possible size of an unreliable segment we could receive.
constexpr int k_cbMaxUnreliableSegmentSizeRecv = k_cbSteamNetworkingSocketsMaxPlaintextPayloadRecv;

// Largest possible total unreliable message we can receive, based on the constraints above
constexpr int k_cbMaxUnreliableMsgSizeRecv = k_nMaxBufferedUnreliableSegments*k_cbMaxUnreliableSegmentSizeRecv;
COMPILE_TIME_ASSERT( k_cbMaxUnreliableMsgSizeRecv > k_cbMaxUnreliableMsgSizeSend + 4096 ); // Postel's law; confirm how much slack we have here

class CSteamNetworkConnectionBase;
class CConnectionTransport;
struct SteamNetworkingMessageQueue;

/// Actual implementation of SteamNetworkingMessage_t, which is the API
/// visible type.  Has extra fields needed to put the message into intrusive
/// linked lists.
class CSteamNetworkingMessage : public SteamNetworkingMessage_t
{
public:
	STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW
	static CSteamNetworkingMessage *New( CSteamNetworkConnectionBase *pParent, uint32 cbSize, int64 nMsgNum, int nFlags, SteamNetworkingMicroseconds usecNow );
	static CSteamNetworkingMessage *New( uint32 cbSize );
	static void DefaultFreeData( SteamNetworkingMessage_t *pMsg );

	/// OK to delay sending this message until this time.  Set to zero to explicitly force
	/// Nagle timer to expire and send now (but this should behave the same as if the
	/// timer < usecNow).  If the timer is cleared, then all messages with lower message numbers
	/// are also cleared.
	inline SteamNetworkingMicroseconds SNPSend_UsecNagle() const { return m_usecTimeReceived; }
	inline void SNPSend_SetUsecNagle( SteamNetworkingMicroseconds x ) { m_usecTimeReceived = x; }

	/// Offset in reliable stream of the header byte.  0 if we're not reliable.
	inline int64 SNPSend_ReliableStreamPos() const { return m_nConnUserData; }
	inline void SNPSend_SetReliableStreamPos( int64 x ) { m_nConnUserData = x; }
	inline int SNPSend_ReliableStreamSize() const
	{
		DbgAssert( m_nFlags & k_nSteamNetworkingSend_Reliable && m_nConnUserData > 0 && m_cbSNPSendReliableHeader > 0 && m_cbSize >= m_cbSNPSendReliableHeader );
		return m_cbSize;
	}

	inline bool SNPSend_IsReliable() const
	{
		if ( m_nFlags & k_nSteamNetworkingSend_Reliable )
		{
			DbgAssert( m_nConnUserData > 0 && m_cbSNPSendReliableHeader > 0 && m_cbSize >= m_cbSNPSendReliableHeader );
			return true;
		}
		DbgAssert( m_nConnUserData == 0 && m_cbSNPSendReliableHeader == 0 );
		return false;
	}

	// Reliable stream header
	int m_cbSNPSendReliableHeader;
	byte *SNPSend_ReliableHeader()
	{
		// !KLUDGE! Reuse the peer identity to hold the reliable header
		return (byte*)&m_identityPeer;
	}

	/// Remove it from queues
	void Unlink();

	struct Links
	{
		SteamNetworkingMessageQueue *m_pQueue;
		CSteamNetworkingMessage *m_pPrev;
		CSteamNetworkingMessage *m_pNext;

		inline void Clear() { m_pQueue = nullptr; m_pPrev = nullptr; m_pNext = nullptr; }
	};

	/// Intrusive links for the "primary" list we are in
	Links m_links;

	/// Intrusive links for any secondary list we may be in.  (Same listen socket or
	/// P2P channel, depending on message type)
	Links m_linksSecondaryQueue;

	void LinkBefore( CSteamNetworkingMessage *pSuccessor, Links CSteamNetworkingMessage::*pMbrLinks, SteamNetworkingMessageQueue *pQueue );
	void LinkToQueueTail( Links CSteamNetworkingMessage::*pMbrLinks, SteamNetworkingMessageQueue *pQueue );
	void UnlinkFromQueue( Links CSteamNetworkingMessage::*pMbrLinks );

private:
	// Use New and Release()!!
	inline CSteamNetworkingMessage() {}
	inline ~CSteamNetworkingMessage() {}
	static void ReleaseFunc( SteamNetworkingMessage_t *pIMsg );
};

/// A doubly-linked list of CSteamNetworkingMessage
struct SteamNetworkingMessageQueue
{
	CSteamNetworkingMessage *m_pFirst = nullptr;
	CSteamNetworkingMessage *m_pLast = nullptr;

	inline bool empty() const
	{
		if ( m_pFirst )
		{
			Assert( m_pLast );
			return false;
		}
		Assert( !m_pLast );
		return true;
	}

	/// Remove the first messages out of the queue (up to nMaxMessages).  Returns the number returned
	int RemoveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages );

	/// Delete all queued messages
	void PurgeMessages();
};

/// Maximum number of packets we will send in one Think() call.
const int k_nMaxPacketsPerThink = 16;

/// Max number of tokens we are allowed to store up in reserve, for a burst.
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
	//
	// FIXME - Could definitely pack this structure better.  And maybe
	//         worth it to optimize cache
	//

	/// Local timestamp when we sent it
	SteamNetworkingMicroseconds m_usecWhenSent;

	/// Did we get an ack block from peer that explicitly marked this
	/// packet as being skipped?  Note that we might subsequently get an
	/// an ack for this same packet, that's OK!
	bool m_bNack;

	/// Transport used to send
	CConnectionTransport *m_pTransport;

	/// List of reliable segments.  Ignoring retransmission,
	/// there really is no reason why we we would need to have
	/// more than 1 in a packet, even if there are multiple
	/// reliable messages.  If we need to retry, we might
	/// be fragmented.  But usually it will only be a few.
	vstd::small_vector<SNPRange_t,1> m_vecReliableSegments;
};

struct SSNPSendMessageList : public SteamNetworkingMessageQueue
{

	/// Unlink the message at the head, if any and return it.
	/// Unlike STL pop_front, this will return nullptr if the
	/// list is empty
	CSteamNetworkingMessage *pop_front()
	{
		CSteamNetworkingMessage *pResult = m_pFirst;
		if ( pResult )
		{
			Assert( m_pLast );
			Assert( pResult->m_links.m_pQueue == this );
			Assert( pResult->m_links.m_pPrev == nullptr );
			m_pFirst = pResult->m_links.m_pNext;
			if ( m_pFirst )
			{
				Assert( m_pFirst->m_links.m_pPrev == pResult );
				Assert( m_pFirst->m_nMessageNumber > pResult->m_nMessageNumber );
				m_pFirst->m_links.m_pPrev = nullptr;
			}
			else
			{
				Assert( m_pLast == pResult );
				m_pLast = nullptr;
			}
			pResult->m_links.m_pQueue = nullptr;
			pResult->m_links.m_pNext = nullptr;
		}
		return pResult;
	}

	/// Optimized insertion when we know it goes at the end
	void push_back( CSteamNetworkingMessage *pMsg )
	{
		if ( m_pFirst == nullptr )
		{
			Assert( m_pLast == nullptr );
			m_pFirst = pMsg;
		}
		else
		{
			// Messages are always kept in message number order
			Assert( pMsg->m_nMessageNumber > m_pLast->m_nMessageNumber );
			Assert( m_pLast->m_links.m_pNext == nullptr );
			m_pLast->m_links.m_pNext = pMsg;
		}
		pMsg->m_links.m_pQueue = this;
		pMsg->m_links.m_pNext = nullptr;
		pMsg->m_links.m_pPrev = m_pLast;
		m_pLast = pMsg;
	}

};

struct SSNPSenderState
{
	SSNPSenderState();
	~SSNPSenderState() {
		Shutdown();
	}
	void Shutdown();

	/// Current sending rate in bytes per second, RFC 3448 4.2 states default
	/// is one packet per second, but that is insane and we're not doing that.
	/// In most cases we will set a default based on initial ping, so this is
	/// only rarely used.
	int m_n_x = 32*1024;

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

		return SteamNetworkingMicroseconds( m_flTokenBucket * -1e6f / (float)m_n_x ) + 1; // +1 to make sure that if we don't have any tokens, we never return 0, since zero means "ready right now"
	}

	/// Nagle timer on all pending messages
	void ClearNagleTimers()
	{
		CSteamNetworkingMessage *pMsg = m_messagesQueued.m_pLast;
		while ( pMsg && pMsg->SNPSend_UsecNagle() )
		{
			pMsg->SNPSend_SetUsecNagle( 0 );
			pMsg = pMsg->m_links.m_pPrev;
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
	std_map<int64,SNPInFlightPacket_t> m_mapInFlightPacketsByPktNum;

	/// The next unacked packet that should be timed out and implicitly NACKed,
	/// if we don't receive an ACK in time.  Will be m_mapInFlightPacketsByPktNum.end()
	/// if we don't have any in flight packets that we are waiting on.
	std_map<int64,SNPInFlightPacket_t>::iterator m_itNextInFlightPacketToTimeout;

	/// Ordered list of reliable ranges that we have recently sent
	/// in a packet.  These should be non-overlapping, and furthermore
	/// should not overlap with with any range in m_listReadyReliableRange
	///
	/// The "value" portion of the map is the message that has the first bit of
	/// reliable data we need for this message
	std_map<SNPRange_t,CSteamNetworkingMessage*,SNPRange_t::NonOverlappingLess> m_listInFlightReliableRange;

	/// Ordered list of ranges that have been put on the wire,
	/// but have been detected as dropped, and now need to be retried.
	std_map<SNPRange_t,CSteamNetworkingMessage*,SNPRange_t::NonOverlappingLess> m_listReadyRetryReliableRange;

	/// Oldest packet sequence number that we are still asking peer
	/// to send acks for.
	int64 m_nMinPktWaitingOnAck = 0;

	// Remove messages from m_unackedReliableMessages that have been fully acked.
	void RemoveAckedReliableMessageFromUnackedList();

	/// Check invariants in debug.
	#if STEAMNETWORKINGSOCKETS_SNP_PARANOIA == 0 
		inline void DebugCheckInFlightPacketMap() const {}
	#else
		void DebugCheckInFlightPacketMap() const;
	#endif
	#if STEAMNETWORKINGSOCKETS_SNP_PARANOIA > 1
		inline void MaybeCheckInFlightPacketMap() const { DebugCheckInFlightPacketMap(); }
	#else
		inline void MaybeCheckInFlightPacketMap() const {}
	#endif
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
	int m_cbSegSize = -1;
	bool m_bLast = false;
	char m_buf[ k_cbMaxUnreliableSegmentSizeRecv ];
};

struct SSNPPacketGap
{
	int64 m_nEnd; // just after the last packet received
	SteamNetworkingMicroseconds m_usecWhenReceivedPktBefore; // So we can send RTT data in our acks
	SteamNetworkingMicroseconds m_usecWhenAckPrior; // We need to send an ack for everything with lower packet numbers than this gap by this time.  (Earlier is OK.)
	SteamNetworkingMicroseconds m_usecWhenOKToNack; // Don't give up on the gap being filed before this time
};

struct SSNPReceiverState
{
	SSNPReceiverState();
	~SSNPReceiverState() {
		Shutdown();
	}
	void Shutdown();

	/// Unreliable message segments that we have received.  When an unreliable message
	/// needs to be fragmented, we store the pieces here.  NOTE: it might be more efficient
	/// to use a simpler container, with worse O(), since this should ordinarily be
	/// a pretty small list.
	std_map<SSNPRecvUnreliableSegmentKey,SSNPRecvUnreliableSegmentData> m_mapUnreliableSegments;

	/// Stream position of the first byte in m_bufReliableData.  Remember that the first byte
	/// in the reliable stream is actually at position 1, not 0
	int64 m_nReliableStreamPos = 1;

	/// The highest message number we have seen so far.
	int64 m_nHighestSeenMsgNum = 0;

	/// The message number of the most recently received reliable message
	int64 m_nLastRecvReliableMsgNum = 0;

	/// Reliable data stream that we have received.  This might have gaps in it!
	std_vector<byte> m_bufReliableStream;

	/// Gaps in the reliable data.  These are created when we receive reliable data that
	/// is beyond what we expect next.  Since these must never overlap, we store them
	/// using begin as the key and end as the value.
	///
	/// !SPEED! We should probably use a small fixed-sized, sorted vector here,
	/// since in most cases the list will be small, and the cost of dynamic memory
	/// allocation will be way worse than O(n) insertion/removal.
	std_map<int64,int64> m_mapReliableStreamGaps;

	/// List of gaps in the packet sequence numbers we have received.
	/// Since these must never overlap, we store them using begin as the
	/// key and the end in the value.
	///
	/// The last item in the list is a sentinel with
	/// begin and end set to INT64_MAX, and m_usecWhenAckPrior is
	/// the time when we need to flush acks/backs for all packets,
	/// including those received after the last gap (if any --
	/// INT64_MAX means nothing scheduled).  Remember, our wire
	/// protocol cannot report on packet N without also reporting
	/// on all packets numbered < N.
	///
	/// !SPEED! We should probably use a small fixed-sized, sorted vector here,
	/// since in most cases the list will be small, and the cost of dynamic memory
	/// allocation will be way worse than O(n) insertion/removal.
	std_map<int64,SSNPPacketGap> m_mapPacketGaps;

	/// Oldest packet sequence number we need to ack to our peer
	int64 m_nMinPktNumToSendAcks = 0;

	/// Packet number when we received the value of m_nMinPktNumToSendAcks
	int64 m_nPktNumUpdatedMinPktNumToSendAcks = 0;

	/// The next ack that needs to be sent.  The invariant
	/// for the times are:
	///
	/// * Blocks with lower packet numbers: m_usecWhenAckPrior = INT64_MAX
	/// * This block: m_usecWhenAckPrior < INT64_MAX, or we are the sentinel
	/// * Blocks with higher packet numbers (if we are not the sentinel): m_usecWhenAckPrior >= previous m_usecWhenAckPrior
	///
	/// We might send acks before they are due, rather than
	/// waiting until the last moment!  If we are going to
	/// send a packet at all, we usually try to send at least
	/// a few acks, and if there is room in the packet, as
	/// many as will fit.  The one exception is that if
	/// sending an ack would imply a NACK that we don't want to
	/// send yet.  (Remember the restrictions on what we are able
	/// to communicate due to the tight RLE encoding of the wire
	/// format.)  These delays are usually very short lived, and
	/// only happen when there is packet loss, so they don't delay
	/// acks very much.  The whole purpose of this rather involved
	/// bookkeeping is to figure out which acks we *need* to send,
	/// and which acks we cannot send yet, so we can make optimal
	/// decisions.
	std_map<int64,SSNPPacketGap>::iterator m_itPendingAck;

	/// Iterator into m_mapPacketGaps.  If != the sentinel,
	/// we will avoid reporting on the dropped packets in this
	/// gap (and all higher numbered packets), because we are
	/// waiting in the hopes that they will arrive out of order.
	std_map<int64,SSNPPacketGap>::iterator m_itPendingNack;

	/// Queue a flush of ALL acks (and NACKs!) by the given time.
	/// If anything is scheduled to happen earlier, that schedule
	/// will still be honered.  We will ack up to that packet number,
	/// and then we we may report higher numbered blocks, or we may
	/// stop and wait to report more acks until later.
	void QueueFlushAllAcks( SteamNetworkingMicroseconds usecWhen );

	/// Return the time when we need to flush out acks, or INT64_MAX
	/// if we don't have any acks pending right now.
	inline SteamNetworkingMicroseconds TimeWhenFlushAcks() const
	{
		// Paranoia
		if ( m_mapPacketGaps.empty() )
		{
			AssertMsg( false, "TimeWhenFlushAcks - we're shut down!" );
			return INT64_MAX;
		}
		return m_itPendingAck->second.m_usecWhenAckPrior;
	}

	/// Check invariants in debug.
	#if STEAMNETWORKINGSOCKETS_SNP_PARANOIA > 1
		void DebugCheckPackGapMap() const;
	#else
		inline void DebugCheckPackGapMap() const {}
	#endif

	// Stats.  FIXME - move to LinkStatsEndToEnd and track rate counters
	int64 m_nMessagesRecvReliable = 0;
	int64 m_nMessagesRecvUnreliable = 0;
};

} // SteamNetworkingSocketsLib
