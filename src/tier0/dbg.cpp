//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// This is a custom version of dbg.cpp for the standalone version of
// SteamnetworkingSockets.  It was taken from the Steam code and then
// stripped to the bare essentials.
//
//=============================================================================//

#include <tier0/dbg.h>

#ifdef STEAMNETWORKINGSOCKETS_FOREXPORT
#include "../steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.h"
using namespace SteamNetworkingSocketsLib;
#endif

#if defined(_WIN32) && !defined(_XBOX)
#include "winlite.h"
#include <tchar.h>
#endif

#include <assert.h>

#ifdef POSIX
#include <unistd.h>
#include <signal.h>
#endif // POSIX

#ifdef LINUX
#include <sys/ptrace.h>
#endif

#ifdef OSX
#include <sys/sysctl.h>
#endif

bool Plat_IsInDebugSession()
{
#ifdef _WIN32
	return (IsDebuggerPresent() != 0);
#elif defined(OSX)
	int mib[4];
	struct kinfo_proc info;
	size_t size;
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID;
	mib[3] = getpid();
	size = sizeof(info);
	info.kp_proc.p_flag = 0;
	sysctl(mib,4,&info,&size,NULL,0);
	return ((info.kp_proc.p_flag & P_TRACED) == P_TRACED);
#elif defined(LINUX)
	static FILE *fp;
	if ( !fp )
	{
		char rgchProcStatusFile[256]; rgchProcStatusFile[0] = '\0';
		snprintf( rgchProcStatusFile, sizeof(rgchProcStatusFile), "/proc/%d/status", getpid() );
		fp = fopen( rgchProcStatusFile, "r" );
	}

	char rgchLine[256]; rgchLine[0] = '\0';
	int nTracePid = 0;
	if ( fp )
	{
		const char *pszSearchString = "TracerPid:";
		const uint cchSearchString = strlen( pszSearchString );
		rewind( fp );
		while ( fgets( rgchLine, sizeof(rgchLine), fp ) )
		{
			if ( !strncasecmp( pszSearchString, rgchLine, cchSearchString ) )
			{
				char *pszVal = rgchLine+cchSearchString+1;
				nTracePid = atoi( pszVal );
				break;
			}
		}
	}
	return (nTracePid != 0);
#elif defined( _PS3 )
#ifdef _CERT
	return false;
#else
	return snIsDebuggerPresent();
#endif
#endif
}

void AssertMsgImplementationV( bool _bFatal, bool bFmt, const char* pstrFile, unsigned int nLine, PRINTF_FORMAT_STRING const char *pMsg, va_list ap )
{
	static intp s_ThreadLocalAssertMsgGuardStatic; // Really should be thread-local
	if ( !_bFatal && s_ThreadLocalAssertMsgGuardStatic > 0 )
	{
		//
		// No need to re-enter.
		//
		return;
	}
	++s_ThreadLocalAssertMsgGuardStatic;

	#ifdef STEAMNETWORKINGSOCKETS_FOREXPORT
		(*g_pfnPreFormatSpewHandler)( k_ESteamNetworkingSocketsDebugOutputType_Bug, bFmt, pstrFile, nLine, pMsg, ap );
	#else
		fflush(stdout);
		if ( pstrFile )
			fprintf( stderr, "%s(%d): ", pstrFile, nLine );
		if ( bFmt )
			vfprintf( stderr, pMsg, ap );
		else
;			fprintf( stderr, "%s", pMsg );
		fflush(stderr);

		if ( Plat_IsInDebugSession() )
		{
			// HELLO DEVELOPER: Set this to true if you are getting fed up with the DebuggerBreak().
			static volatile bool s_bDisableDebuggerBreak = false;
			if ( !s_bDisableDebuggerBreak )
				DebuggerBreak();
		}
	#endif

	if ( _bFatal )
	{
		#ifdef _WIN32
			TerminateProcess( GetCurrentProcess(), EXIT_FAILURE ); // die, die RIGHT NOW! (don't call exit() so destructors will not get run)
		#elif defined( _PS3 )
			sys_process_exit( EXIT_FAILURE );
		#elif defined( __clang__ )
			abort();
		#else
			std::quick_exit( EXIT_FAILURE );
		#endif
	}

	--s_ThreadLocalAssertMsgGuardStatic;
}

void AssertMsgHelper<true,true>::AssertFailed( const char* pstrFile, unsigned int nLine, const char *pMsg )
{
	va_list dummy;
	memset( &dummy, 0, sizeof(dummy) ); // not needed, but might shut up a warning
	AssertMsgImplementationV( true, false, pstrFile, nLine, pMsg, dummy );
}

void AssertMsgHelper<false,true>::AssertFailed( const char* pstrFile, unsigned int nLine, const char *pMsg )
{
	va_list dummy;
	memset( &dummy, 0, sizeof(dummy) ); // not needed, but might shut up a warning
	AssertMsgImplementationV( false, false, pstrFile, nLine, pMsg, dummy );
}

void AssertMsgHelper<true,false>::AssertFailed( const char* pstrFile, unsigned int nLine, const char *pFmt, ... )
{
	va_list ap;
	va_start( ap, pFmt );
	AssertMsgImplementationV( true, true, pstrFile, nLine, pFmt, ap );
	va_end( ap );
}

void AssertMsgHelper<false,false>::AssertFailed( const char* pstrFile, unsigned int nLine, const char *pFmt, ... )
{
	va_list ap;
	va_start( ap, pFmt );
	AssertMsgImplementationV( false, true, pstrFile, nLine, pFmt, ap );
	va_end( ap );
}


