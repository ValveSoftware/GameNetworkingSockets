//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_snp.h"
#include "steamnetworkingsockets_connections.h"

#include "steamnetworkingconfig.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace SteamNetworkingSocketsLib {

// exponentially weighted moving average
template< typename T > T tfrc_ewma( T avg, T newval, T weight )
{
	return avg ? ( weight * avg + ( 10 - weight ) * newval ) / 10 : newval;
}

int TFRCCalcX( int s, SteamNetworkingMicroseconds rtt, float p )
{
	// TFRC throughput equation
	// 
	//                                s
	//   X_Bps = ----------------------------------------------------------
	//           R*sqrt(2*b*p/3) + (t_RTO * (3*sqrt(3*b*p/8)*p*(1+32*p^2)))
	//
	// b is TCP acknowlege packet rate, assumed to be 1 for this implementation

	float R = (double)rtt / k_nMillion;
	float t_RTO = MAX( 4 * R, 1.0f );

	return static_cast< int >( static_cast<float>( s ) /
		( R * sqrt( 2 * p / 3 ) + ( t_RTO * ( 3 * sqrt( 3 * p / 8 ) * p * ( 1 + 32 * ( p * p ) ) ) ) ) );
}

/**
 * To address sequence number wrapping, let S_MAX = 2^b, where b
 * is the bit-length of sequence numbers in a given 
 * implementation.  Dist can handle wrap like so: 
 *  
 * Dist(S_After, S_Before) = (S_After + S_MAX - S_Before) % 
 * S_MAX 
 */
static inline int SeqDist( uint16 after, uint16 before )
{
	uint16 nGap = after - before;
	return nGap;
}

static inline bool IsSeqAfterOrEq( uint16 after, uint16 before )
{
	// We assume that as long as we haven't wrapped more than half way
	// (32768 packets) then if its greater than that its before (negative)
	return SeqDist( after, before ) < USHRT_MAX/2;
}

static inline bool IsSeqAfter( uint16 after, uint16 before )
{
	// We assume that as long as we haven't wrapped more than half way
	// (32768 packets) then if its greater than that its before (negative)
	int sDist = SeqDist( after, before );
	return sDist > 0 && sDist < USHRT_MAX/2;
}

// Fetch ping, and handle two edge cases:
// - if we don't have an estimate, just be relatively conservative
// - clamp to minimum
inline SteamNetworkingMicroseconds GetUsecPingWithFallback( CSteamNetworkConnectionBase *pConnection )
{
	int nPingMS = pConnection->m_statsEndToEnd.m_ping.m_nSmoothedPing;
	if ( nPingMS < 0 )
		return 200*1000; // no estimate, just be conservative
	if ( nPingMS < 1 )
		return 500; // less than 1ms.  Make sure we don't blow up, though, since they are asking for microsecond resolution.  We should just keep our pings with microsecond resolution!
	return nPingMS*1000;
}

/*
* Compute the initial sending rate X_init in the manner of RFC 3390:
*
*	X_init  =  min(4 * s, max(2 * s, 4380 bytes)) / RTT
*
* Note that RFC 3390 uses MSS, RFC 4342 refers to RFC 3390, and rfc3448bis
* (rev-02) clarifies the use of RFC 3390 with regard to the above formula.
*/
static int GetInitialRate( SteamNetworkingMicroseconds usecPing )
{
	Assert( usecPing > 0 );
	int64 w_init = Clamp( 4380, 2 * k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend, 4 * k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend );
	int rate = int( k_nMillion * w_init / usecPing );

	return Max( steamdatagram_snp_min_rate, rate );
}

//-----------------------------------------------------------------------------
void CSteamNetworkConnectionBase::SNP_InitializeConnection( SteamNetworkingMicroseconds usecNow )
{
	m_senderState.TokenBucket_Init( usecNow );

	m_senderState.m_usec_nfb = usecNow + TFRC_INITIAL_TIMEOUT;
	m_senderState.m_bSentPacketSinceNFB = false;

	SteamNetworkingMicroseconds usecPing = GetUsecPingWithFallback( this );
	m_senderState.m_n_x = GetInitialRate( usecPing );

	if ( steamdatagram_snp_log_x )
	SpewMsg( "%12llu %s: INITIAL X=%d rtt=%dms tx_s=%d\n", 
				 usecNow,
				 m_sName.c_str(),
				 m_senderState.m_n_x,
				 m_statsEndToEnd.m_ping.m_nSmoothedPing,
				 m_senderState.m_n_tx_s );

	m_receiverState.m_usec_tstamp_last_feedback = usecNow;

	// Recalc send now that we have rtt
	SNP_UpdateX( usecNow );
}

//-----------------------------------------------------------------------------
EResult CSteamNetworkConnectionBase::SNP_SendMessage( SteamNetworkingMicroseconds usecNow, const void *pData, uint32 cbData, ESteamNetworkingSendType eSendType )
{
	// Check if we're full
	if ( m_senderState.PendingBytesTotal() + (int)cbData > steamdatagram_snp_send_buffer_size )
	{
		SpewWarning( "Connection already has %u bytes pending, cannot queue any more messages\n", m_senderState.PendingBytesTotal() );
		return k_EResultLimitExceeded; 
	}

	if ( eSendType & k_nSteamNetworkingSendFlags_NoDelay )
	{
		// FIXME - need to check how much data is currently pending, and return
		// k_EResultIgnored if we think it's going to be a while before this
		// packet goes on the wire.
	}

	// First, accumulate tokens, and also limit to reasonable burst
	// if we weren't already waiting to send
	m_senderState.TokenBucket_Accumulate( usecNow );

	// Add to the send queue
	SSNPSendMessage *pSendMessage = new SSNPSendMessage();

	pSendMessage->m_pData = new uint8[ cbData ];
	pSendMessage->m_nSize = cbData;
	memcpy( pSendMessage->m_pData, pData, cbData );

	// Allocate the message number
	pSendMessage->m_bReliable = ( eSendType & k_nSteamNetworkingSendFlags_Reliable ) != 0;
	pSendMessage->m_unMsgNum = pSendMessage->m_bReliable ? ++m_senderState.m_unSendMsgNumReliable : ++m_senderState.m_unSendMsgNum;

	// Add it to the end of message queue
	SSNPSendMessage **ppQueuedMessages = &m_senderState.m_pQueuedMessages;
	while ( *ppQueuedMessages )
	{
		ppQueuedMessages = &(*ppQueuedMessages)->m_pNext;
	}
	*ppQueuedMessages = pSendMessage;

	if ( pSendMessage->m_bReliable )
	{
		++m_senderState.m_nMessagesSentReliable;
		m_senderState.m_cbPendingReliable += pSendMessage->m_nSize;
	}
	else
	{
		++m_senderState.m_nMessagesSentUnreliable;
		m_senderState.m_cbPendingUnreliable += pSendMessage->m_nSize;
	}

	if ( steamdatagram_snp_log_message )
	SpewMsg( "%12llu %s: SendMessage %s: MsgNum=%d sz=%d\n",
				 usecNow,
				 m_sName.c_str(),
				 pSendMessage->m_bReliable ? "RELIABLE" : "UNRELIABLE",
				 pSendMessage->m_unMsgNum,
				 pSendMessage->m_nSize );

	// Start nagle timer if needed
	if ( m_senderState.PendingBytesTotal() >= k_cbSteamNetworkingSocketsMaxMessageNoFragment )
	{
		// FIXME - This is actually not quite right.  We want to send out any partially-filled
		// packets, but if this leaves a partially filed packet, we don't want to flush that out.
		// We just want to send out any full packets.  But this code will fix the really bad
		// perf if somebody does something dumb and sends tiny packets, and that's the main
		// point of Nagle, so I guess that's OK.
		if ( steamdatagram_snp_log_nagle )
		SpewMsg( "%12llu %s: NAGLE cleared nagle timer because pendingBytes %d > %d\n", 
					 usecNow,
					 m_sName.c_str(),
					 m_senderState.PendingBytesTotal(),
					 k_cbSteamNetworkingSocketsMaxMessageNoFragment );
		m_senderState.FlushNagle();
	}
	else if ( eSendType & k_nSteamNetworkingSendFlags_NoNagle )
	{
		// More than a packets worth of pending data, send now
		if ( steamdatagram_snp_log_nagle )
		SpewMsg( "%12llu %s: NAGLE cleared nagle timer because message was sent with type %d\n", 
					 usecNow,
					 m_sName.c_str(),
					 eSendType );
		m_senderState.FlushNagle();
	}
	else if ( !m_senderState.m_t_nagle && steamdatagram_snp_nagle_time )
	{
		m_senderState.m_t_nagle = usecNow + steamdatagram_snp_nagle_time;
		if ( steamdatagram_snp_log_nagle )
		SpewMsg( "%12llu %s: NAGLE SET to %llu (%d delay)\n", 
				 usecNow,
				 m_sName.c_str(),
				 m_senderState.m_t_nagle,
				 steamdatagram_snp_nagle_time );
	}

	// Schedule wakeup at the appropriate time.  (E.g. right now, if we're ready to send, or at the Nagle time, if Nagle is active.)
	SteamNetworkingMicroseconds usecNextThink = SNP_GetNextThinkTime( usecNow );

	// If we are rate limiting, spew about it
	if ( m_senderState.m_pSendMessages && usecNextThink > usecNow )
	{
		SpewVerbose( "%12llu %s: RATELIM QueueTime is %.1fms, SendRate=%.1fk, BytesQueued=%d\n", 
			usecNow,
			m_sName.c_str(),
			m_senderState.CalcTimeUntilNextSend() * 1e-3,
			m_senderState.m_n_x * ( 1.0/1024.0),
			m_senderState.PendingBytesTotal()
		);
	}

	EnsureMinThinkTime( usecNextThink, +1 );

	return k_EResultOK;
}

