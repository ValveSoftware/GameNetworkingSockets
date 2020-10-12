//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_snp.h"
#include "steamnetworkingsockets_connections.h"
#include "crypto.h"

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

		// Total size of ack data up to this point:
		// header, all previous blocks, and this block
		int16 m_cbTotalEncodedSize;
	};

	enum { k_cbHeaderSize = 5 };
	enum { k_nMaxBlocks = 64 };
	int m_nBlocks;
	int m_nBlocksNeedToAck; // Number of blocks we really need to send now.
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


void SSNPSenderState::Shutdown()
{
	m_unackedReliableMessages.PurgeMessages();
	m_messagesQueued.PurgeMessages();
	m_mapInFlightPacketsByPktNum.clear();
	m_listInFlightReliableRange.clear();
	m_cbPendingUnreliable = 0;
	m_cbPendingReliable = 0;
	m_cbSentUnackedReliable = 0;
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
		CSteamNetworkingMessage *pMsg = m_unackedReliableMessages.m_pFirst;
		Assert( pMsg->SNPSend_ReliableStreamPos() > 0 );
		int64 nReliableEnd = pMsg->SNPSend_ReliableStreamPos() + pMsg->m_cbSize;

		// Are we backing a range that is in flight (and thus we might need
		// to resend?)
		if ( !m_listInFlightReliableRange.empty() )
		{
			auto head = m_listInFlightReliableRange.begin();
			Assert( head->first.m_nBegin >= pMsg->SNPSend_ReliableStreamPos() );
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
			Assert( head->first.m_nBegin >= pMsg->SNPSend_ReliableStreamPos() );
			if ( head->second == pMsg )
			{
				Assert( head->first.m_nBegin < nReliableEnd );
				return;
			}
			Assert( head->first.m_nBegin >= nReliableEnd );
		}

		// We're all done!
		DbgVerify( m_unackedReliableMessages.pop_front() == pMsg );
		pMsg->Release();
	}
}

//-----------------------------------------------------------------------------
SSNPSenderState::SSNPSenderState()
{
	// Setup the table of inflight packets with a sentinel.
	m_mapInFlightPacketsByPktNum.clear();
	SNPInFlightPacket_t &sentinel = m_mapInFlightPacketsByPktNum[INT64_MIN];
	sentinel.m_bNack = false;
	sentinel.m_pTransport = nullptr;
	sentinel.m_usecWhenSent = 0;
	m_itNextInFlightPacketToTimeout = m_mapInFlightPacketsByPktNum.end();
	DebugCheckInFlightPacketMap();
}

#if STEAMNETWORKINGSOCKETS_SNP_PARANOIA > 0
void SSNPSenderState::DebugCheckInFlightPacketMap() const
{
	Assert( !m_mapInFlightPacketsByPktNum.empty() );
	bool bFoundNextToTimeout = false;
	auto it = m_mapInFlightPacketsByPktNum.begin();
	Assert( it->first == INT64_MIN );
	Assert( m_itNextInFlightPacketToTimeout != it );
	int64 prevPktNum = it->first;
	SteamNetworkingMicroseconds prevWhenSent = it->second.m_usecWhenSent;
	while ( ++it != m_mapInFlightPacketsByPktNum.end() )
	{
		Assert( prevPktNum < it->first );
		Assert( prevWhenSent <= it->second.m_usecWhenSent );
		if ( it == m_itNextInFlightPacketToTimeout )
		{
			Assert( !bFoundNextToTimeout );
			bFoundNextToTimeout = true;
		}
		prevPktNum = it->first;
		prevWhenSent = it->second.m_usecWhenSent;
	}
	if ( !bFoundNextToTimeout )
	{
		Assert( m_itNextInFlightPacketToTimeout == m_mapInFlightPacketsByPktNum.end() );
	}
}
#endif

//-----------------------------------------------------------------------------
SSNPReceiverState::SSNPReceiverState()
{
	// Init packet gaps with a sentinel
	SSNPPacketGap &sentinel = m_mapPacketGaps[INT64_MAX];
	sentinel.m_nEnd = INT64_MAX; // Fixed value
	sentinel.m_usecWhenOKToNack = INT64_MAX; // Fixed value, for when there is nothing left to nack
	sentinel.m_usecWhenAckPrior = INT64_MAX; // Time when we need to flush a report on all lower-numbered packets

	// Point at the sentinel
	m_itPendingAck = m_mapPacketGaps.end();
	--m_itPendingAck;
	m_itPendingNack = m_itPendingAck;
}

//-----------------------------------------------------------------------------
void SSNPReceiverState::Shutdown()
{
	m_mapUnreliableSegments.clear();
	m_bufReliableStream.clear();
	m_mapReliableStreamGaps.clear();
	m_mapPacketGaps.clear();
}

//-----------------------------------------------------------------------------
void CSteamNetworkConnectionBase::SNP_InitializeConnection( SteamNetworkingMicroseconds usecNow )
{
	m_senderState.TokenBucket_Init( usecNow );

	SteamNetworkingMicroseconds usecPing = GetUsecPingWithFallback( this );

	/*
	* Compute the initial sending rate X_init in the manner of RFC 3390:
	*
	*	X_init  =  min(4 * s, max(2 * s, 4380 bytes)) / RTT
	*
	* Note that RFC 3390 uses MSS, RFC 4342 refers to RFC 3390, and rfc3448bis
	* (rev-02) clarifies the use of RFC 3390 with regard to the above formula.
	*/
	Assert( usecPing > 0 );
	int64 w_init = Clamp( 4380, 2 * k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend, 4 * k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend );
	m_senderState.m_n_x = int( k_nMillion * w_init / usecPing );

	// Go ahead and clamp it now
	SNP_ClampSendRate();
}

//-----------------------------------------------------------------------------
void CSteamNetworkConnectionBase::SNP_ShutdownConnection()
{
	m_senderState.Shutdown();
	m_receiverState.Shutdown();
}

//-----------------------------------------------------------------------------
int64 CSteamNetworkConnectionBase::SNP_SendMessage( CSteamNetworkingMessage *pSendMessage, SteamNetworkingMicroseconds usecNow, bool *pbThinkImmediately )
{
	int cbData = (int)pSendMessage->m_cbSize;

	// Assume we won't want to wake up immediately
	if ( pbThinkImmediately )
		*pbThinkImmediately = false;

	// Check if we're full
	if ( m_senderState.PendingBytesTotal() + cbData > m_connectionConfig.m_SendBufferSize.Get() )
	{
		SpewWarningRateLimited( usecNow, "Connection already has %u bytes pending, cannot queue any more messages\n", m_senderState.PendingBytesTotal() );
		pSendMessage->Release();
		return -k_EResultLimitExceeded; 
	}

	// Check if they try to send a really large message
	if ( cbData > k_cbMaxUnreliableMsgSizeSend && !( pSendMessage->m_nFlags & k_nSteamNetworkingSend_Reliable )  )
	{
		SpewWarningRateLimited( usecNow, "Trying to send a very large (%d bytes) unreliable message.  Sending as reliable instead.\n", cbData );
		pSendMessage->m_nFlags |= k_nSteamNetworkingSend_Reliable;
	}

	if ( pSendMessage->m_nFlags & k_nSteamNetworkingSend_NoDelay )
	{
		// FIXME - need to check how much data is currently pending, and return
		// k_EResultIgnored if we think it's going to be a while before this
		// packet goes on the wire.
	}

	// First, accumulate tokens, and also limit to reasonable burst
	// if we weren't already waiting to send
	SNP_ClampSendRate();
	SNP_TokenBucket_Accumulate( usecNow );

	// Assign a message number
	pSendMessage->m_nMessageNumber = ++m_senderState.m_nLastSentMsgNum;

	// Reliable, or unreliable?
	if ( pSendMessage->m_nFlags & k_nSteamNetworkingSend_Reliable )
	{
		pSendMessage->SNPSend_SetReliableStreamPos( m_senderState.m_nReliableStreamPos );

		// Generate the header
		byte *hdr = pSendMessage->SNPSend_ReliableHeader();
		hdr[0] = 0;
		byte *hdrEnd = hdr+1;
		int64 nMsgNumGap = pSendMessage->m_nMessageNumber - m_senderState.m_nLastSendMsgNumReliable;
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
		pSendMessage->m_cbSNPSendReliableHeader = hdrEnd - hdr;

		// Grow the total size of the message by the header
		pSendMessage->m_cbSize += pSendMessage->m_cbSNPSendReliableHeader;

		// Advance stream pointer
		m_senderState.m_nReliableStreamPos += pSendMessage->m_cbSize;

		// Update stats
		++m_senderState.m_nMessagesSentReliable;
		m_senderState.m_cbPendingReliable += pSendMessage->m_cbSize;

		// Remember last sent reliable message number, so we can know how to
		// encode the next one
		m_senderState.m_nLastSendMsgNumReliable = pSendMessage->m_nMessageNumber;

		Assert( pSendMessage->SNPSend_IsReliable() );
	}
	else
	{
		pSendMessage->SNPSend_SetReliableStreamPos( 0 );
		pSendMessage->m_cbSNPSendReliableHeader = 0;

		++m_senderState.m_nMessagesSentUnreliable;
		m_senderState.m_cbPendingUnreliable += pSendMessage->m_cbSize;

		Assert( !pSendMessage->SNPSend_IsReliable() );
	}

	// Add to pending list
	m_senderState.m_messagesQueued.push_back( pSendMessage );
	SpewVerboseGroup( m_connectionConfig.m_LogLevel_Message.Get(), "[%s] SendMessage %s: MsgNum=%lld sz=%d\n",
				 GetDescription(),
				 pSendMessage->SNPSend_IsReliable() ? "RELIABLE" : "UNRELIABLE",
				 (long long)pSendMessage->m_nMessageNumber,
				 pSendMessage->m_cbSize );

	// Use Nagle?
	// We always set the Nagle timer, even if we immediately clear it.  This makes our clearing code simpler,
	// since we can always safely assume that once we find a message with the nagle timer cleared, all messages
	// queued earlier than this also have it cleared.
	// FIXME - Don't think this works if the configuration value is changing.  Since changing the
	// config value could violate the assumption that nagle times are increasing.  Probably not worth
	// fixing.
	pSendMessage->SNPSend_SetUsecNagle( usecNow + m_connectionConfig.m_NagleTime.Get() );
	if ( pSendMessage->m_nFlags & k_nSteamNetworkingSend_NoNagle )
		m_senderState.ClearNagleTimers();

	// Save the message number.  The code below might end up deleting the message we just queued
	int64 result = pSendMessage->m_nMessageNumber;

	// Schedule wakeup at the appropriate time.  (E.g. right now, if we're ready to send, 
	// or at the Nagle time, if Nagle is active.)
	//
	// NOTE: Right now we might not actually be capable of sending end to end data.
	// But that case is relatievly rare, and nothing will break if we try to right now.
	// On the other hand, just asking the question involved a virtual function call,
	// and it will return success most of the time, so let's not make the check here.
	if ( GetState() == k_ESteamNetworkingConnectionState_Connected )
	{
		SteamNetworkingMicroseconds usecNextThink = SNP_GetNextThinkTime( usecNow );

		// Ready to send now?
		if ( usecNextThink > usecNow )
		{

			// We are rate limiting.  Spew about it?
			if ( m_senderState.m_messagesQueued.m_pFirst->SNPSend_UsecNagle() == 0 )
			{
				SpewVerbose( "[%s] RATELIM QueueTime is %.1fms, SendRate=%.1fk, BytesQueued=%d\n", 
					GetDescription(),
					m_senderState.CalcTimeUntilNextSend() * 1e-3,
					m_senderState.m_n_x * ( 1.0/1024.0),
					m_senderState.PendingBytesTotal()
				);
			}

			// Set a wakeup call.
			EnsureMinThinkTime( usecNextThink );
		}
		else
		{

			// We're ready to send right now.  Check if we should!
			if ( pSendMessage->m_nFlags & k_nSteamNetworkingSend_UseCurrentThread )
			{

				// We should send in this thread, before the API entry point
				// that the app used returns.  Is the caller gonna handle this?
				if ( pbThinkImmediately )
				{
					// Caller says they will handle it
					*pbThinkImmediately = true;
				}
				else
				{
					// Caller wants us to just do it here.
					CheckConnectionStateAndSetNextThinkTime( usecNow );
				}
			}
			else
			{
				// Wake up the service thread ASAP to send this in the background thread
				SetNextThinkTimeASAP();
			}
		}
	}

	return result;
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
	if ( m_senderState.m_messagesQueued.m_pLast->SNPSend_UsecNagle() == 0 )
		return k_EResultOK;

	// Accumulate tokens, and also limit to reasonable burst
	// if we weren't already waiting to send before this.
	// (Clearing the Nagle timers might very well make us want to
	// send so we want to do this first.)
	SNP_ClampSendRate();
	SNP_TokenBucket_Accumulate( usecNow );

	// Clear all Nagle timers
	m_senderState.ClearNagleTimers();

	// Schedule wakeup at the appropriate time.  (E.g. right now, if we're ready to send.)
	SteamNetworkingMicroseconds usecNextThink = SNP_GetNextThinkTime( usecNow );
	EnsureMinThinkTime( usecNextThink );
	return k_EResultOK;
}

