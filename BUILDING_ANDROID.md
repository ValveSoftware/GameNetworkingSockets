Building for Android
---

## Dependencies

* CMake 3.10 or later
* A build tool Ninja
* A C++11-compliant compiler
* GCC 7.3 or later
* Clang 3.3 or later

## Supported Crypto solutions
* OpenSSL 1.1.1 or later
* libsodium is not supported yet.

## NOTE

The Cmake file fetches and builds the required libraries for Android: Protobuf and OpenSSL. (not libsodium yet)

## Build by a Linux machine
It is tested at a local Linux machine Debian 12.

Download Android NDK from Google's site, extract it

https://dl.google.com/android/repository/android-ndk-r15c-linux-x86_64.zip~/android-ndk-r15c

Install ncurses
```
sudo apt-get install libncurses5
```

Clone GameNetworkingSockets and create build folder
```
git clone https://github.com/ValveSoftware/GameNetworkingSockets.git
cd GameNetworkingSockets
mkdir buildAndroid
cd buildAndroid
```

Configure GameNetworkingSockets
```
sudo cmake -DCMAKE_TOOLCHAIN_FILE=../../android-ndk-r15c/build/cmake/android.toolchain.cmake -DANDROID_NDK_RPATH=../../android-ndk-r15c -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 -DCMAKE_CXX_FLAGS=-std=c++11 -DCMAKE_POSITION_INDEPENDENT_CODE=ON -Dprotobuf_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DANDROID_STL=c++_static -DOpenSSLTag=OpenSSL_1_1_1e -DProtobufTag=v3.21.6 -G Ninja ..
```
Note: Configure will fetch and build Protobuf and OpenSSL libraries for Android.

* -DCMAKE_TOOLCHAIN_FILE: Path to the toolchain cmake file for android NDK
* -DANDROID_NDK_RPATH: Relative path to the android NDK
* -DANDROID_ABI: Can be arm64-v8a or armeabi-v7a, both supports.
* -DANDROID_PLATFORM: Android platform
* -DCMAKE_BUILD_TYPE: can be Release or Debug
* -DOpenSSLTag: OpenSSL git tag
* -DProtobufTag: Protobuf git tag

Finally, build GameNetworkingSockets
```
sudo ninja
```

## After build
**Folder structure**

android-ndk-r15c/</br>
GameNetworkingSockets/</br>
&nbsp;&nbsp;&nbsp;&nbsp;buildAndroid/</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;bin/</br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;**libGameNetworkingSockets.so**</br>
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

## Build by a Windows machine

I believe with the right shell, MSYS2 or WSL, it should be building properly with the Linux commands above.
