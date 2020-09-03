// Really simple P2P signaling server.
//
// When establishing peer-to-peer connections, the peers
// need some sort of pre-arranged side channels that they
// can use to exchange messages.  This channel is assumed
// to be relatively low bandwidth and high latency.  This
// service is often called "signaling".
//
// This server has the following really simple protocol:
// It listens on a particular TCP port.  Clients connect
// raw TCP.  The protocol is text-based and line oriented,
// so it is easy to test using telnet.  When a client
// connects, it should send its identity on the first line.
// Afterwards, clients can send a message to a peer by
// sending a line formatted as follows:
//
// DESTINATION_IDENTITY PAYLOAD
//
// Identites may not contain spaces, and the payload
// should be plain ASCII text.  (Hex or base64 encode it).
//
// If there is a client with that destination identity,
// then the server will forward the message on.  Otherwise
// it is discarded.
//
// Forwarded messages have basically the same format and
// are the only type of message the server ever sends to the
// client.  The only difference is that the identity is the
// identity of the sender.
//
// This is just an example code to illustrate what a
// signaling service is.  A real production server would
// probably need to be able to scale across multiple
// processes, and provide authentication and rate
// limiting.
//
// Note that SteamNetworkingSockets use of signaling
// service does NOT assume guaranteed delivery.

package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"net"
	"strings"
)

const DEFAULT_LISTEN_PORT = 10000

// Current list of client connections
var g_mapClientConnections = make(map[string]net.Conn)

// Goroutine to service a client connection
func ServiceConnection(conn net.Conn) {

	// Save off address
	addr := conn.RemoteAddr().String()

	// Attach a Reader object to the connection, so we can read from it easily
	in := bufio.NewReader(conn)

	// In our trivial protocol, the first line contains the client identity
	// on a line by itself
	intro, err := in.ReadString('\n')
	if err != nil {
		log.Printf("[%s] Aborting connection before we ever received client identity", addr)
		conn.Close()
	}
	identity := strings.TrimSpace(intro)

	// Amnnnnnd that's it.  No authentication.

	// Locate existing connection, if any.
	existingConn := g_mapClientConnections[identity]

	// Add us to map or replace existing entry
	g_mapClientConnections[identity] = conn

	// Now handle existing entry
	if existingConn != nil {
		log.Printf("[%s@%s] Closing connection to make room for new connection from '%s'", identity, existingConn.RemoteAddr().String(), addr)
		existingConn.Close()
	}
	log.Printf("[%s@%s] Added connection", identity, addr)

	// Keep reading until connection is closed
	for {
		line, err := in.ReadString('\n')
		if err != nil {
			conn.Close()

			// Are we stil in the map?
			if g_mapClientConnections[identity] == conn {
				log.Printf("[%s@%s] Connecton closed. %s", identity, addr, err)
				delete(g_mapClientConnections, identity)
			} else {
				// Assume it's because we got replaced by another connection.
				// The other connection already logged, so don't do anything here
			}
			break
		}

		// Our protocol is just [destination peer identity] [payload]
		// And everything is in text.
		dest_and_msg := strings.SplitN(line, " ", 2)
		if len(dest_and_msg) != 2 {
			log.Printf("[%s@%s] Ignoring weird input '%s' (maybe truncated?)", identity, addr, line)
			continue
		}
		dest_identity := strings.TrimSpace(dest_and_msg[0])
		payload := dest_and_msg[1]

		// Locate the destination peer's connection.
		dest_conn := g_mapClientConnections[dest_identity]
		if dest_conn == nil {
			log.Printf("[%s@%s] Ignoring, destination peer '%s' not found", identity, addr, dest_identity)
			continue
		}

		// Format new message, putting the sender's identity in front.
		msg := identity + " " + payload

		// Send to the peer
		dest_conn.Write([]byte(msg))

		// Lawg
		log.Printf("[%s@%s] -> %s (%d chars)", identity, addr, dest_identity, len(payload))
	}

}

// Main entry point
func main() {

	// Parse command line flags
	port := flag.Int("port", DEFAULT_LISTEN_PORT, "Port to listen on")
	flag.Parse()
	listen_addr := fmt.Sprintf("0.0.0.0:%d", *port)

	// Start listening
	listener, err := net.Listen("tcp", listen_addr)
	if err != nil {
		log.Panic(err)
	}
	log.Printf("Listening at %s", listen_addr)

	// Main loop
	for {

		// Wait for the next incoming connection
		conn, err := listener.Accept()
		if err != nil {
			log.Panic(err)
		}

		// Start goroutine to service it
		go ServiceConnection(conn)
	}

}
