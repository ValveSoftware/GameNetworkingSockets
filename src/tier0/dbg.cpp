//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//

#include "tier0/dbg.h"

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

#ifdef LINUX
#include "tier0/valgrind.h"
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
	if ( RUNNING_ON_VALGRIND )
	{
		return true;
	}
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

#if defined (POSIX) && !defined( _PS3 )
static bool s_bSetSigHandler = false;
#endif

SpewRetval_t DefaultSpewFunc( SpewType_t type, tchar const *pMsg )
{
#if defined (POSIX) && !defined( _PS3 ) // No signals on PS3
	if ( ! s_bSetSigHandler )
	{
		signal( SIGTRAP, SIG_IGN );
		signal( SIGALRM, SIG_IGN );
		s_bSetSigHandler = true;
	}
#endif
#ifdef _PS3
	printf( _T("STEAMPS3 - %s"), pMsg );
#else
	printf( _T("%s"), pMsg );
#endif
	if( type == SPEW_ASSERT )
		return SPEW_DEBUGGER;
	else if( type == SPEW_ERROR )
		return SPEW_ABORT;
	else
		return SPEW_CONTINUE;
}

static SpewOutputFunc_t   s_SpewOutputFunc = DefaultSpewFunc;

static tchar const*	s_pFileName;
static int			s_Line;
static SpewType_t	s_SpewType;

void   SpewOutputFunc( SpewOutputFunc_t func )
{
	s_SpewOutputFunc = func;
}

void _ExitOnFatalAssert( tchar const* pFile, int line, tchar const *pMessage )
{
	_SpewMessage( _T("Fatal assert failed: %s, line %d.  Application exiting.\n"), pFile, line );

#ifdef _WIN32
	TerminateProcess( GetCurrentProcess(), EXIT_FAILURE ); // die, die RIGHT NOW! (don't call exit() so destructors will not get run)
#elif defined( _PS3 )
	sys_process_exit( EXIT_FAILURE );
#else
	_exit( EXIT_FAILURE );
#endif
}

//-----------------------------------------------------------------------------
// Spew functions
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
// Purpose: Lightly clean up a source path (skip to \src\ if we can)
//-----------------------------------------------------------------------------
static tchar const * CleanupAssertPath( tchar const* pFile )
{
#if defined(WIN32)
	for ( tchar const *s = pFile; *s; ++s )
	{
		if ( ! strnicmp( s, _T("\\src\\"), 5) )
		{
			return s;
		}
	}
#endif // no cleanup on other platforms

	return pFile;
}


void  _SpewInfo( SpewType_t type, tchar const* pFile, int line )
{
	//
	//	We want full(ish) paths, not just leaf names, for better diagnostics
	//
	s_pFileName = CleanupAssertPath( pFile );

	s_Line = line;
	s_SpewType = type;
}


SpewRetval_t  _SpewMessageType( SpewType_t spewType, tchar const* pMsgFormat, va_list args )
{
	//LOCAL_THREAD_LOCK();

#ifndef _XBOX
	tchar pTempBuffer[5020];
#else
	char pTempBuffer[1024];
#endif

	assert( _tcslen( pMsgFormat ) < sizeof( pTempBuffer) ); // check that we won't artifically truncate the string

	/* Printf the file and line for warning + assert only... */
	int len = 0;
	if ( spewType == SPEW_ASSERT )
	{
		len = _sntprintf( pTempBuffer, sizeof( pTempBuffer ) - 1, _T("%s (%d) : "), s_pFileName, s_Line );
	}
	if ( len == -1 )
	{
		return SPEW_ABORT;
	}
	
	/* Create the message.... */
	int val= _vsntprintf( &pTempBuffer[len], sizeof( pTempBuffer ) - len - 1, pMsgFormat, args );
	if ( val == -1 )
	{
		return SPEW_ABORT;
	}
	len += val;
	assert( len * sizeof(*pMsgFormat) < sizeof(pTempBuffer) ); /* use normal assert here; to avoid recursion. */

	// Add \n for warning and assert
	if ( spewType == SPEW_ASSERT )
	{
		len += _stprintf( &pTempBuffer[len], _T("\n") ); 
#ifdef WIN32 
		OutputDebugString( pTempBuffer );
#endif
	}
	
	assert( (uint) len < sizeof(pTempBuffer)/sizeof(pTempBuffer[0]) - 1 ); /* use normal assert here; to avoid recursion. */
	assert( s_SpewOutputFunc );
	
	/* direct it to the appropriate target(s) */
	SpewRetval_t ret = s_SpewOutputFunc( spewType, pTempBuffer );
	switch (ret)
	{
// Asserts put the break into the macro so it occurs in the right place
	case SPEW_DEBUGGER:
		if ( spewType != SPEW_ASSERT )
		{
			DebuggerBreak();
		}
		break;
		
	case SPEW_ABORT:
//		MessageBox(NULL,"Error in _SpewMessage","Error",MB_OK);
		//DMsg( "console",  1, _T("Exiting on SPEW_ABORT\n") );
#ifdef _WIN32
		TerminateProcess( GetCurrentProcess(), EXIT_FAILURE ); // die, die RIGHT NOW! (don't call exit() so destructors will not get run)
#elif defined( _PS3 )
		sys_process_exit( EXIT_FAILURE );
#else
		_exit( EXIT_FAILURE ); // forcefully shutdown of the process without destructors running
#endif
	default:
		break;
	}

	return ret;
}

