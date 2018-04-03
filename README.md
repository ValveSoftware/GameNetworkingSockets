GameNetworkingSockets
---

GameNetworkingSockets is a basic transport layer for games.  The features are:

* Connection-oriented protocol (like TCP)
* ... but message-oriented (like UDP)
* Supports both reliable and unreliable message types
* Messages can be larger than underlying MTU, the protocol performs
  fragmentation and reassembly, and retransmission for reliable messages
* Bandwidth estimation based on TCP-friendly rate control (RFC 5348)
* Encryption. AES per packet, Ed25519 crypto for key exchange and cert
  signatures. The details for shared key derivation and per-packet IV are
  based on the [design](https://docs.google.com/document/d/1g5nIXAIkN_Y-7XJW5K45IblHd_L2f5LTaDUDwvZ5L6g/edit?usp=sharing)
  used by Google's QUIC protocol.
* Tools for simulating loss and detailed stats measurement

The main interface class is named SteamNetworkingSockets, and many files have
"steam" in their name.  But *Steam is not needed*.  The reason for the name is
that this provides a subset of the functionality of the API with the same name
in the SteamworksSDK.  The intention is that on PC you can use the Steamworks
version, and on other platforms, you can use this version.  In this way, you
can avoid having the Steam version be "weird" or not take full advantage of the
features above that it provides.

But even if you don't make games or aren't on Steam, feel free to use this code
for whatever purpose you want.

## Building

### Dependencies

* OpenSSL
* Google protobuf
* ed25519-donna and curve25519-donna.  We've made some minor changes, so the
  source is included in this project.

### Linux

This has only really been tested on Ubuntu 17.10.

```
$ meson . build
$ ninja -C build
```

### MSYS2

You can also build this project on [MSYS2](https://www.msys2.org). First,
follow the [instructions](https://github.com/msys2/msys2/wiki/MSYS2-installation) on the
MSYS2 website for updating your MSYS2 install.

Next install the dependencies for building GameNetworkingSockets (if you want
a 32-bit build, install the i686 versions of these packages):

```
$ pacman -S \
    git \
    mingw-w64-x86_64-protobuf \
    mingw-w64-x86_64-meson \
    mingw-w64-x86_64-openssl \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-pkg-config
```

And finally, clone the repository and build it:

```
$ git clone https://github.com/ValveSoftware/GameNetworkingSockets.git
$ cd GameNetworkingSockets
$ meson . build
$ ninja -C build
```

**NOTE:** When building with MSYS2, be sure you launch the correct version of
the MSYS2 terminal, as the three different Start Menu entries will give you
different environment variables that will affect the build. You should run the
Start Menu item named `MSYS2 MinGW 64-bit` or `MSYS2 MinGW 32-bit`, depending
on the packages you've installed and what architecture you want to build
GameNetworkingSockets for.

### Work in progress!

We're still in the process of extracting the code from our proprietary build
toolchain and making everything more open-source friendly.  Bear with us.

* The code in this form is only known to compile on Ubuntu 17.10.  (Although
  this code has shipped through our toolchain on Win32/Win64 and OSX as well
  and also compiles for several Android flavors).
* We don't provide any Windows project files or any straightforward method for
  building on windows yet.
* The unit test compiles, but has some issues.  And we don't have it working
  in any standard framework.
* We don't have a good, simple client/server example of how to use the code.
  (The unit test is not a good example, please don't cut and paste it.)

## Roadmap
Here are some areas where we're working on improvement

### Reliability layer improvements
We have a new version of the "SNP" code in progress.  (This is the code that
takes API messages and puts them into UDP packets.  Long packets are fragmented
and reassembled, short messages can be combined, and lost fragments of reliable
messages are retransmitted.)

* The wire format framing is rather....prodigious.
* The reliability layer is a pretty naive sliding window implementation.
* The reassembly layer is likewise pretty naive.  Out-of-order packets are
  totally discarded, which can be catastrophic for certain patterns of traffic
  over, e.g. DSL lines.

### Abstract SteamIDs to generic "identity"
We'd like to generalize the concept of an identity.  Basically anywhere you see
CSteamID, it would be good to enable the use of a more generic identity
structure.

### OpenSSL bloat
Our use of OpenSSL is extremely limited; basically just AES encryption.  We use
Ed25519 keys for signatures and key exchange and we do not support X.509
certificates.  However, because the code is going through a wrapper layer that
is part of Steam, we are linking in much more code than strictly necessary.
And each time we encrypt and decrypt a packet, this wrapper layer is doing some
work which could be avoided.
