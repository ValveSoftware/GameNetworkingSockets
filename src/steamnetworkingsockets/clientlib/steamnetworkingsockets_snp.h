//====== Copyright Valve Corporation, All rights reserved. ====================

#pragma once

#include "../steamnetworkingsockets_internal.h"
#include <tier1/utllinkedlist.h>
#include <vector>
#include <map>
#include <set>

struct P2PSessionState_t;

namespace SteamNetworkingSocketsLib {

struct LockDebugInfo;

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

/// We implement priority groups using Weighted Fair Queueing.
/// https://en.wikipedia.org/wiki/Weighted_fair_queueing
/// The idea is to assign a virtual "timestamp" when the message
/// would finish sending, and each time we have an opportunity to
/// send, we select the group with the earliest finish time.
/// Virtual time is essentially an arbitrary counter that increases
/// at a fixed rate per outbound byte sent.
typedef int64 VirtualSendTime;
static constexpr VirtualSendTime k_virtSendTime_Infinite = std::numeric_limits<VirtualSendTime>::max();

/// Actual implementation of SteamNetworkingMessage_t, which is the API
/// visible type.  Has extra fields needed to put the message into intrusive
/// linked lists.
class CSteamNetworkingMessage : public SteamNetworkingMessage_t
{
public:
	STEAMNETWORKINGSOCKETS_DECLARE_CLASS_OPERATOR_NEW
	static CSteamNetworkingMessage *New( uint32 cbSize );
	static void DefaultFreeData( SteamNetworkingMessage_t *pMsg );

	/// OK to delay sending this message until this time.  Set to zero to explicitly force
	/// Nagle timer to expire and send now (but this should behave the same as if the
	/// timer < usecNow).  If the timer is cleared, then all messages with lower message numbers
	/// are also cleared.
	// NOTE: Intentionally reusing the m_usecTimeReceived field, which is not used on outbound messages
	inline SteamNetworkingMicroseconds SNPSend_UsecNagle() const { return m_usecTimeReceived; }
	inline void SNPSend_SetUsecNagle( SteamNetworkingMicroseconds x ) { m_usecTimeReceived = x; }

	/// "Virtual finish time".  This is the "virtual time" when we
	/// wound have finished sending the current message, if any,
	/// if all priority groups were busy and we got our proportionate
	/// share.
	inline VirtualSendTime SNPSend_VirtualFinishTime() const { return m_nConnUserData; }
	inline void SNPSend_SetVirtualFinishTime( VirtualSendTime x ) { m_nConnUserData = x; }

	/// Offset in reliable stream of the header byte.  0 if we're not reliable.
	inline int SNPSend_ReliableStreamSize() const
	{
		DbgAssert( m_nFlags & k_nSteamNetworkingSend_Reliable && m_nConnUserData > 0 && ReliableSendInfo().m_cbHdr > 0 && m_cbSize >= ReliableSendInfo().m_cbHdr );
		return m_cbSize;
	}

	inline bool SNPSend_IsReliable() const
	{
		if ( m_nFlags & k_nSteamNetworkingSend_Reliable )
		{
			DbgAssert( ((ReliableSendInfo_t *)&m_identityPeer)->m_nStreamPos > 0 && m_cbSize >= ((ReliableSendInfo_t *)&m_identityPeer)->m_cbHdr );
			return true;
		}
		return false;
	}

	inline int64 SNPSend_ReliableStreamPos() const { Assert( m_nFlags & k_nSteamNetworkingSend_Reliable ); return ReliableSendInfo().m_nStreamPos; }
	inline void SNPSend_SetReliableStreamPos( int64 x ) { Assert( m_nFlags & k_nSteamNetworkingSend_Reliable ); ReliableSendInfo().m_nStreamPos = x; }

	// Working data for reliable messages.
	struct ReliableSendInfo_t
	{
		int64 m_nStreamPos;

