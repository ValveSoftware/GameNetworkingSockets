# Installation instructions for partners

(This is a work in progress.  Note that building on consoles has only been tested using Visual Studio 2017 or 2019.)

1. Make sure you have [git lfs](https://git-lfs.com/) installed.  You can run this to check:

    ```
    ...> git lfs version
    ```

2. Clone this repository and checkout the `partner` branch.

    ```
    ...> git clone --branch partner https://github.com/ValveSoftware/GameNetworkingSockets.git
    ```

3. Initialize some git submodules.

    The `partner` module is always required, and contains some things that
    are required for all use cases involving SDR or console, and are either non-public
    or are large and were not included in the partner branch to avoid bloat.

    ```
    ...\GameNetworkingSockets> git submodule update --init modules/partner
    ```

    At this point you may need to sort through github authentication since the repositories
    are not public.

    Additional modules may be required depending on what features you need.
    - `modules/ps5`, `modules/nswitch`, `modules/xboxone` - support for relevant console
    - `src/external/vjson` - Currently needed for SDR support.  (Note: SDR support is currently
      required on consoles.  This will hopefully change in the future.)
    - `src/external/abseil`, `src/external/webrtc` - Needed to do P2P NAT punching thorugh WebRTC.
      Note that there is a native ICE client, but it is not thoroughly tested.

4. Generate project files using VPC:

    ```
    ...> cd src
    ...\src> devtools\bin\vpc /ps5 @steamnetworkingsockets /mksln steamnetworkingsockets_ps5.sln
    ```

    In this command line:
    - `/ps5` selects the target platform.
    - `@steamnetworkingsockets` says 'build the steamnetworkingsockets project (the client library) and any dependent projects'.  Currently the only dependent project is a "messages" project that runs the protobuf compiler.  This is a separate project for historical reasons and due to how all of this fits into the larger Steaqm codebase.
    - `/mksln steamnetworkingsockets_ps5.sln` - generate a solution with the given filename.

    VPC will locate the platform toolchain from the appropriate environment variables.

5. Open the solution and build in Visual Studio.