# About that P2P support....

### Deploy some infrastructure

Peer-to-peer connections require more than just working code.  You need to
actually deploy some infrastructure.  There are three basic needs:

* **Signaling service**  A side channel, capable of relaying small rendezvous
  messages from one peer to another.  This means peers must have a constant
  connection to your service, so that you can push messages to them.
  SteamNetworkiongSockets imposes little requirements on the service.
  Rendezvous messages are relatively small (maybe bigger than IP MTU, but never
  more than a few KB), and only datagram best-effort delivery is required.
  The end-to-end protocol is tolerant of dropped, duplicated, or reordered
  signals.  Usually there is a burst of a handful of exchanges when a
  connection is created, followed by silence, unless something changes with
  the connection.

* **STUN server**  A STUN server is used to help peers discover the appropate
  IP addresses to use for communication, including piercing NAT and opening up
  firewalls.  This is the ICE protocol.  STUN servers are relatively low
  bandwidth, and there are publicly availableones.

* **Relay fallback**  NAT piercing is not always successful.  In this situation,
  the traffic must be relayed.  In the ICE protocol, this is done using TURN
  servers.  (Any TURN server also doubles as a STUN server.)  Because the TURN
  server is relaying every packet, is is a relatively costly service, so you
  probably need to run your own.

The Steamworks API provides all of these services for Steam games.  On Steam
we use a custom protocol known as SDR, for relaying packets through our network
of relays and on our backbone.   (You may see this mentioned in the opensource
code here, but the SDR support code is not opensource.)

### You have matchmaking, right?

The above requirements are just what is needed to make a connection to a peer,
once you know who to connect to.  But before that, you need a way to assign an
identity to a peer, authenticate them, matchmaking them, etc.  Those services are
also included with Steam, but outside the scope of a transport library like this.

However, assuming you have all of those requirements, you can use
SteamNetworkingSockets to make P2P connections!

### WebRTC ICE implementation

We use [google's WebRTC code](https://webrtc.googlesource.com/src/) to implement
the ICE protocol.  We have a thin [interface layer](src/external/steamwebrtc/ice_session.h)
that isolates this code from SteamNetworkingSockets, because WebRTC has a relatively
complicated build system.  (We compile this into a seperate .dll, which we call
steamwebrtc, but that isn't the only way it could be done.)

OK, so the bad news is that getting compling might be just a bit of work.  We
grabbed a snapshot of the WebRTC code at some point in the past (around 2018).
Although we have not made any changes to it, we don't know exactly what
snapshot we grabbed.  But you will probbaly want to update to more recent
snapshot, anyway.

We'd like to fix this situation, and get synced up so that we are compiling
against a known release of webrtc.  If you are interested in P2P and want to
help with tis, please email us and we can work with you to make it happen!

### Roadmap

Here's some things we have in mind to make P2P support better:

* Make it easier to obtain and compile the right version of WebRTC.
  (If you are interested, please help!)
* LAN beacon support, so that P2P connections can be made when signaling
  is down.
