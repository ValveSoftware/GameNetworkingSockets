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
  * libsodium (can cause issues on Intel machines with AES-NI disabled see [here](https://github.com/ValveSoftware/GameNetworkingSockets/issues/243))
  * [bcrypt](https://docs.microsoft.com/en-us/windows/desktop/api/bcrypt/)
    (Windows only.  Note the primary reason this is supported is to satisfy
    an Xbox requirement.)
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

## Using vcpkg to build the gamenetworkingsockets package

If you are using [vcpkg](https://github.com/microsoft/vcpkg/) and are OK with the latest release and default configuration (OpenSSL for the crypto backend, P2P disabled), then you do not need to sync any of this code or build gamenetworkingsockets explicitly.  You can just install the package using vcpkg.  See [this example](examples/vcpkg_example_chat/README.md)
for more.

## Windows / Visual Studio

To build gamenetworkingsockets on Windows, it's recommended to obtain thge dependencies by using vcpkg in ["manifest mode"](https://vcpkg.readthedocs.io/en/latest/users/manifests/).  The following instructions assume that you will follow the vcpkg recommendations and install vcpkg as a subfolder.  If you want to use "classic mode" or install vcpkg somewhere else, you're on your own.

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
