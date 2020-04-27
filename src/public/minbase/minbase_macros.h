//====== Copyright 1996-2013, Valve Corporation, All rights reserved. ======//
//
// Fundamental macros and defines.
//
//==========================================================================//

#ifndef MINBASE_MACROS_H
#define MINBASE_MACROS_H
#pragma once

// Identification is needed everywhere but we don't want to include
// it over and over again so just make sure it was already included.
#ifndef MINBASE_IDENTIFY_H
#error Must include minbase_identify.h
#endif

// Decls are needed everywhere but we don't want to include
// it over and over again so just make sure it was already included.
#ifndef MINBASE_DECLS_H
#error Must include minbase_decls.h
#endif

#define UID_PREFIX generated_id_
#define UID_CAT1(a,c) a ## c
#define UID_CAT2(a,c) UID_CAT1(a,c)
#define EXPAND_CONCAT(a,c) UID_CAT1(a,c)
#define UNIQUE_ID UID_CAT2(UID_PREFIX,__COUNTER__)

//-----------------------------------------------------------------------------
// Macro to assist in asserting constant invariants during compilation
//
// If available use static_assert instead of weird language tricks. This
// leads to much more readable messages when compile time assert constraints
// are violated.
#if (_MSC_VER > 1500 || __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5) || defined(__clang__) )
	#define PLAT_COMPILE_TIME_ASSERT( pred ) static_assert( pred, "Compile time assert constraint is not true: " #pred )
#else
	#define PLAT_COMPILE_TIME_ASSERT( pred ) typedef int UNIQUE_ID[ (pred) ? 1 : -1]
#endif

#if !defined( COMPILE_TIME_ASSERT )
#define COMPILE_TIME_ASSERT( pred ) PLAT_COMPILE_TIME_ASSERT( pred )
#endif
#if !defined( ASSERT_INVARIANT )
#define ASSERT_INVARIANT( pred ) PLAT_COMPILE_TIME_ASSERT( pred )
#endif

#if defined(_WIN32)
#define PLAT_DLL_EXT "dll"
#elif defined(LINUX)
#define PLAT_DLL_EXT "so"
#elif defined(OSX)
#define PLAT_DLL_EXT "dylib"
#endif

#if defined(_WIN32)
#define PLAT_PATH_SLASH "\\"
#elif defined(POSIX)
#define PLAT_PATH_SLASH "/"
#endif

#if defined( _PS3 )
#include <sysutil/sysutil_gamecontent.h>
#define PATH_MAX CELL_GAME_PATH_MAX
#endif

#if (!defined(_WIN32) || defined(WINDED)) && defined(POSIX)
#define MAX_PATH  PATH_MAX
#define _MAX_PATH PATH_MAX
#endif

#ifdef _WIN32
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#define MAX_UNICODE_PATH 32767
#else
#define MAX_UNICODE_PATH MAX_PATH
#endif

#define MAX_UNICODE_PATH_IN_UTF8 (MAX_UNICODE_PATH * 4)

// MSVC CRT uses 0x7fff while gcc uses MAX_INT, leading to mismatches between platforms
// As a result, we pick the least common denominator here.  This should be used anywhere
// you might typically want to use RAND_MAX
#define VALVE_RAND_MAX 0x7fff

#if !defined(DBL_EPSILON) && !defined(_WIN32)
#define DBL_EPSILON 2.2204460492503131e-16
#endif

// need macro for constant expression
#define ALIGN_VALUE_FLOOR( val, alignment ) ( ( val ) & ~( ( alignment ) - 1 ) )
#define ALIGN_VALUE( val, alignment ) ALIGN_VALUE_FLOOR( ( val ) + ( ( alignment ) - 1 ), alignment )

// Used to step into the debugger
#ifdef _WIN64
	#define DebuggerBreak()  do { __debugbreak(); } while(0)
#elif defined( COMPILER_GCC )
	#if defined( _PS3 )
		#if defined( _CERT )
			#define DebuggerBreak() ((void)0)
		#else
			#define DebuggerBreak() do {  __asm volatile ("tw 31,1,1"); } while(0)
		#endif
	#elif defined(__i386__) || defined(__x86_64__)
		#define DebuggerBreak()  do { __asm__ __volatile__ ( "int $3" ); } while(0)
	#else
		#define DebuggerBreak() do { } while(0)
	#endif
