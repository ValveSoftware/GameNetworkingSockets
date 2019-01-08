//===== Copyright 1996-2005, Valve Corporation, All rights reserved. ========//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//
#ifndef DBG_H
#define DBG_H
#pragma once

#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#include "basetypes.h"
#include "dbgflag.h"
#include "platform.h"
#include <vstdlib/strtools.h>

//-----------------------------------------------------------------------------
// dll export stuff
//-----------------------------------------------------------------------------
//SDR_PUBLIC #ifndef STATIC_TIER0
//SDR_PUBLIC 
//SDR_PUBLIC #ifdef TIER0_DLL_EXPORT
//SDR_PUBLIC #define DBG_INTERFACE	DLL_EXPORT
//SDR_PUBLIC #define DBG_OVERLOAD	DLL_GLOBAL_EXPORT
//SDR_PUBLIC #define DBG_CLASS		DLL_CLASS_EXPORT
//SDR_PUBLIC #else
//SDR_PUBLIC #define DBG_INTERFACE	DLL_IMPORT
//SDR_PUBLIC #define DBG_OVERLOAD	DLL_GLOBAL_IMPORT
//SDR_PUBLIC #define DBG_CLASS		DLL_CLASS_IMPORT
//SDR_PUBLIC #endif
//SDR_PUBLIC 
//SDR_PUBLIC #else // BUILD_AS_DLL
#define DBG_INTERFACE	extern "C"
#define DBG_OVERLOAD	
#define DBG_CLASS		

//#endif // BUILD_AS_DLL

class CDbgFmtMsg
{
public:
	CDbgFmtMsg(PRINTF_FORMAT_STRING const tchar *pszFormat, ...) FMTFUNCTION( 2, 3 )
	{ 
		va_list arg_ptr;

		va_start(arg_ptr, pszFormat);
		_vsnprintf(m_szBuf, sizeof(m_szBuf)-1, pszFormat, arg_ptr);
		va_end(arg_ptr);

		m_szBuf[sizeof(m_szBuf)-1] = 0;
	}

	operator const tchar *() const				
	{ 
		return m_szBuf; 
	}

	const tchar *ToString() const				
	{ 
		return m_szBuf; 
	}
private:
	tchar m_szBuf[256];
};

//-----------------------------------------------------------------------------
// Usage model for the Dbg library
//
// 1. Spew.
// 
//   Spew can be used in a static and a dynamic mode. The static
//   mode allows us to display assertions and other messages either only
//   in debug builds, or in non-release builds. The dynamic mode allows us to
//   turn on and off certain spew messages while the application is running.
// 
//   Static Spew messages:
//
//     Assertions are used to detect and warn about invalid states
//     Spews are used to display a particular status/warning message.
//
//     To use an assertion, use
//
//     Assert( (f == 5) );
//     AssertMsg( (f == 5), ("F needs to be %d here!\n", 5) );
//     AssertFunc( (f == 5), BadFunc() );
//     AssertEquals( f, 5 );
//     AssertFloatEquals( f, 5.0f, 1e-3 );
//
//     The first will simply report that an assertion failed on a particular
//     code file and line. The second version will display a print-f formatted message 
//	   along with the file and line, the third will display a generic message and
//     will also cause the function BadFunc to be executed, and the last two
//	   will report an error if f is not equal to 5 (the last one asserts within
//	   a particular tolerance).
//
//     To use a warning, use
//      
//     Warning("Oh I feel so %s all over\n", "yummy");
//
//     Warning will do its magic in only Debug builds. To perform spew in *all*
//     builds, use RelWarning.
//
//	   Three other spew types, Msg, Log, and Error, are compiled into all builds.
//	   These error types do *not* need two sets of parenthesis.
//
//	   Msg( "Isn't this exciting %d?", 5 );
//	   Error( "I'm just thrilled" );
//
//   Dynamic Spew messages
//
//     It is possible to dynamically turn spew on and off. Dynamic spew is 
//     identified by a spew group and priority level. To turn spew on for a 
//     particular spew group, use SpewActivate( "group", level ). This will 
//     cause all spew in that particular group with priority levels <= the 
//     level specified in the SpewActivate function to be printed. Use DSpew 
//     to perform the spew:
//
//     DWarning( "group", level, "Oh I feel even yummier!\n" );
//
//     Priority level 0 means that the spew will *always* be printed, and group
//     '*' is the default spew group. If a DWarning is encountered using a group 
//     whose priority has not been set, it will use the priority of the default 
//     group. The priority of the default group is initially set to 0.      
//
//   Spew output
//   
//     The output of the spew system can be redirected to an externally-supplied
//     function which is responsible for outputting the spew. By default, the 
//     spew is simply printed using printf.
//
//     To redirect spew output, call SpewOutput.
//
//     SpewOutputFunc( OutputFunc );
//
//     This will cause OutputFunc to be called every time a spew message is
//     generated. OutputFunc will be passed a spew type and a message to print.
//     It must return a value indicating whether the debugger should be invoked,
//     whether the program should continue running, or whether the program 
//     should abort. 
//
// 2. Code activation
//
//   To cause code to be run only in debug builds, use DBG_CODE:
//   An example is below.
//
//   DBG_CODE(
//				{
//					int x = 5;
//					++x;
//				}
//           ); 
//
//   Code can be activated based on the dynamic spew groups also. Use
//  
//   DBG_DCODE( "group", level,
//              { int x = 5; ++x; }
//            );
//
// 3. Breaking into the debugger.
//
//   To cause an unconditional break into the debugger in debug builds only, use DBG_BREAK
//
//   DBG_BREAK();
//
//	 You can force a break in any build (release or debug) using
//
//	 DebuggerBreak();
//-----------------------------------------------------------------------------

