//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_snp.h"
#include "steamnetworkingsockets_connections.h"
#include "crypto.h"

#include "steamnetworkingconfig.h"

// Ug
#ifdef _MSC_VER
	#if _MSC_VER < 1900
		#define constexpr const
	#endif
#endif

// Acks may be delayed.  This controls the precision used on the wire to encode the delay time.
constexpr int k_nAckDelayPrecisionShift = 5;
constexpr SteamNetworkingMicroseconds k_usecAckDelayPrecision = (1 << k_nAckDelayPrecisionShift );

// When a receiver detects a dropped packet, schedule sending of ACKs on this interval
constexpr SteamNetworkingMicroseconds k_usecNackFlush = 10*1000;

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

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace SteamNetworkingSocketsLib {

struct SNPAckSerializerHelper
{
	struct Block
	{
		// Acks and nacks count to serialize
		uint32 m_nAck;
		uint32 m_nNack;

		// What to put in the header if we use this as the
		// highest numbered block
		uint32 m_nLatestPktNum; // Lower 32-bits.  We might send even fewer bits
		uint16 m_nEncodedTimeSinceLatestPktNum;

		// Total size of this block and all earlier ones.
		int16 m_cbTotalEncodedSize;
	};

	enum { k_cbHeaderSize = 5 };
	enum { k_nMaxBlocks = 64 };
	int m_nBlocks;
	Block m_arBlocks[ k_nMaxBlocks ];

	static uint16 EncodeTimeSince( SteamNetworkingMicroseconds usecNow, SteamNetworkingMicroseconds usecWhenSentLast )
	{

		// Encode time since last
		SteamNetworkingMicroseconds usecElapsedSinceLast = usecNow - usecWhenSentLast;
		Assert( usecElapsedSinceLast >= 0 );
		Assert( usecNow > 0x20000*k_usecAckDelayPrecision ); // We should never have small timestamp values.  A timestamp of zero should always be "a long time ago"
		if ( usecElapsedSinceLast > 0xfffell<<k_nAckDelayPrecisionShift )
			return 0xffff;
		return uint16( usecElapsedSinceLast >> k_nAckDelayPrecisionShift );
	}

};

template <typename T>
inline int64 NearestWithSameLowerBits( T nLowerBits, int64 nReference )
{
	COMPILE_TIME_ASSERT( sizeof(T) < sizeof(int64) ); // Make sure it's smaller than 64 bits, or else why are you doing this?
	COMPILE_TIME_ASSERT( ~T(0) < 0 ); // make sure it's a signed type!
	T nDiff = nLowerBits - T( nReference );
	return nReference + nDiff;
}

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
void SSNPSenderState::RemoveAckedReliableMessageFromUnackedList()
{

	// Trim messages from the head that have been acked.
	// Note that in theory we could have a message in the middle that
	// has been acked.  But it's not worth the time to go looking for them,
	// just to free up a bit of memory early.  We'll get to it once the earlier
	// messages have been acked.
	while ( !m_unackedReliableMessages.empty() )
	{
		SNPSendMessage_t *pMsg = m_unackedReliableMessages.m_pFirst;
		Assert( pMsg->m_nReliableStreamPos > 0 );
		int64 nReliableEnd = pMsg->m_nReliableStreamPos + pMsg->m_cbSize;

		// Are we backing a range that is in flight (and thus we might need
		// to resend?)
		if ( !m_listInFlightReliableRange.empty() )
		{
			auto head = m_listInFlightReliableRange.begin();
			Assert( head->first.m_nBegin >= pMsg->m_nReliableStreamPos );
			if ( head->second == pMsg )
			{
				Assert( head->first.m_nBegin < nReliableEnd );
				return;
			}
			Assert( head->first.m_nBegin >= nReliableEnd );
		}

		// Are we backing the next range that is ready for resend now?
		if ( !m_listReadyRetryReliableRange.empty() )
		{
			auto head = m_listReadyRetryReliableRange.begin();
			Assert( head->first.m_nBegin >= pMsg->m_nReliableStreamPos );
			if ( head->second == pMsg )
			{
				Assert( head->first.m_nBegin < nReliableEnd );
				return;
			}
			Assert( head->first.m_nBegin >= nReliableEnd );
		}

		// We're all done!
		DbgVerify( m_unackedReliableMessages.pop_front() == pMsg );
		delete pMsg;
	}
}

//-----------------------------------------------------------------------------
void CSteamNetworkConnectionBase::SNP_InitializeConnection( SteamNetworkingMicroseconds usecNow )
{
	m_senderState.TokenBucket_Init( usecNow );

	// Setup the table of inflight packets with a sentinel.
	m_senderState.m_mapInFlightPacketsByPktNum.clear();
	SNPInFlightPacket_t &sentinel = m_senderState.m_mapInFlightPacketsByPktNum[INT64_MIN];
	sentinel.m_bNack = false;
	sentinel.m_usecWhenSent = 0;
	m_senderState.m_itNextInFlightPacketToTimeout = m_senderState.m_mapInFlightPacketsByPktNum.end();

	//m_senderState.m_usec_nfb = usecNow + TFRC_INITIAL_TIMEOUT;
	//m_senderState.m_bSentPacketSinceNFB = false;

	SteamNetworkingMicroseconds usecPing = GetUsecPingWithFallback( this );
	m_senderState.m_n_x = GetInitialRate( usecPing );

//	if ( steamdatagram_snp_log_x )
//	SpewMsg( "%12llu %s: INITIAL X=%d rtt=%dms tx_s=%d\n", 
//				 usecNow,
//				 m_sName.c_str(),
//				 m_senderState.m_n_x,
//				 m_statsEndToEnd.m_ping.m_nSmoothedPing,
//				 m_senderState.m_n_tx_s );

	m_receiverState.m_usec_tstamp_last_feedback = usecNow;

	// Recalc send now that we have rtt
	SNP_UpdateX( usecNow );
}

//-----------------------------------------------------------------------------
EResult CSteamNetworkConnectionBase::SNP_SendMessage( SteamNetworkingMicroseconds usecNow, const void *pData, int cbData, ESteamNetworkingSendType eSendType )
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
	SNPSendMessage_t *pSendMessage = new SNPSendMessage_t();

	// Assign message number
	pSendMessage->m_nMsgNum = ++m_senderState.m_nLastSentMsgNum;

	// Reliable, or unreliable?
	if ( eSendType & k_nSteamNetworkingSendFlags_Reliable )
	{
		pSendMessage->m_nReliableStreamPos = m_senderState.m_nReliableStreamPos;

		// Generate the header
		byte hdr[ 32 ];
		hdr[0] = 0;
		byte *hdrEnd = hdr+1;
		int64 nMsgNumGap = pSendMessage->m_nMsgNum - m_senderState.m_nLastSendMsgNumReliable;
		Assert( nMsgNumGap >= 1 );
		if ( nMsgNumGap > 1 )
		{
			hdrEnd = SerializeVarInt( hdrEnd, (uint64)nMsgNumGap );
			hdr[0] |= 0x40;
		}
		if ( cbData < 0x20 )
		{
			hdr[0] |= (byte)cbData;
		}
		else
		{
			hdr[0] |= (byte)( 0x20 | ( cbData & 0x1f ) );
			hdrEnd = SerializeVarInt( hdrEnd, cbData>>5U );
		}
		int cbHdr = hdrEnd - hdr;

		// Copy the data into the message, with the header prepended
		pSendMessage->m_cbSize = cbHdr+cbData;
		pSendMessage->m_pData = new uint8[ pSendMessage->m_cbSize ];
		memcpy( pSendMessage->m_pData, hdr, cbHdr );
		memcpy( pSendMessage->m_pData+cbHdr, pData, cbData );

		// Advance stream pointer
		m_senderState.m_nReliableStreamPos += pSendMessage->m_cbSize;

		// Update stats
		++m_senderState.m_nMessagesSentReliable;
		m_senderState.m_cbPendingReliable += pSendMessage->m_cbSize;

		// Remember last sent reliable message number, so we can know how to
		// encode the next one
		m_senderState.m_nLastSendMsgNumReliable = pSendMessage->m_nMsgNum;
	}
	else
	{

		// Just copy the data
		pSendMessage->m_pData = new uint8[ cbData ];
		pSendMessage->m_cbSize = cbData;
		memcpy( pSendMessage->m_pData, pData, cbData );

		pSendMessage->m_nReliableStreamPos = 0;

		++m_senderState.m_nMessagesSentUnreliable;
		m_senderState.m_cbPendingUnreliable += pSendMessage->m_cbSize;
	}

	// Add to pending list
	m_senderState.m_messagesQueued.push_back( pSendMessage );
	SpewType( steamdatagram_snp_log_message, "%s: SendMessage %s: MsgNum=%lld sz=%d\n",
				 m_sName.c_str(),
				 ( eSendType & k_nSteamNetworkingSendFlags_Reliable ) ? "RELIABLE" : "UNRELIABLE",
				 (long long)pSendMessage->m_nMsgNum,
				 pSendMessage->m_cbSize );

	// Use Nagle?
	// We always set the Nagle timer, even if we immediately clear it.  This makes our clearing code simpler,
	// since we can always safely assume that once we find a message with the nagle timer cleared, all messages
	// queued earlier than this also have it cleared.
	pSendMessage->m_usecNagle = usecNow + steamdatagram_snp_nagle_time;
	if ( eSendType & k_nSteamNetworkingSendFlags_NoNagle )
		m_senderState.ClearNagleTimers();

	// Schedule wakeup at the appropriate time.  (E.g. right now, if we're ready to send, 
	// or at the Nagle time, if Nagle is active.)
	if ( GetState() == k_ESteamNetworkingConnectionState_Connected )
	{
		SteamNetworkingMicroseconds usecNextThink = SNP_GetNextThinkTime( usecNow );

		// If we are rate limiting, spew about it
		if ( m_senderState.m_messagesQueued.m_pFirst->m_usecNagle == 0 && usecNextThink > usecNow )
		{
			SpewVerbose( "%12llu %s: RATELIM QueueTime is %.1fms, SendRate=%.1fk, BytesQueued=%d\n", 
				usecNow,
				m_sName.c_str(),
				m_senderState.CalcTimeUntilNextSend() * 1e-3,
				m_senderState.m_n_x * ( 1.0/1024.0),
				m_senderState.PendingBytesTotal()
			);
		}

		// Set a wakeup call.  If this is newer than the next time anything else needs to
		// wake up (e.g. the important case of "ASAP"), then it will trigger the service
		// thread to wake up, and so if we are ready to send packets right now, it should
		// begin very soon in the other thread, as soon as this thread releases the lock
		EnsureMinThinkTime( usecNextThink, +1 );
	}

	return k_EResultOK;
}

EResult CSteamNetworkConnectionBase::SNP_FlushMessage( SteamNetworkingMicroseconds usecNow )
{
	// If we're not connected, then go ahead and mark the messages ready to send
	// once we connect, but otherwise don't take any action
	if ( GetState() != k_ESteamNetworkingConnectionState_Connected )
	{
		m_senderState.ClearNagleTimers();
		return k_EResultIgnored;
	}

	if ( m_senderState.m_messagesQueued.empty() )
		return k_EResultOK;

	// If no Nagle timer was set, then there's nothing to do, we should already
	// be properly scheduled.  Don't do work to re-discover that fact.
	if ( m_senderState.m_messagesQueued.m_pLast->m_usecNagle == 0 )
		return k_EResultOK;

	// Accumulate tokens, and also limit to reasonable burst
	// if we weren't already waiting to send before this.
	// (Clearing the Nagle timers might very well make us want to
	// send so we want to do this first.)
	m_senderState.TokenBucket_Accumulate( usecNow );

	// Clear all Nagle timers
	m_senderState.ClearNagleTimers();

	// Schedule wakeup at the appropriate time.  (E.g. right now, if we're ready to send.)
	SteamNetworkingMicroseconds usecNextThink = SNP_GetNextThinkTime( usecNow );
	EnsureMinThinkTime( usecNextThink, +1 );
	return k_EResultOK;
}

