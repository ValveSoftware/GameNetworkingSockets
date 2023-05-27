// Misc stuff used in the tests

#include "test_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <chrono>
#include <thread>

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

static FILE *g_fpLog = nullptr;
static SteamNetworkingMicroseconds g_logTimeZero;

static void DebugOutput( ESteamNetworkingSocketsDebugOutputType eType, const char *pszMsg )
{
	// !KLUDGE!
	if ( strstr( pszMsg, "Send Nagle") )
		return;

	SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - g_logTimeZero;
	if ( g_fpLog )
		fprintf( g_fpLog, "%10.6f %s\n", time*1e-6, pszMsg );
	if ( eType <= k_ESteamNetworkingSocketsDebugOutputType_Msg )
	{
		printf( "%10.6f %s\n", time*1e-6, pszMsg );
		fflush(stdout);
	}
	if ( eType == k_ESteamNetworkingSocketsDebugOutputType_Bug )
	{
		fflush(stdout);
		fflush(stderr);
		if ( g_fpLog )
			fflush( g_fpLog );

		// !KLUDGE! Our logging (which is done while we hold the lock)
		// is occasionally triggering this assert.  Just ignroe that one
		// error for now.
		// Yes, this is a kludge.
		if ( strstr( pszMsg, "SteamNetworkingGlobalLock held for" ) )
			return;

		assert( !"TEST FAILED" );
	}
}

void TEST_Printf( const char *fmt, ... )
{
	char text[ 2048 ];
	va_list ap;
	va_start( ap, fmt );
	vsprintf( text, fmt, ap );
	va_end(ap);
	char *nl = strchr( text, '\0' ) - 1;
	if ( nl >= text && *nl == '\n' )
		*nl = '\0';
	DebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Msg, text );
}

void TEST_Fatal( const char *fmt, ... )
{
	fflush(stdout);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
	exit(1);
}

void TEST_InitLog( const char *pszFilename )
{
	if ( g_logTimeZero )
		return;

	g_logTimeZero = SteamNetworkingUtils()->GetLocalTimestamp();

	//SteamNetworkingUtils()->SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Debug, DebugOutput );
	SteamNetworkingUtils()->SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Verbose, DebugOutput );
	//SteamNetworkingUtils()->SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput );

	SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, k_ESteamNetworkingSocketsDebugOutputType_Verbose );

	if ( !g_fpLog )
		g_fpLog = fopen( pszFilename, "wt" );
}

void TEST_Init( const SteamNetworkingIdentity *pIdentity )
{
	TEST_InitLog( "log.txt" );

	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		SteamDatagramErrMsg errMsg;
		if ( !GameNetworkingSockets_Init( pIdentity, errMsg ) )
		{
			fprintf( stderr, "GameNetworkingSockets_Init failed.  %s", errMsg );
			exit(1);
		}
	#else
		//SteamAPI_Init();

		SteamDatagram_SetUniverse();
		SteamDatagram_SetAppID( 570 ); // Just set something, doesn't matter what

		SteamDatagramErrMsg errMsg;
		if ( !SteamDatagramClient_Init( errMsg ) )
		{
			fprintf( stderr, "SteamDatagramClient_Init failed.  %s", errMsg );
			exit(1);
		}

		if ( pIdentity )
			SteamNetworkingSockets()->ResetIdentity( pIdentity );

		// Disable auth
		SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2 );

    #endif
}

void TEST_Kill()
{
	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		GameNetworkingSockets_Kill();
	#else
		SteamDatagramClient_Kill();
		SteamDatagramServer_Kill();
	#endif
}

void TEST_PumpCallbacks()
{
	if ( SteamNetworkingSockets() )
		SteamNetworkingSockets()->RunCallbacks();
	#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
		if ( SteamGameServerNetworkingSockets() )
			SteamGameServerNetworkingSockets()->RunCallbacks();
	#endif
	std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
}