/* Various types of spew messages */
// I'm sure you're asking yourself why SPEW_ instead of DBG_ ?
// It's because DBG_ is used all over the place in windows.h
// For example, DBG_CONTINUE is defined. Feh.
enum SpewType_t
{
	SPEW_MESSAGE = 0,
	SPEW_WARNING,
	SPEW_ASSERT,
	SPEW_ERROR,
	SPEW_LOG,
	SPEW_INPUT,
	SPEW_BOLD_MESSAGE, // no error condition, but should stand out if possible

	SPEW_TYPE_COUNT
};

enum SpewRetval_t
{
	SPEW_DEBUGGER = 0,
	SPEW_CONTINUE,
	SPEW_ABORT
};


enum 
{ 
	MAX_GROUP_NAME_LENGTH = 48 
};


/* type of externally defined function used to display debug spew */
typedef SpewRetval_t (*SpewOutputFunc_t)( SpewType_t spewType, tchar const *pMsg );

/* Used to redirect spew output */
DBG_INTERFACE void   SpewOutputFunc( SpewOutputFunc_t func );

/* Used ot get the current spew output function */
DBG_INTERFACE SpewOutputFunc_t GetSpewOutputFunc( void );

//SDR_PUBLIC /* Used to manage spew groups and subgroups */
//SDR_PUBLIC DBG_INTERFACE void   SpewActivate( tchar const* pGroupName, int level );
//SDR_PUBLIC DBG_INTERFACE void   SpewAndLogActivate( tchar const* pGroupName, int level, int logLevel );
//SDR_PUBLIC DBG_INTERFACE void   SpewChangeIfStillDefault( tchar const* pGroupName, int level, int leveldefault );
//SDR_PUBLIC DBG_INTERFACE void   SpewAndLogChangeIfStillDefault( tchar const* pGroupName, int level, int leveldefault, int logLevel, int logLevelDefault );
//SDR_PUBLIC DBG_INTERFACE bool   GetSpewAndLogLevel( tchar const* pGroupName, int &spewLevel, int &logLevel );
//SDR_PUBLIC DBG_INTERFACE int    GetNumberOfSpewAndLogGroups();
//SDR_PUBLIC DBG_INTERFACE bool   GetSpewAndLogLevelByGroupIndex( int index, tchar const *&pGroupName, int &spewLevel, int &logLevel );
//SDR_PUBLIC DBG_INTERFACE bool   IsSpewActive( tchar const* pGroupName, int level );
//SDR_PUBLIC DBG_INTERFACE bool	 IsLogActive( tchar const* pGroupName, int logLevel );

/* Used to display messages, should never be called directly. */
DBG_INTERFACE void   _SpewInfo( SpewType_t type, tchar const* pFile, int line );
DBG_INTERFACE SpewRetval_t   _SpewMessage( PRINTF_FORMAT_STRING tchar const* pMsg, ... ) FMTFUNCTION( 1, 2 );
DBG_INTERFACE SpewRetval_t  _SpewMessageType( SpewType_t spewType, tchar const* pMsgFormat, va_list args );
DBG_INTERFACE SpewRetval_t   _DSpewMessage( tchar const *pGroupName, int level, PRINTF_FORMAT_STRING tchar const* pMsg, ... ) FMTFUNCTION( 3, 4 );
DBG_INTERFACE void _ExitOnFatalAssert( tchar const* pFile, int line, tchar const* pMessage );

