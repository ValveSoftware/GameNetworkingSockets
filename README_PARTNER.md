# About the partner branch

The `partner` branch is intended for use by developers who want access to:
- the code for interacingt with the Steam Datagram Relay (SDR) network.
- the NDA-protected code for compiling the library on various consoles platforms.

This branch differs from the main branch in the following ways:

- It contains any extra files neede to support the two use cases mentioned
  above, if the files are not under NDA and not too large.
- Generally, the project files for this branch are made using VPC, Valve's
  bespoke project generator.  cmake is not supported.
- It contains some symlinks and submodules that reference the files that
  are not publicly available.

# Installation instructions

NOTE: This is a work in progress.  If you hit a snag following these instructions, please let us know.
Building on consoles has only been tested using Visual Studio 2017 or 2019.

1. Make sure you have [git lfs](https://git-lfs.com/) installed.  You can run this to check:

    ```
    ...> git lfs version
    ```

2. Make sure symbolic links are enabled on windows.  This [stackoverflow post](https://stackoverflow.com/questions/5917249/git-symbolic-links-in-windows)
   has some good info.

3. Clone this repository and checkout the `partner` branch.

    ```
    ...> git clone --branch partner https://github.com/ValveSoftware/GameNetworkingSockets.git
    ```

4. Initialize some git submodules.

    The `partner` module is always required, and contains some things that
    are required for all use cases involving SDR or console, and are either non-public
    or are large and were not included in the partner branch to avoid bloat.

    ```
    ...\GameNetworkingSockets> git submodule update --init modules/partner
    ```

    (Note the difference between the partner *branch*, the publicly-accessible branch of the GameNetworkingSockets
    repository, and the partner *submodule*, a private repository.)

    At this point you may need to sort through github authentication since the repositories
    are not public.

    Additional modules may be required depending on what features you need.
    - `modules/ps5`, `modules/nswitch`, `modules/xboxone` - support for relevant console
    - `src/external/vjson` - Currently needed for SDR support.  (Note: SDR support is currently
      required on consoles.  This will hopefully change in the future.)
    - `src/external/abseil`, `src/external/webrtc` - Needed to do P2P NAT punching thorugh WebRTC.
      Note that there is a native ICE client, but it is not thoroughly tested.

5. Generate project files using VPC:

    ```
    ...> cd src
    ...\src> devtools\bin\vpc /ps5 @steamnetworkingsockets /mksln steamnetworkingsockets_ps5.sln
    ```

    In this command line:
    - `/ps5` selects the target platform.
    - `@steamnetworkingsockets` says 'build the steamnetworkingsockets project (the client library) and any dependent projects'.  Currently the only dependent project is a "messages" project that runs the protobuf compiler.  This is a separate project for historical reasons and due to how all of this fits into the larger Steaqm codebase.
    - `/mksln steamnetworkingsockets_ps5.sln` - generate a solution with the given filename.

    VPC will locate the platform toolchain from the appropriate environment variables.

6. Open the solution and build in Visual Studio.

7. You can enable/disable several features with `STEAMNETWORKINGSOCKETS_xxx` AND `SDR_xxx` defines.  For example, if you don't need to run servers in our dedicated servers, you turn off `SDR_ENABLE_HOSTED_SERVER` and `SDR_ENABLE_HOSTED_CLIENT`.  If you just want to compile the library for a console but don't wnat to talk to relays, turn off `STEAMNETWORKINGSOCKETS_ENABLE_SDR`