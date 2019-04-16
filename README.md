GameNetworkingSockets
---
[![Build Status](https://travis-ci.org/ValveSoftware/GameNetworkingSockets.svg?branch=master)](https://travis-ci.org/ValveSoftware/GameNetworkingSockets)

GameNetworkingSockets is a basic transport layer for games.  The features are:

* Connection-oriented API (like TCP)
* ... but message-oriented (like UDP), not stream-oriented.
* Supports both reliable and unreliable message types
* Messages can be larger than underlying MTU.  The protocol performs
  fragmentation, reassembly, and retransmission for reliable messages.
* A [reliability layer](src/steamnetworkingsockets/clientlib/SNP_WIRE_FORMAT.md)
  significantly more sophisticated than a basic TCP-style sliding window.
  It is based on the "ack vector" model from DCCP (RFC 4340, section 11.4)
  and Google QUIC and discussed in the context of games by
  [Glenn Fiedler](https://gafferongames.com/post/reliable_ordered_messages/).
  The basic idea is for the receiver to efficiently communicate to the sender
  the status of every packet number (whether or not a packet was received
  with that number).  By remembering which segments were sent in each packet,
  the sender can deduce which segments need to be retransmitted.
* Encryption. AES-GCM-256 per packet, [Curve25519](https://cr.yp.to/ecdh.html) for
  key exchange and cert signatures. The details for shared key derivation and
  per-packet IV are based on the [design](https://docs.google.com/document/d/1g5nIXAIkN_Y-7XJW5K45IblHd_L2f5LTaDUDwvZ5L6g/edit?usp=sharing)
  used by Google's QUIC protocol.
* Tools for simulating loss and detailed stats measurement
* IPv6

What it does *not* do:

* Higher level serialization of entities, delta encoding of changed state
  variables, etc
* Compression

### Why do I see "Steam" everywhere?

The main interface class is named SteamNetworkingSockets, and many files have
"steam" in their name.  But *Steam is not needed*.  If you don't make games or
aren't on Steam, feel free to use this code for whatever purpose you want.

The reason for "Steam" in the names is that this provides a subset of the
functionality of the [API](https://partner.steamgames.com/doc/api/ISteamNetworkingSockets)
with the same name in the Steamworks SDK.  Our main
reason for releasing this code is so that developers won't have any hesitation
coding to the API in the Steamworks SDK.  On Steam, you will link against the
Steamworks version, and you can get the additional features there (access to
the relay network).  And on other platforms, you can use this version, which
has the same names for everything, the same semantics, the same behavioural
quirks.  We want you to take maximum advantage of the features in the
Steamworks version, and that won't happen if the Steam code is a weird "wart"
that's hidden behind `#ifdef STEAM`.

The desire to match the Steamworks SDK also explains a somewhat anachronistic
coding style and weird directory layout.  This project is kept in sync with the
Steam code here at Valve.  When we extracted the code from the much larger
codebase, we had to do some relatively gross hackery.  The files in folders
named  `tier0`, `tier1`, `vstdlib`, `common`, etc have especially suffered
trauma.  Also if you see code that appears to have unnecessary layers of
abstraction, it's probably because those layers are needed to support relayed
connection types or some part of the Steamworks SDK.

### Building

See [BULDING](BUILDING.md) for more information.

### Language bindings

The library was written in C++, but there is also a plain C interface
to facilitate binding to other languages.

Third party language bindings:

* C#: https://github.com/nxrighthere/ValveSockets-CSharp

## Roadmap

Here are some large features that we expect to add to a future release:

### Bandwidth estimation
An earlier version of this code implemented TCP-friendly rate control (RFC
5348).  But as part of the reliability layer rewrite, bandwidth estimation has
been temporarily broken, and a fixed (configurable) rate is used.  It's not
clear if it's worth the complexity of implementation and testing to get
sender-calculated TCP-friendly rate control implemented, or a simpler method
would do just as good.  Whatever method we use, needs to work even if the app
code inspects the state and decides not to send a message.  In this case, the
bandwidth estimation logic might perceive that the channel is not
"data-limited", when it essentially is.  We could add an entry point to allow
the application to express this, but this is getting complicated, making it more
difficult for app code to do the right thing.  It'd be better if it mostly
"just worked" when app code does the simple thing.

### NAT piercing (ICE/STUN/TURN)
The Steamworks code supports a custom protocol for relaying packets through our
network of relays and on our backbone.  At this time the open-source code does
not have any support for piercing NAT or relaying packets.  But since the
Steamworks code already has those concepts, it should be pretty easy to add
support for this.  You'd still be responsible for running the STUN/TURN servers
and doing the rendezvous/signalling, but the code could use them.

### Non-connection-oriented interface
The Steam version has ISteamMessages, which is a UDP-like interface.  Messages
are addressed by peer identity, not connection handle.  (Both reliable and
unreliable messages are still supported.)  We should open-source this API,
too.  Previously it was only for P2P, but we've found that it's useful for
porting UDP-based code.