EResult CSteamNetworkConnectionBase::SNP_FlushMessage( SteamNetworkingMicroseconds usecNow )
{
	if ( m_senderState.m_pQueuedMessages == nullptr )
		return k_EResultOK;

	// Clear nagle
	if ( steamdatagram_snp_log_nagle )
	SpewMsg( "%12llu %s: NAGLE FlushMessasge\n", 
			 usecNow,
			 m_sName.c_str() );

	m_senderState.FlushNagle();

	// Schedule wakeup at the appropriate time.  (E.g. right now, if we're ready to send.)
	SteamNetworkingMicroseconds usecNextThink = SNP_GetNextThinkTime( usecNow );
	EnsureMinThinkTime( usecNextThink, +1 );
	return k_EResultOK;
}


void CSteamNetworkConnectionBase::SNP_MoveSentToSend( SteamNetworkingMicroseconds usecNow )
{
	// Called when we should move all of the messages in m_pSentMessages back into m_pSendMessages so they will be retransmitted,
	// note we have to insert them back in the correct order
	if ( !m_senderState.m_pSentMessages )
	{
		// In this case we are reseting the send msg
		if ( m_senderState.m_pSendMessages )
		{
			SSNPSendMessage *pSendMsg = m_senderState.m_pSendMessages;
			if ( pSendMsg->m_bReliable )
			{
				if ( !pSendMsg->m_vecSendPackets.IsEmpty() )
				{
					pSendMsg->m_nSendPos = pSendMsg->m_vecSendPackets[ 0 ].m_nOffset;
					pSendMsg->m_vecSendPackets.RemoveAll();
				}
				else
				{
					pSendMsg->m_nSendPos = 0;
				}
			}
		}
	}
	else
	{
		// Get pointer to last sent message
		SSNPSendMessage **ppSentMsg = &m_senderState.m_pSentMessages;
		while ( *ppSentMsg )
		{
			ppSentMsg = &(*ppSentMsg)->m_pNext;
		}

		// Have pointer to end of list, first up set position of first message and clear position of succeeding
		SSNPSendMessage *pSentMsg = m_senderState.m_pSentMessages;
		if ( pSentMsg->m_bReliable )
		{
			if ( !pSentMsg->m_vecSendPackets.IsEmpty() )
			{
				pSentMsg->m_nSendPos = pSentMsg->m_vecSendPackets[ 0 ].m_nOffset;
				pSentMsg->m_vecSendPackets.RemoveAll();
			}
			else
			{
				pSentMsg->m_nSendPos = 0;
			}
		}

		// any messages afterward are full re-trans
		for ( pSentMsg = pSentMsg->m_pNext; pSentMsg; pSentMsg = pSentMsg->m_pNext )
		{
			if ( pSentMsg->m_bReliable )
			{
				pSentMsg->m_vecSendPackets.RemoveAll();
				pSentMsg->m_nSendPos = 0;
			}
		}

		// reset current send msg
		if ( m_senderState.m_pSendMessages && m_senderState.m_pSendMessages->m_bReliable )
		{
			SSNPSendMessage *pSendMsg = m_senderState.m_pSendMessages;
			pSendMsg->m_vecSendPackets.RemoveAll();
			pSendMsg->m_nSendPos = 0;
		}

		// Push Sent to head of Send
		*ppSentMsg = m_senderState.m_pSendMessages;
		m_senderState.m_pSendMessages = m_senderState.m_pSentMessages;
		m_senderState.m_pSentMessages = nullptr;
	}

	// recalc queued.
	// UG - do we really need to do this?  This could be slow.  Can't we maintain this data
	// as we go?
	m_senderState.m_cbSentUnackedReliable = 0; // ???? This whole function is a giant mess.
	m_senderState.m_cbPendingUnreliable = 0;
	m_senderState.m_cbPendingReliable = 0;
	for ( SSNPSendMessage *pSendMessage = m_senderState.m_pSendMessages; pSendMessage; pSendMessage = pSendMessage->m_pNext )
	{
		int cbPending = pSendMessage->m_nSize - pSendMessage->m_nSendPos;
		if ( pSendMessage->m_bReliable )
			m_senderState.m_cbPendingReliable += cbPending;
		else
			m_senderState.m_cbPendingUnreliable += cbPending;
	}
	for ( SSNPSendMessage *pQueuedMessage = m_senderState.m_pQueuedMessages; pQueuedMessage; pQueuedMessage = pQueuedMessage->m_pNext )
	{
		int cbPending = pQueuedMessage->m_nSize;
		if ( pQueuedMessage->m_bReliable )
			m_senderState.m_cbPendingReliable += cbPending;
		else
			m_senderState.m_cbPendingUnreliable += cbPending;
	}

}

void CSteamNetworkConnectionBase::SNP_CheckForReliable( SteamNetworkingMicroseconds usecNow )
{
	// We start with Sent and then move to Send
	// See if any messages that are waiting for acknowledgement are ready to go
	SSNPSendMessage *pMsg = m_senderState.m_pSentMessages;
	if ( !pMsg )
	{
		// If nothing waiting ack in sent, check current send message
		pMsg = m_senderState.m_pSendMessages;
	}

	// Clear and move to next Sent message function
	auto NextMsg = [&] 
	{
		// If we are at the first send message stop since its still being sent
		if ( pMsg == m_senderState.m_pSendMessages )
		{
			pMsg = nullptr;
		}
		else
		{
			Assert( pMsg->m_bReliable );
			m_senderState.m_pSentMessages = m_senderState.m_pSentMessages->m_pNext;
			Assert( m_senderState.m_cbSentUnackedReliable >= pMsg->m_nSize );
			m_senderState.m_cbSentUnackedReliable -= pMsg->m_nSize;
			delete pMsg;
			pMsg = m_senderState.m_pSentMessages;
			if ( !pMsg )
			{
				pMsg = m_senderState.m_pSendMessages;
			}
		}
	};

	while ( pMsg )
	{
		if ( pMsg->m_vecSendPackets.IsEmpty() )
		{
			NextMsg();
			continue;
		}

		if ( steamdatagram_snp_log_reliable )
		SpewMsg( "%12llu %s: %s CheckForReliable: sentMsgNum=%d, recvMsgNum=%d, recvSeqNum=%d, sendSeqNum=%d sendSeqOffset=%d\n",
					 usecNow,
					 m_sName.c_str(),
					 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
					 pMsg->m_unMsgNum,
					 m_senderState.m_unRecvMsgNumReliable,
					 m_senderState.m_unRecvSeqNum,
					 pMsg->m_vecSendPackets.Head().m_unSeqNum,
					 pMsg->m_vecSendPackets.Head().m_nOffset );

		// If the acknowledge message number is after this one, its received
		if ( IsSeqAfter( m_senderState.m_unRecvMsgNumReliable, pMsg->m_unMsgNum ) )
		{
			if ( steamdatagram_snp_log_reliable )
			SpewMsg( "%12llu %s: %s ACK recvMsgNum %d is after sentMsgNum %d, acknowledged\n", 
						 usecNow,
						 m_sName.c_str(),
						 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
						 m_senderState.m_unRecvMsgNumReliable, 
						 pMsg->m_unMsgNum );

			// Next
			NextMsg();
			continue;
		}

		// If the message number doesn't match current, we're waiting for ack
		if ( m_senderState.m_unRecvMsgNumReliable != pMsg->m_unMsgNum )
		{
			if ( !pMsg->m_vecSendPackets.IsEmpty() )
			{
				SSNPSendMessage::SSendPacketEntry &sendPacketEntry = pMsg->m_vecSendPackets.Head();

				// The other end might have lost the first packet of the current message, check if they are still
				// ackowledging the previous message but the seqNum is higher
				if ( m_senderState.m_unLastAckMsgNumReliable == m_senderState.m_unRecvMsgNumReliable &&
					 m_senderState.m_unLastAckMsgAmtReliable == m_senderState.m_unRecvMsgAmtReliable &&
					 IsSeqAfterOrEq( m_senderState.m_unRecvSeqNum, sendPacketEntry.m_unSeqNum ) )
				{
					// They received the packet of this message (or after), but didn't get the first section
					// we need to resend

					// Should only occur on the first section
					Assert( sendPacketEntry.m_nOffset == 0 );
					if ( steamdatagram_snp_log_reliable )
					SpewMsg( "%12llu %s: %s NAK sentMsgNum %d: recvSeqNum %d is GTE sentSeqNum %d, but ack is previous msg %d:%u\n", 
								 usecNow,
								 m_sName.c_str(),
								 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
								 pMsg->m_unMsgNum,
								 m_senderState.m_unRecvSeqNum, 
								 sendPacketEntry.m_unSeqNum, 
								 m_senderState.m_unRecvMsgNumReliable, 
								 m_senderState.m_unRecvMsgAmtReliable );

					SNP_MoveSentToSend( usecNow );
					return; // No need to check more we're going to start all over again
				}
			}

			if ( steamdatagram_snp_log_reliable )
			SpewMsg( "%12llu %s: %s recvMsgNum %d != sentMsgNum %d, lastAck %d:%u\n", 
					 usecNow,
					 m_sName.c_str(),
					 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
					 m_senderState.m_unRecvMsgNumReliable, 
					 pMsg->m_unMsgNum,
					 m_senderState.m_unLastAckMsgNumReliable,
					 m_senderState.m_unLastAckMsgAmtReliable );
			break; // We will check retransmit timeout below
		}

		// Pull out sent entries on acknowledgement
		while ( !pMsg->m_vecSendPackets.IsEmpty() )
		{
			SSNPSendMessage::SSendPacketEntry &sendPacketEntry = pMsg->m_vecSendPackets.Head();

			// If the received seq num is after or equal the sent, check if we've received this entry
			if ( IsSeqAfterOrEq( m_senderState.m_unRecvSeqNum, sendPacketEntry.m_unSeqNum ) )
			{
				if ( m_senderState.m_unRecvMsgAmtReliable < (uint32)sendPacketEntry.m_nSentAmt )
				{
					if ( steamdatagram_snp_log_reliable )
					SpewMsg( "%12llu %s: %s NAK sentMsgNum %d: recvSeqNum %d is GTE sentSeqNum %d, but m_unRecvMsgAmt %d is less than m_nSentAmt %d\n", 
								 usecNow,
								 m_sName.c_str(),
								 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
								 pMsg->m_unMsgNum,
								 m_senderState.m_unRecvSeqNum, 
								 sendPacketEntry.m_unSeqNum, 
								 m_senderState.m_unRecvMsgAmtReliable, 
								 sendPacketEntry.m_nSentAmt );
					SNP_MoveSentToSend( usecNow );
					return; // No need to check more we're going to start all over again
				}
				else
				{
					if ( steamdatagram_snp_log_reliable )
					SpewMsg( "%12llu %s: %s ACK sentMsgNum %d: recvSeqNum %d is GTE sentSeqNum %d, m_unRecvMsgAmt %d is GTE than m_nSentAmt %d\n", 
								 usecNow,
								 m_sName.c_str(),
								 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
								 pMsg->m_unMsgNum,
								 m_senderState.m_unRecvSeqNum, 
								 sendPacketEntry.m_unSeqNum, 
								 m_senderState.m_unRecvMsgAmtReliable, 
								 sendPacketEntry.m_nSentAmt );
					// This portion is acknowledged
					pMsg->m_vecSendPackets.Remove( 0 );
				}
			}
			else
			{
				break; // Not received yet
			}
		}

		if ( pMsg != m_senderState.m_pSendMessages && pMsg->m_vecSendPackets.IsEmpty() )
		{
			if ( steamdatagram_snp_log_reliable )
			SpewMsg( "%12llu %s: SENT Finished sentMsgNum %d lastAck %d:%u\n", 
					 usecNow,
					 m_sName.c_str(),
					 pMsg->m_unMsgNum,
					 m_senderState.m_unRecvMsgNumReliable,
					 m_senderState.m_unRecvMsgAmtReliable );

			// Note we record this ack, we need it in case the other end misses the first section
			// of the next message and we need to double check if we need to retransmit it
			m_senderState.m_unLastAckMsgNumReliable = m_senderState.m_unRecvMsgNumReliable;
			m_senderState.m_unLastAckMsgAmtReliable = m_senderState.m_unRecvMsgAmtReliable;

			// All done!
			NextMsg();
			continue;
		}
		else
		{
			break; // still pending
		}
	}

	// If pSentMsg is not null here, we are still waiting for ack, check if we need to start sending again due to RTO
	if ( pMsg )
	{
		if ( m_senderState.m_usec_rto &&
			 !pMsg->m_vecSendPackets.IsEmpty() &&
			 pMsg->m_vecSendPackets.Head().m_usec_sentTime - usecNow > m_senderState.m_usec_rto )
		{
			if ( steamdatagram_snp_log_reliable || steamdatagram_snp_log_loss )
			SpewMsg( "%12llu %s: RTO sentMsgNum %d\n", 
						 usecNow,
						 m_sName.c_str(),
						 pMsg->m_unMsgNum );

			// Set for retransmit due to timeout
			SNP_MoveSentToSend( usecNow );
		}
	}
}

