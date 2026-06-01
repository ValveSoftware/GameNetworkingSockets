#!/usr/bin/env python3

# Test script to run the p2p test.  This runs three processes: the two clients,
# and the dummy signaling service.
#
# NOTE: You usually won't run this script from its original location.  The
# makefiles will copy it into the same location as the tests and examples

import argparse
import subprocess
import threading
import os
import platform
import sys
import copy
import time
import re

g_failed = False
g_server_ready = threading.Event()
g_stun_ready = threading.Event()
g_server_startup_timeout = 3  # seconds
g_spew_level = None
g_p2p_rendezvous_level = None
g_stun_ip = "127.0.100.1"
g_stun_ipv6 = "fd7f:0:100::1"
g_stun_port = 3478
g_setup_mock_ips = False
g_cleanup_mock_ips = False
g_repeat = 1

def ParseArgs():
    global g_spew_level
    global g_p2p_rendezvous_level
    global g_setup_mock_ips
    global g_cleanup_mock_ips
    global g_repeat

    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--spewlevel',
        choices=[ 'msg', 'verbose', 'debug' ],
        help='Control how much diagnostic spew is mirrored to stdio. More detailed output is always available in the per-process file logs.'
    )
    parser.add_argument(
        '--loglevel-p2prendezvous',
        dest='loglevel_p2prendezvous',
        choices=[ 'msg', 'verbose', 'debug' ],
        help='Control detail level specifically for P2P rendezvous-related spew.'
    )
    parser.add_argument(
        '--setup-mock-ips',
        action='store_true',
        help='Add any addresses needed by the mock network that are not already bindable. Exits without running tests.'
    )
    parser.add_argument(
        '--cleanup-mock-ips',
        action='store_true',
        help='Remove addresses added by --setup-mock-ips. Exits without running tests.'
    )
    parser.add_argument(
        '--repeat',
        type=int,
        default=1,
        metavar='N',
        help='Repeat each connection N times (passed through to the test executable).'
    )
    args = parser.parse_args()
    g_spew_level = args.spewlevel
    g_p2p_rendezvous_level = args.loglevel_p2prendezvous
    g_setup_mock_ips = args.setup_mock_ips
    g_cleanup_mock_ips = args.cleanup_mock_ips
    g_repeat = args.repeat

# Thread class that runs a process and captures its output
class RunProcessInThread(threading.Thread):

    def __init__( self, tag, cmdline, env, ready_message=None, ready_event=None, **popen_kwargs ):
        threading.Thread.__init__( self, name=tag )
        self.daemon = True
        self.tag = tag
        self.cmdline = cmdline
        self.ready_message = ready_message
        self.ready_event = ready_event
        if env:
            self.env = env
        else:
            self.env = dict( os.environ )
        self.popen_kwargs = popen_kwargs
        self.log = open( self.tag + ".log", "wt" )
        self.process = None
        self.route_type = None
        self.counters = {}  # populated from TEST_ICE_ctr_xxx=N lines in output

    def WriteLn( self, ln ):
        print( "%s> %s" % (self.tag, ln ) )
        self.log.write( "%s\n" % ln )
        self.log.flush()

    def run( self ):
        global g_failed

        try:
            # Set LD_LIBRARY_PATH
            if os.name == 'posix':
                LD_LIBRARY_PATH = self.env.get( 'LD_LIBRARY_PATH', '' )
                if LD_LIBRARY_PATH: LD_LIBRARY_PATH += ';'
                self.env['LD_LIBRARY_PATH'] = LD_LIBRARY_PATH + "."
                self.WriteLn( "LD_LIBRARY_PATH = '%s'" % self.env['LD_LIBRARY_PATH'])

            self.WriteLn( "Executing: " + ' '.join( self.cmdline ) )
            self.process = subprocess.Popen( self.cmdline, stdout=subprocess.PIPE, stdin=subprocess.PIPE, stderr=subprocess.STDOUT, env=self.env, **self.popen_kwargs )
            self.process.stdin.close()
            while True:
                sOutput = self.process.stdout.readline()
                if sOutput:
                    sOutput = str(sOutput, 'utf-8', 'ignore')
                    self.WriteLn( sOutput.rstrip() )
                    # Check if this is the ready message, then set requested event
                    if self.ready_message and self.ready_event and self.ready_message in sOutput:
                        self.ready_event.set()
                    m = re.search( r'TEST ROUTE: addr=\S+ type=(\w+)', sOutput )
                    if m:
                        self.route_type = m.group(1)
                    m = re.search( r'TEST_ICE_ctr_(\w+)=(\d+)', sOutput )
                    if m:
                        self.counters[m.group(1)] = int( m.group(2) )
                elif self.process.poll() is not None:
                    break
            self.process.wait()
            self.WriteLn( "Exitted with %d" % self.process.returncode )
            if self.process.returncode != 0:
                g_failed = True
        except Exception as ex:
            self.WriteLn( "FAILED to execute process: %s" % ex )
            g_failed = True

    # Wait for thread to shutdown.  Nuke process if we don't exit in time
    def join( self, timeout ):
        threading.Thread.join( self, timeout )
        if self.is_alive():
            self.WriteLn( "Still running after %d seconds.  Killing" % timeout )
            global g_failed
            g_failed = True
            if self.process is not None:
                self.process.kill()

    # Attempt graceful shutdown
    def term( self ):
        self.WriteLn( "Attempting graceful shutdown" )
        if self.process is not None:
            self.process.terminate()
        self.join( 5 )

