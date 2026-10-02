//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// This is a custom version of dbg.cpp for the standalone version of
// SteamnetworkingSockets.  It was taken from the Steam code and then
// stripped to the bare essentials.
//
//=============================================================================//

#include <tier0/dbg.h>

#if defined ( STEAMDATAGRAM_GAMECOORDINATOR_FOREXPORT )
extern void SteamDatagramGame_AssertFailed( bool bFmt, const char* pstrFile, unsigned int nLine, const char *pMsg, va_list ap );
#elif defined( STEAMNETWORKINGSOCKETS_FOREXPORT )
#include "../steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.h"
using namespace SteamNetworkingSocketsLib;
#endif

#if defined(_WIN32) && !defined(_XBOX)
#include "winlite.h"
#include <tchar.h>
#endif

#include <assert.h>

#if IsPosix()
	#include <unistd.h>
	#if !IsPlaystation()
		#include <signal.h>
	#endif
#endif // POSIX

#if IsLinux()
#include <sys/ptrace.h>
#include <fcntl.h>
#endif

#if IsOSX() || IsIOS() || IsTVOS() || IsFreeBSD() || IsOpenBSD()
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#if IsFreeBSD()
#include <sys/user.h>
#endif

#if IsOpenBSD()
#include <sys/proc.h>
#endif

#if IsPlaystation()
#include "tier0/dbg_playstation.h"
#endif

bool Plat_IsInDebugSession()
{
#ifdef _WIN32
	return (IsDebuggerPresent() != 0);
#elif IsOSX() || IsIOS() || IsTVOS()
	// Apple TN2151 / QA1361.  The same sysctl works on macOS, iOS and tvOS.
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
#elif IsFreeBSD()
	int mib[4];
	struct kinfo_proc info;
	size_t size;
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID;
	mib[3] = getpid();
	size = sizeof(info);
	if (sysctl(mib, 4, &info, &size, NULL, 0) == -1)
	    return false;
	return ((info.ki_flag & P_TRACED) != 0);
#elif IsOpenBSD()
	int mib[4];
	struct kinfo_proc info;
	size_t size;
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID;
	mib[3] = getpid();
	size = sizeof(info);
	if (sysctl(mib, 4, &info, &size, NULL, 0) == -1)
	    return false;
	return ((info.p_psflags & PS_TRACED) != 0);
#elif IsLinux()
	//if ( RUNNING_ON_VALGRIND )
	//	return true;

	int nFd = open( "/proc/self/status", O_RDONLY | O_CLOEXEC );
	if ( nFd < 0 )
		return false;

	// TracerPid always appears within the first few lines of the status output,
	// so a single 1024 bytes read should be enough to read the data.
	char rgchStatus[ 1024 ];
	ssize_t cbRead = read( nFd, rgchStatus, sizeof( rgchStatus ) - 1 );
	close( nFd );
	if ( cbRead <= 0 )
		return false;

	rgchStatus[ cbRead ] = '\0';

	static constexpr const char rgchTracerPidPrefix[] = "TracerPid:\t";
	const char *pszTracerPid = strstr( rgchStatus, rgchTracerPidPrefix );
	if ( !pszTracerPid )
		return false;

	static constexpr size_t nValueOffset = sizeof( rgchTracerPidPrefix ) - 1;
	if ( pszTracerPid + nValueOffset >= rgchStatus + cbRead )
		return false;

	// TracerPid: will always be '0' when no tracer is hooked to the process
	return ( pszTracerPid[ nValueOffset ] != '0' );
#elif IsPlaystation()
	return Plat_IsInDebugSession_Playstation();
#elif IsNintendoSwitch()
	return false;
#elif IsAndroid()
	return false;
#else
	#error "HALP"
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

	#if defined ( STEAMDATAGRAM_GAMECOORDINATOR_FOREXPORT )
		SteamDatagramGame_AssertFailed( bFmt, pstrFile, nLine, pMsg, ap );
	#elif defined( STEAMNETWORKINGSOCKETS_FOREXPORT )
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


