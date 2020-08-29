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

### Roadmap

Here are some things we have in mind to make P2P support better:

* Write a dummy P2P signaling server, and a test/example.  This will make it
  possible for others to write plugins for their own signaling service.
  (Issue #133)
* Write plugins for some standard signaling services.  [XMPP](https://xmpp.org/)
  is a logical choice.  This would be a great project for somebody else to
  do.
* LAN beacon support, so that P2P connections can be made when signaling
  is down.  (Issue #82)