def StartProcessInThread( tag, cmdline, env=None, ready_message=None, ready_event=None, **popen_kwargs ):
    thread = RunProcessInThread( tag, cmdline, env, ready_message=ready_message, ready_event=ready_event, **popen_kwargs )
    thread.start()
    return thread

_DEFAULT_STUN = object()  # sentinel: use the shared STUN server
_DEFAULT_TURN = object()  # sentinel: use the shared TURN server

# Credentials used by the shared STUN/TURN server and all test clients.
_TURN_USERNAME = 'testuser'
_TURN_PASSWORD = 'testpass'

def StartClientInThread( role, local, remote, extra_args=[], stun=_DEFAULT_STUN, turn=_DEFAULT_TURN ):
    cmdline = [
        "./test_p2p",
        "--" + role,
        "--identity-local", "str:"+local,
        "--identity-remote", "str:"+remote,
        "--signaling-server", "localhost:10000",
        "--log", local + ".verbose.log"
    ]

    if stun is _DEFAULT_STUN:
        cmdline += [ '--stun-server', "%s:%d,[%s]:%d" % (g_stun_ip, g_stun_port, g_stun_ipv6, g_stun_port) ]
    elif stun is not None:
        cmdline += [ '--stun-server', stun ]
    # stun=None: omit --stun-server entirely (executable uses its built-in default)

    if turn is _DEFAULT_TURN:
        cmdline += [ '--turn-server', "%s:%d" % (g_stun_ip, g_stun_port) ]
        cmdline += [ '--turn-username', _TURN_USERNAME, '--turn-password', _TURN_PASSWORD ]
    elif turn is not None:
        cmdline += [ '--turn-server', turn ]
    # turn=None: omit --turn-server entirely (no relay)

    if g_repeat > 1:
        cmdline += [ '--repeat', str(g_repeat) ]
    cmdline += extra_args
    if g_spew_level is not None:
        cmdline.append( '--spewlevel=' + g_spew_level )
    if g_p2p_rendezvous_level is not None:
        cmdline.append( '--loglevel-p2prendezvous=' + g_p2p_rendezvous_level )

    env = dict( os.environ )
    if os.name == 'nt' and not os.path.exists( 'steamnetworkingsockets.dll' ) and not os.path.exists( 'GameNetworkingSockets.dll' ):
        bindir = os.path.abspath('../../../bin')
        if not os.path.exists( bindir ):
            print( "Can't find steamnetworkingsockets.dll" )
            sys.exit(1)
        env['PATH'] = os.path.join( bindir, 'win64' ) + ';' + os.path.join( bindir, 'win32' ) + ';' + env['PATH']

    return StartProcessInThread( local, cmdline, env );

# Mock network address constants -- IPv4
# Public range: 127.0.100.x  Private LANs: 127.0.X.x (X != 100)
_SRV_GW   = '127.0.100.2'  # server-side NAT gateway (public)
_CLI_GW   = '127.0.100.3'  # client-side NAT gateway (public)
_SRV_GW2  = '127.0.100.4'  # second server gateway (public)
_CLI_GW2  = '127.0.100.5'  # second client gateway (public)
_SRV_INT  = '127.0.1.2'    # server internal address behind NAT
_CLI_INT  = '127.0.2.2'    # client internal address behind NAT
_SRV_INT2 = '127.0.3.2'    # second server internal address
_CLI_INT2 = '127.0.4.2'    # second client internal address
_DEAD_INT = '127.0.9.2'    # address used for disabled adapters
_CLI_SAME_LAN = '127.0.1.3' # client on the same /24 private LAN as _SRV_INT

# Mock network address constants -- IPv6
# Mirrors the IPv4 layout: fd7f:0:100::x = public, fd7f:0:X::x = private LAN X
_SRV_GW_V6  = 'fd7f:0:100::2'  # server-side NAT gateway (public, IPv6)
_CLI_GW_V6  = 'fd7f:0:100::3'  # client-side NAT gateway (public, IPv6)
_SRV_INT_V6 = 'fd7f:0:1::2'    # server internal address behind NAT (IPv6)
_CLI_INT_V6 = 'fd7f:0:2::2'    # client internal address behind NAT (IPv6)

# All addresses that the mock network needs to be able to bind sockets to.
_ALL_MOCK_ADDRS = [
    g_stun_ip,
    _SRV_GW, _CLI_GW, _SRV_GW2, _CLI_GW2,
    _SRV_INT, _CLI_INT, _SRV_INT2, _CLI_INT2, _DEAD_INT, _CLI_SAME_LAN,
    g_stun_ipv6,
    _SRV_GW_V6, _CLI_GW_V6, _SRV_INT_V6, _CLI_INT_V6,
]

def _IsAddressBindable( addr ):
    import socket
    family = socket.AF_INET6 if ':' in addr else socket.AF_INET
    try:
        s = socket.socket( family, socket.SOCK_DGRAM )
        s.bind( ( addr, 0 ) )
        s.close()
        return True
    except OSError:
        return False

_WINDOWS_LOOPBACK_IFACE = 'Loopback Pseudo-Interface 1'

