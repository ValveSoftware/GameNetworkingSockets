GameNetworkingSockets
---

GameNetworkingSockets is a basic transport layer for games.  The features are:

* Connection-oriented protocol (like TCP)
* ... but message-oriented instead of stream-oriented.
* Mix of reliable and unreliable messages
* Messages can be larger than underlying MTU, the protocol performs fragmentation and reassembly, and retransmission for reliable
* Bandwidth estimation based on TFP-friendly rate control (RFC 5348)
* Encryption.
* Tools for simulating loss and detailed stats measurement

The main interface class is named SteamNetworkingSockets, and many files have "steam" in their name.
But *Steam is not needed*.  The reason for the name is that this provides a subset of the functionality of the API with the same name in the SteamworksSDK.  The intention is that on PC you can use the Steamworks version, and on other platforms, you can use this version.  In this way, you can avoid having the Steam version be "weird" or not take full advantage of the features above that it provides.

But even if you don't make games or aren't on Steam, feel free to use this code for whatever purpose you want.

### Coming soon

Sorry, we're still in the process of taking the code from the SteamNetworkingSockets library and making it ready to be open-sourced.  Watch this space.