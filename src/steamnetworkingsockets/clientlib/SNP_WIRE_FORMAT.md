## SNP frames

SNP encrypted payload is a sequence of frames.  Each frame begins with an 8-bit
frame type / flags field.

### Unreliable message segment

    00emosss [message_num] [offset] [size] data

    e: 0: More message data after this segment.
       1: Message ends at the end of this segment.
    m: encoded size of message_num
       First segment in packet: message_num is absolute.  Only bottom N bits are sent.
           0: 16-bits
           1: 32-bits
       Subsequent segments: message number field is relative to previous
           0: no message number field follows, assume 1 greater than previous segment
           1: Var-int encoded offset from previous follows
           (NOTE: while encoding/decoding a packet, any reliable segment frames sent after unreliable data
           will *also* increment the current message number, even though the message number is *not*
           guaranteed to match that reliable segment.  Since in practice the message number often will
           match, making this encode/decode rule affords a small optimization.)
    o:  offset of this segment within message
        If first segment in packet, or message number differs from previous segment in packet:
            0: Zero offset, segment is first in message.  No offset field follows.
            1: varint-encoded offset follows
    sss: Size of data
        000-100: Append upper three bits to lower 8 bits in explicit size field,
                 which follows  (Max value is 0x4ff = 1279, which is larger than our MTU)
        101,110: Reserved
        111: This is the last frame, so message data extends to the end of the packet.

### Reliable message segment

    010mmsss [stream_pos] [size] data

    mm: encoded size of stream_pos
        First reliable segment in packet: stream_pos is absolute.  Only bottom N bits are sent.
            00: 24-bits
            01: 32-bits
            10: 48-bits
            11: Reserved
        Subsequent reliable segments: stream_pos field is relative to end of previous reliable segment
            00: assume segment immediately follows previous reliable segment in stream.
                (If two small reliable messages appear in sequence, we can combine them into a
                single segment, so this shortcut would not be needed.  When it is useful is when
                a small unreliable segment appears between two consecutive reliable segments.)
            01: 8-bit offset from previous
            10: 16-bit offset from previous
            11: 32-bit offset from previous
        NOTE: Stream position 0 is reserved.  The first reliable byte is actually at position 1.
    sss: Size of data
        000-100: Append upper three bits to lower 8 bits in explicit size field,
                 which follows  (Max value is 0x4ff = 1279, which is larger than our MTU)
        101,110: Reserved
        111: This is the last frame, so message data extends to the end of the packet.

### Stop waiting

Meaning: "Stop acking packets older than this_pkt_num - pkt_num_offset - 1".
Concept comes from Google QUIC

Stop waiting segments are encoded as follows:

          100000ww pkt_num_offset

          ww: size of pkt_num_offset:
              00: 8-bit offset
              01: 16-bit offset
              10: 24-bit offset
              11: 64-bit offset

### Ack

Meaning: "Here's the latest packet number I have received from you, and how long ago I received it.
Also, here's an RLE-encoded bitfield of recent packets, describing which I have received."

Concept comes from Google QUIC.  The eventual goal is for a sender to know the fate (whether or
it was received) of every single packet that it sends.  (!)  However, the sender's understanding
may include some false NACKs, if packets arrive out of order.  For example, if a receiver observes
a gap in the packet numbers, he will NACKs the skipped packets to the sender.  However, subsequently
he may receive the "lost" packet.  It MUST be safe for the receiver to go ahead and process the packet,
even though it has NACKed it.  The protocol is designed to work even if the sender is "wrong"
about the fate of this packet and acts on that incorrect information.  E.g. if the sender retransmits
data in the packet believed to be lost, the redundant data must be discarded by the receiver.  Or if
the packet contained unreliable messages, the sender may incorrectly believe that the message was lost,
when in fact it was delivered.

Compared to simple sliding window schemes, this provides the sender with far more visibility into
the receiver's state, and thus when there is loss, only specific lost packets need to be retransmitted,
instead of rewinding all the way back to the point of loss.  This also serves as loss feedback for
TCP-friendly bandwidth estimation.

All packets between the latest stop_waiting packet number and latest_received_pkt_num (inclusive)
will be accounted for.  Specifically, any packet after the latest received latest_received_pkt_num
and the earliest ack block (if any) are being acknowledged, even though the "starting" point is not
specifically included!  This works because the sender (in this case, I mean the peer who is processing
the ack frame, whose packets are being acked) never decreases the stop_waiting packet number.
And so when the sender receives an ack frame, the frame may cover packets that are older than the most
recently sent stop_waiting message (in which case they will be discarded, since the sender should already
know the fate of those packets, or has already arranged for any necessary retransmission).

If packet loss is such that the ack/nack record is badly fragmented, it may not be possible
to give a full account of all packets between stop_waiting and latest_received_pkt_num in a
reasonable number of blocks.  In this case, the receiver (the peer encoding these acks) can simply
encode only the oldest packets that will fit.  Eventually the sender should advance the stop_waiting
packet number past this fragmentation, at which point the receiver can catch up.  In cases of very bad
fragmentation we will probably end up slowing down the packet rate, due to the packet loss causing the
fragmentation, anyway.

Ack frames are encoded as follows:

    1001wnnn latest_received_pkt_num latest_received_delay [N] [ack_block_0 ... ack_block_N]

    w: size of latest_received_pkt_num
        0=16-bit
        1=32-bit
    nnn: number of blocks in this frame.
        000-110: use this number
        111: number of blocks is >7, explicit count byte is present.

latest_received_delay measures how long since I received that packet number until I sent the
packet containing these acks.  It is 16-bit value on a scale of 1=32usec.  Thus the largest
delay that can be encoded is 65,535 * 32 is a little over 2 seconds.  However, the max value of
65,535 is reserved to indicate that value is missing/invalid.  In general, since the sender
remembers the time each outbound packet is sent, the combination of the sequence number and
delay can be used to estimate the ping.

Ack blocks work backwards from the most recent towards older packets, and essentially
run-length-encode the sequence of received/not-received.  Each block conceptually
contains a count of packets being acked, and a count of packets that have not (yet)
been received.

The "acks" for a block are for newer (greater numbered) packets than the nacks in
the same block.

Each block is encoded as as follows:

    aaaannnn [num_ack] [num_nack]

    aaaa: number of consecutive packets being acked
          0000-0111: run length is directly encoded here, no explicit count follows
          1xxx: xxx are lower 3 bits.  Upper bits are var-int encoded and follow.
    nnnn: same as aaaa, but for nacks

Note that except for the oldest (serialized last) ack block, if should never be necessary
to encode a zero for any of these counts.  (FIXME - we could make a micro-optimization
here to take advantage of this, although it would add a slight complication.)

### Reserved lead bytes

    100001xx
    10001xxx
    101xxxxx
    11xxxxxx

## Reliable stream message framing

Unreliable segments represent a piece of a *message*, and do not span
messages.  Reliable segments represent a chunk of an abstract reliable
stream (much like TCP), and do not necessarily begin or end at message
boundaries, although certainly it is common for this to be the case.
Additional framing exists within this abstract stream to mark the boundaries
between messages and facilitate reconstruction of message numbers.

Each reliable message is prefixed with a flags byte and optional fields:

    0mssssss [msg_num] [msg_size]

    m: Message number:
        0: msg_num not present, assume it's 1 greater than previous
        1: var-int increment from previous reliable message number follows
    ssssss: Message size:
        000000-011111: msg_size not present, these bits directly encode size
        1xxxxx: xxxxx are lower bits.  Upper bits follow var-int encoded

Lead bytes with the high bit set are reserved for future expansion.
