//====== Copyright 1996-2012, Valve Corporation, All rights reserved. ======//
//
// Static compilation analysis annotations.
//
//==========================================================================//

#ifndef MINBASE_ANNOTATIONS_H
#define MINBASE_ANNOTATIONS_H
#pragma once

// Identification is needed everywhere but we don't want to include
// it over and over again so just make sure it was already included.
#ifndef MINBASE_IDENTIFY_H
#error Must include minbase_identify.h
#endif

// Pass hints to the compiler to prevent it from generating unnessecary / stupid code
// in certain situations.  Several compilers other than MSVC also have an equivalent 
// construct.
//
// Essentially the 'Hint' is that the condition specified is assumed to be true at 
// that point in the compilation.  If '0' is passed, then the compiler assumes that
// any subsequent code in the same 'basic block' is unreachable, and thus usually 
// removed.
#ifdef _MSC_VER
	#define HINT(THE_HINT)	__assume((THE_HINT))
#elif defined ( COMPILER_GCC ) && ( ( __GNUC__ > 4 || ( __GNUC__ == 4 && __GNUC_MINOR__ >= 5 ) ) || __clang__ )
	#define HINT(THE_HINT)	do { if ( !(THE_HINT) ) __builtin_unreachable(); } while (0)
#elif defined ( COMPILER_GCC ) || defined ( COMPILER_SNC ) 
	#define HINT(THE_HINT)	__builtin_expect( THE_HINT, 1 )
#else
    #define HINT(THE_HINT)	(void)0
#endif

#if _MSC_VER >= 1600 // VS 2010 and above.
// Suppress some code analysis warnings
#ifdef _PREFAST_
// Include the annotation header file.
#include <sal.h>

// /Analyze warnings can only be suppressed when using a compiler that supports them, which VS 2010
// Professional does not.

// We don't care about these warnings because they are bugs that can only occur during resource
// exhaustion or other unexpected API failure, which we are nowhere near being able to handle.
#pragma warning(disable : 6308) // warning C6308: 'realloc' might return null pointer: assigning null pointer to 's_ppTestCases', which is passed as an argument to 'realloc', will cause the original memory block to be leaked
#pragma warning(disable : 6255) // warning C6255: _alloca indicates failure by raising a stack overflow exception. Consider using _malloca instead
#pragma warning(disable : 6387) // warning C6387: 'argument 1' might be '0': this does not adhere to the specification for the function 'GetProcAddress'
#pragma warning(disable : 6309) // warning C6309: Argument '1' is null: this does not adhere to function specification of 'GetProcAddress'
#pragma warning(disable : 6011) // warning C6011: Dereferencing NULL pointer 'm_ppTestCases'
#pragma warning(disable : 6211) // warning C6211: Leaking memory 'newKeyValue' due to an exception. Consider using a local catch block to clean up memory
#pragma warning(disable : 6031) // warning C6031: Return value ignored: '_getcwd'

// These warnings are because /analyze doesn't like our use of constants, especially things like IsPC()
#pragma warning(disable : 6326) // warning C6326: Potential comparison of a constant with another constant
#pragma warning(disable : 6239) // warning C6239: (<non-zero constant> && <expression>) always evaluates to the result of <expression>. Did you intend to use the bitwise-and operator?
#pragma warning(disable : 6285) // warning C6285: (<non-zero constant> || <non-zero constant>) is always a non-zero constant. Did you intend to use the bitwise-and operator?
#pragma warning(disable : 6237) // warning C6237: (<zero> && <expression>) is always zero. <expression> is never evaluated and might have side effects
#pragma warning(disable : 6235) // warning C6235: (<non-zero constant> || <expression>) is always a non-zero constant
#pragma warning(disable : 6240) // warning C6240: (<expression> && <non-zero constant>) always evaluates to the result of <expression>. Did you intend to use the bitwise-and operator?

// These warnings aren't really important:
#pragma warning(disable : 6323) // warning C6323: Use of arithmetic operator on Boolean type(s)

// Miscellaneous other /analyze warnings. We should consider fixing these at some point.
//#pragma warning(disable : 6204) // warning C6204: Possible buffer overrun in call to 'memcpy': use of unchecked parameter 'src'
//#pragma warning(disable : 6262) // warning C6262: Function uses '16464' bytes of stack: exceeds /analyze:stacksize'16384'. Consider moving some data to heap
// This is a serious warning. Don't suppress it.
//#pragma warning(disable : 6263) // warning C6263: Using _alloca in a loop: this can quickly overflow stack
// 6328 is also used for passing __int64 to printf when int is expected so we can't suppress it.
//#pragma warning(disable : 6328) // warning C6328: 'char' passed as parameter '1' when 'unsigned char' is required in call to 'V_isdigit'
// /analyze doesn't like GCOMPILER_ASSERT's implementation of compile-time asserts
#pragma warning(disable : 6326) // warning C6326: Potential comparison of a constant with another constant
#pragma warning(disable : 6335) // warning C6335: Leaking process information handle 'pi.hThread'
#pragma warning(disable : 6320) // warning C6320: Exception-filter expression is the constant EXCEPTION_EXECUTE_HANDLER. This might mask exceptions that were not intended to be handled
#pragma warning(disable : 6250) // warning C6250: Calling 'VirtualFree' without the MEM_RELEASE flag might free memory but not address descriptors (VADs). This causes address space leaks
#pragma warning(disable : 6384) // ientity2_class_h_schema_gen.cpp(76): warning C6384: Dividing sizeof a pointer by another value

