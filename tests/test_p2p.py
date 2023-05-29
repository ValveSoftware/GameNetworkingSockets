#!/usr/bin/env python3

# Test script to run the p2p test.  This runs three processes: the two clients,
# and the dummy signaling service.
#
# NOTE: You usually won't run this script from its original location.  The
# makefiles will copy it into the same location as the tests and examples

import subprocess
import threading
import os
import sys
import copy

g_failed = False

# Thread class that runs a process and captures its output
class RunProcessInThread(threading.Thread):

    def __init__( self, tag, cmdline, env, **popen_kwargs ):
        threading.Thread.__init__( self, name=tag )
        self.tag = tag
        self.cmdline = cmdline
        if env:
            self.env = env
        else:
            self.env = dict( os.environ )
        self.popen_kwargs = popen_kwargs
        self.log = open( self.tag + ".log", "wt" )

    def WriteLn( self, ln ):
        print( "%s> %s" % (self.tag, ln ) )
        self.log.write( "%s\n" % ln )
        self.log.flush()

    def run( self ):

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
            elif self.process.poll() is not None:
                break
        self.process.wait()
        self.WriteLn( "Exitted with %d" % self.process.returncode )
        if self.process.returncode != 0:
            global g_failed
            g_failed = True

    # Wait for thread to shutdown.  Nuke process if we don't exit in time
    def join( self, timeout ):
        threading.Thread.join( self, timeout )
        if self.isAlive():
            self.WriteLn( "Still running after %d seconds.  Killing" % timeout )
            g_failed = True
            self.process.kill()

    # Attempt graceful shutdown
    def term( self ):
        self.WriteLn( "Attempting graceful shutdown" )
        self.process.terminate()
        self.join( 5 )

def StartProcessInThread( tag, cmdline, env=None, **popen_kwargs ):
    thread = RunProcessInThread( tag, cmdline, env, **popen_kwargs )
    thread.start()
    return thread

def StartClientInThread( role, local, remote ):
    cmdline = [
        "./test_p2p",
        "--" + role,
        "--identity-local", "str:"+local,
        "--identity-remote", "str:"+remote,
        "--signaling-server", "localhost:10000",
        "--log", local + ".verbose.log"
    ]

    env = dict( os.environ )
    if os.name == 'nt' and not os.path.exists( 'steamnetworkingsockets.dll' ):
        bindir = os.path.abspath('../../../bin')
        if not os.path.exists( bindir ):
            print( "Can't find steamnetworkingsockets.dll" )
            sys.exit(1)
        env['PATH'] = os.path.join( bindir, 'win64' ) + ';' + os.path.join( bindir, 'win32' ) + ';' + env['PATH']

    return StartProcessInThread( local, cmdline, env );

# Run a standard client/server connection-oriented case.
# where one peer is the "server" and "listens" and a "client" connects.
def ClientServerTest():
    print( "Running basic socket client/server test" )

    client1 = StartClientInThread( "server", "peer_server", "peer_client" )
    client2 = StartClientInThread( "client", "peer_client", "peer_server" )

    # Wait for clients to shutdown.  Nuke them if necessary
    client1.join( timeout=20 )
    client2.join( timeout=20 )

def SymmetricTest():
    print( "Running socket symmetric test" )

    client1 = StartClientInThread( "symmetric", "alice", "bob" )
    client2 = StartClientInThread( "symmetric", "bob", "alice" )

    # Wait for clients to shutdown.  Nuke them if necessary
    client1.join( timeout=20 )
    client2.join( timeout=20 )

#
# Main
#

# Start the signaling server
trivial_signaling_server = './trivial_signaling_server'
if os.name == 'nt' and not os.path.exists( 'trivial_signaling_server.exe' ):
    trivial_signaling_server = '../examples/trivial_signaling_server.exe'
    if not os.path.exists( trivial_signaling_server ):
        print( "Can't find trivial_signaling_server.exe" )
        sys.exit(1)
signaling = StartProcessInThread( "signaling", [ trivial_signaling_server ] )

# Run the tests
for test in [ ClientServerTest, SymmetricTest ]:
    print( "=================================================================" )
    print( "=================================================================" )
    test()
    print( "=================================================================" )
    print( "=================================================================" )
    if g_failed:
        break

# Ignore any "failure" detected in signaling server shutdown.
really_failed = g_failed

# Shutdown signaling
signaling.term()

if really_failed:
    print( "TEST FAILED" )
    sys.exit(1)

print( "TEST SUCCEEDED" )