SpewRetval_t  _SpewMessage( tchar const* pMsgFormat, ... )
{
	va_list args;
	va_start( args, pMsgFormat );
	SpewRetval_t ret = _SpewMessageType( s_SpewType, pMsgFormat, args );
	va_end(args);
	return ret;
}

//SpewRetval_t _DSpewMessage( tchar const *pGroupName, int level, tchar const* pMsgFormat, ... )
//{
//	if( !IsSpewActive( pGroupName, level ) )
//		return SPEW_CONTINUE;
//
//	va_list args;
//	va_start( args, pMsgFormat );
//	SpewRetval_t ret = _SpewMessageType( s_SpewType, pMsgFormat, args );
//	va_end(args);
//	return ret;
//}

void Msg( tchar const* pMsgFormat, ... )
{
	va_list args;
	va_start( args, pMsgFormat );
	_SpewMessageType( SPEW_MESSAGE, pMsgFormat, args );
	va_end(args);
}

//void _DMsg( tchar const *pGroupName, int level, tchar const *pMsgFormat, ... )
//{
//	if( !IsSpewActive( pGroupName, level ) )
//		return;
//
//	va_list args;
//	va_start( args, pMsgFormat );
//	_SpewMessageType( SPEW_MESSAGE, pMsgFormat, args );
//	va_end(args);
//}


void Warning( tchar const *pMsgFormat, ... )
{
	va_list args;
	va_start( args, pMsgFormat );
	_SpewMessageType( SPEW_WARNING, pMsgFormat, args );
	va_end(args);
}

//void DWarning( tchar const *pGroupName, int level, tchar const *pMsgFormat, ... )
//{
//	if( !IsSpewActive( pGroupName, level ) )
//		return;
//
//	va_list args;
//	va_start( args, pMsgFormat );
//	_SpewMessageType( SPEW_WARNING, pMsgFormat, args );
//	va_end(args);
//}
//
//void Log( tchar const *pMsgFormat, ... )
//{
//	va_list args;
//	va_start( args, pMsgFormat );
//	_SpewMessageType( SPEW_LOG, pMsgFormat, args );
//	va_end(args);
//}
//
//void DLog( tchar const *pGroupName, int level, tchar const *pMsgFormat, ... )
//{
//	if( !IsSpewActive( pGroupName, level ) )
//		return;
//
//	va_list args;
//	va_start( args, pMsgFormat );
//	_SpewMessageType( SPEW_LOG, pMsgFormat, args );
//	va_end(args);
//}

void Error( tchar const *pMsgFormat, ... )
{
	va_list args;
	va_start( args, pMsgFormat );
	_SpewMessageType( SPEW_ERROR, pMsgFormat, args );
	va_end(args);
}

//-----------------------------------------------------------------------------
// Implementation helper for assertion messages. The AssergMsg() macro has become
// very big, so when possible this function moves the work it does out-of-line.
// This helps the size of debug builds overall, and is an absolute must for Steam,
// which builds with assertions turned on in release mode.
//-----------------------------------------------------------------------------


void AssertMsgImplementation( const tchar* _msg, bool _bFatal, const tchar* pstrFile, unsigned int nLine, bool bFullDump )
{
	static intp s_ThreadLocalAssertMsgGuardStatic; // Really should be thread-local
	if ( s_ThreadLocalAssertMsgGuardStatic > 0 )
	{
		//
		// No need to re-enter.
		//
		return;
	}
	++s_ThreadLocalAssertMsgGuardStatic;

#if defined(_PS3)
	SetInAssert( true );
	_SpewInfo( SPEW_ASSERT, pstrFile, nLine );
	_SpewMessage( "%s", _msg);

	if ( _bFatal )
	{
		_SpewMessage( _T("Fatal assert failed: %s, line %d.  Application exiting.\n"), pstrFile, nLine );
		DebuggerBreak();
		sys_process_exit( EXIT_FAILURE );
	}

	SetInAssert( false );
#else

	//
	// Always spew, even if we aren't going to dump.
	//
	_SpewInfo( SPEW_ASSERT, pstrFile, nLine );
	SpewRetval_t ret = _SpewMessage( "%s", _msg);

	if ( ret == SPEW_DEBUGGER && Plat_IsInDebugSession() )
	{
		// HELLO DEVELOPER: Set this to true if you are getting fed up with the DebuggerBreak().
		static volatile bool s_bDisableDebuggerBreak = false;
		if ( !s_bDisableDebuggerBreak )
			DebuggerBreak();
	}
	else
	{
		if ( _bFatal )
			_ExitOnFatalAssert( pstrFile, nLine, _msg );
	}
#endif

	--s_ThreadLocalAssertMsgGuardStatic;
}
