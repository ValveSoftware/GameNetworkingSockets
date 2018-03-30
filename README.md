GameNetworkingSockets
---

GameNetworkingSockets is a basic transport layer for games.  The features are:

* Connection-oriented protocol (like TCP)
* ... but message-oriented instead of stream-oriented
* Mix of reliable and unreliable messages
* Messages can be larger than underlying MTU, the protocol performs fragmentation and reassembly, and retransmission for reliable
* Bandwidth estimation based on TCP-friendly rate control (RFC 5348)
* Encryption.  AES per packet, Ed25519 crypto for key exchange and cert signatures.  The details for shared key derivation and per-packet IV are based on Google QUIC.
* Tools for simulating loss and detailed stats measurement

The main interface class is named SteamNetworkingSockets, and many files have "steam" in their name.
But *Steam is not needed*.  The reason for the name is that this provides a subset of the functionality of the API with the same name in the SteamworksSDK.  The intention is that on PC you can use the Steamworks version, and on other platforms, you can use this version.  In this way, you can avoid having the Steam version be "weird" or not take full advantage of the features above that it provides.

But even if you don't make games or aren't on Steam, feel free to use this code for whatever purpose you want.

## Building

### Dependencies

* OpenSSL
* Google protobuf
* ujson.  A small JSON parser, with DOM storage using STL.  The project seems to have been abandoned, as our pull requests have been ignored.  The code is included in this project, with some of our own fixes and changes.
* ed25519-donna and curve25519-donna.  We've made some minor changes, so the source is included in this project.

### Linux

This has only really been tested on Ubuntu 17.

```
meson build src
```

### Work in progress!

We're still in the process of extracting the code from our proprietary build toolchain and making everything more open-source friendly.  Bear with us.

* The code in this form is only known to compile on Ubuntu 17.  (Although this code has shipped through our toolchain on Win32/Win64 and OSX as well and also compiles for several Android flavors).
* We don't provide any Windows project files or any straightforward method for building on windows yet.
* There is a unit test, but it's not currently included and we don't have it working in any standard framework.

## Roadmap
Here are some areas where we're working on improvement

### Reliability layer improvements
We have a new version of the "SNP" code in progress.  (This is the code that takes API messages and puts them into UDP packets.  Long packets are fragmented and reassembled, short messages can be combined, and lost fragments of reliable messages are retransmitted.)
* The wire format framing is rather....prodigious.
* The reliability layer is a pretty naive sliding window implementation.
* The reassembly layer is likewise pretty naive.  Out-of-order packets are totally discarded, which can be catastrophic for certain patterns of traffic over, e.g. DSL lines.

### Abstract SteamIDs to generic "identity"
We'd like to generalize the concept of an identity.  Basically anywhere you see CSteamID, it would be good to enable the use of a more generic identity structure.

### OpenSSL bloat
Our use of OpenSSL is extremely limited; basically just AES encryption.  We use Ed25519 keys for signatures and key exchange and we do not support X.509 certificates.  However, because the code is going through a wrapper layer that is part of Steam, we are linking in much more code than strictly necessary.  And each time we encrypt and decrypt a packet, this wrapper layer is doing some work which could be avoided.
