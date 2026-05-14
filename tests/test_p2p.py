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
g_stun_port = 3478

def ParseArgs():
    global g_spew_level
    global g_p2p_rendezvous_level

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
    args = parser.parse_args()
    g_spew_level = args.spewlevel
    g_p2p_rendezvous_level = args.loglevel_p2prendezvous

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

    cmdline += [ '--stun-server', "%s:%d" % (g_stun_ip, g_stun_port) ]
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

# Mock network address constants
_SRV_GW  = '127.0.100.2'   # server-side NAT gateway (public)
_CLI_GW  = '127.0.100.3'   # client-side NAT gateway (public)
_SRV_INT = '127.0.1.2'     # server internal address behind NAT
_CLI_INT = '127.0.2.2'     # client internal address behind NAT

def _nat( internal, gateway, nat_type ):
    return [ '--mock-adapter', internal, '--mock-gateway', gateway, '--mock-nat', nat_type ]

# Each entry: ( description, server_extra_args, client_extra_args, expected_server_route, expected_client_route )
# Route types: 'local' = host-to-host (no NAT traversal needed)
#              'udp'   = srflx or peer-reflexive (NAT traversal used)
#              'relay' = TURN relay
CLIENT_SERVER_TEST_CASES = [
    ( 'no-mock',
      [], [],
      'local', 'local' ),
    ( 'no-nat (both public)',
      [ '--mock-adapter', _SRV_GW ],
      [ '--mock-adapter', _CLI_GW ],
      'local', 'local' ),
    ( 'full-cone NAT',
      _nat( _SRV_INT, _SRV_GW, 'full-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ),
      'udp', 'udp' ),
    ( 'restricted-cone NAT',
      _nat( _SRV_INT, _SRV_GW, 'restricted-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'restricted-cone' ),
      'udp', 'udp' ),
    ( 'port-restricted-cone NAT',
      _nat( _SRV_INT, _SRV_GW, 'port-restricted-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'port-restricted-cone' ),
      'udp', 'udp' ),
    # symmetric-vs-symmetric requires TURN relay; ICE alone cannot traverse it
    ( 'asymmetric: server public, client full-cone',
      [ '--mock-adapter', _SRV_GW ],
      _nat( _CLI_INT, _CLI_GW, 'full-cone' ),
      'udp', 'local' ),  # server sees client's srflx; client's host-host pair wins via NAT passthrough
    ( 'asymmetric: server full-cone, client symmetric',
      _nat( _SRV_INT, _SRV_GW, 'full-cone' ),
      _nat( _CLI_INT, _CLI_GW, 'symmetric' ),
      'udp', 'udp' ),
]

def ClientServerTest( server_extra_args=[], client_extra_args=[], expected_server_route=None, expected_client_route=None ):
    global g_failed
    server = StartClientInThread( "server", "peer_server", "peer_client", server_extra_args )
    client = StartClientInThread( "client", "peer_client", "peer_server", client_extra_args )

    # Wait for clients to shutdown.  Nuke them if necessary
    server.join( timeout=20 )
    client.join( timeout=20 )

    # Verify route types if expected values were provided
    if expected_server_route is not None:
        if server.route_type is None:
            print( "ERROR: server did not report a route type" )
            g_failed = True
        elif server.route_type != expected_server_route:
            print( "ERROR: server route type '%s', expected '%s'" % ( server.route_type, expected_server_route ) )
            g_failed = True
    if expected_client_route is not None:
        if client.route_type is None:
            print( "ERROR: client did not report a route type" )
            g_failed = True
        elif client.route_type != expected_client_route:
            print( "ERROR: client route type '%s', expected '%s'" % ( client.route_type, expected_client_route ) )
            g_failed = True

#
# Main
#

ParseArgs()

# Find and start the STUN server
stun_server_script = './stun_server.py'
if not os.path.exists( stun_server_script ):
    stun_server_script = '../tests/stun_server.py'
if not os.path.exists( stun_server_script ):
    print( "Can't find stun_server.py" )
    sys.exit(1)

stun = StartProcessInThread( "stun", [ sys.executable, stun_server_script, '--host', g_stun_ip, '--port', str(g_stun_port) ],
                             ready_message="STUN server listening on", ready_event=g_stun_ready )

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
for desc, srv_args, cli_args, exp_srv_route, exp_cli_route in CLIENT_SERVER_TEST_CASES:
    print( "=================================================================" )
    print( "Test: " + desc )
    print( "=================================================================" )
    ClientServerTest( srv_args, cli_args, exp_srv_route, exp_cli_route )
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
