#!/usr/bin/env python3
"""
Minimal STUN (RFC 5389) + TURN (RFC 5766) server

Handles Binding requests (STUN) and Allocate/Refresh/CreatePermission/
Send/Data (TURN).
"""

import argparse
import hashlib
import hmac as _hmac
import os
import select
import socket
import struct
import sys
import time

STUN_MAGIC_COOKIE = 0x2112A442

# Allocations expire after TURN_DEFAULT_LIFETIME seconds if not refreshed.
# A Refresh with LIFETIME=0 immediately releases the allocation.
TURN_DEFAULT_LIFETIME = 600  # seconds (overridden by --allocation-lifetime)

# Long-term credential auth (RFC 5766 §10).  None = no auth required.
_g_turn_realm    = None
_g_turn_nonce    = None  # random bytes, generated at startup; hex-encoded for wire
_g_turn_hmac_key = None  # MD5(username:realm:password)

# Message types
MSG_BINDING_REQUEST             = 0x0001
MSG_BINDING_SUCCESS             = 0x0101
MSG_ALLOCATE_REQUEST            = 0x0003
MSG_ALLOCATE_SUCCESS            = 0x0103
MSG_ALLOCATE_ERROR              = 0x0113
MSG_REFRESH_REQUEST             = 0x0004
MSG_REFRESH_SUCCESS             = 0x0104
MSG_REFRESH_ERROR               = 0x0114
MSG_CREATE_PERMISSION_REQUEST   = 0x0008
MSG_CREATE_PERMISSION_SUCCESS   = 0x0108
MSG_CREATE_PERMISSION_ERROR     = 0x0118
MSG_SEND_INDICATION             = 0x0016
MSG_DATA_INDICATION             = 0x0017

# Attribute types
ATTR_USERNAME                   = 0x0006
ATTR_MESSAGE_INTEGRITY          = 0x0008
ATTR_ERROR_CODE                 = 0x0009
ATTR_REALM                      = 0x0014
ATTR_NONCE                      = 0x0015
ATTR_XOR_MAPPED_ADDRESS         = 0x0020
ATTR_XOR_RELAYED_ADDRESS        = 0x0016
ATTR_XOR_PEER_ADDRESS           = 0x0012
ATTR_DATA                       = 0x0013
ATTR_LIFETIME                   = 0x000D
ATTR_REQUESTED_TRANSPORT        = 0x0019


class Allocation:
    def __init__(self, relay_sock, relay_host, relay_port, server_sock, client_addr, lifetime):
        self.relay_sock   = relay_sock
        self.relay_host   = relay_host
        self.relay_port   = relay_port
        self.server_sock  = server_sock   # server socket to reply on
        self.client_addr  = client_addr   # (ip, port) of TURN client
        self.permissions  = set()         # permitted peer IPs
        self.expiry       = time.monotonic() + lifetime
        self.first_packet = True          # True until the first packet is successfully forwarded

    def is_expired(self):
        return time.monotonic() > self.expiry

    def refresh(self, lifetime):
        self.expiry = time.monotonic() + lifetime


# allocations keyed by (client_ip, client_port)
_allocations = {}
# relay_sock -> Allocation, for dispatching incoming relay packets
_relay_sock_map = {}

# Pending delayed sends: list of (send_at, sock, data, addr) sorted by send_at.
# Used when --relay-latency > 0.
_pending_sends = []
_relay_latency_sec = 0.0


def _schedule_relay_send(sock, data, addr):
    """Send a relay packet, optionally after a configured delay."""
    if _relay_latency_sec <= 0.0:
        sock.sendto(data, addr)
    else:
        _pending_sends.append( (time.monotonic() + _relay_latency_sec, sock, data, addr) )


# ---------------------------------------------------------------------------
# Attribute builders
# ---------------------------------------------------------------------------

def _build_xor_addr_attr(attr_type, addr, port, transaction_id):
    xport = port ^ (STUN_MAGIC_COOKIE >> 16)
    if ':' in addr:
        raw     = socket.inet_pton(socket.AF_INET6, addr)
        xor_key = struct.pack('!I', STUN_MAGIC_COOKIE) + transaction_id
        xraw    = bytes(a ^ b for a, b in zip(raw, xor_key))
        body    = struct.pack('!BBH', 0x00, 0x02, xport) + xraw
    else:
        xip  = struct.unpack('!I', socket.inet_aton(addr))[0] ^ STUN_MAGIC_COOKIE
        body = struct.pack('!BBHI', 0x00, 0x01, xport, xip)
    return struct.pack('!HH', attr_type, len(body)) + body

