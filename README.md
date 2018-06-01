GameNetworkingSockets
---

GameNetworkingSockets is a basic transport layer for games.  The features are:

* Connection-oriented API (like TCP)
* ... but message-oriented (like UDP), not stream-oriented.
* Supports both reliable and unreliable message types
* Messages can be larger than underlying MTU.  The protocol performs
  fragmentation, reassembly, and retransmission for reliable messages.
* An [ack model](src/steamnetworkingsockets/clientlib/SNP_WIRE_FORMAT.md)
  significantly more sophisticated than a basic TCP-style sliding window.
  It is based on the "ack vector" model from DCCP (RFC 4340, section 11.4)
  and Google QUIC and discussed in the context of games by
  [Glenn Fiedler](https://gafferongames.com/post/reliable_ordered_messages/).
  The basic idea is for the receiver to efficiently communicate to the sender
  the status of every packet number (whether or not a packet was received
  with that number).  By remembering what segments were sent in each packet,
  the sender can deduce which individual segments need to be retransmitted.
* Encryption. AES per packet, Ed25519 crypto for key exchange and cert
  signatures. The details for shared key derivation and per-packet IV are
  based on the [design](https://docs.google.com/document/d/1g5nIXAIkN_Y-7XJW5K45IblHd_L2f5LTaDUDwvZ5L6g/edit?usp=sharing)
  used by Google's QUIC protocol.
* Tools for simulating loss and detailed stats measurement

What it does *not* do:

* Higher level serialization of entities, delta encoding of changed state variables, etc
* Compression

### Why do I see "Steam" everywhere?

The main interface class is named SteamNetworkingSockets, and many files have
"steam" in their name.  But *Steam is not needed*.  If you don't make games or
aren't on Steam, feel free to use this code for whatever purpose you want.

The reason for "Steam" in the names is that this provides a subset of the
functionality of the API with the same name in the Steamworks SDK.  Our main
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

So the code has some style issues that some people probably won't like, but it
does have the advantage of being battle tested over several years and millions
of customers.  We aim to make this lib a solid transport library and we hope
people will use it to ship their games on non-Steam platforms.  If there is
something about this code that makes it awkward to use, or if it doesn't work
properly or you see a security or performance issue, please let us know.

## Building

### Dependencies

* CMake or Meson, and build tool like Ninja, GNU Make or Visual Studio
* OpenSSL
* Google protobuf
* ed25519-donna and curve25519-donna.  We've made some minor changes, so the
  source is included in this project.


#### OpenSSL
If you're building on Linux or Mac, just install the appropriate packages from your package manager.

Ubuntu/Debian:
```
# apt install libssl-dev
```

Arch Linux:
```
# pacman -S openssl
```

Mac OS X, using [Homebrew](https://brew.sh):
```
$ brew install openssl
$ export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/opt/openssl/lib/pkgconfig
```

For MSYS2, see the [MSYS2](#msys2) section. There are packages available in
the MinGW repositories for i686 and x86_64.

For Visual Studio, you can install the [OpenSSL
binaries](https://slproweb.com/products/Win32OpenSSL.html) provided by Shining
Light Productions. The Windows CMake distribution understands how to find the
OpenSSL binaries from these installers, which makes building a lot easier. Be
sure to pick the installers **without** the "Light"suffix. In this instance,
"Light" means no development libraries or headers.


#### protobuf

If you're building on Linux or Mac, just install the appropriate packages from your package manager.

Ubuntu/Debian:
```
# apt install libprotobuf-dev protobuf-compiler
```

Arch Linux:
```
# pacman -S protobuf
```

Mac OS X, using [Homebrew](https://brew.sh):
```
$ brew install protobuf
```

For MSYS2, see the [MSYS2](#msys2) section. There are packages available in
the MinGW repositories for i686 and x86_64.

For Visual Studio, the process is a bit more involved, as you need to compile
protobuf yourself. The process we used is something like this:

```
C:\dev> git clone https://github.com/google/protobuf
C:\dev> cd protobuf
C:\dev\protobuf> git checkout -t origin/3.5.x
C:\dev\protobuf> mkdir cmake_build
C:\dev\protobuf> cd cmake_build
C:\dev\protobuf\cmake_build> vcvarsall amd64
C:\dev\protobuf\cmake_build> cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=c:\sdk\protobuf-amd64 ..\cmake
C:\dev\protobuf\cmake_build> ninja
C:\dev\protobuf\cmake_build> ninja install
```


### Linux

If you already have the dependencies installed (see above sections), then you
should be able to build fairly trivially.

Using Meson:

```
$ meson . build
$ ninja -C build
```

Or CMake:

```
$ mkdir build
$ cd build
$ cmake -G Ninja ..
$ ninja
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
the MSYS2 terminal, as the three different Start menu entries will give you
different environment variables that will affect the build.  You should run the
Start menu item named `MSYS2 MinGW 64-bit` or `MSYS2 MinGW 32-bit`, depending
on the packages you've installed and what architecture you want to build
GameNetworkingSockets for.


### Visual Studio

When configuring GameNetworkingSockets using CMake, you need to add the protobuf bin dir to your path in order to help CMake figure out the protobuf installation prefix:
```
C:\dev\GameNetworkingSockets> mkdir build
C:\dev\GameNetworkingSockets> cd build
C:\dev\GameNetworkingSockets\build> set PATH=%PATH%;C:\sdk\protobuf-amd64\bin
C:\dev\GameNetworkingSockets\build> vcvarsall amd64
C:\dev\GameNetworkingSockets\build> cmake -G Ninja ..
C:\dev\GameNetworkingSockets\build> ninja
```


## Work in progress!

We're still in the process of extracting the code from our proprietary build
toolchain and making everything more open-source friendly.  Bear with us.

* The test compiles and runs, but generates lots of spew that looks like bugs
  but is actually normal.  We also don't have it working in any standard framework.
  It isn't really a narrowly targeted unit test, it is designed to exercise
  almost all of the reliability, rate limiting, and serialization layers, by
  connecting two peers and enabling some paket loss and reordering, and then
  delivering a bunch of random-sized packets.  Exercising specific patterns
  of packet misdelivery and checking the exact behaviour more strictly would
  be great, since certain classes of bugs can just cause poor performance or
  miscalculation of the rate, while still appearing to be "working."
* We don't have a good, simple client/server example of how to use the code.
  (The unit test is not a good example, please don't cut and paste it.)


## Roadmap
Here are some areas we're actively working on improving.

### Bandwidth estimation
An earlier version of this code implemented TCP-friendly rate control (RFC 5348)
But as part of the reliability layer rewrite, bandwidth estimation has been
temporarily broken, and a fixed (configurable) rate is used.  It's not clear
if it's worth the complexity of implementation and testing to get sender-calculated
TCP-friendly rate control implemented, or a simpler method would do just as good.
Whatever method we use, needs to work even if the app code inspects the state and
decides not to send a message.  In this case, the bandwidth estimation logic might
perceive that the channel is not "data-limited", when it essentially is.  We could
add an entry point to allow the application to express this, but this is getting
complicted, making it more difficult for app code to do the right thing.  It'd
be better if it mostly "just worked" when app code does the simple thing.

### Use of STL causing more dynamic memory allocations than necessary
There are a few STL maps and such that could be significantly optimized
by the use of custom data structures or allocators.

### Abstract SteamIDs to generic "identity"
We'd like to generalize the concept of an identity.  Basically anywhere you see
CSteamID, it would be good to enable the use of a more generic identity
structure.

### NAT piercing (ICE/STUN/TURN)
The Steamworks code supports a custom protocol for relaying packets through
our network of relays and on our backbone.  At this time the opensource code
does not have any support for piercing NAT or relaying packets.  But since
the Steamworks code already has those concepts, it should be pretty easy to
add support for this.  You'd still be responsible for running the STUN/TURN
servers and doing the rendezvous/signalling, but the code could use them.

### OpenSSL bloat
Our use of OpenSSL is extremely limited; basically just AES encryption.  We use
Ed25519 keys for signatures and key exchange and we do not support X.509
certificates.  However, because the code is going through a wrapper layer that
is part of Steam, we are linking in much more code than strictly necessary.
And each time we encrypt and decrypt a packet, this wrapper layer is doing some
work which could be avoided.