DBG_INTERFACE void AssertMsgImplementation( const tchar* _msg, bool _bFatal, const tchar* pstrFile, unsigned int nLine, bool bFullDump );

#if 0

DBG_INTERFACE bool SetAssertDumpStack( bool bAssertDumpStack );

// Used to manage stack filters for memory allocations
DBG_INTERFACE void ClearStackTrackingFilters();
DBG_INTERFACE int GetNumberOfStackTrackingFilters();
DBG_INTERFACE const char *GetStackTrackingFilter( int nFilterIndex );
DBG_INTERFACE bool SetStackTrackingFilter( int nFilterIndex, const char *pchFilter );
DBG_INTERFACE bool IsStackTrackingFiltered( const char *pchModule );
DBG_INTERFACE bool HasStackTrackingFilters();
DBG_INTERFACE bool AreStackTrackingFiltersEnabledAtStart();
DBG_INTERFACE void InitializeStackTrackingFilters();

#ifndef _XBOX
DBG_INTERFACE bool ShouldUseNewAssertDialog();
#else
#define ShouldUseNewAssertDialog()	0
#endif

#ifndef _XBOX
// Returns true if they want to break in the debugger.
DBG_INTERFACE bool DoNewAssertDialog( const tchar *pFile, int line, const tchar *pExpression );
#else
#define DoNewAssertDialog(a,b,c)	0
#endif
DBG_INTERFACE bool IsInAssert();
DBG_INTERFACE void SetInAssert( bool bState );

typedef void (*FlushLogFunc_t)();
DBG_INTERFACE void SetFlushLogFunc( FlushLogFunc_t func );
DBG_INTERFACE void CallFlushLogFunc();

typedef void (*AssertFailedNotifyFunc_t)();
DBG_INTERFACE void SetAssertFailedNotifyFunc( AssertFailedNotifyFunc_t func );
DBG_INTERFACE void CallAssertFailedNotifyFunc();

// predec of minidump methods
PLATFORM_INTERFACE void WriteMiniDump( const char *pchMsg, const char *pchFile, int nLine, bool bWriteFullDump,  bool bFatal );
PLATFORM_INTERFACE bool BGetMiniDumpLock();
PLATFORM_INTERFACE bool BBlockingGetMiniDumpLock();
PLATFORM_INTERFACE bool BWritingMiniDump();
PLATFORM_INTERFACE void MiniDumpUnlock();
PLATFORM_INTERFACE bool BWritingFatalMiniDump();
PLATFORM_INTERFACE bool BWritingNonFatalMiniDump();
PLATFORM_INTERFACE bool SetFullMiniDump( bool );
PLATFORM_INTERFACE bool GetFullMiniDump( );

#endif // #if 0

/* Used to define macros, never use these directly. */

// DON'T pass _msg directly to _SpewMessage() as the format string directly because it might contain format string chars 
// (i.e if Assert( V_snprintf( "%s", pchFoo ) ); fails) 
// Only do the assert if we can get the minidump lock (meaning the minidump is NOT being run in any other thread
// AND if !BWritingMiniDump(), meaning we are not writing the minidump in THIS thread

#ifdef _PREFAST_
	// When doing /analyze builds define the assert macros to be __analysis_assume. This tells
	// the compiler to assume that the condition is true, which helps to suppress many
	// warnings. This define is done in debug and release builds, but debug builds should be
	// preferred for static analysis because some asserts are compiled out in release.
	// The unfortunate !! is necessary because otherwise /analyze is incapable of evaluating
	// all of the logical expressions that the regular compiler can handle.
	// Include _msg in the macro so that format errors in it are detected.
	#define _AssertMsgSmall( _exp, _msg, _bFatal, _bWriteFullDump )  do { __analysis_assume( !!(_exp) ); _msg; } while (0)
	#define  _AssertMsgOnce( _exp, _msg, _bFatal, _bWriteFullDump )  do { __analysis_assume( !!(_exp) ); _msg; } while (0)
	// Force asserts on for /analyze so that we get a __analysis_assume of all of the constraints.
	#define DBGFLAG_ASSERT
	#define DBGFLAG_ASSERTFATAL