bool CSteamNetworkConnectionBase::SNP_RecvDataChunk( int64 nPktNum, const void *pChunk, int cbChunk, int cbPacketSize, SteamNetworkingMicroseconds usecNow )
{
	#define DECODE_ERROR( ... ) do { \
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, __VA_ARGS__ ); \
		return false; } while(false)

	#define EXPECT_BYTES(n,pszWhatFor) \
		do { \
			if ( pDecode + (n) > pEnd ) \
				DECODE_ERROR( "SNP decode overrun, %d bytes for %s", (n), pszWhatFor ); \
		} while (false)

	#define READ_8BITU( var, pszWhatFor ) \
		do { EXPECT_BYTES(1,pszWhatFor); var = *(uint8 *)pDecode; pDecode += 1; } while(false)

	#define READ_16BITU( var, pszWhatFor ) \
		do { EXPECT_BYTES(2,pszWhatFor); var = LittleWord(*(uint16 *)pDecode); pDecode += 2; } while(false)

	#define READ_24BITU( var, pszWhatFor ) \
		do { EXPECT_BYTES(3,pszWhatFor); \
			var = *(uint8 *)pDecode; pDecode += 1; \
			var |= uint32( LittleWord(*(uint16 *)pDecode) ) << 8U; pDecode += 2; \
		} while(false)

	#define READ_32BITU( var, pszWhatFor ) \
		do { EXPECT_BYTES(4,pszWhatFor); var = LittleDWord(*(uint32 *)pDecode); pDecode += 4; } while(false)

	#define READ_48BITU( var, pszWhatFor ) \
		do { EXPECT_BYTES(6,pszWhatFor); \
			var = LittleWord( *(uint16 *)pDecode ); pDecode += 2; \
			var |= uint64( LittleDWord(*(uint32 *)pDecode) ) << 16U; pDecode += 4; \
		} while(false)

	#define READ_64BITU( var, pszWhatFor ) \
		do { EXPECT_BYTES(8,pszWhatFor); var = LittleQWord(*(uint64 *)pDecode); pDecode += 2; } while(false)

	#define READ_VARINT( var, pszWhatFor ) \
		do { pDecode = DeserializeVarInt( pDecode, pEnd, var ); if ( !pDecode ) { DECODE_ERROR( "SNP data chunk decode overflow, varint for %s", pszWhatFor ); } } while(false)

	#define READ_SEGMENT_DATA_SIZE( is_reliable ) \
		int cbSegmentSize; \
		{ \
			int sizeFlags = nFrameType & 7; \
			if ( sizeFlags <= 4 ) \
			{ \
				uint8 lowerSizeBits; \
				READ_8BITU( lowerSizeBits, #is_reliable " size lower bits" ); \
				cbSegmentSize = (sizeFlags<<8) + lowerSizeBits; \
				if ( pDecode + cbSegmentSize > pEnd ) \
				{ \
					DECODE_ERROR( "SNP decode overrun %d bytes for %s segment data.", cbSegmentSize, #is_reliable ); \
				} \
			} \
			else if ( sizeFlags == 7 ) \
			{ \
				cbSegmentSize = pEnd - pDecode; \
			} \
			else \
			{ \
				DECODE_ERROR( "Invalid SNP frame lead byte 0x%02x. (size bits)", nFrameType ); \
			} \
		} \
		const uint8 *pSegmentData = pDecode; \
		pDecode += cbSegmentSize;

	// Make sure we have initialized the connection
	Assert( BStateIsConnectedForWirePurposes() );

	SpewType( steamdatagram_snp_log_packet, "%s decode pkt %lld\n",
		m_sName.c_str(), (long long)nPktNum );

	// Decode frames until we get to the end of the payload
	const byte *pDecode = (const byte *)pChunk;
	const byte *pEnd = pDecode + cbChunk;
	int64 nCurMsgNum = 0;
	int64 nDecodeReliablePos = 0;
	while ( pDecode < pEnd )
	{

		uint8 nFrameType = *pDecode;
		++pDecode;
		if ( ( nFrameType & 0xc0 ) == 0x00 )
		{

			//
			// Unreliable segment
			//

			// Decode message number
			if ( nCurMsgNum == 0 )
			{
				// First unreliable frame.  Message number is absolute, but only bottom N bits are sent
				static const char szUnreliableMsgNumOffset[] = "unreliable msgnum";
				int64 nLowerBits, nMask;
				if ( nFrameType & 0x10 )
				{
					READ_32BITU( nLowerBits, szUnreliableMsgNumOffset );
					nMask = 0xffffffff;
					nCurMsgNum = NearestWithSameLowerBits( (int32)nLowerBits, m_receiverState.m_nHighestSeenMsgNum );
				}
				else
				{
					READ_16BITU( nLowerBits, szUnreliableMsgNumOffset );
					nMask = 0xffff;
					nCurMsgNum = NearestWithSameLowerBits( (int16)nLowerBits, m_receiverState.m_nHighestSeenMsgNum );
				}
				Assert( ( nCurMsgNum & nMask ) == nLowerBits );

				if ( nCurMsgNum <= 0 )
				{
					DECODE_ERROR( "SNP decode unreliable msgnum underflow.  %llx mod %llx, highest seen %llx",
						(unsigned long long)nLowerBits, (unsigned long long)( nMask+1 ), (unsigned long long)m_receiverState.m_nHighestSeenMsgNum );
				}
				if ( std::abs( nCurMsgNum - m_receiverState.m_nHighestSeenMsgNum ) > (nMask>>2) )
				{
					// We really should never get close to this boundary.
					SpewWarningRateLimited( usecNow, "Sender sent abs unreliable message number using %llx mod %llx, highest seen %llx\n",
						(unsigned long long)nLowerBits, (unsigned long long)( nMask+1 ), (unsigned long long)m_receiverState.m_nHighestSeenMsgNum );
				}

			}
			else
			{
				if ( nFrameType & 0x10 )
				{
					uint64 nMsgNumOffset;
					READ_VARINT( nMsgNumOffset, "unreliable msgnum offset" );
					nCurMsgNum += nMsgNumOffset;
				}
				else
				{
					++nCurMsgNum;
				}
			}
			if ( nCurMsgNum > m_receiverState.m_nHighestSeenMsgNum )
				m_receiverState.m_nHighestSeenMsgNum = nCurMsgNum;

			//
			// Decode segment offset in message
			//
			uint32 nOffset = 0;
			if ( nFrameType & 0x08 )
				READ_VARINT( nOffset, "unreliable data offset" );

			//
			// Decode size, locate segment data
			//
			READ_SEGMENT_DATA_SIZE( unreliable )
			Assert( cbSegmentSize > 0 ); // !TEST! Bogus assert, zero byte messages are OK.  Remove after testing

			// Receive the segment
			bool bLastSegmentInMessage = ( nFrameType & 0x20 ) != 0;
			SNP_ReceiveUnreliableSegment( nCurMsgNum, nOffset, pSegmentData, cbSegmentSize, bLastSegmentInMessage, usecNow );
		}
		else if ( ( nFrameType & 0xe0 ) == 0x40 )
		{

			//
			// Reliable segment
			//

			// First reliable segment?
			if ( nDecodeReliablePos == 0 )
			{

				// Stream position is absolute.  How many bits?
				static const char szFirstReliableStreamPos[] = "first reliable streampos";
				int64 nOffset, nMask;
				switch ( nFrameType & (3<<3) )
				{
					case 0<<3: READ_24BITU( nOffset, szFirstReliableStreamPos ); nMask = (1ll<<24)-1; break;
					case 1<<3: READ_32BITU( nOffset, szFirstReliableStreamPos ); nMask = (1ll<<32)-1; break;
					case 2<<3: READ_48BITU( nOffset, szFirstReliableStreamPos ); nMask = (1ll<<48)-1; break;
					default: DECODE_ERROR( "Reserved reliable stream pos size" );
				}

				// What do we expect to receive next?
				int64 nExpectNextStreamPos = m_receiverState.m_nReliableStreamPos + len( m_receiverState.m_bufReliableStream );

				// Find the stream offset closest to that
				nDecodeReliablePos = ( nExpectNextStreamPos & ~nMask ) + nOffset;
				if ( nDecodeReliablePos + (nMask>>1) < nExpectNextStreamPos )
				{
					nDecodeReliablePos += nMask+1;
					Assert( ( nDecodeReliablePos & nMask ) == nOffset );
					Assert( nExpectNextStreamPos < nDecodeReliablePos );
					Assert( nExpectNextStreamPos + (nMask>>1) >= nDecodeReliablePos );
				}
				if ( nDecodeReliablePos <= 0 )
				{
					DECODE_ERROR( "SNP decode first reliable stream pos underflow.  %llx mod %llx, expected next %llx",
						(unsigned long long)nOffset, (unsigned long long)( nMask+1 ), (unsigned long long)nExpectNextStreamPos );
				}
				if ( std::abs( nDecodeReliablePos - nExpectNextStreamPos ) > (nMask>>2) )
				{
					// We really should never get close to this boundary.
					SpewWarningRateLimited( usecNow, "Sender sent reliable stream pos using %llx mod %llx, expected next %llx\n",
						(unsigned long long)nOffset, (unsigned long long)( nMask+1 ), (unsigned long long)nExpectNextStreamPos );
				}
			}
			else
			{
				// Subsequent reliable message encode the position as an offset from previous.
				static const char szOtherReliableStreamPos[] = "reliable streampos offset";
				int64 nOffset;
				switch ( nFrameType & (3<<3) )
				{
					case 0<<3: nOffset = 0; break;
					case 1<<3: READ_8BITU( nOffset, szOtherReliableStreamPos ); break;
					case 2<<3: READ_16BITU( nOffset, szOtherReliableStreamPos ); break;
					default: READ_32BITU( nOffset, szOtherReliableStreamPos ); break;
				}
				nDecodeReliablePos += nOffset;
			}

			//
			// Decode size, locate segment data
			//
			READ_SEGMENT_DATA_SIZE( reliable )

			// Ingest the segment.  If it seems fishy, abort processing of this packet
			// and do not acknowledge to the sender.
			if ( !SNP_ReceiveReliableSegment( nPktNum, nDecodeReliablePos, pSegmentData, cbSegmentSize, usecNow ) )
				return false;

			// Advance pointer for the next reliable segment, if any.
			nDecodeReliablePos += cbSegmentSize;

			// Decoding rules state that if we have established a message number,
			// (from an earlier unreliable message), then we advance it.
			if ( nCurMsgNum > 0 ) 
				++nCurMsgNum;
		}
		else if ( ( nFrameType & 0xfc ) == 0x80 )
		{
			//
			// Stop waiting
			//

			int64 nOffset = 0;
			static const char szStopWaitingOffset[] = "stop_waiting offset";
			switch ( nFrameType & 3 )
			{
				case 0: READ_8BITU( nOffset, szStopWaitingOffset ); break;
				case 1: READ_16BITU( nOffset, szStopWaitingOffset ); break;
				case 2: READ_24BITU( nOffset, szStopWaitingOffset ); break;
				case 3: READ_64BITU( nOffset, szStopWaitingOffset ); break;
			}
			if ( nOffset >= nPktNum )
			{
				DECODE_ERROR( "stop_waiting pktNum %llu offset %llu", nPktNum, nOffset );
			}
			++nOffset;
			int64 nMinPktNumToSendAcks = nPktNum-nOffset;
			if ( nMinPktNumToSendAcks == m_receiverState.m_nMinPktNumToSendAcks )
				continue;
			if ( nMinPktNumToSendAcks < m_receiverState.m_nMinPktNumToSendAcks )
			{
				// Sender must never reduce this number!  Check for bugs or bogus sender
				if ( nPktNum >= m_receiverState.m_nPktNumUpdatedMinPktNumToSendAcks )
				{
					DECODE_ERROR( "SNP stop waiting reduced %lld (pkt %lld) -> %lld (pkt %lld)",
						(long long)m_receiverState.m_nMinPktNumToSendAcks,
						(long long)m_receiverState.m_nPktNumUpdatedMinPktNumToSendAcks,
						(long long)nMinPktNumToSendAcks,
						(long long)nPktNum
						);
				}
				continue;
			}
			SpewType( steamdatagram_snp_log_packet+1, "  %s decode pkt %lld stop waiting: %lld (was %lld)",
				m_sName.c_str(),
				(long long)nPktNum,
				(long long)nMinPktNumToSendAcks, (long long)m_receiverState.m_nMinPktNumToSendAcks );
			m_receiverState.m_nMinPktNumToSendAcks = nMinPktNumToSendAcks;
			m_receiverState.m_nPktNumUpdatedMinPktNumToSendAcks = nPktNum;

			// Trim from the front of the packet gap list,
			// we can stop reporting these losses to the sender
			while ( !m_receiverState.m_mapPacketGaps.empty() )
			{
				auto h = m_receiverState.m_mapPacketGaps.begin();
				if ( h->first > m_receiverState.m_nMinPktNumToSendAcks )
					break;
				if ( h->second.m_nEnd > m_receiverState.m_nMinPktNumToSendAcks )
				{
					// Ug.  You're not supposed to modify the key in a map.
					// I suppose that's legit, since you could violate the ordering.
					// but in this case I know that this change is OK.
					const_cast<int64 &>( h->first ) = m_receiverState.m_nMinPktNumToSendAcks;
					break;
				}
				m_receiverState.m_mapPacketGaps.erase(h);
			}
		}
		else if ( ( nFrameType & 0xf0 ) == 0x90 )
		{

			//
			// Ack
			//

			// Parse latest received sequence number
			int64 nLatestRecvSeqNum;
			{
				static const char szAckLatestPktNum[] = "ack latest pktnum";
				int64 nLowerBits, nMask;
				if ( nFrameType & 0x40 )
				{
					READ_32BITU( nLowerBits, szAckLatestPktNum );
					nMask = 0xffffffff;
					nLatestRecvSeqNum = NearestWithSameLowerBits( (int32)nLowerBits, m_statsEndToEnd.m_nNextSendSequenceNumber );
				}
				else
				{
					READ_16BITU( nLowerBits, szAckLatestPktNum );
					nMask = 0xffff;
					nLatestRecvSeqNum = NearestWithSameLowerBits( (int16)nLowerBits, m_statsEndToEnd.m_nNextSendSequenceNumber );
				}
				Assert( ( nLatestRecvSeqNum & nMask ) == nLowerBits );

				// Find the message number that is closes to 
				if ( nLatestRecvSeqNum < 0 )
				{
					DECODE_ERROR( "SNP decode ack latest pktnum underflow.  %llx mod %llx, next send %llx",
						(unsigned long long)nLowerBits, (unsigned long long)( nMask+1 ), (unsigned long long)m_statsEndToEnd.m_nNextSendSequenceNumber );
				}
				if ( std::abs( nLatestRecvSeqNum - m_statsEndToEnd.m_nNextSendSequenceNumber ) > (nMask>>2) )
				{
					// We really should never get close to this boundary.
					SpewWarningRateLimited( usecNow, "Sender sent abs latest recv pkt number using %llx mod %llx, next send %llx\n",
						(unsigned long long)nLowerBits, (unsigned long long)( nMask+1 ), (unsigned long long)m_statsEndToEnd.m_nNextSendSequenceNumber );
				}
				if ( nLatestRecvSeqNum >= m_statsEndToEnd.m_nNextSendSequenceNumber )
				{
					DECODE_ERROR( "SNP decode ack latest pktnum %lld (%llx mod %llx), but next outoing packet is %lld (%llx).",
						(long long)nLatestRecvSeqNum, (unsigned long long)nLowerBits, (unsigned long long)( nMask+1 ),
						(long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (unsigned long long)m_statsEndToEnd.m_nNextSendSequenceNumber
					);
				}
			}

			SpewType( steamdatagram_snp_log_packet+1, "  %s decode pkt %lld latest recv %lld\n",
				m_sName.c_str(),
				(long long)nPktNum, (long long)nLatestRecvSeqNum
			);

			// Locate our bookkeeping for this packet, or the latest one before it
			// Remember, we have a sentinel with a low, invalid packet number
			Assert( !m_senderState.m_mapInFlightPacketsByPktNum.empty() );
			auto inFlightPkt = m_senderState.m_mapInFlightPacketsByPktNum.upper_bound( nLatestRecvSeqNum );
			--inFlightPkt;
			Assert( inFlightPkt->first <= nLatestRecvSeqNum );

			// Parse out delay, and process the ping
			{
				uint16 nPackedDelay;
				READ_16BITU( nPackedDelay, "ack delay" );
				if ( nPackedDelay != 0xffff && inFlightPkt->first == nLatestRecvSeqNum )
				{
					SteamNetworkingMicroseconds usecDelay = SteamNetworkingMicroseconds( nPackedDelay ) << k_nAckDelayPrecisionShift;
					SteamNetworkingMicroseconds usecElapsed = usecNow - inFlightPkt->second.m_usecWhenSent;
					Assert( usecElapsed >= 0 );

					// Account for their reported delay, and calculate ping, in MS
					int msPing = ( usecElapsed - usecDelay ) / 1000;

					// Does this seem bogus?  (We allow a small amount of slop.)
					// NOTE: A malicious sender could lie about this delay, tricking us
					// into thinking that the real network latency is low, they are just
					// delaying their replies.  This actually matters, since the ping time
					// is an input into the rate calculation.  So we might need to
					// occasionally send pings that require an immediately reply, and
					// if those ping times seem way out of whack with the ones where they are
					// allowed to send a delay, take action against them.
					if ( msPing < -1 )
					{
						// Either they are lying or some weird timer stuff is happening.
						// Either way, discard it.

						SpewType( steamdatagram_snp_log_ackrtt, "%s decode pkt %lld latest recv %lld delay %lluusec INVALID ping %lldusec\n",
							m_sName.c_str(),
							(long long)nPktNum, (long long)nLatestRecvSeqNum,
							(unsigned long long)usecDelay,
							(long long)usecElapsed
						);
					}
					else
					{
						// Clamp, if we have slop
						if ( msPing < 0 )
							msPing = 0;
						m_statsEndToEnd.m_ping.ReceivedPing( msPing, usecNow );

						// Spew
						SpewType( steamdatagram_snp_log_ackrtt, "%s decode pkt %lld latest recv %lld delay %.1fms ping %.1fms\n",
							m_sName.c_str(),
							(long long)nPktNum, (long long)nLatestRecvSeqNum,
							(float)(usecDelay * 1e-3 ),
							(float)(usecElapsed * 1e-3 )
						);
					}
				}
			}

			// Parse number of blocks
			int nBlocks = nFrameType&7;
			if ( nBlocks == 7 )
				READ_8BITU( nBlocks, "ack num blocks" );

			// If they actually sent us any blocks, that means they are fragmented.
			// We should make sure and tell them to stop sending us these nacks
			// and move forward.  This could be more robust, if we remember when
			// the last stop_waiting value we sent was, and when we sent it.
			if ( nBlocks > 0 )
			{
				// Decrease flush delay the more blocks they send us.
				SteamNetworkingMicroseconds usecDelay = 250*1000 / nBlocks;
				m_receiverState.m_usecWhenFlushAck = std::min( m_receiverState.m_usecWhenFlushAck, usecNow + usecDelay );
			}

			// Process ack blocks, working backwards from the latest received sequence number.
			// Note that we have to parse all this stuff out, even if it's old news (packets older
			// than the stop_aiting value we sent), because we need to do that to get to the rest
			// of the packet.
			bool bAckedReliableRange = false;
			int64 nPktNumAckEnd = nLatestRecvSeqNum+1;
			while ( nBlocks >= 0 )
			{

				// Parse out number of acks/nacks.
				// Have we parsed all the real blocks?
				int64 nPktNumAckBegin, nPktNumNackBegin;
				if ( nBlocks == 0 )
				{
					// Implicit block.  Everything earlier between the last
					// NACK and the stop_waiting value is implicitly acked!
					if ( nPktNumAckEnd <= m_senderState.m_nMinPktWaitingOnAck )
						break;

					nPktNumAckBegin = m_senderState.m_nMinPktWaitingOnAck;
					nPktNumNackBegin = nPktNumAckBegin;
					SpewType( steamdatagram_snp_log_packet+1, "  %s decode pkt %lld ack last block ack begin %lld\n",
						m_sName.c_str(),
						(long long)nPktNum, (long long)nPktNumAckBegin );
				}
				else
				{
					uint8 nBlockHeader;
					READ_8BITU( nBlockHeader, "ack block header" );

					// Ack count?
					int64 numAcks = ( nBlockHeader>> 4 ) & 7;
					if ( nBlockHeader & 0x80 )
					{
						uint64 nUpperBits;
						READ_VARINT( nUpperBits, "ack count upper bits" );
						if ( nUpperBits > 100000 )
							DECODE_ERROR( "Ack count of %llu<<3 is crazy", (unsigned long long)nUpperBits );
						numAcks |= nUpperBits<<3;
					}
					nPktNumAckBegin = nPktNumAckEnd - numAcks;
					if ( nPktNumAckBegin < 0 )
						DECODE_ERROR( "Ack range underflow, end=%lld, num=%lld", (long long)nPktNumAckEnd, (long long)numAcks );

					// Extended nack count?
					int64 numNacks = nBlockHeader & 7;
					if ( nBlockHeader & 0x08)
					{
						uint64 nUpperBits;
						READ_VARINT( nUpperBits, "nack count upper bits" );
						if ( nUpperBits > 100000 )
							DECODE_ERROR( "Nack count of %llu<<3 is crazy", nUpperBits );
						numNacks |= nUpperBits<<3;
					}
					nPktNumNackBegin = nPktNumAckBegin - numNacks;
					if ( nPktNumNackBegin < 0 )
						DECODE_ERROR( "Nack range underflow, end=%lld, num=%lld", (long long)nPktNumAckBegin, (long long)numAcks );

					SpewType( steamdatagram_snp_log_packet+1, "  %s decode pkt %lld nack [%lld,%lld) ack [%lld,%lld)\n",
						m_sName.c_str(),
						(long long)nPktNum,
						(long long)nPktNumNackBegin, (long long)( nPktNumNackBegin + numNacks ),
						(long long)nPktNumAckBegin, (long long)( nPktNumAckBegin + numAcks )
					);
				}

				// Process acks first.
				Assert( nPktNumAckBegin >= 0 );
				while ( inFlightPkt->first >= nPktNumAckBegin )
				{
					Assert( inFlightPkt->first < nPktNumAckEnd );

					// Scan reliable segments, and see if any are marked for retry or are in flight
					for ( const SNPRange_t &relRange: inFlightPkt->second.m_vecReliableSegments )
					{

						// If range is present, it should be in only one of these two tables.
						if ( m_senderState.m_listInFlightReliableRange.erase( relRange ) == 0 )
						{
							if ( m_senderState.m_listReadyRetryReliableRange.erase( relRange ) > 0 )
							{

								// When we put stuff into the reliable retry list, we mark it as pending again.
								// But now it's acked, so it's no longer pending, even though we didn't send it.
								m_senderState.m_cbPendingReliable -= int( relRange.length() );
								Assert( m_senderState.m_cbPendingReliable >= 0 );

								bAckedReliableRange = true;
							}
						}
						else
						{
							bAckedReliableRange = true;
							Assert( m_senderState.m_listReadyRetryReliableRange.count( relRange ) == 0 );
						}
					}

					// Check if this was the next packet we were going to timeout, then advance
					// pointer.  This guy didn't timeout.
					if ( inFlightPkt == m_senderState.m_itNextInFlightPacketToTimeout )
						++m_senderState.m_itNextInFlightPacketToTimeout;

					// No need to track this anymore, remove from our table
					inFlightPkt = m_senderState.m_mapInFlightPacketsByPktNum.erase( inFlightPkt );
					--inFlightPkt;
					Assert( !m_senderState.m_mapInFlightPacketsByPktNum.empty() );
				}

				// Process nacks.
				Assert( nPktNumNackBegin >= 0 );
				while ( inFlightPkt->first >= nPktNumNackBegin )
				{
					Assert( inFlightPkt->first < nPktNumAckEnd );
					SNP_SenderProcessPacketNack( inFlightPkt->first, inFlightPkt->second, "NACK" );

					// We'll keep the record on hand, though, in case an ACK comes in
					--inFlightPkt;
				}

				// Continue on to the the next older block
				nPktNumAckEnd = nPktNumNackBegin;
				--nBlocks;
			}

			// Should we check for discarding reliable messages we are keeping around in case
			// of retransmission, since we know now that they were delivered?
			if ( bAckedReliableRange )
			{
				m_senderState.RemoveAckedReliableMessageFromUnackedList();

				// Spew where we think the peer is decoding the reliable stream
				if ( g_eSteamDatagramDebugOutputDetailLevel <= k_ESteamNetworkingSocketsDebugOutputType_Debug )
				{

					int64 nPeerReliablePos = m_senderState.m_nReliableStreamPos;
					if ( !m_senderState.m_listInFlightReliableRange.empty() )
						nPeerReliablePos = std::min( nPeerReliablePos, m_senderState.m_listInFlightReliableRange.begin()->first.m_nBegin );
					if ( !m_senderState.m_listReadyRetryReliableRange.empty() )
						nPeerReliablePos = std::min( nPeerReliablePos, m_senderState.m_listReadyRetryReliableRange.begin()->first.m_nBegin );

					SpewType( steamdatagram_snp_log_packet+1, "  %s decode pkt %lld peer reliable pos = %lld\n",
						m_sName.c_str(),
						(long long)nPktNum, (long long)nPeerReliablePos );
				}
			}

			// Check if any of this was new info, then advance our stop_waiting value.
			if ( nLatestRecvSeqNum > m_senderState.m_nMinPktWaitingOnAck )
			{
				SpewType( steamdatagram_snp_log_packet, "  %s updating min_waiting_on_ack %lld -> %lld\n",
					m_sName.c_str(),
					(long long)m_senderState.m_nMinPktWaitingOnAck, (long long)nLatestRecvSeqNum );
				m_senderState.m_nMinPktWaitingOnAck = nLatestRecvSeqNum;
				//m_senderState.m_usecWhenAdvancedMinPktWaitingOnAck = usecNow;
			}
		}
		else
		{
			DECODE_ERROR( "Invalid SNP frame lead byte 0x%02x", nFrameType );
		}
	}

	// Update structures needed to populate our ACKs
	return SNP_RecordReceivedPktNum( nPktNum, usecNow );

	// Make sure these don't get used beyond where we intended them toget used
	#undef DECODE_ERROR
	#undef EXPECT_BYTES
	#undef READ_8BITU
	#undef READ_16BITU
	#undef READ_24BITU
	#undef READ_32BITU
	#undef READ_64BITU
	#undef READ_VARINT
	#undef READ_SEGMENT_DATA_SIZE
}

void CSteamNetworkConnectionBase::SNP_SenderProcessPacketNack( int64 nPktNum, SNPInFlightPacket_t &pkt, const char *pszDebug )
{

	// Did we already treat the packet as dropped (implicitly or explicitly)?
	if ( pkt.m_bNack )
		return;

	// Mark as dropped
	pkt.m_bNack = true;

	// Scan reliable segments
	for ( const SNPRange_t &relRange: pkt.m_vecReliableSegments )
	{

		// Marked as in-flight?
		auto inFlightRange = m_senderState.m_listInFlightReliableRange.find( relRange );
		if ( inFlightRange == m_senderState.m_listInFlightReliableRange.end() )
			continue;

		SpewType( steamdatagram_snp_log_packet, "%s pkt %lld %s, queueing retry of reliable range [%lld,%lld)\n", 
			m_sName.c_str(),
			nPktNum,
			pszDebug,
			relRange.m_nBegin, relRange.m_nEnd );

		// The ready-to-retry list counts towards the "pending" stat
		m_senderState.m_cbPendingReliable += int( relRange.length() );

		// Move it to the ready for retry list!
		// if shouldn't already be there!
		Assert( m_senderState.m_listReadyRetryReliableRange.count( relRange ) == 0 );
		m_senderState.m_listReadyRetryReliableRange[ inFlightRange->first ] = inFlightRange->second;
		m_senderState.m_listInFlightReliableRange.erase( inFlightRange );
	}
}

SteamNetworkingMicroseconds CSteamNetworkConnectionBase::SNP_SenderCheckInFlightPackets( SteamNetworkingMicroseconds usecNow )
{
	// Fast path for nothing in flight.
	Assert( !m_senderState.m_mapInFlightPacketsByPktNum.empty() );
	if ( m_senderState.m_mapInFlightPacketsByPktNum.size() == 1 )
	{
		Assert( m_senderState.m_itNextInFlightPacketToTimeout == m_senderState.m_mapInFlightPacketsByPktNum.end() );
		return k_nThinkTime_Never;
	}
	Assert( m_senderState.m_mapInFlightPacketsByPktNum.begin()->first < 0 );

	SteamNetworkingMicroseconds usecNextRetry = k_nThinkTime_Never;

	// Process retry timeout.  Here we use a shorter timeout to trigger retry
	// than we do to totally forgot about the packet, in case an ack comes in late,
	// we can take advantage of it.
	SteamNetworkingMicroseconds usecRTO = m_statsEndToEnd.CalcSenderRetryTimeout();
	while ( m_senderState.m_itNextInFlightPacketToTimeout != m_senderState.m_mapInFlightPacketsByPktNum.end() )
	{
		Assert( m_senderState.m_itNextInFlightPacketToTimeout->first > 0 );

		// If already nacked, then no use waiting on it, just skip it
		if ( !m_senderState.m_itNextInFlightPacketToTimeout->second.m_bNack )
		{

			// Not yet time to give up?
			SteamNetworkingMicroseconds usecRetryPkt = m_senderState.m_itNextInFlightPacketToTimeout->second.m_usecWhenSent + usecRTO;
			if ( usecRetryPkt > usecNow )
			{
				usecNextRetry = usecRetryPkt;
				break;
			}

			// Mark as dropped, and move any reliable contents into the
			// retry list.
			SNP_SenderProcessPacketNack( m_senderState.m_itNextInFlightPacketToTimeout->first, m_senderState.m_itNextInFlightPacketToTimeout->second, "AckTimeout" );
		}

		// Advance to next packet waiting to timeout
		++m_senderState.m_itNextInFlightPacketToTimeout;
	}

	// Skip the sentinel
	auto inFlightPkt = m_senderState.m_mapInFlightPacketsByPktNum.begin();
	Assert( inFlightPkt->first < 0 );
	++inFlightPkt;

	// Expire old packets (all of these should have been marked as nacked)
	SteamNetworkingMicroseconds usecNackExpiry = usecRTO*2;
	for (;;)
	{
		SteamNetworkingMicroseconds usecWhenExpiry = inFlightPkt->second.m_usecWhenSent - usecNackExpiry;
		if ( inFlightPkt->second.m_usecWhenSent > usecWhenExpiry )
			break;

		// Should have already been timed out by the code above
		Assert( inFlightPkt->second.m_bNack );
		Assert( inFlightPkt != m_senderState.m_itNextInFlightPacketToTimeout );

		// Expire it, advance to the next one
		inFlightPkt = m_senderState.m_mapInFlightPacketsByPktNum.erase( inFlightPkt );
		Assert( !m_senderState.m_mapInFlightPacketsByPktNum.empty() );

		// Bail if we've hit the end of the nacks
		if ( inFlightPkt == m_senderState.m_mapInFlightPacketsByPktNum.end() )
			break;
	}

	// Return time when we really need to check back in again.
	// We don't wake up early just to expire old nacked packets,
	// there is no urgency or value in doing that, we can clean
	// those up whenever.  We only make sure and wake up when we
	// need to retry.  (And we need to make sure we don't let
	// our list of old packets grow unnecessarily long.)
	return usecNextRetry;
}

struct EncodedSegment
{
	static constexpr int k_cbMaxHdr = 16; 
	uint8 m_hdr[ k_cbMaxHdr ];
	int m_cbHdr; // Doesn't include any size byte
	SNPSendMessage_t *m_pMsg;
	int m_cbSize;
	int m_nOffset;

	inline void SetupReliable( SNPSendMessage_t *pMsg, int64 nBegin, int64 nEnd, int64 nLastReliableStreamPosEnd )
	{
		Assert( nBegin < nEnd );
		Assert( nBegin + k_cbSteamNetworkingSocketsMaxReliableMessageSegment >= nEnd ); // Max sure we don't exceed max segment size
		Assert( pMsg->m_cbSize > 0 );

		// Start filling out the header with the top three bits = 010,
		// identifying this as a reliable segment
		uint8 *pHdr = m_hdr;
		*(pHdr++) = 0x40;

		// First reliable segment in the message?
		if ( nLastReliableStreamPosEnd == 0 )
		{
			// Always use 48-byte offsets, to make sure we are exercising the worst case.
			// Later we should optimize this
			m_hdr[0] |= 0x10;
			*(uint16*)pHdr = LittleWord( uint16( nBegin ) ); pHdr += 2;
			*(uint32*)pHdr = LittleDWord( uint32( nBegin>>16 ) ); pHdr += 4;
		}
		else
		{
			// Offset from end of previous reliable segment in the same packet
			Assert( nBegin >= nLastReliableStreamPosEnd );
			int64 nOffset = nBegin - nLastReliableStreamPosEnd;
			if ( nOffset == 0)
			{
				// Nothing to encode
			}
			else if ( nOffset < 0x100 )
			{
				m_hdr[0] |= (1<<3);
				*pHdr = uint8( nOffset ); pHdr += 1;
			}
			else if ( nOffset < 0x10000 )
			{
				m_hdr[0] |= (2<<3);
				*(uint16*)pHdr = LittleWord( uint16( nOffset ) ); pHdr += 2;
			}
			else
			{
				m_hdr[0] |= (3<<3);
				*(uint32*)pHdr = LittleDWord( uint32( nOffset ) ); pHdr += 4;
			}
		}

		m_cbHdr = pHdr-m_hdr;

		// Size of the segment.  We assume that the whole things fits for now,
		// even though it might need to get truncated
		int cbSegData = nEnd - nBegin;
		Assert( cbSegData > 0 );
		Assert( nBegin >= pMsg->m_nReliableStreamPos );
		Assert( nEnd <= pMsg->m_nReliableStreamPos + pMsg->m_cbSize );

		m_pMsg = pMsg;
		m_nOffset = nBegin - pMsg->m_nReliableStreamPos;
		m_cbSize = cbSegData;
	}

	inline void SetupUnreliable( SNPSendMessage_t *pMsg, int nOffset, int64 nLastMsgNum )
	{

		// Start filling out the header with the top two bits = 00,
		// identifying this as an unreliable segment
		uint8 *pHdr = m_hdr;
		*(pHdr++) = 0x00;

		// Encode message number.  First unreliable message?
		if ( nLastMsgNum == 0 )
		{

			// Just always encode message number with 32 bits for now,
			// to make sure we are hitting the worst case.  We can optimize this later
			*(uint32*)pHdr = LittleWord( (uint32)pMsg->m_nMsgNum ); pHdr += 4;
			m_hdr[0] |= 0x10;
		}
		else
		{
			// Subsequent unreliable message
			Assert( pMsg->m_nMsgNum > nLastMsgNum );
			uint64 nDelta = pMsg->m_nMsgNum - nLastMsgNum;
			if ( nDelta == 1 )
			{
				// Common case of sequential messages.  Don't encode any offset
			}
			else
			{
				pHdr = SerializeVarInt( pHdr, nDelta, m_hdr+k_cbMaxHdr );
				Assert( pHdr ); // Overflow shouldn't be possible
				m_hdr[0] |= 0x10;
			}
		}

		// Encode segment offset within message, except in the special common case of the first segment
		if ( nOffset > 0 )
		{
			pHdr = SerializeVarInt( pHdr, (uint32)( nOffset ), m_hdr+k_cbMaxHdr );
			Assert( pHdr ); // Overflow shouldn't be possible
			m_hdr[0] |= 0x08;
		}

		m_cbHdr = pHdr-m_hdr;

		// Size of the segment.  We assume that the whole things fits for now, event hough it might ned to get truncated
		int cbSegData = pMsg->m_cbSize - nOffset;
		Assert( cbSegData > 0 || ( cbSegData == 0 && pMsg->m_cbSize == 0 ) ); // We should only send zero-byte segments if the message itself is zero bytes.  (Which is legitimate!)

		m_pMsg = pMsg;
		m_cbSize = cbSegData;
		m_nOffset = nOffset;
	}

};

template <typename T, typename L>
inline bool HasOverlappingRange( const SNPRange_t &range, const std::map<SNPRange_t,T,L> &map )
{
	auto l = map.lower_bound( range );
	if ( l != map.end() )
	{
		Assert( l->first.m_nBegin >= range.m_nBegin );
		if ( l->first.m_nBegin < range.m_nEnd )
			return true;
	}
	auto u = map.upper_bound( range );
	if ( u != map.end() )
	{
		Assert( range.m_nBegin < u->first.m_nBegin );
		if ( range.m_nEnd > l->first.m_nBegin )
			return true;
	}

	return false;
}

int CSteamNetworkConnectionBase::SNP_SendPacket( SteamNetworkingMicroseconds usecNow, int cbMaxEncryptedPayload, void *pConnectionData )
{
	// If we aren't being specifically asked to send a packet, and we don't have anything to send,
	// then don't send right now.
	if ( pConnectionData == nullptr && usecNow < m_receiverState.m_usecWhenFlushAck && m_senderState.TimeWhenWantToSendNextPacket() > usecNow && m_receiverState.m_usecWhenFlushAck > usecNow )
		return 0;

	// Make sure we have initialized the connection
	Assert( BStateIsConnectedForWirePurposes() );
	Assert( !m_senderState.m_mapInFlightPacketsByPktNum.empty() );

	// Check if they are asking us to make room
	int cbMaxPlaintextPayload = k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend;
	if ( cbMaxEncryptedPayload < k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend )
	{
		COMPILE_TIME_ASSERT( ( k_cbSteamNetworkingSocketsEncryptionBlockSize & (k_cbSteamNetworkingSocketsEncryptionBlockSize-1) ) == 0 ); // key size should be power of two
		cbMaxPlaintextPayload = ( cbMaxEncryptedPayload - 1 ) & ~(k_cbSteamNetworkingSocketsEncryptionBlockSize-1); // we need at least one byte of padding, and then round up to multiple of key size
		cbMaxPlaintextPayload = std::max( 0, cbMaxPlaintextPayload );
	}

	uint8 payload[ k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend ];
	uint8 *pPayloadEnd = payload + cbMaxPlaintextPayload;
	uint8 *pPayloadPtr = payload;

	SpewType( steamdatagram_snp_log_packet, "%s encode pkt %lld",
		m_sName.c_str(),
		(long long)m_statsEndToEnd.m_nNextSendSequenceNumber );

	// Get list of ack blocks we might want to serialize
	SNPAckSerializerHelper ackHelper;
	SNP_GatherAckBlocks( ackHelper, usecNow );

	// Stop waiting frame
	pPayloadPtr = SNP_SerializeStopWaitingFrame( pPayloadPtr, pPayloadEnd, usecNow );
	if ( pPayloadPtr == nullptr )
		return -1;

	// Should we try to send as many acks as possible?
	int cbReserveForAcks = 0;
	int cbFlushedAcks = 0;
	if ( m_receiverState.m_usecWhenFlushAck <= usecNow )
	{
		uint8 *pAfterAck = SNP_SerializeAckBlocks( ackHelper, pPayloadPtr, pPayloadEnd, usecNow );
		if ( pAfterAck == nullptr )
			return -1; // bug!  Abort

		// Did anything fit?
		if ( pAfterAck > pPayloadPtr )
		{
			cbFlushedAcks = pAfterAck - pPayloadPtr;
			pPayloadPtr = pAfterAck;
			if ( m_receiverState.m_usecWhenFlushAck == INT64_MAX )
			{
				SpewType( steamdatagram_snp_log_packet, "%s flushed %d acks (%d bytes)\n", m_sName.c_str(), ackHelper.m_nBlocks, cbFlushedAcks );
			}
			else
			{
				SpewType( steamdatagram_snp_log_packet, "%s flush didn't fit; rescheduling\n", m_sName.c_str() );

				// If we are artificially limited, then the connection
				// type specific code initiated this packet, and so
				// let's keep the timer set and we'll try again on our own terms.
				// But if we have quite a bit of space and we still failed,
				// then we really are badly fragmented.  In that case, don't
				// keep trying to ack over and over in every single packet.
				if ( cbMaxPlaintextPayload > 128 )
					m_receiverState.m_usecWhenFlushAck = usecNow + 50*1000;
			}
		}
	}
	else if ( m_statsEndToEnd.m_nLastRecvSequenceNumber > 0 )
	{
		// Should we try to reserve a bit of space for acks?
		// If possible, always send at least a few blocks (if we have them)
		int cbPayloadRemainingForBlocks = pPayloadEnd - pPayloadPtr - SNPAckSerializerHelper::k_cbHeaderSize;
		if ( cbPayloadRemainingForBlocks >= 0 )
		{
			cbReserveForAcks = SNPAckSerializerHelper::k_cbHeaderSize;
			int n = std::min( 3, ackHelper.m_nBlocks );
			while ( n > 0 )
			{
				--n;
				if ( ackHelper.m_arBlocks[n].m_cbTotalEncodedSize <= cbPayloadRemainingForBlocks )
				{
					cbReserveForAcks += ackHelper.m_arBlocks[n].m_cbTotalEncodedSize;
					break;
				}
			}
		}
	}

	// Check if we don't actually have room to send any data, then don't.
	// (This means that acks or other responsibilities are choking the pipe
	// and should basically never happen in ordinary circumstances!)
	if ( m_senderState.m_flTokenBucket < 0.0 )
	{
		SpewWarningRateLimited( usecNow, "%s exceeding rate limit just sending acks / stats!  Not sending any data!", m_sName.c_str() );

		// Serialize some acks, if we want to
		if ( cbReserveForAcks > 0 )
		{
			pPayloadPtr = SNP_SerializeAckBlocks( ackHelper, pPayloadPtr, std::min( pPayloadPtr+cbReserveForAcks, pPayloadEnd ), usecNow );
			if ( pPayloadPtr == nullptr )
				return -1; // bug!  Abort
			cbReserveForAcks = 0;
		}

		// No more
		pPayloadEnd = pPayloadPtr;
	}

	int64 nLastReliableStreamPosEnd = 0;
	int cbBytesRemainingForSegments = pPayloadEnd - pPayloadPtr - cbReserveForAcks;
	vstd::small_vector<EncodedSegment,8> vecSegments;

	// If we need to *retry* any reliable data, then try to put that in first.
	// Bail if we only have a tiny sliver of data left
	while ( !m_senderState.m_listReadyRetryReliableRange.empty() && cbBytesRemainingForSegments > 2 )
	{
		auto h = m_senderState.m_listReadyRetryReliableRange.begin();

		// Start a reliable segment
		EncodedSegment &seg = *push_back_get_ptr( vecSegments );
		seg.SetupReliable( h->second, h->first.m_nBegin, h->first.m_nEnd, nLastReliableStreamPosEnd );
		int cbSegTotalWithoutSizeField = seg.m_cbHdr + seg.m_cbSize;
		if ( cbSegTotalWithoutSizeField > cbBytesRemainingForSegments )
		{
			// This one won't fit.
			vecSegments.pop_back();

			// FIXME If there's a decent amount of space left in this packet, it might
			// be worthwhile to send what we can.  Right now, once we send a reliable range,
			// we always retry exactly that range.  The only complication would be when we
			// receive an ack, we would need to be aware that the acked ranges might not
			// exactly match up with the ranges that we sent.  Actually this shouldn't
			// be that big of a deal.  But for now let's always retry the exact ranges that
			// things got chopped up during the initial send.

			// This should only happen if we have already fit some data in, or
			// the caller asked us to see what we could squeeze into a smaller
			// packet, or we need to serialized a bunch of acks.  If this is an
			// opportunity to fill a normal packet and we fail on the first segment,
			// we will never make progress and we are hosed!
			AssertMsg2(
				nLastReliableStreamPosEnd > 0
				|| cbMaxPlaintextPayload < k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend
				|| cbFlushedAcks > 20,
				"We cannot fit reliable segment, need %d bytes, only %d remaining", cbSegTotalWithoutSizeField, cbBytesRemainingForSegments
			);

			// Don't try to put more stuff in the packet, even if we have room.  We're
			// already having to retry, so this data is already delayed.  If we skip ahead
			// and put more into this packet, that's just extending the time until we can send
			// the next packet.
			break;
		}

		// If we only have a sliver left, then don't try to fit any more.
		cbBytesRemainingForSegments -= cbSegTotalWithoutSizeField;
		nLastReliableStreamPosEnd = h->first.m_nEnd;

		// Assume for now this won't be the last segment, in which case we will also need
		// the byte for the size field.
		// NOTE: This might cause cbPayloadBytesRemaining to go negative by one!  I know
		// that seems weird, but it actually keeps the logic below simpler.
		cbBytesRemainingForSegments -= 1;

		// Remove from retry list.  (We'll add to the in-flight list later)
		m_senderState.m_listReadyRetryReliableRange.erase( h );
	}

	// Did we retry everything we needed to?  If not, then don't try to send new stuff,
	// before we send those retries.
	if ( m_senderState.m_listReadyRetryReliableRange.empty() )
	{

		// OK, check the outgoing messages, and send as much stuff as we can cram in there
		int64 nLastMsgNum = 0;
		while ( cbBytesRemainingForSegments > 4 )
		{
			if ( m_senderState.m_messagesQueued.empty() )
			{
				m_senderState.m_cbCurrentSendMessageSent = 0;
				break;
			}
			SNPSendMessage_t *pSendMsg = m_senderState.m_messagesQueued.m_pFirst;
			Assert( m_senderState.m_cbCurrentSendMessageSent < pSendMsg->m_cbSize );

			// Start a new segment
			EncodedSegment &seg = *push_back_get_ptr( vecSegments );

			// Reliable?
			bool bLastSegment = false;
			if ( pSendMsg->m_nReliableStreamPos )
			{

				// FIXME - Coalesce adjacent reliable messages ranges

				int64 nBegin = pSendMsg->m_nReliableStreamPos + m_senderState.m_cbCurrentSendMessageSent;

				// How large would we like this segment to be,
				// ignoring how much space is left in the packet.
				// We limit the size of reliable segments, to make
				// sure that we don't make an excessively large
				// one and then have a hard time retrying it later.
				int cbDesiredSegSize = pSendMsg->m_cbSize - m_senderState.m_cbCurrentSendMessageSent;
				if ( cbDesiredSegSize > k_cbSteamNetworkingSocketsMaxReliableMessageSegment )
				{
					cbDesiredSegSize = k_cbSteamNetworkingSocketsMaxReliableMessageSegment;
					bLastSegment = true;
				}

				int64 nEnd = nBegin + cbDesiredSegSize;
				seg.SetupReliable( pSendMsg, nBegin, nEnd, nLastReliableStreamPosEnd );

				// If we encode subsequent 
				nLastReliableStreamPosEnd = nEnd;
			}
			else
			{
				seg.SetupUnreliable( pSendMsg, m_senderState.m_cbCurrentSendMessageSent, nLastMsgNum );
			}

			// Can't fit the whole thing?
			if ( bLastSegment || seg.m_cbHdr + seg.m_cbSize > cbBytesRemainingForSegments )
			{

				// Check if we have enough room to send anything worthwhile.
				// Don't send really tiny silver segments at the very end of a packet.  That sort of fragmentation
				// just makes it more likely for something to drop.  Our goal is to reduce the number of packets
				// just as much as the total number of bytes, so if we're going to have to send another packet
				// anyway, don't send a little sliver of a message at the beginning of a packet
				// We need to finish the header by this point if we're going to send anything
				int cbMinSegDataSizeToSend = std::min( 16, seg.m_cbSize );
				if ( seg.m_cbHdr + cbMinSegDataSizeToSend > cbBytesRemainingForSegments )
				{
					// Don't send this segment now.
					vecSegments.pop_back();
					break;
				}

				// Truncate, and leave the message in the queue
				seg.m_cbSize = std::min( seg.m_cbSize, cbBytesRemainingForSegments - seg.m_cbHdr );
				m_senderState.m_cbCurrentSendMessageSent += seg.m_cbSize;
				Assert( m_senderState.m_cbCurrentSendMessageSent < pSendMsg->m_cbSize );
				cbBytesRemainingForSegments -= seg.m_cbHdr + seg.m_cbSize;
				break;
			}

			// The whole message fit (perhaps exactly, without the size byte)
			// Reset send pointer for the next message
			Assert( m_senderState.m_cbCurrentSendMessageSent + seg.m_cbSize == pSendMsg->m_cbSize );
			m_senderState.m_cbCurrentSendMessageSent = 0;

			// Remove message from queue,w e have transfered ownership to the segment and will
			// dispose of the message when we serialize the segments
			m_senderState.m_messagesQueued.pop_front();

			// Consume payload bytes
			cbBytesRemainingForSegments -= seg.m_cbHdr + seg.m_cbSize;

			// Assume for now this won't be the last segment, in which case we will also need the byte for the size field.
			// NOTE: This might cause cbPayloadBytesRemaining to go negative by one!  I know that seems weird, but it actually
			// keeps the logic below simpler.
			cbBytesRemainingForSegments -= 1;

			// Update various accounting, depending on reliable or unreliable
			if ( pSendMsg->m_nReliableStreamPos > 0 )
			{
				// Reliable segments advance the current message number.
				// NOTE: If we coalesce adjacent reliable segments, this will probably need to be adjusted
				if ( nLastMsgNum > 0 )
					++nLastMsgNum;

				// Go ahead and add us to the end of the list of unacked messages
				m_senderState.m_unackedReliableMessages.push_back( seg.m_pMsg );
			}
			else
			{
				nLastMsgNum = pSendMsg->m_nMsgNum;

				// Set the "This is the last segment in this message" header bit
				seg.m_hdr[0] |= 0x20;
			}
		}
	}

	// Now we know how much space we need for the segments.  If we asked to reserve
	// space for acks, we should have at least that much.  But we might have more.
	// Serialize acks, as much as will fit.  If we are badly fragmented and we have
	// the space, it's better to keep sending acks over and over to try to clear
	// it out as fast as possible.
	if ( cbReserveForAcks > 0 )
	{

		// If we didn't use all the space for data, that's more we could use for acks
		int cbAvailForAcks = cbReserveForAcks;
		if ( cbBytesRemainingForSegments > 0 )
			cbAvailForAcks += cbBytesRemainingForSegments;
		uint8 *pAckEnd = pPayloadPtr + cbAvailForAcks;
		Assert( pAckEnd <= pPayloadEnd );

		uint8 *pAfterAcks = SNP_SerializeAckBlocks( ackHelper, pPayloadPtr, pAckEnd, usecNow );
		if ( pAfterAcks == nullptr )
			return -1; // bug!  Abort

		int cbAckBytesWritten = pAfterAcks - pPayloadPtr;
		if ( cbAckBytesWritten > cbReserveForAcks )
		{
			// We used more space for acks than was strictly reserved.
			// Update space remaining for data segments.  We should have the room!
			cbBytesRemainingForSegments -= ( cbAckBytesWritten - cbReserveForAcks );
			Assert( cbBytesRemainingForSegments >= -1 ); // remember we might go over by one byte
		}
		else
		{
			Assert( cbAckBytesWritten == cbReserveForAcks ); // The code above reserves space very carefuly.  So if we reserve it, we should fill it!
		}

		pPayloadPtr = pAfterAcks;
	}

	// We are gonna send a packet.  Start filling out an entry so that when it's acked (or nacked)
	// we can know what to do.
	Assert( m_senderState.m_mapInFlightPacketsByPktNum.lower_bound( m_statsEndToEnd.m_nNextSendSequenceNumber ) == m_senderState.m_mapInFlightPacketsByPktNum.end() );
	std::pair<int64,SNPInFlightPacket_t> pairInsert( m_statsEndToEnd.m_nNextSendSequenceNumber, SNPInFlightPacket_t{} );
	SNPInFlightPacket_t &inFlightPkt = pairInsert.second;
	inFlightPkt.m_usecWhenSent = usecNow;
	inFlightPkt.m_bNack = false;

	// We might have gone over exactly one byte, because we counted the size byte of the last
	// segment, which doesn't actually need to be sent
	Assert( cbBytesRemainingForSegments >= 0 || ( cbBytesRemainingForSegments == -1 && vecSegments.size() > 0 ) );

	// OK, now go through and actually serialize the segments
	int nSegments = len( vecSegments );
	for ( int idx = 0 ; idx < nSegments ; ++idx )
	{
		EncodedSegment &seg = vecSegments[ idx ];

		// Check if this message is still sitting in the queue.  (If so, it has to be the first one!)
		bool bStillInQueue = ( seg.m_pMsg == m_senderState.m_messagesQueued.m_pFirst );

		// Finish the segment size byte
		if ( idx < nSegments-1 )
		{
			// Stash upper 3 bits into the header
			int nUpper3Bits = ( seg.m_cbSize>>8 );
			Assert( nUpper3Bits <= 4 ); // The values 5 and 6 are reserved and shouldn't be needed due to the MTU we support
			seg.m_hdr[0] |= nUpper3Bits;

			// And the lower 8 bits follow the other fields
			seg.m_hdr[ seg.m_cbHdr++ ] = uint8( seg.m_cbSize );
		}
		else
		{
			// Set "no explicit size field included, segment extends to end of packet"
			seg.m_hdr[0] |= 7;
		}

		// Double-check that we didn't overflow
		Assert( seg.m_cbHdr <= seg.k_cbMaxHdr );

		// Copy the header
		memcpy( pPayloadPtr, seg.m_hdr, seg.m_cbHdr ); pPayloadPtr += seg.m_cbHdr;

		// Copy the data
		Assert( pPayloadPtr+seg.m_cbSize <= pPayloadEnd );
		memcpy( pPayloadPtr, seg.m_pMsg->m_pData + seg.m_nOffset, seg.m_cbSize ); pPayloadPtr += seg.m_cbSize;

		// Reliable?
		if ( seg.m_pMsg->m_nReliableStreamPos > 0 )
		{
			// We should never encode an empty range of the stream, that is worthless.
			// (Even an empty reliable message requires some framing in the stream.)
			Assert( seg.m_cbSize > 0 );

			// Remember that this range is in-flight
			SNPRange_t range;
			range.m_nBegin = seg.m_pMsg->m_nReliableStreamPos + seg.m_nOffset;
			range.m_nEnd = range.m_nBegin + seg.m_cbSize;

			// Ranges of the reliable stream that have not been acked should either be
			// in flight, or queued for retry.  Make sure this range is not already in
			// either state.
			Assert( !HasOverlappingRange( range, m_senderState.m_listInFlightReliableRange ) );
			Assert( !HasOverlappingRange( range, m_senderState.m_listReadyRetryReliableRange ) );

			// Spew
			SpewType( steamdatagram_snp_log_packet+1, "  %s encode pkt %lld reliable msg %lld offset %d+%d=%d range [%lld,%lld)\n",
				m_sName.c_str(), (long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)seg.m_pMsg->m_nMsgNum,
				seg.m_nOffset, seg.m_cbSize, seg.m_nOffset+seg.m_cbSize,
				(long long)range.m_nBegin, (long long)range.m_nEnd );

			// Add to table of in-flight reliable ranges
			m_senderState.m_listInFlightReliableRange[ range ] = seg.m_pMsg;

			// Remember that this packet contained that range
			inFlightPkt.m_vecReliableSegments.push_back( range );

			// Less reliable data pending
			m_senderState.m_cbPendingReliable -= seg.m_cbSize;
			Assert( m_senderState.m_cbPendingReliable >= 0 );
		}
		else
		{
			// We should only encode an empty segment if the message itself is empty
			Assert( seg.m_cbSize > 0 || ( seg.m_cbSize == 0 && seg.m_pMsg->m_cbSize == 0 ) );

			// Check some stuff
			Assert( bStillInQueue == ( seg.m_nOffset + seg.m_cbSize < seg.m_pMsg->m_cbSize ) ); // If we ended the message, we should have removed it from the queue
			Assert( bStillInQueue == ( ( seg.m_hdr[0] & 0x20 ) == 0 ) );
			Assert( bStillInQueue || seg.m_pMsg->m_pNext == nullptr ); // If not in the queue, we should be detached
			Assert( seg.m_pMsg->m_pPrev == nullptr ); // We should either be at the head of the queue, or detached

			// Spew
			SpewType( steamdatagram_snp_log_packet+1, "  %s encode pkt %lld unreliable msg %lld offset %d+%d=%d\n",
				m_sName.c_str(), (long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)seg.m_pMsg->m_nMsgNum,
				seg.m_nOffset, seg.m_cbSize, seg.m_nOffset+seg.m_cbSize );

			// Less unreliable data pending
			m_senderState.m_cbPendingUnreliable -= seg.m_cbSize;
			Assert( m_senderState.m_cbPendingUnreliable >= 0 );

			// Done with this message?  Clean up
			if ( !bStillInQueue )
				delete seg.m_pMsg;
		}
	}

	// One last check for overflow
	Assert( pPayloadPtr <= pPayloadEnd );
	int cbPlainText = pPayloadPtr - payload;
	if ( cbPlainText > cbMaxPlaintextPayload )
	{
		AssertMsg1( false, "Payload exceeded max size of %d\n", cbMaxPlaintextPayload );
		return 0;
	}

	//
	// Encrypt it
	//

	Assert( m_bCryptKeysValid );

	// Make sure sizes are reasonable.
	COMPILE_TIME_ASSERT( k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend % k_cbSteamNetworkingSocketsEncryptionBlockSize == 0 );
	COMPILE_TIME_ASSERT( k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend >= k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend - 4 );

	// Encrypt the chunk
	uint8 arEncryptedChunk[ k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend + 64 ]; // Should not need pad
	*(uint64 *)&m_cryptIVSend.m_buf = LittleQWord( m_statsEndToEnd.m_nNextSendSequenceNumber );
	uint32 cbEncrypted = sizeof(arEncryptedChunk);
	DbgVerify( CCrypto::SymmetricEncryptWithIV(
		(const uint8 *)payload, cbPlainText, // plaintext
		m_cryptIVSend.m_buf, m_cryptIVSend.k_nSize, // IV
		arEncryptedChunk, &cbEncrypted, // output
		m_cryptKeySend.m_buf, m_cryptKeySend.k_nSize // Key
	) );
	Assert( (int)cbEncrypted >= cbPlainText );
	Assert( (int)cbEncrypted <= k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend ); // confirm that pad above was not necessary and we never exceed k_nMaxSteamDatagramTransportPayload, even after encrypting

	//SpewMsg( "Send encrypt IV %llu + %02x%02x%02x%02x, key %02x%02x%02x%02x\n", *(uint64 *)&m_cryptIVSend.m_buf, m_cryptIVSend.m_buf[8], m_cryptIVSend.m_buf[9], m_cryptIVSend.m_buf[10], m_cryptIVSend.m_buf[11], m_cryptKeySend.m_buf[0], m_cryptKeySend.m_buf[1], m_cryptKeySend.m_buf[2], m_cryptKeySend.m_buf[3] );

	// Connection-specific method to send it
	int nBytesSent = SendEncryptedDataChunk( arEncryptedChunk, cbEncrypted, usecNow, pConnectionData );
	if ( nBytesSent <= 0 )
		return -1;

	// We sent a packet.  Track it
	auto pairInsertResult = m_senderState.m_mapInFlightPacketsByPktNum.insert( pairInsert );
	Assert( pairInsertResult.second ); // We should have inserted a new element, not updated an existing element

	// If we aren't already tracking anything to timeout, then this is the next one.
	if ( m_senderState.m_itNextInFlightPacketToTimeout == m_senderState.m_mapInFlightPacketsByPktNum.end() )
		m_senderState.m_itNextInFlightPacketToTimeout = pairInsertResult.first;

	// If we needed to send acks, presumably we sent them, so clear timeout
	m_receiverState.SentAcks();

	// We spent some tokens
	m_senderState.m_flTokenBucket -= (float)nBytesSent;
	return nBytesSent;
}

void CSteamNetworkConnectionBase::SNP_GatherAckBlocks( SNPAckSerializerHelper &helper, SteamNetworkingMicroseconds usecNow )
{
	helper.m_nBlocks = 0;

	// Fast case for no packet loss we need to ack, which will (hopefully!) be a common case
	if ( m_receiverState.m_mapPacketGaps.empty() )
		return;

	int n = std::min( (int)helper.k_nMaxBlocks, len( m_receiverState.m_mapPacketGaps ) );
	auto itNext = m_receiverState.m_mapPacketGaps.begin();

	int cbEncodedSize = 0;
	while ( n > 0 )
	{
		--n;
		auto itCur = itNext;
		++itNext;

		Assert( itCur->first < itCur->second.m_nEnd );

		SNPAckSerializerHelper::Block &block = helper.m_arBlocks[ helper.m_nBlocks ];
		block.m_nNack = uint32( itCur->second.m_nEnd - itCur->first );

		int64 nAckEnd;
		SteamNetworkingMicroseconds usecWhenSentLast;
		if ( n == 0 )
		{
			Assert( itNext == m_receiverState.m_mapPacketGaps.end() );
			nAckEnd = m_statsEndToEnd.m_nLastRecvSequenceNumber+1;
			usecWhenSentLast = m_statsEndToEnd.m_usecTimeLastRecvSeq;
		}
		else
		{
			nAckEnd = itNext->first;
			usecWhenSentLast = itNext->second.m_usecWhenReceivedPktBefore;
		}
		Assert( itCur->second.m_nEnd < nAckEnd );
		block.m_nAck = uint32( nAckEnd - itCur->second.m_nEnd );

		block.m_nLatestPktNum = uint32( nAckEnd-1 );
		block.m_nEncodedTimeSinceLatestPktNum = SNPAckSerializerHelper::EncodeTimeSince( usecNow, usecWhenSentLast );

		++cbEncodedSize;
		if ( block.m_nAck > 7 )
			cbEncodedSize += VarIntSerializedSize( block.m_nAck>>3 );
		if ( block.m_nNack > 7 )
			cbEncodedSize += VarIntSerializedSize( block.m_nNack>>3 );
		block.m_cbTotalEncodedSize = cbEncodedSize;

		// FIXME Here if the caller knows they are working with limited space,
		// they could tell us how much space they have and we could bail
		// if we already know we're over

		++helper.m_nBlocks;
	}
}

uint8 *CSteamNetworkConnectionBase::SNP_SerializeAckBlocks( const SNPAckSerializerHelper &helper, uint8 *pOut, const uint8 *pOutEnd, SteamNetworkingMicroseconds usecNow )
{

	// Never received anything?
	if ( m_statsEndToEnd.m_nLastRecvSequenceNumber == 0 )
	{
		m_receiverState.m_usecWhenFlushAck = INT64_MAX;
		return pOut;
	}

	// No room even for the header?
	if ( pOut + 5 > pOutEnd )
		return pOut;

	// !KLUDGE! For now limit number of blocks, and always use 16-bit ID.
	//          Later we might want to make this code smarter.
	COMPILE_TIME_ASSERT( SNPAckSerializerHelper::k_cbHeaderSize == 5 );
	uint8 *pAckHeaderByte = pOut;
	++pOut;
	uint16 *pLatestPktNum = (uint16 *)pOut;
	pOut += 2;
	uint16 *pTimeSinceLatestPktNum = (uint16 *)pOut;
	pOut += 2;

	// 10011000 - ack frame designator, with 16-bit last-received sequence number, and no ack blocks
	*pAckHeaderByte = 0x98;

	// Fast case for no packet loss we need to ack, which will (hopefully!) be a common case
	if ( m_receiverState.m_mapPacketGaps.empty() )
	{
		int64 nLastRecvPktNum = m_statsEndToEnd.m_nLastRecvSequenceNumber;
		*pLatestPktNum = uint16( nLastRecvPktNum );
		*pTimeSinceLatestPktNum = SNPAckSerializerHelper::EncodeTimeSince( usecNow, m_statsEndToEnd.m_usecTimeLastRecvSeq );

		SpewType( steamdatagram_snp_log_packet+1, "  %s encode pkt %lld last recv %lld (no loss)\n",
			m_sName.c_str(),
			(long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)nLastRecvPktNum
		);
		m_receiverState.m_usecWhenFlushAck = INT64_MAX; // Clear timer, we wrote everything we needed to
		return pOut;
	}

	// Fit as many blocks as possible.
	// (Unless we are badly fragmented and are trying to squeeze in what
	// we can at the end of a packet, this won't ever iterate
	int nBlocks = helper.m_nBlocks;
	uint8 *pExpectedOutEnd;
	for (;;)
	{

		// Can't fit any blocks at all?  Just fill in the header
		// with the oldest thing we can ack and call it a day
		if ( nBlocks == 0 )
		{
			auto itOldestGap = m_receiverState.m_mapPacketGaps.begin();
			int64 nLastRecvPktNum = itOldestGap->first-1;
			*pLatestPktNum = uint16( nLastRecvPktNum );
			*pTimeSinceLatestPktNum = SNPAckSerializerHelper::EncodeTimeSince( usecNow, itOldestGap->second.m_usecWhenReceivedPktBefore );

			SpewType( steamdatagram_snp_log_packet+1, "  %s encode pkt %lld last recv %lld (no blocks, actual last recv=%lld)\n",
				m_sName.c_str(),
				(long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)nLastRecvPktNum, (long long)m_statsEndToEnd.m_nLastRecvSequenceNumber
			);
			return pOut;
		}

		int cbTotalEncoded = helper.m_arBlocks[nBlocks-1].m_cbTotalEncodedSize;
		if ( nBlocks > 6 )
			++cbTotalEncoded;
		pExpectedOutEnd = pOut + cbTotalEncoded; // Save for debugging below
		if ( pExpectedOutEnd <= pOutEnd )
			break;

		// Won't fit, peel off the newest one, see if the earlier ones will fit
		--nBlocks;
	}

	// OK, we know how many blocks we are going to write.  Finish the header byte
	Assert( nBlocks == uint8(nBlocks) );
	if ( nBlocks > 6 )
	{
		*pAckHeaderByte |= 7;
		*(pOut++) = uint8( nBlocks );
	}
	else
	{
		*pAckHeaderByte |= uint8( nBlocks );
	}

	// Locate the first one we will serialize.
	// (It's the newest one, which is the last one in the list).
	const SNPAckSerializerHelper::Block *pBlock = &helper.m_arBlocks[nBlocks-1];

	// Latest packet number and time
	*pLatestPktNum = LittleWord( uint16( pBlock->m_nLatestPktNum ) );
	*pTimeSinceLatestPktNum = LittleWord( pBlock->m_nEncodedTimeSinceLatestPktNum );

	// Full packet number, for spew
	int64 nAckEnd = ( m_statsEndToEnd.m_nLastRecvSequenceNumber & ~(int64)(~(uint32)0) ) | pBlock->m_nLatestPktNum;
	++nAckEnd;

	SpewType( steamdatagram_snp_log_packet+1, "  %s encode pkt %lld last recv %lld (%d blocks, actual last recv=%lld)\n",
		m_sName.c_str(),
		(long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)(nAckEnd-1), nBlocks, (long long)m_statsEndToEnd.m_nLastRecvSequenceNumber
	);

	// Serialize the blocks into the packet, from newest to oldest
	while ( pBlock >= helper.m_arBlocks )
	{
		uint8 *pAckBlockHeaderByte = pOut;
		++pOut;

		// Encode ACK (number of packets successfully received)
		{
			if ( pBlock->m_nAck < 8 )
			{
				// Small block of packets.  Encode directly in the header.
				*pAckBlockHeaderByte = uint8(pBlock->m_nAck << 4);
			}
			else
			{
				// Larger block of received packets.  Put lowest bits in the header,
				// and overflow using varint.  This is probably going to be pretty
				// common.
				*pAckBlockHeaderByte = 0x80 | ( uint8(pBlock->m_nAck & 7) << 4 );
				pOut = SerializeVarInt( pOut, pBlock->m_nAck>>3, pOutEnd );
				if ( pOut == nullptr )
				{
					AssertMsg( false, "Overflow serializing packet ack varint count" );
					return nullptr;
				}
			}
		}

		// Encode NACK (number of packets dropped)
		{
			if ( pBlock->m_nNack < 8 )
			{
				// Small block of packets.  Encode directly in the header.
				*pAckBlockHeaderByte |= uint8(pBlock->m_nNack);
			}
			else
			{
				// Larger block of dropped packets.  Put lowest bits in the header,
				// and overflow using varint.  This is probably going to be less common than
				// large ACK runs, but not totally uncommon.  Losing one or two packets is
				// really common, but loss events often involve a lost of many packets in a run.
				*pAckBlockHeaderByte |= 0x08 | uint8(pBlock->m_nNack & 7);
				pOut = SerializeVarInt( pOut, pBlock->m_nNack >> 3, pOutEnd );
				if ( pOut == nullptr )
				{
					AssertMsg( false, "Overflow serializing packet nack varint count" );
					return nullptr;
				}
			}
		}

		// Debug
		int64 nAckBegin = nAckEnd - pBlock->m_nAck;
		int64 nNackBegin = nAckBegin - pBlock->m_nNack;
		SpewType( steamdatagram_snp_log_packet+1, "  %s encode pkt %lld nack [%lld,%lld) ack [%lld,%lld) \n",
			m_sName.c_str(),
			(long long)m_statsEndToEnd.m_nNextSendSequenceNumber,
			(long long)nNackBegin, (long long)nAckBegin,
			(long long)nAckBegin, (long long)nAckEnd
		);
		nAckEnd = nNackBegin;
		Assert( nAckEnd > 0 ); // Make sure we don't try to ack packet 0 or below

		// Move backwards in time
		--pBlock;
	}

	// Make sure when we were checking what would fit, we correctly calculated serialized size
	Assert( pOut == pExpectedOutEnd );

	// If we were able to fit all blocks, then clear the ack timeout,
	// since we wrote everything we wanted to
	// NOTE: This assumes that helper.m_nBlocks wasn't artificially limited
	// due to trying to fit in a special space-limited packet
	if ( nBlocks == helper.m_nBlocks )
		m_receiverState.m_usecWhenFlushAck = INT64_MAX;

	return pOut;
}

uint8 *CSteamNetworkConnectionBase::SNP_SerializeStopWaitingFrame( uint8 *pOut, const uint8 *pOutEnd, SteamNetworkingMicroseconds usecNow )
{
	// For now, we will always write this.  We should optimize this and try to be
	// smart about when to send it (probably maybe once per RTT, or when N packets
	// have been received or N blocks accumulate?)

	// Calculate offset from the current sequence number
	int64 nOffset = m_statsEndToEnd.m_nNextSendSequenceNumber - m_senderState.m_nMinPktWaitingOnAck;
	AssertMsg2( nOffset > 0, "Told peer to stop acking up to %lld, but latest packet we have sent is %lld", (long long)m_senderState.m_nMinPktWaitingOnAck, (long long)m_statsEndToEnd.m_nNextSendSequenceNumber );
	SpewType( steamdatagram_snp_log_packet, "  %s encode pkt %lld stop_waiting offset %lld = %lld",
		m_sName.c_str(),
		(long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)nOffset, (long long)m_senderState.m_nMinPktWaitingOnAck );

	// Subtract one, as a *tiny* optimization, since they cannot possible have
	// acknowledged this packet we are serializing already
	--nOffset;

	// Now encode based on number of bits needed
	if ( nOffset < 0x100 )
	{
		if ( pOut + 2 > pOutEnd )
			return pOut;
		*pOut = 0x80;
		++pOut;
		*pOut = uint8( nOffset );
		++pOut;
	}
	else if ( nOffset < 0x10000 )
	{
		if ( pOut + 3 > pOutEnd )
			return pOut;
		*pOut = 0x81;
		++pOut;
		*(uint16*)pOut = LittleWord( uint16( nOffset ) );
		pOut += 2;
	}
	else if ( nOffset < 0x1000000 )
	{
		if ( pOut + 4 > pOutEnd )
			return pOut;
		*pOut = 0x82;
		++pOut;
		*pOut = uint8( nOffset ); // Wire format is little endian, so lowest 8 bits first
		++pOut;
		*(uint16*)pOut = LittleWord( uint16( nOffset>>8 ) );
		pOut += 2;
	}
	else
	{
		if ( pOut + 9 > pOutEnd )
			return pOut;
		*pOut = 0x83;
		++pOut;
		*(uint64*)pOut = LittleQWord( nOffset );
		pOut += 8;
	}

	Assert( pOut <= pOutEnd );
	return pOut;
}

void CSteamNetworkConnectionBase::SNP_ReceiveUnreliableSegment( int64 nMsgNum, int nOffset, const void *pSegmentData, int cbSegmentSize, bool bLastSegmentInMessage, SteamNetworkingMicroseconds usecNow )
{
	SpewType( steamdatagram_snp_log_packet+1, "%s RX msg %lld offset %d+%d=%d %02x ... %02x\n", m_sName.c_str(), nMsgNum, nOffset, cbSegmentSize, nOffset+cbSegmentSize, ((byte*)pSegmentData)[0], ((byte*)pSegmentData)[cbSegmentSize-1] );

	// Check for a common special case: non-fragmented message.
	if ( nOffset == 0 && bLastSegmentInMessage )
	{

		// Deliver it immediately, don't go through the fragmentation assembly process below.
		// (Although that would work.)
		ReceivedMessage( pSegmentData, cbSegmentSize, nMsgNum, usecNow );
		return;
	}

	// Limit number of unreliable segments
	// We store.  We just use a fixed limit, rather than
	// trying to be smart by expiring based on time or whatever.
	if ( len( m_receiverState.m_mapUnreliableSegments ) > k_nMaxBufferedUnreliableSegments )
	{
		auto itDelete = m_receiverState.m_mapUnreliableSegments.begin();

		// If we're going to delete some, go ahead and delete all of them for this
		// message.
		int64 nDeleteMsgNum = itDelete->first.m_nMsgNum;
		do {
			itDelete = m_receiverState.m_mapUnreliableSegments.erase( itDelete );
		} while ( itDelete != m_receiverState.m_mapUnreliableSegments.end() && itDelete->first.m_nMsgNum == nDeleteMsgNum );

		// Warn if the message we are receiving is older (or the same) than the one
		// we are deleting.  If sender is legit, then it probably means that we have
		// something tuned badly.
		if ( nDeleteMsgNum >= nMsgNum )
		{
			// Spew, but rate limit in case of malicious sender
			SpewWarningRateLimited( usecNow, "SNP expiring unreliable segments for msg %lld, while receiving unreliable segments for msg %lld\n",
				(long long)nDeleteMsgNum, (long long)nMsgNum );
		}
	}

	// Message fragment.  Find/insert the entry in our reassembly queue
	// I really hate this syntax and interface.
	SSNPRecvUnreliableSegmentKey key;
	key.m_nMsgNum = nMsgNum;
	key.m_nOffset = nOffset;
	SSNPRecvUnreliableSegmentData &data = m_receiverState.m_mapUnreliableSegments[ key ];
	if ( data.m_cbSize >= 0 )
	{

		// We got the same segment twice (weird, since they shouldn't be doing
		// retry -- but remember that we're working on top of UDP, which could deliver packets
		// multiple times).  Duplicate packet delivery is actually really rare, let's spew about it.
		SpewMsg( "Received unreliable msg %lld offset %d twice.  Sizes %d,%d\n", nMsgNum, nOffset, data.m_cbSize, cbSegmentSize );
	}

	if ( data.m_cbSize >= cbSegmentSize )
	{

		// The data we already have is either a duplicate, or a superset.
		// So ignore this incoming segment.
		return;
	}

	// Segment in the map either just got inserted, or is a subset of the segment
	// we just received.  Replace it.
	data.m_cbSize = cbSegmentSize;
	Assert( !data.m_bLast ); // sender is doing weird stuff or we have a bug
	data.m_bLast = bLastSegmentInMessage;
	memcpy( data.m_buf, pSegmentData, cbSegmentSize );

	// Now check if that completed the message
	key.m_nOffset = 0;
	auto itMsgStart = m_receiverState.m_mapUnreliableSegments.lower_bound( key );
	auto end = m_receiverState.m_mapUnreliableSegments.end();
	Assert( itMsgStart != end );
	auto itMsgLast = itMsgStart;
	int cbMessageSize = 0;
	for (;;)
	{
		// Is this the thing we expected?
		if ( itMsgLast->first.m_nMsgNum != nMsgNum || itMsgLast->first.m_nOffset > cbMessageSize )
			return; // We've got a gap.

		// Update.  This code looks more complicated than strictly necessary, but it works
		// if we have overlapping segments.
		cbMessageSize = Max( cbMessageSize, itMsgLast->first.m_nOffset + itMsgLast->second.m_cbSize );

		// Is that the end?
		if ( itMsgLast->second.m_bLast )
			break;

		// Still looking for the end
		++itMsgLast;
		if ( itMsgLast == end )
			return;
	}

	// OK, we have the complete message!  Gather the
	// segments into a contiguous buffer
	uint8 *pMessage = new uint8[ cbMessageSize ];
	for (;;)
	{
		Assert( itMsgStart->first.m_nMsgNum == nMsgNum );
		memcpy( pMessage + itMsgStart->first.m_nOffset, itMsgStart->second.m_buf, itMsgStart->second.m_cbSize );

		// Done?
		if ( itMsgStart->second.m_bLast )
			break;

		// Remove entry from list, and move onto the next entry
		itMsgStart = m_receiverState.m_mapUnreliableSegments.erase( itMsgStart );
	}

	// Erase the last segment, and anything else we might have hanging around
	// for this message (???)
	do {
		itMsgStart = m_receiverState.m_mapUnreliableSegments.erase( itMsgStart );
	} while ( itMsgStart != end && itMsgStart->first.m_nMsgNum == nMsgNum );

	// Deliver the message.
	ReceivedMessage( pMessage, cbMessageSize, nMsgNum, usecNow );

	// Clean up
	// !SPEED! It would be best if we could hand ownership of this buffer to
	// ReceivedMessage, instead of having that function copy it.
	delete[] pMessage;
}

bool CSteamNetworkConnectionBase::SNP_ReceiveReliableSegment( int64 nPktNum, int64 nSegBegin, const uint8 *pSegmentData, int cbSegmentSize, SteamNetworkingMicroseconds usecNow )
{
	// Calculate segment end stream position
	int64 nSegEnd = nSegBegin + cbSegmentSize;

	// Spew
	SpewType( steamdatagram_snp_log_packet, "  %s decode pkt %lld reliable range [%lld,%lld)\n",
		m_sName.c_str(),
		(long long)nPktNum,
		(long long)nSegBegin, (long long)nSegEnd );

	// If they ever send us a reliable segment, then we should make sure we
	// send them an ack of what we have.
	m_receiverState.MarkNeedToSendAck( usecNow );

	// No segment data?  Seems fishy, but if it happens, just skip it.
	Assert( cbSegmentSize >= 0 );
	if ( cbSegmentSize <= 0 )
	{
		// Spew but rate limit in case of malicious sender
		SpewWarningRateLimited( usecNow, "%s decode pkt %lld empty reliable segment?\n",
			m_sName.c_str(),
			(long long)nPktNum );
		return true;
	}

	// Check if the entire thing is stuff we have already received, then
	// we can discard it
	if ( nSegEnd <= m_receiverState.m_nReliableStreamPos )
		return true;

	// !SPEED! Should we have a fast path here for small messages
	// where we have nothing buffered, and avoid all the copying into the 
	// stream buffer and decode directly.

	// What do we expect to receive next?
	const int64 nExpectNextStreamPos = m_receiverState.m_nReliableStreamPos + len( m_receiverState.m_bufReliableStream );

	// Check if we need to grow the reliable buffer to hold the data
	if ( nSegEnd > nExpectNextStreamPos )
	{
		int64 cbNewSize = nSegEnd - m_receiverState.m_nReliableStreamPos;
		Assert( cbNewSize > len( m_receiverState.m_bufReliableStream ) );

		// Check if we have too much data buffered, just stop processing
		// this packet, and forget we ever received it.  We need to protect
		// against a malicious sender trying to create big gaps.  If they
		// are legit, they will notice that we go back and fill in the gaps
		// and we will get caught up.
		if ( cbNewSize > k_cbMaxBufferedReceiveReliableData )
		{
			// Stop processing the packet, and don't ack it.
			// This indicates the connection is in pretty bad shape,
			// so spew about it.  But rate limit in case of malicious sender
			SpewWarningRateLimited( usecNow, "%s decode pkt %lld abort.  %lld bytes reliable data buffered [%lld-%lld), new size would be %lld to %lld\n",
				m_sName.c_str(),
				(long long)nPktNum,
				(long long)m_receiverState.m_bufReliableStream.size(),
				(long long)m_receiverState.m_nReliableStreamPos,
				(long long)( m_receiverState.m_nReliableStreamPos + m_receiverState.m_bufReliableStream.size() ),
				(long long)cbNewSize, (long long)nSegEnd
			);
			return false; 
		}

		// Check if this is going to make a new gap
		if ( nSegBegin > nExpectNextStreamPos )
		{
			if ( !m_receiverState.m_mapReliableStreamGaps.empty() )
			{

				// We should never have a gap at the very end of the buffer.
				// (Why would we extend the buffer, unless we needed to to
				// store some data?)
				Assert( m_receiverState.m_mapReliableStreamGaps.rbegin()->second < nExpectNextStreamPos );

				// We need to add a new gap.  See if we're already too fragmented.
				if ( len( m_receiverState.m_mapReliableStreamGaps ) >= k_nMaxReliableStreamGaps_Extend )
				{
					// Stop processing the packet, and don't ack it
					// This indicates the connection is in pretty bad shape,
					// so spew about it.  But rate limit in case of malicious sender
					SpewWarningRateLimited( usecNow, "%s decode pkt %lld abort.  Reliable stream already has %d fragments, first is [%lld,%lld), last is [%lld,%lld), new segment is [%lld,%lld)\n",
						m_sName.c_str(),
						(long long)nPktNum,
						len( m_receiverState.m_mapReliableStreamGaps ),
						(long long)m_receiverState.m_mapReliableStreamGaps.begin()->first, (long long)m_receiverState.m_mapReliableStreamGaps.begin()->second,
						(long long)m_receiverState.m_mapReliableStreamGaps.rbegin()->first, (long long)m_receiverState.m_mapReliableStreamGaps.rbegin()->second,
						(long long)nSegBegin, (long long)nSegEnd
					);
					return false; 
				}
			}

			// Add a gap
			m_receiverState.m_mapReliableStreamGaps[ nExpectNextStreamPos ] = nSegBegin;
		}
		m_receiverState.m_bufReliableStream.resize( size_t( cbNewSize ) );
	}

	// If segment overlapped the existing buffer, we might need to discard the front
	// bit or discard a gap that was filled
	if ( nSegBegin < nExpectNextStreamPos )
	{

		// Check if the front bit has already been processed, then skip it
		if ( nSegBegin < m_receiverState.m_nReliableStreamPos )
		{
			int nSkip = m_receiverState.m_nReliableStreamPos - nSegBegin;
			cbSegmentSize -= nSkip;
			pSegmentData += nSkip;
			nSegBegin += nSkip;
		}
		Assert( nSegBegin < nSegEnd );

		// Check if this filled in one or more gaps (or made a hole in the middle!)
		if ( !m_receiverState.m_mapReliableStreamGaps.empty() )
		{
			auto gapFilled = m_receiverState.m_mapReliableStreamGaps.upper_bound( nSegBegin );
			if ( gapFilled != m_receiverState.m_mapReliableStreamGaps.begin() )
			{
				--gapFilled;
				Assert( gapFilled->first < gapFilled->second ); // Make sure we don't have degenerate/invalid gaps in our table
				Assert( gapFilled->first <= nSegBegin ); // Make sure we located the gap we think we located
				if ( gapFilled->second > nSegBegin ) // gap is not entirely before this segment
				{
					do {

						// Common case where we fill exactly at the start
						if ( nSegBegin == gapFilled->first )
						{
							if ( nSegEnd < gapFilled->second )
							{
								// We filled the first bit of the gap.  Chop off the front bit that we filled.
								// We cast away const here because we know that we aren't violating the ordering constraints
								const_cast<int64&>( gapFilled->first ) = nSegEnd;
								break;
							}

							// Filled the whole gap.
							// Erase, and move forward in case this also fills more gaps
							// !SPEED! Since exactly filing the gap should be common, we might
							// check specifically for that case and early out here.
							gapFilled = m_receiverState.m_mapReliableStreamGaps.erase( gapFilled );
						}
						else if ( nSegEnd >= gapFilled->second )
						{
							// Chop off the end of the gap
							Assert( nSegBegin < gapFilled->second );
							gapFilled->second = nSegBegin;

							// And maybe subsequent gaps!
							++gapFilled;
						}
						else
						{
							// We are fragmenting.
							Assert( nSegBegin > gapFilled->first );
							Assert( nSegEnd < gapFilled->second );

							// Protect against malicious sender.  A good sender will
							// fill the gaps in stream position order and not fragment
							// like this
							if ( len( m_receiverState.m_mapReliableStreamGaps ) >= k_nMaxReliableStreamGaps_Fragment )
							{
								// Stop processing the packet, and don't ack it
								SpewWarningRateLimited( usecNow, "%s decode pkt %lld abort.  Reliable stream already has %d fragments, first is [%lld,%lld), last is [%lld,%lld).  We don't want to fragment [%lld,%lld) with new segment [%lld,%lld)\n",
									m_sName.c_str(),
									(long long)nPktNum,
									len( m_receiverState.m_mapReliableStreamGaps ),
									(long long)m_receiverState.m_mapReliableStreamGaps.begin()->first, (long long)m_receiverState.m_mapReliableStreamGaps.begin()->second,
									(long long)m_receiverState.m_mapReliableStreamGaps.rbegin()->first, (long long)m_receiverState.m_mapReliableStreamGaps.rbegin()->second,
									(long long)gapFilled->first, (long long)gapFilled->second,
									(long long)nSegBegin, (long long)nSegEnd
								);
								return false; 
							}

							// Save bounds of the right side
							int64 nRightHandBegin = nSegEnd;
							int64 nRightHandEnd = gapFilled->second;

							// Truncate the left side
							gapFilled->second = nSegBegin;

							// Add the right hand gap
							m_receiverState.m_mapReliableStreamGaps[ nRightHandBegin ] = nRightHandEnd;

							// And we know that we cannot possible have covered any more gaps
							break;
						}

						// In some rare cases we might fill more than one gap with a single segment.
						// So keep searching forward.
					} while ( gapFilled != m_receiverState.m_mapReliableStreamGaps.end() && gapFilled->first < nSegEnd );
				}
			}
		}
	}

	// Copy the data into the buffer.
	// It might be redundant, but if so, we aren't going to take the
	// time to figure that out.
	int nBufOffset = nSegBegin - m_receiverState.m_nReliableStreamPos;
	Assert( nBufOffset >= 0 );
	Assert( nBufOffset+cbSegmentSize <= len( m_receiverState.m_bufReliableStream ) );
	memcpy( &m_receiverState.m_bufReliableStream[nBufOffset], pSegmentData, cbSegmentSize );

	// Figure out how many valid bytes are at the head of the buffer
	int nNumReliableBytes;
	if ( m_receiverState.m_mapReliableStreamGaps.empty() )
	{
		nNumReliableBytes = len( m_receiverState.m_bufReliableStream );
	}
	else
	{
		auto firstGap = m_receiverState.m_mapReliableStreamGaps.begin();
		Assert( firstGap->first >= m_receiverState.m_nReliableStreamPos );
		if ( firstGap->first < nSegBegin )
		{
			// There's gap in front of us, and therefore if we didn't have
			// a complete reliable message before, we don't have one now.
			Assert( firstGap->second <= nSegBegin );
			return true;
		}

		// We do have a gap, but it's somewhere after this segment.
		Assert( firstGap->first >= nSegEnd );
		nNumReliableBytes = firstGap->first - m_receiverState.m_nReliableStreamPos;
		Assert( nNumReliableBytes > 0 );
		Assert( nNumReliableBytes < len( m_receiverState.m_bufReliableStream ) ); // The last byte in the buffer should always be valid!
	}
	Assert( nNumReliableBytes > 0 );

	// OK, now dispatch as many reliable messages as are now available
	do
	{

		// OK, if we get here, we have some data.  Attempt to decode a reliable message.
		// NOTE: If the message is really big, we will end up doing this parsing work
		// each time we get a new packet.  We could cache off the result if we find out
		// that it's worth while.  It should be pretty fast, though, so let's keep the
		// code simple until we know that it's worthwhile.
		uint8 *pReliableStart = &m_receiverState.m_bufReliableStream[0];
		uint8 *pReliableDecode = pReliableStart;
		uint8 *pReliableEnd = pReliableDecode + nNumReliableBytes;

		// Spew
		SpewType( steamdatagram_snp_log_packet+1, "  %s decode pkt %lld valid reliable bytes = %d [%lld,%lld)\n",
			m_sName.c_str(),
			(long long)nPktNum, nNumReliableBytes,
			(long long)m_receiverState.m_nReliableStreamPos,
			(long long)( m_receiverState.m_nReliableStreamPos + nNumReliableBytes ) );

		// Sanity check that we have a valid header byte.
		uint8 nHeaderByte = *(pReliableDecode++);
		if ( nHeaderByte & 0x80 )
		{
			ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Invalid reliable message header byte 0x%02x", nHeaderByte );
			return false;
		}

		// Parse the message number
		int64 nMsgNum = m_receiverState.m_nLastRecvReliableMsgNum;
		if ( nHeaderByte & 0x40 )
		{
			uint64 nOffset;
			pReliableDecode = DeserializeVarInt( pReliableDecode, pReliableEnd, nOffset );
			if ( pReliableDecode == nullptr )
				return true; // We haven't received all of the message

			nMsgNum += nOffset;

			// Sanity check against a HUGE jump in the message number.
			// This is almost certainly bogus.  (OKOK, yes it is theoretically
			// possible.  But for now while this thing is still under development,
			// most likely it's a bug.  Eventually we can lessen these to handle
			// the case where the app decides to send literally a million unreliable
			// messages in between reliable messages.  The second condition is probably
			// legit, though.)
			if ( nOffset > 1000000 || nMsgNum > m_receiverState.m_nHighestSeenMsgNum+10000 )
			{
				ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError,
					"Reliable message number lurch.  Last reliable %lld, offset %llu, highest seen %lld",
					(long long)m_receiverState.m_nLastRecvReliableMsgNum, (unsigned long long)nOffset,
					(long long)m_receiverState.m_nHighestSeenMsgNum );
				return false;
			}
		}
		else
		{
			++nMsgNum;
		}

		// Check for updating highest message number seen, so we know how to interpret
		// message numbers from the sender with only the lowest N bits present.
		// And yes, we want to do this even if we end up not processing the entire message
		if ( nMsgNum > m_receiverState.m_nHighestSeenMsgNum )
			m_receiverState.m_nHighestSeenMsgNum = nMsgNum;

		// Parse message size.
		int cbMsgSize = nHeaderByte&0x1f;
		if ( nHeaderByte & 0x20 )
		{
			uint64 nMsgSizeUpperBits;
			pReliableDecode = DeserializeVarInt( pReliableDecode, pReliableEnd, nMsgSizeUpperBits );
			if ( pReliableDecode == nullptr )
				return true; // We haven't received all of the message

			// Sanity check size.  Note that we do this check before we shift,
			// to protect against overflow.
			// (Although DeserializeVarInt doesn't detect overflow...)
			if ( nMsgSizeUpperBits > (uint64)k_cbMaxMessageSizeRecv<<5 )
			{
				ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError,
					"Reliable message size too large.  (%llu<<5 + %d)",
					(unsigned long long)nMsgSizeUpperBits, cbMsgSize );
				return false;
			}

			// Compute total size, and check it again
			cbMsgSize += int( nMsgSizeUpperBits<<5 );
			if ( cbMsgSize > k_cbMaxMessageSizeRecv )
			{
				ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError,
					"Reliable message size %d too large.", cbMsgSize );
				return false;
			}
		}

		// Do we have the full thing?
		if ( pReliableDecode+cbMsgSize > pReliableEnd )
		{
			// Ouch, we did all that work and still don't have the whole message.
			return true;
		}

		// We have a full message!  Queue it
		ReceivedMessage( pReliableDecode, cbMsgSize, nMsgNum, usecNow );
		pReliableDecode += cbMsgSize;
		int cbStreamConsumed = pReliableDecode-pReliableStart;

		// Advance bookkeeping
		m_receiverState.m_nLastRecvReliableMsgNum = nMsgNum;
		m_receiverState.m_nReliableStreamPos += cbStreamConsumed;

		// Remove the data from the from the front of the buffer
		pop_from_front( m_receiverState.m_bufReliableStream, cbStreamConsumed );

		// We might have more in the stream that is ready to dispatch right now.
		nNumReliableBytes -= cbStreamConsumed;
	} while ( nNumReliableBytes > 0 );

	return true;
}

