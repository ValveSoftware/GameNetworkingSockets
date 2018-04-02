//====== Copyright Valve Corporation, All rights reserved. ====================

#pragma once

#include <tier0/memdbgoff.h>
#include <deque>
#include <tier0/memdbgon.h>

#include "../steamnetworkingsockets_internal.h"

#include <tier1/utlvector.h>
#include <tier1/utlbuffer.h>

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

const int k_nBurstMultiplier = 4; // how much we should allow for burst, TFRC defaults to 2 but we need more

const int k_nMaxPacketsPerThink = 16;

const float k_flSteamDatagramTransportSendRateBurstOverageAllowance = (float)k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend;


///
/// Wire format structures for data packets.
///

#pragma pack(push,1)

struct SNPPacketHdr
{
	uint16 m_unRecvSeqNum;		// last packet sequence number we received
	uint16 m_unRecvMsgNum;		// Last reliable msgNum received (may not be complete)
	uint32 m_unRecvMsgAmt;  	// how many bytes we've received
};

enum ESNPPacketSegmentFlags
{
	// Only one of these is set
	kPacketSegmentFlags_Message			= 0x00, // This is a message segment
	kPacketSegmentFlags_Feedback		= 0x01, // This is a feedback segment

	// These are OR when the Message bit is set
	kPacketSegmentFlags_Reliable		= 0x02, // This is a reliable message
	kPacketSegmentFlags_End				= 0x04, // This is the last block in a message
};

struct SNPPacketSegmentType
{
	uint8 m_unFlags;
	uint16 m_unSize;
};

struct SNPPacketSegmentMessage
{
	uint16 m_unMsgNum;	// Num of this message (spans multiple segments)
	uint32 m_unOffset;	// Offset of this message segment in the stream
	// Payload follows here
};

const int SNPPacketSegmentMessageSize = sizeof( SNPPacketSegmentMessage );

COMPILE_TIME_ASSERT( SNPPacketSegmentMessageSize == 6 );

struct SNPPacketSegmentFeedback
{
	uint32  m_un_X_recv;		// The rate at which the receiver estimates that data was received in the previous round-trip time
	uint32  m_un_t_delay;		// t_delay in us between receipt of t_recvdata time and sending this feedback packet
	uint32  m_un_p;				// The receiver's current estimate of the loss event rate p, 0 - 1.0 converted to 0-UINT_MAX
};

// Make sure we could fit all of our stuff in a typical message
// FIXME This framing is totally terrible!  We need to redo this!
COMPILE_TIME_ASSERT( sizeof(SNPPacketHdr) + sizeof(SNPPacketSegmentType) + sizeof(SNPPacketSegmentMessage) + sizeof(SNPPacketSegmentFeedback) + k_cbSteamNetworkingSocketsMaxMessageNoFragment <= k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend );

#pragma pack(pop)

//----------------------------------------------------------------------------
// Connection based TFRC controllers
//----------------------------------------------------------------------------

struct SSNPBuffer
{
	SSNPBuffer *m_pNext = nullptr;
	int m_nSize = 0; // How many bytes in this chunk
	uint8 m_buf[ k_cbSteamNetworkingSocketsMaxEncryptedPayloadRecv ];
};

struct SSNPSendMessage
{
	~SSNPSendMessage()
	{
		delete [] m_pData;
	}

	SSNPSendMessage *m_pNext = nullptr; // Single linked list

	uint16 m_unMsgNum = 0; // Sequence number for this message
	int m_nSize = -1; // total size of this message
	uint8 *m_pData = nullptr; // Allocated buffer holding message
	int m_nSendPos = 0; // Where we are in the send position.  If m_sendPos == m_size its all sent and just waiting for ack
	bool m_bReliable = false; // True if this was a reliable message