def _AddLoopbackAddr( addr ):
    is_ipv6 = ':' in addr
    sys_name = platform.system()
    if sys_name == 'Darwin':
        if is_ipv6:
            print( "Running 'ifconfig lo0 inet6 %s'" % addr )
            subprocess.run( [ 'ifconfig', 'lo0', 'inet6', addr ], check=True )
        else:
            print( "Running 'ifconfig lo0 alias %s'" % addr )
            subprocess.run( [ 'ifconfig', 'lo0', 'alias', addr ], check=True )
    elif sys_name == 'Linux':
        if is_ipv6:
            print( "Running 'ip -6 addr add %s/112 dev lo'" % addr )
            subprocess.run( [ 'ip', '-6', 'addr', 'add', addr + '/112', 'dev', 'lo' ], check=True )
        # IPv4 on Linux: the entire 127/8 block is routable on lo, nothing to do.
    elif sys_name == 'Windows':
        if is_ipv6:
            print( "Running 'netsh interface ipv6 add address \"%s\" %s'" % ( _WINDOWS_LOOPBACK_IFACE, addr ) )
            subprocess.run( [ 'netsh', 'interface', 'ipv6', 'add', 'address',
                               _WINDOWS_LOOPBACK_IFACE, addr ], check=True )
        # IPv4 on Windows: the entire 127/8 block is routable on the loopback adapter, nothing to do.

def _RemoveLoopbackAddr( addr ):
    is_ipv6 = ':' in addr
    sys_name = platform.system()
    if sys_name == 'Darwin':
        if is_ipv6:
            print( "Running 'ifconfig lo0 inet6 %s delete'" % addr )
            subprocess.run( [ 'ifconfig', 'lo0', 'inet6', addr, 'delete' ], check=False )
        else:
            print( "Running 'ifconfig lo0 -alias %s'" % addr )
            subprocess.run( [ 'ifconfig', 'lo0', '-alias', addr ], check=False )
    elif sys_name == 'Linux':
        if is_ipv6:
            print( "Running 'ip -6 addr del %s/112 dev lo'" % addr )
            subprocess.run( [ 'ip', '-6', 'addr', 'del', addr + '/112', 'dev', 'lo' ], check=False )
        # IPv4 on Linux: nothing was added, nothing to remove.
    elif sys_name == 'Windows':
        if is_ipv6:
            print( "Running 'netsh interface ipv6 delete address \"%s\" %s'" % ( _WINDOWS_LOOPBACK_IFACE, addr ) )
            subprocess.run( [ 'netsh', 'interface', 'ipv6', 'delete', 'address',
                               _WINDOWS_LOOPBACK_IFACE, addr ], check=False )
        # IPv4 on Windows: nothing was added, nothing to remove.

def SetupMockIPs():
    """Add loopback aliases for every mock address that is not already bindable."""
    for addr in _ALL_MOCK_ADDRS:
        _AddLoopbackAddr( addr )
    print( "Setup complete." )

def CleanupMockIPs():
    """Remove loopback aliases added by --setup-mock-ips."""
    for addr in _ALL_MOCK_ADDRS:
        if addr in ( '127.0.0.1', '::1' ):
            continue
        _RemoveLoopbackAddr( addr )
    print( "Cleanup complete." )

def CheckMockIPsBindable():
    """Verify all mock addresses are bindable; exit with an error if any are not."""
    missing = [ addr for addr in _ALL_MOCK_ADDRS if not _IsAddressBindable( addr ) ]
    if not missing:
        return
    print( "ERROR: the following addresses required by the mock network are not bindable:" )
    for addr in missing:
        print( "  " + addr )
    if platform.system() == 'Windows':
        print( "Run '%s --setup-mock-ips' from an elevated (Administrator) prompt." % sys.argv[0] )
    else:
        print( "Run 'sudo %s --setup-mock-ips' to add the required loopback aliases." % sys.argv[0] )
    sys.exit(1)

def _nat( internal, gateway, nat_type ):
    # Gateway must be declared before the adapter that uses it
    return [ '--mock-gateway', gateway, '--mock-nat', nat_type, '--mock-adapter', internal ]

def _disabled_adapter( ip ):
    return [ '--mock-adapter', ip, '--mock-disabled' ]

def _slow_nat( internal, gateway, nat_type, latency_ms ):
    return [ '--mock-gateway', gateway, '--mock-nat', nat_type, '--mock-adapter', internal, '--mock-latency', str(latency_ms) ]

def _parse_candidate_log( filename ):
    """
    Parse a verbose log file for local and remote candidate counts.
    Returns (local_dict, remote_dict) mapping candidate type -> count.
    Local  = lines containing 'LocalCandidateAdded' (candidates this peer gathered).
    Remote = lines containing 'Got remote candidate'  (candidates received from peer).
    """
    local, remote = {}, {}
    try:
        with open( filename, 'rt', errors='replace' ) as f:
            for line in f:
                if 'LocalCandidateAdded' in line:
                    m = re.search( r'\btyp (\w+)', line )
                    if m:
                        t = m.group(1)
                        local[t] = local.get(t, 0) + 1
                if 'Got remote candidate' in line:
                    m = re.search( r'\btyp (\w+)', line )
                    if m:
                        t = m.group(1)
                        remote[t] = remote.get(t, 0) + 1
    except FileNotFoundError:
        pass
    return local, remote


# Address used for "server is down" tests: valid loopback IP but no STUN/TURN listening.
# Packets are sent successfully but never answered, so the connection timeout drives failure.
_DEAD_SERVER = '%s:9999' % g_stun_ip