bool CSteamNetworkConnectionBase::SNP_RecordReceivedPktNum( int64 nPktNum, SteamNetworkingMicroseconds usecNow )
{

	// Make sure the last received sequence number is never marked as being in a gap.
	// Since we did receive it!
	Assert( m_receiverState.m_mapPacketGaps.empty() || m_receiverState.m_mapPacketGaps.rbegin()->second.m_nEnd <= m_statsEndToEnd.m_nLastRecvSequenceNumber );

	// Check if sender has already told us they don't need us to
	// account for packets this old anymore
	if ( nPktNum < m_receiverState.m_nMinPktNumToSendAcks )
		return true;

	// Fast path for the (hopefully) common case of packets arriving in order
	if ( nPktNum == m_statsEndToEnd.m_nLastRecvSequenceNumber+1 )
		return true;

	// Check if this introduced a gap since the last sequence packet we have received
	if ( nPktNum > m_statsEndToEnd.m_nLastRecvSequenceNumber )
	{

		// Protect against malicious sender!
		if ( len( m_receiverState.m_mapPacketGaps ) >= k_nMaxPacketGaps )
			return false; // CALLER: Do not record that we received this packet!

		// Add a gap for the skipped packet(s)
		int64 nBegin = m_statsEndToEnd.m_nLastRecvSequenceNumber+1;
		SSNPPacketGap &gap = m_receiverState.m_mapPacketGaps[ nBegin ];
		gap.m_usecWhenReceivedPktBefore = m_statsEndToEnd.m_usecTimeLastRecvSeq;
		gap.m_nEnd = nPktNum;

		SpewType( steamdatagram_snp_log_packetgaps, "%s drop %d pkts [%lld-%lld)",
			m_sName.c_str(),
			(int)( nPktNum - nBegin ),
			(long long)nBegin, (long long)nPktNum );

		// Schedule sending of a NACK pretty quickly.
		// FIXME - really we should probably use two different timers.
		// If this timer expires, we should check if the gap still exists,
		// and if so, there's no need to do anything.  Because we want
		// packets arriving out of order close together to basically
		// be treated the same as arriving in order.
		m_receiverState.m_usecWhenFlushAck = std::min( m_receiverState.m_usecWhenFlushAck, usecNow + k_usecNackFlush );
	}
	else if ( !m_receiverState.m_mapPacketGaps.empty() )
	{
		// Check if this filed a gap
		auto itGap = m_receiverState.m_mapPacketGaps.upper_bound( nPktNum );
		--itGap;
		Assert( itGap->first <= nPktNum );
		if ( itGap->second.m_nEnd <= nPktNum )
			return true; // We already received this packet

		// Packet is in a gap where we previously thought packets were lost.
		// (Packets arriving out of order.)

		// Last packet in gap?
		if ( itGap->second.m_nEnd-1 == nPktNum )
		{
			// Single-packet gap?
			if ( itGap->first == nPktNum )
			{
				// Gap is totally filed
				m_receiverState.m_mapPacketGaps.erase( itGap );

				SpewType( steamdatagram_snp_log_packetgaps, "%s decode pkt %lld, single pkt gap filled", m_sName.c_str(), (long long)nPktNum );
			}
			else
			{
				// Shrink gap by one from the end
				--itGap->second.m_nEnd;
				Assert( itGap->first < itGap->second.m_nEnd );

				SpewType( steamdatagram_snp_log_packetgaps, "%s decode pkt %lld, last packet in gap, reduced to [%lld,%lld)", m_sName.c_str(),
					(long long)nPktNum, (long long)itGap->first, (long long)itGap->second.m_nEnd );
			}
		}
		else if ( itGap->first == nPktNum )
		{
			// First packet in multi-packet gap.
			// Shrink packet from the front
			// Cast away const to allow us to modify the key.
			// We know this won't break the map ordering
			++const_cast<int64&>( itGap->first );
			Assert( itGap->first < itGap->second.m_nEnd );
			itGap->second.m_usecWhenReceivedPktBefore = usecNow;

			SpewType( steamdatagram_snp_log_packetgaps, "%s decode pkt %lld, first packet in gap, reduced to [%lld,%lld)", m_sName.c_str(),
				(long long)nPktNum, (long long)itGap->first, (long long)itGap->second.m_nEnd );
		}
		else
		{
			// Packet is in the middle of the gap.  We'll need to fragment this gap
			// Protect against malicious sender!
			if ( len( m_receiverState.m_mapPacketGaps ) >= k_nMaxPacketGaps )
				return false; // CALLER: Do not record that we received this packet!

			// Save end
			int64 nEnd = itGap->second.m_nEnd;

			// Truncate this gap
			itGap->second.m_nEnd = nPktNum;
			Assert( itGap->first < itGap->second.m_nEnd );

			int64 nUpperBegin = nPktNum+1;

			SpewType( steamdatagram_snp_log_packetgaps, "%s decode pkt %lld, gap split [%lld,%lld) and [%lld,%lld)", m_sName.c_str(),
				(long long)nPktNum, (long long)itGap->first, (long long)itGap->second.m_nEnd, nUpperBegin, nEnd );

			// Insert a new gap to account for the upper end
			SSNPPacketGap &gap = m_receiverState.m_mapPacketGaps[ nUpperBegin ];
			gap.m_usecWhenReceivedPktBefore = usecNow;
			gap.m_nEnd = nEnd;
		}

	}

	// OK, this packet is legit, allow caller to continue processing it
	return true;
}

