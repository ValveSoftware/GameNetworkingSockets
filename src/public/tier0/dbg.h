//===== Copyright 1996-2005, Valve Corporation, All rights reserved. ========//
//
// Assert implementation.
//
// This is a custom version for the standalone version of
// SteamnetworkingSockets.  It was taken from the Steam code and then
// stripped to the bare essentials.
//
//===========================================================================//
#ifndef DBG_H
#define DBG_H
#pragma once

#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#include "basetypes.h"
#include "platform.h"
#include <vstdlib/strtools.h>

// DBGFLAG_ASSERT is defined when we want Assert to do something.  By default,
// we write our code so that most asserts can be compiled in without negatively
// impacting performance, and so this is defined, even in release.  However,
// no load-bearing code should appear in an Assert(), and so it *should* be legal
// to turn this off if you wish.
#define DBGFLAG_ASSERT

// DBGFLAG_ASSERTFLAT is defined when AssertFatal should do something.  This
// really must always be defined.
#define DBGFLAG_ASSERTFATAL

// Helper function declaration to encode variadic argument count >= 1 as size of return type
template <typename... A>
static auto DbgMsgCountArgsT( A... a ) -> char(&)[sizeof...( a )];

template <bool bFatal, bool bSingleRawStringArgument> class AssertMsgHelper;

// Single-arg specialization is used with AssertMsg( cond, "string with % in it" )
template <> class AssertMsgHelper<true, true>
{
public:
	static void AssertFailed( const char* pstrFile, unsigned int nLine, const char *pMsg );
};
template <> class AssertMsgHelper<false, true>
{
public:
	static void AssertFailed( const char* pstrFile, unsigned int nLine, const char *pMsg );
};

// Multi-arg specialization is used with AssertMsg( cond, "format string %s %d", pszString, iValue )
template <> class AssertMsgHelper<true, false>
{
public:
	static void AssertFailed( const char* pstrFile, unsigned int nLine, PRINTF_FORMAT_STRING const char *pFmt, ... ) FMTFUNCTION( 3, 4 );
};
template <> class AssertMsgHelper<false, false>
{
public:
	static void AssertFailed( const char* pstrFile, unsigned int nLine, PRINTF_FORMAT_STRING const char *pFmt, ... ) FMTFUNCTION( 3, 4 );
};

/* Used to define macros, never use these directly. */

#ifdef _PREFAST_
	// When doing /analyze builds define the assert macros to be __analysis_assume. This tells
	// the compiler to assume that the condition is true, which helps to suppress many
	// warnings. This define is done in debug and release builds, but debug builds should be
	// preferred for static analysis because some asserts are compiled out in release.
	// The unfortunate !! is necessary because otherwise /analyze is incapable of evaluating
	// all of the logical expressions that the regular compiler can handle.
	// Include _msg in the macro so that format errors in it are detected.
	#define _AssertMsgSmall( _exp, _bFatal, _fmt, ... )  do { __analysis_assume( !!(_exp) ); _fmt; } while (0)
	#define  _AssertMsgOnce( _exp, _bFatal, _fmt, ... )  do { __analysis_assume( !!(_exp) ); _fmt; } while (0)
#else
	#define _AssertMsgSmall( _exp, _bFatal, ... ) \
		do { \
			if ( !(_exp) ) \
			{ \
				AssertMsgHelper< _bFatal, sizeof( DbgMsgCountArgsT( __VA_ARGS__ ) ) == 1 >::AssertFailed( __FILE__, __LINE__, __VA_ARGS__ ); \
			} \
		} while (0)

	#define _AssertMsgOnce( _exp, _bFatal, ... ) \
		do {																\
			static bool fAsserted = false;									\
			if ( !fAsserted && !(_exp) )									\
			{ 																\
				fAsserted = true;											\
				AssertMsgHelper< _bFatal, sizeof( DbgMsgCountArgsT( __VA_ARGS__ ) ) == 1 >::AssertFailed( __FILE__, __LINE__, __VA_ARGS__ ); \
			}																\
		} while (0)
#endif

// AssertFatal is used to detect an unrecoverable error condition.
#define  AssertFatal( _exp )									_AssertMsgSmall( _exp, true, "Fatal Assertion Failed: " #_exp )
#define  AssertFatalMsg( _exp, ... )							_AssertMsgSmall( _exp, true, __VA_ARGS__ )
#define  VerifyFatal( _exp )									AssertFatal( _exp )

