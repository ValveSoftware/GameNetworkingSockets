//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Misc platform compatibility wrappers.  This file is a grab back of junk,
// stripped out from the version in Steam branch, just so we can compile.
//
//========================================================================//

#ifndef PLATFORM_H
#define PLATFORM_H
#pragma once

#include "wchartypes.h"
#include "tier0/memdbgoff.h"
#include "tier0/valve_off.h"

#include "minbase/minbase_identify.h"
#include "minbase/minbase_securezeromemory_impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <new>
#include <utility>
#include <string.h>

#include "minbase/minbase_types.h"
#include "minbase/minbase_decls.h"
#include "minbase/minbase_macros.h"
#include "minbase/minbase_endian.h"

#ifdef POSIX

typedef int SOCKET;
#define INVALID_SOCKET (-1)

typedef int OSFILEHANDLE;
#define INVALID_OSFILEHANDLE (-1)

// Appropriate for any I/O handle, such as a file or socket.
typedef int OSANYIOHANDLE;
#define INVALID_OSANYIOHANDLE (-1)

#else

typedef uintp SOCKET;
#define INVALID_SOCKET	(SOCKET)(~0) // must exactly match winsock2.h to avoid warnings

// Appropriate for any I/O handle, such as a file or socket.
typedef void *OSANYIOHANDLE;
#define INVALID_OSANYIOHANDLE ((void*)((intp)-1)) // aka INVALID_HANDLE_VALUE from windows.h

typedef void *OSFILEHANDLE;
#define INVALID_OSFILEHANDLE ((void*)((intp)-1))

#endif

// Force a function call site -not- to inlined. (useful for profiling)
#define DONT_INLINE(a) (((int)(a)+1)?(a):(a))

// Marks the codepath from here until the next branch entry point as unreachable,
// and asserts if any attempt is made to execute it.
#define UNREACHABLE() { Assert(0); HINT(0); }

// In cases where no default is present or appropriate, this causes MSVC to generate 
// as little code as possible, and throw an assertion in debug.
#define NO_DEFAULT default: UNREACHABLE();

#ifdef _WIN32
// Alloca defined for this platform
	#ifndef stackalloc
		#define  stackalloc( _size ) _alloca( _size )
	#endif
	#ifndef stackfree
		#define  stackfree( _p )   (void) _p
	#endif
#elif defined( COMPILER_GCC ) || defined( COMPILER_SNC ) 
	#define stackalloc( _size )		alloca( ALIGN_VALUE( _size, 16 ) )
	#define  stackfree( _p )			(void) _p
#elif defined(POSIX)
// Alloca defined for this platform
#include <alloca.h>
#define  stackalloc( _size ) alloca( _size )
#define  stackfree( _p )   (void) _p
#endif

// Enable/disable warnings.
#include "minbase/minbase_warnings.h"
// Pull in the /analyze code annotations.
#include "minbase/minbase_annotations.h"

#include "platformtime.h"

#if defined( POSIX )
// handle mapping windows names used in tier0 to posix names in one place
#define _snprintf snprintf //validator.cpp
#if !defined( stricmp )
#define stricmp strcasecmp // assert_dialog.cpp
#endif

#if !defined( _stricmp )
#define _stricmp strcasecmp // validator.cpp
#endif
#define _strcmpi strcasecmp // vprof.cpp

#include <errno.h>
inline int GetLastError() { return errno; }
#define OutputDebugString( str ) fputs( str, stderr );


// these are mostly fileops that will be moved into tier1
#define _chdir chdir
#define _chmod chmod
#define _close close
#define _fileno fileno
#define _fstat fstat
#define _getcwd getcwd
#define _lseek lseek
#define _open( fname, mode ) open( fname, mode, S_IRWXU | S_IRWXG | S_IRWXO )
#define _putenv( envstr ) putenv( strdup( envstr ) )
#define _read read
#define _rmdir rmdir
#define SetEnvironmentVariable SetEnvironmentVariableA
#define SetEnvironmentVariableA( var, val ) setenv( var, val, 1 )
#define _stat stat
#define _stat32 stat
#define _stat64 stat64
#define __stat64 stat64
#define _tzset tzset
#define _unlink unlink
#define _write write

// a handful of stdio defines
#define _O_BINARY 0
#define _O_CREAT O_CREAT
#define _O_WRONLY O_WRONLY
#define _S_IFDIR S_IFDIR
#define _S_IFREG S_IFREG

// note: apparently these are obsolete (according to gnu.org [thx google]).
#define _S_IREAD S_IREAD
#define _S_IWRITE S_IWRITE

#endif

#define PLATFORM_INTERFACE	extern "C"
#define PLATFORM_CXX_INTERFACE
#define PLATFORM_OVERLOAD	

// #endif	// BUILD_AS_DLL

// Processor Information:
struct CPUInformation
{
	int	 m_Size;			// Size of this structure, for forward compatability.

	bool m_bRDTSC : 1,		// Is RDTSC supported?
		 m_bCMOV  : 1,		// Is CMOV supported?
		 m_bFCMOV : 1,		// Is FCMOV supported?
		 m_bSSE	  : 1,		// Is SSE supported?
		 m_bSSE2  : 1,		// Is SSE2 Supported?
		 m_bSSE3  : 1,		// Is SSE3 Supported?
		 m_bSSSE3 : 1,		// Is SSSE3 Supported?
		 m_bSSE41 : 1,		// Is SSE41 Supported?
		 m_bSSE42 : 1,		// Is SSE42 Supported?
		 m_bSSE4a : 1,		// Is SSE4a Supported?
		 m_b3DNow : 1,		// Is 3DNow! Supported?
		 m_bMMX   : 1,		// Is MMX supported?
		 m_bHT	  : 1,		// Is HyperThreading supported?
		 m_bAES   : 1,		// Is AES supported?
		 m_bAVX   : 1,		// Is AVX supported?
		 m_bCMPXCHG16B : 1,	// Is CMPXCHG16B supported?
		 m_bLAHFSAHF : 1;	// Is LAHF/SAHFsupported?
//		 m_bPrefetchW : 1;	// Is PrefetchWsupported?
	
//	uint8 m_nLogicalProcessors,		// Number op logical processors.
//		  m_nPhysicalProcessors;	// Number of physical processors
//
//	int64 m_Speed;					// In cycles per second.

