# GameNetworkingSockets [![Build Status](https://github.com/ValveSoftware/GameNetworkingSockets/workflows/CI/badge.svg)](https://github.com/ValveSoftware/GameNetworkingSockets/actions)

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
* Tools for simulating packet latency/loss, and detailed stats measurement
* Head-of-line blocking control and bandwidth sharing of multiple message
  streams ("lanes") on the same connection.  You can use strict priority
  values, softer [weight values](https://en.wikipedia.org/wiki/Weighted_fair_queueing)
  that control how bandwidth is shared, or some combination of the two methods.
  See [``ISteamNetworkingSockets::ConfigureConnectionLanes``](include/steam/isteamnetworkingsockets.h).
* IPv6 support
* Peer-to-peer networking:
  * NAT traversal through google WebRTC's ICE implementation.
  * Plug in your own signaling service.
  * Unique "symmetric connect" mode.
  * [``ISteamNetworkingMessages``](include/steam/isteamnetworkingmessages.h) is an
    interface designed to make it easy to port UDP-based code to P2P use cases.  (By
    UDP-based, we mean non-connection-oriented code, where each time you send a
    packet, you specify the recipient's address.)
  * See [README_P2P.md](README_P2P.md) for more info
* Cross platform.  This library has shipped on consoles, mobile platforms, and non-Steam
  stores, and has been used to facilitate cross-platform connectivity.  Contact us to get
  access to the code.  (We are not allowed to distribute it here.)

What it does *not* do:

* Higher level serialization of entities, delta encoding of changed state
  variables, etc
* Compression

## Quick API overview

To get an idea of what the API is like, here are a few things to check out:

* The [include/steam](include/steam) folder has the public API headers.
  * [``ISteamNetworkingSockets``](include/steam/isteamnetworkingsockets.h) is the
    most important interface.
  * [``steamnetworkingtypes.h``](include/steam/steamnetworkingtypes.h) has misc
    types and declarations.
* The
  [Steamworks SDK documentation](https://partner.steamgames.com/doc/api/ISteamNetworkingSockets)
  offers web-based documentation for these APIs.  Note that some features
  are only available on Steam, such as Steam's authentication service,
  signaling service, and the SDR relay service.
* Look at these examples:
  * [example_chat.cpp](examples/example_chat.cpp).  Very simple client/server
    program using all reliable messages over ordinary IPv4.
  * [test_p2p.cpp](tests/test_p2p.cpp).  Shows how to get two hosts to connect
    to each other using P2P connectivity.  Also an example of how to write a
    signaling service plugin.

## Building

See [BUILDING](BUILDING.md) for more information.

## Language bindings

The library was written in C++, but there is also a plain C interface
to facilitate binding to other languages.

Third party language bindings:

* C#:
  * <https://github.com/nxrighthere/ValveSockets-CSharp>
  * <https://github.com/Facepunch/Facepunch.Steamworks>
* Go:
  * <https://github.com/nielsAD/gns/>

## Why do I see "Steam" everywhere?

The main interface class is named SteamNetworkingSockets, and many files have
"steam" in their name.  But *Steam is not needed*.  If you don't make games or
aren't on Steam, feel free to use this code for whatever purpose you want.

The reason for "Steam" in the names is that this provides a subset of the
functionality of the [API](https://partner.steamgames.com/doc/api/ISteamNetworkingSockets)
with the same name in the Steamworks SDK.  Our main
reason for releasing this code is so that developers won't have any hesitation
coding to the API in the Steamworks SDK.  On Steam, you will link against the
Steamworks version, and you can access the additional services provided by
the [Steam Datagram Relay](https://partner.steamgames.com/doc/features/multiplayer/steamdatagramrelay)
network.  On other platforms and stores, as long as you ship a version of your
game on Steam, you might be able to take advantage of these services.  See
the Steamworks documentation for more information.  Because this is a live
service, and we need to control our security and backward compatibility burden,
at this time we are not able to offer access to SDR on other platforms to all
partners.

If you aren't a Steam partner, or don't have a version of your game on Steam,
then use this opensource version of the API and take advantage of the permissive
license to do whatever you want.  We want you to take maximum advantage of the
features in the Steamworks version.  That won't happen if this API is a weird
"wart" that's hidden behind `#ifdef STEAM`, which is why we're making this
opensource version available.

The desire to match the Steamworks SDK also explains a somewhat anachronistic
coding style and weird directory layout.  This project is kept in sync with the
Steam code here at Valve.  When we extracted the code from the much larger
codebase, we had to do some relatively gross hackery.  The files in folders
named  `tier0`, `tier1`, `vstdlib`, `common`, etc have especially suffered
trauma.  Also if you see code that appears to have unnecessary layers of
abstraction, it's probably because those layers are needed to support relayed
connection types or some part of the Steamworks SDK.

## Security

Did you find a security vulnerability?  Please inform us responsibly; you may
be eligible for a bug bounty.  See the [security policy](SECURITY.md) for more
information.
