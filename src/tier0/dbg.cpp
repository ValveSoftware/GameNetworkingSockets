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

static SpewRetval_t  _SpewMessageType( SpewType_t spewType, char const* pMsgFormat, va_list args );

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

SpewRetval_t DefaultSpewFunc( SpewType_t type, char const *pMsg )
{
#if defined (POSIX) && !defined( _PS3 ) && !defined( NN_NINTENDO_SDK )
	static bool s_bSetSigHandler = false;
	if ( ! s_bSetSigHandler )
	{
		signal( SIGTRAP, SIG_IGN );
		signal( SIGALRM, SIG_IGN );
		s_bSetSigHandler = true;
	}
#endif
	printf( "%s", pMsg );
	if( type == SPEW_ASSERT )
		return SPEW_DEBUGGER;
	else if( type == SPEW_ERROR )
		return SPEW_ABORT;
	else
		return SPEW_CONTINUE;
}

static SpewOutputFunc_t   s_SpewOutputFunc = DefaultSpewFunc;

static char const*	s_pFileName;
static int			s_Line;
static SpewType_t	s_SpewType;

void   SpewOutputFunc( SpewOutputFunc_t func )
{
	s_SpewOutputFunc = func;
}

static void _ExitFatal()
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

//-----------------------------------------------------------------------------
// Spew functions
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
// Purpose: Lightly clean up a source path (skip to \src\ if we can)
//-----------------------------------------------------------------------------
static char const * CleanupAssertPath( char const* pFile )
{
#if defined(WIN32)
	for ( char const *s = pFile; *s; ++s )
	{
		if ( !V_strnicmp( s, "\\src\\", 5) )
		{
			return s;
		}
	}
#endif // no cleanup on other platforms

	return pFile;
}


static void  _SpewInfo( SpewType_t type, char const* pFile, int line )
{
	//
	//	We want full(ish) paths, not just leaf names, for better diagnostics
	//
	s_pFileName = CleanupAssertPath( pFile );

	s_Line = line;
	s_SpewType = type;
}


static SpewRetval_t  _SpewMessageType( SpewType_t spewType, char const* pMsgFormat, va_list args )
{
	char pTempBuffer[1024];

	/* Printf the file and line for warning + assert only... */
	int len = 0;
	if ( spewType == SPEW_ASSERT )
	{
		len = V_sprintf_safe( pTempBuffer, "%s(%d): ", s_pFileName, s_Line );
	}
	if ( len == -1 )
	{
		return SPEW_ABORT;
	}
	
	/* Create the message.... */
	int val= V_vsnprintf( &pTempBuffer[len], sizeof( pTempBuffer ) - len - 1, pMsgFormat, args );
	if ( val == -1 )
	{
		return SPEW_ABORT;
	}
	len += val;

	// Add \n for assert
	if ( spewType == SPEW_ASSERT )
	{
		if ( len+1 < sizeof(pTempBuffer) )
		{
			pTempBuffer[len++] = '\n';
			pTempBuffer[len] = '\0';
		}
	}
	
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
		_ExitFatal();

	default:
		break;
	}

	return ret;
}

SpewRetval_t  _SpewMessage( char const* pMsgFormat, ... )
{
	va_list args;
	va_start( args, pMsgFormat );
	SpewRetval_t ret = _SpewMessageType( s_SpewType, pMsgFormat, args );
	va_end(args);
	return ret;
}


void Error( char const *pMsgFormat, ... )
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


void AssertMsgImplementation( const char* _msg, bool _bFatal, const char* pstrFile, unsigned int nLine, bool bFullDump )
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
			_ExitFatal();
	}
#endif

	--s_ThreadLocalAssertMsgGuardStatic;
}