#elif defined(_WIN32)
	#define DebuggerBreak()  do { __asm { int 3 }; } while(0)
#elif defined( COMPILER_SNC ) && defined( COMPILER_PS3 )
	static volatile bool sPS3_SuppressAssertsInThisFile = false; // you can throw this in the debugger to temporarily disable asserts inside any particular .cpp module.
	#define DebuggerBreak() if (!sPS3_SuppressAssertsInThisFile) __builtin_snpause(); // <sergiy> from SNC Migration Guide, tw 31,1,1
#else
	#define DebuggerBreak()  do { __asm__ __volatile__ ( "int $3" ); } while(0)
#endif

#if defined(_WIN64)

extern "C"
{
   unsigned __int64 __rdtsc();
}
#pragma intrinsic(__rdtsc)
#define PLAT_CPU_TIME() __rdtsc()

#elif defined(COMPILER_GCC) && ( defined(__i386__) || defined(__x86_64__) )

inline __attribute__ ((always_inline)) unsigned long long PLAT_CPU_TIME()
{
#ifdef PLATFORM_64BITS
    unsigned long long Low, High;
    __asm__ __volatile__ ( "rdtsc" : "=a" (Low), "=d" (High) );
    return (High << 32) | Low;
#else
    unsigned long long Val;
    __asm__ __volatile__ ( "rdtsc" : "=A" (Val) );
    return Val;
#endif
}

#elif defined(_WIN32)

FORCEINLINE unsigned __int64 PLAT_CPU_TIME()
{
    __asm rdtsc
}


#elif defined(POSIX)

#include <sys/time.h>

FORCEINLINE unsigned long long PLAT_CPU_TIME()
{
	struct timeval  tv;
	struct timezone tz;
	gettimeofday(&tv, &tz);
	return ((unsigned long long)tv.tv_sec)*1000000 + (unsigned long long)tv.tv_usec;
}

#endif

//
// GET_OUTER()
//
// A platform-independent way for a contained class to get a pointer to its
// owner. If you know a class is exclusively used in the context of some
// "outer" class, this is a much more space efficient way to get at the outer
// class than having the inner class store a pointer to it.
//
//	class COuter
//	{
//		class CInner // Note: this does not need to be a nested class to work
//		{
//			void PrintAddressOfOuter()
//			{
//				printf( "Outer is at 0x%x\n", GET_OUTER( COuter, m_Inner ) );
//			}
//		};
//
//		CInner m_Inner;
//		friend class CInner;
//	};

#define GET_OUTER( OuterType, OuterMember ) \
   ( ( OuterType * ) ( (uint8 *)this - offsetof( OuterType, OuterMember ) ) )

#define DISALLOW_COPY_CONSTRUCTORS(TypeName)	\
	TypeName(const TypeName&);					\
	void operator=(const TypeName&);

// Using ARRAYSIZE implementation from winnt.h:
#ifdef ARRAYSIZE
#undef ARRAYSIZE
#define ARRAYSIZE Dont_Use_ARRAYSIZE_Use_V_ARRAYSIZE
#endif

// Return the number of elements in a statically sized array.
//   DWORD Buffer[100];
//   RTL_NUMBER_OF(Buffer) == 100
// This is also popularly known as: NUMBER_OF, ARRSIZE, _countof, NELEM, etc.
//
#define RTL_NUMBER_OF_V1(A) (sizeof(A)/sizeof((A)[0]))

#if defined(__cplusplus) && \
	!defined(MIDL_PASS) && \
	!defined(RC_INVOKED) && \
    !defined(SORTPP_PASS)

#if _MSC_FULL_VER >= 13009466 || defined(__clang__) || __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)

#ifdef __GNUC__
#include <stddef.h>
#endif

// RtlpNumberOf is a function that takes a reference to an array of N Ts.
//
// typedef T array_of_T[N];
// typedef array_of_T &reference_to_array_of_T;
//
// RtlpNumberOf returns a pointer to an array of N chars.
// We could return a reference instead of a pointer but older compilers do not accept that.
//
// typedef char array_of_char[N];
// typedef array_of_char *pointer_to_array_of_char;
//
// sizeof(array_of_char) == N
// sizeof(*pointer_to_array_of_char) == N
//
// pointer_to_array_of_char RtlpNumberOf(reference_to_array_of_T);
//
// We never even call RtlpNumberOf, we just take the size of dereferencing its return type.
// We do not even implement RtlpNumberOf, we just decare it.
//
// Attempts to pass pointers instead of arrays to this macro result in compile time errors.
// That is the point.
extern "C++" // templates cannot be declared to have 'C' linkage
template <typename T, size_t N>
char (*RtlpNumberOf( UNALIGNED T (&)[N] ))[N];