#else
	#define _AssertMsgSmall( _exp, _msg, _bFatal, _bWriteFullDump ) \
		do { \
			if ( !(_exp) ) \
			{ \
				AssertMsgImplementation( _msg, _bFatal, __TFILE__, __LINE__, _bWriteFullDump ); \
			} \
		} while (0)

	#define  _AssertMsgOnce( _exp, _msg, _bFatal, _bWriteFullDump ) \
		do {																\
			static bool fAsserted = false;									\
			if ( !fAsserted && !(_exp) )									\
			{ 																\
				fAsserted = true;											\
				AssertMsgImplementation( _msg, _bFatal, __TFILE__, __LINE__, _bWriteFullDump ); \
			}																\
		} while (0)
#endif

/* Spew macros... */

// AssertFatal macros
// AssertFatal is used to detect an unrecoverable error condition.
// If enabled, it may display an assert dialog (if DBGFLAG_ASSERTDLG is turned on or running under the debugger),
// and always terminates the application

#ifdef DBGFLAG_ASSERTFATAL

#define  AssertFatal( _exp )									_AssertMsgSmall( _exp, _T("Fatal Assertion Failed: ") _T(#_exp), true, false )
#define  AssertFatalOnce( _exp )								_AssertMsgOnce( _exp, _T("Fatal Assertion Failed: ") _T(#_exp), true, false )
#define  AssertFatalMsg( _exp, _msg )							_AssertMsgSmall( _exp, CDbgFmtMsg( "Fatal Assertion Failed: %s", (const char*)_msg ).ToString(), true, false )
#define  AssertFatalMsgOnce( _exp, _msg )						_AssertMsgOnce( _exp, CDbgFmtMsg( "Fatal Assertion Failed: %s", (const char*)_msg ).ToString(), true, false )
#define  AssertFatalEquals( _exp, _expectedValue )				AssertFatalMsg2( (_exp) == (_expectedValue), _T("Expected %d but got %d!"), (_expectedValue), (_exp) ) 
#define  AssertFatalFloatEquals( _exp, _expectedValue, _tol )   AssertFatalMsg2( fabs((_exp) - (_expectedValue)) <= (_tol), _T("Expected %f but got %f!"), (_expectedValue), (_exp) )
#define  VerifyFatal( _exp )									AssertFatal( _exp )
#define  VerifyEqualsFatal( _exp, _expectedValue )				AssertFatalEquals( _exp, _expectedValue )
#define  DbgVerifyFatal( _exp )									AssertFatal( _exp )

#define	 AssertFatalFullDumpMsg( _exp, _msg )					_AssertMsgSmall( _exp, CDbgFmtMsg( "Fatal Assertion Failed: %s", (const char*)_msg ).ToString(), true, true )
#define  AssertFatalFullDump( _exp )							_AssertMsgSmall( _exp, _T("Fatal Assertion Failed: ") _T(#_exp), true, true )
#define  AssertFatalOnceFullDump( _exp )						_AssertMsgOnce( _exp, _T("Fatal Assertion Failed: ") _T(#_exp), true, true )

#define  AssertFatalMsg1( _exp, _msg, a1 )									AssertFatalMsg( _exp, CDbgFmtMsg( _msg, a1 ).ToString())
#define  AssertFatalMsg2( _exp, _msg, a1, a2 )								AssertFatalMsg( _exp, CDbgFmtMsg( _msg, a1, a2 ).ToString())
#define  AssertFatalMsg3( _exp, _msg, a1, a2, a3 )							AssertFatalMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3 ).ToString())
#define  AssertFatalMsg4( _exp, _msg, a1, a2, a3, a4 )						AssertFatalMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4 ).ToString())
#define  AssertFatalMsg5( _exp, _msg, a1, a2, a3, a4, a5 )					AssertFatalMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5 ).ToString())
#define  AssertFatalMsg6( _exp, _msg, a1, a2, a3, a4, a5, a6 )				AssertFatalMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5, a6 ).ToString())
#define  AssertFatalMsg6( _exp, _msg, a1, a2, a3, a4, a5, a6 )				AssertFatalMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5, a6 ).ToString())
#define  AssertFatalMsg7( _exp, _msg, a1, a2, a3, a4, a5, a6, a7 )			AssertFatalMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5, a6, a7 ).ToString())
#define  AssertFatalMsg8( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8 )		AssertFatalMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5, a6, a7, a8 ).ToString())
#define  AssertFatalMsg9( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8, a9 )	AssertFatalMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5, a6, a7, a8, a9 ).ToString())

#else // DBGFLAG_ASSERTFATAL