// Called when receiver wants to send feedback, sets up the next packet transmit to include feedback
void CSteamNetworkConnectionBase::SNP_PrepareFeedback( SteamNetworkingMicroseconds usecNow )
{
	// Update x_recv
	SteamNetworkingMicroseconds usec_delta = usecNow - m_receiverState.m_usec_tstamp_last_feedback;
	if ( usec_delta )
	{
		int n_x_recv = k_nMillion * m_receiverState.m_n_bytes_recv / usec_delta;
		// if higher take, otherwise smooth
		if ( n_x_recv > m_receiverState.m_n_x_recv )
		{
			m_receiverState.m_n_x_recv = n_x_recv;
		}
		else
		{
			m_receiverState.m_n_x_recv = tfrc_ewma( m_receiverState.m_n_x_recv, n_x_recv, 9 );
		}

		if ( steamdatagram_snp_log_feedback )
			SpewMsg( "%12llu %s: TFRC_FBACK_PERIODIC usec_delta=%llu bytes_recv=%d n_x_recv=%d m_n_x_recv=%d\n",
					 usecNow,
					 m_sName.c_str(),
					 usec_delta,
					 m_receiverState.m_n_bytes_recv,
					 n_x_recv,
					 m_receiverState.m_n_x_recv );
	}

	m_receiverState.m_usec_tstamp_last_feedback = usecNow;
	m_receiverState.m_usec_next_feedback = usecNow + GetUsecPingWithFallback( this );
	m_receiverState.m_n_bytes_recv = 0;
}

bool CSteamNetworkConnectionBase::SNP_CalcIMean( SteamNetworkingMicroseconds usecNow )
{
	static const float tfrc_lh_weights[NINTERVAL] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.8f, 0.6f, 0.4f, 0.2f };

	// RFC 3448, 5.4

	int i_i;
	float i_tot0 = 0, i_tot1 = 0, w_tot = 0;

	if ( m_receiverState.m_vec_li_hist.empty() )
		return false;

	int i = 0;
	for ( auto li_iterator = m_receiverState.m_vec_li_hist.rbegin() ; li_iterator != m_receiverState.m_vec_li_hist.rend() && i < NINTERVAL ; ++li_iterator )
	{
		i_i = li_iterator->m_un_length;

		i_tot0 += i_i * tfrc_lh_weights[i];
		w_tot += tfrc_lh_weights[i];

		if (i > 0)
			i_tot1 += i_i * tfrc_lh_weights[i-1];

		++i;
	}

	float f_mean = Max( i_tot0, i_tot1 ) / w_tot;

	m_receiverState.m_li_i_mean = (uint32)( ( 1.0f / f_mean ) * (float)UINT_MAX );
	return true;
}

bool CSteamNetworkConnectionBase::SNP_AddLossEvent( uint16 unSeqNum, SteamNetworkingMicroseconds usecNow )
{
	// See if this loss is a new loss
	Assert( !m_receiverState.m_queue_rx_hist.empty() );
	uint16 lastRecvSeqNum = m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum;
	uint16 firstLossSeqNum = lastRecvSeqNum + 1;
	uint16 lastLossSeqNum = unSeqNum - 1;
	Assert( SeqDist( lastLossSeqNum, firstLossSeqNum ) >= 0 );

	if ( !m_receiverState.m_vec_li_hist.empty() )
	{
		// RFC 3448, 5.2
		// 
		// For a lost packet, we can interpolate to infer the nominal "arrival time".  Assume:
		// 
		//       S_loss is the sequence number of a lost packet.
		// 
		//       S_before is the sequence number of the last packet to arrive with
		//       sequence number before S_loss.
		// 
		//       S_after is the sequence number of the first packet to arrive with
		//       sequence number after S_loss.
		// 
		//       T_before is the reception time of S_before.
		// 
		//       T_after is the reception time of S_after.
		// 
		//    Note that T_before can either be before or after T_after due to
		//    reordering.
		// 
		//    For a lost packet S_loss, we can interpolate its nominal "arrival
		//    time" at the receiver from the arrival times of S_before and S_after.
		//    Thus:
		// 
		//    T_loss = T_before + ( (T_after - T_before)
		//                * (S_loss - S_before)/(S_after - S_before) )

		SteamNetworkingMicroseconds before_ts = m_receiverState.m_queue_rx_hist.back().m_usec_recvTs;
		SteamNetworkingMicroseconds after_ts = usecNow;

		uint16 S_before = lastRecvSeqNum;
		uint16 S_after = unSeqNum;

		SteamNetworkingMicroseconds firstloss_ts = before_ts + 
			( (after_ts - before_ts ) * SeqDist( firstLossSeqNum, S_before ) / SeqDist( S_after, S_before ) );

		SteamNetworkingMicroseconds usec_rtt = m_receiverState.m_queue_rx_hist.back().m_usecPing;
		if ( firstloss_ts - m_receiverState.m_vec_li_hist.back().m_ts <= usec_rtt )
		{
			// This is in the same loss interval
			return false;
		}
	}

	// Cull if full
	if ( m_receiverState.m_vec_li_hist.size() == LIH_SIZE )
	{
		m_receiverState.m_vec_li_hist.pop_front();
	}
	
	// New loss interval
	SSNPReceiverState::S_lh_hist hist;
	hist.m_ts = usecNow;
	hist.m_un_seqno = firstLossSeqNum;
	hist.m_b_is_closed = false;
	m_receiverState.m_vec_li_hist.push_back( hist );

	auto plh_hist = m_receiverState.m_vec_li_hist.rbegin();

	// Update previous loss interval length if needed
	if ( m_receiverState.m_vec_li_hist.size() == 1 )
	{
		// First loss interval, we have to do some special handling to calc i_mean
		// via RFC 3448 6.3.1

		// We need to find the p value that results in something close to x_recv
		SteamNetworkingMicroseconds usec_rtt = m_receiverState.m_queue_rx_hist.back().m_usecPing;
		SteamNetworkingMicroseconds usec_delta = usecNow - m_receiverState.m_usec_tstamp_last_feedback;
		int n_cur_x_recv = usec_delta ? k_nMillion * m_receiverState.m_n_bytes_recv / usec_delta : 0;
		int n_x_recv = MAX( n_cur_x_recv * k_nBurstMultiplier / 2, 
							m_receiverState.m_n_x_recv * k_nBurstMultiplier / 2 );

		if ( !n_x_recv )
		{
			n_x_recv = GetInitialRate( usec_rtt );
		}

		int n_s = m_receiverState.m_n_rx_s;
		if ( !n_s )
		{
			n_s = k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend;
		}

		// Find a value of p that matches within 5% of x_recv using binary search
		int x_cur = 0;
		float cur_p = 0.5f;
		while ( 100llu * (uint64)x_cur / (uint64)n_x_recv < 95llu ) // 5% tolerance
		{
			x_cur = TFRCCalcX( m_receiverState.m_n_rx_s, usec_rtt, cur_p );
			// Too small
			if ( x_cur == 0 )
			{
				cur_p = 0;
				break;
			}
			if ( x_cur < n_x_recv )
			{
				cur_p -= cur_p / 2.0f;
			}
			else
			{
				cur_p += cur_p / 2.0f;
			}
		}

		// Set the packet number to a value that results in this p value
		uint16 len = cur_p ? (uint16)( MIN( USHRT_MAX, (uint32) ( 1.0f / cur_p ) ) ) : 1;

		uint16 firstSeqNum = unSeqNum - len;
		plh_hist->m_un_seqno = firstSeqNum;
		plh_hist->m_un_length = SeqDist( unSeqNum, firstSeqNum );

		Assert( plh_hist->m_un_length == len );

		if ( steamdatagram_snp_log_loss )
		SpewMsg( "%12llu %s: LOSS INITIAL: x_recv: %d, cur_p: %.8f len: %d\n",
				 usecNow,
				 m_sName.c_str(),
				 n_x_recv,
				 cur_p,
				 len );
	}
	else
	{
		plh_hist->m_un_length = SeqDist( unSeqNum, firstLossSeqNum );

		// Back up one more to the one before this
		auto pprev_lh_hist = plh_hist;
		++pprev_lh_hist;
		pprev_lh_hist->m_un_length = SeqDist( firstLossSeqNum, pprev_lh_hist->m_un_seqno );
	}

	SNP_CalcIMean( usecNow );
	return true;
}

