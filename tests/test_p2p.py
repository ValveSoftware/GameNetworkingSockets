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

def StartClientInThread( role, local, remote, extra_args=[] ):
    cmdline = [
        "./test_p2p",
        "--" + role,
        "--identity-local", "str:"+local,
        "--identity-remote", "str:"+remote,
        "--signaling-server", "localhost:10000",
        "--log", local + ".verbose.log"
    ]

    cmdline += [ '--stun-server', "%s:%d,[%s]:%d" % (g_stun_ip, g_stun_port, g_stun_ipv6, g_stun_port) ]
    cmdline += [ '--turn-server', "%s:%d" % (g_stun_ip, g_stun_port) ]
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

# Mock network address constants — IPv4
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

# Mock network address constants — IPv6
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

# Each entry: ( description, server_extra_args, client_extra_args, expected_route, ice_impl )
# Both sides must report the same route type — ICE nominates one candidate pair
# and both ends classify the same path, so agreement is guaranteed by the protocol.
# Route types: 'local' = Fast flag set: both host candidates on the same private /24 LAN subnet
#              'udp'   = direct UDP but not same-LAN (NAT traversal, or public IPs)
#              'relay' = TURN relay
# ice_impl: 0 = default (WebRTC if compiled in), 1 = force native ICE client.
# All mocked tests must use 1: the mock network is only wired into the native ICE path.
# The two no-mock entries exercise both implementations on the real loopback network.
CLIENT_SERVER_TEST_CASES = [
    # No-mock tests: run on real network (same host), both implementations.
    # We verify the route is 'local' (same-host loopback) but do not check the IP.
    ( 'no-mock, default ICE implementation',
      [], [],
      'local', 0 ),
    ( 'no-mock, native ICE implementation',
      [], [],
      'local', 1 ),

    # Both on the same private /24 LAN: the core case for 'local' classification.
    ( 'same LAN (private subnet, no NAT)',
      [ '--mock-adapter', _SRV_INT ],
      [ '--mock-adapter', _CLI_SAME_LAN ],
      'local', 1 ),

    # Both on the same LAN but each also has a NAT to the public internet via the
    # same shared gateway — the typical home/office scenario.  Both a direct host path
    # and a hairpin path through the gateway exist; ICE must select the direct host
    # path (higher priority) and classify it as 'local'.
    ( 'same LAN, shared gateway (hairpin)',
      _nat( _SRV_INT, _SRV_GW, 'full-cone' ),
      _nat( _CLI_SAME_LAN, _SRV_GW, 'full-cone' ),
      'local', 1 ),

    # Both on the public network: direct host-to-host but NOT 'local' — public IPs
    # are not classified as fast even when they share a subnet.
    ( 'no-nat (both public)',
      [ '--mock-adapter', _SRV_GW ],
      [ '--mock-adapter', _CLI_GW ],
      'udp', 1 ),

    ( 'full-cone NAT',
      _nat( _SRV_INT, _SRV_GW, 'full-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ),
      'udp', 1 ),
    ( 'restricted-cone NAT',
      _nat( _SRV_INT, _SRV_GW, 'restricted-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'restricted-cone' ),
      'udp', 1 ),
    ( 'port-restricted-cone NAT',
      _nat( _SRV_INT, _SRV_GW, 'port-restricted-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'port-restricted-cone' ),
      'udp', 1 ),
    # symmetric-vs-symmetric requires TURN relay; ICE alone cannot traverse it
    ( 'asymmetric: server public, client full-cone',
      [ '--mock-adapter', _SRV_GW ],
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ),
      'udp', 1 ),  # client host (127.0.2.x) to server host (127.0.100.x): different subnets
    ( 'asymmetric: server full-cone, client symmetric',
      _nat( _SRV_INT, _SRV_GW, 'full-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'symmetric' ),
      'udp', 1 ),

    # Disabled adapter: verify connection still succeeds when one adapter is down
    ( 'server has disabled second adapter',
      [ '--mock-adapter', _SRV_GW ] + _disabled_adapter( _DEAD_INT ),
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ),
      'udp', 1 ),
    ( 'client has disabled second adapter',
      [ '--mock-adapter', _SRV_GW ],
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ) + _disabled_adapter( _DEAD_INT ),
      'udp', 1 ),

    # Multi-adapter with latency: fast public adapter + slow NATd adapter.
    # ICE should prefer the low-latency host-to-host path.  Both public adapters
    # are on 127.0.100.x (not a private subnet) so the route is 'udp', not 'local'.
    ( 'both multi-adapter: fast public + slow NATd',
      [ '--mock-adapter', _SRV_GW ] + _slow_nat( _SRV_INT2, _SRV_GW2, 'full-cone', 50 ),
      [ '--mock-adapter', _CLI_GW ] + _slow_nat( _CLI_INT2, _CLI_GW2, 'full-cone', 50 ),
      'udp', 1 ),

    # IPv6 host candidates: both endpoints have a public IPv6 address, no NAT.
    # fd7f:0:100::x is the mock public IPv6 network (not classified as 'local').
    ( 'IPv6 no-nat (both public)',
      [ '--mock-adapter', _SRV_GW_V6 ],
      [ '--mock-adapter', _CLI_GW_V6 ],
      'udp', 1 ),

    # IPv6 full-cone NAT
    ( 'IPv6 full-cone NAT',
      _nat( _SRV_INT_V6, _SRV_GW_V6, 'full-cone' ),
      _nat( _CLI_INT_V6, _CLI_GW_V6, 'full-cone' ),
      'udp', 1 ),
]

def ClientServerTest( server_extra_args=[], client_extra_args=[], expected_route=None, ice_impl=1 ):
    global g_failed
    impl_args = [ '--ice-implementation', str(ice_impl) ]
    server = StartClientInThread( "server", "peer_server", "peer_client", server_extra_args + impl_args )
    client = StartClientInThread( "client", "peer_client", "peer_server", client_extra_args + impl_args )

    # Wait for clients to shutdown.  Nuke them if necessary
    server.join( timeout=20 * g_repeat )
    client.join( timeout=20 * g_repeat )

    # Verify route types if an expected value was provided
    if expected_route is not None:
        for peer, thread in [ ( 'server', server ), ( 'client', client ) ]:
            if thread.route_type is None:
                print( "ERROR: %s did not report a route type" % peer )
                g_failed = True
            elif thread.route_type != expected_route:
                print( "ERROR: %s route type '%s', expected '%s'" % ( peer, thread.route_type, expected_route ) )
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
                                       '--host', g_stun_ip, '--host6', g_stun_ipv6, '--port', str(g_stun_port) ],
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

# Run the tests
for desc, srv_args, cli_args, exp_route, ice_impl in CLIENT_SERVER_TEST_CASES:
    print( "=================================================================" )
    print( "Test: " + desc )
    print( "=================================================================" )
    ClientServerTest( srv_args, cli_args, exp_route, ice_impl )
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