#define  AssertFatal( _exp )									((void)0)
#define  AssertFatalOnce( _exp )								((void)0)
#define  AssertFatalMsg( _exp, _msg )							((void)0)
#define  AssertFatalMsgOnce( _exp, _msg )						((void)0)
#define  AssertFatalFunc( _exp, _f )							((void)0)
#define  AssertFatalEquals( _exp, _expectedValue )				((void)0)
#define  AssertFatalFloatEquals( _exp, _expectedValue, _tol )	((void)0)
#define  VerifyFatal( _exp )									(_exp)
#define  VerifyEqualsFatal( _exp, _expectedValue )				(_exp)
#define  DbgVerifyFatal( _exp )									(_exp)

#define	 AssertFatalFullDumpMsg( _exp, _msg )					((void)0)

#define  AssertFatalMsg1( _exp, _msg, a1 )									((void)0)
#define  AssertFatalMsg2( _exp, _msg, a1, a2 )								((void)0)
#define  AssertFatalMsg3( _exp, _msg, a1, a2, a3 )							((void)0)
#define  AssertFatalMsg4( _exp, _msg, a1, a2, a3, a4 )						((void)0)
#define  AssertFatalMsg5( _exp, _msg, a1, a2, a3, a4, a5 )					((void)0)
#define  AssertFatalMsg6( _exp, _msg, a1, a2, a3, a4, a5, a6 )				((void)0)
#define  AssertFatalMsg6( _exp, _msg, a1, a2, a3, a4, a5, a6 )				((void)0)
#define  AssertFatalMsg7( _exp, _msg, a1, a2, a3, a4, a5, a6, a7 )			((void)0)
#define  AssertFatalMsg8( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8 )		((void)0)
#define  AssertFatalMsg9( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8, a9 )	((void)0)

#endif // DBGFLAG_ASSERTFATAL

#if defined( _PS3 )
// lightweight assert macros: in theory, can be run in release without slowing it down
#if defined(_CERT) || defined(_RETAIL) 
#define AssertAligned(PTR)
#else
#  if defined( _X360 )
#    define AssertAligned(PTR) __twnei( intp(PTR) & 0xF, 0 ) // trap if not equal to immediate value; unsigned comparison
//#  elif defined( _PS3 )
//#    define AssertAligned(PTR) __asm__ ("twnei %0,0" : "=r" ( intp(PTR) & 0xF ) ) // trap if not equal to immediate value; unsigned comparison
#  elif defined( DBGFLAG_ASSERT )
#    define  AssertAligned( adr )                               Assert( ( ( ( intp ) ( adr ) ) & 0xf ) == 0 )
#  else
#  define AssertAligned(PTR) 
#  endif
#endif
#endif // _PS3

// Assert macros
// Assert is used to detect an important but survivable error.
// It's only turned on when DBGFLAG_ASSERT is true.

#ifdef DBGFLAG_ASSERT

#define  Assert( _exp )           							_AssertMsgSmall( _exp, _T("Assertion Failed: ") _T(#_exp),  false, false )
#define  AssertMsg( _exp, _msg )  							_AssertMsgSmall( _exp, CDbgFmtMsg( "Assertion Failed: %s", (const char*)_msg ).ToString(), false, false )
#define  AssertOnce( _exp )       							_AssertMsgOnce( _exp, _T("Assertion Failed: ") _T(#_exp), false, false )
#define  AssertMsgOnce( _exp, _msg )  						_AssertMsgOnce( _exp, CDbgFmtMsg( "Assertion Failed: %s", (const char*)_msg ).ToString(), false, false )
#define  AssertEquals( _exp, _expectedValue )              	AssertMsg2( (_exp) == (_expectedValue), _T("Expected %d but got %d!"), (_expectedValue), (_exp) ) 
#define  AssertFloatEquals( _exp, _expectedValue, _tol )  	AssertMsg2( fabs((_exp) - (_expectedValue)) <= (_tol), _T("Expected %f but got %f!"), (_expectedValue), (_exp) )
#define  VerifyEquals( _exp, _expectedValue )           	AssertEquals( _exp, _expectedValue )

#ifdef _DEBUG
#define DbgVerify( _exp )			Assert( _exp )
#define DbgAssert( _exp )			Assert( _exp )
#define DbgAssertMsg( _exp, _msg )	AssertMsg( _exp, _msg )
#else
#define DbgVerify( _exp )			( (void)( _exp ) )
#define DbgAssert( _exp )			( (void)0 )
#define DbgAssertMsg( _exp, _msg )	( (void)0 )
#endif

