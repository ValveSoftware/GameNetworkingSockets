#!/usr/bin/env python3

# Test script to run the p2p test.  This runs three processes: the two clients,
# and the dummy signaling service.
#
# NOTE: You usually won't run this script from its original location.  The
# makefiles will copy it into the same location as the tests and examples

import subprocess
import threading
import os

def RunProcess( tag, cmdline, **popen_kwargs ):

    with open( tag + ".log", "wt" ) as log:

        def WriteLn( ln ):
            print( "%s> %s" % (tag, ln ) )
            log.write( "%s\n" % ln )

        # Make a copy of the environment
        env = dict( os.environ )

        # Set LD_LIBRARY_PATH
        if os.name == 'posix':
            LD_LIBRARY_PATH = env.get( 'LD_LIBRARY_PATH', '' )
            if LD_LIBRARY_PATH: LD_LIBRARY_PATH += ';'
            env['LD_LIBRARY_PATH'] = LD_LIBRARY_PATH + "."
            WriteLn( "LD_LIBRARY_PATH = '%s'" % env['LD_LIBRARY_PATH'])

        WriteLn( "Executing: " + ' '.join( cmdline ) )
        process = subprocess.Popen( cmdline, stdout=subprocess.PIPE, stdin=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, **popen_kwargs )
        process.stdin.close()
        while True:
            sOutput = process.stdout.readline()
            if sOutput:
                sOutput = str(sOutput, 'utf-8', 'ignore')
                WriteLn( sOutput.rstrip() )
            elif process.poll() is not None:
                break
        process.wait()
        WriteLn( "Exitted with %d" % process.returncode )

def StartProcessInThread( tag, cmdline, **popen_kwargs ):
    def ThreadProc():
        RunProcess( tag, cmdline, **popen_kwargs )
    thread = threading.Thread( target=ThreadProc, name=tag )
    thread.start()
    return thread

def StartClientInThread( role, local, remote ):
    cmdline = [
        "./test_p2p",
        "--" + role,
        "--identity-local", "str:"+local,
        "--identity-remote", "str:"+remote,
        "--signaling-server", "localhost:10000"
    ]
    return StartProcessInThread( local, cmdline );

signaling = StartProcessInThread( "signaling", [ './trivial_signaling_server' ] )

client1 = StartClientInThread( "server", "peer_server", "peer_client" )
client2 = StartClientInThread( "client", "peer_client", "peer_server" )

client1.join()
client2.join()
signaling.join()