	// Every time we sent a section we record the send packet number and offset
	// This is used to detect retransmit requirements as the other end sends over its received position in message stream
	struct SSendPacketEntry
	{
		SteamNetworkingMicroseconds m_usec_sentTime;
		uint16 m_unSeqNum;
		int m_nOffset;
		int m_nSentAmt;
	};
	CUtlVector< SSendPacketEntry > m_vecSendPackets;
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
	int m_n_x_recv = 0; // Received rate transmitted by receiver
	int m_n_x_calc = 0;	// Calculated rate in bytes per second
	uint32 m_un_p = 0;		// Current loss event rate (0-1) scaled to 0-UINT_MAX
	int m_n_tx_s = k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend;	// Average packet size in bytes
	SteamNetworkingMicroseconds m_usec_rto = 0; // RTO calc
	SteamNetworkingMicroseconds m_usec_nfb = 0; // Nofeedback Timer
	bool m_bSentPacketSinceNFB = false; // Set to false whenever we update nfb to detect sender idle periods

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
		m_flTokenBucket = k_flSteamDatagramTransportSendRateBurstOverageAllowance;
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
		if ( m_pSendMessages == nullptr )
			TokenBucket_Limit();
	}

	/// Limit our token bucket to the max reserve amount
	void TokenBucket_Limit()
	{
		if ( m_flTokenBucket > k_flSteamDatagramTransportSendRateBurstOverageAllowance )
			m_flTokenBucket = k_flSteamDatagramTransportSendRateBurstOverageAllowance;
	}

	/// Calculate time until we could send our next packet, checking our token
	/// bucket and the current send rate
	SteamNetworkingMicroseconds CalcTimeUntilNextSend() const
	{
		// Do we have tokens to burn right now?
		if ( m_flTokenBucket >= 0.0f )
			return 0;

		return SteamNetworkingMicroseconds( m_flTokenBucket * -1e6 / (float)m_n_x );
	}

	/// Reset Nagle timer, and move all queued messages to the send list
	void FlushNagle()
	{
		SSNPSendMessage **ppSendMessages = &m_pSendMessages;
		while ( *ppSendMessages )
		{
			ppSendMessages = &(*ppSendMessages)->m_pNext;
		}
		*ppSendMessages = m_pQueuedMessages;
		m_pQueuedMessages = nullptr;
		m_t_nagle = 0;
	}

	/* TFRC sender states */
	enum ETFRCSenderStates {
		TFRC_SSTATE_NO_SENT = 1,
		TFRC_SSTATE_NO_FBACK,
		TFRC_SSTATE_FBACK,
	};
	ETFRCSenderStates m_e_tx_state = TFRC_SSTATE_NO_SENT; // Sender state
	SteamNetworkingMicroseconds m_usec_no_feedback_timer = 0; // No feedback timer
	SteamNetworkingMicroseconds m_usec_ld = 0; // Time last doubled during slow start
	SteamNetworkingMicroseconds m_t_nagle = 0; // Nagle send timer

	// Transmit history used for calc rtt
	struct S_tx_hist_entry
	{
		uint16 m_unSeqNum; // send seq number
		SteamNetworkingMicroseconds m_usec_ts; // Time when sent
	};
	std::deque< S_tx_hist_entry > m_tx_hist;

	// Current message number, we ++ when adding a message
	uint16 m_unSendMsgNum = 0; // Will increment to 1 with first message
	uint16 m_unSendMsgNumReliable = 0; // Will increment to 1 with first message

	// Message send position and values
	SSNPSendMessage *m_pQueuedMessages = nullptr; // List of messages we are queued for nagle, this gets move to Send when nagle 
										// expire or we have enough to fill a packet
	SSNPSendMessage *m_pSendMessages = nullptr; // List of messages we are sending.  Head of the list maybe in transmission
	SSNPSendMessage *m_pSentMessages = nullptr; // Linked list of messages we sent, m_pCurrentSendMessage 
												// is placed at the end of this list entries are removed 
												// as acknowledge.  Note that a retransmission may cause 
												// entries to move from this list back to m_pCurrentSendMessage

	// Buffered data counters.  See SteamNetworkingQuickConnectionStatus for more info
	int m_cbPendingUnreliable = 0;
	int m_cbPendingReliable = 0;
	int m_cbSentUnackedReliable = 0;
	inline int PendingBytesTotal() const { return m_cbPendingUnreliable + m_cbPendingReliable; }

	uint16 m_unRecvSeqNum = USHRT_MAX; // Sequence number receiver last received

	// Receive reliable position, used to determine when we should retransmit
	uint16 m_unRecvMsgNumReliable = USHRT_MAX; // Msg number receiver last received
	uint32 m_unRecvMsgAmtReliable = 0; // How many bytes the receiver has received so far in the message

	// Last acknowledged message
	uint16 m_unLastAckMsgNumReliable = 0; // Last msg number we know was acknowledged
	uint32 m_unLastAckMsgAmtReliable = 0; // How much of the message was acknowledged

	// If this is true, we detected a lost packet in the receiver state and we need to send an
	// implied NAK at the next ipi, even if we don't have pending app data
	bool m_bPendingNAK = false;

	// Set when receiver determines we need to send a feedback packet
	/* TFRC feedback sender states */
	enum ETFRCSenderFeedbackStates {
		TFRC_SSTATE_FBACK_NONE = 0, //
		TFRC_SSTATE_FBACK_REQ = 1, // Set if we should send a feedback without data (parm change)
		TFRC_SSTATE_FBACK_PERODIC = 2, // When periodic feedback is set, we piggy pack on a data packet
	};
	ETFRCSenderFeedbackStates m_sendFeedbackState = TFRC_SSTATE_FBACK_NONE;

	// Stats
	int64 m_nMessagesSentReliable = 0;
	int64 m_nMessagesSentUnreliable = 0;

	void SetNoFeedbackTimer( SteamNetworkingMicroseconds usecNow )
	{
		if ( m_usec_rto == 0 )
		{
			m_usec_nfb = usecNow + TFRC_INITIAL_TIMEOUT;
		}
		else
		{
			// Calculate inter-packet-interval ("ipi")
			SteamNetworkingMicroseconds ipi = k_nMillion * (int64)m_n_tx_s / (int64)m_n_x;

			// Expect feedback within the RTO timeout, or 2 sent packets,
			// whichever is greater
			//
			// Hm, this isn't what the RFC said to do.  I wonder where this logic came from.
			m_usec_nfb = usecNow + Max( m_usec_rto, 2 * ipi );
		}
	}
};