		// Number of reliable segments that refer to this message.
		// Also while we are in the queue waiting to be sent the queue holds a reference
		int m_nSentReliableSegRefCount;
		int m_cbHdr;
		byte m_hdr[16];
	};
	const ReliableSendInfo_t &ReliableSendInfo() const
	{
		DbgAssert( m_nFlags & k_nSteamNetworkingSend_Reliable );
		// !KLUDGE! Reuse the identity field.  We don't actually put this in a union because
		//          this is internal stuff that doesn't need to be exposed in the API
		COMPILE_TIME_ASSERT( sizeof(m_identityPeer) >= sizeof(ReliableSendInfo_t) );
		return *(ReliableSendInfo_t *)&m_identityPeer;
	}
	ReliableSendInfo_t &ReliableSendInfo()
	{
		DbgAssert( m_nFlags & k_nSteamNetworkingSend_Reliable );
		COMPILE_TIME_ASSERT( sizeof(m_identityPeer) >= sizeof(ReliableSendInfo_t) );
		return *(ReliableSendInfo_t *)&m_identityPeer;
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
	LockDebugInfo *m_pRequiredLock = nullptr; // Is there a lock that is required to be held while we access this queue?
	int m_nMessageCount = 0;
	int m_nMessageSize = 0;


	inline bool empty() const
	{
		if ( m_pFirst )
		{
			Assert( m_pLast );
			Assert( m_nMessageCount > 0 );
			return false;
		}
		Assert( !m_pLast );
		Assert( m_nMessageCount == 0 );
		Assert( m_nMessageSize == 0 );
		return true;
	}

	/// Remove the first messages out of the queue (up to nMaxMessages).  Returns the number returned
	int RemoveMessages( SteamNetworkingMessage_t **ppOutMessages, int nMaxMessages );

	/// Delete all queued messages
	void PurgeMessages();

	/// Check the lock is held, if appropriate
	void AssertLockHeld() const;
};

/// Max number of tokens we are allowed to store up in reserve, for a burst.
const float k_flSendRateBurstOverageAllowance = k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend;

/// Describe a reliable segment that was placed in a packet
#pragma pack(push, 1)
struct SNPSendReliableSegment_t
{

	/// Message that backs the segment.  Although our wire format can accommodate
	/// a segment that spans messages, in our internal bookkeeping we do not do this.
	/// All segments we track come from a single message.
	CSteamNetworkingMessage *m_pMsg;

	/// Offset from the start of the message's position in the reliable stream
	/// where this segment begins
	int m_nOffset;

	/// Size of the segment
	int m_cbSize;

	/// Number of references to this
	int16 m_nRefCount;

	/// Status of this segment.  Either one of the k_nStatus_xxx constants
	/// below, or we are scheduled for retry, in which case it is a handle into
	/// m_listReadyRetryReliableRange
	uint16 m_hStatusOrRetry;

	static constexpr uint16 k_nStatus_InFlight = 0xffff;
	static constexpr uint16 k_nStatus_Acked = 0xfffe;

	int64 begin() const { return m_pMsg->SNPSend_ReliableStreamPos() + m_nOffset; }

	// CUtlLinkedList will use two words here for the prev/next links
};
#pragma pack(pop)

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

	/// Reliable segments that were sent.  We might have multiple of
	/// these either due to multiple lanes or retransmission.
	/// Each entry is a handle into m_listSentReliableSegments
	vstd::small_vector<uint16,2> m_vecReliableSegments;
};

/// Info used by a sender to estimate the available bandwidth
struct SSendRateData
{
	/// Current sending rate in bytes per second, RFC 3448 4.2 states default
	/// is one packet per second, but that is insane and we're not doing that.
	/// In most cases we will set a default based on initial ping, so this is
	/// only rarely used.
	int m_nCurrentSendRateEstimate = 64*1024;

	/// Actual send rate we are going to USE.  This depends on the send rate estimate
	/// and the current BBR state
	float m_flCurrentSendRateUsed = 64*1024;

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

	/// Calculate time until we could send our next packet, checking our token
	/// bucket and the current send rate
	SteamNetworkingMicroseconds CalcTimeUntilNextSend() const
	{
		// Do we have tokens to burn right now?
		if ( m_flTokenBucket >= 0.0f )
			return 0;

		return SteamNetworkingMicroseconds( m_flTokenBucket * -1e6f / m_flCurrentSendRateUsed ) + 1; // +1 to make sure that if we don't have any tokens, we never return 0, since zero means "ready right now"
	}
};

/// Track the state of a host, in its role as a sender
struct SSNPSenderState
{
	SSNPSenderState();
	~SSNPSenderState() {
		Shutdown();
	}
	void Shutdown();

	/// Constant conversion factor for bytes -> virtual time.  This
	/// represents the amount of virtual time that will elapse when
	/// ALL lanes within a given priority class send one byte.  The
	/// large value is used to avoid precision loss when dealing
	/// in integral values.
	static constexpr float k_flVirtalTimePerByteAllLanes = 65536.0f;

	/// Each priority classes has its own virtual timer for weighted fair queuing
	struct PriorityClass
	{

		/// Current virtual timestamp.  This is used when a message
		/// is queued to calculate its virtual finish time.
		VirtualSendTime m_virtTimeCurrent = 0;