// This returns true if i_mean gets smaller and the sender should reduce rate
bool CSteamNetworkConnectionBase::SNP_UpdateIMean( uint16 unSeqNum, SteamNetworkingMicroseconds usecNow )
{
	if ( m_receiverState.m_vec_li_hist.empty() )
	{
		return false;
	}

	SSNPReceiverState::S_lh_hist &lh_hist = m_receiverState.m_vec_li_hist.back();

	// Cap at USHRT_MAX
	if ( lh_hist.m_un_length < USHRT_MAX )
	{
		uint16 len = SeqDist( unSeqNum, lh_hist.m_un_seqno ) + 1;
		if ( len < lh_hist.m_un_length ) // We wrapped
		{
			lh_hist.m_un_length = USHRT_MAX;
		}
		else
		{
			lh_hist.m_un_length = (uint16)len;
		}
	}

	uint16 old_i_mean = m_receiverState.m_li_i_mean;
	SNP_CalcIMean( usecNow );
	return m_receiverState.m_li_i_mean < old_i_mean;
}

void CSteamNetworkConnectionBase::SNP_NoFeedbackTimer( SteamNetworkingMicroseconds usecNow )
{
	SteamNetworkingMicroseconds usecPing = GetUsecPingWithFallback( this );
	int recover_rate = GetInitialRate( usecPing );

	// Reset feedback state to "no feedback received"
	if ( m_senderState.m_e_tx_state == SSNPSenderState::TFRC_SSTATE_FBACK)
	{
		m_senderState.m_e_tx_state = SSNPSenderState::TFRC_SSTATE_NO_FBACK;
	}

	int n_old_x = m_senderState.m_n_x;

	// Determine new allowed sending rate X as per RFC5348 4.4

	// sender does not have an RTT sample, has not received any feedback from receiver,
	// and has not been idle ever since the nofeedback timer was set
	if ( m_statsEndToEnd.m_ping.m_nSmoothedPing < 0 && m_senderState.m_usec_rto == 0 && m_senderState.m_bSentPacketSinceNFB )
	{
		/* halve send rate directly */
		m_senderState.m_n_x = MAX( m_senderState.m_n_x / 2, k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend );
	}
	else if ( ( ( m_senderState.m_un_p > 0 && m_senderState.m_n_x_recv < recover_rate ) ||
			    ( m_senderState.m_un_p == 0 && m_senderState.m_n_x < 2 * recover_rate ) ) &&
			  !m_senderState.m_bSentPacketSinceNFB )
	{
		// Don't halve the allowed sending rate.
		// Do nothing
	}
	else if ( m_senderState.m_un_p == 0 )
	{
		// We do not have X_Bps yet.
		// Halve the allowed sending rate.		
		int configured_min_rate = m_senderState.m_n_minRate ? m_senderState.m_n_minRate : steamdatagram_snp_min_rate;
		m_senderState.m_n_x = MAX( configured_min_rate, MAX( m_senderState.m_n_x / 2, k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend ) );
	}
	else if ( m_senderState.m_n_x_calc > k_nBurstMultiplier * m_senderState.m_n_x_recv )
	{
		// 2*X_recv was already limiting the sending rate.
		// Halve the allowed sending rate.
		m_senderState.m_n_x_recv = m_senderState.m_n_x_recv / 2;
		SNP_UpdateX( usecNow );
	}
	else
	{
		// The sending rate was limited by X_Bps, not by X_recv.
		// Halve the allowed sending rate.
    	m_senderState.m_n_x_recv = m_senderState.m_n_x_calc / 2;
		SNP_UpdateX( usecNow );
	}

	// Set new timeout for the nofeedback timer.	
	m_senderState.SetNoFeedbackTimer( usecNow );
	m_senderState.m_bSentPacketSinceNFB = false;

	if ( steamdatagram_snp_log_feedback )
		SpewMsg( "%12llu %s: NO FEEDBACK TIMER X=%d, was %d, timer is %llu (rtt is %dms)\n",
				 usecNow,
				 m_sName.c_str(),
				 m_senderState.m_n_x,
				 n_old_x,
				 m_senderState.m_usec_nfb - usecNow,
				 m_statsEndToEnd.m_ping.m_nSmoothedPing );
}

// 0 for no loss
// 1 for loss
// -1 for discard (out of order)
int CSteamNetworkConnectionBase::SNP_CheckForLoss( uint16 unSeqNum, SteamNetworkingMicroseconds usecNow )
{
	if ( !m_receiverState.m_queue_rx_hist.empty() && SeqDist( unSeqNum, m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum ) > 1 )
	{
		int nSeqDelta = SeqDist( unSeqNum, m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum );
		if ( nSeqDelta > USHRT_MAX/2 ) // Out of order
		{
			SpewMsg( "%12llu %s: RECV OOO PACKET(S) %d (wanted %d)\n",
				usecNow,
				m_sName.c_str(),
				unSeqNum,
				( uint16 )( m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum + 1 ) );

			// We're totally fine with out of order packets, just accept them.
			return 0;
		}

		uint16 first = m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum + 1;
		uint16 second = unSeqNum - 1;
		if ( steamdatagram_snp_log_packet || steamdatagram_snp_log_loss )
		SpewMsg( "%12llu %s: RECV LOST %d PACKET(S) %d - %d\n", 
				 usecNow,
				 m_sName.c_str(),
				 nSeqDelta - 1,
				 first,
				 second );

		// Add a lost event
		SNP_AddLossEvent( unSeqNum, usecNow );

		// If we detect loss, we should send a packet on the next interval
		// This is so the sender can quickly determine if a retransmission is necesary
		m_senderState.m_bPendingNAK = true;

		return 1;
	}
	return 0;
}

void CSteamNetworkConnectionBase::SNP_RecordPacket( uint16 unSeqNum, SteamNetworkingMicroseconds unRtt, SteamNetworkingMicroseconds usecNow )
{
	// Record the packet
	if ( m_receiverState.m_queue_rx_hist.size() >= TFRC_NDUPACK )
		m_receiverState.m_queue_rx_hist.pop_front();

	SSNPReceiverState::S_rx_hist rx_hist;
	rx_hist.m_unRecvSeqNum = unSeqNum;
	rx_hist.m_usecPing = unRtt;
	rx_hist.m_usec_recvTs = usecNow;
	m_receiverState.m_queue_rx_hist.push_back( rx_hist );
}

