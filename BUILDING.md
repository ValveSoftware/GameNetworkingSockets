Building
---

## Dependencies

* CMake 3.10 or later
* A build tool like Ninja, GNU Make or Visual Studio
* A C++11-compliant compiler, such as:
  * GCC 7.3 or later
  * Clang 3.3 or later
  * Visual Studio 2017 or later
* One of the following crypto solutions:
  * OpenSSL 1.1.1 or later
  * OpenSSL 1.1.x, plus ed25519-donna and curve25519-donna.  (We've made some
    minor changes, so the source is included in this project.)
  * libsodium
  * [bcrypt](https://docs.microsoft.com/en-us/windows/desktop/api/bcrypt/)
    (windows only)
* Google protobuf 2.6.1+
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

## Windows / Visual Studio

On Windows, you can use the [vcpkg](https://github.com/microsoft/vcpkg/) package manager.
The following instructions assume that you will follow the vcpkg recommendations and install
vcpkg as a subfolder.  If you want to install vcpkg somewhere else, you're on your own.
See the [quick start](https://github.com/microsoft/vcpkg/#quick-start-windows) for more info.

First, bootstrap vcpkg.  From the root folder of your GameNetworkingSockets workspace:

```
> git clone https://github.com/microsoft/vcpkg
> .\vcpkg\bootstrap-vcpkg.bat
```

The following command will build the prerequisites and GameNetworkingSockets.
You can use the vcpkg option `--triplet x64-windows` or `--triplet x86-windows` to force
the hoice of a particular target architecture.  To use libsodium as the crypto backend
rather than OpenSSL, install `gamenetworkingsockets[core,libsodium]`.

```
> .\vcpkg\vcpkg --overlay-ports=vcpkg_ports install gamenetworkingsockets
```

The library should be immediately available in Visual Studio projects if
the vcpkg integration is installed, or the vcpkg CMake toolchain file can
be used for CMake-based projects.

### Manual setup

Setting up the dependencies by hand is a bit of an arduous gauntlet.

#### OpenSSL

You can install the [OpenSSL binaries](https://slproweb.com/products/Win32OpenSSL.html)
provided by Shining Light Productions. The Windows CMake distribution understands
how to find the OpenSSL binaries from these installers, which makes building a lot
easier. Be sure to pick the installers **without** the "Light"suffix. In this instance,
"Light" means no development libraries or headers.

For CMake to find the libraries, you may need to set the environment variable
`OPENSSL_ROOT_DIR`.

#### Checking prerequisites

Start a Visual Studio Command Prompt (2017+), and double-check
that you have everything you need.  Note that Visual Studio comes with these tools,
but you might not have selected to install them.  Or just install them from somewhere
else and put them in your `PATH`.

*IMPORTANT*: Make sure you start the command prompt for the desired target
architecture (x64 or x64)!  In the examples here we are building 64-bit.

```
**********************************************************************
** Visual Studio 2019 Developer Command Prompt v16.5.4
** Copyright (c) 2019 Microsoft Corporation
**********************************************************************
[vcvarsall.bat] Environment initialized for: 'x64'

C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise> cd \dev

C:\dev> git --version
git version 2.17.1.windows.2

C:\dev> cmake --version       # 3.5 or higher is required
cmake version 3.16.19112601-MSVC_2

C:\dev> ninja --version
1.8.2
```


#### Protobuf

Instructions for getting a working installation of google protobuf on Windows can
be found [here](https://github.com/protocolbuffers/protobuf/blob/master/cmake/README.md).

Here is an example.  First, start a Visual Studio Command Prompt as above.  Then download
a particular release of the source.  Here we are using `git`, but
you can also just download a [release .zip](https://github.com/protocolbuffers/protobuf/releases).

```
C:\dev> git clone -b 3.5.x https://github.com/google/protobuf
C:\dev> cd protobuf
```

Compile the protobuf source.  You need to make sure that all of the following match
the settings you will use for compiling GameNetworkingSockets:

* The target architecture must match (controlled by the MSVC environment variables).
* ```CMAKE_BUILD_TYPE```, which controls debug or release for both projects,
  and must match.
* ```protobuf_BUILD_SHARED_LIBS=ON``` in the example indicates that
  GameNetworkingSockets will link dynamically with protobuf .dlls, which is the
  default for GameNetworkingSockets.  For static linkage, remove this and set
  ``Protobuf_USE_STATIC_LIBS=ON`` when building GameNetworkingSockets.
* If you link statically with protobuf, then you will also need to make sure that
  the linkage with the MSVC CRT is the same.  The default for both protobuf and
  GameNetworkingSockets is multithreaded dll.

Also, note the value for ```CMAKE_INSTALL_PREFIX```.  This specifies where to
"install" the library (headers, link libraries, and the protoc compiler tool).

```
C:\dev\protobuf> mkdir cmake_build
C:\dev\protobuf> cd cmake_build
C:\dev\protobuf\cmake_build> cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=c:\sdk\protobuf-amd64 ..\cmake
C:\dev\protobuf\cmake_build> ninja
C:\dev\protobuf\cmake_build> ninja install
```

#### Building

Start a Visual Studio Command Prompt, and create a directory to hold the build output.

```
C:\dev\GameNetworkingSockets> mkdir build
C:\dev\GameNetworkingSockets> cd build
```

You'll need to add the path to the protobuf `bin` folder to your path, so
CMake can find the protobuf compiler.  If you followed the example above, that would
be something like this:

```
C:\dev\GameNetworkingSockets\build> set PATH=%PATH%;C:\sdk\protobuf-amd64\bin
```

Now invoke cmake to generate the type or project you want to build.  Here we are creating
ninja files, for a 100% command line build.  It's also possible to get cmake to output
Visual studio project (`.vcxproj`) and solution (`.sln`) files.
```
C:\dev\GameNetworkingSockets\build> cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
```

Finally, perform the build
```
C:\dev\GameNetworkingSockets\build> ninja
```

## Mac OS X

Using [Homebrew](https://brew.sh)

### OpenSSL

```
$ brew install openssl
$ export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/opt/openssl/lib/pkgconfig
```
GameNetworkingSockets requries openssl version 1.1+, so if you install and link openssl but at compile you see the error ```Dependency libcrypto found: NO (tried cmake and framework)``` you'll need to force Brew to install openssl 1.1. You can do that like this:
```
$ brew install openssl@1.1
$ export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/opt/openssl@1.1/lib/pkgconfig
```

### protobuf

```
$ brew install protobuf
```

## MSYS2

You can also build this project on [MSYS2](https://www.msys2.org). First,
follow the [instructions](https://github.com/msys2/msys2/wiki/MSYS2-installation) on the
MSYS2 website for updating your MSYS2 install.

**Be sure to follow the instructions at the site above to update MSYS2 before
you continue. A fresh install is *not* up to date by default.**

Next install the dependencies for building GameNetworkingSockets (if you want
a 32-bit build, install the i686 versions of these packages):

```
$ pacman -S \
    git \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-openssl \
    mingw-w64-x86_64-pkg-config \
    mingw-w64-x86_64-protobuf
```

And finally, clone the repository and build it:

```
$ git clone https://github.com/ValveSoftware/GameNetworkingSockets.git
$ cd GameNetworkingSockets
$ mkdir build
$ cd build
$ cmake -G Ninja ..
$ ninja
```

**NOTE:** When building with MSYS2, be sure you launch the correct version of
the MSYS2 terminal, as the three different Start menu entries will give you
different environment variables that will affect the build.  You should run the
Start menu item named `MSYS2 MinGW 64-bit` or `MSYS2 MinGW 32-bit`, depending
on the packages you've installed and what architecture you want to build
GameNetworkingSockets for.


## Visual Studio Code
If you're using Visual Studio Code, we have a few extensions to recommend
installing, which will help build the project. Once you have these extensions
installed, open up the .code-workspace file in Visual Studio Code.

### C/C++ by Microsoft
This extension provides IntelliSense support for C/C++.

VS Marketplace Link: https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools

### CMake Tools by vector-of-bool
This extension allows for configuring the CMake project and building it from
within the Visual Studio Code IDE.

VS Marketplace Link: https://marketplace.visualstudio.com/items?itemName=vector-of-bool.cmake-tools
