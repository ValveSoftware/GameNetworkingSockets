#!/usr/bin/env python3

# Really simple P2P signaling server.
#
# When establishing peer-to-peer connections, the peers
# need some sort of pre-arranged side channels that they
# can use to exchange messages.  This channel is assumed
# to be relatively low bandwidth and high latency.  This
# service is often called "signaling".
#
# This server has the following really simple protocol:
# It listens on a particular TCP port.  Clients connect
# raw TCP.  The protocol is text-based and line oriented,
# so it is easy to test using telnet.  When a client
# connects, it should send its identity on the first line.
# Afterwards, clients can send a message to a peer by
# sending a line formatted as follows:
#
# DESTINATION_IDENTITY PAYLOAD
#
# Identites may not contain spaces, and the payload
# should be plain ASCII text.  (Hex or base64 encode it).
#
# If there is a client with that destination identity,
# then the server will forward the message on.  Otherwise
# it is discarded.
#
# Forwarded messages have basically the same format and
# are the only type of message the server ever sends to the
# client.  The only difference is that the identity is the
# identity of the sender.
#
# This is just an example code to illustrate what a
# signaling service is.  A real production server would
# probably need to be able to scale across multiple
# processes, and provide authentication and rate
# limiting.
#
# Note that SteamNetworkingSockets use of signaling
# service does NOT assume guaranteed delivery.

import argparse
import errno
import io
import socket
import socketserver
import sys
import threading

DEFAULT_LISTEN_PORT = 10000


class SignalingState(object):
    def __init__(self):
        self.lock = threading.Lock()
        self.clients = {}


class SignalingTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, server_address, request_handler_class, state, address_family=socket.AF_INET, dual_stack=False):
        self.address_family = address_family
        self.dual_stack = dual_stack
        socketserver.TCPServer.__init__(self, server_address, request_handler_class, bind_and_activate=False)

        # For AF_INET6 sockets, ask the OS to also accept IPv4-mapped addresses.
        if self.dual_stack and self.address_family == socket.AF_INET6 and hasattr(socket, "IPV6_V6ONLY"):
            try:
                self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            except OSError:
                pass

        self.server_bind()
        self.server_activate()
        self.state = state


class SignalingRequestHandler(socketserver.StreamRequestHandler):
    def setup(self):
        socketserver.StreamRequestHandler.setup(self)
        try:
            self.request.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)

            # Use more aggressive probing where the platform exposes these knobs.
            if hasattr(socket, "TCP_KEEPIDLE"):
                self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 30)
            if hasattr(socket, "TCP_KEEPINTVL"):
                self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 10)
            if hasattr(socket, "TCP_KEEPCNT"):
                self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 3)
        except OSError:
            # Keepalive tuning is best-effort for this example server.
            pass

    def handle(self):
        peer_addr = self.client_address[0] + ":" + str(self.client_address[1])
        identity = None
        disconnect_reason = None

        try:
            intro = self.rfile.readline()
            if not intro:
                print("[%s] Aborting connection before receiving client identity" % peer_addr)
                return

            identity = intro.strip().decode("utf-8", "ignore")
            if not identity:
                print("[%s] Aborting connection due to empty client identity" % peer_addr)
                return

            replaced_conn = None
            with self.server.state.lock:
                replaced_conn = self.server.state.clients.get(identity)
                self.server.state.clients[identity] = self.request

            if replaced_conn is not None and replaced_conn is not self.request:
                try:
                    print("[%s@%s] Closing previous connection" % (identity, peer_addr))
                    replaced_conn.close()
                except Exception:
                    pass

            print("[%s@%s] Added connection" % (identity, peer_addr))

            while True:
                try:
                    line = self.rfile.readline()
                except ConnectionResetError:
                    disconnect_reason = "peer reset connection"
                    break
                except OSError as ex:
                    if ex.errno == errno.ETIMEDOUT:
                        disconnect_reason = "connection timed out (keepalive failure)"
                    else:
                        disconnect_reason = "socket error %s" % ex
                    break
                if not line:
                    disconnect_reason = "peer closed connection"
                    break

                try:
                    line_text = line.decode("utf-8", "ignore")
                except Exception:
                    continue

                parts = line_text.split(" ", 1)
                if len(parts) != 2:
                    print("[%s@%s] Ignoring weird input '%s'" % (identity, peer_addr, line_text.rstrip()))
                    continue

                dest_identity = parts[0].strip()
                payload = parts[1]
                if not dest_identity:
                    continue

                with self.server.state.lock:
                    dest_conn = self.server.state.clients.get(dest_identity)

                if dest_conn is None:
                    print("[%s@%s] Ignoring, destination peer '%s' not found" % (identity, peer_addr, dest_identity))
                    continue

                out_msg = (identity + " " + payload).encode("utf-8")
                try:
                    dest_conn.sendall(out_msg)
                    print("[%s@%s] -> %s (%d chars)" % (identity, peer_addr, dest_identity, len(payload)))
                except Exception as ex:
                    print("[%s@%s] Failed forwarding to %s: %s" % (identity, peer_addr, dest_identity, ex))
        finally:
            if identity is not None:
                with self.server.state.lock:
                    if self.server.state.clients.get(identity) is self.request:
                        del self.server.state.clients[identity]
                if disconnect_reason is None:
                    disconnect_reason = "connection ended"
                print("[%s@%s] Connection closed: %s" % (identity, peer_addr, disconnect_reason))


def parse_args():
    parser = argparse.ArgumentParser(description="Trivial P2P signaling server")
    parser.add_argument("--port", type=int, default=DEFAULT_LISTEN_PORT, help="Port to listen on")
    return parser.parse_args()


def main():
    # Enable line buffering for stdout so output appears immediately in CI environments
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(line_buffering=True)
    else:
        # Fallback for older Python versions
        sys.stdout = io.TextIOWrapper(sys.stdout.buffer, line_buffering=True)

    args = parse_args()
    listen_addr = ("::", args.port)
    state = SignalingState()

    try:
        server = SignalingTCPServer(
            listen_addr,
            SignalingRequestHandler,
            state,
            address_family=socket.AF_INET6,
            dual_stack=True,
        )
    except OSError:
        # Fall back to IPv4-only if dual-stack IPv6 listener is not available.
        listen_addr = ("0.0.0.0", args.port)
        server = SignalingTCPServer(
            listen_addr,
            SignalingRequestHandler,
            state,
            address_family=socket.AF_INET,
            dual_stack=False,
        )

    print("Listening at %s:%d" % (listen_addr[0], listen_addr[1]))
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