int CSteamNetworkConnectionBase::GetEffectiveMinRate() const
{
	int nResult = m_senderState.m_n_minRate;
	if ( nResult == 0 )
		nResult = steamdatagram_snp_min_rate;
	return Clamp( nResult, k_nSteamDatagramGlobalMinRate, k_nSteamDatagramGlobalMaxRate );
}

int CSteamNetworkConnectionBase::GetEffectiveMaxRate() const
{
	int nResult = m_senderState.m_n_maxRate;
	if ( nResult == 0 )
		nResult = steamdatagram_snp_max_rate;
	return Clamp( nResult, k_nSteamDatagramGlobalMinRate, k_nSteamDatagramGlobalMaxRate );
}

// RFC 3448, 4.3
void CSteamNetworkConnectionBase::SNP_UpdateX( SteamNetworkingMicroseconds usecNow )
{
//	int configured_min_rate = m_senderState.m_n_minRate ? m_senderState.m_n_minRate : steamdatagram_snp_min_rate;
//
//	int min_rate = MAX( configured_min_rate, m_senderState.m_n_x_recv * k_nBurstMultiplier ); 
//
//	SteamNetworkingMicroseconds usecPing = GetUsecPingWithFallback( this );
//
//	int n_old_x = m_senderState.m_n_x;
//	if ( m_senderState.m_un_p )
//	{
//		m_senderState.m_n_x = MAX( MIN( m_senderState.m_n_x_calc, min_rate ), m_senderState.m_n_tx_s );
//		m_senderState.m_n_x = MAX( m_senderState.m_n_x, configured_min_rate );
//	}
//	else if ( m_statsEndToEnd.m_ping.m_nSmoothedPing >= 0 && usecNow - m_senderState.m_usec_ld >= usecPing )
//	{   	   
//		m_senderState.m_n_x = MAX( MIN( 2 * m_senderState.m_n_x, min_rate ), GetInitialRate( usecPing ) );
//		m_senderState.m_usec_ld = usecNow;
//	}
//
//	// For now cap at steamdatagram_snp_max_rate
//	if ( m_senderState.m_n_maxRate )
//	{
//		m_senderState.m_n_x = MIN( m_senderState.m_n_x, m_senderState.m_n_maxRate );
//	}
//	else if ( steamdatagram_snp_max_rate )
//	{
//		m_senderState.m_n_x = MIN( m_senderState.m_n_x, steamdatagram_snp_max_rate );
//	}
//
//	if ( m_senderState.m_n_x != n_old_x ) 
//	{
//		if ( steamdatagram_snp_log_x )
//		SpewMsg( "%12llu %s: UPDATE X=%d (was %d) x_recv=%d min_rate=%d p=%u x_calc=%d tx_s=%d\n", 
//					 usecNow,
//					 m_sName.c_str(),
//					 m_senderState.m_n_x,
//					 n_old_x,
//					 m_senderState.m_n_x_recv,
//					 min_rate,
//					 m_senderState.m_un_p,
//					 m_senderState.m_n_x_calc,
//					 m_senderState.m_n_tx_s );
//	}
//
//	UpdateSpeeds( m_senderState.m_n_x, m_senderState.m_n_x_recv );

	m_senderState.m_n_x = Clamp( m_senderState.m_n_x, GetEffectiveMinRate(), GetEffectiveMaxRate() );
}

