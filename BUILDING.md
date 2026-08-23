Building
---

## Dependencies

* CMake 3.15 or later (3.21 or later to use the macOS/iOS presets in `CMakePresets.json`)
* A build tool like Ninja, GNU Make or Visual Studio
* A C++17-compliant compiler, such as:
  * GCC 7.3 or later
  * Clang 3.3 or later
  * Visual Studio 2017 or later

  (The library's own code only needs C++11, but protobuf 22 and later require
  C++17 of everything that includes their headers.)
* One of the following crypto solutions:
  * OpenSSL 1.1.1 or later
  * libsodium (can cause issues on Intel machines with AES-NI disabled see [here](https://github.com/ValveSoftware/GameNetworkingSockets/issues/243))
  * [bcrypt](https://docs.microsoft.com/en-us/windows/desktop/api/bcrypt/)
    (Windows only.  Note the primary reason this is supported is to satisfy
    an Xbox requirement.)
* Google protobuf 2.6.1+.  protobuf 22 and later also pull in Abseil.  The
  `protoc` compiler and the `libprotobuf` runtime you link against must be the
  **same version**; the build prints a warning if it can tell that they aren't.
* Google [webrtc](https://opensource.google/projects/webrtc) is used for
  NAT piercing (ICE) for P2P connections.  The relevant code is linked in as a
  git submodule.  You'll need to initialize that submodule to compile.

## Known Issues
* The build may have link errors when building with LLVM 10+:
  [LLVM bug #46313](https://bugs.llvm.org/show_bug.cgi?id=46313). As
  a workaround, consider building the library with GCC instead.

## Linux

### OpenSSL and protobuf

Just use the appropriate package manager.

Ubuntu/debian:

```
# apt install libssl-dev
# apt install libprotobuf-dev protobuf-compiler
```

Arch Linux:

```
# pacman -S openssl
# pacman -S protobuf
```

### Building

Using CMake (preferred):

```
$ mkdir build
$ cd build
$ cmake -G Ninja ..
$ ninja
```

## Using vcpkg to install the gamenetworkingsockets package

If you are using [vcpkg](https://github.com/microsoft/vcpkg/) and are OK with the latest release and default configuration (OpenSSL for the crypto backend, P2P disabled), then you do not need to sync any of this code or build gamenetworkingsockets explicitly.  You can install it directly from the vcpkg registry:

```
vcpkg install gamenetworkingsockets
```

Then include the headers in your project as, e.g.:

```cpp
#include <steam/steamnetworkingsockets.h>
```

See [this example](examples/vcpkg_example_chat/README.md) for more.

## Windows / Visual Studio

To build gamenetworkingsockets on Windows, it's recommended to obtain the dependencies by using vcpkg in ["manifest mode"](https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode).  The following instructions assume that you will follow the vcpkg recommendations and install vcpkg as a subfolder.  If you want to use "classic mode" or install vcpkg somewhere else, you're on your own.

If you don't want to use vcpkg, try the [manual instructions](BUILDING_WINDOWS_MANUAL.md).

First, bootstrap vcpkg.  From the root folder of your GameNetworkingSockets workspace:

```
> git clone https://github.com/microsoft/vcpkg
> .\vcpkg\bootstrap-vcpkg.bat
```

For the following commands, it's important to run them from a Visual Studio command prompt so that the compiler can be located.

You can obtain the dependent packages into your local `vcpkg` folder as an explicit step.  This is optional because the `cmake` command line below will also do it for you, but doing it as a separate step can help isolate any problems.

```
> .\vcpkg\vcpkg install --triplet=x64-windows
```

If you want to use the libsodium backend, install the libsodium dependencies by adding `--x-feature=libsodium`.

Now run cmake to create the project files.  Assuming you have vcpkg in the recommended location as shown above, the vcpkg toolchain will automatically be used, so you do not need to explicitly set `CMAKE_TOOLCHAIN_FILE`.  A minimal command line might look like this:

```
> cmake -S . -B build -G Ninja
```

To build all the examples and tests and add P2P/ICE support via the WebRTC submodule, use something like this:

```
> cmake -S . -B build -G Ninja -DBUILD_EXAMPLES=ON -DBUILD_TESTS=ON -DUSE_STEAMWEBRTC=ON
```

Finally, build the projects:

```
> cd build
> ninja
```

## macOS

Everything the build needs is available from [Homebrew](https://brew.sh):

```
$ brew install cmake ninja openssl@3 protobuf
```

What each of those is for:

| Formula | Used for |
| --- | --- |
| `openssl@3` | `libcrypto`, the default crypto backend (AES-GCM, SHA-256, ed25519/curve25519) |
| `protobuf` | the `protoc` compiler *and* the `libprotobuf` runtime |
| `abseil` | not installed directly -- Homebrew's `protobuf` depends on it, and `libprotobuf` links against it, so it ends up in your link line too |
| `cmake`, `ninja` | build tooling |

Additionally, `python3` is needed only if you want to run the tests, and `perl`
(the one in `/usr/bin` is fine) only if you build dependencies from source for
iOS.

Do **not** try to use the OpenSSL that ships with macOS.  `/usr/lib/libcrypto.dylib`
is LibreSSL, Apple ships no headers for it, and it is not a drop-in substitute.

### Building

```
$ cmake -S . -B build -G Ninja
$ cmake --build build
```

CMake finds the Homebrew OpenSSL and protobuf on its own, because CMake searches
the Homebrew prefix by default -- `/opt/homebrew` on Apple Silicon, `/usr/local`
on Intel.  If you have more than one OpenSSL around and it picks the wrong one,
point it at the right one explicitly:

```
$ cmake -S . -B build -G Ninja -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
```

There is also a preset that does the same thing with a shorter command line:

```
$ cmake --preset macos
$ cmake --build --preset macos
```

### Examples, tests and the cert tool

```
$ git submodule update --init src/external/vjson     # needed by the cert tool
$ cmake -S . -B build -G Ninja -DBUILD_EXAMPLES=ON -DBUILD_TESTS=ON -DBUILD_TOOLS=ON
$ cmake --build build
$ ctest --test-dir build
```

### Deployment target and universal binaries

Set `CMAKE_OSX_DEPLOYMENT_TARGET` to control the minimum supported macOS
version.  A universal (arm64 + x86_64) build needs universal dependencies,
which Homebrew does not provide -- its bottles are single-architecture.  To get
a universal *static* library, build each architecture against its own
dependencies and `lipo` the results; `cmake/apple/make-xcframework.sh --macos` does
exactly that.

## iOS

CMake has built-in support for cross-compiling to iOS, so no third-party
toolchain file is required.  What you do need is a set of dependencies built
*for iOS* -- Homebrew only ships macOS binaries -- and a `protoc` that runs on
the host to generate the protobuf sources.

### Prerequisites

* **Xcode** with the iOS SDK.  `xcodebuild -showsdks` should list an iOS SDK and
  an iOS Simulator SDK.  If you only have the Command Line Tools installed,
  point `xcode-select` at the full Xcode:

  ```
  $ sudo xcode-select -s /Applications/Xcode.app
  ```

* **Homebrew**: `brew install cmake ninja protobuf`

  Only the *host* tools come from Homebrew here.  `protoc` runs on your Mac to
  generate the `.pb.cc` sources; the `libprotobuf` you link into the iOS binary
  has to be built separately (see below) and must be the **same version** as
  that `protoc`.  The dependency script pins it to whatever `protoc --version`
  reports, and the CMake build warns you if the two ever diverge.

  You do **not** need `openssl@3` for an iOS build -- that formula is macOS-only,
  and OpenSSL for iOS is built from source by the script below.

* **Perl** -- used by OpenSSL's `Configure`.  The system `/usr/bin/perl` is fine.

### The short version

```
$ ./cmake/apple/make-xcframework.sh --with-deps
```

That builds OpenSSL and protobuf/Abseil for iOS device and simulator, builds
GameNetworkingSockets against them, and packages the result as
`build/GameNetworkingSockets.xcframework` with an `ios-arm64` slice and a fat
`ios-arm64_x86_64-simulator` slice.

The dependencies are folded into the archives, so the xcframework is
self-contained.  In Xcode, add it to *Frameworks, Libraries, and Embedded
Content*, and also link `CoreFoundation` (Abseil's time-zone code needs it) and
the C++ standard library.  The first run takes a while -- it builds OpenSSL and
protobuf three times, once per architecture -- but the results are cached under
`build/apple-deps/`, so later runs only rebuild GameNetworkingSockets itself.

Useful options:

```
$ ./cmake/apple/make-xcframework.sh --help
```

### Verifying it works

```
$ ./cmake/apple/run-ios-smoke-test.sh
```

Builds [tests/ios_smoke_test.cpp](tests/ios_smoke_test.cpp) against the
simulator slice of the xcframework and runs it in an iOS Simulator: it stands up
a listen socket on loopback, connects to it, and round-trips a reliable message,
which exercises the crypto backend, protobuf, the socket thread and SNP.  It
boots a simulator if none is running, and shuts that one back down afterwards.

### The long version

Build the dependencies for one platform/architecture at a time:

```
$ ./cmake/apple/build-apple-deps.sh --platform iphoneos --arch arm64
$ ./cmake/apple/build-apple-deps.sh --platform iphonesimulator --arch arm64
```

Each invocation installs into `build/apple-deps/<platform>-<arch>/`.  Then
configure and build the library against one of those prefixes:

```
$ PREFIX=$PWD/build/apple-deps/iphoneos-arm64
$ cmake -S . -B build/iphoneos-arm64 -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DCMAKE_PREFIX_PATH=$PREFIX \
    -DCMAKE_FIND_ROOT_PATH=$PREFIX \
    -DOPENSSL_ROOT_DIR=$PREFIX
$ cmake --build build/iphoneos-arm64
```

`CMAKE_FIND_ROOT_PATH` is not redundant with `CMAKE_PREFIX_PATH`: when
cross-compiling, CMake will only look for libraries and headers underneath the
find-root paths, so without it `find_package(OpenSSL)` fails even though the
prefix is correct.

Presets are provided for the common cases:

```
$ cmake --preset ios-device       # or ios-simulator, or ios-device-framework
$ cmake --build --preset ios-device
```

### Static library or framework?

The static library (`BUILD_STATIC_LIB`, on by default) is the straightforward
choice, and is what the xcframework contains.

`BUILD_SHARED_LIB` is **off** by default on iOS.  A dynamic library shipped
inside an app bundle is normally a framework -- that is what Xcode's *Embed &
Sign* and the App Store tooling expect; a bare `.dylib` can be embedded and
loaded too, but you have to handle the embedding and code signing yourself.
So on iOS, turning the shared library on gives you a framework by default:

```
$ cmake --preset ios-device-framework
```

That produces `GameNetworkingSockets.framework`, with the public headers under
`Headers/steam/`.  It is not code-signed; sign it when you embed it (Xcode's
*Embed & Sign*).

`BUILD_FRAMEWORK` is a separate knob from `BUILD_SHARED_LIB` rather than a
synonym for it, because the two questions really are separate: whether to build
a shared library at all, and how to package it.  `-DBUILD_SHARED_LIB=ON` alone
is enough on iOS -- `BUILD_FRAMEWORK` follows it.  The separate option is there
so that macOS can opt *in* to a framework (`-DBUILD_FRAMEWORK=ON`, which keeps
the default macOS output a plain `libGameNetworkingSockets.dylib` for everything
that already expects one), and so that iOS can opt *out* with
`-DBUILD_FRAMEWORK=OFF` if you are doing your own packaging.

### Notes for iOS apps

* **Local network permission.**  On iOS 14 and later, the system requires user
  consent before an app can talk to other devices on the local network.  Without
  an `NSLocalNetworkUsageDescription` string in your `Info.plist`, connections
  to peers on the LAN will simply never complete.  Traffic to hosts off the
  local network is unaffected.
* **Backgrounding.**  iOS tears down UDP sockets when an app is suspended.
  Expect connections to drop when you go to the background, and reconnect on
  the way back rather than assuming the session survived.
* **Examples, tests and the cert tool** are command-line programs and cannot be
  built for iOS; the CMake configure step will tell you so if you ask for them.
  Build and run those in a separate macOS build directory.
* **P2P/ICE** is compiled in by default and uses the built-in native ICE client.
  The Google WebRTC backend (`USE_STEAMWEBRTC`) is not supported on iOS here.

### tvOS, watchOS, visionOS

The CMake and platform-detection plumbing recognizes these as well, so a build
should work if you supply dependencies for them.  Only iOS and macOS are
actually built and tested; in particular `cmake/apple/build-apple-deps.sh` knows how
to configure OpenSSL for iOS and macOS only.