def _build_lifetime_attr(lifetime):
    return struct.pack('!HHI', ATTR_LIFETIME, 4, lifetime)

def _build_error_attr(code, reason=b''):
    body = struct.pack('!BBBb', 0, 0, code // 100, code % 100) + reason
    pad  = (4 - len(body) % 4) % 4
    return struct.pack('!HH', ATTR_ERROR_CODE, len(body)) + body + b'\x00' * pad

def _build_data_attr(payload):
    pad = (4 - len(payload) % 4) % 4
    return struct.pack('!HH', ATTR_DATA, len(payload)) + payload + b'\x00' * pad

def _build_str_attr(attr_type, text):
    raw = text.encode('utf-8') if isinstance(text, str) else text
    pad = (4 - len(raw) % 4) % 4
    return struct.pack('!HH', attr_type, len(raw)) + raw + b'\x00' * pad

def _build_response(msg_type, transaction_id, *attr_bytes):
    body = b''.join(attr_bytes)
    return struct.pack('!HHI', msg_type, len(body), STUN_MAGIC_COOKIE) + transaction_id + body


# ---------------------------------------------------------------------------
# Long-term credential auth (RFC 5766 §10)
# ---------------------------------------------------------------------------

def _build_401_response(msg_error_type, transaction_id):
    """Build a 401 Unauthorized response with REALM and NONCE."""
    realm_attr = _build_str_attr(ATTR_REALM, _g_turn_realm)
    nonce_attr = _build_str_attr(ATTR_NONCE, _g_turn_nonce)
    error_attr = _build_error_attr(401, b'Unauthorized')
    return _build_response(msg_error_type, transaction_id, error_attr, realm_attr, nonce_attr)


def _check_turn_auth(sock, data, attrs, tid, client_addr, msg_error_type):
    """Return True if auth is not required or credentials are valid.
    Send a 401 and return False otherwise."""
    if _g_turn_hmac_key is None:
        return True

    username_bytes = _get_attr(attrs, ATTR_USERNAME)
    realm_bytes    = _get_attr(attrs, ATTR_REALM)
    nonce_bytes    = _get_attr(attrs, ATTR_NONCE)

    # Find byte offset of MESSAGE-INTEGRITY attribute for HMAC prefix computation.
    mi_pos = None
    offset = 20
    while offset + 4 <= len(data):
        t, l = struct.unpack_from('!HH', data, offset)
        if t == ATTR_MESSAGE_INTEGRITY:
            mi_pos = offset
            break
        offset += 4 + l + (4 - l % 4) % 4

    if username_bytes is None or realm_bytes is None or nonce_bytes is None or mi_pos is None:
        sock.sendto(_build_401_response(msg_error_type, tid), client_addr)
        return False

    # RFC 5389 §15.4: HMAC covers the STUN message up to (not including) the
    # MESSAGE-INTEGRITY attribute, with the length field set as if MESSAGE-INTEGRITY
    # were the last attribute (length = (mi_pos - 20) + 24).
    prefix = bytearray(data[:mi_pos])
    struct.pack_into('!H', prefix, 2, mi_pos - 20 + 24)
    expected_mi = _hmac.new(_g_turn_hmac_key, bytes(prefix), hashlib.sha1).digest()

    mi_value = data[mi_pos + 4: mi_pos + 4 + 20]
    if len(mi_value) != 20 or not _hmac.compare_digest(mi_value, expected_mi):
        sock.sendto(_build_401_response(msg_error_type, tid), client_addr)
        return False

    return True


# ---------------------------------------------------------------------------
# Attribute parser
# ---------------------------------------------------------------------------

def _parse_attrs(data, offset=20):
    """Return list of (attr_type, attr_bytes) pairs."""
    result = []
    while offset + 4 <= len(data):
        attr_type, attr_len = struct.unpack_from('!HH', data, offset)
        offset += 4
        if offset + attr_len > len(data):
            break
        result.append((attr_type, data[offset:offset + attr_len]))
        offset += attr_len + (4 - attr_len % 4) % 4
    return result

def _get_attr(attrs, attr_type):
    for t, v in attrs:
        if t == attr_type:
            return v
    return None

def _get_attrs(attrs, attr_type):
    return [v for t, v in attrs if t == attr_type]

def _decode_xor_addr(attr_bytes, transaction_id):
    """Decode an XOR-PEER/RELAYED/MAPPED address attribute. Returns (ip, port) or (None, None)."""
    if len(attr_bytes) < 4:
        return None, None
    family = attr_bytes[1]
    xport  = struct.unpack_from('!H', attr_bytes, 2)[0]
    port   = xport ^ (STUN_MAGIC_COOKIE >> 16)
    if family == 0x01:   # IPv4
        if len(attr_bytes) < 8:
            return None, None
        xip = struct.unpack_from('!I', attr_bytes, 4)[0]
        ip  = socket.inet_ntoa(struct.pack('!I', xip ^ STUN_MAGIC_COOKIE))
    elif family == 0x02: # IPv6
        if len(attr_bytes) < 20:
            return None, None
        xor_key = struct.pack('!I', STUN_MAGIC_COOKIE) + transaction_id
        raw     = bytes(a ^ b for a, b in zip(attr_bytes[4:20], xor_key))
        ip      = socket.inet_ntop(socket.AF_INET6, raw)
    else:
        return None, None
    return ip, port


# ---------------------------------------------------------------------------
# Message handlers
# ---------------------------------------------------------------------------

def _handle_binding_request(sock, data, addr):
    tid  = data[8:20]
    print("Binding request from %s:%d" % (addr[0], addr[1]), flush=True)
    attr = _build_xor_addr_attr(ATTR_XOR_MAPPED_ADDRESS, addr[0], addr[1], tid)
    sock.sendto(_build_response(MSG_BINDING_SUCCESS, tid, attr), addr)


def _handle_allocate_request(sock, data, addr, server_host):
    tid   = data[8:20]
    key   = (addr[0], addr[1])
    attrs = _parse_attrs(data)
    print("Allocate request from %s:%d" % (addr[0], addr[1]), flush=True)

    if not _check_turn_auth(sock, data, attrs, tid, addr, MSG_ALLOCATE_ERROR):
        return

    if key in _allocations and not _allocations[key].is_expired():
        err = _build_error_attr(437, b'Allocation Mismatch')
        sock.sendto(_build_response(MSG_ALLOCATE_ERROR, tid, err), addr)
        return

    # Clean up any expired allocation for this client before creating a new one
    if key in _allocations:
        _delete_allocation(key)

    family     = socket.AF_INET6 if ':' in server_host else socket.AF_INET
    relay_sock = socket.socket(family, socket.SOCK_DGRAM)
    relay_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if family == socket.AF_INET6:
        relay_sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
    relay_sock.bind((server_host, 0))
    relay_host, relay_port = relay_sock.getsockname()[:2]

    alloc = Allocation(relay_sock, relay_host, relay_port, sock, addr, TURN_DEFAULT_LIFETIME)
    _allocations[key]           = alloc
    _relay_sock_map[relay_sock] = alloc

    print("  Relay %s:%d allocated for %s:%d" % (relay_host, relay_port, addr[0], addr[1]), flush=True)

    relayed = _build_xor_addr_attr(ATTR_XOR_RELAYED_ADDRESS, relay_host, relay_port, tid)
    mapped  = _build_xor_addr_attr(ATTR_XOR_MAPPED_ADDRESS,  addr[0],    addr[1],    tid)
    lftime  = _build_lifetime_attr(TURN_DEFAULT_LIFETIME)
    sock.sendto(_build_response(MSG_ALLOCATE_SUCCESS, tid, relayed, mapped, lftime), addr)


def _handle_refresh_request(sock, data, addr):
    tid   = data[8:20]
    key   = (addr[0], addr[1])
    attrs = _parse_attrs(data)

    if not _check_turn_auth(sock, data, attrs, tid, addr, MSG_REFRESH_ERROR):
        return

    alloc = _allocations.get(key)
    if alloc is None or alloc.is_expired():
        err = _build_error_attr(437, b'No Allocation')
        sock.sendto(_build_response(MSG_REFRESH_ERROR, tid, err), addr)
        return

    lf_bytes = _get_attr(attrs, ATTR_LIFETIME)
    lifetime = struct.unpack('!I', lf_bytes)[0] if lf_bytes else TURN_DEFAULT_LIFETIME

    if lifetime == 0:
        print("Explicit deallocation from %s:%d" % addr, flush=True)
        _delete_allocation(key)
    else:
        alloc.refresh(lifetime)

    sock.sendto(_build_response(MSG_REFRESH_SUCCESS, tid, _build_lifetime_attr(lifetime)), addr)


def _handle_create_permission_request(sock, data, addr):
    tid   = data[8:20]
    key   = (addr[0], addr[1])
    attrs = _parse_attrs(data)

    if not _check_turn_auth(sock, data, attrs, tid, addr, MSG_CREATE_PERMISSION_ERROR):
        return

    alloc = _allocations.get(key)
    if alloc is None or alloc.is_expired():
        err = _build_error_attr(437, b'No Allocation')
        sock.sendto(_build_response(MSG_CREATE_PERMISSION_ERROR, tid, err), addr)
        return
    for peer_bytes in _get_attrs(attrs, ATTR_XOR_PEER_ADDRESS):
        peer_ip, _ = _decode_xor_addr(peer_bytes, tid)
        if peer_ip:
            alloc.permissions.add(peer_ip)
            print("  Permission: %s may reach relay %s:%d" % (peer_ip, alloc.relay_host, alloc.relay_port), flush=True)

    sock.sendto(_build_response(MSG_CREATE_PERMISSION_SUCCESS, tid), addr)


def _handle_send_indication(data, addr):
    tid   = data[8:20]
    key   = (addr[0], addr[1])
    alloc = _allocations.get(key)
    if alloc is None or alloc.is_expired():
        return

    attrs = _parse_attrs(data)
    peer_bytes = _get_attr(attrs, ATTR_XOR_PEER_ADDRESS)
    payload    = _get_attr(attrs, ATTR_DATA)
    if peer_bytes is None or payload is None:
        return

    peer_ip, peer_port = _decode_xor_addr(peer_bytes, tid)
    if peer_ip is None:
        return

    if peer_ip not in alloc.permissions:
        print("  Dropped Send to %s -- no permission" % peer_ip, flush=True)
        return

    _schedule_relay_send(alloc.relay_sock, payload, (peer_ip, peer_port))


def _handle_relay_packet(relay_sock, payload, peer_addr):
    """Data arriving on a relay socket -- wrap in a Data indication and forward to the client."""
    alloc = _relay_sock_map.get(relay_sock)
    if alloc is None:
        return
    if peer_addr[0] not in alloc.permissions:
        print("  Dropped relay packet from %s:%d on relay port %d -- no permission (would forward to %s:%d)" % (peer_addr[0], peer_addr[1], alloc.relay_port, alloc.client_addr[0], alloc.client_addr[1]), flush=True)
        return

    if alloc.first_packet:
        alloc.first_packet = False
        print("  Forwarding first packet from %s:%d on relay port %d to %s:%d" % (peer_addr[0], peer_addr[1], alloc.relay_port, alloc.client_addr[0], alloc.client_addr[1]), flush=True)

    # Transaction ID doesn't matter for indications; use zeros.
    tid      = b'\x00' * 12
    peer_attr = _build_xor_addr_attr(ATTR_XOR_PEER_ADDRESS, peer_addr[0], peer_addr[1], tid)
    data_attr = _build_data_attr(payload)
    indication = _build_response(MSG_DATA_INDICATION, tid, peer_attr, data_attr)
    _schedule_relay_send(alloc.server_sock, indication, alloc.client_addr)


def _handle_packet(sock, data, addr, server_host):
    if len(data) < 20:
        return
    msg_type, _msg_len, magic = struct.unpack_from('!HHI', data)
    if magic != STUN_MAGIC_COOKIE:
        return

    if   msg_type == MSG_BINDING_REQUEST:
        _handle_binding_request(sock, data, addr)
    elif msg_type == MSG_ALLOCATE_REQUEST:
        _handle_allocate_request(sock, data, addr, server_host)
    elif msg_type == MSG_REFRESH_REQUEST:
        _handle_refresh_request(sock, data, addr)
    elif msg_type == MSG_CREATE_PERMISSION_REQUEST:
        _handle_create_permission_request(sock, data, addr)
    elif msg_type == MSG_SEND_INDICATION:
        _handle_send_indication(data, addr)
    else:
        print("Unhandled message type 0x%04x from %s:%d" % (msg_type, addr[0], addr[1]), flush=True)


# ---------------------------------------------------------------------------
# Allocation cleanup
# ---------------------------------------------------------------------------

def _delete_allocation(key):
    alloc = _allocations.pop(key, None)
    if alloc:
        _relay_sock_map.pop(alloc.relay_sock, None)
        alloc.relay_sock.close()

def _cleanup_expired():
    expired = [k for k, a in _allocations.items() if a.is_expired()]
    for k in expired:
        print("Allocation expired for %s:%d" % k, flush=True)
        _delete_allocation(k)


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

def run(host4, host6, port):
    server_socks  = []
    host_by_sock  = {}

    if host4:
        s4 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s4.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s4.bind((host4, port))
        print("STUN/TURN server listening on %s:%d" % (host4, port), flush=True)
        server_socks.append(s4)
        host_by_sock[s4] = host4

    if host6:
        s6 = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        s6.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        s6.bind((host6, port))
        print("STUN/TURN server listening on [%s]:%d" % (host6, port), flush=True)
        server_socks.append(s6)
        host_by_sock[s6] = host6

    while True:
        try:
            # Flush any sends that are ready to go, or figure out when the next one is due.
            select_timeout = 2.0
            now = time.monotonic()
            while _pending_sends:
                remaining = _pending_sends[0][0] - now
                if remaining > 0:
                    select_timeout = remaining
                    break
                _, sock, data, addr = _pending_sends.pop(0)
                sock.sendto(data, addr)

            all_socks = server_socks + list(_relay_sock_map.keys())
            readable, _, _ = select.select(all_socks, [], [], select_timeout)
            for sock in readable:
                data, addr = sock.recvfrom(65535)
                if sock in host_by_sock:
                    _handle_packet(sock, data, addr, host_by_sock[sock])
                else:
                    _handle_relay_packet(sock, data, addr)
            _cleanup_expired()
        except KeyboardInterrupt:
            break
        except Exception as e:
            print("Error: %s" % e, file=sys.stderr, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='STUN (RFC 5389) + TURN (RFC 5766) server')
    parser.add_argument('--host',  default='0.0.0.0',
                        help='IPv4 address to bind (default: 0.0.0.0)')
    parser.add_argument('--host6', default=None,
                        help='IPv6 address to bind (optional)')
    parser.add_argument('--port',  type=int, default=3478,
                        help='UDP port (default: 3478)')
    parser.add_argument('--relay-latency', type=float, default=0.0, metavar='MS',
                        help='Extra one-way latency in milliseconds applied to all relayed packets (default: 0)')
    parser.add_argument('--allocation-lifetime', type=int, default=TURN_DEFAULT_LIFETIME, metavar='SEC',
                        help='TURN allocation lifetime in seconds (default: %d)' % TURN_DEFAULT_LIFETIME)
    parser.add_argument('--username', default=None,
                        help='Require TURN long-term credential auth with this username')
    parser.add_argument('--password', default=None,
                        help='TURN long-term credential password (requires --username)')
    parser.add_argument('--realm', default='steamgaming.local',
                        help='TURN auth realm (default: steamgaming.local)')
    args = parser.parse_args()
    _relay_latency_sec = args.relay_latency / 1000.0
    TURN_DEFAULT_LIFETIME = args.allocation_lifetime
    if args.username and args.password:
        _g_turn_realm    = args.realm
        _g_turn_nonce    = os.urandom(16).hex()
        _g_turn_hmac_key = hashlib.md5(('%s:%s:%s' % (args.username, args.realm, args.password)).encode('utf-8')).digest()
        print("TURN auth enabled: user=%s realm=%s" % (args.username, args.realm), flush=True)
    run(args.host, args.host6, args.port)