// Returns next think time
SteamNetworkingMicroseconds CSteamNetworkConnectionBase::SNP_ThinkSendState( SteamNetworkingMicroseconds usecNow )
{
	// Accumulate tokens based on how long it's been since last time
	m_senderState.TokenBucket_Accumulate( usecNow );

	// Calculate next time we want to take action.  If it isn't right now, then we're either idle or throttled.
	// Importantly, this will also check for retry timeout
	SteamNetworkingMicroseconds usecNextThink = SNP_GetNextThinkTime( usecNow );
	if ( usecNextThink > usecNow )
		return usecNextThink;

	// Keep sending packets until we run out of tokens
	int nPacketsSent = 0;
	for (;;)
	{

//		// If send feedback is peroidic but more than RTO/2 has passed force it
//		if ( m_senderState.m_sendFeedbackState == SSNPSenderState::TFRC_SSTATE_FBACK_PERODIC )
//		{
//			if ( m_senderState.m_usec_rto && usecNow - m_receiverState.m_usec_tstamp_last_feedback > m_senderState.m_usec_rto / 2 )
//			{
//				m_senderState.m_sendFeedbackState = SSNPSenderState::TFRC_SSTATE_FBACK_REQ;
//				if ( steamdatagram_snp_log_feedback )
//					SpewMsg( "%12llu %s: TFRC_SSTATE_FBACK_REQ due to rto/2 timeout\n",
//								usecNow,
//								m_sName.c_str() );
//			}
//			if ( !m_senderState.m_usec_rto && usecNow - m_receiverState.m_usec_tstamp_last_feedback > TCP_RTO_MIN / 2 )
//			{
//				m_senderState.m_sendFeedbackState = SSNPSenderState::TFRC_SSTATE_FBACK_REQ;
//				if ( steamdatagram_snp_log_feedback )
//					SpewMsg( "%12llu %s: TFRC_SSTATE_FBACK_REQ due to TCP_RTO_MIN/2 timeout\n",
//								usecNow,
//								m_sName.c_str() );
//			}
//		}
//			
//		bool bSendPacket = m_senderState.m_pSendMessages || 
//			m_senderState.m_bPendingNAK || 
//			m_senderState.m_sendFeedbackState == SSNPSenderState::TFRC_SSTATE_FBACK_REQ;
//
//		if ( !bSendPacket )
//			break;

		if ( nPacketsSent > k_nMaxPacketsPerThink )
		{
			// We're sending too much at one time.  Nuke token bucket so that
			// we'll be ready to send again very soon, but not immediately.
			// We don't want the outer code to complain that we are requesting
			// a wakeup call in the past
			m_senderState.m_flTokenBucket = m_senderState.m_n_x * -0.0005f;
			return usecNow + 1000;
		}

		int nBytesSent = SNP_SendPacket( usecNow, k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend, nullptr );
		if ( nBytesSent < 0 )
		{
			// Problem sending packet.  Nuke token bucket, but request
			// a wakeup relatively quick to check on our state again
			m_senderState.m_flTokenBucket = m_senderState.m_n_x * -0.001f;
			return usecNow + 2000;
		}

		// Nothing to send at this time
		if ( nBytesSent == 0 )
		{

			// We've sent everything we want to send.  Limit our reserve to a
			// small burst overage, in case we had built up an excess reserve
			// before due to the scheduler waking us up late.
			m_senderState.TokenBucket_Limit();
			break;
		}

		// We spent some tokens, do we have any left?
		if ( m_senderState.m_flTokenBucket < 0.0f )
			break;

		// Limit number of packets sent at a time, even if the scheduler is really bad
		// or somebody holds the lock for along time, or we wake up late for whatever reason
		++nPacketsSent;
	}

	// Return time when we need to check in again.
	return SNP_GetNextThinkTime( usecNow );
}

