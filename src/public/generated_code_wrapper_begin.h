//============ Copyright © Valve Corporation, All rights reserved. ============//
//
// Purpose: expected to be included just before a block of externally generated code
//
// $NoKeywords: $
//=============================================================================//

#if !defined( _GCWB_INTERNAL_MACROS_DEFINED )
#	if defined( __clang__ )
#		define _GCWB_COMPILER_CLANG 1
#	elif defined(__GCC__) || defined(__GNUC__)
#		define _GCWB_COMPILER_GCC 1
#	elif defined( _MSC_VER )
#		define _GCWB_COMPILER_MSVC 1
#	endif

#	if defined( _GCWB_COMPILER_MSVC )
#		define _GCWB_PUSH_WARNING_STATE()						__pragma(warning(push))
#		define _GCWB_POP_WARNING_STATE()						__pragma(warning(pop))
#		define _GCWB_MSVC_DISABLE_WARNING( _warningNumber )		__pragma(warning(disable: _warningNumber))
#		define _GCWB_GCC_DISABLE_WARNING( _rawUnqotedType )
#	elif defined( _GCWB_COMPILER_CLANG ) || defined( _GCWB_COMPILER_GCC )
#		define _GCWB_GCC_INLINE_PRAGMA( _fullUnquotedToken ) _Pragma( #_fullUnquotedToken )
#		if defined( _GCWB_COMPILER_CLANG )
#			define _GCWB_PUSH_WARNING_STATE()		_Pragma("clang diagnostic push")
#			define _GCWB_POP_WARNING_STATE()		_Pragma("clang diagnostic pop")
#			define __GCWB_GCC_DISABLE_WARNING_INNER( _joinedUnquoted )	_GCWB_GCC_INLINE_PRAGMA( clang diagnostic ignored #_joinedUnquoted )
#		else
#			define _GCWB_PUSH_WARNING_STATE()		_Pragma("GCC diagnostic push")
#			define _GCWB_POP_WARNING_STATE()		_Pragma("GCC diagnostic pop")
#			define __GCWB_GCC_DISABLE_WARNING_INNER( _joinedUnquoted )	_GCWB_GCC_INLINE_PRAGMA( GCC diagnostic ignored #_joinedUnquoted )
#		endif
#		define _GCWB_MSVC_DISABLE_WARNING( _warningNumber )
#		define _GCWB_GCC_DISABLE_WARNING( _rawUnqotedType ) __GCWB_GCC_DISABLE_WARNING_INNER( -W ## _rawUnqotedType )
#	endif

#	define _GCWB_INTERNAL_MACROS_DEFINED
#endif //#if !defined( _GCWB_INTERNAL_MACROS_DEFINED )





#if defined( GENERATED_CODE_IS_PROTOBUF_HEADER ) || defined( GENERATED_CODE_IS_PROTOBUF_SOURCE )
	_GCWB_PUSH_WARNING_STATE()

	_GCWB_MSVC_DISABLE_WARNING( 4530 )	// warning C4530: C++ exception handler used, but unwind semantics are not enabled. Specify /EHsc (disabled due to std headers having exception syntax)
	_GCWB_MSVC_DISABLE_WARNING( 4244 )	// warning C4244:  warning C4244: '=' : conversion from '__w64 int' to 'int', possible loss of data
	_GCWB_MSVC_DISABLE_WARNING( 4267 )	// warning C4267: 'argument' : conversion from 'size_t' to 'int', possible loss of data
	_GCWB_MSVC_DISABLE_WARNING( 4125 )	// warning C4125: decimal digit terminates octal escape sequence
	_GCWB_MSVC_DISABLE_WARNING( 4127 )	// warning C4127: conditional expression is constant
	_GCWB_MSVC_DISABLE_WARNING( 4100 )	// warning C4100: 'op' : unreferenced formal parameter
	_GCWB_MSVC_DISABLE_WARNING( 4456 )	// warning C4456: declaration of 'size' hides previous local declaration
	_GCWB_MSVC_DISABLE_WARNING( 4800 )	// warning C4800: 'type': forcing value to bool 'true' or 'false' (performance warning)

	_GCWB_GCC_DISABLE_WARNING( shadow )
#if __GNUC__ >= 6
	_GCWB_GCC_DISABLE_WARNING( misleading-indentation )
#endif


#endif //#if defined( GENERATED_CODE_IS_PROTOBUF_HEADER ) || defined( GENERATED_CODE_IS_PROTOBUF_SOURCE )
