# About that P2P support....

SteamNetworkingSockets supports peer-to-peer connections.  A "peer-to-peer"
connection in this context means that the hosts do not (initially) know
each other's IP address.  Furthermore, they may be behind NAT, and so they
may not know their *own* public IP address.  They may not even *have* a public
IP that other hosts can use to send them inbound traffic.  [Here](https://tailscale.com/blog/how-nat-traversal-works/)
is a good article about the problem of NAT traversal.

[ICE](https://en.wikipedia.org/wiki/Interactive_Connectivity_Establishment)
is an internet standard protocol for discovering and sharing IP addresses,
negotiating NAT, and establishing a direct connection or fallback to relaying
the connection if necessary.

The opensource version of the code can compile with [google webrtc](https://webrtc.googlesource.com/src)'s ICE
implementation.  We interface with the WebRTC code at a reletaively low level
and only use it for datagram transport.  We don't use DTLS or WebRTC data
channels.  (In the future, we may offer alternate NAT traversal
implementations.  In particular, we'd like to have an implementation that
uses [PCP](https://tools.ietf.org/html/rfc6887), which is not used by
the current google WebRTC code.)

## Symmetric connect mode

SteamNetworkingSockets offers a unique feature known as *symmetric
connect mode*, which really puts the "peer" in "peer-to-peer connection".
This feature is useful in the following common use case:

* Two hosts wish to establish a *single* connection between them.
* Either host may initiate the connection.
* The hosts may initiate the connection at the same time.

This situation involves race conditions that can be tricky to get
right, especially when authentication, encryption, ICE roles, etc
are involved.  With symmetric connect mode, sorting out these race
conditions and producing a single connection is handled by the API.

See the ``k_ESteamNetworkingConfig_SymmetricConnect``
connection flag in [steamnetworkingtypes.h](include/steam/steamnetworkingtypes.h)
for more info.

## ISteamNetworkingMessages

Most P2P libraries, such as google WebRTC, and indeed our own
[ISteamNetworkingSockets](include/steam/isteamnetworkingsockets.h), are *connection
oriented*.  To talk to a peer, you first establish a connection to the peer, and
when you send and receive messages, the peer is identified by the connection handle.

Much existing network code is based on UDP with a single socket, where
 connection handles are not used.  Instead, packets are sent with the IP address
 of the recipeient specified for each packet.   (E.g. ``sentto()`` and ``recvfrom()``).
[ISteamNetworkingMessages](include/steam/isteamnetworkingmessages.h) was created
to provide a more "ad-hoc" interface like UDP.  It can be useful when adding P2P
support to existing code, depending on the abstraction you are working with.  If
the code you are modifing already has the concept of a connection, then you might
find it easier to use ISteamNetworkinSockets directly.  But if you are modifying code
at a lower level, you may find that you need to maintain a table of active connections,
and each time you send a packet, use the existing connection if one exists, or
create a new connection if one does not exist.  This is exactly what
ISteamNetworkingMessages does for you.  It creates symmetric-mode connections on
demand the first time you communicate with a given peer, and idle connections are
automatically closed when they are no longer needed.

## Requirements

Peer-to-peer connections require more than just working code.  In addition
to the code in this library, there are several other prerequisites.
(Note that the Steamworks API provides all of these services for Steam games.)

### Signaling service

A side channel, capable of relaying small rendezvous
messages from one host to another.  This means hosts must have a constant
connection to your service, once that enables you to *push* messages to them.

SteamNetworkingSockets supports a pluggable signaling service.  The requirements
placed on your signaling service are relatively minimal:

* Individual rendezvous messages are small.  (Perhaps bigger than IP MTU,
  but never more than a few KB.)
* Only datagram "best-effort" delivery is required. We are tolerant
  protocol is tolerant of dropped, duplicated, or reordered messages.
  These anomolies may cause negotiation to take longer, but not fail.
  This means, for example that there doesn't need to be a mechanism to
  inform the system when your connection to the signaling service is
  disrupted.
* The channel can be relatively low bandwidth and high latency.  Usually
  there is a burst of a handful of exchanges when a connection is created,
  followed by silence, unless something changes with the connection.


### STUN server(s)

A [STUN](https://en.wikipedia.org/wiki/STUN) server is used to help peers
discover their own public IP address and open up firewalls.  STUN
servers are relatively low bandwidth, and there are publicly-available ones.

### Relay fallback

Unfortunatley, for some pairs of hosts, NAT piercing is not successful.
In this situation, the traffic must be relayed.  In the ICE protocol, this is
done using [TURN](https://en.wikipedia.org/wiki/Traversal_Using_Relays_around_NAT)
servers.  (NOTE: Any TURN server also doubles as a STUN server.)  Because the TURN
server is relaying every packet, is is a relatively costly service, so you probably
will need to run your own, or just fail connections that cannot pierce NAT.

On Steam we use a custom relay service known as [Steam Datgaram Relay](https://partner.steamgames.com/doc/features/multiplayer/steamdatagramrelay)
-- SDR for short -- carrying packets through our network of relays and
on our backbone.   (You may see this mentioned in the opensource code here,
but the SDR support code is not opensource.)  Also, on Steam we always
relay traffic and do not share IP addresses between untrusted peers, so
that malicious players cannot DoS attack.

### Naming hosts and matchmaking

The above requirements are just what is needed to make a connection between two
hosts, once you know who to connect to.  But before that, you need a way to assign an
identity to a host, authenticate them, matchmaking them, etc.  Those services are
also included with Steam, but outside the scope of a transport library like this.

## Using P2P

Assuming you have all of those requirements, you can use SteamNetworkingSockets
to make P2P connections!

To compile with ICE support, set USE_STEAMWEBRTC when building the project files:
```
cmake -DUSE_STEAMWEBRTC=ON (etc...)
```

You'll also need to activate two git submodules to pull down the google WebRTC code.
(Just run ``cmake`` and follow the instructions.)

Take a look at these files for more information:

* [steamnetworkingcustomsignaling.h](include/steam/steamnetworkingcustomsignaling.h)
  contains the interfaces you'll need to implement for your signaling service.
* An example of a really trivial signaling protocol:
  * [trivial_signaling_server.go](examples/trivial_signaling_server.go) server
  * [trivial_signaling_client.cpp](examples/trivial_signaling_client.cpp) client
* A test case that puts everything together.  It starts up an example trivial
  signaling protocol server and two peers, and has them connect to each other
  and exchange a few messages.  We use the publicly-available google STUN servers.
  (No TURN servers for relay fallback in this example.)
  * [test_p2p.py](tests/test_p2p.py) Executive test script that starts all the processes.
  * [test_p2p.cpp](tests/test_p2p.cpp) Code for the process each peer runs.

## Roadmap

Here are some things we have in mind to make P2P support better:

* Get plugins written for standard, opensource protocols that could be used for
signaling, such as [XMPP](https://xmpp.org/).  This would be a great project for
  somebody else to do!  We would welcome contributions to this repository, or
  happily link to your own project.  (See issue #136)
* LAN beacon support, so that P2P connections can be made even when signaling
  is down or the hosts do not have Internet connectivity.  (See issue #82)