bool CSteamNetworkConnectionBase::SNP_RecvDataChunk( uint16 unSeqNum, const void *pChunk, int cbChunk, int cbPacketSize, SteamNetworkingMicroseconds usecNow )
{
	if ( cbChunk < sizeof( SNPPacketHdr ) )
	{
		return false; // Not enough in the packet?
	}

	const SNPPacketHdr *pHdr = static_cast< const SNPPacketHdr * >( pChunk );

	// Update sender's view from the receiver position
	m_senderState.m_unRecvSeqNum = LittleWord( pHdr->m_unRecvSeqNum );
	m_senderState.m_unRecvMsgNumReliable = LittleWord( pHdr->m_unRecvMsgNum );
	m_senderState.m_unRecvMsgAmtReliable = LittleDWord( pHdr->m_unRecvMsgAmt );

	if ( steamdatagram_snp_log_packet )
	SpewMsg( "%12llu %s: RECV PACKET %d usecNow=%llu sz=%d(%d) recvSeqNum:%d recvMsgNum:%d recvMsgAmt:%d\n",
				 usecNow,
				 m_sName.c_str(),
				 unSeqNum,
				 usecNow,
				 cbChunk,
				 cbPacketSize,
				 LittleWord( pHdr->m_unRecvSeqNum ),
				 LittleWord( pHdr->m_unRecvMsgNum ),
				 LittleDWord( pHdr->m_unRecvMsgAmt ) );

	bool bIsDataPacket = false;

	// Check if packet has actual data, i.e isn't control only
	// this is true if it has a segment with a message payload
	int nPos = sizeof( SNPPacketHdr );
	while ( nPos < cbChunk )
	{
		if ( nPos + (int)sizeof( SNPPacketSegmentType ) > cbChunk )
		{
			break;
		}

		const SNPPacketSegmentType *pSegment = (const SNPPacketSegmentType * )( (const uint8 *)pChunk + nPos );
		uint8 unFlags = pSegment->m_unFlags;
		int nSegmentSize = LittleWord( pSegment->m_unSize );

		if ( ( unFlags & kPacketSegmentFlags_Feedback ) == 0 )
		{
			bIsDataPacket = true;
			break;
		}

		nPos += sizeof( SNPPacketSegmentType ) + nSegmentSize;
	}

	// Check for loss
	int nLossRes = SNP_CheckForLoss( unSeqNum, usecNow );

	if ( nLossRes == -1  ) // Packet so stale we should just drop it.
	{
		return true;
	}

	SSNPReceiverState::ETFRCFeedbackType do_feedback = SSNPReceiverState::TFRC_FBACK_NONE;

	if ( nLossRes == 1 && bIsDataPacket )
	{
		do_feedback = SSNPReceiverState::TFRC_FBACK_PARAM_CHANGE;
	}

	if ( m_receiverState.m_e_rx_state == SSNPReceiverState::TFRC_RSTATE_NO_DATA )
	{
		if ( bIsDataPacket ) 
		{
			do_feedback = SSNPReceiverState::TFRC_FBACK_INITIAL;
			m_receiverState.m_e_rx_state = SSNPReceiverState::TFRC_RSTATE_DATA;
			m_receiverState.m_n_rx_s = cbPacketSize;
		}
	}
	else if ( bIsDataPacket )
	{
		// Update recv counts
		m_receiverState.m_n_rx_s = tfrc_ewma( m_receiverState.m_n_rx_s, cbPacketSize, 9 );
		m_receiverState.m_n_bytes_recv += cbPacketSize;
	}

	if ( bIsDataPacket && SNP_UpdateIMean( unSeqNum, usecNow ) )
	{
		do_feedback = SSNPReceiverState::TFRC_FBACK_PARAM_CHANGE;
	}

	nPos = sizeof( SNPPacketHdr );

	SteamNetworkingMicroseconds usecPing = GetUsecPingWithFallback( this );
	while ( nPos < cbChunk )
	{
		if ( nPos + (int)sizeof( SNPPacketSegmentType ) > cbChunk )
		{
			break;
		}

		const SNPPacketSegmentType *pSegment = (const SNPPacketSegmentType * )( (const uint8 *)pChunk + nPos );
		uint8 unFlags = pSegment->m_unFlags;
		int nSegmentSize = LittleWord( pSegment->m_unSize );

		nPos += sizeof( SNPPacketSegmentType );

		if ( unFlags & kPacketSegmentFlags_Feedback )
		{
			const SNPPacketSegmentFeedback *pFeedback = ( const SNPPacketSegmentFeedback * )( (const uint8 *)pChunk + nPos );

			if ( steamdatagram_snp_log_feedback )
			SpewMsg( "%12llu %s: RECV FEEDBACK %d x_recv:%d t_delay:%d p:%d\n",
						 usecNow,
						 m_sName.c_str(),
						 unSeqNum,
						 LittleDWord( pFeedback->m_un_X_recv ),
						 LittleDWord( pFeedback->m_un_t_delay ),
						 LittleDWord( pFeedback->m_un_p ) );

			m_senderState.m_un_p = LittleDWord( pFeedback->m_un_p );

			Assert( sizeof( SNPPacketSegmentFeedback ) == nSegmentSize );

			nPos += sizeof( SNPPacketSegmentFeedback );

			// Purge any history before the ack in this packet since its old news
			while ( !m_senderState.m_tx_hist.empty() && 
				   IsSeqAfter( m_senderState.m_unRecvSeqNum, m_senderState.m_tx_hist.front().m_unSeqNum ) )
			{
				m_senderState.m_tx_hist.pop_front();
			}

			// Find the sender packet in the tx_history
			for ( SSNPSenderState::S_tx_hist_entry &tx_hist_entry: m_senderState.m_tx_hist )
			{
				if ( tx_hist_entry.m_unSeqNum == m_senderState.m_unRecvSeqNum )
				{
					SteamNetworkingMicroseconds usecElapsed = ( usecNow - tx_hist_entry.m_usec_ts );
					uint32 usecDelay = LittleDWord( pFeedback->m_un_t_delay );
					SteamNetworkingMicroseconds usecPingCalc = usecElapsed - usecDelay;
					if ( usecPing < -1000 )
					{
						SpewWarning( "Ignoring weird ack delay of %uusec, we sent that packet only %lldusec ago!\n", usecDelay, usecElapsed );
					}
					else
					{
						usecPing = Max( usecPingCalc, (SteamNetworkingMicroseconds)1 );
						m_statsEndToEnd.m_ping.ReceivedPing( usecPing/1000, usecNow );
					}

					// found it update rtt
					if ( steamdatagram_snp_log_rtt )
					SpewMsg( "%12llu %s: RECV UPDATE RTT rtt:%dms seqNum:%d ts:%d r_sample:%llu diff_ts:%llu t_delay:%d\n",
								 usecNow,
								 m_sName.c_str(),
								 m_statsEndToEnd.m_ping.m_nSmoothedPing,
								 m_senderState.m_unRecvSeqNum,
								 (int)( tx_hist_entry.m_usec_ts / 1000 ),
								 usecPing,
								 usecElapsed,
								 usecDelay );

					break;
				}
			}

			m_senderState.m_n_x_recv = LittleDWord( pFeedback->m_un_X_recv );

			// Update allowed sending rate X as per draft rfc3448bis-00, 4.2/3
			bool bUpdateX = true;
			if ( m_senderState.m_e_tx_state == SSNPSenderState::TFRC_SSTATE_NO_FBACK )
			{
				m_senderState.m_e_tx_state = SSNPSenderState::TFRC_SSTATE_FBACK;

				if ( m_senderState.m_usec_rto == 0 )
				{
					// Initial feedback packet: Larger Initial Windows (4.2)
					m_senderState.m_n_x = GetInitialRate( usecPing );
					m_senderState.m_usec_ld = usecNow;
					bUpdateX = false;
				}
				else if ( m_senderState.m_un_p == 0 )
				{
					// First feedback after nofeedback timer expiry (4.3)
					bUpdateX = false;
				}
			}

			if ( m_senderState.m_un_p )
			{
				m_senderState.m_n_x_calc = TFRCCalcX( m_senderState.m_n_tx_s, 
													  usecPing,
													  (float)m_senderState.m_un_p / (float)UINT_MAX );
			}

			if ( bUpdateX )
			{
				SNP_UpdateX( usecNow );
			}

			// As we have calculated new ipi, delta, t_nom it is possible
			// that we now can send a packet, so wake up the thinker
			m_senderState.m_usec_rto = MAX( 4 * usecPing, TCP_RTO_MIN );
			m_senderState.SetNoFeedbackTimer( usecNow );
			m_senderState.m_bSentPacketSinceNFB = false;
		}
		else
		{
			// must be message type reliable/unreliable
			bool bIsReliable = ( unFlags & kPacketSegmentFlags_Reliable ) != 0;
			bool bIsEnd = ( unFlags & kPacketSegmentFlags_End ) != 0;

			const SNPPacketSegmentMessage *pSegmentMessage = ( const SNPPacketSegmentMessage * )( (const uint8 *)pChunk + nPos );

			nPos += sizeof( SNPPacketSegmentMessage );

			uint16 unMsgNum = LittleWord( pSegmentMessage->m_unMsgNum );
			int unOffset = LittleDWord( pSegmentMessage->m_unOffset );
			int nMsgSize = nSegmentSize - sizeof( SNPPacketSegmentMessage );

			int msgPos = nPos;
			nPos += nMsgSize;

			if ( bIsReliable )
			{
				CUtlBuffer &recvBuf = m_receiverState.m_recvBufReliable;

				// If this isn't the message we are expecting throw it away
				if ( m_receiverState.m_unRecvMsgNumReliable != unMsgNum )
				{
					if ( steamdatagram_snp_log_segments )
					SpewMsg( "%12llu %s: Unexpected reliable message segment %d:%u sz=%d (expected %d)\n", 
								 usecNow,
								 m_sName.c_str(),
								 unMsgNum, 
								 unOffset, 
								 nSegmentSize,
								 m_receiverState.m_unRecvMsgNumReliable );
					continue;
				}

				if ( recvBuf.TellPut() != unOffset )
				{
					if ( steamdatagram_snp_log_segments )
					SpewMsg( "%12llu %s: Unexpected reliable message offset %d:%u sz=%d (expected %d:%u)\n", 
								 usecNow,
								 m_sName.c_str(),
								 unMsgNum, 
								 unOffset, 
								 nSegmentSize,
								 m_receiverState.m_unRecvMsgNumReliable,
								 recvBuf.TellPut() );
					continue;
				}

				recvBuf.Put( (const uint8 *)pChunk + msgPos, nMsgSize );

				m_receiverState.m_unLastReliableRecvMsgNum = unMsgNum;
				m_receiverState.m_unLastReliabeRecvMsgAmt = recvBuf.TellPut();

				if ( steamdatagram_snp_log_segments )
				SpewMsg( "%12llu %s: RELIABLE    %d: msgNum %d offset=%d recvAmt=%d segmentSize=%d%s\n", 
							 usecNow,
							 m_sName.c_str(), 
							 unSeqNum,
							 unMsgNum,
							 unOffset,
							 recvBuf.TellPut(),
							 nSegmentSize,
							 bIsEnd ? " (end)" : "" );

				// Are we done?
				if ( bIsEnd )
				{
					ReceivedMessage( recvBuf.Base(), recvBuf.TellPut(), usecNow );

					if ( steamdatagram_snp_log_message || steamdatagram_snp_log_reliable )
					SpewMsg( "%12llu %s: RecvMessage RELIABLE: MsgNum=%d sz=%d\n",
								 usecNow,
								 m_sName.c_str(),
								 unMsgNum,
								 recvBuf.TellPut() );

					// Clear and ready for the next message!
					recvBuf.Clear();
					++m_receiverState.m_unRecvMsgNumReliable;

					// Update counter
					++m_receiverState.m_nMessagesRecvReliable;
				}
			}
			else
			{
				CUtlBuffer &recvBuf = m_receiverState.m_recvBuf;

				// If this isn't the message we are expecting throw it away
				if ( m_receiverState.m_unRecvMsgNum != unMsgNum )
				{
					if ( steamdatagram_snp_log_segments )
					SpewMsg( "%12llu %s: Throwing away unreliable message %d sz=%d\n", 
								 usecNow,
								 m_sName.c_str(),
								 m_receiverState.m_unRecvMsgNum, 
								 recvBuf.TellPut() );
					recvBuf.Purge();
				}

				if ( recvBuf.TellPut() != unOffset )
				{
					recvBuf.Purge();

					// If its zero its a new message, just go with it
					if ( unOffset != 0 )
					{
						if ( steamdatagram_snp_log_segments )
						SpewMsg( "%12llu %s: Unexpected reliable message offset %d:%u sz=%d\n", 
									 usecNow,
									 m_sName.c_str(),
									 unMsgNum, 
									 unOffset, 
									 nSegmentSize );
						continue;
					}
				}

				m_receiverState.m_unRecvMsgNum = unMsgNum;
				recvBuf.Put( (const uint8 *)pChunk + msgPos, nMsgSize );

				if ( steamdatagram_snp_log_segments )
				SpewMsg( "%12llu %s: UNRELIABLE  %d: msgNum %d offset=%d recvAmt=%d segmentSize=%d%s\n", 
							 usecNow,
							 m_sName.c_str(), 
							 unSeqNum,
							 unMsgNum,
							 unOffset,
							 recvBuf.TellPut(),
							 nSegmentSize,
							 bIsEnd ? " (end)" : "" );

				// Are we done?
				if ( bIsEnd )
				{
					ReceivedMessage( recvBuf.Base(), recvBuf.TellPut(), usecNow );

					if ( steamdatagram_snp_log_message )
					SpewMsg( "%12llu %s: RecvMessage UNRELIABLE: MsgNum=%d sz=%d\n",
								 usecNow,
								 m_sName.c_str(),
								 unMsgNum,
								 recvBuf.TellPut() );

					// Clear and ready for the next message!
					recvBuf.Clear();
					++m_receiverState.m_unRecvMsgNum;

					// Update counter
					++m_receiverState.m_nMessagesRecvUnreliable;
				}
				else
				{
					if ( steamdatagram_snp_log_segments )
					SpewMsg( "%12llu %s: MSG recieved unreliable message %d section offset %u (sz=%d)\n",
								 usecNow,
								 m_sName.c_str(),
								 unMsgNum,
								 unOffset,
								 nSegmentSize );
				}
			}
		}
	}

	if ( bIsDataPacket &&
		do_feedback == SSNPReceiverState::TFRC_FBACK_NONE &&
		m_senderState.m_sendFeedbackState == SSNPSenderState::TFRC_SSTATE_FBACK_NONE &&
		m_receiverState.m_usec_next_feedback &&
		m_receiverState.m_usec_next_feedback <= usecNow )
	{
		do_feedback = SSNPReceiverState::TFRC_FBACK_PERIODIC;
	}

	switch ( do_feedback )
	{
	case SSNPReceiverState::TFRC_FBACK_NONE :
		break; // No work
	case SSNPReceiverState::TFRC_FBACK_INITIAL :
		m_receiverState.m_n_x_recv = 0;
		m_receiverState.m_li_i_mean = 0;
		// FALL THROUGH

	case SSNPReceiverState::TFRC_FBACK_PARAM_CHANGE :
		m_senderState.m_sendFeedbackState = SSNPSenderState::TFRC_SSTATE_FBACK_REQ; // Send feedback immediately
		break;
	case SSNPReceiverState::TFRC_FBACK_PERIODIC :
		m_senderState.m_sendFeedbackState = SSNPSenderState::TFRC_SSTATE_FBACK_PERODIC; // Send feedback on next data packet
		break;
	}
	
	SNP_RecordPacket( unSeqNum, usecPing, usecNow );

	// Check for retransmit
	SNP_CheckForReliable( usecNow );

	return true;
}