#if defined( _SERVER )
// full dumps only available on _SERVER side
#define  AssertFullDump( _exp )           					_AssertMsgSmall( _exp, _T("Assertion Failed: ") _T(#_exp),  false, true )
#define  AssertOnceFullDump( _exp )       					_AssertMsgOnce( _exp, _T("Assertion Failed: ") _T(#_exp), false, true )
#else
// anywhere else, they silently regress to minidumps
#define  AssertFullDump( _exp )           					_AssertMsgSmall( _exp, _T("Assertion Failed: ") _T(#_exp),  false, false )
#define  AssertOnceFullDump( _exp )       					_AssertMsgOnce( _exp, _T("Assertion Failed: ") _T(#_exp), false, false )
#endif

#define  AssertMsg1( _exp, _msg, a1 )									AssertMsg( _exp, CDbgFmtMsg( _msg, a1 ).ToString() )
#define  AssertMsg2( _exp, _msg, a1, a2 )								AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2 ).ToString() )
#define  AssertMsg3( _exp, _msg, a1, a2, a3 )							AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3 ).ToString() )
#define  AssertMsg4( _exp, _msg, a1, a2, a3, a4 )						AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4 ).ToString() )
#define  AssertMsg5( _exp, _msg, a1, a2, a3, a4, a5 )					AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5 ).ToString() )
#define  AssertMsg6( _exp, _msg, a1, a2, a3, a4, a5, a6 )				AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5, a6 ).ToString() )
#define  AssertMsg6( _exp, _msg, a1, a2, a3, a4, a5, a6 )				AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5, a6 ).ToString() )
#define  AssertMsg7( _exp, _msg, a1, a2, a3, a4, a5, a6, a7 )			AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5, a6, a7 ).ToString() )
#define  AssertMsg8( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8 )		AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5, a6, a7, a8 ).ToString() )
#define  AssertMsg9( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8, a9 )	AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3, a4, a5, a6, a7, a8, a9 ).ToString() )

#if defined(_DEBUG)
#define  DbgAssertMsg1( _exp, _msg, a1 )								AssertMsg( _exp, CDbgFmtMsg( _msg, a1 ).ToString() )
#define  DbgAssertMsg2( _exp, _msg, a1, a2 )							AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2 ).ToString() )
#define  DbgAssertMsg3( _exp, _msg, a1, a2, a3 )						AssertMsg( _exp, CDbgFmtMsg( _msg, a1, a2, a3 ).ToString() )
#else
#define  DbgAssertMsg1( _exp, _msg, a1 )								((void)0)
#define  DbgAssertMsg2( _exp, _msg, a1, a2 )							((void)0)
#define  DbgAssertMsg3( _exp, _msg, a1, a2, a3 )						((void)0)
#endif

#else // DBGFLAG_ASSERT

#define  Assert( _exp )										((void)0)
#define  AssertOnce( _exp )									((void)0)
#define  AssertMsg( _exp, _msg )							((void)0)
#define  AssertMsgOnce( _exp, _msg )						((void)0)
#define  AssertEquals( _exp, _expectedValue )				((void)0)
#define  AssertFloatEquals( _exp, _expectedValue, _tol )	((void)0)
#define  VerifyEquals( _exp, _expectedValue )           	(_exp)
#define  DbgVerify( _exp )			  (_exp)
#define  AssertFullDump( _exp )								((void)0)
#define  AssertOnceFullDump( _exp )							((void)0)
#define	 DbgAssert( _exp )									((void)0)

#define  AssertMsg1( _exp, _msg, a1 )									((void)0)
#define  AssertMsg2( _exp, _msg, a1, a2 )								((void)0)
#define  AssertMsg3( _exp, _msg, a1, a2, a3 )							((void)0)
#define  AssertMsg4( _exp, _msg, a1, a2, a3, a4 )						((void)0)
#define  AssertMsg5( _exp, _msg, a1, a2, a3, a4, a5 )					((void)0)
#define  AssertMsg6( _exp, _msg, a1, a2, a3, a4, a5, a6 )				((void)0)
#define  AssertMsg6( _exp, _msg, a1, a2, a3, a4, a5, a6 )				((void)0)
#define  AssertMsg7( _exp, _msg, a1, a2, a3, a4, a5, a6, a7 )			((void)0)
#define  AssertMsg8( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8 )		((void)0)
#define  AssertMsg9( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8, a9 )	((void)0)

