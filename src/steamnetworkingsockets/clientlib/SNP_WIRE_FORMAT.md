## SNP frames

SNP encrypted payload is a sequence of frames.  Each frame begins with an 8-bit
frame type / flags field.

### Unreliable message segment

Encodes a segment of an unreliable message.  (Often, an entire message.)

    00emosss [message_num] [offset] [size] data

    e: 0: There's more data after ths in the unreliable message.
          (Will be sent in another packet.)
       1: This is the last segment in the unreliable message.
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

Encodes a segment of the reliable stream.

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

Meaning: "Stop acking packets older than `this_pkt_num - pkt_num_offset - 1`".

Stop waiting frames are encoded as follows:

    100000ww pkt_num_offset

     ww: size of pkt_num_offset:
         00: 8-bit offset
         01: 16-bit offset
         10: 24-bit offset
         11: 64-bit offset

### Ack

Meaning: "Here's the latest packet number I have received from you, and how long ago I received it.
Also, here's an RLE-encoded bitfield of recent packets, describing which I have received."

A quick word on terminology used here: "sender" refers to a peer's role in sending actual *data*
and the "stop waiting" frame, but not acks.  "Receiver" refers to a peer's role in sending the *acks*.
Each side is actually both a sender and receiver, but the message data flow in one direction is
essentially unrelated to the flow in the opposite direction, other than the fact that a peer will
try to send data as a "sender" and acks as a "receiver" in the same packet.  For purposes of
analyzing the protocol we ignore this symmetry, and focus on a single flow.  Thus, ack frames are
actually sent by the "receiver" and received by the "sender"!  We'll use the words "encode" and
"decode" below to try to minimize confusion.  Also, note that all packet numbers here are those
assigned by the sender.

The eventual goal is for a sender to know the fate of every single packet that it sends.  Since
packets can arrive late or out of order, and there is transmission delay for the acks,
the sender's understanding of the situation is not perfect: it may include some false NACKs.
For example, if the receiver observes a gap in the packet numbers, he will NACKs the skipped
packets to the sender.  However, subsequently he may receive the "lost" packets.  It MUST be
safe for the receiver to go ahead and process the packets, even though he NACKed them.  The
protocol is designed to work even if the sender is wrong about the fate of this packet, and
acts on that incorrect information.  If the packet that was presumed lost contained reliable
segments, this means that the sender may unnecessarily retransmit those reliable segments
unnecessarily.  This redundant data must be discarded by the receiver.  If the presumed lost
packet contained unreliable message(s), the sender may believe that those messages were not
delivered, when in fact they were.  However, the sender will never mistakenly believe that
a packet has been delivered when it has not.  Note that packet numbers are strictly
increasing, and any retransmissions will be assigned a new packet number by the sender.
The sender is free to retransmit reliable data on different segment boundaries; it is not
restricted to retransmitting identical packets or segments.

Compared to simple sliding window schemes, this provides the sender with far more visibility into
the receiver's state, and thus when there is loss, only specific lost packets need to be retransmitted,
instead of rewinding all the way back to the point of loss.  This also serves as loss feedback for
TCP-friendly bandwidth estimation.

Ack frames are encoded as follows:

    1001wnnn latest_received_pkt_num latest_received_delay [N] [ack_block_0 ... ack_block_N]

    w: size of latest_received_pkt_num
        0=16-bit
        1=32-bit
    nnn: number of blocks in this frame.
        000-110: use this number
        111: number of blocks is >6, explicit count byte N is present.  (Max 255 blocks)

`latest_received_delay` measures the delay between the receiver receiving the packet
numbered `latest_received_pkt_num` and sending these acks.  It is 16-bit value on a scale
of 1=32usec.  Thus the largest delay that can be encoded is 65,535 x 32usec = a little
over 2 seconds.  Since the sender remembers the time each outbound packet is sent,
the combination of the packet number and delay means that every ack frame also serves
as a ping reply, so that the RTT can be continuously monitored.    However, the
max value of 65,535 is reserved to indicate that the value is intentionally missing
or invalid and no timing information should be inferred.

Ack blocks are encoded working backwards, starting from the most recent packets,
towards older packets.  This is essentially a run-length encoding of a bitfield
describing the ACK or NACK state of each packet.  Each block conceptually contains
a count of packets being acked, and a count of packets that have not (yet) been received.
The ACKs for a block are for newer (greater numbered) packets, and the NACKed
range in the same block is for the older packets.

Each ack block is encoded as as follows:

    aaaannnn [num_ack] [num_nack]

    aaaa: number of consecutive packets being acked
          0000-0111: run length is directly encoded here, no explicit count follows
          1xxx: xxx are lower 3 bits.  Upper bits are var-int encoded and follow.
    nnnn: same as aaaa, but for nacks

When a receiver encodes an ack frame, it MUST account for all packets between the latest
`stop_waiting` packet number that it has received and the `latest_received_pkt_num` in the
frame (inclusive).  For this purpose, he MUST NOT use a value of `stop_waiting` larger
than the larger received value from the sender.  But the receiver MAY use
a value of `latest_received_pkt_num` that is lower (older) than the latest packet number
actually received.  In particular, this may be necessary if packet loss is such that the
ack/nack record is badly fragmented and there is not enough space to give a full account
of all the packets starting with `stop_waiting` up to the latest packet actually received.
In this case, the receiver can simply encode only the oldest packets that will fit, and not
ack (for now) the newer packets after the value of `latest_received_pkt_num` encoded
in the frame.  Eventually the sender will advance the `stop_waiting` packet number past
this fragmentation, at which point the receiver can stop acking the fragmented region of the
packet number address space, and can catch up.  In cases of very bad fragmentation, we will
probably end up slowing down the packet rate anyway, due to the packet loss that is causing the
fragmentation.

When the sender decodes the frame, it doesn't actually know what `stop_waiting` value the receiver
has received, but it does know the latest `stop_waiting` value that it has sent, which cannot be
lower (older) than the one that was used by the receiver to encode the packet.  This is a key
guarantee that allows the receiver to avoid encoding acks between `stop_waiting` and the first
loss, and for the sender to implicitly mark those packets as acked when decoding.  In particular,
in the (hopefully!) common case where no packets have been lost in the range being accounted for,
no blocks need be encoded at all.

(FIXME - based on the above description, it's actually never necessary to encode a zero.
So should we then always encode the number - 1?  Saving one byte in the case of a run of 8 dropped
packets?)

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