// Assert is used to detect an important but survivable error.
// It's only turned on when DBGFLAG_ASSERT is true.
#ifdef DBGFLAG_ASSERT

	#define  Assert( _exp )           							_AssertMsgSmall( _exp, false, "Assertion Failed: " #_exp )
	#define  AssertMsg( _exp, ... )  							_AssertMsgSmall( _exp, false, __VA_ARGS__ )
	#define  AssertOnce( _exp )       							_AssertMsgOnce( _exp, false, "Assertion Failed: " #_exp )
	#define  AssertMsgOnce( _exp, ... )  						_AssertMsgOnce( _exp, false, __VA_ARGS__ )

#else // DBGFLAG_ASSERT

	// Stubs
	#define  Assert( _exp )										((void)0)
	#define  AssertOnce( _exp )									((void)0)
	#define  AssertMsg( _exp, _msg )							((void)0)
	#define  AssertMsgOnce( _exp, _msg )						((void)0)

#endif // DBGFLAG_ASSERT

// DbgAssert is an assert that is only compiled into _DEBUG builds.
// DbgVerify will always evaluate the expression.  In a _DEBUG build,
// it will also Assert that the expression is true.
#if defined(_DEBUG) && defined( DBGFLAG_ASSERT )
	#define DbgAssert( _exp )			Assert( _exp )
	#define DbgAssertMsg( _exp, ... )	AssertMsg( _exp, __VA_ARGS__ )
	#define DbgVerify( _exp )			Assert( _exp )
#else
	#define DbgAssert( _exp )			( (void)0 )
	#define DbgAssertMsg( _exp, ... )	( (void)0 )
	#define DbgVerify( _exp )			(void)(_exp)
#endif

// Some legacy code uses macros named with the number of arguments
#define AssertMsg1( _exp, _msg, a1 )									AssertMsg( _exp, _msg, a1 )
#define AssertMsg2( _exp, _msg, a1, a2 )								AssertMsg( _exp, _msg, a1, a2 )
#define AssertMsg3( _exp, _msg, a1, a2, a3 )							AssertMsg( _exp, _msg, a1, a2, a3 )
#define AssertMsg4( _exp, _msg, a1, a2, a3, a4 )						AssertMsg( _exp, _msg, a1, a2, a3, a4 )
#define AssertMsg5( _exp, _msg, a1, a2, a3, a4, a5 )					AssertMsg( _exp, _msg, a1, a2, a3, a4, a5 )
#define AssertMsg6( _exp, _msg, a1, a2, a3, a4, a5, a6 )				AssertMsg( _exp, _msg, a1, a2, a3, a4, a5, a6 )
#define AssertMsg6( _exp, _msg, a1, a2, a3, a4, a5, a6 )				AssertMsg( _exp, _msg, a1, a2, a3, a4, a5, a6 )
#define AssertMsg7( _exp, _msg, a1, a2, a3, a4, a5, a6, a7 )			AssertMsg( _exp, _msg, a1, a2, a3, a4, a5, a6, a7 )
#define AssertMsg8( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8 )		AssertMsg( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8 )
#define AssertMsg9( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8, a9 )	AssertMsg( _exp, _msg, a1, a2, a3, a4, a5, a6, a7, a8, a9 )
#define DbgAssertMsg1( _exp, _msg, a1 )									DbgAssertMsg( _exp, _msg, a1 )
#define DbgAssertMsg2( _exp, _msg, a1, a2 )								DbgAssertMsg( _exp, _msg, a1, a2 )
#define DbgAssertMsg3( _exp, _msg, a1, a2, a3 )							DbgAssertMsg( _exp, _msg, a1, a2, a3 )

// assert_cast is a static_cast in release.  If _DEBUG, it does a dynamic_cast to make sure
// that the static cast is appropriate.
template<typename DEST_POINTER_TYPE, typename SOURCE_POINTER_TYPE>
inline DEST_POINTER_TYPE assert_cast(SOURCE_POINTER_TYPE* pSource)
{
    DbgAssert( static_cast<DEST_POINTER_TYPE>(pSource) == dynamic_cast<DEST_POINTER_TYPE>(pSource) );
    return static_cast<DEST_POINTER_TYPE>(pSource);
}

#define Plat_FatalError( ... ) AssertFatalMsg( false, __VA_ARGS__ )

#endif /* DBG_H */