#define  DbgAssertMsg1( _exp, _msg, a1 )								((void)0)
#define  DbgAssertMsg2( _exp, _msg, a1, a2 )							((void)0)
#define  DbgAssertMsg3( _exp, _msg, a1, a2, a3 )						((void)0)

#endif // DBGFLAG_ASSERT

#define Plat_FatalError Error

#if 0 // SDR_PUBLIC

#if defined ( _CLIENT ) && !defined ( _DEBUG )
#define DMsg true ? (void)true : _DMsg
#else // _CLIENT && !_DEBUG   - so we're either DEBUG CLIENT, or anything else that isn't RELEASE CLIENT
#define DMsg _DMsg
#endif

#if defined( _DEBUG )
#define DbgMsg _DMsg
#else
#define DbgMsg( ... ) ((void)0)
#endif

#if !defined( _PS3 ) || !defined( _CERT )

/* These are always compiled in */
DBG_INTERFACE void Msg( PRINTF_FORMAT_STRING tchar const* pMsg, ... ) FMTFUNCTION( 1, 2 );
DBG_INTERFACE void _DMsg( tchar const *pGroupName, int level, PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 3, 4 );
DBG_INTERFACE void Warning( PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 1, 2 );
DBG_INTERFACE void DWarning( tchar const *pGroupName, int level, PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 3, 4 );
DBG_INTERFACE void Log( PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 1, 2 );
DBG_INTERFACE void DLog( tchar const *pGroupName, int level, PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 3, 4 );
DBG_INTERFACE void Error( PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 1, 2 );

#else

FORCEINLINE void Msg( PRINTF_FORMAT_STRING tchar const* pMsg, ... ) FMTFUNCTION( 1, 2 ) {}
FORCEINLINE void _DMsg( tchar const *pGroupName, int level, PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 3, 4 ) {}
FORCEINLINE void Warning( PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 1, 2 ) {}
FORCEINLINE void DWarning( tchar const *pGroupName, int level, PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 3, 4 ) {}
FORCEINLINE void Log( PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 1, 2 ) {}
FORCEINLINE void DLog( tchar const *pGroupName, int level, PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 3, 4 ) {}
FORCEINLINE void Error( PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 1, 2 ) {}
#endif

// You can use this macro like a runtime assert macro.
// If the condition fails, then Error is called with the message. This macro is called
// like AssertMsg, where msg must be enclosed in parenthesis:
//
// ErrorIfNot( bCondition, ("a b c %d %d %d", 1, 2, 3) );
#define ErrorIfNot( condition, msg ) \
	if ( (condition) )		\
		;					\
	else 					\
	{						\
		Error msg;			\
	}

#if !defined(_XBOX) || !defined(_RETAIL)

// RelAssert is just like a regular assert, except guaranteed to be compiled in release builds
#define RelAssert( _exp ) _AssertMsgSmall( _exp, _T("Assertion Failed: ") _T(#_exp),  false, false )


DBG_INTERFACE void ValidateSpew( class CValidator &validator );

#else

inline void DevMsg( ... ) {}
inline void DevWarning( ... ) {}
inline void DevLog( ... ) {}
inline void ConMsg( ... ) {}
inline void ConLog( ... ) {}
inline void NetMsg( ... ) {}
inline void NetWarning( ... ) {}
inline void NetLog( ... ) {}

#endif

/* Code macros, debugger interface */

#ifdef _DEBUG

#define DBG_CODE( _code )            if (0) ; else { _code }
#define DBG_DCODE( _g, _l, _code )   if (IsSpewActive( _g, _l )) { _code } else {}
#define DBG_BREAK()                  DebuggerBreak()	/* defined in platform.h */ 

#else /* not _DEBUG */

#define DBG_CODE( _code )            ((void)0)
#define DBG_DCODE( _g, _l, _code )   ((void)0)
#define DBG_BREAK()                  ((void)0)

#endif /* _DEBUG */

#endif // #if 0 // SDR_PUBLIC

#if defined( _DEBUG ) && !defined( STEAMNETWORKINGSOCKETS_OPENSOURCE )
template<typename DEST_POINTER_TYPE, typename SOURCE_POINTER_TYPE>
inline DEST_POINTER_TYPE assert_cast(SOURCE_POINTER_TYPE* pSource)
{
    Assert( static_cast<DEST_POINTER_TYPE>(pSource) == dynamic_cast<DEST_POINTER_TYPE>(pSource) );
    return static_cast<DEST_POINTER_TYPE>(pSource);
}
#else
#define assert_cast static_cast
#endif