struct SSNPReceiverState
{
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
	int m_n_bytes_recv = 0; // Total sum of payload bytes
	int m_n_x_recv = 0; // Receiver estimate of send rate (RFC 3448, sec. 4.3)
	SteamNetworkingMicroseconds m_usec_tstamp_last_feedback = 0; // Time at which last feedback was sent
	int m_n_rx_s = 0;	// Average packet size in bytes
	SteamNetworkingMicroseconds m_usec_next_feedback = 0; // Time we should send next feedback,
														  // its usually now + rtt, but can be set earlier 
														  // if a param change happense.


	struct S_rx_hist
	{
		SteamNetworkingMicroseconds m_usecPing;	// estimated rtt at the time we received the packet
		SteamNetworkingMicroseconds m_usec_recvTs; // Time we received the packet
		uint16 m_unRecvSeqNum; // Sequence number of this packet
	};

	std::deque< S_rx_hist > m_queue_rx_hist; // History of received packets, kept to TFRC_NDUPACK - 1 entries

	struct S_lh_hist
	{
		SteamNetworkingMicroseconds m_ts; // Estimated time we lost this packet
		uint16 m_un_seqno; // Highest received seqno before the start of loss
		uint16 m_un_length; // Loss interval sequence length
		bool m_b_is_closed; // Whether seqno is older than 1 RTT
	};

	//CUtlQueueFixed< S_lh_hist, LIH_SIZE > m_vec_li_hist;
	std::deque< S_lh_hist > m_vec_li_hist;
	uint32 m_li_i_mean = 0; // Current Average Loss Interval [RFC 3448, 5.4] scaled to 0-UINT_MAX

	uint16 m_unRecvMsgNum = 1; // Expected receiving message, incremented when we are fully received
	uint16 m_unRecvMsgNumReliable = 1; // Expected receiving message, incremented when we are fully received

	// These are placed in the outgoing packets so the other end can determine retransmit
	uint16 m_unLastReliableRecvMsgNum = 0; // MsgNum of the last message chunk we received
	uint32 m_unLastReliabeRecvMsgAmt = 0; // How much we've received so far

	CUtlBuffer m_recvBuf;
	CUtlBuffer m_recvBufReliable;

	// Stats
	int64 m_nMessagesRecvReliable = 0;
	int64 m_nMessagesRecvUnreliable = 0;
};

} // SteamNetworkingSocketsLib
