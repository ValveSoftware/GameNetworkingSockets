Building Windows w/out Using vcpkg
---

We recommend using vcpkg to get the dependencies installed.  But if that
doesn't work for you for some reason, you might try these instructions.
This is a bit of an arduous gauntlet, and this method is no longer supported,
but since these instructions were written, and at the present time still work,
we didn't delete them in case somebody finds them useful.  However, this is
not a supported method of building the library so please don't file any issues.

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

#### OpenSSL

You can install the [OpenSSL binaries](https://slproweb.com/products/Win32OpenSSL.html)
provided by Shining Light Productions. The Windows CMake distribution understands
how to find the OpenSSL binaries from these installers, which makes building a lot
easier. Be sure to pick the installers **without** the "Light"suffix. In this instance,
"Light" means no development libraries or headers.

For CMake to find the libraries, you may need to set the environment variable
`OPENSSL_ROOT_DIR`.

If you are linking statically with OpenSSL you will need to set `OPENSSL_USE_STATIC_LIBS` to `ON`.
`OPENSSL_MSVC_STATIC_RT` is automatically matched to the value of `MSVC_CRT_STATIC`.

Both need to be set before any calls to `find_package(OpenSSL REQUIRED)` because CMake caches the paths to libraries.
When building GameNetworkingSockets by itself it is sufficient to set these variables on the command line or in the GUI before first configuration.

See the documentation for [FindOpenSSL](https://cmake.org/cmake/help/latest/module/FindOpenSSL.html) for more information about CMake variables involing OpenSSL.

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