#ifndef RTL_NUMBER_OF_V2
#ifdef _PREFAST_
// The +0 is so that we can go:
// size = ARRAYSIZE(array) * sizeof(array[0]) without triggering a /analyze
// warning about multiplying sizeof.
#define RTL_NUMBER_OF_V2(A) (sizeof(*RtlpNumberOf(A))+0)
#else
#define RTL_NUMBER_OF_V2(A) (sizeof(*RtlpNumberOf(A)))
#endif
#endif

#elif defined(LINUX)

// On Linux/gcc, RtlpNumberOf doesn't work well with pointers to structs
// that are defined function locally. Here we have an alternative implementation
// to detect if something is a pointer vs an array.

// We can use sizeof( DetectIfIsPointer( p ) ) to detect if something is a
// pointer rather than an array at compile time:
// If sizeof(int) then it's an array
// If sizeof(char) then it's a pointer
// If sizeof(short) then it's a pointer or array to a non-templatizable type,
// typically a type defined as function-local.
// If it's neither a pointer or an array, it'll generate an error.
template <typename T, unsigned int N>
int DetectIfIsPointer( T (&)[N] );

template <typename T>
char DetectIfIsPointer( T* );

short DetectIfIsPointer( const void* );

// ErrorIfIsPointerNotArrayGivenToARRAYSIZE<b>::Value evaluates to 0 if b
// is true, and generates a compile error otherwise.
template<bool>
struct ErrorIfIsPointerNotArrayGivenToARRAYSIZE
{
	enum { Value = 0 };
};

template<>
struct ErrorIfIsPointerNotArrayGivenToARRAYSIZE<false>
{
};

// Run a barrage of tests to see if this is a pointer rather than an array,
// and fail with a compile error if it's a pointer. We test if it's
// obviously a pointer. And if it's a non-templatizable type, we make sure
// sizeof(A) == 0 or sizeof(A) >= sizeof(*A) -- a test any array should pass,
// but very few pointers-to-structs will.
#define RTL_NUMBER_OF_V2(A) \
  ( ErrorIfIsPointerNotArrayGivenToARRAYSIZE< \
         sizeof( DetectIfIsPointer( ( A ) ) ) == sizeof( int ) || \
         ( sizeof( DetectIfIsPointer( ( A ) ) ) == sizeof( short ) && \
         ( sizeof( A ) == 0 || sizeof( A ) >= sizeof( *A ) ) ) >::Value + \
    RTL_NUMBER_OF_V1(A) )

#endif // Compiler version
#endif // C++

#ifndef RTL_NUMBER_OF_V2
#define RTL_NUMBER_OF_V2(A) RTL_NUMBER_OF_V1(A)
#endif

// ARRAYSIZE is more readable version of RTL_NUMBER_OF_V2
// _ARRAYSIZE is a version useful for anonymous types
#define V_ARRAYSIZE(A)    RTL_NUMBER_OF_V2(A)
#define _ARRAYSIZE(A)   RTL_NUMBER_OF_V1(A)

// A helper to prevent compiler warnings / errors for unused parameters and variables.
// There are three versions in the code, but NOTE_UNUSED is the most
// frequently used and should be used in new code.
#ifndef NOTE_UNUSED
#define NOTE_UNUSED(arg) ((void)arg)
#endif

// Don't define for Windows, since it is defined unconditionally in winnt.h.
#if !defined( UNREFERENCED_PARAMETER ) && !defined( WIN32 )
#define UNREFERENCED_PARAMETER(parm) NOTE_UNUSED(parm)
#endif

#ifndef REFERENCE
#define REFERENCE(arg) NOTE_UNUSED(arg)
#endif


// #define COMPILETIME_MAX and COMPILETIME_MIN for max/min in constant expressions
#define COMPILETIME_MIN( a, b ) ( ( ( a ) < ( b ) ) ? ( a ) : ( b ) )
#define COMPILETIME_MAX( a, b ) ( ( ( a ) > ( b ) ) ? ( a ) : ( b ) )

#endif // #ifndef MINBASE_MACROS_H