void CSteamNetworkConnectionBase::SNP_RecvNonDataPacket( uint16 unSeqNum, SteamNetworkingMicroseconds usecNow )
{
	// underlying sytem consumed a packet for data reporting, mark it in the history as consumed, use previous values
	// note if we don't have any history we don't care
	if ( !m_receiverState.m_queue_rx_hist.empty() )
	{
		// Even a consumed non-data packet could have detected loss
		SNP_CheckForLoss( unSeqNum, usecNow );

		// Duplicate rtt from previous packet since we don't have it
		SteamNetworkingMicroseconds usecPing = GetUsecPingWithFallback( this );
		SNP_RecordPacket( unSeqNum, usecPing, usecNow );
	}
}

// RFC 3448, 4.3
void CSteamNetworkConnectionBase::SNP_UpdateX( SteamNetworkingMicroseconds usecNow )
{
	int configured_min_rate = m_senderState.m_n_minRate ? m_senderState.m_n_minRate : steamdatagram_snp_min_rate;

	int min_rate = MAX( configured_min_rate, m_senderState.m_n_x_recv * k_nBurstMultiplier ); 

	SteamNetworkingMicroseconds usecPing = GetUsecPingWithFallback( this );

	int n_old_x = m_senderState.m_n_x;
	if ( m_senderState.m_un_p )
	{
		m_senderState.m_n_x = MAX( MIN( m_senderState.m_n_x_calc, min_rate ), m_senderState.m_n_tx_s );
		m_senderState.m_n_x = MAX( m_senderState.m_n_x, configured_min_rate );
	}
	else if ( m_statsEndToEnd.m_ping.m_nSmoothedPing >= 0 && usecNow - m_senderState.m_usec_ld >= usecPing )
	{   	   
		m_senderState.m_n_x = MAX( MIN( 2 * m_senderState.m_n_x, min_rate ), GetInitialRate( usecPing ) );
		m_senderState.m_usec_ld = usecNow;
	}

	// For now cap at steamdatagram_snp_max_rate
	if ( m_senderState.m_n_maxRate )
	{
		m_senderState.m_n_x = MIN( m_senderState.m_n_x, m_senderState.m_n_maxRate );
	}
	else if ( steamdatagram_snp_max_rate )
	{
		m_senderState.m_n_x = MIN( m_senderState.m_n_x, steamdatagram_snp_max_rate );
	}

	if ( m_senderState.m_n_x != n_old_x ) 
	{
		if ( steamdatagram_snp_log_x )
		SpewMsg( "%12llu %s: UPDATE X=%d (was %d) x_recv=%d min_rate=%d p=%u x_calc=%d tx_s=%d\n", 
					 usecNow,
					 m_sName.c_str(),
					 m_senderState.m_n_x,
					 n_old_x,
					 m_senderState.m_n_x_recv,
					 min_rate,
					 m_senderState.m_un_p,
					 m_senderState.m_n_x_calc,
					 m_senderState.m_n_tx_s );
	}

	UpdateSpeeds( m_senderState.m_n_x, m_senderState.m_n_x_recv );
}

bool CSteamNetworkConnectionBase::SNP_InsertSegment( SSNPBuffer *p_buf, uint8 flags, int nSize )
{
	if ( p_buf->m_nSize + sizeof( SNPPacketSegmentType ) + nSize > k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend )
	{
		return false;
	}

	SNPPacketSegmentType *pSegment = (SNPPacketSegmentType *)( p_buf->m_buf + p_buf->m_nSize );
	pSegment->m_unFlags = flags;
	pSegment->m_unSize = LittleWord( static_cast< uint16>( nSize ) );
	p_buf->m_nSize += sizeof( SNPPacketSegmentType );
	return true;
}

struct SSendPacketEntryMsg
{
	bool m_bReliable = false;
	SSNPSendMessage *m_pMsg;
	SSNPSendMessage::SSendPacketEntry m_sendPacketEntry;
};

