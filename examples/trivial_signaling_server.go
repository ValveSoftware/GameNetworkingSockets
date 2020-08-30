package main

import (
	"strings"
	"fmt"
	"net"
	"flag"
	"bufio"
	"log"
)

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
	intro,err := in.ReadString('\n')
	if err != nil {
		log.Printf( "[%s] Aborting connection before we ever received client identity", addr )
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
		log.Printf( "[%s@%s] Closing connection to make room for new connection from '%s'", identity, existingConn.RemoteAddr().String(), addr )
		existingConn.Close()
	}
	log.Printf( "[%s@%s] Added connection", identity, addr )

	// Keep reading until connection is closed
	for {
		line,err := in.ReadString('\n')
		if err != nil {
			conn.Close()

			// Are we stil in the map?
			if g_mapClientConnections[identity] == conn {
				log.Printf( "[%s@%s] Connecton closed. %s", identity, addr, err )
				delete(g_mapClientConnections, identity)
			} else {
				// Assume it's because we got replaced by another connection.
				// The other connection already logged, so don't do anything here
			}
			break;
		}

		// Our protocol is just [destination peer identity] [payload]
		// And everything is in text.
		dest_and_msg := strings.SplitN(line, " ", 2)
		if len(dest_and_msg) != 2 {
			log.Printf("[%s@%s] Ignoring weird input '%s' (maybe truncated?)", identity, addr, line)
			continue
		}
		dest_identity := strings.TrimSpace(dest_and_msg[0]);
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
	port := flag.Int("port", 10000, "Port to listen on")
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
		conn,err := listener.Accept()
		if err != nil {
			log.Panic(err)
		}

		// Start goroutine to service it
		go ServiceConnection(conn)
	}

}

