#!/usr/bin/env python3
"""
Minimal STUN server implementing RFC 5389 Binding Request/Response.

Other STUN message types (TURN Allocate, Send/Data/Channel, etc.) are
silently ignored.  Message integrity (HMAC-SHA1) is not implemented.
"""

import argparse
import socket
import struct
import sys

STUN_MAGIC_COOKIE       = 0x2112A442
MSG_BINDING_REQUEST     = 0x0001
MSG_BINDING_SUCCESS     = 0x0101
ATTR_XOR_MAPPED_ADDRESS = 0x0020

def run(host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    print("STUN server listening on %s:%d" % (host, port), flush=True)

    while True:
        try:
            data, addr = sock.recvfrom(2048)
            if len(data) < 20:
                print("Dropped runt packet (%d bytes) from %s:%d" % (len(data), addr[0], addr[1]), flush=True)
                continue
            msg_type, _msg_len, magic = struct.unpack_from('!HHI', data)
            if magic != STUN_MAGIC_COOKIE:
                print("Dropped packet from %s:%d: bad magic 0x%08x" % (addr[0], addr[1], magic), flush=True)
                continue
            if msg_type != MSG_BINDING_REQUEST:
                print("Dropped packet from %s:%d: unexpected message type 0x%04x" % (addr[0], addr[1], msg_type), flush=True)
                continue
            print("Binding request from %s:%d" % (addr[0], addr[1]), flush=True)
            transaction_id = data[8:20]

            # XOR-MAPPED-ADDRESS (IPv4)
            xport = addr[1] ^ (STUN_MAGIC_COOKIE >> 16)
            xip   = struct.unpack('!I', socket.inet_aton(addr[0]))[0] ^ STUN_MAGIC_COOKIE
            attr_body = struct.pack('!BBH I', 0x00, 0x01, xport, xip)
            attr = struct.pack('!HH', ATTR_XOR_MAPPED_ADDRESS, len(attr_body)) + attr_body

            response = struct.pack('!HHI', MSG_BINDING_SUCCESS, len(attr), STUN_MAGIC_COOKIE) \
                       + transaction_id + attr
            sock.sendto(response, addr)
        except KeyboardInterrupt:
            break
        except Exception as e:
            print("Error: %s" % e, file=sys.stderr, flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='Minimal STUN server (RFC 5389 Binding Request only; no message integrity)')
    parser.add_argument('--host', default='0.0.0.0',
                        help='IP address to bind (default: 0.0.0.0)')
    parser.add_argument('--port', type=int, default=3478,
                        help='UDP port to listen on (default: 3478)')
    args = parser.parse_args()
    run(args.host, args.port)
