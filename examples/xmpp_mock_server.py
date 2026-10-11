#!/usr/bin/env python3
"""Minimal XMPP mock server for P2P signaling tests and sandbox smoke testing.

Handles lightweight XMPP stream initiation, SASL PLAIN authentication, and
forwards <message type="chat"> stanzas between connected peers.
"""

from __future__ import annotations

import argparse
import base64
import re
import socket
import socketserver
import sys
import threading

DEFAULT_LISTEN_PORT = 5222


class XmppState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.clients: dict[str, socket.socket] = {}

    def register(self, jid: str, sock: socket.socket) -> None:
        with self.lock:
            # Register by full JID and bare username
            self.clients[jid] = sock
            bare = jid.split("/")[0]
            self.clients[bare] = sock
            user = bare.split("@")[0]
            self.clients[user] = sock

    def unregister(self, sock: socket.socket) -> None:
        with self.lock:
            to_remove = [k for k, v in self.clients.items() if v is sock]
            for k in to_remove:
                del self.clients[k]

    def forward(self, to_jid: str, raw_data: bytes) -> bool:
        with self.lock:
            target = self.clients.get(to_jid)
            if target is None:
                bare = to_jid.split("/")[0]
                target = self.clients.get(bare)
            if target is None:
                user = to_jid.split("@")[0]
                target = self.clients.get(user)

            if target is not None:
                try:
                    target.sendall(raw_data)
                    return True
                except OSError:
                    return False
        return False


class XmppServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, server_address: tuple[str, int], handler_cls: type[socketserver.BaseRequestHandler], state: XmppState) -> None:
        super().__init__(server_address, handler_cls)
        self.state = state


class XmppHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        peer_addr = f"{self.client_address[0]}:{self.client_address[1]}"
        print(f"[XMPP] New connection from {peer_addr}", flush=True)

        current_jid = f"unknown_{self.client_address[1]}"
        buf = ""

        try:
            while True:
                chunk = self.request.recv(4096)
                if not chunk:
                    break

                buf += chunk.decode("utf-8", "ignore")

                # Handle stream opening
                if "<stream:stream" in buf:
                    stream_resp = (
                        "<?xml version='1.0'?><stream:stream from='localhost' id='gns_xmpp_1' "
                        "version='1.0' xmlns='jabber:client' xmlns:stream='http://etherx.jabber.org/streams'>"
                        "<stream:features><mechanisms xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                        "<mechanism>PLAIN</mechanism></mechanisms></stream:features>\n"
                    )
                    self.request.sendall(stream_resp.encode("utf-8"))

                # Handle SASL PLAIN auth
                auth_match = re.search(r"<auth [^>]*mechanism=['\"]PLAIN['\"][^>]*>(.*?)</auth>", buf, re.DOTALL)
                if auth_match:
                    auth_b64 = auth_match.group(1).strip()
                    try:
                        decoded = base64.b64decode(auth_b64)
                        parts = decoded.split(b"\x00")
                        # Format is \0username\0password or authzid\0authcid\0password
                        user = ""
                        for p in parts:
                            if p:
                                user = p.decode("utf-8", "ignore")
                                break
                        if user:
                            current_jid = user
                    except Exception:
                        pass

                    self.server.state.register(current_jid, self.request)
                    print(f"[XMPP] Authenticated client '{current_jid}' from {peer_addr}", flush=True)
                    self.request.sendall(b"<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>\n")

                # Handle message forwarding
                while "<message" in buf and "</message>" in buf:
                    msg_start = buf.find("<message")
                    msg_end = buf.find("</message>", msg_start)
                    if msg_end == -1:
                        break

                    full_msg = buf[msg_start : msg_end + 10]
                    buf = buf[msg_end + 10 :]

                    to_match = re.search(r"to=['\"]([^'\"]+)['\"]", full_msg)
                    if to_match:
                        target_jid = to_match.group(1)
                        success = self.server.state.forward(target_jid, (full_msg + "\n").encode("utf-8"))
                        if success:
                            print(f"[XMPP] Forwarded message: {current_jid} -> {target_jid}", flush=True)
                        else:
                            print(f"[XMPP] Discarded message: target '{target_jid}' not found", flush=True)

        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            self.server.state.unregister(self.request)
            print(f"[XMPP] Disconnected: {current_jid} ({peer_addr})", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Minimal XMPP signaling mock server")
    parser.add_argument("--host", default="0.0.0.0", help="Host/IP to bind to (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=DEFAULT_LISTEN_PORT, help=f"TCP port (default: {DEFAULT_LISTEN_PORT})")
    args = parser.parse_args()

    state = XmppState()
    server = XmppServer((args.host, args.port), XmppHandler, state)

    print(f"XMPP mock server listening on {args.host}:{args.port} (Ctrl+C to stop)...", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down XMPP mock server...", flush=True)
    finally:
        server.server_close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
