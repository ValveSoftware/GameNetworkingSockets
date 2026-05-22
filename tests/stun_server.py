#!/usr/bin/env python3
"""
Minimal STUN server implementing RFC 5389 Binding Request/Response.

Supports both IPv4 and IPv6.  Other STUN message types (TURN Allocate,
Send/Data/Channel, etc.) are silently ignored.  Message integrity
(HMAC-SHA1) is not implemented.
"""

import argparse
import select
import socket
import struct
import sys

STUN_MAGIC_COOKIE       = 0x2112A442
MSG_BINDING_REQUEST     = 0x0001
MSG_BINDING_SUCCESS     = 0x0101
ATTR_XOR_MAPPED_ADDRESS = 0x0020

def build_xor_mapped_address(addr, port, transaction_id):
    xport = port ^ (STUN_MAGIC_COOKIE >> 16)
    if ':' in addr:
        # IPv6: XOR 16-byte address with magic_cookie || transaction_id
        raw = socket.inet_pton(socket.AF_INET6, addr)
        xor_key = struct.pack('!I', STUN_MAGIC_COOKIE) + transaction_id
        xraw = bytes(a ^ b for a, b in zip(raw, xor_key))
        attr_body = struct.pack('!BBH', 0x00, 0x02, xport) + xraw
    else:
        # IPv4: XOR 4-byte address with magic cookie
        xip = struct.unpack('!I', socket.inet_aton(addr))[0] ^ STUN_MAGIC_COOKIE
        attr_body = struct.pack('!BBH I', 0x00, 0x01, xport, xip)
    return struct.pack('!HH', ATTR_XOR_MAPPED_ADDRESS, len(attr_body)) + attr_body

def handle_packet(sock, data, addr):
    if len(data) < 20:
        print("Dropped runt packet (%d bytes) from %s:%d" % (len(data), addr[0], addr[1]), flush=True)
        return
    msg_type, _msg_len, magic = struct.unpack_from('!HHI', data)
    if magic != STUN_MAGIC_COOKIE:
        print("Dropped packet from %s:%d: bad magic 0x%08x" % (addr[0], addr[1], magic), flush=True)
        return
    if msg_type != MSG_BINDING_REQUEST:
        print("Dropped packet from %s:%d: unexpected message type 0x%04x" % (addr[0], addr[1], msg_type), flush=True)
        return
    print("Binding request from %s:%d" % (addr[0], addr[1]), flush=True)
    transaction_id = data[8:20]
    attr = build_xor_mapped_address(addr[0], addr[1], transaction_id)
    response = struct.pack('!HHI', MSG_BINDING_SUCCESS, len(attr), STUN_MAGIC_COOKIE) \
               + transaction_id + attr
    sock.sendto(response, addr)

def run(host4, host6, port):
    socks = []

    if host4:
        s4 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s4.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s4.bind((host4, port))
        print("STUN server listening on %s:%d" % (host4, port), flush=True)
        socks.append(s4)

    if host6:
        s6 = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        s6.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        s6.bind((host6, port))
        print("STUN server listening on [%s]:%d" % (host6, port), flush=True)
        socks.append(s6)

    while True:
        try:
            readable, _, _ = select.select(socks, [], [])
            for sock in readable:
                data, addr = sock.recvfrom(2048)
                handle_packet(sock, data, addr)
        except KeyboardInterrupt:
            break
        except Exception as e:
            print("Error: %s" % e, file=sys.stderr, flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='Minimal STUN server (RFC 5389 Binding Request only; no message integrity)')
    parser.add_argument('--host', default='0.0.0.0',
                        help='IPv4 address to bind (default: 0.0.0.0)')
    parser.add_argument('--host6', default=None,
                        help='IPv6 address to bind (optional)')
    parser.add_argument('--port', type=int, default=3478,
                        help='UDP port to listen on (default: 3478)')
    args = parser.parse_args()
    run(args.host, args.host6, args.port)