bool CSteamNetworkConnectionBase::ProcessPlainTextDataChunk( int usecTimeSinceLast, RecvPacketContext_t &ctx )
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
		do { EXPECT_BYTES(8,pszWhatFor); var = LittleQWord(*(uint64 *)pDecode); pDecode += 8; } while(false)

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
	Assert( BStateIsActive() );

	const SteamNetworkingMicroseconds usecNow = ctx.m_usecNow;
	const int64 nPktNum = ctx.m_nPktNum;
	bool bInhibitMarkReceived = false;

	const int nLogLevelPacketDecode = m_connectionConfig.m_LogLevel_PacketDecode.Get();
	SpewVerboseGroup( nLogLevelPacketDecode, "[%s] decode pkt %lld\n", GetDescription(), (long long)nPktNum );

	// Decode frames until we get to the end of the payload
	const byte *pDecode = (const byte *)ctx.m_pPlainText;
	const byte *pEnd = pDecode + ctx.m_cbPlainText;
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

			// Check if offset+size indicates a message larger than what we support.  (Also,
			// protect against malicious sender sending *extremely* large offset causing overflow.)
			if ( (int64)nOffset + cbSegmentSize > k_cbMaxUnreliableMsgSizeRecv || cbSegmentSize > k_cbMaxUnreliableSegmentSizeRecv )
			{

				// Since this is unreliable data, we can just ignore the segment.
				SpewWarningRateLimited( usecNow, "[%s] Ignoring unreliable segment with invalid offset %u size %d\n",
					GetDescription(), nOffset, cbSegmentSize );
			}
			else
			{

				// Receive the segment
				bool bLastSegmentInMessage = ( nFrameType & 0x20 ) != 0;
				SNP_ReceiveUnreliableSegment( nCurMsgNum, nOffset, pSegmentData, cbSegmentSize, bLastSegmentInMessage, usecNow );
			}
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

			// Ingest the segment.
			if ( !SNP_ReceiveReliableSegment( nPktNum, nDecodeReliablePos, pSegmentData, cbSegmentSize, usecNow ) )
			{
				if ( !BStateIsActive() )
					return false; // we decided to nuke the connection - abort packet processing

				// We're not able to ingest this reliable segment at the moment,
				// but we didn't terminate the connection.  So do not ack this packet
				// to the peer.  We need them to retransmit
				bInhibitMarkReceived = true;
			}

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
			SpewDebugGroup( nLogLevelPacketDecode, "[%s]   decode pkt %lld stop waiting: %lld (was %lld)",
				GetDescription(),
				(long long)nPktNum,
				(long long)nMinPktNumToSendAcks, (long long)m_receiverState.m_nMinPktNumToSendAcks );
			m_receiverState.m_nMinPktNumToSendAcks = nMinPktNumToSendAcks;
			m_receiverState.m_nPktNumUpdatedMinPktNumToSendAcks = nPktNum;

			// Trim from the front of the packet gap list,
			// we can stop reporting these losses to the sender
			auto h = m_receiverState.m_mapPacketGaps.begin();
			while ( h->first <= m_receiverState.m_nMinPktNumToSendAcks )
			{
				if ( h->second.m_nEnd > m_receiverState.m_nMinPktNumToSendAcks )
				{
					// Ug.  You're not supposed to modify the key in a map.
					// I suppose that's legit, since you could violate the ordering.
					// but in this case I know that this change is OK.
					const_cast<int64 &>( h->first ) = m_receiverState.m_nMinPktNumToSendAcks;
					break;
				}

				// Were we pending an ack on this?
				if ( m_receiverState.m_itPendingAck == h )
					++m_receiverState.m_itPendingAck;

				// Were we pending a nack on this?
				if ( m_receiverState.m_itPendingNack == h )
				{
					// I am not sure this is even possible.
					AssertMsg( false, "Expiring packet gap, which had pending NACK" );

					// But just in case, this would be the proper action
					++m_receiverState.m_itPendingNack;
				}

				// Packet loss is in the past.  Forget about it and move on
				h = m_receiverState.m_mapPacketGaps.erase(h);
			}
		}
		else if ( ( nFrameType & 0xf0 ) == 0x90 )
		{

			//
			// Ack
			//

			#if STEAMNETWORKINGSOCKETS_SNP_PARANOIA > 0
				m_senderState.DebugCheckInFlightPacketMap();
				#if STEAMNETWORKINGSOCKETS_SNP_PARANOIA == 1
				if ( ( nPktNum & 255 ) == 0 ) // only do it periodically
				#endif
				{
					m_senderState.DebugCheckInFlightPacketMap();
				}
			#endif

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

			SpewDebugGroup( nLogLevelPacketDecode, "[%s]   decode pkt %lld latest recv %lld\n",
				GetDescription(),
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
				if ( nPackedDelay != 0xffff && inFlightPkt->first == nLatestRecvSeqNum && inFlightPkt->second.m_pTransport == ctx.m_pTransport )
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
					if ( msPing < -1 || msPing > 2000 )
					{
						// Either they are lying or some weird timer stuff is happening.
						// Either way, discard it.

						SpewMsgGroup( m_connectionConfig.m_LogLevel_AckRTT.Get(), "[%s] decode pkt %lld latest recv %lld delay %lluusec INVALID ping %lldusec\n",
							GetDescription(),
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
						ProcessSNPPing( msPing, ctx );

						// Spew
						SpewVerboseGroup( m_connectionConfig.m_LogLevel_AckRTT.Get(), "[%s] decode pkt %lld latest recv %lld delay %.1fms elapsed %.1fms ping %dms\n",
							GetDescription(),
							(long long)nPktNum, (long long)nLatestRecvSeqNum,
							(float)(usecDelay * 1e-3 ),
							(float)(usecElapsed * 1e-3 ),
							msPing
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
			// and move forward.
			if ( nBlocks > 0 )
			{
				// Decrease flush delay the more blocks they send us.
				// FIXME - This is not an optimal way to do this.  Forcing us to
				// ack everything is not what we want to do.  Instead, we should
				// use a separate timer for when we need to flush out a stop_waiting
				// packet!
				SteamNetworkingMicroseconds usecDelay = 250*1000 / nBlocks;
				QueueFlushAllAcks( usecNow + usecDelay );
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
					SpewDebugGroup( nLogLevelPacketDecode, "[%s]   decode pkt %lld ack last block ack begin %lld\n",
						GetDescription(),
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

					SpewDebugGroup( nLogLevelPacketDecode, "[%s]   decode pkt %lld nack [%lld,%lld) ack [%lld,%lld)\n",
						GetDescription(),
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
					m_senderState.MaybeCheckInFlightPacketMap();
				}

				// Ack of in-flight end-to-end stats?
				if ( nPktNumAckBegin <= m_statsEndToEnd.m_pktNumInFlight && m_statsEndToEnd.m_pktNumInFlight < nPktNumAckEnd )
					m_statsEndToEnd.InFlightPktAck( usecNow );

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
				if ( nLogLevelPacketDecode >= k_ESteamNetworkingSocketsDebugOutputType_Debug )
				{

					int64 nPeerReliablePos = m_senderState.m_nReliableStreamPos;
					if ( !m_senderState.m_listInFlightReliableRange.empty() )
						nPeerReliablePos = std::min( nPeerReliablePos, m_senderState.m_listInFlightReliableRange.begin()->first.m_nBegin );
					if ( !m_senderState.m_listReadyRetryReliableRange.empty() )
						nPeerReliablePos = std::min( nPeerReliablePos, m_senderState.m_listReadyRetryReliableRange.begin()->first.m_nBegin );

					SpewDebugGroup( nLogLevelPacketDecode, "[%s]   decode pkt %lld peer reliable pos = %lld\n",
						GetDescription(),
						(long long)nPktNum, (long long)nPeerReliablePos );
				}
			}

			// Check if any of this was new info, then advance our stop_waiting value.
			if ( nLatestRecvSeqNum > m_senderState.m_nMinPktWaitingOnAck )
			{
				SpewVerboseGroup( nLogLevelPacketDecode, "[%s]   updating min_waiting_on_ack %lld -> %lld\n",
					GetDescription(),
					(long long)m_senderState.m_nMinPktWaitingOnAck, (long long)nLatestRecvSeqNum );
				m_senderState.m_nMinPktWaitingOnAck = nLatestRecvSeqNum;
			}
		}
		else
		{
			DECODE_ERROR( "Invalid SNP frame lead byte 0x%02x", nFrameType );
		}
	}

	// Should we record that we received it?
	if ( bInhibitMarkReceived )
	{
		// Something really odd.  High packet loss / fragmentation.
		// Potentially the peer is being abusive and we need
		// to protect ourselves.
		//
		// Act as if the packet was dropped.  This will cause the
		// peer's sender logic to interpret this as additional packet
		// loss and back off.  That's a feature, not a bug.
	}
	else
	{

		// Update structures needed to populate our ACKs.
		// If we received reliable data now, then schedule an ack
		bool bScheduleAck = nDecodeReliablePos > 0;
		SNP_RecordReceivedPktNum( nPktNum, usecNow, bScheduleAck );
	}

	// Track end-to-end flow.  Even if we decided to tell our peer that
	// we did not receive this, we want our own stats to reflect
	// that we did.  (And we want to be able to quickly reject a
	// packet with this same number.)
	//
	// Also, note that order of operations is important.  This call must
	// happen after the SNP_RecordReceivedPktNum call above
	m_statsEndToEnd.TrackProcessSequencedPacket( nPktNum, usecNow, usecTimeSinceLast );

	// Packet can be processed further
	return true;

	// Make sure these don't get used beyond where we intended them to get used
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

	// Is this in-flight stats we were expecting an ack for?
	if ( m_statsEndToEnd.m_pktNumInFlight == nPktNum )
		m_statsEndToEnd.InFlightPktTimeout();

	// Scan reliable segments
	for ( const SNPRange_t &relRange: pkt.m_vecReliableSegments )
	{

		// Marked as in-flight?
		auto inFlightRange = m_senderState.m_listInFlightReliableRange.find( relRange );
		if ( inFlightRange == m_senderState.m_listInFlightReliableRange.end() )
			continue;

		SpewMsgGroup( m_connectionConfig.m_LogLevel_PacketDecode.Get(), "[%s] pkt %lld %s, queueing retry of reliable range [%lld,%lld)\n", 
			GetDescription(),
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
	m_senderState.MaybeCheckInFlightPacketMap();
	if ( m_senderState.m_mapInFlightPacketsByPktNum.size() <= 1 )
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
	SteamNetworkingMicroseconds usecWhenExpiry = usecNow - usecRTO*2;
	for (;;)
	{
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

	// Make sure we didn't hose data structures
	m_senderState.MaybeCheckInFlightPacketMap();

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
	CSteamNetworkingMessage *m_pMsg;
	int m_cbSegSize;
	int m_nOffset;

	inline void SetupReliable( CSteamNetworkingMessage *pMsg, int64 nBegin, int64 nEnd, int64 nLastReliableStreamPosEnd )
	{
		Assert( nBegin < nEnd );
		//Assert( nBegin + k_cbSteamNetworkingSocketsMaxReliableMessageSegment >= nEnd ); // Max sure we don't exceed max segment size
		Assert( pMsg->SNPSend_IsReliable() );

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
		Assert( nBegin >= pMsg->SNPSend_ReliableStreamPos() );
		Assert( nEnd <= pMsg->SNPSend_ReliableStreamPos() + pMsg->m_cbSize );

		m_pMsg = pMsg;
		m_nOffset = nBegin - pMsg->SNPSend_ReliableStreamPos();
		m_cbSegSize = cbSegData;
	}

	inline void SetupUnreliable( CSteamNetworkingMessage *pMsg, int nOffset, int64 nLastMsgNum )
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
			*(uint32*)pHdr = LittleDWord( (uint32)pMsg->m_nMessageNumber ); pHdr += 4;
			m_hdr[0] |= 0x10;
		}
		else
		{
			// Subsequent unreliable message
			Assert( pMsg->m_nMessageNumber > nLastMsgNum );
			uint64 nDelta = pMsg->m_nMessageNumber - nLastMsgNum;
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
		m_cbSegSize = cbSegData;
		m_nOffset = nOffset;
	}

};

template <typename T, typename L>
inline bool HasOverlappingRange( const SNPRange_t &range, const std_map<SNPRange_t,T,L> &map )
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

bool CSteamNetworkConnectionBase::SNP_SendPacket( CConnectionTransport *pTransport, SendPacketContext_t &ctx )
{
	// Check calling conditions, and don't crash
	if ( !BStateIsActive() || m_senderState.m_mapInFlightPacketsByPktNum.empty() || !pTransport )
	{
		Assert( BStateIsActive() );
		Assert( !m_senderState.m_mapInFlightPacketsByPktNum.empty() );
		Assert( pTransport );
		return false;
	}

	SteamNetworkingMicroseconds usecNow = ctx.m_usecNow;

	// Get max size of plaintext we could send.
	// AES-GCM has a fixed size overhead, for the tag.
	// FIXME - but what we if we aren't using AES-GCM!
	int cbMaxPlaintextPayload = std::max( 0, ctx.m_cbMaxEncryptedPayload-k_cbSteamNetwokingSocketsEncrytionTagSize );
	cbMaxPlaintextPayload = std::min( cbMaxPlaintextPayload, m_cbMaxPlaintextPayloadSend );

	uint8 payload[ k_cbSteamNetworkingSocketsMaxPlaintextPayloadSend ];
	uint8 *pPayloadEnd = payload + cbMaxPlaintextPayload;
	uint8 *pPayloadPtr = payload;

	int nLogLevelPacketDecode = m_connectionConfig.m_LogLevel_PacketDecode.Get();
	SpewVerboseGroup( nLogLevelPacketDecode, "[%s] encode pkt %lld",
		GetDescription(),
		(long long)m_statsEndToEnd.m_nNextSendSequenceNumber );

	// Stop waiting frame
	pPayloadPtr = SNP_SerializeStopWaitingFrame( pPayloadPtr, pPayloadEnd, usecNow );
	if ( pPayloadPtr == nullptr )
		return false;

	// Get list of ack blocks we might want to serialize, and which
	// of those acks we really want to flush out right now.
	SNPAckSerializerHelper ackHelper;
	SNP_GatherAckBlocks( ackHelper, usecNow );

	#ifdef SNP_ENABLE_PACKETSENDLOG
		PacketSendLog *pLog = push_back_get_ptr( m_vecSendLog );
		pLog->m_usecTime = usecNow;
		pLog->m_cbPendingReliable = m_senderState.m_cbPendingReliable;
		pLog->m_cbPendingUnreliable = m_senderState.m_cbPendingUnreliable;
		pLog->m_nPacketGaps = len( m_receiverState.m_mapPacketGaps )-1;
		pLog->m_nAckBlocksNeeded = ackHelper.m_nBlocksNeedToAck;
		pLog->m_nPktNumNextPendingAck = m_receiverState.m_itPendingAck->first;
		pLog->m_usecNextPendingAckTime = m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior;
		pLog->m_fltokens = m_senderState.m_flTokenBucket;
		pLog->m_nMaxPktRecv = m_statsEndToEnd.m_nMaxRecvPktNum;
		pLog->m_nMinPktNumToSendAcks = m_receiverState.m_nMinPktNumToSendAcks;
		pLog->m_nReliableSegmentsRetry = 0;
		pLog->m_nSegmentsSent = 0;
	#endif

	// How much space do we need to reserve for acks?
	int cbReserveForAcks = 0;
	if ( m_statsEndToEnd.m_nMaxRecvPktNum > 0 )
	{
		int cbPayloadRemainingForAcks = pPayloadEnd - pPayloadPtr;
		if ( cbPayloadRemainingForAcks >= SNPAckSerializerHelper::k_cbHeaderSize )
		{
			cbReserveForAcks = SNPAckSerializerHelper::k_cbHeaderSize;
			int n = 3; // Assume we want to send a handful
			n = std::max( n, ackHelper.m_nBlocksNeedToAck ); // But if we have blocks that need to be flushed now, try to fit all of them
			n = std::min( n, ackHelper.m_nBlocks ); // Cannot send more than we actually have
			while ( n > 0 )
			{
				--n;
				if ( ackHelper.m_arBlocks[n].m_cbTotalEncodedSize <= cbPayloadRemainingForAcks )
				{
					cbReserveForAcks = ackHelper.m_arBlocks[n].m_cbTotalEncodedSize;
					break;
				}
			}
		}
	}

	// Check if we are actually going to send data in this packet
	if (
		m_senderState.m_flTokenBucket < 0.0 // No bandwidth available.  (Presumably this is a relatively rare out-of-band connectivity check, etc)  FIXME should we use a different token bucket per transport?
		|| !BStateIsConnectedForWirePurposes() // not actually in a connection stats where we should be sending real data yet
		|| pTransport != m_pTransport // transport is not the selected transport
	) {

		// Serialize some acks, if we want to
		if ( cbReserveForAcks > 0 )
		{
			// But if we're going to send any acks, then try to send as many
			// as possible, not just the bare minimum.
			pPayloadPtr = SNP_SerializeAckBlocks( ackHelper, pPayloadPtr, pPayloadEnd, usecNow );
			if ( pPayloadPtr == nullptr )
				return false; // bug!  Abort

			// We don't need to serialize any more acks
			cbReserveForAcks = 0;
		}

		// Truncate the buffer, don't try to fit any data
		// !SPEED! - instead of doing this, we could just put all of the segment code below
		// in an else() block.
		pPayloadEnd = pPayloadPtr;
	}

	int64 nLastReliableStreamPosEnd = 0;
	int cbBytesRemainingForSegments = pPayloadEnd - pPayloadPtr - cbReserveForAcks;
	vstd::small_vector<EncodedSegment,8> vecSegments;

	// If we need to retry any reliable data, then try to put that in first.
	// Bail if we only have a tiny sliver of data left
	while ( !m_senderState.m_listReadyRetryReliableRange.empty() && cbBytesRemainingForSegments > 2 )
	{
		auto h = m_senderState.m_listReadyRetryReliableRange.begin();

		// Start a reliable segment
		EncodedSegment &seg = *push_back_get_ptr( vecSegments );
		seg.SetupReliable( h->second, h->first.m_nBegin, h->first.m_nEnd, nLastReliableStreamPosEnd );
		int cbSegTotalWithoutSizeField = seg.m_cbHdr + seg.m_cbSegSize;
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
				|| cbMaxPlaintextPayload < m_cbMaxPlaintextPayloadSend
				|| ( cbReserveForAcks > 15 && ackHelper.m_nBlocksNeedToAck > 8 ),
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

		#ifdef SNP_ENABLE_PACKETSENDLOG
			++pLog->m_nReliableSegmentsRetry;
		#endif
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
			CSteamNetworkingMessage *pSendMsg = m_senderState.m_messagesQueued.m_pFirst;
			Assert( m_senderState.m_cbCurrentSendMessageSent < pSendMsg->m_cbSize );

			// Start a new segment
			EncodedSegment &seg = *push_back_get_ptr( vecSegments );

			// Reliable?
			bool bLastSegment = false;
			if ( pSendMsg->SNPSend_IsReliable() )
			{

				// FIXME - Coalesce adjacent reliable messages ranges

				int64 nBegin = pSendMsg->SNPSend_ReliableStreamPos() + m_senderState.m_cbCurrentSendMessageSent;

				// How large would we like this segment to be,
				// ignoring how much space is left in the packet.
				// We limit the size of reliable segments, to make
				// sure that we don't make an excessively large
				// one and then have a hard time retrying it later.
				int cbDesiredSegSize = pSendMsg->m_cbSize - m_senderState.m_cbCurrentSendMessageSent;
				if ( cbDesiredSegSize > m_cbMaxReliableMessageSegment )
				{
					cbDesiredSegSize = m_cbMaxReliableMessageSegment;
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
			if ( bLastSegment || seg.m_cbHdr + seg.m_cbSegSize > cbBytesRemainingForSegments )
			{

				// Check if we have enough room to send anything worthwhile.
				// Don't send really tiny silver segments at the very end of a packet.  That sort of fragmentation
				// just makes it more likely for something to drop.  Our goal is to reduce the number of packets
				// just as much as the total number of bytes, so if we're going to have to send another packet
				// anyway, don't send a little sliver of a message at the beginning of a packet
				// We need to finish the header by this point if we're going to send anything
				int cbMinSegDataSizeToSend = std::min( 16, seg.m_cbSegSize );
				if ( seg.m_cbHdr + cbMinSegDataSizeToSend > cbBytesRemainingForSegments )
				{
					// Don't send this segment now.
					vecSegments.pop_back();
					break;
				}

				#ifdef SNP_ENABLE_PACKETSENDLOG
					++pLog->m_nSegmentsSent;
				#endif

				// Truncate, and leave the message in the queue
				seg.m_cbSegSize = std::min( seg.m_cbSegSize, cbBytesRemainingForSegments - seg.m_cbHdr );
				m_senderState.m_cbCurrentSendMessageSent += seg.m_cbSegSize;
				Assert( m_senderState.m_cbCurrentSendMessageSent < pSendMsg->m_cbSize );
				cbBytesRemainingForSegments -= seg.m_cbHdr + seg.m_cbSegSize;
				break;
			}

			// The whole message fit (perhaps exactly, without the size byte)
			// Reset send pointer for the next message
			Assert( m_senderState.m_cbCurrentSendMessageSent + seg.m_cbSegSize == pSendMsg->m_cbSize );
			m_senderState.m_cbCurrentSendMessageSent = 0;

			// Remove message from queue,w e have transfered ownership to the segment and will
			// dispose of the message when we serialize the segments
			m_senderState.m_messagesQueued.pop_front();

			// Consume payload bytes
			cbBytesRemainingForSegments -= seg.m_cbHdr + seg.m_cbSegSize;

			// Assume for now this won't be the last segment, in which case we will also need the byte for the size field.
			// NOTE: This might cause cbPayloadBytesRemaining to go negative by one!  I know that seems weird, but it actually
			// keeps the logic below simpler.
			cbBytesRemainingForSegments -= 1;

			// Update various accounting, depending on reliable or unreliable
			if ( pSendMsg->SNPSend_IsReliable() )
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
				nLastMsgNum = pSendMsg->m_nMessageNumber;

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
			return false; // bug!  Abort

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
	std::pair<int64,SNPInFlightPacket_t> pairInsert( m_statsEndToEnd.m_nNextSendSequenceNumber, SNPInFlightPacket_t{ usecNow, false, pTransport, {} } );
	SNPInFlightPacket_t &inFlightPkt = pairInsert.second;

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
			int nUpper3Bits = ( seg.m_cbSegSize>>8 );
			Assert( nUpper3Bits <= 4 ); // The values 5 and 6 are reserved and shouldn't be needed due to the MTU we support
			seg.m_hdr[0] |= nUpper3Bits;

			// And the lower 8 bits follow the other fields
			seg.m_hdr[ seg.m_cbHdr++ ] = uint8( seg.m_cbSegSize );
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
		Assert( pPayloadPtr+seg.m_cbSegSize <= pPayloadEnd );

		// Reliable?
		if ( seg.m_pMsg->SNPSend_IsReliable() )
		{
			// We should never encode an empty range of the stream, that is worthless.
			// (Even an empty reliable message requires some framing in the stream.)
			Assert( seg.m_cbSegSize > 0 );

			// Copy the unreliable segment into the packet.  Does the portion we are serializing
			// begin in the header?
			if ( seg.m_nOffset < seg.m_pMsg->m_cbSNPSendReliableHeader )
			{
				int cbCopyHdr = std::min( seg.m_cbSegSize, seg.m_pMsg->m_cbSNPSendReliableHeader - seg.m_nOffset );

				memcpy( pPayloadPtr, seg.m_pMsg->SNPSend_ReliableHeader() + seg.m_nOffset, cbCopyHdr );
				pPayloadPtr += cbCopyHdr;

				int cbCopyBody = seg.m_cbSegSize - cbCopyHdr;
				if ( cbCopyBody > 0 )
				{
					memcpy( pPayloadPtr, seg.m_pMsg->m_pData, cbCopyBody );
					pPayloadPtr += cbCopyBody;
				}
			}
			else
			{
				// This segment is entirely from the message body
				memcpy( pPayloadPtr, (char*)seg.m_pMsg->m_pData + seg.m_nOffset - seg.m_pMsg->m_cbSNPSendReliableHeader, seg.m_cbSegSize );
				pPayloadPtr += seg.m_cbSegSize;
			}


			// Remember that this range is in-flight
			SNPRange_t range;
			range.m_nBegin = seg.m_pMsg->SNPSend_ReliableStreamPos() + seg.m_nOffset;
			range.m_nEnd = range.m_nBegin + seg.m_cbSegSize;

			// Ranges of the reliable stream that have not been acked should either be
			// in flight, or queued for retry.  Make sure this range is not already in
			// either state.
			Assert( !HasOverlappingRange( range, m_senderState.m_listInFlightReliableRange ) );
			Assert( !HasOverlappingRange( range, m_senderState.m_listReadyRetryReliableRange ) );

			// Spew
			SpewDebugGroup( nLogLevelPacketDecode, "[%s]   encode pkt %lld reliable msg %lld offset %d+%d=%d range [%lld,%lld)\n",
				GetDescription(), (long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)seg.m_pMsg->m_nMessageNumber,
				seg.m_nOffset, seg.m_cbSegSize, seg.m_nOffset+seg.m_cbSegSize,
				(long long)range.m_nBegin, (long long)range.m_nEnd );

			// Add to table of in-flight reliable ranges
			m_senderState.m_listInFlightReliableRange[ range ] = seg.m_pMsg;

			// Remember that this packet contained that range
			inFlightPkt.m_vecReliableSegments.push_back( range );

			// Less reliable data pending
			m_senderState.m_cbPendingReliable -= seg.m_cbSegSize;
			Assert( m_senderState.m_cbPendingReliable >= 0 );
		}
		else
		{
			// We should only encode an empty segment if the message itself is empty
			Assert( seg.m_cbSegSize > 0 || ( seg.m_cbSegSize == 0 && seg.m_pMsg->m_cbSize == 0 ) );

			// Check some stuff
			Assert( bStillInQueue == ( seg.m_nOffset + seg.m_cbSegSize < seg.m_pMsg->m_cbSize ) ); // If we ended the message, we should have removed it from the queue
			Assert( bStillInQueue == ( ( seg.m_hdr[0] & 0x20 ) == 0 ) );
			Assert( bStillInQueue || seg.m_pMsg->m_links.m_pNext == nullptr ); // If not in the queue, we should be detached
			Assert( seg.m_pMsg->m_links.m_pPrev == nullptr ); // We should either be at the head of the queue, or detached

			// Copy the unreliable segment into the packet
			memcpy( pPayloadPtr, (char*)seg.m_pMsg->m_pData + seg.m_nOffset, seg.m_cbSegSize );
			pPayloadPtr += seg.m_cbSegSize;

			// Spew
			SpewDebugGroup( nLogLevelPacketDecode, "[%s]   encode pkt %lld unreliable msg %lld offset %d+%d=%d\n",
				GetDescription(), (long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)seg.m_pMsg->m_nMessageNumber,
				seg.m_nOffset, seg.m_cbSegSize, seg.m_nOffset+seg.m_cbSegSize );

			// Less unreliable data pending
			m_senderState.m_cbPendingUnreliable -= seg.m_cbSegSize;
			Assert( m_senderState.m_cbPendingUnreliable >= 0 );

			// Done with this message?  Clean up
			if ( !bStillInQueue )
				seg.m_pMsg->Release();
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

	// OK, we have a plaintext payload.  Encrypt and send it.
	// What cipher are we using?
	int nBytesSent = 0;
	switch ( m_eNegotiatedCipher )
	{
		default:
			AssertMsg1( false, "Bogus cipher %d", m_eNegotiatedCipher );
			break;

		case k_ESteamNetworkingSocketsCipher_NULL:
		{

			// No encryption!
			// Ask current transport to deliver it
			nBytesSent = pTransport->SendEncryptedDataChunk( payload, cbPlainText, ctx );
		}
		break;

		case k_ESteamNetworkingSocketsCipher_AES_256_GCM:
		{

			Assert( m_bCryptKeysValid );

			// Adjust the IV by the packet number
			*(uint64 *)&m_cryptIVSend.m_buf += LittleQWord( m_statsEndToEnd.m_nNextSendSequenceNumber );

			// Encrypt the chunk
			uint8 arEncryptedChunk[ k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend + 64 ]; // Should not need pad
			uint32 cbEncrypted = sizeof(arEncryptedChunk);
			DbgVerify( m_cryptContextSend.Encrypt(
				payload, cbPlainText, // plaintext
				m_cryptIVSend.m_buf, // IV
				arEncryptedChunk, &cbEncrypted, // output
				nullptr, 0 // no AAD
			) );

			//SpewMsg( "Send encrypt IV %llu + %02x%02x%02x%02x  encrypted %d %02x%02x%02x%02x\n",
			//	*(uint64 *)&m_cryptIVSend.m_buf,
			//	m_cryptIVSend.m_buf[8], m_cryptIVSend.m_buf[9], m_cryptIVSend.m_buf[10], m_cryptIVSend.m_buf[11],
			//	cbEncrypted,
			//	arEncryptedChunk[0], arEncryptedChunk[1], arEncryptedChunk[2],arEncryptedChunk[3]
			//);

			// Restore the IV to the base value
			*(uint64 *)&m_cryptIVSend.m_buf -= LittleQWord( m_statsEndToEnd.m_nNextSendSequenceNumber );

			Assert( (int)cbEncrypted >= cbPlainText );
			Assert( (int)cbEncrypted <= k_cbSteamNetworkingSocketsMaxEncryptedPayloadSend ); // confirm that pad above was not necessary and we never exceed k_nMaxSteamDatagramTransportPayload, even after encrypting

			// Ask current transport to deliver it
			nBytesSent = pTransport->SendEncryptedDataChunk( arEncryptedChunk, cbEncrypted, ctx );
		}
	}
	if ( nBytesSent <= 0 )
		return false;

	// We sent a packet.  Track it
	auto pairInsertResult = m_senderState.m_mapInFlightPacketsByPktNum.insert( pairInsert );
	Assert( pairInsertResult.second ); // We should have inserted a new element, not updated an existing element

	// If we sent any reliable data, we should expect a reply
	if ( !inFlightPkt.m_vecReliableSegments.empty() )
	{
		m_statsEndToEnd.TrackSentMessageExpectingSeqNumAck( usecNow, true );
		// FIXME - should let transport know
	}

	// If we aren't already tracking anything to timeout, then this is the next one.
	if ( m_senderState.m_itNextInFlightPacketToTimeout == m_senderState.m_mapInFlightPacketsByPktNum.end() )
		m_senderState.m_itNextInFlightPacketToTimeout = pairInsertResult.first;

	#ifdef SNP_ENABLE_PACKETSENDLOG
		pLog->m_cbSent = nBytesSent;
	#endif

	// We spent some tokens
	m_senderState.m_flTokenBucket -= (float)nBytesSent;
	return true;
}

void CSteamNetworkConnectionBase::SNP_SentNonDataPacket( CConnectionTransport *pTransport, SteamNetworkingMicroseconds usecNow )
{
	std::pair<int64,SNPInFlightPacket_t> pairInsert( m_statsEndToEnd.m_nNextSendSequenceNumber-1, SNPInFlightPacket_t{ usecNow, false, pTransport, {} } );
	auto pairInsertResult = m_senderState.m_mapInFlightPacketsByPktNum.insert( pairInsert );
	Assert( pairInsertResult.second ); // We should have inserted a new element, not updated an existing element.  Probably an order ofoperations bug with m_nNextSendSequenceNumber
}

void CSteamNetworkConnectionBase::SNP_GatherAckBlocks( SNPAckSerializerHelper &helper, SteamNetworkingMicroseconds usecNow )
{
	helper.m_nBlocks = 0;
	helper.m_nBlocksNeedToAck = 0;

	// Fast case for no packet loss we need to ack, which will (hopefully!) be a common case
	int n = len( m_receiverState.m_mapPacketGaps ) - 1;
	if ( n <= 0 )
		return;

	// Let's not just flush the acks that are due right now.  Let's flush all of them
	// that will be due any time before we have the bandwidth to send the next packet.
	// (Assuming that we send the max packet size here.)
	SteamNetworkingMicroseconds usecSendAcksDueBefore = usecNow;
	SteamNetworkingMicroseconds usecTimeUntilNextPacket = SteamNetworkingMicroseconds( ( m_senderState.m_flTokenBucket - (float)m_cbMTUPacketSize ) / (float)m_senderState.m_n_x * -1e6 );
	if ( usecTimeUntilNextPacket > 0 )
		usecSendAcksDueBefore += usecTimeUntilNextPacket;

	m_receiverState.DebugCheckPackGapMap();

	n = std::min( (int)helper.k_nMaxBlocks, n );
	auto itNext = m_receiverState.m_mapPacketGaps.begin();

	int cbEncodedSize = helper.k_cbHeaderSize;
	while ( n > 0 )
	{
		--n;
		auto itCur = itNext;
		++itNext;

		Assert( itCur->first < itCur->second.m_nEnd );

		// Do we need to report on this block now?
		bool bNeedToReport = ( itNext->second.m_usecWhenAckPrior <= usecSendAcksDueBefore );

		// Should we wait to NACK this?
		if ( itCur == m_receiverState.m_itPendingNack )
		{

			// Wait to NACK this?
			if ( !bNeedToReport )
			{
				if ( usecNow < itCur->second.m_usecWhenOKToNack )
					break;
				bNeedToReport = true;
			}

			// Go ahead and NACK it.  If the packet arrives, we will use it.
			// But our NACK may cause the sender to retransmit.
			++m_receiverState.m_itPendingNack;
		}

		SNPAckSerializerHelper::Block &block = helper.m_arBlocks[ helper.m_nBlocks ];
		block.m_nNack = uint32( itCur->second.m_nEnd - itCur->first );

		int64 nAckEnd;
		SteamNetworkingMicroseconds usecWhenSentLast;
		if ( n == 0 )
		{
			// itNext should be the sentinel
			Assert( itNext->first == INT64_MAX );
			nAckEnd = m_statsEndToEnd.m_nMaxRecvPktNum+1;
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

		// When we encode 7+ blocks, the header grows by one byte
		// to store an explicit count
		if ( helper.m_nBlocks == 6 )
			++cbEncodedSize;

		// This block
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

		// Do we really need to try to flush the ack/nack for that block out now?
		if ( bNeedToReport )
			helper.m_nBlocksNeedToAck = helper.m_nBlocks;
	}
}

uint8 *CSteamNetworkConnectionBase::SNP_SerializeAckBlocks( const SNPAckSerializerHelper &helper, uint8 *pOut, const uint8 *pOutEnd, SteamNetworkingMicroseconds usecNow )
{

	// We shouldn't be called if we never received anything
	Assert( m_statsEndToEnd.m_nMaxRecvPktNum > 0 );

	// No room even for the header?
	if ( pOut + SNPAckSerializerHelper::k_cbHeaderSize > pOutEnd )
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

	int nLogLevelPacketDecode = m_connectionConfig.m_LogLevel_PacketDecode.Get();

	#ifdef SNP_ENABLE_PACKETSENDLOG
		PacketSendLog *pLog = &m_vecSendLog[ m_vecSendLog.size()-1 ];
	#endif

	// Fast case for no packet loss we need to ack, which will (hopefully!) be a common case
	if ( m_receiverState.m_mapPacketGaps.size() == 1 )
	{
		int64 nLastRecvPktNum = m_statsEndToEnd.m_nMaxRecvPktNum;
		*pLatestPktNum = LittleWord( (uint16)nLastRecvPktNum );
		*pTimeSinceLatestPktNum = LittleWord( (uint16)SNPAckSerializerHelper::EncodeTimeSince( usecNow, m_statsEndToEnd.m_usecTimeLastRecvSeq ) );

		SpewDebugGroup( nLogLevelPacketDecode, "[%s]   encode pkt %lld last recv %lld (no loss)\n",
			GetDescription(),
			(long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)nLastRecvPktNum
		);
		m_receiverState.m_mapPacketGaps.rbegin()->second.m_usecWhenAckPrior = INT64_MAX; // Clear timer, we wrote everything we needed to

		#ifdef SNP_ENABLE_PACKETSENDLOG
			pLog->m_nAckBlocksSent = 0;
			pLog->m_nAckEnd = nLastRecvPktNum;
		#endif

		return pOut;
	}

	// Fit as many blocks as possible.
	// (Unless we are badly fragmented and are trying to squeeze in what
	// we can at the end of a packet, this won't ever iterate
	int nBlocks = helper.m_nBlocks;
	uint8 *pExpectedOutEnd;
	for (;;)
	{

		// Not sending any blocks at all?  (Either they don't fit, or we are waiting because we don't
		// want to nack yet.)  Just fill in the header with the oldest ack
		if ( nBlocks == 0 )
		{
			auto itOldestGap = m_receiverState.m_mapPacketGaps.begin();
			int64 nLastRecvPktNum = itOldestGap->first-1;
			*pLatestPktNum = LittleWord( uint16( nLastRecvPktNum ) );
			*pTimeSinceLatestPktNum = LittleWord( (uint16)SNPAckSerializerHelper::EncodeTimeSince( usecNow, itOldestGap->second.m_usecWhenReceivedPktBefore ) );

			SpewDebugGroup( nLogLevelPacketDecode, "[%s]   encode pkt %lld last recv %lld (no blocks, actual last recv=%lld)\n",
				GetDescription(),
				(long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)nLastRecvPktNum, (long long)m_statsEndToEnd.m_nMaxRecvPktNum
			);

			#ifdef SNP_ENABLE_PACKETSENDLOG
				pLog->m_nAckBlocksSent = 0;
				pLog->m_nAckEnd = nLastRecvPktNum;
			#endif

			// Acked packets before this gap.  Were we waiting to flush them?
			if ( itOldestGap == m_receiverState.m_itPendingAck )
			{
				// Mark it as sent
				m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior = INT64_MAX;
				++m_receiverState.m_itPendingAck;
			}

			// NOTE: We did NOT nack anything just now
			return pOut;
		}

		int cbTotalEncoded = helper.m_arBlocks[nBlocks-1].m_cbTotalEncodedSize;
		pExpectedOutEnd = pAckHeaderByte + cbTotalEncoded; // Save for debugging below
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
	int64 nAckEnd = ( m_statsEndToEnd.m_nMaxRecvPktNum & ~(int64)(~(uint32)0) ) | pBlock->m_nLatestPktNum;
	++nAckEnd;

	#ifdef SNP_ENABLE_PACKETSENDLOG
		pLog->m_nAckBlocksSent = nBlocks;
		pLog->m_nAckEnd = nAckEnd;
	#endif

	SpewDebugGroup( nLogLevelPacketDecode, "[%s]   encode pkt %lld last recv %lld (%d blocks, actual last recv=%lld)\n",
		GetDescription(),
		(long long)m_statsEndToEnd.m_nNextSendSequenceNumber, (long long)(nAckEnd-1), nBlocks, (long long)m_statsEndToEnd.m_nMaxRecvPktNum
	);

	// Check for a common case where we report on everything
	if ( nAckEnd > m_statsEndToEnd.m_nMaxRecvPktNum )
	{
		Assert( nAckEnd == m_statsEndToEnd.m_nMaxRecvPktNum+1 );
		for (;;)
		{
			m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior = INT64_MAX;
			if ( m_receiverState.m_itPendingAck->first == INT64_MAX )
				break;
			++m_receiverState.m_itPendingAck;
		}
		m_receiverState.m_itPendingNack = m_receiverState.m_itPendingAck;
	}
	else
	{

		// Advance pointer to next block that needs to be acked,
		// past the ones we are about to ack.
		if ( m_receiverState.m_itPendingAck->first <= nAckEnd )
		{
			do
			{
				m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior = INT64_MAX;
				++m_receiverState.m_itPendingAck;
			} while ( m_receiverState.m_itPendingAck->first <= nAckEnd );
		}

		// Advance pointer to next block that needs to be nacked, past the ones
		// we are about to nack.
		while ( m_receiverState.m_itPendingNack->first < nAckEnd )
			++m_receiverState.m_itPendingNack;
	}

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
		SpewDebugGroup( nLogLevelPacketDecode, "[%s]   encode pkt %lld nack [%lld,%lld) ack [%lld,%lld) \n",
			GetDescription(),
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
	SpewVerboseGroup( m_connectionConfig.m_LogLevel_PacketDecode.Get(), "[%s]   encode pkt %lld stop_waiting offset %lld = %lld",
		GetDescription(),
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
	SpewDebugGroup( m_connectionConfig.m_LogLevel_PacketDecode.Get(), "[%s] RX msg %lld offset %d+%d=%d %02x ... %02x\n", GetDescription(), nMsgNum, nOffset, cbSegmentSize, nOffset+cbSegmentSize, ((byte*)pSegmentData)[0], ((byte*)pSegmentData)[cbSegmentSize-1] );

	// Ignore data segments when we are not going to process them (e.g. linger)
	if ( GetState() != k_ESteamNetworkingConnectionState_Connected )
	{
		SpewDebugGroup( m_connectionConfig.m_LogLevel_PacketDecode.Get(), "[%s] discarding msg %lld [%d,%d) as connection is in state %d\n",
			GetDescription(),
			nMsgNum,
			nOffset, nOffset+cbSegmentSize,
			(int)GetState() );
		return;
	}

	// Check for a common special case: non-fragmented message.
	if ( nOffset == 0 && bLastSegmentInMessage )
	{

		// Deliver it immediately, don't go through the fragmentation assembly process below.
		// (Although that would work.)
		ReceivedMessage( pSegmentData, cbSegmentSize, nMsgNum, k_nSteamNetworkingSend_Unreliable, usecNow );
		return;
	}

	// Limit number of unreliable segments we store.  We just use a fixed
	// limit, rather than trying to be smart by expiring based on time or whatever.
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
			SpewWarningRateLimited( usecNow, "[%s] SNP expiring unreliable segments for msg %lld, while receiving unreliable segments for msg %lld\n",
				GetDescription(), (long long)nDeleteMsgNum, (long long)nMsgNum );
		}
	}

	// Message fragment.  Find/insert the entry in our reassembly queue
	// I really hate this syntax and interface.
	SSNPRecvUnreliableSegmentKey key;
	key.m_nMsgNum = nMsgNum;
	key.m_nOffset = nOffset;
	SSNPRecvUnreliableSegmentData &data = m_receiverState.m_mapUnreliableSegments[ key ];
	if ( data.m_cbSegSize >= 0 )
	{
		// We got another segment starting at the same offset.  This is weird, since they shouldn't
		// be doing.  But remember that we're working on top of UDP, which could deliver packets
		// multiple times.  We'll spew about it, just in case it indicates a bug in this code or the sender.
		SpewWarningRateLimited( usecNow, "[%s] Received unreliable msg %lld segment offset %d twice.  Sizes %d,%d, last=%d,%d\n",
			GetDescription(), nMsgNum, nOffset, data.m_cbSegSize, cbSegmentSize, (int)data.m_bLast, (int)bLastSegmentInMessage );

		// Just drop the segment.  Note that the sender might have sent a longer segment from the previous
		// one, in which case this segment contains new data, and is not therefore redundant.  That seems
		// "legal", but very weird, and not worth handling.  If senders do retransmit unreliable segments
		// (perhaps FEC?) then they need to retransmit the exact same segments.
		return;
	}

	// Segment in the map either just got inserted, or is a subset of the segment
	// we just received.  Replace it.
	data.m_cbSegSize = cbSegmentSize;
	Assert( !data.m_bLast );
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
		cbMessageSize = std::max( cbMessageSize, itMsgLast->first.m_nOffset + itMsgLast->second.m_cbSegSize );

		// Is that the end?
		if ( itMsgLast->second.m_bLast )
			break;

		// Still looking for the end
		++itMsgLast;
		if ( itMsgLast == end )
			return;
	}

	CSteamNetworkingMessage *pMsg = CSteamNetworkingMessage::New( this, cbMessageSize, nMsgNum, k_nSteamNetworkingSend_Unreliable, usecNow );
	if ( !pMsg )
		return;

	// OK, we have the complete message!  Gather the
	// segments into a contiguous buffer
	for (;;)
	{
		Assert( itMsgStart->first.m_nMsgNum == nMsgNum );
		memcpy( (char *)pMsg->m_pData + itMsgStart->first.m_nOffset, itMsgStart->second.m_buf, itMsgStart->second.m_cbSegSize );

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
	ReceivedMessage( pMsg );
}

bool CSteamNetworkConnectionBase::SNP_ReceiveReliableSegment( int64 nPktNum, int64 nSegBegin, const uint8 *pSegmentData, int cbSegmentSize, SteamNetworkingMicroseconds usecNow )
{
	int nLogLevelPacketDecode = m_connectionConfig.m_LogLevel_PacketDecode.Get();

	// Calculate segment end stream position
	int64 nSegEnd = nSegBegin + cbSegmentSize;

	// Spew
	SpewVerboseGroup( nLogLevelPacketDecode, "[%s]   decode pkt %lld reliable range [%lld,%lld)\n",
		GetDescription(),
		(long long)nPktNum,
		(long long)nSegBegin, (long long)nSegEnd );

	// No segment data?  Seems fishy, but if it happens, just skip it.
	Assert( cbSegmentSize >= 0 );
	if ( cbSegmentSize <= 0 )
	{
		// Spew but rate limit in case of malicious sender
		SpewWarningRateLimited( usecNow, "[%s] decode pkt %lld empty reliable segment?\n",
			GetDescription(),
			(long long)nPktNum );
		return true;
	}

	// Ignore data segments when we are not going to process them (e.g. linger)
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_FindingRoute: // Go ahead and process it here.  The higher level code should change the state soon enough.
			break;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_FinWait:
			// Discard data, but continue processing packet
			SpewVerboseGroup( nLogLevelPacketDecode, "[%s]   discarding pkt %lld [%lld,%lld) as connection is in state %d\n",
				GetDescription(),
				(long long)nPktNum,
				(long long)nSegBegin, (long long)nSegEnd,
				(int)GetState() );
			return true;

		default:
			// Higher level code should probably not call this in these states
			AssertMsg( false, "Unexpected state %d", GetState() );
			return false;
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
			SpewWarningRateLimited( usecNow, "[%s] decode pkt %lld abort.  %lld bytes reliable data buffered [%lld-%lld), new size would be %lld to %lld\n",
				GetDescription(),
				(long long)nPktNum,
				(long long)m_receiverState.m_bufReliableStream.size(),
				(long long)m_receiverState.m_nReliableStreamPos,
				(long long)( m_receiverState.m_nReliableStreamPos + m_receiverState.m_bufReliableStream.size() ),
				(long long)cbNewSize, (long long)nSegEnd
			);
			return false;  // DO NOT ACK THIS PACKET
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
					SpewWarningRateLimited( usecNow, "[%s] decode pkt %lld abort.  Reliable stream already has %d fragments, first is [%lld,%lld), last is [%lld,%lld), new segment is [%lld,%lld)\n",
						GetDescription(),
						(long long)nPktNum,
						len( m_receiverState.m_mapReliableStreamGaps ),
						(long long)m_receiverState.m_mapReliableStreamGaps.begin()->first, (long long)m_receiverState.m_mapReliableStreamGaps.begin()->second,
						(long long)m_receiverState.m_mapReliableStreamGaps.rbegin()->first, (long long)m_receiverState.m_mapReliableStreamGaps.rbegin()->second,
						(long long)nSegBegin, (long long)nSegEnd
					);
					return false;  // DO NOT ACK THIS PACKET
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
								SpewWarningRateLimited( usecNow, "[%s] decode pkt %lld abort.  Reliable stream already has %d fragments, first is [%lld,%lld), last is [%lld,%lld).  We don't want to fragment [%lld,%lld) with new segment [%lld,%lld)\n",
									GetDescription(),
									(long long)nPktNum,
									len( m_receiverState.m_mapReliableStreamGaps ),
									(long long)m_receiverState.m_mapReliableStreamGaps.begin()->first, (long long)m_receiverState.m_mapReliableStreamGaps.begin()->second,
									(long long)m_receiverState.m_mapReliableStreamGaps.rbegin()->first, (long long)m_receiverState.m_mapReliableStreamGaps.rbegin()->second,
									(long long)gapFilled->first, (long long)gapFilled->second,
									(long long)nSegBegin, (long long)nSegEnd
								);
								return false;  // DO NOT ACK THIS PACKET
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
		SpewDebugGroup( nLogLevelPacketDecode, "[%s]   decode pkt %lld valid reliable bytes = %d [%lld,%lld)\n",
			GetDescription(),
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
			{
				// We haven't received all of the message
				return true; // Packet OK and can be acked.
			}

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
			{
				// We haven't received all of the message
				return true; // Packet OK and can be acked.
			}

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
			return true; // packet is OK, can be acked, and continue processing it
		}

		// We have a full message!  Queue it
		if ( !ReceivedMessage( pReliableDecode, cbMsgSize, nMsgNum, k_nSteamNetworkingSend_Reliable, usecNow ) )
			return false; // Weird failure.  Most graceful response is to not ack this packet, and maybe we will work next on retry.
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

	return true; // packet is OK, can be acked, and continue processing it
}

void CSteamNetworkConnectionBase::SNP_RecordReceivedPktNum( int64 nPktNum, SteamNetworkingMicroseconds usecNow, bool bScheduleAck )
{

	// Check if sender has already told us they don't need us to
	// account for packets this old anymore
	if ( unlikely( nPktNum < m_receiverState.m_nMinPktNumToSendAcks ) )
		return;

	// Fast path for the (hopefully) most common case of packets arriving in order
	if ( likely( nPktNum == m_statsEndToEnd.m_nMaxRecvPktNum+1 ) )
	{
		if ( bScheduleAck ) // fast path for all unreliable data (common when we are just being used for transport)
		{
			// Schedule ack of this packet (since we are the highest numbered
			// packet, that means reporting on everything)
			QueueFlushAllAcks( usecNow + k_usecMaxDataAckDelay );
		}
		return;
	}

	// At this point, ack invariants should be met
	m_receiverState.DebugCheckPackGapMap();

	// Latest time that this packet should be acked.
	// (We might already be scheduled to send and ack that would include this packet.)
	SteamNetworkingMicroseconds usecScheduleAck = bScheduleAck ? usecNow + k_usecMaxDataAckDelay : INT64_MAX;

	// Check if this introduced a gap since the last sequence packet we have received
	if ( nPktNum > m_statsEndToEnd.m_nMaxRecvPktNum )
	{

		// Protect against malicious sender!
		if ( len( m_receiverState.m_mapPacketGaps ) >= k_nMaxPacketGaps )
			return; // Nope, we will *not* actually mark the packet as received

		// Add a gap for the skipped packet(s).
		int64 nBegin = m_statsEndToEnd.m_nMaxRecvPktNum+1;
		std::pair<int64,SSNPPacketGap> x;
		x.first = nBegin;
		x.second.m_nEnd = nPktNum;
		x.second.m_usecWhenReceivedPktBefore = m_statsEndToEnd.m_usecTimeLastRecvSeq;
		x.second.m_usecWhenAckPrior = m_receiverState.m_mapPacketGaps.rbegin()->second.m_usecWhenAckPrior;

		// When should we nack this?
		x.second.m_usecWhenOKToNack = usecNow;
		if ( nPktNum < m_statsEndToEnd.m_nMaxRecvPktNum + 3 )
			x.second.m_usecWhenOKToNack += k_usecNackFlush;

		auto iter = m_receiverState.m_mapPacketGaps.insert( x ).first;

		SpewMsgGroup( m_connectionConfig.m_LogLevel_PacketGaps.Get(), "[%s] drop %d pkts [%lld-%lld)",
			GetDescription(),
			(int)( nPktNum - nBegin ),
			(long long)nBegin, (long long)nPktNum );

		// Remember that we need to send a NACK
		if ( m_receiverState.m_itPendingNack->first == INT64_MAX )
		{
			m_receiverState.m_itPendingNack = iter;
		}
		else
		{
			// Pending nacks should be for older packet, not newer
			Assert( m_receiverState.m_itPendingNack->first < nBegin );
		}

		// Back up if we we had a flush of everything scheduled
		if ( m_receiverState.m_itPendingAck->first == INT64_MAX && m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior < INT64_MAX )
		{
			Assert( iter->second.m_usecWhenAckPrior == m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior );
			m_receiverState.m_itPendingAck = iter;
		}

		// At this point, ack invariants should be met
		m_receiverState.DebugCheckPackGapMap();

		// Schedule ack of this packet (since we are the highest numbered
		// packet, that means reporting on everything) by the requested
		// time
		QueueFlushAllAcks( usecScheduleAck );
	}
	else
	{

		// Check if this filed a gap
		auto itGap = m_receiverState.m_mapPacketGaps.upper_bound( nPktNum );
		if ( itGap == m_receiverState.m_mapPacketGaps.end() )
		{
			AssertMsg( false, "[%s] Cannot locate gap, or processing packet %lld multiple times. %s | %s",
				GetDescription(), (long long)nPktNum,
				m_statsEndToEnd.RecvPktNumStateDebugString().c_str(), m_statsEndToEnd.HistoryRecvSeqNumDebugString(8).c_str() );
			return;
		}
		if ( itGap == m_receiverState.m_mapPacketGaps.begin() )
		{
			AssertMsg( false, "[%s] Cannot locate gap, or processing packet %lld multiple times. [%lld,%lld) %s | %s",
				GetDescription(), (long long)nPktNum, (long long)itGap->first, (long long)itGap->second.m_nEnd,
				m_statsEndToEnd.RecvPktNumStateDebugString().c_str(), m_statsEndToEnd.HistoryRecvSeqNumDebugString(8).c_str() );
			return;
		}
		--itGap;
		if ( itGap->first > nPktNum || itGap->second.m_nEnd <= nPktNum )
		{
			// We already received this packet.  But this should be impossible now,
			// we should be rejecting duplicate packet numbers earlier
			AssertMsg( false, "[%s] Packet gap bug.  %lld [%lld,%lld) %s | %s",
				GetDescription(), (long long)nPktNum, (long long)itGap->first, (long long)itGap->second.m_nEnd,
				m_statsEndToEnd.RecvPktNumStateDebugString().c_str(), m_statsEndToEnd.HistoryRecvSeqNumDebugString(8).c_str() );
			return;
		}

		// Packet is in a gap where we previously thought packets were lost.
		// (Packets arriving out of order.)

		// Last packet in gap?
		if ( itGap->second.m_nEnd-1 == nPktNum )
		{
			// Single-packet gap?
			if ( itGap->first == nPktNum )
			{
				// Were we waiting to ack/nack this?  Then move forward to the next gap, if any
				usecScheduleAck = std::min( usecScheduleAck, itGap->second.m_usecWhenAckPrior );
				if ( m_receiverState.m_itPendingAck == itGap )
					++m_receiverState.m_itPendingAck;
				if ( m_receiverState.m_itPendingNack == itGap )
					++m_receiverState.m_itPendingNack;

				// Save time when we needed to ack the packets before this gap
				SteamNetworkingMicroseconds usecWhenAckPrior = itGap->second.m_usecWhenAckPrior;

				// Gap is totally filled.  Erase, and move to the next one,
				// if any, so we can schedule ack below
				itGap = m_receiverState.m_mapPacketGaps.erase( itGap );

				// Were we scheduled to ack the packets before this?  If so, then
				// we still need to do that, only now when we send that ack, we will
				// ack the packets after this gap as well, since they will be included
				// in the same ack block.
				//
				// NOTE: This is based on what was scheduled to be acked before we got
				// this packet.  If we need to update the schedule to ack the current
				// packet, we will do that below.  However, usually if previous
				// packets were already scheduled to be acked, then that deadline time
				// will be sooner usecScheduleAck, so the code below will not actually
				// do anything.
				if ( usecWhenAckPrior < itGap->second.m_usecWhenAckPrior )
				{
					itGap->second.m_usecWhenAckPrior = usecWhenAckPrior;
				}
				else
				{
					// Otherwise, we might not have any acks scheduled.  In that
					// case, the invariant is that m_itPendingAck should point at the sentinel
					if ( m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior == INT64_MAX )
					{
						m_receiverState.m_itPendingAck = m_receiverState.m_mapPacketGaps.end();
						--m_receiverState.m_itPendingAck;
						Assert( m_receiverState.m_itPendingAck->first == INT64_MAX );
					}
				}

				SpewVerboseGroup( m_connectionConfig.m_LogLevel_PacketGaps.Get(), "[%s] decode pkt %lld, single pkt gap filled", GetDescription(), (long long)nPktNum );

				// At this point, ack invariants should be met
				m_receiverState.DebugCheckPackGapMap();
			}
			else
			{
				// Shrink gap by one from the end
				--itGap->second.m_nEnd;
				Assert( itGap->first < itGap->second.m_nEnd );

				SpewVerboseGroup( m_connectionConfig.m_LogLevel_PacketGaps.Get(), "[%s] decode pkt %lld, last packet in gap, reduced to [%lld,%lld)", GetDescription(),
					(long long)nPktNum, (long long)itGap->first, (long long)itGap->second.m_nEnd );

				// Move to the next gap so we can schedule ack below
				++itGap;

				// At this point, ack invariants should be met
				m_receiverState.DebugCheckPackGapMap();
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

			SpewVerboseGroup( m_connectionConfig.m_LogLevel_PacketGaps.Get(), "[%s] decode pkt %lld, first packet in gap, reduced to [%lld,%lld)", GetDescription(),
				(long long)nPktNum, (long long)itGap->first, (long long)itGap->second.m_nEnd );

			// At this point, ack invariants should be met
			m_receiverState.DebugCheckPackGapMap();
		}
		else
		{
			// Packet is in the middle of the gap.  We'll need to fragment this gap
			// Protect against malicious sender!
			if ( len( m_receiverState.m_mapPacketGaps ) >= k_nMaxPacketGaps )
				return; // Nope, we will *not* actually mark the packet as received

			// Locate the next block so we can set the schedule time
			auto itNext = itGap;
			++itNext;

			// Start making a new gap to account for the upper end
			std::pair<int64,SSNPPacketGap> upper;
			upper.first = nPktNum+1;
			upper.second.m_nEnd = itGap->second.m_nEnd;
			upper.second.m_usecWhenReceivedPktBefore = usecNow;
			if ( itNext == m_receiverState.m_itPendingAck )
				upper.second.m_usecWhenAckPrior = INT64_MAX;
			else
				upper.second.m_usecWhenAckPrior = itNext->second.m_usecWhenAckPrior;
			upper.second.m_usecWhenOKToNack = itGap->second.m_usecWhenOKToNack;

			// Truncate the current gap
			itGap->second.m_nEnd = nPktNum;
			Assert( itGap->first < itGap->second.m_nEnd );

			SpewVerboseGroup( m_connectionConfig.m_LogLevel_PacketGaps.Get(), "[%s] decode pkt %lld, gap split [%lld,%lld) and [%lld,%lld)", GetDescription(),
				(long long)nPktNum, (long long)itGap->first, (long long)itGap->second.m_nEnd, upper.first, upper.second.m_nEnd );

			// Insert a new gap to account for the upper end, and
			// advance iterator to it, so that we can schedule ack below
			itGap = m_receiverState.m_mapPacketGaps.insert( upper ).first;

			// At this point, ack invariants should be met
			m_receiverState.DebugCheckPackGapMap();
		}

		Assert( itGap != m_receiverState.m_mapPacketGaps.end() );

		// Need to schedule ack (earlier than it is already scheduled)?
		if ( usecScheduleAck < itGap->second.m_usecWhenAckPrior )
		{

			// Earlier than the current thing being scheduled?
			if ( usecScheduleAck <= m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior )
			{

				// We're next, set the time
				itGap->second.m_usecWhenAckPrior = usecScheduleAck;

				// Any schedules for lower-numbered packets are superseded
				// by this one.
				if ( m_receiverState.m_itPendingAck->first <= itGap->first )
				{
					while ( m_receiverState.m_itPendingAck != itGap )
					{
						m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior = INT64_MAX;
						++m_receiverState.m_itPendingAck;
					}
				}
				else
				{
					// If our number is lower than the thing that was scheduled next,
					// then back up and re-schedule any blocks in between to be effectively
					// the same time as they would have been flushed before.
					SteamNetworkingMicroseconds usecOldSched = m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior;
					while ( --m_receiverState.m_itPendingAck != itGap )
					{
						m_receiverState.m_itPendingAck->second.m_usecWhenAckPrior = usecOldSched;
					}
				}
			}
			else
			{
				// We're not the next thing that needs to be acked.
				
				if ( itGap->first < m_receiverState.m_itPendingAck->first )
				{
					// We're a lowered numbered packet,	so this request is subsumed by the
					// request to flush more packets at an earlier time,
					// and we don't need to do anything.

				}
				else
				{

					// We need to ack a bit earlier
					itGap->second.m_usecWhenAckPrior = usecScheduleAck;

					// Now the only way for our invariants to be violated is for lower
					// numbered blocks to have later scheduled times.
					Assert( itGap != m_receiverState.m_mapPacketGaps.begin() );
					while ( (--itGap)->second.m_usecWhenAckPrior > usecScheduleAck )
					{
						Assert( itGap != m_receiverState.m_mapPacketGaps.begin() );
						itGap->second.m_usecWhenAckPrior = usecScheduleAck;
					}
				}
			}

			// Make sure we didn't screw things up
			m_receiverState.DebugCheckPackGapMap();
		}

		// Make sure are scheduled to wake up
		if ( bScheduleAck )
			EnsureMinThinkTime( m_receiverState.TimeWhenFlushAcks() );
	}
}

int CSteamNetworkConnectionBase::SNP_ClampSendRate()
{
	// Get effective clamp limits.  We clamp the limits themselves to be safe
	// and make sure they are sane
	int nMin = Clamp( m_connectionConfig.m_SendRateMin.Get(), 1024, 100*1024*1024 );
	int nMax = Clamp( m_connectionConfig.m_SendRateMax.Get(), nMin, 100*1024*1024 );

	// Clamp it, adjusting the value if it's out of range
	m_senderState.m_n_x = Clamp( m_senderState.m_n_x, nMin, nMax );

	// Return value
	return m_senderState.m_n_x;
}

// Returns next think time
SteamNetworkingMicroseconds CSteamNetworkConnectionBase::SNP_ThinkSendState( SteamNetworkingMicroseconds usecNow )
{
	// Accumulate tokens based on how long it's been since last time
	SNP_ClampSendRate();
	SNP_TokenBucket_Accumulate( usecNow );

	// Calculate next time we want to take action.  If it isn't right now, then we're either idle or throttled.
	// Importantly, this will also check for retry timeout
	SteamNetworkingMicroseconds usecNextThink = SNP_GetNextThinkTime( usecNow );
	if ( usecNextThink > usecNow )
		return usecNextThink;

	// Keep sending packets until we run out of tokens
	int nPacketsSent = 0;
	while ( m_pTransport )
	{

		if ( nPacketsSent > k_nMaxPacketsPerThink )
		{
			// We're sending too much at one time.  Nuke token bucket so that
			// we'll be ready to send again very soon, but not immediately.
			// We don't want the outer code to complain that we are requesting
			// a wakeup call in the past
			m_senderState.m_flTokenBucket = m_senderState.m_n_x * -0.0005f;
			return usecNow + 1000;
		}

		// Check if we have anything to send.
		if ( usecNow < m_receiverState.TimeWhenFlushAcks() && usecNow < SNP_TimeWhenWantToSendNextPacket() )
		{

			// We've sent everything we want to send.  Limit our reserve to a
			// small burst overage, in case we had built up an excess reserve
			// before due to the scheduler waking us up late.
			m_senderState.TokenBucket_Limit();
			break;
		}

		// Send the next data packet.
		if ( !m_pTransport->SendDataPacket( usecNow ) )
		{
			// Problem sending packet.  Nuke token bucket, but request
			// a wakeup relatively quick to check on our state again
			m_senderState.m_flTokenBucket = m_senderState.m_n_x * -0.001f;
			return usecNow + 2000;
		}

		// We spent some tokens, do we have any left?
		if ( m_senderState.m_flTokenBucket < 0.0f )
			break;

		// Limit number of packets sent at a time, even if the scheduler is really bad
		// or somebody holds the lock for along time, or we wake up late for whatever reason
		++nPacketsSent;
	}

	// Return time when we need to check in again.
	SteamNetworkingMicroseconds usecNextAction = SNP_GetNextThinkTime( usecNow );
	Assert( usecNextAction > usecNow );
	return usecNextAction;
}

void CSteamNetworkConnectionBase::SNP_TokenBucket_Accumulate( SteamNetworkingMicroseconds usecNow )
{
	// If we're not connected, just keep our bucket full
	if ( !BStateIsConnectedForWirePurposes() )
	{
		m_senderState.m_flTokenBucket = k_flSendRateBurstOverageAllowance;
		m_senderState.m_usecTokenBucketTime = usecNow;
		return;
	}

	float flElapsed = ( usecNow - m_senderState.m_usecTokenBucketTime ) * 1e-6;
	m_senderState.m_flTokenBucket += (float)m_senderState.m_n_x * flElapsed;
	m_senderState.m_usecTokenBucketTime = usecNow;

	// If we don't currently have any packets ready to send right now,
	// then go ahead and limit the tokens.  If we do have packets ready
	// to send right now, then we must assume that we would be trying to
	// wakeup as soon as we are ready to send the next packet, and thus
	// any excess tokens we accumulate are because the scheduler woke
	// us up late, and we are not actually bursting
	if ( SNP_TimeWhenWantToSendNextPacket() > usecNow )
		m_senderState.TokenBucket_Limit();
}

void SSNPReceiverState::QueueFlushAllAcks( SteamNetworkingMicroseconds usecWhen )
{
	DebugCheckPackGapMap();

	Assert( usecWhen > 0 ); // zero is reserved and should never be used as a requested wake time

	// if we're already scheduled for earlier, then there cannot be any work to do
	auto it = m_mapPacketGaps.end();
	--it;
	if ( it->second.m_usecWhenAckPrior <= usecWhen )
		return;
	it->second.m_usecWhenAckPrior = usecWhen;

	// Nothing partial scheduled?
	if ( m_itPendingAck == it )
		return;

	if ( m_itPendingAck->second.m_usecWhenAckPrior >= usecWhen )
	{
		do
		{
			m_itPendingAck->second.m_usecWhenAckPrior = INT64_MAX;
			++m_itPendingAck;
		} while ( m_itPendingAck != it );
		DebugCheckPackGapMap();
	}
	else
	{
		// Maintain invariant
		while ( (--it)->second.m_usecWhenAckPrior >= usecWhen )
			it->second.m_usecWhenAckPrior = usecWhen;
		DebugCheckPackGapMap();
	}
}

#if STEAMNETWORKINGSOCKETS_SNP_PARANOIA > 1
void SSNPReceiverState::DebugCheckPackGapMap() const
{
	int64 nPrevEnd = 0;
	SteamNetworkingMicroseconds usecPrevAck = 0;
	bool bFoundPendingAck = false;
	for ( auto it: m_mapPacketGaps )
	{
		Assert( it.first > nPrevEnd );
		if ( it.first == m_itPendingAck->first )
		{
			Assert( !bFoundPendingAck );
			bFoundPendingAck = true;
			if ( it.first < INT64_MAX )
				Assert( it.second.m_usecWhenAckPrior < INT64_MAX );
		}
		else if ( !bFoundPendingAck )
		{
			Assert( it.second.m_usecWhenAckPrior == INT64_MAX );
		}
		else
		{
			Assert( it.second.m_usecWhenAckPrior >= usecPrevAck );
		}
		usecPrevAck = it.second.m_usecWhenAckPrior;
		if ( it.first == INT64_MAX )
		{
			Assert( it.second.m_nEnd == INT64_MAX );
		}
		else
		{
			Assert( it.first < it.second.m_nEnd );
		}
		nPrevEnd = it.second.m_nEnd;
	}
	Assert( nPrevEnd == INT64_MAX );
}
#endif

SteamNetworkingMicroseconds CSteamNetworkConnectionBase::SNP_TimeWhenWantToSendNextPacket() const
{
	// We really shouldn't be trying to do this when not connected
	if ( !BStateIsConnectedForWirePurposes() )
	{
		AssertMsg( false, "We shouldn't be asking about sending packets when not fully connected" );
		return k_nThinkTime_Never;
	}

	// Reliable triggered?  Then send it right now
	if ( !m_senderState.m_listReadyRetryReliableRange.empty() )
		return 0;

	// Anything queued?
	SteamNetworkingMicroseconds usecNextSend;
	if ( m_senderState.m_messagesQueued.empty() )
	{

		// Queue is empty, nothing to send except perhaps nacks (below)
		Assert( m_senderState.PendingBytesTotal() == 0 );
		usecNextSend = INT64_MAX;
	}
	else
	{

		// FIXME acks, stop_waiting?

		// Have we got at least a full packet ready to go?
		if ( m_senderState.PendingBytesTotal() >= m_cbMaxPlaintextPayloadSend )
			// Send it ASAP
			return 0;

		// We have less than a full packet's worth of data.  Wait until
		// the Nagle time, if we have one
		usecNextSend = m_senderState.m_messagesQueued.m_pFirst->SNPSend_UsecNagle();
	}

	// Check if the receiver wants to send a NACK.
	usecNextSend = std::min( usecNextSend, m_receiverState.m_itPendingNack->second.m_usecWhenOKToNack );

	// Return the earlier of the two
	return usecNextSend;
}

SteamNetworkingMicroseconds CSteamNetworkConnectionBase::SNP_GetNextThinkTime( SteamNetworkingMicroseconds usecNow )
{
	// We really shouldn't be trying to do this when not connected
	if ( !BStateIsConnectedForWirePurposes() )
	{
		AssertMsg( false, "We shouldn't be trying to think SNP when not fully connected" );
		return k_nThinkTime_Never;
	}

	// We cannot send any packets if we don't have transport
	if ( !m_pTransport )
		return k_nThinkTime_Never;

	// Start with the time when the receiver needs to flush out ack.
	SteamNetworkingMicroseconds usecNextThink = m_receiverState.TimeWhenFlushAcks();

	// Check retransmit timers.  If they have expired, this will move reliable
	// segments into the "ready to retry" list, which will cause
	// TimeWhenWantToSendNextPacket to think we want to send data.  If nothing has timed out,
	// it will return the time when we need to check back in.  Or, if everything is idle it will
	// return "never" (very large number).
	SteamNetworkingMicroseconds usecNextRetry = SNP_SenderCheckInFlightPackets( usecNow );

	// If we want to send packets, then we might need to wake up and take action
	SteamNetworkingMicroseconds usecTimeWantToSend = SNP_TimeWhenWantToSendNextPacket();
	usecTimeWantToSend = std::min( usecNextRetry, usecTimeWantToSend );
	if ( usecTimeWantToSend < usecNextThink )
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

		// Time when we will next send is the greater of when we want to and when we can
		usecNextSend = std::max( usecNextSend, usecTimeWantToSend );

		// Earlier than any other reason to wake up?
		usecNextThink = std::min( usecNextThink, usecNextSend );
	}

	return usecNextThink;
}

void CSteamNetworkConnectionBase::SNP_PopulateDetailedStats( SteamDatagramLinkStats &info )
{
	info.m_latest.m_nSendRate = SNP_ClampSendRate();
	info.m_latest.m_nPendingBytes = m_senderState.m_cbPendingUnreliable + m_senderState.m_cbPendingReliable;
	info.m_lifetime.m_nMessagesSentReliable    = m_senderState.m_nMessagesSentReliable;
	info.m_lifetime.m_nMessagesSentUnreliable  = m_senderState.m_nMessagesSentUnreliable;
	info.m_lifetime.m_nMessagesRecvReliable    = m_receiverState.m_nMessagesRecvReliable;
	info.m_lifetime.m_nMessagesRecvUnreliable  = m_receiverState.m_nMessagesRecvUnreliable;
}

void CSteamNetworkConnectionBase::SNP_PopulateQuickStats( SteamNetworkingQuickConnectionStatus &info, SteamNetworkingMicroseconds usecNow )
{
	info.m_nSendRateBytesPerSecond = SNP_ClampSendRate();
	info.m_cbPendingUnreliable = m_senderState.m_cbPendingUnreliable;
	info.m_cbPendingReliable = m_senderState.m_cbPendingReliable;
	info.m_cbSentUnackedReliable = m_senderState.m_cbSentUnackedReliable;
	if ( GetState() == k_ESteamNetworkingConnectionState_Connected )
	{

		// Accumulate tokens so that we can properly predict when the next time we'll be able to send something is
		SNP_TokenBucket_Accumulate( usecNow );

		//
		// Time until we can send the next packet
		// If anything is already queued, then that will have to go out first.  Round it down
		// to the nearest packet.
		//
		// NOTE: This ignores the precise details of SNP framing.  If there are tons of
		// small packets, it'll actually be worse.  We might be able to approximate that
		// the framing overhead better by also counting up the number of *messages* pending.
		// Probably not worth it here, but if we had that number available, we'd use it.
		int cbPendingTotal = m_senderState.PendingBytesTotal() / m_cbMaxMessageNoFragment * m_cbMaxMessageNoFragment;

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

			info.m_usecQueueTime = (int64)cbPendingTotal * k_nMillion / SNP_ClampSendRate();
		}
	}
	else
	{
		// We'll never be able to send it.  (Or, we don't know when that will be.)
		info.m_usecQueueTime = INT64_MAX;
	}
}

} // namespace SteamNetworkingSocketsLib