def ClientServerExpectedFailureTest( server_extra_args=[], client_extra_args=[], ice_impl=1,
                                     stun=_DEFAULT_STUN, turn=_DEFAULT_TURN,
                                     expected_counters=None, expected_candidates=None ):
    """Run a test where both sides are expected to fail to connect."""
    global g_failed
    impl_args = [ '--ice-implementation', str(ice_impl) ]
    fail_args = [ '--expect-failure' ]
    server = StartClientInThread( "server", "peer_server", "peer_client",
                                  server_extra_args + impl_args + fail_args,
                                  stun=stun, turn=turn )
    client = StartClientInThread( "client", "peer_client", "peer_server",
                                  client_extra_args + impl_args + fail_args,
                                  stun=stun, turn=turn )

    server.join( timeout=30 )
    client.join( timeout=30 )

    if expected_counters is not None and g_repeat == 1:
        for peer, thread in [ ( 'server', server ), ( 'client', client ) ]:
            for name, (lo, hi) in expected_counters.items():
                val = thread.counters.get( name, 0 )
                if lo is not None and val < lo:
                    print( "ERROR: %s TEST_ICE_ctr_%s=%d, expected >= %d" % ( peer, name, val, lo ) )
                    g_failed = True
                if hi is not None and val > hi:
                    print( "ERROR: %s TEST_ICE_ctr_%s=%d, expected <= %d" % ( peer, name, val, hi ) )
                    g_failed = True

    if g_repeat == 1:
        srv_local, srv_remote = _parse_candidate_log( "peer_server.verbose.log" )
        cli_local, cli_remote = _parse_candidate_log( "peer_client.verbose.log" )
        if expected_candidates is not None:
            exp_srv, exp_cli = expected_candidates
            if exp_srv is not None and srv_local != exp_srv:
                print( "ERROR: server gathered %s, expected %s" % ( srv_local, exp_srv ) )
                g_failed = True
            if exp_cli is not None and cli_local != exp_cli:
                print( "ERROR: client gathered %s, expected %s" % ( cli_local, exp_cli ) )
                g_failed = True


# Failure test cases: ( description, server_args, client_args, kwargs_for_failure_test )
# 'stun' and 'turn' kwargs override the server address; None = omit entirely.
# _CAND_NAT_NO_TURN = behind NAT + STUN works, but no relay (TURN not configured or failed)
_CAND_NAT_NO_TURN = {'host': 1, 'srflx': 1}