		/// Indices of lanes that belong to this priority class
		vstd::small_vector<int,4<STEAMNETWORKINGSOCKETS_MAX_LANES ? 4 : STEAMNETWORKINGSOCKETS_MAX_LANES> m_vecLaneIdx;
	};
	vstd::small_vector<PriorityClass,4<STEAMNETWORKINGSOCKETS_MAX_LANES ? 4 : STEAMNETWORKINGSOCKETS_MAX_LANES> m_vecPriorityClasses;

	/// Info we track for each lane
	struct Lane
	{
		inline ~Lane() { Assert( m_messagesQueued.empty() ); }

		/// List of messages for this priority group that we have not yet
		/// finished putting on the wire the first time. The Nagle timer may
		/// be active on one or more, but if so, it is only on messages at the
		/// END of the list.  The first message may have been partially sent.
		///
		/// We use the CSteamNetworkingMessage::m_linksSecondaryQueue
		SteamNetworkingMessageQueue m_messagesQueued;

		/// Current "write" position in the reliable stream for this lane.
		/// The next reliable message will begin at this address.
		int64 m_nReliableStreamNextSendPos = 1;

		/// Last message number we sent on this channel
		int64 m_nLastSendMsgNumReliable = 0;

		// Current message number, we ++ when adding a message
		int64 m_nLastSentMsgNum = 0; // Will increment to 1 with first message

		/// How many bytes into the first message in the queue have we put on the wire?
		int m_cbCurrentSendMessageSent = 0;

		// Amount of buffered data in this lane
		int m_cbPendingUnreliable = 0;
		int m_cbPendingReliable = 0;
		int m_cbSentUnackedReliable = 0;
		inline int PendingBytesTotal() const { return m_cbPendingUnreliable + m_cbPendingReliable; }

		/// Multiplier used to calculate virtual finish time.
		float m_flBytesToVirtualTime;

		/// Index of the priority class we belong to.  Priority classes
		/// are sorted, and so a lower m_idxPriorityClass means lower priority.
		uint16 m_idxPriorityClass;

		/// Weight value they used.  This is only meaningful
		/// relative to the other lanes with the same priority class
		uint16 m_nWeight;
	};
	#if STEAMNETWORKINGSOCKETS_MAX_LANES > 4
		std_vector<Lane> m_vecLanes;
	#else
		vstd::small_vector<Lane,STEAMNETWORKINGSOCKETS_MAX_LANES> m_vecLanes;
	#endif

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

	/// Queue of all messages (from all lanes), that we have not yet
	/// finished putting on the wire the first time. The Nagle timer may be active
	/// on one or more, but if so, it is only on messages at the END of the list.
	/// The first message in each lane may have been partially sent
	SteamNetworkingMessageQueue m_messagesQueued;

	/// List of reliable messages that have been fully placed on the wire at least once,
	/// but we're hanging onto because of the potential need to retry.
	SteamNetworkingMessageQueue m_unackedReliableMessages;

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

	/// Reliable segments that have been sent at least once and either
	/// have not yet been acked, or have references by in-flight packets
	CUtlLinkedList<SNPSendReliableSegment_t, uint16> m_listSentReliableSegments;

	/// Queue of reliable ranges that have been scheduled for retry.
	/// Each entry is a handle into m_listSentReliableSegments
	CUtlLinkedList<uint16,uint16> m_listReadyRetryReliableRange;

	/// Called when we discard a packet after having nacked it
	void RemoveRefCountReliableSegment( uint16 hSeg );

	/// Called when a reliable segment is scheduled for retry, and we need to un-schedule it
	void RemoveReliableSegmentFromRetryList( uint16 hSeg, uint16 nNewStatus )
	{
		SNPSendReliableSegment_t &seg = m_listSentReliableSegments[ hSeg ];
		DbgAssert( seg.m_hStatusOrRetry < 0xfffe );
		DbgAssert( m_listReadyRetryReliableRange[ seg.m_hStatusOrRetry ] == hSeg );
		m_listReadyRetryReliableRange.Remove( seg.m_hStatusOrRetry );
		seg.m_hStatusOrRetry = nNewStatus;
	}

	/// Oldest packet sequence number that we are still asking peer
	/// to send acks for.
	int64 m_nMinPktWaitingOnAck = 0;