SteamNetworkingMicroseconds CSteamNetworkConnectionBase::SNP_GetNextThinkTime( SteamNetworkingMicroseconds usecNow )
{
	// We really shouldn't be trying to do this when not connected
	if ( GetState() != k_ESteamNetworkingConnectionState_Connected )
	{
		AssertMsg( false, "We shouldn't be trying to think SNP when not fully connected" );
		return k_nThinkTime_Never;
	}

	// Check retransmit timers.  If they have expired, this will move reliable
	// segments into the "ready to retry" list, which will cause
	// TimeWhenWantToSendNextPacket to think we want to send data.  If nothing has timed out,
	// it will return the time when we need to check back in.  Or, if everything is idle it will
	// return "never" (very large number).
	SteamNetworkingMicroseconds usecNextThink = SNP_SenderCheckInFlightPackets( usecNow );

	// If we want to send packets, then we might need to wake up and take action
	SteamNetworkingMicroseconds usecTimeWantToSend = m_senderState.TimeWhenWantToSendNextPacket();
	if ( usecTimeWantToSend < INT64_MAX )
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

		// Time when we will next send is greater of when we want to and when we can
		usecNextSend = Max( usecNextSend, usecTimeWantToSend );

		// Earlier than any other reason to wake up?
		usecNextThink = Min( usecNextThink, usecNextSend );
	}

	// Check if the receiver side needs to send an ack
	usecNextThink = std::min( usecNextThink, m_receiverState.m_usecWhenFlushAck );

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
	for ( SNPSendMessage_t *pMsg = m_senderState.m_messagesQueued.m_pFirst ; pMsg ; pMsg = pMsg->m_pNext )
	{
		info.m_nBytesQueuedForSend += pMsg->m_cbSize;
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
				 //" x_recv. . . . %d\n"
				 " x_calc. . . . %d\n"
				 " rtt . . . . . %dms\n"
				 //" p . . . . . . %.8f\n"
				 " tx_s. . . . . %d\n"
				 //" recvSeqNum. . %d\n"
				 " pendingB. . . %d\n"
				 " outReliableB. %d\n"
				 " msgsReliable. %lld\n"
				 " msgs. . . . . %lld\n"
				 " minRate . . . %d\n"
				 " maxRate . . . %d\n"
				 "\n"
				 "ReceiverState\n"
				 " x_recv. . . . %d\n"
				 " rx_s. . . . . %d\n"
				 " i_mean. . . . %d (%.8f)\n"
				 " msgsReliable. %lld\n"
				 " msgs. . . . . %lld\n",
				 m_sName.c_str(),
				 m_senderState.m_n_x,
				 //m_senderState.m_n_x_recv,
				 m_senderState.m_n_x_calc,
				 m_statsEndToEnd.m_ping.m_nSmoothedPing,
				 //(float)m_senderState.m_un_p / (float)UINT_MAX,
				 m_senderState.m_n_tx_s,
				 //m_senderState.m_unRecvSeqNum,
				 m_senderState.PendingBytesTotal(),
				 m_senderState.m_cbSentUnackedReliable,
				 m_senderState.m_nMessagesSentReliable,
				 m_senderState.m_nMessagesSentUnreliable,
				 m_senderState.m_n_minRate ? m_senderState.m_n_minRate : steamdatagram_snp_min_rate,
				 m_senderState.m_n_maxRate ? m_senderState.m_n_maxRate : steamdatagram_snp_max_rate,
				 //
				 m_receiverState.m_n_x_recv,
				 m_receiverState.m_n_rx_s,
				 m_receiverState.m_li_i_mean,
				 (float)m_receiverState.m_li_i_mean / (float)UINT_MAX,
				 m_receiverState.m_nMessagesRecvReliable,
				 m_receiverState.m_nMessagesRecvUnreliable
				 );

}

} // namespace SteamNetworkingSocketsLib


//void CSteamNetworkConnectionBase::SNP_MoveSentToSend( SteamNetworkingMicroseconds usecNow )
//{
//	// Called when we should move all of the messages in m_pSentMessages back into m_pSendMessages so they will be retransmitted,
//	// note we have to insert them back in the correct order
//	if ( !m_senderState.m_pSentMessages )
//	{
//		// In this case we are reseting the send msg
//		if ( m_senderState.m_pSendMessages )
//		{
//			SSNPSendMessage *pSendMsg = m_senderState.m_pSendMessages;
//			if ( pSendMsg->m_bReliable )
//			{
//				if ( !pSendMsg->m_vecSendPackets.IsEmpty() )
//				{
//					pSendMsg->m_nSendPos = pSendMsg->m_vecSendPackets[ 0 ].m_nOffset;
//					pSendMsg->m_vecSendPackets.RemoveAll();
//				}
//				else
//				{
//					pSendMsg->m_nSendPos = 0;
//				}
//			}
//		}
//	}
//	else
//	{
//		// Get pointer to last sent message
//		SSNPSendMessage **ppSentMsg = &m_senderState.m_pSentMessages;
//		while ( *ppSentMsg )
//		{
//			ppSentMsg = &(*ppSentMsg)->m_pNext;
//		}
//
//		// Have pointer to end of list, first up set position of first message and clear position of succeeding
//		SSNPSendMessage *pSentMsg = m_senderState.m_pSentMessages;
//		if ( pSentMsg->m_bReliable )
//		{
//			if ( !pSentMsg->m_vecSendPackets.IsEmpty() )
//			{
//				pSentMsg->m_nSendPos = pSentMsg->m_vecSendPackets[ 0 ].m_nOffset;
//				pSentMsg->m_vecSendPackets.RemoveAll();
//			}
//			else
//			{
//				pSentMsg->m_nSendPos = 0;
//			}
//		}
//
//		// any messages afterward are full re-trans
//		for ( pSentMsg = pSentMsg->m_pNext; pSentMsg; pSentMsg = pSentMsg->m_pNext )
//		{
//			if ( pSentMsg->m_bReliable )
//			{
//				pSentMsg->m_vecSendPackets.RemoveAll();
//				pSentMsg->m_nSendPos = 0;
//			}
//		}
//
//		// reset current send msg
//		if ( m_senderState.m_pSendMessages && m_senderState.m_pSendMessages->m_bReliable )
//		{
//			SSNPSendMessage *pSendMsg = m_senderState.m_pSendMessages;
//			pSendMsg->m_vecSendPackets.RemoveAll();
//			pSendMsg->m_nSendPos = 0;
//		}
//
//		// Push Sent to head of Send
//		*ppSentMsg = m_senderState.m_pSendMessages;
//		m_senderState.m_pSendMessages = m_senderState.m_pSentMessages;
//		m_senderState.m_pSentMessages = nullptr;
//	}
//
//	// recalc queued.
//	// UG - do we really need to do this?  This could be slow.  Can't we maintain this data
//	// as we go?
//	m_senderState.m_cbSentUnackedReliable = 0; // ???? This whole function is a giant mess.
//	m_senderState.m_cbPendingUnreliable = 0;
//	m_senderState.m_cbPendingReliable = 0;
//	for ( SSNPSendMessage *pSendMessage = m_senderState.m_pSendMessages; pSendMessage; pSendMessage = pSendMessage->m_pNext )
//	{
//		int cbPending = pSendMessage->m_nSize - pSendMessage->m_nSendPos;
//		if ( pSendMessage->m_bReliable )
//			m_senderState.m_cbPendingReliable += cbPending;
//		else
//			m_senderState.m_cbPendingUnreliable += cbPending;
//	}
//	for ( SSNPSendMessage *pQueuedMessage = m_senderState.m_pQueuedMessages; pQueuedMessage; pQueuedMessage = pQueuedMessage->m_pNext )
//	{
//		int cbPending = pQueuedMessage->m_nSize;
//		if ( pQueuedMessage->m_bReliable )
//			m_senderState.m_cbPendingReliable += cbPending;
//		else
//			m_senderState.m_cbPendingUnreliable += cbPending;
//	}
//
//}

