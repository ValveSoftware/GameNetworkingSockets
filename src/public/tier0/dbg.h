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
#define DBG_INTERFACE	extern "C"
#define DBG_OVERLOAD	
#define DBG_CLASS		

//#endif // BUILD_AS_DLL

class CDbgFmtMsg
{
public:
	explicit CDbgFmtMsg(PRINTF_FORMAT_STRING const char *pszFormat, ...) FMTFUNCTION( 2, 3 )
	{ 
		va_list arg_ptr;

		va_start(arg_ptr, pszFormat);
		_vsnprintf(m_szBuf, kBufLen-1, pszFormat, arg_ptr);
		va_end(arg_ptr);

		m_szBuf[kBufLen-1] = 0;
	}

	operator const char *() const				
	{ 
		return m_szBuf; 
	}

	const char *ToString() const				
	{ 
		return m_szBuf; 
	}
protected:
	CDbgFmtMsg() {}
	static constexpr int kBufLen = 256;
	char m_szBuf[kBufLen];
};

// Helper function declaration to encode variadic argument count >= 1 as size of return type
template <typename... A>
static auto DbgMsgCountArgsT( A... a ) -> char(&)[sizeof...( a )];

template <bool bSingleRawStringArgument> class CDbgFmtSafeImplT;

// Single-arg specialization is used with AssertMsg( cond, "string with % in it" )
template <> class CDbgFmtSafeImplT<true> : public CDbgFmtMsg
{
public:
	CDbgFmtSafeImplT( const char *pszMsg )
	{
		_snprintf(m_szBuf, kBufLen-1, "Assertion Failed: %s", pszMsg );
		m_szBuf[kBufLen - 1] = 0;
	}
};

// Multi-arg specialization is used with AssertMsg( cond, "format string %s %d", pszString, iValue )
template <> class CDbgFmtSafeImplT<false> : public CDbgFmtMsg
{
public:
	CDbgFmtSafeImplT( PRINTF_FORMAT_STRING const char *pszFormat, ... ) FMTFUNCTION( 2, 3 )
	{
		static const char szPrefix[] = "Assertion Failed: ";
		constexpr int k_nPrefixLen = sizeof( szPrefix );
		memcpy( m_szBuf, szPrefix, k_nPrefixLen-1 );

		va_list arg_ptr;

		va_start(arg_ptr, pszFormat);
		_vsnprintf(m_szBuf+k_nPrefixLen, kBufLen-k_nPrefixLen-1, pszFormat, arg_ptr);
		va_end(arg_ptr);

		m_szBuf[kBufLen-1] = 0;
	}
};

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
typedef SpewRetval_t (*SpewOutputFunc_t)( SpewType_t spewType, char const *pMsg );

/* Used to redirect spew output */
DBG_INTERFACE void SpewOutputFunc( SpewOutputFunc_t func );
DBG_INTERFACE void AssertMsgImplementation( const char* _msg, bool _bFatal, const char* pstrFile, unsigned int nLine, bool bFullDump );


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
				AssertMsgImplementation( _msg, _bFatal, __FILE__, __LINE__, _bWriteFullDump ); \
			} \
		} while (0)

	#define  _AssertMsgOnce( _exp, _msg, _bFatal, _bWriteFullDump ) \
		do {																\
			static bool fAsserted = false;									\
			if ( !fAsserted && !(_exp) )									\
			{ 																\
				fAsserted = true;											\
				AssertMsgImplementation( _msg, _bFatal, __FILE__, __LINE__, _bWriteFullDump ); \
			}																\
		} while (0)
#endif

/* Spew macros... */

// AssertFatal macros
// AssertFatal is used to detect an unrecoverable error condition.
// If enabled, it may display an assert dialog (if DBGFLAG_ASSERTDLG is turned on or running under the debugger),
// and always terminates the application

#ifdef DBGFLAG_ASSERTFATAL

#define  AssertFatal( _exp )									_AssertMsgSmall( _exp, "Fatal Assertion Failed: " #_exp, true, false )
#define  AssertFatalOnce( _exp )								_AssertMsgOnce( _exp, "Fatal Assertion Failed: " #_exp, true, false )
#define  AssertFatalMsg( _exp, _msg )							_AssertMsgSmall( _exp, CDbgFmtMsg( "Fatal Assertion Failed: %s", (const char*)_msg ).ToString(), true, false )
#define  AssertFatalMsgOnce( _exp, _msg )						_AssertMsgOnce( _exp, CDbgFmtMsg( "Fatal Assertion Failed: %s", (const char*)_msg ).ToString(), true, false )
#define  AssertFatalEquals( _exp, _expectedValue )				AssertFatalMsg2( (_exp) == (_expectedValue), "Expected %d but got %d!", (_expectedValue), (_exp) ) 
#define  AssertFatalFloatEquals( _exp, _expectedValue, _tol )   AssertFatalMsg2( fabs((_exp) - (_expectedValue)) <= (_tol), "Expected %f but got %f!", (_expectedValue), (_exp) )
#define  VerifyFatal( _exp )									AssertFatal( _exp )
#define  VerifyEqualsFatal( _exp, _expectedValue )				AssertFatalEquals( _exp, _expectedValue )
#define  DbgVerifyFatal( _exp )									AssertFatal( _exp )

#define	 AssertFatalFullDumpMsg( _exp, _msg )					_AssertMsgSmall( _exp, CDbgFmtMsg( "Fatal Assertion Failed: %s", (const char*)_msg ).ToString(), true, true )
#define  AssertFatalFullDump( _exp )							_AssertMsgSmall( _exp, "Fatal Assertion Failed: " #_exp), true, true )
#define  AssertFatalOnceFullDump( _exp )						_AssertMsgOnce( _exp, "Fatal Assertion Failed: " #_exp), true, true )

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

#define  Assert( _exp )           							_AssertMsgSmall( _exp, "Assertion Failed: " #_exp,  false, false )
#define  AssertMsg( _exp, ... )  							_AssertMsgSmall( _exp, CDbgFmtSafeImplT< sizeof( DbgMsgCountArgsT( __VA_ARGS__ ) ) == 1 >( __VA_ARGS__ ).ToString(), false, false )
#define  AssertOnce( _exp )       							_AssertMsgOnce( _exp, "Assertion Failed: " #_exp), false, false )
#define  AssertMsgOnce( _exp, ... )  						_AssertMsgOnce( _exp, CDbgFmtSafeImplT< sizeof( DbgMsgCountArgsT( __VA_ARGS__ ) ) == 1 >( __VA_ARGS__ ).ToString(), false, false )
#define  AssertEquals( _exp, _expectedValue )              	AssertMsg2( (_exp) == (_expectedValue), "Expected %d but got %d!", (_expectedValue), (_exp) ) 
#define  AssertFloatEquals( _exp, _expectedValue, _tol )  	AssertMsg2( fabs((_exp) - (_expectedValue)) <= (_tol), "Expected %f but got %f!", (_expectedValue), (_exp) )
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

DBG_INTERFACE void Error( PRINTF_FORMAT_STRING char const *pMsg, ... ) FMTFUNCTION( 1, 2 );

#if !defined(_XBOX) || !defined(_RETAIL)

// RelAssert is just like a regular assert, except guaranteed to be compiled in release builds
#define RelAssert( _exp ) _AssertMsgSmall( _exp, _T("Assertion Failed: ") _T(#_exp),  false, false )

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

DBG_INTERFACE void Error( PRINTF_FORMAT_STRING char const *pMsg, ... ) FMTFUNCTION( 1, 2 );

#endif /* DBG_H */