	/// Check invariants in debug.
	#if STEAMNETWORKINGSOCKETS_SNP_PARANOIA == 0 
		inline void DebugCheckInFlightPacketMap() const {}
		inline void DebugCheckReliable() const {}
	#else
		void DebugCheckInFlightPacketMap() const;
		void DebugCheckReliable() const;
	#endif
	#if STEAMNETWORKINGSOCKETS_SNP_PARANOIA > 1
		inline void MaybeCheckInFlightPacketMap() const { DebugCheckInFlightPacketMap(); }
		inline void MaybeCheckReliable() const { DebugCheckReliable(); }
	#else
		inline void MaybeCheckInFlightPacketMap() const {}
		inline void MaybeCheckReliable() const {}
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
	int64 m_nEnd; // just after the last packet in the gap.  This is the first in a block of packets that was received.
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

	/// Each lane has its own reliable stream
	struct Lane
	{

		/// Unreliable message segments that we have received.  When an unreliable message
		/// needs to be fragmented, we store the pieces here.  NOTE: it might be more efficient
		/// to use a simpler container, with worse O(), since this should ordinarily be
		/// a pretty small list.
		std_map<SSNPRecvUnreliableSegmentKey,SSNPRecvUnreliableSegmentData> m_mapUnreliableSegments;

		/// The highest message number we have seen so far.
		int64 m_nHighestSeenMsgNum = 0;

		/// Stream position of the first byte in m_bufReliableData.  Remember that the first byte
		/// in the reliable stream is actually at position 1, not 0
		int64 m_nReliableStreamPos = 1;

		/// The message number of the most recently received reliable message
		int64 m_nLastRecvReliableMsgNum = 0;

		/// Reliable data stream that we have received but not yet
		/// parsed as reliable messages and dispatched them.  This
		/// might have gaps in it!
		std_vector<byte> m_bufReliableStream;

		/// Gaps in the reliable data.  These are created when we receive reliable data that
		/// is beyond what we expect next.  Since these must never overlap, we store them
		/// using begin as the key and end as the value.
		///
		/// !SPEED! We should probably use a small fixed-sized, sorted vector here,
		/// since in most cases the list will be small, and the cost of dynamic memory
		/// allocation will be way worse than O(n) insertion/removal.
		std_map<int64,int64> m_mapReliableStreamGaps;
	};
	#if STEAMNETWORKINGSOCKETS_MAX_LANES > 4
		std_vector<Lane> m_vecLanes;
	#else
		vstd::small_vector<Lane,STEAMNETWORKINGSOCKETS_MAX_LANES> m_vecLanes;
	#endif

	/// List of gaps in the packet sequence numbers we have received.
	/// Since these must never overlap, we store them using begin as the
	/// key and the end in the value.
	///
	/// We always have at least one item in the list.  The last item is
	/// a special sentinel:
	/// - The key (the "begin" packet number) is one
	///   higher than the highest packet number that we want to ack.
	///   (Important: this might not be the highest packet number we have
	///   received.  Sometimes we choose to not ack packets, as a mitigation
	///   against malicious senders blowing up our data structures.)
	/// - m_usecWhenReceivedPktBefore tracks the time when we received this
	///   packet.
	/// - m_usecWhenAckPrior is the time when we need to flush acks/nacks for
	///   *all* packets, including those received after the last gap (if any --
	///   INT64_MAX means nothing scheduled).  Remember, our wire
	///   protocol cannot report on packet N without also reporting
	///   on all packets numbered < N.
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
	/// will still be honored.  We will ack up to that packet number,
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
		void DebugCheckPacketGapMap() const;
	#else
		inline void DebugCheckPacketGapMap() const {}
	#endif

	/// Setup the sentinel
	void InitPacketGapMap( int64 nMaxRecvPktNum, SteamNetworkingMicroseconds usecRecvTime );