//void CSteamNetworkConnectionBase::SNP_CheckForReliable( SteamNetworkingMicroseconds usecNow )
//{
//	// We start with Sent and then move to Send
//	// See if any messages that are waiting for acknowledgement are ready to go
//	SSNPSendMessage *pMsg = m_senderState.m_pSentMessages;
//	if ( !pMsg )
//	{
//		// If nothing waiting ack in sent, check current send message
//		pMsg = m_senderState.m_pSendMessages;
//	}
//
//	// Clear and move to next Sent message function
//	auto NextMsg = [&] 
//	{
//		// If we are at the first send message stop since its still being sent
//		if ( pMsg == m_senderState.m_pSendMessages )
//		{
//			pMsg = nullptr;
//		}
//		else
//		{
//			Assert( pMsg->m_bReliable );
//			m_senderState.m_pSentMessages = m_senderState.m_pSentMessages->m_pNext;
//			Assert( m_senderState.m_cbSentUnackedReliable >= pMsg->m_nSize );
//			m_senderState.m_cbSentUnackedReliable -= pMsg->m_nSize;
//			delete pMsg;
//			pMsg = m_senderState.m_pSentMessages;
//			if ( !pMsg )
//			{
//				pMsg = m_senderState.m_pSendMessages;
//			}
//		}
//	};
//
//	while ( pMsg )
//	{
//		if ( pMsg->m_vecSendPackets.IsEmpty() )
//		{
//			NextMsg();
//			continue;
//		}
//
//		if ( steamdatagram_snp_log_reliable )
//		SpewMsg( "%12llu %s: %s CheckForReliable: sentMsgNum=%d, recvMsgNum=%d, recvSeqNum=%d, sendSeqNum=%d sendSeqOffset=%d\n",
//					 usecNow,
//					 m_sName.c_str(),
//					 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
//					 pMsg->m_nMsgNum,
//					 m_senderState.m_unRecvMsgNumReliable,
//					 m_senderState.m_unRecvSeqNum,
//					 pMsg->m_vecSendPackets.Head().m_unSeqNum,
//					 pMsg->m_vecSendPackets.Head().m_nOffset );
//
//		// If the acknowledge message number is after this one, its received
//		if ( IsSeqAfter( m_senderState.m_unRecvMsgNumReliable, pMsg->m_unMsgNum ) )
//		{
//			if ( steamdatagram_snp_log_reliable )
//			SpewMsg( "%12llu %s: %s ACK recvMsgNum %d is after sentMsgNum %d, acknowledged\n", 
//						 usecNow,
//						 m_sName.c_str(),
//						 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
//						 m_senderState.m_unRecvMsgNumReliable, 
//						 pMsg->m_unMsgNum );
//
//			// Next
//			NextMsg();
//			continue;
//		}
//
//		// If the message number doesn't match current, we're waiting for ack
//		if ( m_senderState.m_unRecvMsgNumReliable != pMsg->m_unMsgNum )
//		{
//			if ( !pMsg->m_vecSendPackets.IsEmpty() )
//			{
//				SSNPSendMessage::SSendPacketEntry &sendPacketEntry = pMsg->m_vecSendPackets.Head();
//
//				// The other end might have lost the first packet of the current message, check if they are still
//				// ackowledging the previous message but the seqNum is higher
//				if ( m_senderState.m_unLastAckMsgNumReliable == m_senderState.m_unRecvMsgNumReliable &&
//					 m_senderState.m_unLastAckMsgAmtReliable == m_senderState.m_unRecvMsgAmtReliable &&
//					 IsSeqAfterOrEq( m_senderState.m_unRecvSeqNum, sendPacketEntry.m_unSeqNum ) )
//				{
//					// They received the packet of this message (or after), but didn't get the first section
//					// we need to resend
//
//					// Should only occur on the first section
//					Assert( sendPacketEntry.m_nOffset == 0 );
//					if ( steamdatagram_snp_log_reliable )
//					SpewMsg( "%12llu %s: %s NAK sentMsgNum %d: recvSeqNum %d is GTE sentSeqNum %d, but ack is previous msg %d:%u\n", 
//								 usecNow,
//								 m_sName.c_str(),
//								 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
//								 pMsg->m_unMsgNum,
//								 m_senderState.m_unRecvSeqNum, 
//								 sendPacketEntry.m_unSeqNum, 
//								 m_senderState.m_unRecvMsgNumReliable, 
//								 m_senderState.m_unRecvMsgAmtReliable );
//
//					SNP_MoveSentToSend( usecNow );
//					return; // No need to check more we're going to start all over again
//				}
//			}
//
//			if ( steamdatagram_snp_log_reliable )
//			SpewMsg( "%12llu %s: %s recvMsgNum %d != sentMsgNum %d, lastAck %d:%u\n", 
//					 usecNow,
//					 m_sName.c_str(),
//					 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
//					 m_senderState.m_unRecvMsgNumReliable, 
//					 pMsg->m_unMsgNum,
//					 m_senderState.m_unLastAckMsgNumReliable,
//					 m_senderState.m_unLastAckMsgAmtReliable );
//			break; // We will check retransmit timeout below
//		}
//
//		// Pull out sent entries on acknowledgement
//		while ( !pMsg->m_vecSendPackets.IsEmpty() )
//		{
//			SSNPSendMessage::SSendPacketEntry &sendPacketEntry = pMsg->m_vecSendPackets.Head();
//
//			// If the received seq num is after or equal the sent, check if we've received this entry
//			if ( IsSeqAfterOrEq( m_senderState.m_unRecvSeqNum, sendPacketEntry.m_unSeqNum ) )
//			{
//				if ( m_senderState.m_unRecvMsgAmtReliable < (uint32)sendPacketEntry.m_nSentAmt )
//				{
//					if ( steamdatagram_snp_log_reliable )
//					SpewMsg( "%12llu %s: %s NAK sentMsgNum %d: recvSeqNum %d is GTE sentSeqNum %d, but m_unRecvMsgAmt %d is less than m_nSentAmt %d\n", 
//								 usecNow,
//								 m_sName.c_str(),
//								 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
//								 pMsg->m_unMsgNum,
//								 m_senderState.m_unRecvSeqNum, 
//								 sendPacketEntry.m_unSeqNum, 
//								 m_senderState.m_unRecvMsgAmtReliable, 
//								 sendPacketEntry.m_nSentAmt );
//					SNP_MoveSentToSend( usecNow );
//					return; // No need to check more we're going to start all over again
//				}
//				else
//				{
//					if ( steamdatagram_snp_log_reliable )
//					SpewMsg( "%12llu %s: %s ACK sentMsgNum %d: recvSeqNum %d is GTE sentSeqNum %d, m_unRecvMsgAmt %d is GTE than m_nSentAmt %d\n", 
//								 usecNow,
//								 m_sName.c_str(),
//								 pMsg == m_senderState.m_pSendMessages ? "SEND" : "SENT",
//								 pMsg->m_unMsgNum,
//								 m_senderState.m_unRecvSeqNum, 
//								 sendPacketEntry.m_unSeqNum, 
//								 m_senderState.m_unRecvMsgAmtReliable, 
//								 sendPacketEntry.m_nSentAmt );
//					// This portion is acknowledged
//					pMsg->m_vecSendPackets.Remove( 0 );
//				}
//			}
//			else
//			{
//				break; // Not received yet
//			}
//		}
//
//		if ( pMsg != m_senderState.m_pSendMessages && pMsg->m_vecSendPackets.IsEmpty() )
//		{
//			if ( steamdatagram_snp_log_reliable )
//			SpewMsg( "%12llu %s: SENT Finished sentMsgNum %d lastAck %d:%u\n", 
//					 usecNow,
//					 m_sName.c_str(),
//					 pMsg->m_unMsgNum,
//					 m_senderState.m_unRecvMsgNumReliable,
//					 m_senderState.m_unRecvMsgAmtReliable );
//
//			// Note we record this ack, we need it in case the other end misses the first section
//			// of the next message and we need to double check if we need to retransmit it
//			m_senderState.m_unLastAckMsgNumReliable = m_senderState.m_unRecvMsgNumReliable;
//			m_senderState.m_unLastAckMsgAmtReliable = m_senderState.m_unRecvMsgAmtReliable;
//
//			// All done!
//			NextMsg();
//			continue;
//		}
//		else
//		{
//			break; // still pending
//		}
//	}
//
//	// If pSentMsg is not null here, we are still waiting for ack, check if we need to start sending again due to RTO
//	if ( pMsg )
//	{
//		if ( m_senderState.m_usec_rto &&
//			 !pMsg->m_vecSendPackets.IsEmpty() &&
//			 pMsg->m_vecSendPackets.Head().m_usec_sentTime - usecNow > m_senderState.m_usec_rto )
//		{
//			if ( steamdatagram_snp_log_reliable || steamdatagram_snp_log_loss )
//			SpewMsg( "%12llu %s: RTO sentMsgNum %d\n", 
//						 usecNow,
//						 m_sName.c_str(),
//						 pMsg->m_unMsgNum );
//
//			// Set for retransmit due to timeout
//			SNP_MoveSentToSend( usecNow );
//		}
//	}
//}
//
//// Called when receiver wants to send feedback, sets up the next packet transmit to include feedback
//void CSteamNetworkConnectionBase::SNP_PrepareFeedback( SteamNetworkingMicroseconds usecNow )
//{
//	// Update x_recv
//	SteamNetworkingMicroseconds usec_delta = usecNow - m_receiverState.m_usec_tstamp_last_feedback;
//	if ( usec_delta )
//	{
//		int n_x_recv = k_nMillion * m_receiverState.m_n_bytes_recv / usec_delta;
//		// if higher take, otherwise smooth
//		if ( n_x_recv > m_receiverState.m_n_x_recv )
//		{
//			m_receiverState.m_n_x_recv = n_x_recv;
//		}
//		else
//		{
//			m_receiverState.m_n_x_recv = tfrc_ewma( m_receiverState.m_n_x_recv, n_x_recv, 9 );
//		}
//
//		if ( steamdatagram_snp_log_feedback )
//			SpewMsg( "%12llu %s: TFRC_FBACK_PERIODIC usec_delta=%llu bytes_recv=%d n_x_recv=%d m_n_x_recv=%d\n",
//					 usecNow,
//					 m_sName.c_str(),
//					 usec_delta,
//					 m_receiverState.m_n_bytes_recv,
//					 n_x_recv,
//					 m_receiverState.m_n_x_recv );
//	}
//
//	m_receiverState.m_usec_tstamp_last_feedback = usecNow;
//	m_receiverState.m_usec_next_feedback = usecNow + GetUsecPingWithFallback( this );
//	m_receiverState.m_n_bytes_recv = 0;
//}
//
//bool CSteamNetworkConnectionBase::SNP_CalcIMean( SteamNetworkingMicroseconds usecNow )
//{
//	static const float tfrc_lh_weights[NINTERVAL] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.8f, 0.6f, 0.4f, 0.2f };
//
//	// RFC 3448, 5.4
//
//	int i_i;
//	float i_tot0 = 0, i_tot1 = 0, w_tot = 0;
//
//	if ( m_receiverState.m_vec_li_hist.empty() )
//		return false;
//
//	int i = 0;
//	for ( auto li_iterator = m_receiverState.m_vec_li_hist.rbegin() ; li_iterator != m_receiverState.m_vec_li_hist.rend() && i < NINTERVAL ; ++li_iterator )
//	{
//		i_i = li_iterator->m_un_length;
//
//		i_tot0 += i_i * tfrc_lh_weights[i];
//		w_tot += tfrc_lh_weights[i];
//
//		if (i > 0)
//			i_tot1 += i_i * tfrc_lh_weights[i-1];
//
//		++i;
//	}
//
//	float f_mean = Max( i_tot0, i_tot1 ) / w_tot;
//
//	m_receiverState.m_li_i_mean = (uint32)( ( 1.0f / f_mean ) * (float)UINT_MAX );
//	return true;
//}
//
//bool CSteamNetworkConnectionBase::SNP_AddLossEvent( uint16 unSeqNum, SteamNetworkingMicroseconds usecNow )
//{
//	// See if this loss is a new loss
//	Assert( !m_receiverState.m_queue_rx_hist.empty() );
//	uint16 lastRecvSeqNum = m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum;
//	uint16 firstLossSeqNum = lastRecvSeqNum + 1;
//	uint16 lastLossSeqNum = unSeqNum - 1;
//	Assert( SeqDist( lastLossSeqNum, firstLossSeqNum ) >= 0 );
//
//	if ( !m_receiverState.m_vec_li_hist.empty() )
//	{
//		// RFC 3448, 5.2
//		// 
//		// For a lost packet, we can interpolate to infer the nominal "arrival time".  Assume:
//		// 
//		//       S_loss is the sequence number of a lost packet.
//		// 
//		//       S_before is the sequence number of the last packet to arrive with
//		//       sequence number before S_loss.
//		// 
//		//       S_after is the sequence number of the first packet to arrive with
//		//       sequence number after S_loss.
//		// 
//		//       T_before is the reception time of S_before.
//		// 
//		//       T_after is the reception time of S_after.
//		// 
//		//    Note that T_before can either be before or after T_after due to
//		//    reordering.
//		// 
//		//    For a lost packet S_loss, we can interpolate its nominal "arrival
//		//    time" at the receiver from the arrival times of S_before and S_after.
//		//    Thus:
//		// 
//		//    T_loss = T_before + ( (T_after - T_before)
//		//                * (S_loss - S_before)/(S_after - S_before) )
//
//		SteamNetworkingMicroseconds before_ts = m_receiverState.m_queue_rx_hist.back().m_usec_recvTs;
//		SteamNetworkingMicroseconds after_ts = usecNow;
//
//		uint16 S_before = lastRecvSeqNum;
//		uint16 S_after = unSeqNum;
//
//		SteamNetworkingMicroseconds firstloss_ts = before_ts + 
//			( (after_ts - before_ts ) * SeqDist( firstLossSeqNum, S_before ) / SeqDist( S_after, S_before ) );
//
//		SteamNetworkingMicroseconds usec_rtt = m_receiverState.m_queue_rx_hist.back().m_usecPing;
//		if ( firstloss_ts - m_receiverState.m_vec_li_hist.back().m_ts <= usec_rtt )
//		{
//			// This is in the same loss interval
//			return false;
//		}
//	}
//
//	// Cull if full
//	if ( m_receiverState.m_vec_li_hist.size() == LIH_SIZE )
//	{
//		m_receiverState.m_vec_li_hist.pop_front();
//	}
//	
//	// New loss interval
//	SSNPReceiverState::S_lh_hist hist;
//	hist.m_ts = usecNow;
//	hist.m_un_seqno = firstLossSeqNum;
//	hist.m_b_is_closed = false;
//	m_receiverState.m_vec_li_hist.push_back( hist );
//
//	auto plh_hist = m_receiverState.m_vec_li_hist.rbegin();
//
//	// Update previous loss interval length if needed
//	if ( m_receiverState.m_vec_li_hist.size() == 1 )
//	{
//		// First loss interval, we have to do some special handling to calc i_mean
//		// via RFC 3448 6.3.1
//
//		// We need to find the p value that results in something close to x_recv
//		SteamNetworkingMicroseconds usec_rtt = m_receiverState.m_queue_rx_hist.back().m_usecPing;
//		SteamNetworkingMicroseconds usec_delta = usecNow - m_receiverState.m_usec_tstamp_last_feedback;
//		int n_cur_x_recv = usec_delta ? k_nMillion * m_receiverState.m_n_bytes_recv / usec_delta : 0;
//		int n_x_recv = MAX( n_cur_x_recv * k_nBurstMultiplier / 2, 
//							m_receiverState.m_n_x_recv * k_nBurstMultiplier / 2 );
//
//		if ( !n_x_recv )
//		{
//			n_x_recv = GetInitialRate( usec_rtt );
//		}
//
//		int n_s = m_receiverState.m_n_rx_s;
//		if ( !n_s )
//		{
//			n_s = k_cbSteamNetworkingSocketsMaxEncryptedPayload;
//		}
//
//		// Find a value of p that matches within 5% of x_recv using binary search
//		int x_cur = 0;
//		float cur_p = 0.5f;
//		while ( 100llu * (uint64)x_cur / (uint64)n_x_recv < 95llu ) // 5% tolerance
//		{
//			x_cur = TFRCCalcX( m_receiverState.m_n_rx_s, usec_rtt, cur_p );
//			// Too small
//			if ( x_cur == 0 )
//			{
//				cur_p = 0;
//				break;
//			}
//			if ( x_cur < n_x_recv )
//			{
//				cur_p -= cur_p / 2.0f;
//			}
//			else
//			{
//				cur_p += cur_p / 2.0f;
//			}
//		}
//
//		// Set the packet number to a value that results in this p value
//		uint16 len = cur_p ? (uint16)( MIN( USHRT_MAX, (uint32) ( 1.0f / cur_p ) ) ) : 1;
//
//		uint16 firstSeqNum = unSeqNum - len;
//		plh_hist->m_un_seqno = firstSeqNum;
//		plh_hist->m_un_length = SeqDist( unSeqNum, firstSeqNum );
//
//		Assert( plh_hist->m_un_length == len );
//
//		if ( steamdatagram_snp_log_loss )
//		SpewMsg( "%12llu %s: LOSS INITIAL: x_recv: %d, cur_p: %.8f len: %d\n",
//				 usecNow,
//				 m_sName.c_str(),
//				 n_x_recv,
//				 cur_p,
//				 len );
//	}
//	else
//	{
//		plh_hist->m_un_length = SeqDist( unSeqNum, firstLossSeqNum );
//
//		// Back up one more to the one before this
//		auto pprev_lh_hist = plh_hist;
//		++pprev_lh_hist;
//		pprev_lh_hist->m_un_length = SeqDist( firstLossSeqNum, pprev_lh_hist->m_un_seqno );
//	}
//
//	SNP_CalcIMean( usecNow );
//	return true;
//}
//
//// This returns true if i_mean gets smaller and the sender should reduce rate
//bool CSteamNetworkConnectionBase::SNP_UpdateIMean( uint16 unSeqNum, SteamNetworkingMicroseconds usecNow )
//{
//	if ( m_receiverState.m_vec_li_hist.empty() )
//	{
//		return false;
//	}
//
//	SSNPReceiverState::S_lh_hist &lh_hist = m_receiverState.m_vec_li_hist.back();
//
//	// Cap at USHRT_MAX
//	if ( lh_hist.m_un_length < USHRT_MAX )
//	{
//		uint16 len = SeqDist( unSeqNum, lh_hist.m_un_seqno ) + 1;
//		if ( len < lh_hist.m_un_length ) // We wrapped
//		{
//			lh_hist.m_un_length = USHRT_MAX;
//		}
//		else
//		{
//			lh_hist.m_un_length = (uint16)len;
//		}
//	}
//
//	uint16 old_i_mean = m_receiverState.m_li_i_mean;
//	SNP_CalcIMean( usecNow );
//	return m_receiverState.m_li_i_mean < old_i_mean;
//}

//void CSteamNetworkConnectionBase::SNP_NoFeedbackTimer( SteamNetworkingMicroseconds usecNow )
//{
//	SteamNetworkingMicroseconds usecPing = GetUsecPingWithFallback( this );
//	int recover_rate = GetInitialRate( usecPing );
//
//	// Reset feedback state to "no feedback received"
//	if ( m_senderState.m_e_tx_state == SSNPSenderState::TFRC_SSTATE_FBACK)
//	{
//		m_senderState.m_e_tx_state = SSNPSenderState::TFRC_SSTATE_NO_FBACK;
//	}
//
//	int n_old_x = m_senderState.m_n_x;
//
//	// Determine new allowed sending rate X as per RFC5348 4.4
//
//	// sender does not have an RTT sample, has not received any feedback from receiver,
//	// and has not been idle ever since the nofeedback timer was set
//	if ( m_statsEndToEnd.m_ping.m_nSmoothedPing < 0 && m_senderState.m_usec_rto == 0 && m_senderState.m_bSentPacketSinceNFB )
//	{
//		/* halve send rate directly */
//		m_senderState.m_n_x = MAX( m_senderState.m_n_x / 2, k_cbSteamNetworkingSocketsMaxEncryptedPayload );
//	}
//	else if ( ( ( m_senderState.m_un_p > 0 && m_senderState.m_n_x_recv < recover_rate ) ||
//			    ( m_senderState.m_un_p == 0 && m_senderState.m_n_x < 2 * recover_rate ) ) &&
//			  !m_senderState.m_bSentPacketSinceNFB )
//	{
//		// Don't halve the allowed sending rate.
//		// Do nothing
//	}
//	else if ( m_senderState.m_un_p == 0 )
//	{
//		// We do not have X_Bps yet.
//		// Halve the allowed sending rate.		
//		int configured_min_rate = m_senderState.m_n_minRate ? m_senderState.m_n_minRate : steamdatagram_snp_min_rate;
//		m_senderState.m_n_x = MAX( configured_min_rate, MAX( m_senderState.m_n_x / 2, k_cbSteamNetworkingSocketsMaxEncryptedPayload ) );
//	}
//	else if ( m_senderState.m_n_x_calc > k_nBurstMultiplier * m_senderState.m_n_x_recv )
//	{
//		// 2*X_recv was already limiting the sending rate.
//		// Halve the allowed sending rate.
//		m_senderState.m_n_x_recv = m_senderState.m_n_x_recv / 2;
//		SNP_UpdateX( usecNow );
//	}
//	else
//	{
//		// The sending rate was limited by X_Bps, not by X_recv.
//		// Halve the allowed sending rate.
//    	m_senderState.m_n_x_recv = m_senderState.m_n_x_calc / 2;
//		SNP_UpdateX( usecNow );
//	}
//
//	// Set new timeout for the nofeedback timer.	
//	m_senderState.SetNoFeedbackTimer( usecNow );
//	m_senderState.m_bSentPacketSinceNFB = false;
//
//	if ( steamdatagram_snp_log_feedback )
//		SpewMsg( "%12llu %s: NO FEEDBACK TIMER X=%d, was %d, timer is %llu (rtt is %dms)\n",
//				 usecNow,
//				 m_sName.c_str(),
//				 m_senderState.m_n_x,
//				 n_old_x,
//				 m_senderState.m_usec_nfb - usecNow,
//				 m_statsEndToEnd.m_ping.m_nSmoothedPing );
//}

//// 0 for no loss
//// 1 for loss
//// -1 for discard (out of order)
//int CSteamNetworkConnectionBase::SNP_CheckForLoss( uint16 unSeqNum, SteamNetworkingMicroseconds usecNow )
//{
//	if ( !m_receiverState.m_queue_rx_hist.empty() && SeqDist( unSeqNum, m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum ) > 1 )
//	{
//		int nSeqDelta = SeqDist( unSeqNum, m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum );
//		if ( nSeqDelta > USHRT_MAX/2 ) // Out of order
//		{
//			SpewMsg( "%12llu %s: RECV OOO PACKET(S) %d (wanted %d)\n",
//				usecNow,
//				m_sName.c_str(),
//				unSeqNum,
//				( uint16 )( m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum + 1 ) );
//
//			// We're totally fine with out of order packets, just accept them.
//			return 0;
//		}
//
//		uint16 first = m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum + 1;
//		uint16 second = unSeqNum - 1;
//		if ( steamdatagram_snp_log_packet || steamdatagram_snp_log_loss )
//		SpewMsg( "%12llu %s: RECV LOST %d PACKET(S) %d - %d\n", 
//				 usecNow,
//				 m_sName.c_str(),
//				 nSeqDelta - 1,
//				 first,
//				 second );
//
//		// Add a lost event
//		SNP_AddLossEvent( unSeqNum, usecNow );
//
//		// If we detect loss, we should send a packet on the next interval
//		// This is so the sender can quickly determine if a retransmission is necesary
//		m_senderState.m_bPendingNAK = true;
//
//		return 1;
//	}
//	return 0;
//}
//
//void CSteamNetworkConnectionBase::SNP_RecordPacket( int64 nPktNum, SteamNetworkingMicroseconds usecNow )
//{
//	// Record the packet
//	if ( m_receiverState.m_queue_rx_hist.size() >= TFRC_NDUPACK )
//		m_receiverState.m_queue_rx_hist.pop_front();
//	
//	SSNPReceiverState::S_rx_hist rx_hist;
//	rx_hist.m_unRecvSeqNum = unSeqNum;
//	rx_hist.m_usecPing = unRtt;
//	rx_hist.m_usec_recvTs = usecNow;
//	m_receiverState.m_queue_rx_hist.push_back( rx_hist );
//}
//
//int CSteamNetworkConnectionBase::SNP_SendPacket( SteamNetworkingMicroseconds usecNow )
//{
//	SSNPBuffer sendBuf;
//
//	SNPPacketHdr *pHdr = (SNPPacketHdr *)sendBuf.m_buf;
//	//pHdr->m_unRtt = LittleWord( static_cast<uint16>( m_senderState.m_usec_rtt / 1000 ) );
//
//	uint16 recvNum = USHRT_MAX;
//	if ( !m_receiverState.m_queue_rx_hist.empty() )
//	{
//		recvNum = m_receiverState.m_queue_rx_hist.back().m_unRecvSeqNum;
//	}
//	pHdr->m_unRecvSeqNum = LittleWord( recvNum );
//	pHdr->m_unRecvMsgNum = LittleWord( m_receiverState.m_unLastReliableRecvMsgNum );
//	pHdr->m_unRecvMsgAmt = LittleDWord( m_receiverState.m_unLastReliabeRecvMsgAmt );
//
//	sendBuf.m_nSize += sizeof( SNPPacketHdr );
//
//	// Do we need to put in a feedback packet?
//	if ( m_senderState.m_sendFeedbackState != SSNPSenderState::TFRC_SSTATE_FBACK_NONE )
//	{
//		SNP_PrepareFeedback( usecNow );
//
//		// Insert segment
//		SNP_InsertSegment( &sendBuf, kPacketSegmentFlags_Feedback, sizeof( SNPPacketSegmentFeedback ) );
//
//		SNPPacketSegmentFeedback *pFeedback = ( SNPPacketSegmentFeedback * )( sendBuf.m_buf + sendBuf.m_nSize );
//
//		uint32 rx_hist_t_delay = 0;
//		if ( !m_receiverState.m_queue_rx_hist.empty() )
//		{
//			rx_hist_t_delay = static_cast< uint32 >( ( usecNow - m_receiverState.m_queue_rx_hist.back().m_usec_recvTs ) );
//		}
//
//		pFeedback->m_un_t_delay = LittleDWord( rx_hist_t_delay );
//		pFeedback->m_un_X_recv = LittleDWord( m_receiverState.m_n_x_recv );
//		pFeedback->m_un_p = LittleDWord( m_receiverState.m_li_i_mean );
//
//		m_senderState.m_sendFeedbackState = SSNPSenderState::TFRC_SSTATE_FBACK_NONE;
//
//		sendBuf.m_nSize += sizeof( SNPPacketSegmentFeedback );
//	}
//
//	CUtlVector< SSendPacketEntryMsg > vecSendPacketEntryMsg;
//
//	SNPSendMessage_t *pDeleteMessages = nullptr;
//
//	// Send msg pieces
//	SNPSendMessage_t *pCurMsg = m_senderState.m_pSendMessages;
//	while ( pCurMsg )
//	{
//		// Figure out size based on room left in packet and next segment
//
//		// How many bytes left in the message
//		int nMsgRemainingSize = pCurMsg->m_nSize - pCurMsg->m_nSendPos;
//
//		// How many bytes in the buffer, this will be prepended by segment headers
//		int nMsgRemainingBuffer = k_cbSteamNetworkingSocketsMaxEncryptedPayload - sendBuf.m_nSize;
//
//		// Is there room?
//		int msgHeaderSize = sizeof( SNPPacketSegmentType ) + sizeof( SNPPacketSegmentMessage );
//		if ( nMsgRemainingBuffer <= msgHeaderSize )
//			break;
//
//		int nSendSize = MIN( nMsgRemainingSize, nMsgRemainingBuffer - msgHeaderSize );
//		if ( !nSendSize ) // no room?  TODO: should there be a minimum?  This will squeeze one byte in
//		{
//			break; 
//		}
//
//		// Is this the last block?
//		bool bIsLastSegment = ( pCurMsg->m_nSendPos + nSendSize >= pCurMsg->m_nSize );
//
//		uint8 unSegmentFlags = kPacketSegmentFlags_Message;
//		if ( pCurMsg->m_bReliable )
//		{
//			unSegmentFlags |= kPacketSegmentFlags_Reliable;
//		}
//		if ( bIsLastSegment )
//		{
//			unSegmentFlags |= kPacketSegmentFlags_End;
//		}
//
//		SNP_InsertSegment( &sendBuf, unSegmentFlags, nSendSize + sizeof( SNPPacketSegmentMessage ) );
//
//		// Now insert the message segment header
//		SNPPacketSegmentMessage *pSegmentMessage = ( SNPPacketSegmentMessage * )( sendBuf.m_buf + sendBuf.m_nSize );
//		pSegmentMessage->m_unMsgNum = LittleWord( pCurMsg->m_unMsgNum );
//		pSegmentMessage->m_unOffset = LittleDWord( pCurMsg->m_nSendPos );
//
//		sendBuf.m_nSize += sizeof( SNPPacketSegmentMessage );
//
//		// Copy the bytes
//		memcpy( sendBuf.m_buf + sendBuf.m_nSize, pCurMsg->m_pData + pCurMsg->m_nSendPos, nSendSize );
//		sendBuf.m_nSize += nSendSize;
//
//		// Allocate an entry so we can record this send after we get a packet number
//		{
//			SSendPacketEntryMsg *pEntry = vecSendPacketEntryMsg.AddToTailGetPtr();
//			pEntry->m_bReliable = pCurMsg->m_bReliable;
//			pEntry->m_pMsg = pCurMsg;
//			pEntry->m_sendPacketEntry.m_usec_sentTime = usecNow; // Will be filed in later
//			pEntry->m_sendPacketEntry.m_unSeqNum = 0; // Will be filed in later
//			pEntry->m_sendPacketEntry.m_nOffset = pCurMsg->m_nSendPos;
//			pEntry->m_sendPacketEntry.m_nSentAmt = pCurMsg->m_nSendPos + nSendSize;
//		}
//
//		// Move message position
//		pCurMsg->m_nSendPos += nSendSize;
//
//		// Remove from pending bytes
//		if ( pCurMsg->m_bReliable )
//		{
//			Assert( m_senderState.m_cbPendingReliable >= nSendSize );
//			m_senderState.m_cbPendingReliable -= nSendSize;
//		}
//		else
//		{
//			Assert( m_senderState.m_cbPendingUnreliable >= nSendSize );
//			m_senderState.m_cbPendingUnreliable -= nSendSize;
//		}
//
//		// If this message is done, move it to SentMessages and go next
//		if ( bIsLastSegment )
//		{
//			// Unlink it
//			m_senderState.m_pSendMessages = pCurMsg->m_pNext;
//			pCurMsg->m_pNext = nullptr;
//
//			// If it's reliable move it to the sent queue as we may need to re-xmit
//			if ( pCurMsg->m_bReliable )
//			{
//				// Add it to the end of sent queue
//				SNPSendMessage_t **ppSentMessages = &m_senderState.m_pSentMessages;
//				while ( *ppSentMessages )
//				{
//					ppSentMessages = &(*ppSentMessages)->m_pNext;
//				}
//				*ppSentMessages = pCurMsg;
//				m_senderState.m_cbSentUnackedReliable += pCurMsg->m_nSize;
//			}
//			else
//			{
//				// move unreliable to the delete list
//				pCurMsg->m_pNext = pDeleteMessages;
//				pDeleteMessages = pCurMsg;
//			}
//
//			pCurMsg = m_senderState.m_pSendMessages;
//			continue; // Try to fit next message
//		}
//
//		// If here we put as much as we can into the message
//		break;
//	}
//
//	// Send this packet
//	uint16 sendSeqNum = 0;
//	int sendSize = EncryptAndSendDataChunk( sendBuf.m_buf, sendBuf.m_nSize, usecNow, &sendSeqNum );
//
//	// Note we sent a packet during the NFB period
//	m_senderState.m_bSentPacketSinceNFB = true;
//
//	if ( steamdatagram_snp_log_packet )
//	SpewMsg( "%12llu %s: SEND PACKET %d usecNow=%llu sz=%d(%d) recvSeqNum:%d recvMsgNum:%d recvMsgAmt:%d\n", 
//				 usecNow,
//				 m_sName.c_str(),
//				 sendSeqNum,
//				 usecNow,
//				 sendBuf.m_nSize,
//				 sendSize,
//				 LittleWord( pHdr->m_unRecvSeqNum ),
//				 LittleWord( pHdr->m_unRecvMsgNum ),
//				 LittleDWord( pHdr->m_unRecvMsgAmt ) );
//
//	if ( sendSize )
//	{
//		/**
//		 *	Track the mean packet size `s'
//		 *  size: packet payload size in bytes, this should by the wire
//		 *  size so include steam datagram header, UDP header and IP
//		 *  header.
//		 *
//		 *	cf. RFC 4342, 5.3 and  RFC 3448, 4.1
//		 */
//		m_senderState.m_n_tx_s = tfrc_ewma( m_senderState.m_n_tx_s, sendSize, 9 );
//
//		// Add to history
//		SSNPSenderState::S_tx_hist_entry tx_hist_entry;
//		tx_hist_entry.m_unSeqNum = sendSeqNum;
//		tx_hist_entry.m_usec_ts = usecNow;
//		m_senderState.m_tx_hist.push_back( tx_hist_entry );
//	}
//
//	// Update entries with send seq
//	// Note that on failure seqNum is 0 which will cause a retransmit in the recv if the other end tells us so
//	for ( auto &sendPacketEntryMsg : vecSendPacketEntryMsg )
//	{
//		SNPSendMessage_t::SSendPacketEntry *pEntry = &sendPacketEntryMsg.m_sendPacketEntry;
//		if ( sendPacketEntryMsg.m_bReliable )
//		{
//			pEntry->m_unSeqNum = sendSeqNum;
//			sendPacketEntryMsg.m_pMsg->m_vecSendPackets.AddToTail( *pEntry );
//		}
//
//		if ( steamdatagram_snp_log_segments )
//		SpewMsg( "%12llu %s: %s  %d: msgNum %d offset=%d sendAmt=%d segmentSize=%d%s\n", 
//					 usecNow,
//					 m_sName.c_str(),
//					 sendPacketEntryMsg.m_bReliable ? "RELIABLE  " : "UNRELIABLE",
//					 sendSeqNum,
//					 sendPacketEntryMsg.m_pMsg->m_unMsgNum,
//					 pEntry->m_nOffset,
//					 pEntry->m_nSentAmt,
//					 pEntry->m_nSentAmt - pEntry->m_nOffset,
//					 pEntry->m_nSentAmt >= sendPacketEntryMsg.m_pMsg->m_nSize ? " (end)" : "" );
//	}
//
//	// Pending deletions (completed unreliable messages)
//	while ( pDeleteMessages )
//	{
//		SNPSendMessage_t *pNext = pDeleteMessages->m_pNext;
//		delete pDeleteMessages;
//		pDeleteMessages = pNext;
//	}
//
//	return sendSize;
//}