int CSteamNetworkConnectionBase::SNP_SendPacket( SteamNetworkingMicroseconds usecNow )
{
	SSNPBuffer sendBuf;

	SNPPacketHdr *pHdr = (SNPPacketHdr *)sendBuf.m_buf;
	//pHdr->m_unRtt = LittleWord( static_cast<uint16>( m_senderState.m_usec_rtt / 1000 ) );

	uint16 recvNum = USHRT_MAX;
	if ( !m_receiverState.m_queue_rx_hist.empty() )
	{
		recvNum = m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum;
	}
	pHdr->m_unRecvSeqNum = LittleWord( recvNum );
	pHdr->m_unRecvMsgNum = LittleWord( m_receiverState.m_unLastReliableRecvMsgNum );
	pHdr->m_unRecvMsgAmt = LittleDWord( m_receiverState.m_unLastReliabeRecvMsgAmt );

	sendBuf.m_nSize += sizeof( SNPPacketHdr );

	// Do we need to put in a feedback packet?
	if ( m_senderState.m_sendFeedbackState != SSNPSenderState::TFRC_SSTATE_FBACK_NONE )
	{
		SNP_PrepareFeedback( usecNow );

		// Insert segment
		SNP_InsertSegment( &sendBuf, kPacketSegmentFlags_Feedback, sizeof( SNPPacketSegmentFeedback ) );

		SNPPacketSegmentFeedback *pFeedback = ( SNPPacketSegmentFeedback * )( sendBuf.m_buf + sendBuf.m_nSize );

		uint32 rx_hist_t_delay = 0;
		if ( !m_receiverState.m_queue_rx_hist.empty() )
		{
			rx_hist_t_delay = static_cast< uint32 >( ( usecNow - m_receiverState.m_queue_rx_hist.back().m_usec_recvTs ) );
		}

		pFeedback->m_un_t_delay = LittleDWord( rx_hist_t_delay );
		pFeedback->m_un_X_recv = LittleDWord( m_receiverState.m_n_x_recv );
		pFeedback->m_un_p = LittleDWord( m_receiverState.m_li_i_mean );

		m_senderState.m_sendFeedbackState = SSNPSenderState::TFRC_SSTATE_FBACK_NONE;

		sendBuf.m_nSize += sizeof( SNPPacketSegmentFeedback );
	}

	CUtlVector< SSendPacketEntryMsg > vecSendPacketEntryMsg;

	SSNPSendMessage *pDeleteMessages = nullptr;

	// Send msg pieces
	SSNPSendMessage *pCurMsg = m_senderState.m_pSendMessages;
	while ( pCurMsg )
	{
		// Figure out size based on room left in packet and next segment

		// How many bytes left in the message
		int nMsgRemainingSize = pCurMsg->m_nSize - pCurMsg->m_nSendPos;

		// How many bytes in the buffer, this will be prepended by segment headers
		int nMsgRemainingBuffer = k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend - sendBuf.m_nSize;

		// Is there room?
		int msgHeaderSize = sizeof( SNPPacketSegmentType ) + sizeof( SNPPacketSegmentMessage );
		if ( nMsgRemainingBuffer <= msgHeaderSize )
			break;

		int nSendSize = MIN( nMsgRemainingSize, nMsgRemainingBuffer - msgHeaderSize );
		if ( !nSendSize ) // no room?  TODO: should there be a minimum?  This will squeeze one byte in
		{
			break; 
		}

		// Is this the last block?
		bool bIsLastSegment = ( pCurMsg->m_nSendPos + nSendSize >= pCurMsg->m_nSize );

		uint8 unSegmentFlags = kPacketSegmentFlags_Message;
		if ( pCurMsg->m_bReliable )
		{
			unSegmentFlags |= kPacketSegmentFlags_Reliable;
		}
		if ( bIsLastSegment )
		{
			unSegmentFlags |= kPacketSegmentFlags_End;
		}

		SNP_InsertSegment( &sendBuf, unSegmentFlags, nSendSize + sizeof( SNPPacketSegmentMessage ) );

		// Now insert the message segment header
		SNPPacketSegmentMessage *pSegmentMessage = ( SNPPacketSegmentMessage * )( sendBuf.m_buf + sendBuf.m_nSize );
		pSegmentMessage->m_unMsgNum = LittleWord( pCurMsg->m_unMsgNum );
		pSegmentMessage->m_unOffset = LittleDWord( pCurMsg->m_nSendPos );

		sendBuf.m_nSize += sizeof( SNPPacketSegmentMessage );

		// Copy the bytes
		memcpy( sendBuf.m_buf + sendBuf.m_nSize, pCurMsg->m_pData + pCurMsg->m_nSendPos, nSendSize );
		sendBuf.m_nSize += nSendSize;

		// Allocate an entry so we can record this send after we get a packet number
		{
			SSendPacketEntryMsg *pEntry = vecSendPacketEntryMsg.AddToTailGetPtr();
			pEntry->m_bReliable = pCurMsg->m_bReliable;
			pEntry->m_pMsg = pCurMsg;
			pEntry->m_sendPacketEntry.m_usec_sentTime = usecNow; // Will be filed in later
			pEntry->m_sendPacketEntry.m_unSeqNum = 0; // Will be filed in later
			pEntry->m_sendPacketEntry.m_nOffset = pCurMsg->m_nSendPos;
			pEntry->m_sendPacketEntry.m_nSentAmt = pCurMsg->m_nSendPos + nSendSize;
		}

		// Move message position
		pCurMsg->m_nSendPos += nSendSize;

		// Remove from pending bytes
		if ( pCurMsg->m_bReliable )
		{
			Assert( m_senderState.m_cbPendingReliable >= nSendSize );
			m_senderState.m_cbPendingReliable -= nSendSize;
		}
		else
		{
			Assert( m_senderState.m_cbPendingUnreliable >= nSendSize );
			m_senderState.m_cbPendingUnreliable -= nSendSize;
		}

		// If this message is done, move it to SentMessages and go next
		if ( bIsLastSegment )
		{
			// Unlink it
			m_senderState.m_pSendMessages = pCurMsg->m_pNext;
			pCurMsg->m_pNext = nullptr;

			// If it's reliable move it to the sent queue as we may need to re-xmit
			if ( pCurMsg->m_bReliable )
			{
				// Add it to the end of sent queue
				SSNPSendMessage **ppSentMessages = &m_senderState.m_pSentMessages;
				while ( *ppSentMessages )
				{
					ppSentMessages = &(*ppSentMessages)->m_pNext;
				}
				*ppSentMessages = pCurMsg;
				m_senderState.m_cbSentUnackedReliable += pCurMsg->m_nSize;
			}
			else
			{
				// move unreliable to the delete list
				pCurMsg->m_pNext = pDeleteMessages;
				pDeleteMessages = pCurMsg;
			}

			pCurMsg = m_senderState.m_pSendMessages;
			continue; // Try to fit next message
		}

		// If here we put as much as we can into the message
		break;
	}

	// Send this packet
	uint16 sendSeqNum = 0;
	int sendSize = EncryptAndSendDataChunk( sendBuf.m_buf, sendBuf.m_nSize, usecNow, &sendSeqNum );

	// Note we sent a packet during the NFB period
	m_senderState.m_bSentPacketSinceNFB = true;

	if ( steamdatagram_snp_log_packet )
	SpewMsg( "%12llu %s: SEND PACKET %d usecNow=%llu sz=%d(%d) recvSeqNum:%d recvMsgNum:%d recvMsgAmt:%d\n", 
				 usecNow,
				 m_sName.c_str(),
				 sendSeqNum,
				 usecNow,
				 sendBuf.m_nSize,
				 sendSize,
				 LittleWord( pHdr->m_unRecvSeqNum ),
				 LittleWord( pHdr->m_unRecvMsgNum ),
				 LittleDWord( pHdr->m_unRecvMsgAmt ) );

	if ( sendSize )
	{
		/**
		 *	Track the mean packet size `s'
		 *  size: packet payload size in bytes, this should by the wire
		 *  size so include steam datagram header, UDP header and IP
		 *  header.
		 *
		 *	cf. RFC 4342, 5.3 and  RFC 3448, 4.1
		 */
		m_senderState.m_n_tx_s = tfrc_ewma( m_senderState.m_n_tx_s, sendSize, 9 );

		// Add to history
		SSNPSenderState::S_tx_hist_entry tx_hist_entry;
		tx_hist_entry.m_unSeqNum = sendSeqNum;
		tx_hist_entry.m_usec_ts = usecNow;
		m_senderState.m_tx_hist.push_back( tx_hist_entry );
	}

	// Update entries with send seq
	// Note that on failure seqNum is 0 which will cause a retransmit in the recv if the other end tells us so
	for ( auto &sendPacketEntryMsg : vecSendPacketEntryMsg )
	{
		SSNPSendMessage::SSendPacketEntry *pEntry = &sendPacketEntryMsg.m_sendPacketEntry;
		if ( sendPacketEntryMsg.m_bReliable )
		{
			pEntry->m_unSeqNum = sendSeqNum;
			sendPacketEntryMsg.m_pMsg->m_vecSendPackets.AddToTail( *pEntry );
		}

		if ( steamdatagram_snp_log_segments )
		SpewMsg( "%12llu %s: %s  %d: msgNum %d offset=%d sendAmt=%d segmentSize=%d%s\n", 
					 usecNow,
					 m_sName.c_str(),
					 sendPacketEntryMsg.m_bReliable ? "RELIABLE  " : "UNRELIABLE",
					 sendSeqNum,
					 sendPacketEntryMsg.m_pMsg->m_unMsgNum,
					 pEntry->m_nOffset,
					 pEntry->m_nSentAmt,
					 pEntry->m_nSentAmt - pEntry->m_nOffset,
					 pEntry->m_nSentAmt >= sendPacketEntryMsg.m_pMsg->m_nSize ? " (end)" : "" );
	}

	// Pending deletions (completed unreliable messages)
	while ( pDeleteMessages )
	{
		SSNPSendMessage *pNext = pDeleteMessages->m_pNext;
		delete pDeleteMessages;
		pDeleteMessages = pNext;
	}

	return sendSize;
}

// Returns next think time
void CSteamNetworkConnectionBase::SNP_ThinkSendState( SteamNetworkingMicroseconds usecNow )
{
	// Check expiry of no feedback timer
	if ( m_senderState.m_usec_nfb <= usecNow )
	{
		SNP_NoFeedbackTimer( usecNow );
		Assert( m_senderState.m_usec_nfb > usecNow );
	}

	// Accumulate tokens based on how long it's been since last time
	m_senderState.TokenBucket_Accumulate( usecNow );

	// Still can't send anything?
	if ( m_senderState.m_flTokenBucket < 0.0f )
		return;

	// Keep sending packets until we run out of tokens
	int nPacketsSent = 0;
	for (;;)
	{
		// Check if we need to move queued messages over
		if ( m_senderState.m_pQueuedMessages )
		{
			// Check nagle delay
			if ( m_senderState.m_t_nagle && m_senderState.m_t_nagle > usecNow )
			{
				// Wait until Nagle if we don't already have messages pending
				if ( !m_senderState.m_pSendMessages )
				{
					if ( steamdatagram_snp_log_nagle )
					SpewMsg( "%12llu %s: NAGLE WAIT %llu to go\n", 
								usecNow,
								m_sName.c_str(), 
								m_senderState.m_t_nagle - usecNow );
					break;
				}
				if ( steamdatagram_snp_log_nagle )
				SpewMsg( "%12llu %s: NAGLE cleared (pending send), %llu early\n", 
							usecNow,
							m_sName.c_str(), 
							m_senderState.m_t_nagle - usecNow );
				m_senderState.m_t_nagle = 0; 
			}

			// Add it to the end of send queue
			m_senderState.FlushNagle();

			// Clear nagle
			if ( steamdatagram_snp_log_nagle && m_senderState.m_t_nagle )
			{
				SpewMsg( "%12llu %s: NAGLE REACHED (cleared)\n", 
							usecNow,
							m_sName.c_str() );
			}
		}

		// If send feedback is peroidic but more than RTO/2 has passed force it
		if ( m_senderState.m_sendFeedbackState == SSNPSenderState::TFRC_SSTATE_FBACK_PERODIC )
		{
			if ( m_senderState.m_usec_rto && usecNow - m_receiverState.m_usec_tstamp_last_feedback > m_senderState.m_usec_rto / 2 )
			{
				m_senderState.m_sendFeedbackState = SSNPSenderState::TFRC_SSTATE_FBACK_REQ;
				if ( steamdatagram_snp_log_feedback )
					SpewMsg( "%12llu %s: TFRC_SSTATE_FBACK_REQ due to rto/2 timeout\n",
								usecNow,
								m_sName.c_str() );
			}
			if ( !m_senderState.m_usec_rto && usecNow - m_receiverState.m_usec_tstamp_last_feedback > TCP_RTO_MIN / 2 )
			{
				m_senderState.m_sendFeedbackState = SSNPSenderState::TFRC_SSTATE_FBACK_REQ;
				if ( steamdatagram_snp_log_feedback )
					SpewMsg( "%12llu %s: TFRC_SSTATE_FBACK_REQ due to TCP_RTO_MIN/2 timeout\n",
								usecNow,
								m_sName.c_str() );
			}
		}
			
		bool bSendPacket = m_senderState.m_pSendMessages || 
			m_senderState.m_bPendingNAK || 
			m_senderState.m_sendFeedbackState == SSNPSenderState::TFRC_SSTATE_FBACK_REQ;

		if ( !bSendPacket )
			break;

		if ( nPacketsSent > k_nMaxPacketsPerThink )
		{
			// We're sending too much at one time.  Nuke token bucket so that
			// we'll be ready to send again very soon, but not immediately.
			// We don't want the outer code to complain that we are requesting
			// a wakeup call in the past
			m_senderState.m_flTokenBucket = m_senderState.m_n_x * -0.0005f;
			return;
		}

		int nBytesSent = SNP_SendPacket( usecNow );
		if ( nBytesSent <= 0 )
		{
			// Immediately nuke token bucket, so we don't get caught in a loop
			// thinking it's time to wake up immediately again
			m_senderState.m_flTokenBucket = m_senderState.m_n_x * -0.001f;
			return;
		}

		// If we had a NAK to send, we should have now sent it
		m_senderState.m_bPendingNAK = false;

		// We spent some tokens
		m_senderState.m_flTokenBucket -= (float)nBytesSent;
		if ( m_senderState.m_flTokenBucket < 0.0f )
			return;

		// Limit number of packets sent at a time, even if the scheduler is really bad
		// or somebody holds the lock for along time, or we wake up late for whatever reason
		++nPacketsSent;
	}

	// OK, if we get here, we've sent everything we want to send.
	// Now limit our reserve to a small burst overage
	m_senderState.TokenBucket_Limit();
}