// For temporarily suppressing warnings -- the warnings are suppressed for the next source line.
#define ANALYZE_SUPPRESS(wnum) __pragma(warning(suppress: wnum))
#define ANALYZE_SUPPRESS2(wnum1, wnum2) __pragma(warning(supress: wnum1  wnum2))
#define ANALYZE_SUPPRESS3(wnum1, wnum2, wnum3) __pragma(warning(suppress: wnum1 wnum2 wnum3))
#define ANALYZE_SUPPRESS4(wnum1, wnum2, wnum3, wnum4) __pragma(warning(suppress: wnum1 wnum2 wnum3 wnum4))

// Tag all printf-style format strings with this (consumed by MSVC).
#define PRINTF_FORMAT_STRING _Printf_format_string_
#define SCANF_FORMAT_STRING _Scanf_format_string_impl_

// Various macros for specifying the capacity of the buffer pointed
// to by a function parameter. Variations include in/out/inout,
// CAP (elements) versus BYTECAP (bytes), and null termination or
// not (_Z).
#define IN_Z _In_z_
#define IN_CAP(x) _In_count_(x)
#define IN_BYTECAP(x) _In_bytecount_(x)
#define OUT_Z_CAP(x) _Out_z_cap_(x)
#define OUT_CAP(x) _Out_cap_(x)
#define OUT_BYTECAP(x) _Out_bytecap_(x)
#define OUT_Z_BYTECAP(x) _Out_z_bytecap_(x)
#define INOUT_BYTECAP(x) _Inout_bytecap_(x)
#define INOUT_Z_CAP(x) _Inout_z_cap_(x)
#define INOUT_Z_BYTECAP(x) _Inout_z_bytecap_(x)
// These macros are use for annotating array reference parameters, typically used in functions
// such as V_strcpy_safe. Because they are array references the capacity is already known.
#if _MSC_VER >= 1700
#define IN_Z_ARRAY _Pre_z_
#define OUT_Z_ARRAY _Post_z_
#define INOUT_Z_ARRAY _Prepost_z_
#else
#define IN_Z_ARRAY _Deref_pre_z_
#define OUT_Z_ARRAY _Deref_post_z_
#define INOUT_Z_ARRAY _Deref_prepost_z_
#endif // _MSC_VER >= 1700
// Use the macros above to annotate string functions that fill buffers as shown here,
// in order to give VS's /analyze more opportunities to find bugs.
// void V_wcsncpy( OUT_Z_BYTECAP(maxLenInBytes) wchar_t *pDest, wchar_t const *pSrc, int maxLenInBytes );
// int V_snwprintf( OUT_Z_CAP(maxLenInCharacters) wchar_t *pDest, int maxLenInCharacters, PRINTF_FORMAT_STRING const wchar_t *pFormat, ... );

#endif // _PREFAST_
#endif // _MSC_VER >= 1600 // VS 2010 and above.

#ifndef ANALYZE_SUPPRESS
#define ANALYZE_SUPPRESS(wnum)
#define ANALYZE_SUPPRESS2(wnum1, wnum2)
#define ANALYZE_SUPPRESS3(wnum1, wnum2, wnum3)
#define ANALYZE_SUPPRESS4(wnum1, wnum2, wnum3, wnum4)
// Tag all printf-style format strings with this (consumed by MSVC).
#define PRINTF_FORMAT_STRING
#define SCANF_FORMAT_STRING
#define IN_Z
#define IN_CAP(x)
#define IN_BYTECAP(x)
#define OUT_Z_CAP(x)
#define OUT_CAP(x)
#define OUT_BYTECAP(x)
#define OUT_Z_BYTECAP(x)
#define INOUT_BYTECAP(x)
#define INOUT_Z_CAP(x)
#define INOUT_Z_BYTECAP(x)
#define OUT_Z_ARRAY
#define INOUT_Z_ARRAY
#endif

// Tag the end of functions using printf-style format strings with this (consumed by GCC).
// Note that for methods the 'this' pointer counts as the first argument in argument numbering.
#ifdef __MINGW32__
#define FMTFUNCTION( fmtargnumber, firstvarargnumber ) __attribute__ (( format( __MINGW_PRINTF_FORMAT, fmtargnumber, firstvarargnumber )))
#elif defined(COMPILER_GCC)
#define FMTFUNCTION( fmtargnumber, firstvarargnumber ) __attribute__ (( format( __printf__, fmtargnumber, firstvarargnumber )))
#else
#define FMTFUNCTION( fmtargnumber, firstvarargnumber )
#endif

#endif // #ifndef MINBASE_ANNOTATIONS_H