DBG_INTERFACE void Msg( PRINTF_FORMAT_STRING tchar const* pMsg, ... ) FMTFUNCTION( 1, 2 );
DBG_INTERFACE void Warning( PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 1, 2 );
DBG_INTERFACE void Error( PRINTF_FORMAT_STRING tchar const *pMsg, ... ) FMTFUNCTION( 1, 2 );

#if 0 // SDR_PUBLIC

//-----------------------------------------------------------------------------
// Macro to protect functions that are not reentrant

#ifdef _DEBUG
class CReentryGuard
{
public:
	CReentryGuard(int *pSemaphore)
	 : m_pSemaphore(pSemaphore)
	{
		++(*m_pSemaphore);
	}
	
	~CReentryGuard()
	{
		--(*m_pSemaphore);
	}
	
private:
	int *m_pSemaphore;
};

#define ASSERT_NO_REENTRY() \
	static int fSemaphore##__LINE__; \
	Assert( !fSemaphore##__LINE__ ); \
	CReentryGuard ReentryGuard##__LINE__( &fSemaphore##__LINE__ )
#else
#define ASSERT_NO_REENTRY()
#endif

//-----------------------------------------------------------------------------
//
// Purpose: Embed debug info in each file.
//
#ifdef _WIN32

	#if defined(_DEBUG) && defined(GIVES_DEPRECATED_WARNING_IN_VC8_SO_ITS_DISABLED_FOR_NOW)
		#pragma comment(compiler)
	#endif

#endif

//-----------------------------------------------------------------------------
//
// Purpose: Wrap around a variable to create a simple place to put a breakpoint
//

#ifdef _DEBUG

template< class Type >
class CDataWatcher
{
public:
	const Type& operator=( const Type &val ) 
	{ 
		return Set( val ); 
	}
	
	const Type& operator=( const CDataWatcher<Type> &val ) 
	{ 
		return Set( val.m_Value ); 
	}
	
	const Type& Set( const Type &val )
	{
		// Put your breakpoint here
		m_Value = val;
		return m_Value;
	}
	
	Type& GetForModify()
	{
		return m_Value;
	}
	
	const Type& operator+=( const Type &val ) 
	{
		return Set( m_Value + val ); 
	}
	
	const Type& operator-=( const Type &val ) 
	{
		return Set( m_Value - val ); 
	}
	
	const Type& operator/=( const Type &val ) 
	{
		return Set( m_Value / val ); 
	}
	
	const Type& operator*=( const Type &val ) 
	{
		return Set( m_Value * val ); 
	}
	
	const Type& operator^=( const Type &val ) 
	{
		return Set( m_Value ^ val ); 
	}
	
	const Type& operator|=( const Type &val ) 
	{
		return Set( m_Value | val ); 
	}
	
	const Type& operator++()
	{
		return (*this += 1);
	}
	
	Type operator--()
	{
		return (*this -= 1);
	}
	
	Type operator++( int ) // postfix version..
	{
		Type val = m_Value;
		(*this += 1);
		return val;
	}
	
	Type operator--( int ) // postfix version..
	{
		Type val = m_Value;
		(*this -= 1);
		return val;
	}
	
	// For some reason the compiler only generates type conversion warnings for this operator when used like 
	// CNetworkVarBase<unsigned tchar> = 0x1
	// (it warns about converting from an int to an unsigned char).
	template< class C >
	const Type& operator&=( C val ) 
	{ 
		return Set( m_Value & val ); 
	}
	
	operator const Type&() const 
	{
		return m_Value; 
	}
	
	const Type& Get() const 
	{
		return m_Value; 
	}
	
	const Type* operator->() const 
	{
		return &m_Value; 
	}
	
	Type m_Value;
	
};

#else

template< class Type >
class CDataWatcher
{
private:
	CDataWatcher(); // refuse to compile in non-debug builds
};

#endif


// When you need to explicitly convert from one int type to another
// and want to assert that no information is lost. So this will catch
// if the input was too large for the output type, or if the input
// was negative and the output type is unsigned.
template <class _out, class _in>
_out checked_static_cast( const _in &i )
{
	_out ret = static_cast<_out>( i );

	Assert( static_cast<_in>( ret ) == i );
	Assert( ( i < 0 ) == ( ret < 0 ) );

	return ret;
}

//-----------------------------------------------------------------------------

#endif /* DBG_H */

#endif