SteamNetworkingMicroseconds CSteamNetworkConnectionBase::SNP_GetNextThinkTime( SteamNetworkingMicroseconds usecNow ) const
{

	// When's the next time we need to wake up?
	SteamNetworkingMicroseconds usecNextThink = Min( usecNow + kSNPMinThink, m_senderState.m_usec_nfb );

	// If we have messages to send, then we might need to wake up and take action
	if ( m_senderState.m_pQueuedMessages || m_senderState.m_pSendMessages )
	{
		// Time when we *could* send the next packet, ignoring Nagle
		SteamNetworkingMicroseconds usecNextSend = usecNow;
		SteamNetworkingMicroseconds usecQueueTime = m_senderState.CalcTimeUntilNextSend();
		if ( usecQueueTime > 0 )
		{
			usecNextSend += usecQueueTime;

			// Add a small amount of fudge here, so that we don't wake up too early and think
			// we're not ready yet, causing us to spin our wheels.  Our token bucket system
			// should keep us sending at the correct overall rate.  Remember that the
			// underlying kernel timer/wake resolution might be 1 or 2ms, (E.g. Windows.)
			usecNextSend += 25;
		}

		// If everything is waiting for Nagle, then that is the next thing we need to wake up for
		if ( m_senderState.m_pSendMessages == nullptr )
			usecNextSend = Max( usecNextSend, m_senderState.m_t_nagle );

		// Earlier than any other reason to wake up?
		usecNextThink = Min( usecNextThink, usecNextSend );
	}
	
	return usecNextThink;
}

void CSteamNetworkConnectionBase::SNP_PopulateDetailedStats( SteamDatagramLinkStats &info ) const
{
	info.m_latest.m_nSendRate = m_senderState.m_n_x;
	info.m_latest.m_nPendingBytes = m_senderState.m_cbPendingUnreliable + m_senderState.m_cbPendingReliable;
	info.m_lifetime.m_nMessagesSentReliable    = m_senderState.m_nMessagesSentReliable;
	info.m_lifetime.m_nMessagesSentUnreliable  = m_senderState.m_nMessagesSentUnreliable;
	info.m_lifetime.m_nMessagesRecvReliable    = m_receiverState.m_nMessagesRecvReliable;
	info.m_lifetime.m_nMessagesRecvUnreliable  = m_receiverState.m_nMessagesRecvUnreliable;
}

void CSteamNetworkConnectionBase::SNP_PopulateQuickStats( SteamNetworkingQuickConnectionStatus &info, SteamNetworkingMicroseconds usecNow )
{
	info.m_nSendRateBytesPerSecond = m_senderState.m_n_x;
	info.m_cbPendingUnreliable = m_senderState.m_cbPendingUnreliable;
	info.m_cbPendingReliable = m_senderState.m_cbPendingReliable;
	info.m_cbSentUnackedReliable = m_senderState.m_cbSentUnackedReliable;

	// Accumulate tokens so that we can properly predict when the next time we'll be able to send something is
	m_senderState.TokenBucket_Accumulate( usecNow );

	//
	// Time until we can send the next packet
	// If anything is already queued, then that will have to go out first.  Round it down
	// to the nearest packet.
	//
	// NOTE: This ignores the precise details of SNP framing.  If there are tons of
	// small packets, it'll actually be worse.  We might be able to approximate that
	// the framing overhead better by also counting up the number of *messages* pending.
	// Probably not worth it here, but if we had that number available, we'd use it.
	int cbPendingTotal = m_senderState.PendingBytesTotal() / k_cbSteamNetworkingSocketsMaxMessageNoFragment * k_cbSteamNetworkingSocketsMaxMessageNoFragment;

	// Adjust based on how many tokens we have to spend now (or if we are already
	// over-budget and have to wait until we could spend another)
	cbPendingTotal -= (int)m_senderState.m_flTokenBucket;
	if ( cbPendingTotal <= 0 )
	{
		// We could send it right now.
		info.m_usecQueueTime = 0;
	}
	else
	{

		info.m_usecQueueTime = (int64)cbPendingTotal * k_nMillion / m_senderState.m_n_x;
	}
}

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
void CSteamNetworkConnectionBase::SNP_PopulateP2PSessionStateStats( P2PSessionState_t &info ) const
{
	info.m_nBytesQueuedForSend = 0;
	info.m_nPacketsQueuedForSend = 0;
	for ( SSNPSendMessage *pMsg = m_senderState.m_pQueuedMessages ; pMsg ; pMsg = pMsg->m_pNext )
	{
		Assert( pMsg->m_nSendPos == 0 );
		info.m_nBytesQueuedForSend += pMsg->m_nSize;
		info.m_nPacketsQueuedForSend += 1;
	}
	for ( SSNPSendMessage *pMsg = m_senderState.m_pSendMessages ; pMsg ; pMsg = pMsg->m_pNext )
	{
		Assert( pMsg->m_nSendPos == 0 || pMsg == m_senderState.m_pSendMessages );
		info.m_nBytesQueuedForSend += pMsg->m_nSize - pMsg->m_nSendPos;
		info.m_nPacketsQueuedForSend += 1;
	}
}
#endif

void CSteamNetworkConnectionBase::SetMinimumRate( int nRate )
{
	m_senderState.m_n_minRate = nRate;

	// Apply clamp immediately, don't wait for us to re-calc
	if ( m_senderState.m_n_x < nRate )
		m_senderState.m_n_x = nRate;
}

void CSteamNetworkConnectionBase::SetMaximumRate( int nRate )
{
	m_senderState.m_n_maxRate = nRate;

	// Apply clamp immediately, don't wait for us to re-calc
	if ( nRate > 0 && m_senderState.m_n_x > nRate )
		m_senderState.m_n_x = nRate;
}

void CSteamNetworkConnectionBase::GetDebugText( char *pszOut, int nOutCCH )
{
	V_snprintf( pszOut, nOutCCH, "%s\n"
				 "SenderState\n"
				 " x . . . . . . %d\n"
				 " x_recv. . . . %d\n"
				 " x_calc. . . . %d\n"
				 " rtt . . . . . %dms\n"
				 " p . . . . . . %.8f\n"
				 " tx_s. . . . . %d\n"
				 " recvSeqNum. . %d\n"
				 " pendingB. . . %d\n"
				 " outReliableB. %d\n"
				 " msgsReliable. %lld\n"
				 " msgs. . . . . %lld\n"
				 " minRate . . . %d\n"
				 " maxRate . . . %d\n"
				 "\n"
				 "ReceiverState\n"
				 " bytes_recv. . %d\n"
				 " x_recv. . . . %d\n"
				 " rx_s. . . . . %d\n"
				 " i_mean. . . . %d (%.8f)\n"
				 " msgsReliable. %lld\n"
				 " msgs. . . . . %lld\n",
				 m_sName.c_str(),
				 m_senderState.m_n_x,
				 m_senderState.m_n_x_recv,
				 m_senderState.m_n_x_calc,
				 m_statsEndToEnd.m_ping.m_nSmoothedPing,
				 (float)m_senderState.m_un_p / (float)UINT_MAX,
				 m_senderState.m_n_tx_s,
				 m_senderState.m_unRecvSeqNum,
				 m_senderState.PendingBytesTotal(),
				 m_senderState.m_cbSentUnackedReliable,
				 m_senderState.m_nMessagesSentReliable,
				 m_senderState.m_nMessagesSentUnreliable,
				 m_senderState.m_n_minRate ? m_senderState.m_n_minRate : steamdatagram_snp_min_rate,
				 m_senderState.m_n_maxRate ? m_senderState.m_n_maxRate : steamdatagram_snp_max_rate,
				 //
				 m_receiverState.m_n_bytes_recv,
				 m_receiverState.m_n_x_recv,
				 m_receiverState.m_n_rx_s,
				 m_receiverState.m_li_i_mean,
				 (float)m_receiverState.m_li_i_mean / (float)UINT_MAX,
				 m_receiverState.m_nMessagesRecvReliable,
				 m_receiverState.m_nMessagesRecvUnreliable
				 );
}

} // namespace SteamNetworkingSocketsLib