	const char* m_szProcessorID;	// Processor vendor Identification.
//	const char* m_szProcessorBrand;	// Processor brand string.
//	int32 m_cunProcessDetail;
//	const uint32* m_punProcessDetail; // All the bits from cpuid glued together
};

PLATFORM_CXX_INTERFACE const CPUInformation& GetCPUInformation();

// ---------------------------------------------------------------------------------- //
// This struct is passed between platforms, such as a Linux client
// sending its information to a server running on Windows.
// It must compile to the same thing on all platforms and must be
// completely self-contained.
// Additions should only come at the end for cross-version compatibility.
// ---------------------------------------------------------------------------------- //

// Bit fields in CPUInformation are not portable, so
// express as explicit flags.
#define PROC_FEATURE_RDTSC			0x00000001
#define PROC_FEATURE_CMOV			0x00000002
#define PROC_FEATURE_FCMOV			0x00000004
#define PROC_FEATURE_SSE			0x00000008
#define PROC_FEATURE_SSE2			0x00000010
#define PROC_FEATURE_SSE3			0x00000020
#define PROC_FEATURE_SSSE3			0x00000040
#define PROC_FEATURE_SSE41			0x00000080
#define PROC_FEATURE_SSE42			0x00000100
#define PROC_FEATURE_SSE4A			0x00000200
#define PROC_FEATURE_3DNOW			0x00000400
#define PROC_FEATURE_MMX			0x00000800
#define PROC_FEATURE_HT				0x00001000
#define PROC_FEATURE_AES			0x00002000
#define PROC_FEATURE_AVX			0x00004000
#define PROC_FEATURE_CMPXCHG16B		0x00008000
#define PROC_FEATURE_LAHFSAHF		0x00010000
#define PROC_FEATURE_PREFETCHW		0x00020000


// m_VendorId is a null-terminated string.
#define PROC_FLAG_VENDOR_ID_IS_STR 0x0001

enum EProcessorType
{
	k_EProcessorTypeUnknown,
	k_EProcessorTypeX86,
	k_EProcessorTypeX64,
};

PLATFORM_INTERFACE bool Plat_IsInDebugSession();

//-----------------------------------------------------------------------------
// There is no requirement that a va_list be usable in multiple calls,
// but the Steam code does this.  Linux64 does not support reuse, whereas
// Windows does, so Linux64 breaks on code that was written and working
// on Windows.  Fortunately Linux has va_copy, which provides a simple
// way to let a va_list be used multiple times.  Unfortunately Windows
// does not have va_copy, so here we provide things to hide the difference.
//-----------------------------------------------------------------------------

class CReuseVaList
{
public:
    CReuseVaList( va_list List )
    {
#if defined(LINUX) || defined(OSX) || defined(IOS)
        va_copy( m_ReuseList, List );
#else
        m_ReuseList = List;
#endif
    }
    ~CReuseVaList()
    {
#if defined(LINUX) || defined(OSX)
        va_end( m_ReuseList );
#endif
    }

    va_list m_ReuseList;
};

//-----------------------------------------------------------------------------
// Methods to invoke the constructor, copy constructor, and destructor
//-----------------------------------------------------------------------------

template <class T> 
inline void Construct( T* pMemory )
{
	HINT( pMemory != 0 );
	::new( pMemory ) T;
}

template <class T> 
inline void CopyConstruct( T* pMemory, T const& src )
{
	HINT( pMemory != 0 );
	::new( pMemory ) T(src);
}

#ifdef VALVE_RVALUE_REFS
template <class T>
inline void CopyConstruct( T* pMemory, T&& src )
{
	HINT( pMemory != 0 );
	::new(pMemory)T( std::forward<T>(src) );
}
#endif

template <class T, class P>
inline void ConstructOneArg( T* pMemory, P const& arg)
{
	HINT( pMemory != 0 );
	::new( pMemory ) T(arg);
}

template <class T, class P, class P2 >
inline void ConstructTwoArg( T* pMemory, P const& arg1, P2 const &arg2 )
{
	HINT( pMemory != 0 );
	::new( pMemory ) T(arg1, arg2);
}

template <class T, class P, class P2 >
inline void ConstructTwoArgNoConst( T* pMemory, P& arg1, P2& arg2 )
{
	HINT( pMemory != 0 );
	::new( pMemory ) T( arg1, arg2 );
}

template <class T, class P, class P2, class P3, class P4, class P5, class P6, class P7 >
inline void ConstructSevenArg( T* pMemory, P const& arg1, P2 const &arg2, P3 const &arg3, P4 const &arg4, P5 const &arg5, P6 const &arg6, P7 const &arg7 )
{
	HINT( pMemory != 0 );
	::new( pMemory ) T(arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

template <class T> 
inline void Destruct( T* pMemory )
{
	pMemory->~T();

#ifdef _DEBUG
	memset( (void*)pMemory, 0xDD, sizeof(T) );
#endif
}

#endif /* PLATFORM_H */