	// Stats.  FIXME - move to LinkStatsEndToEnd and track rate counters
	int64 m_nMessagesRecvReliable = 0;
	int64 m_nMessagesRecvUnreliable = 0;
};

inline void CSteamNetworkingMessage::LinkBefore( CSteamNetworkingMessage *pSuccessor, CSteamNetworkingMessage::Links CSteamNetworkingMessage::*pMbrLinks, SteamNetworkingMessageQueue *pQueue )
{
	// No successor?
	if ( !pSuccessor )
	{
		LinkToQueueTail( pMbrLinks, pQueue );
		return;
	}

	// Check lock.  We must not already be in a queue
	pQueue->AssertLockHeld();
	Assert( (this->*pMbrLinks).m_pQueue == nullptr );
	Assert( (this->*pMbrLinks).m_pPrev == nullptr );
	Assert( (this->*pMbrLinks).m_pNext == nullptr );

	// The queue cannot be empty, since it at least contains the successor
	Assert( pQueue->m_pFirst );
	Assert( pQueue->m_pLast );
	Assert( (pSuccessor->*pMbrLinks).m_pQueue == pQueue );
	Assert( pQueue->m_nMessageCount > 0 );

	CSteamNetworkingMessage *pPrev = (pSuccessor->*pMbrLinks).m_pPrev;
	if ( pPrev )
	{
		Assert( pQueue->m_pFirst != pSuccessor );
		Assert( (pPrev->*pMbrLinks).m_pNext == pSuccessor );
		Assert( (pPrev->*pMbrLinks).m_pQueue == pQueue );

		(pPrev->*pMbrLinks).m_pNext = this;
		(this->*pMbrLinks).m_pPrev = pPrev;
	}
	else
	{
		Assert( pQueue->m_pFirst == pSuccessor );
		pQueue->m_pFirst = this;
		(this->*pMbrLinks).m_pPrev = nullptr; // Should already be null, but let's slam it again anyway
	}

	// Finish up
	Assert( pQueue->m_nMessageCount > 0 );
	++pQueue->m_nMessageCount;
	Assert( m_cbSize >= 0 );
	pQueue->m_nMessageSize += m_cbSize;

	(this->*pMbrLinks).m_pQueue = pQueue;
	(this->*pMbrLinks).m_pNext = pSuccessor;
	(pSuccessor->*pMbrLinks).m_pPrev = this;
}

inline void CSteamNetworkingMessage::LinkToQueueTail( CSteamNetworkingMessage::Links CSteamNetworkingMessage::*pMbrLinks, SteamNetworkingMessageQueue *pQueue )
{
	// Check lock.  We must not already be in a queue
	pQueue->AssertLockHeld();
	Assert( (this->*pMbrLinks).m_pQueue == nullptr );
	Assert( (this->*pMbrLinks).m_pPrev == nullptr );
	Assert( (this->*pMbrLinks).m_pNext == nullptr );

	// Locate previous link that should point to us.
	// Does the queue have anything in it?
	if ( pQueue->m_pLast )
	{
		Assert( pQueue->m_pFirst );
		Assert( !(pQueue->m_pLast->*pMbrLinks).m_pNext );
		Assert( pQueue->m_nMessageCount > 0 );
		(pQueue->m_pLast->*pMbrLinks).m_pNext = this;
	}
	else
	{
		Assert( !pQueue->m_pFirst );
		Assert( pQueue->m_nMessageCount == 0 );
		pQueue->m_pFirst = this;
	}

	// Link back to the previous guy, if any
	(this->*pMbrLinks).m_pPrev = pQueue->m_pLast;

	// We're last in the list, nobody after us
	(this->*pMbrLinks).m_pNext = nullptr;
	pQueue->m_pLast = this;

	// Remember what queue we're in
	++pQueue->m_nMessageCount;
	Assert( m_cbSize >= 0 );
	pQueue->m_nMessageSize += m_cbSize;
	(this->*pMbrLinks).m_pQueue = pQueue;
}

inline void CSteamNetworkingMessage::UnlinkFromQueue( CSteamNetworkingMessage::Links CSteamNetworkingMessage::*pMbrLinks )
{
	Links &links = this->*pMbrLinks;

	// Not in a queue?
	if ( links.m_pQueue == nullptr )
	{
		Assert( links.m_pPrev == nullptr );
		Assert( links.m_pNext == nullptr );
		return;
	}
	SteamNetworkingMessageQueue &q = *links.m_pQueue;
	q.AssertLockHeld();
	Assert( q.m_nMessageCount > 0 );

	// Unlink from previous
	if ( links.m_pPrev )
	{
		Assert( q.m_pFirst != this );
		Assert( (links.m_pPrev->*pMbrLinks).m_pNext == this );
		(links.m_pPrev->*pMbrLinks).m_pNext = links.m_pNext;
	}
	else
	{
		Assert( q.m_pFirst == this );
		q.m_pFirst = links.m_pNext;
	}

	// Unlink from next
	if ( links.m_pNext )
	{
		Assert( q.m_pLast != this );
		Assert( (links.m_pNext->*pMbrLinks).m_pPrev == this );
		(links.m_pNext->*pMbrLinks).m_pPrev = links.m_pPrev;
	}
	else
	{
		Assert( q.m_pLast == this );
		q.m_pLast = links.m_pPrev;
	}
	--q.m_nMessageCount;
	Assert( m_cbSize >= 0 );
	Assert( q.m_nMessageSize >= m_cbSize );
	q.m_nMessageSize -= m_cbSize;
	// Clear links
	links.m_pQueue = nullptr;
	links.m_pPrev = nullptr;
	links.m_pNext = nullptr;
}

} // SteamNetworkingSocketsLib