FAILURE_TEST_CASES = [
    # STUN is unavailable: no srflx gathered, no relay, host candidates can't cross NAT subnets.
    # The STUN binding request retransmits 4 times (5 total sends) before giving up at ~5.3s,
    # then the connection timeout fires at 10s.
    ( 'STUN unavailable (full-cone NAT, no TURN)',
      _nat( _SRV_INT, _SRV_GW, 'full-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ),
      dict( stun=_DEAD_SERVER, turn=None,
            expected_counters={
                'allocate_send':      (0, 0),
                'data_ind_recv':      (0, 0),
                'binding_req_retx':   (4, 4),  # 5 sends total: 1 initial + 4 retransmits
                'allocate_retx':      (0, 0),
            },
            expected_candidates=( {'host': 1}, {'host': 1} ) ) ),

    # TURN not configured: symmetric NAT requires relay; without it the connection must fail.
    # Connectivity checks to srflx candidates retransmit 4 times before giving up.
    ( 'TURN not configured (symmetric NAT)',
      _nat( _SRV_INT, _SRV_GW, 'symmetric' ),
      _nat( _CLI_INT, _CLI_GW, 'symmetric' ),
      dict( turn=None,
            expected_counters={
                'allocate_send':      (0, 0),
                'data_ind_recv':      (0, 0),
                'binding_req_retx':   (4, 4),
                'allocate_retx':      (0, 0),
            },
            expected_candidates=( _CAND_NAT_NO_TURN, _CAND_NAT_NO_TURN ) ) ),

    # TURN server unreachable: allocation requests are sent but never answered.
    # STUN works so srflx is gathered, but symmetric NAT blocks direct paths and relay fails.
    # Both the srflx connectivity checks and the TURN allocation each retransmit 4 times.
    ( 'TURN unreachable (symmetric NAT)',
      _nat( _SRV_INT, _SRV_GW, 'symmetric' ),
      _nat( _CLI_INT, _CLI_GW, 'symmetric' ),
      dict( turn=_DEAD_SERVER,
            expected_counters={
                'allocate_send':      (1, None),
                'data_ind_recv':      (0, 0),
                'binding_req_retx':   (4, 4),
                'allocate_retx':      (4, 4),
            },
            expected_candidates=( _CAND_NAT_NO_TURN, _CAND_NAT_NO_TURN ) ) ),

    # TURN wrong password: the server sends a 401 challenge; the client retries with a
    # bad HMAC (wrong password) and gets a second 401, marking relay as failed.
    # allocate_send=2: initial (no auth) + one retry (wrong credentials).
    # No retransmits because the server responds immediately each time.
    ( 'TURN wrong password (symmetric NAT)',
      _nat( _SRV_INT, _SRV_GW, 'symmetric' ) + [ '--turn-password', 'wrongpass' ],
      _nat( _CLI_INT, _CLI_GW, 'symmetric' ) + [ '--turn-password', 'wrongpass' ],
      dict( expected_counters={
                'allocate_send':      (2, 2),
                'allocate_retx':      (0, 0),
                'data_ind_recv':      (0, 0),
                'binding_req_retx':   (4, 4),
            },
            expected_candidates=( _CAND_NAT_NO_TURN, _CAND_NAT_NO_TURN ) ) ),

    # TURN auth required but no credentials configured: the server sends a 401 challenge;
    # the client has no username so it does not retry, marking relay as failed immediately.
    # allocate_send=1: only the initial unauthenticated request.
    ( 'TURN auth required, no credentials (symmetric NAT)',
      _nat( _SRV_INT, _SRV_GW, 'symmetric' ),
      _nat( _CLI_INT, _CLI_GW, 'symmetric' ),
      dict( turn='%s:%d' % (g_stun_ip, g_stun_port),  # server address only, no credentials
            expected_counters={
                'allocate_send':      (1, 1),
                'allocate_retx':      (0, 0),
                'data_ind_recv':      (0, 0),
                'binding_req_retx':   (4, 4),
            },
            expected_candidates=( _CAND_NAT_NO_TURN, _CAND_NAT_NO_TURN ) ) ),
]


# Counter constraint dicts: map short counter name -> (min, max), None = no bound.
# Applied to both server and client after each test.
# Only the relay-path counters are pinned; binding_req counts vary with retransmit timing.
_CTR_RELAY = {          # TURN relay path was used for data
    'allocate_send':  (1, None),
    'send_ind_send':  (1, None),
    'data_ind_recv':  (1, None),
}
_CTR_DIRECT = {         # Direct path; TURN was allocated (but relay candidates are still probed)
    'allocate_send':  (1, None),
}
_CTR_DIRECT_NO_TURN = { # Direct path; no TURN allocated (e.g. IPv6-only adapter with IPv4-only TURN server)
    'allocate_send':  (0, 0),
    'send_ind_send':  (0, 0),
    'data_ind_recv':  (0, 0),
}

# Candidate count dicts: map candidate type -> expected count for one connection.
# Only checked when g_repeat == 1 (log files accumulate across repeats).
# srflx is absent when the mapped address equals the host address (no NAT).
_CAND_NAT_TURN    = {'host': 1, 'srflx': 1, 'relay': 1}  # single adapter behind NAT, TURN configured
_CAND_DIRECT_TURN = {'host': 1, 'relay': 1}               # single adapter, no NAT, TURN configured
_CAND_IPV6_NAT    = {'host': 1, 'srflx': 1}               # single IPv6 adapter behind NAT, no IPv6 TURN
_CAND_IPV6_DIRECT = {'host': 1}                            # single IPv6 adapter, no NAT, no IPv6 TURN
_CAND_MULTI       = {'host': 2, 'srflx': 1, 'relay': 2}   # two adapters: one public (no srflx) + one NATd

# Each entry: ( description, server_extra_args, client_extra_args, expected_route, ice_impl,
#               counter_constraints, (server_expected_candidates, client_expected_candidates) )
# candidate pair is None to skip the check (e.g. no-mock tests with unpredictable real adapters)
# Both sides must report the same route type -- ICE nominates one candidate pair
# and both ends classify the same path, so agreement is guaranteed by the protocol.
# Route types: 'local' = Fast flag set: both host candidates on the same private /24 LAN subnet
#              'udp'   = direct UDP but not same-LAN (NAT traversal, or public IPs)
#              'relay' = TURN relay
# ice_impl: 0 = default (WebRTC if compiled in), 1 = force native ICE client.
# All mocked tests must use 1: the mock network is only wired into the native ICE path.
# The two no-mock entries exercise both implementations on the real loopback network.
CLIENT_SERVER_TEST_CASES = [
    # Symmetric NAT on both sides: reflexive candidates cannot traverse this NAT type,
    # so connectivity requires TURN relay.  This test is expected to fail until the
    # relay data path (Send/Data Indication) is implemented.
    ( 'symmetric NAT vs symmetric NAT (requires TURN relay)',
      _nat( _SRV_INT, _SRV_GW, 'symmetric' ),
      _nat( _CLI_INT, _CLI_GW, 'symmetric' ),
      'relay', 1, _CTR_RELAY,
      ( _CAND_NAT_TURN, _CAND_NAT_TURN ) ),

    # No-mock tests: run on real network (same host), both implementations.
    # No STUN/TURN: a local STUN server can't reveal a real public IP, and a local
    # TURN relay allocating loopback addresses is meaningless.  Host candidates suffice.
    # No counter or candidate constraints: real adapters vary by host.
    ( 'no-mock, default ICE implementation',
      [], [],
      'local', 0, None, None, {'stun': None, 'turn': None} ),
    ( 'no-mock, native ICE implementation',
      [], [],
      'local', 1, None, None, {'stun': None, 'turn': None} ),

    # Both on the same private /24 LAN: the core case for 'local' classification.
    # No NAT, so STUN mapped address == host address; srflx is suppressed.
    ( 'same LAN (private subnet, no NAT)',
      [ '--mock-adapter', _SRV_INT ],
      [ '--mock-adapter', _CLI_SAME_LAN ],
      'local', 1, _CTR_DIRECT,
      ( _CAND_DIRECT_TURN, _CAND_DIRECT_TURN ) ),

    # Both on the same LAN but each also has a NAT to the public internet via the
    # same shared gateway -- the typical home/office scenario.  Both a direct host path
    # and a hairpin path through the gateway exist; ICE must select the direct host
    # path (higher priority) and classify it as 'local'.
    ( 'same LAN, shared gateway (hairpin)',
      _nat( _SRV_INT, _SRV_GW, 'full-cone' ),
      _nat( _CLI_SAME_LAN, _SRV_GW, 'full-cone' ),
      'local', 1, _CTR_DIRECT,
      ( _CAND_NAT_TURN, _CAND_NAT_TURN ) ),

    # Both on the public network: direct host-to-host but NOT 'local' -- public IPs
    # are not classified as fast even when they share a subnet.
    # No NAT: STUN mapped address == host address; srflx is suppressed.
    ( 'no-nat (both public)',
      [ '--mock-adapter', _SRV_GW ],
      [ '--mock-adapter', _CLI_GW ],
      'udp', 1, _CTR_DIRECT,
      ( _CAND_DIRECT_TURN, _CAND_DIRECT_TURN ) ),

    ( 'full-cone NAT',
      _nat( _SRV_INT, _SRV_GW, 'full-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ),
      'udp', 1, _CTR_DIRECT,
      ( _CAND_NAT_TURN, _CAND_NAT_TURN ) ),
    ( 'restricted-cone NAT',
      _nat( _SRV_INT, _SRV_GW, 'restricted-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'restricted-cone' ),
      'udp', 1, _CTR_DIRECT,
      ( _CAND_NAT_TURN, _CAND_NAT_TURN ) ),
    ( 'port-restricted-cone NAT',
      _nat( _SRV_INT, _SRV_GW, 'port-restricted-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'port-restricted-cone' ),
      'udp', 1, _CTR_DIRECT,
      ( _CAND_NAT_TURN, _CAND_NAT_TURN ) ),
    ( 'asymmetric: server public, client full-cone',
      [ '--mock-adapter', _SRV_GW ],
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ),
      'udp', 1, _CTR_DIRECT,
      ( _CAND_DIRECT_TURN, _CAND_NAT_TURN ) ),
    ( 'asymmetric: server full-cone, client symmetric',
      _nat( _SRV_INT, _SRV_GW, 'full-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'symmetric' ),
      'udp', 1, _CTR_DIRECT,
      ( _CAND_NAT_TURN, _CAND_NAT_TURN ) ),

    # Disabled adapter: verify connection still succeeds when one adapter is down.
    # The disabled adapter contributes no candidates.
    ( 'server has disabled second adapter',
      [ '--mock-adapter', _SRV_GW ] + _disabled_adapter( _DEAD_INT ),
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ),
      'udp', 1, _CTR_DIRECT,
      ( _CAND_DIRECT_TURN, _CAND_NAT_TURN ) ),
    ( 'client has disabled second adapter',
      [ '--mock-adapter', _SRV_GW ],
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ) + _disabled_adapter( _DEAD_INT ),
      'udp', 1, _CTR_DIRECT,
      ( _CAND_DIRECT_TURN, _CAND_NAT_TURN ) ),

    # Multi-adapter with latency: fast public adapter + slow NATd adapter.
    # ICE should prefer the low-latency host-to-host path.  Both public adapters
    # are on 127.0.100.x (not a private subnet) so the route is 'udp', not 'local'.
    ( 'both multi-adapter: fast public + slow NATd',
      [ '--mock-adapter', _SRV_GW ] + _slow_nat( _SRV_INT2, _SRV_GW2, 'full-cone', 50 ),
      [ '--mock-adapter', _CLI_GW ] + _slow_nat( _CLI_INT2, _CLI_GW2, 'full-cone', 50 ),
      'udp', 1, _CTR_DIRECT,
      ( _CAND_MULTI, _CAND_MULTI ) ),

    # IPv6 host candidates: both endpoints have a public IPv6 address, no NAT.
    # fd7f:0:100::x is the mock public IPv6 network (not classified as 'local').
    # No TURN allocation: the TURN server is IPv4-only; IPv6 adapters skip it.
    # No srflx: STUN mapped address equals host address (no NAT).
    ( 'IPv6 no-nat (both public)',
      [ '--mock-adapter', _SRV_GW_V6 ],
      [ '--mock-adapter', _CLI_GW_V6 ],
      'udp', 1, _CTR_DIRECT_NO_TURN,
      ( _CAND_IPV6_DIRECT, _CAND_IPV6_DIRECT ) ),

    # IPv6 full-cone NAT: STUN mapped address differs from host, so srflx is published.
    ( 'IPv6 full-cone NAT',
      _nat( _SRV_INT_V6, _SRV_GW_V6, 'full-cone' ),
      _nat( _CLI_INT_V6, _CLI_GW_V6, 'full-cone' ),
      'udp', 1, _CTR_DIRECT_NO_TURN,
      ( _CAND_IPV6_NAT, _CAND_IPV6_NAT ) ),

    # Packet loss: relay can win the initial nomination race over the direct path
    # (relay responses bypass the lossy adapter; see the route-upgrade commit for the
    # full analysis), but the ICE client now continues probing higher-priority pairs
    # after selection and upgrades when one succeeds.  We therefore expect to end up
    # on 'udp' (direct srflx path).
    #
    # Spurious-failure analysis (at the time of this writing):
    #   Each STUN round trip succeeds with P = 0.9 * 0.9 = 0.81 under 10% outbound
    #   loss per side.  The request schedule is 5 total sends (1 initial + 4 retx).
    #   P(all 5 fail) = 0.19^5 ~= 0.025% (~1 in 4000 runs).  Triggered checks from
    #   the remote side add extra attempts, so the real rate is lower still.
    ( 'full-cone NAT, 10% packet loss',
      [ '--mock-loss', '10' ] + _nat( _SRV_INT, _SRV_GW, 'full-cone' ),
      [ '--mock-loss', '10' ] + _nat( _CLI_INT, _CLI_GW, 'full-cone' ),
      'udp', 1, _CTR_DIRECT, None ),

    # Signaling impairment: verify connection succeeds despite lossy or duplicate
    # signals.  Uses real loopback (no mock network), so the route is always 'local'.
    # Signaling loss only slows setup (the library retransmits rendezvous messages);
    # it does not affect route selection or ICE counter values in a predictable way.
    # ICE_Enable filtering: TURN server is configured but relay candidates are excluded
    # by ICE_Enable flags (Private=2 + Public=4, no Relay=1).  Both sides are on
    # full-cone NAT so srflx connectivity works; relay candidates must NOT appear.
    ( 'full-cone NAT, relay excluded by ICE_Enable',
      _nat( _SRV_INT, _SRV_GW, 'full-cone' ) + [ '--ice-enable', '6' ],
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ) + [ '--ice-enable', '6' ],
      'udp', 1, _CTR_DIRECT_NO_TURN,
      ( {'host': 1, 'srflx': 1}, {'host': 1, 'srflx': 1} ) ),

    # Public IP disclosure disabled (ICE_Enable = Private=2 + Relay=1, no Public=4).
    # No srflx candidates gathered or shared; no STUN binding requests sent for srflx.
    # Same /24 LAN: direct path via private host candidates wins; relay is allocated
    # but not used for data.
    ( 'no public IP, same LAN',
      [ '--mock-adapter', _SRV_INT, '--ice-enable', '3' ],
      [ '--mock-adapter', _CLI_SAME_LAN, '--ice-enable', '3' ],
      'local', 1,
      { 'srflx_send': (0, 0), 'allocate_send': (1, None) },
      ( {'host': 1, 'relay': 1}, {'host': 1, 'relay': 1} ) ),

    # Public IP disclosure disabled (ICE_Enable = Private + Relay, no Public).
    # Symmetric NAT: private hosts can't punch through, no srflx to assist.
    # Falls back to TURN relay.  Verifies srflx_send=0 (no STUN binding attempt).
    ( 'no public IP, symmetric NAT (relay fallback)',
      _nat( _SRV_INT, _SRV_GW, 'symmetric' ) + [ '--ice-enable', '3' ],
      _nat( _CLI_INT, _CLI_GW, 'symmetric' ) + [ '--ice-enable', '3' ],
      'relay', 1,
      { 'srflx_send': (0, 0), 'allocate_send': (1, None),
        'send_ind_send': (1, None), 'data_ind_recv': (1, None) },
      ( {'host': 1, 'relay': 1}, {'host': 1, 'relay': 1} ) ),

    ( 'no-mock native ICE, 30% signaling loss',
      [ '--signaling-loss', '30' ],
      [ '--signaling-loss', '30' ],
      'local', 1, None, None ),
    ( 'no-mock native ICE, 50% signaling duplicates',
      [ '--signaling-dup', '50' ],
      [ '--signaling-dup', '50' ],
      'local', 1, None, None ),
]

def ClientServerTest( server_extra_args=[], client_extra_args=[], expected_route=None, ice_impl=1, expected_counters=None, expected_candidates=None, timeout_sec=None, stun=_DEFAULT_STUN, turn=_DEFAULT_TURN ):
    global g_failed
    impl_args = [ '--ice-implementation', str(ice_impl) ]
    server = StartClientInThread( "server", "peer_server", "peer_client", server_extra_args + impl_args, stun=stun, turn=turn )
    client = StartClientInThread( "client", "peer_client", "peer_server", client_extra_args + impl_args, stun=stun, turn=turn )

    # Wait for clients to shutdown.  Nuke them if necessary
    t = timeout_sec if timeout_sec is not None else 20 * g_repeat
    server.join( timeout=t )
    client.join( timeout=t )

    # Verify route types if an expected value was provided
    if expected_route is not None:
        for peer, thread in [ ( 'server', server ), ( 'client', client ) ]:
            if thread.route_type is None:
                print( "ERROR: %s did not report a route type" % peer )
                g_failed = True
            elif thread.route_type != expected_route:
                print( "ERROR: %s route type '%s', expected '%s'" % ( peer, thread.route_type, expected_route ) )
                g_failed = True

    # Verify packet counters if constraints were provided
    if expected_counters is not None:
        for peer, thread in [ ( 'server', server ), ( 'client', client ) ]:
            for name, (lo, hi) in expected_counters.items():
                val = thread.counters.get( name, 0 )
                if lo is not None and val < lo:
                    print( "ERROR: %s TEST_ICE_ctr_%s=%d, expected >= %d" % ( peer, name, val, lo ) )
                    g_failed = True
                if hi is not None and val > hi:
                    print( "ERROR: %s TEST_ICE_ctr_%s=%d, expected <= %d" % ( peer, name, val, hi ) )
                    g_failed = True

    # Candidate checks only make sense for a single connection (log files accumulate across repeats).
    if g_repeat == 1:
        srv_local, srv_remote = _parse_candidate_log( "peer_server.verbose.log" )
        cli_local, cli_remote = _parse_candidate_log( "peer_client.verbose.log" )

        # Cross-check: received candidates must be a subset of what the other side gathered.
        # Strict equality would fail on fast connections (e.g. same-LAN) where the
        # connection closes before late-arriving relay candidates finish signaling.
        def _check_subset( received, gathered, receiver, gatherer ):
            for ctype, count in received.items():
                if count > gathered.get( ctype, 0 ):
                    print( "ERROR: %s received %d %s candidate(s) from %s but %s only gathered %d" % (
                        receiver, count, ctype, gatherer, gatherer, gathered.get( ctype, 0 ) ) )
                    g_failed = True
        _check_subset( cli_remote, srv_local, 'client', 'server' )
        _check_subset( srv_remote, cli_local, 'server', 'client' )

        # Check against expected topology if provided.
        if expected_candidates is not None:
            exp_srv, exp_cli = expected_candidates
            if exp_srv is not None and srv_local != exp_srv:
                print( "ERROR: server gathered %s, expected %s" % ( srv_local, exp_srv ) )
                g_failed = True
            if exp_cli is not None and cli_local != exp_cli:
                print( "ERROR: client gathered %s, expected %s" % ( cli_local, exp_cli ) )
                g_failed = True

#
# Main
#

ParseArgs()

if g_setup_mock_ips:
    SetupMockIPs()
    sys.exit(0)

if g_cleanup_mock_ips:
    CleanupMockIPs()
    sys.exit(0)

CheckMockIPsBindable()

# Find and start the STUN server
stun_server_script = './stun_server.py'
if not os.path.exists( stun_server_script ):
    stun_server_script = '../tests/stun_server.py'
if not os.path.exists( stun_server_script ):
    print( "Can't find stun_server.py" )
    sys.exit(1)

stun = StartProcessInThread( "stun", [ sys.executable, stun_server_script,
                                       '--host', g_stun_ip, '--host6', g_stun_ipv6, '--port', str(g_stun_port),
                                       '--relay-latency', '75', '--allocation-lifetime', '15',
                                       '--username', _TURN_USERNAME, '--password', _TURN_PASSWORD ],
                             ready_message="STUN/TURN server listening on", ready_event=g_stun_ready )

if not g_stun_ready.wait( timeout=g_server_startup_timeout ):
    print( "ERROR: STUN server failed to start within %d seconds" % g_server_startup_timeout )
    g_failed = True
    stun.term()
    sys.exit(1)

print( "STUN server is ready" )

# Start the signaling server
trivial_signaling_server = './trivial_signaling_server.py'
if not os.path.exists( trivial_signaling_server ):
    trivial_signaling_server = '../examples/trivial_signaling_server.py'
if not os.path.exists( trivial_signaling_server ):
    print( "Can't find trivial_signaling_server.py" )
    stun.term()
    sys.exit(1)

signaling = StartProcessInThread( "signaling", [ sys.executable, trivial_signaling_server ],
                                  ready_message="Listening at", ready_event=g_server_ready )

# Wait for the signaling server to be ready before starting tests
if not g_server_ready.wait( timeout=g_server_startup_timeout ):
    print( "ERROR: Signaling server failed to start within %d seconds" % g_server_startup_timeout )
    g_failed = True
    signaling.term()
    stun.term()
    sys.exit(1)

print( "Signaling server is ready, starting test clients" )

# Run the positive tests
for case in CLIENT_SERVER_TEST_CASES:
    desc, srv_args, cli_args, exp_route, ice_impl, exp_counters, exp_candidates = case[:7]
    extra = case[7] if len(case) > 7 else {}
    print( "=================================================================" )
    print( "Test: " + desc )
    print( "=================================================================" )
    ClientServerTest( srv_args, cli_args, exp_route, ice_impl, exp_counters, exp_candidates, **extra )
    if g_failed:
        break

# TURN allocation refresh test: run long enough (~20s, 400 ticks) to see two refreshes.
# With --allocation-lifetime 15, refreshes fire at ~7.5s and ~15s from allocation.
# refresh_send >= 2 confirms both fires; allocate_send == 2 confirms the auth challenge/response
# (one unauthenticated attempt + one with credentials) and no further re-allocation.
if not g_failed:
    print( "=================================================================" )
    print( "Test: TURN allocation refresh (symmetric NAT, 400 ticks)" )
    print( "=================================================================" )
    _refresh_args = [ '--ticks', '400' ] + _nat( _SRV_INT, _SRV_GW, 'symmetric' )
    _refresh_cli_args = [ '--ticks', '400' ] + _nat( _CLI_INT, _CLI_GW, 'symmetric' )
    ClientServerTest( _refresh_args, _refresh_cli_args,
                      expected_route='relay', ice_impl=1,
                      expected_counters={ 'refresh_send': (2, None), 'allocate_send': (2, 2) },
                      timeout_sec=35 * g_repeat )

# Run the expected-failure tests
if not g_failed:
    for desc, srv_args, cli_args, kwargs in FAILURE_TEST_CASES:
        print( "=================================================================" )
        print( "Test (expected failure): " + desc )
        print( "=================================================================" )
        ClientServerExpectedFailureTest( srv_args, cli_args, **kwargs )
        if g_failed:
            break

# Ignore any "failure" detected in server shutdowns.
really_failed = g_failed

signaling.term()
stun.term()

if really_failed:
    print( "TEST FAILED" )
    sys.exit(1)

print( "TEST SUCCEEDED" )