//void CSteamNetworkConnectionBase::SNP_RecvDataChunk( uint64 nPktNum, const void *pChunk, int cbChunk, int cbPacketSize, SteamNetworkingMicroseconds usecNow )
//{
//	if ( cbChunk < sizeof( SNPPacketHdr ) )
//		return; // Not enough in the packet?
//
//	const SNPPacketHdr *pHdr = static_cast< const SNPPacketHdr * >( pChunk );
//
//	// Update sender's view from the receiver position
//	m_senderState.m_unRecvSeqNum = LittleWord( pHdr->m_unRecvSeqNum );
//	m_senderState.m_unRecvMsgNumReliable = LittleWord( pHdr->m_unRecvMsgNum );
//	m_senderState.m_unRecvMsgAmtReliable = LittleDWord( pHdr->m_unRecvMsgAmt );
//
//	if ( steamdatagram_snp_log_packet )
//	SpewMsg( "%12llu %s: RECV PACKET %d usecNow=%llu sz=%d(%d) recvSeqNum:%d recvMsgNum:%d recvMsgAmt:%d\n",
//				 usecNow,
//				 m_sName.c_str(),
//				 unSeqNum,
//				 usecNow,
//				 cbChunk,
//				 cbPacketSize,
//				 LittleWord( pHdr->m_unRecvSeqNum ),
//				 LittleWord( pHdr->m_unRecvMsgNum ),
//				 LittleDWord( pHdr->m_unRecvMsgAmt ) );
//
//	bool bIsDataPacket = false;
//
//	// Check if packet has actual data, i.e isn't control only
//	// this is true if it has a segment with a message payload
//	int nPos = sizeof( SNPPacketHdr );
//	while ( nPos < cbChunk )
//	{
//		if ( nPos + (int)sizeof( SNPPacketSegmentType ) > cbChunk )
//		{
//			break;
//		}
//
//		const SNPPacketSegmentType *pSegment = (const SNPPacketSegmentType * )( (const uint8 *)pChunk + nPos );
//		uint8 unFlags = pSegment->m_unFlags;
//		int nSegmentSize = LittleWord( pSegment->m_unSize );
//
//		if ( ( unFlags & kPacketSegmentFlags_Feedback ) == 0 )
//		{
//			bIsDataPacket = true;
//			break;
//		}
//
//		nPos += sizeof( SNPPacketSegmentType ) + nSegmentSize;
//	}
//
//	// Check for loss
//	int nLossRes = SNP_CheckForLoss( unSeqNum, usecNow );
//
//	if ( nLossRes == -1  ) // Packet so stale we should just drop it.
//	{
//		return true;
//	}
//
//	SSNPReceiverState::ETFRCFeedbackType do_feedback = SSNPReceiverState::TFRC_FBACK_NONE;
//
//	if ( nLossRes == 1 && bIsDataPacket )
//	{
//		do_feedback = SSNPReceiverState::TFRC_FBACK_PARAM_CHANGE;
//	}
//
//	if ( m_receiverState.m_e_rx_state == SSNPReceiverState::TFRC_RSTATE_NO_DATA )
//	{
//		if ( bIsDataPacket ) 
//		{
//			do_feedback = SSNPReceiverState::TFRC_FBACK_INITIAL;
//			m_receiverState.m_e_rx_state = SSNPReceiverState::TFRC_RSTATE_DATA;
//			m_receiverState.m_n_rx_s = cbPacketSize;
//		}
//	}
//	else if ( bIsDataPacket )
//	{
//		// Update recv counts
//		m_receiverState.m_n_rx_s = tfrc_ewma( m_receiverState.m_n_rx_s, cbPacketSize, 9 );
//		m_receiverState.m_n_bytes_recv += cbPacketSize;
//	}
//
//	if ( bIsDataPacket && SNP_UpdateIMean( unSeqNum, usecNow ) )
//	{
//		do_feedback = SSNPReceiverState::TFRC_FBACK_PARAM_CHANGE;
//	}
//
//	nPos = sizeof( SNPPacketHdr );
//
//	SteamNetworkingMicroseconds usecPing = GetUsecPingWithFallback( this );
//	while ( nPos < cbChunk )
//	{
//		if ( nPos + (int)sizeof( SNPPacketSegmentType ) > cbChunk )
//		{
//			break;
//		}
//
//		const SNPPacketSegmentType *pSegment = (const SNPPacketSegmentType * )( (const uint8 *)pChunk + nPos );
//		uint8 unFlags = pSegment->m_unFlags;
//		int nSegmentSize = LittleWord( pSegment->m_unSize );
//
//		nPos += sizeof( SNPPacketSegmentType );
//
//		if ( unFlags & kPacketSegmentFlags_Feedback )
//		{
//			const SNPPacketSegmentFeedback *pFeedback = ( const SNPPacketSegmentFeedback * )( (const uint8 *)pChunk + nPos );
//
//			if ( steamdatagram_snp_log_feedback )
//			SpewMsg( "%12llu %s: RECV FEEDBACK %d x_recv:%d t_delay:%d p:%d\n",
//						 usecNow,
//						 m_sName.c_str(),
//						 unSeqNum,
//						 LittleDWord( pFeedback->m_un_X_recv ),
//						 LittleDWord( pFeedback->m_un_t_delay ),
//						 LittleDWord( pFeedback->m_un_p ) );
//
//			m_senderState.m_un_p = LittleDWord( pFeedback->m_un_p );
//
//			Assert( sizeof( SNPPacketSegmentFeedback ) == nSegmentSize );
//
//			nPos += sizeof( SNPPacketSegmentFeedback );
//
//			// Purge any history before the ack in this packet since its old news
//			while ( !m_senderState.m_tx_hist.empty() && 
//				   IsSeqAfter( m_senderState.m_unRecvSeqNum, m_senderState.m_tx_hist.front().m_unSeqNum ) )
//			{
//				m_senderState.m_tx_hist.pop_front();
//			}
//
//			// Find the sender packet in the tx_history
//			for ( SSNPSenderState::S_tx_hist_entry &tx_hist_entry: m_senderState.m_tx_hist )
//			{
//				if ( tx_hist_entry.m_unSeqNum == m_senderState.m_unRecvSeqNum )
//				{
//					SteamNetworkingMicroseconds usecElapsed = ( usecNow - tx_hist_entry.m_usec_ts );
//					uint32 usecDelay = LittleDWord( pFeedback->m_un_t_delay );
//					SteamNetworkingMicroseconds usecPingCalc = usecElapsed - usecDelay;
//					if ( usecPing < -1000 )
//					{
//						SpewWarning( "Ignoring weird ack delay of %uusec, we sent that packet only %lldusec ago!\n", usecDelay, usecElapsed );
//					}
//					else
//					{
//						usecPing = Max( usecPingCalc, (SteamNetworkingMicroseconds)1 );
//						m_statsEndToEnd.m_ping.ReceivedPing( usecPing/1000, usecNow );
//					}
//
//					// found it update rtt
//					if ( steamdatagram_snp_log_rtt )
//					SpewMsg( "%12llu %s: RECV UPDATE RTT rtt:%dms seqNum:%d ts:%d r_sample:%llu diff_ts:%llu t_delay:%d\n",
//								 usecNow,
//								 m_sName.c_str(),
//								 m_statsEndToEnd.m_ping.m_nSmoothedPing,
//								 m_senderState.m_unRecvSeqNum,
//								 (int)( tx_hist_entry.m_usec_ts / 1000 ),
//								 usecPing,
//								 usecElapsed,
//								 usecDelay );
//
//					break;
//				}
//			}
//
//			m_senderState.m_n_x_recv = LittleDWord( pFeedback->m_un_X_recv );
//
//			// Update allowed sending rate X as per draft rfc3448bis-00, 4.2/3
//			bool bUpdateX = true;
//			if ( m_senderState.m_e_tx_state == SSNPSenderState::TFRC_SSTATE_NO_FBACK )
//			{
//				m_senderState.m_e_tx_state = SSNPSenderState::TFRC_SSTATE_FBACK;
//
//				if ( m_senderState.m_usec_rto == 0 )
//				{
//					// Initial feedback packet: Larger Initial Windows (4.2)
//					m_senderState.m_n_x = GetInitialRate( usecPing );
//					m_senderState.m_usec_ld = usecNow;
//					bUpdateX = false;
//				}
//				else if ( m_senderState.m_un_p == 0 )
//				{
//					// First feedback after nofeedback timer expiry (4.3)
//					bUpdateX = false;
//				}
//			}
//
//			if ( m_senderState.m_un_p )
//			{
//				m_senderState.m_n_x_calc = TFRCCalcX( m_senderState.m_n_tx_s, 
//													  usecPing,
//													  (float)m_senderState.m_un_p / (float)UINT_MAX );
//			}
//
//			if ( bUpdateX )
//			{
//				SNP_UpdateX( usecNow );
//			}
//
//			// As we have calculated new ipi, delta, t_nom it is possible
//			// that we now can send a packet, so wake up the thinker
//			m_senderState.m_usec_rto = MAX( 4 * usecPing, TCP_RTO_MIN );
//			m_senderState.SetNoFeedbackTimer( usecNow );
//			m_senderState.m_bSentPacketSinceNFB = false;
//		}
//		else
//		{
//			// must be message type reliable/unreliable
//			bool bIsReliable = ( unFlags & kPacketSegmentFlags_Reliable ) != 0;
//			bool bIsEnd = ( unFlags & kPacketSegmentFlags_End ) != 0;
//
//			const SNPPacketSegmentMessage *pSegmentMessage = ( const SNPPacketSegmentMessage * )( (const uint8 *)pChunk + nPos );
//
//			nPos += sizeof( SNPPacketSegmentMessage );
//
//			uint16 unMsgNum = LittleWord( pSegmentMessage->m_unMsgNum );
//			int unOffset = LittleDWord( pSegmentMessage->m_unOffset );
//			int nMsgSize = nSegmentSize - sizeof( SNPPacketSegmentMessage );
//
//			int msgPos = nPos;
//			nPos += nMsgSize;
//
//			if ( bIsReliable )
//			{
//				CUtlBuffer &recvBuf = m_receiverState.m_recvBufReliable;
//
//				// If this isn't the message we are expecting throw it away
//				if ( m_receiverState.m_unRecvMsgNumReliable != unMsgNum )
//				{
//					if ( steamdatagram_snp_log_segments )
//					SpewMsg( "%12llu %s: Unexpected reliable message segment %d:%u sz=%d (expected %d)\n", 
//								 usecNow,
//								 m_sName.c_str(),
//								 unMsgNum, 
//								 unOffset, 
//								 nSegmentSize,
//								 m_receiverState.m_unRecvMsgNumReliable );
//					continue;
//				}
//
//				if ( recvBuf.TellPut() != unOffset )
//				{
//					if ( steamdatagram_snp_log_segments )
//					SpewMsg( "%12llu %s: Unexpected reliable message offset %d:%u sz=%d (expected %d:%u)\n", 
//								 usecNow,
//								 m_sName.c_str(),
//								 unMsgNum, 
//								 unOffset, 
//								 nSegmentSize,
//								 m_receiverState.m_unRecvMsgNumReliable,
//								 recvBuf.TellPut() );
//					continue;
//				}
//
//				recvBuf.Put( (const uint8 *)pChunk + msgPos, nMsgSize );
//
//				m_receiverState.m_unLastReliableRecvMsgNum = unMsgNum;
//				m_receiverState.m_unLastReliabeRecvMsgAmt = recvBuf.TellPut();
//
//				if ( steamdatagram_snp_log_segments )
//				SpewMsg( "%12llu %s: RELIABLE    %d: msgNum %d offset=%d recvAmt=%d segmentSize=%d%s\n", 
//							 usecNow,
//							 m_sName.c_str(), 
//							 unSeqNum,
//							 unMsgNum,
//							 unOffset,
//							 recvBuf.TellPut(),
//							 nSegmentSize,
//							 bIsEnd ? " (end)" : "" );
//
//				// Are we done?
//				if ( bIsEnd )
//				{
//					ReceivedMessage( recvBuf.Base(), recvBuf.TellPut(), usecNow );
//
//					if ( steamdatagram_snp_log_message || steamdatagram_snp_log_reliable )
//					SpewMsg( "%12llu %s: RecvMessage RELIABLE: MsgNum=%d sz=%d\n",
//								 usecNow,
//								 m_sName.c_str(),
//								 unMsgNum,
//								 recvBuf.TellPut() );
//
//					// Clear and ready for the next message!
//					recvBuf.Clear();
//					++m_receiverState.m_unRecvMsgNumReliable;
//
//					// Update counter
//					++m_receiverState.m_nMessagesRecvReliable;
//				}
//			}
//			else
//			{
//				CUtlBuffer &recvBuf = m_receiverState.m_recvBuf;
//
//				// If this isn't the message we are expecting throw it away
//				if ( m_receiverState.m_unRecvMsgNum != unMsgNum )
//				{
//					if ( steamdatagram_snp_log_segments )
//					SpewMsg( "%12llu %s: Throwing away unreliable message %d sz=%d\n", 
//								 usecNow,
//								 m_sName.c_str(),
//								 m_receiverState.m_unRecvMsgNum, 
//								 recvBuf.TellPut() );
//					recvBuf.Purge();
//				}
//
//				if ( recvBuf.TellPut() != unOffset )
//				{
//					recvBuf.Purge();
//
//					// If its zero its a new message, just go with it
//					if ( unOffset != 0 )
//					{
//						if ( steamdatagram_snp_log_segments )
//						SpewMsg( "%12llu %s: Unexpected reliable message offset %d:%u sz=%d\n", 
//									 usecNow,
//									 m_sName.c_str(),
//									 unMsgNum, 
//									 unOffset, 
//									 nSegmentSize );
//						continue;
//					}
//				}
//
//				m_receiverState.m_unRecvMsgNum = unMsgNum;
//				recvBuf.Put( (const uint8 *)pChunk + msgPos, nMsgSize );
//
//				if ( steamdatagram_snp_log_segments )
//				SpewMsg( "%12llu %s: UNRELIABLE  %d: msgNum %d offset=%d recvAmt=%d segmentSize=%d%s\n", 
//							 usecNow,
//							 m_sName.c_str(), 
//							 unSeqNum,
//							 unMsgNum,
//							 unOffset,
//							 recvBuf.TellPut(),
//							 nSegmentSize,
//							 bIsEnd ? " (end)" : "" );
//
//				// Are we done?
//				if ( bIsEnd )
//				{
//					ReceivedMessage( recvBuf.Base(), recvBuf.TellPut(), usecNow );
//
//					if ( steamdatagram_snp_log_message )
//					SpewMsg( "%12llu %s: RecvMessage UNRELIABLE: MsgNum=%d sz=%d\n",
//								 usecNow,
//								 m_sName.c_str(),
//								 unMsgNum,
//								 recvBuf.TellPut() );
//
//					// Clear and ready for the next message!
//					recvBuf.Clear();
//					++m_receiverState.m_unRecvMsgNum;
//
//					// Update counter
//					++m_receiverState.m_nMessagesRecvUnreliable;
//				}
//				else
//				{
//					if ( steamdatagram_snp_log_segments )
//					SpewMsg( "%12llu %s: MSG recieved unreliable message %d section offset %u (sz=%d)\n",
//								 usecNow,
//								 m_sName.c_str(),
//								 unMsgNum,
//								 unOffset,
//								 nSegmentSize );
//				}
//			}
//		}
//	}
//
//	if ( bIsDataPacket &&
//		do_feedback == SSNPReceiverState::TFRC_FBACK_NONE &&
//		m_senderState.m_sendFeedbackState == SSNPSenderState::TFRC_SSTATE_FBACK_NONE &&
//		m_receiverState.m_usec_next_feedback &&
//		m_receiverState.m_usec_next_feedback <= usecNow )
//	{
//		do_feedback = SSNPReceiverState::TFRC_FBACK_PERIODIC;
//	}
//
//	switch ( do_feedback )
//	{
//	case SSNPReceiverState::TFRC_FBACK_NONE :
//		break; // No work
//	case SSNPReceiverState::TFRC_FBACK_INITIAL :
//		m_receiverState.m_n_x_recv = 0;
//		m_receiverState.m_li_i_mean = 0;
//		// FALL THROUGH
//
//	case SSNPReceiverState::TFRC_FBACK_PARAM_CHANGE :
//		m_senderState.m_sendFeedbackState = SSNPSenderState::TFRC_SSTATE_FBACK_REQ; // Send feedback immediately
//		break;
//	case SSNPReceiverState::TFRC_FBACK_PERIODIC :
//		m_senderState.m_sendFeedbackState = SSNPSenderState::TFRC_SSTATE_FBACK_PERODIC; // Send feedback on next data packet
//		break;
//	}
//	
//	SNP_RecordPacket( unSeqNum, usecPing, usecNow );
//
//	// Check for retransmit
//	SNP_CheckForReliable( usecNow );
//
//	return true;
//}
