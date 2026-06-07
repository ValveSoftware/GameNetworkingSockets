Building for iOS
---

## Dependencies

* CMake 3.10 or later
* A build tool Ninja
* A C++11-compliant compiler
* GCC 7.3 or later
* Clang 3.3 or later
* A Macintosh

## Supported Crypto solutions
* OpenSSL 1.1.1 or later
* libsodium is not supported yet.

## NOTE

The Cmake file fetches and builds the required libraries for iOS: Protobuf and OpenSSL. (not libsodium yet)
It requires a MacOS platform.

## Build with a Mac

Install Brew if not installed.
```
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Install ninja by brew
```
brew install ninja llvm
```
Install xcode
```
xcode-select --install
```
Clone GameNetworkingSockets and create build folder
```
git clone https://github.com/ValveSoftware/GameNetworkingSockets.git
cd GameNetworkingSockets
mkdir buildiOS
cd buildiOS
```

Configure GameNetworkingSockets
```
sudo cmake -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_POSITION_INDEPENDENT_CODE=ON -Dprotobuf_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DOpenSSLTag=OpenSSL_1_1_1e -DProtobufTag=v3.21.6 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -G Ninja ..
```
Note: Configure will fetch and build Protobuf and OpenSSL libraries for iOS.

* -DCMAKE_BUILD_TYPE: can be Release or Debug
* -DOpenSSLTag: OpenSSL git tag
* -DProtobufTag: Protobuf git tag

Finally, build GameNetworkingSockets
```
sudo ninja
```

## After build
**Folder structure**

GameNetworkingSockets/</br>
&nbsp;&nbsp;&nbsp;&nbsp;buildiOS/</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;bin/</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;**libGameNetworkingSockets.dylib**</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;src/</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;**libGameNetworkingSockets_s.a**</br>
&nbsp;&nbsp;&nbsp;&nbsp;_deps/</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;protobuf-build/</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;**libprotoc.a**</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;**libprotobuf.a**</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;**libprotobuf-lite.a**</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;openssl-src/</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;**libssl.a**</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;**libcrypto.a**</br>
