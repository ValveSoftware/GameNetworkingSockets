//====== Copyright 1996-2012, Valve Corporation, All rights reserved. ======//
//
// Enable and disable specific warnings.
//
//==========================================================================//

#ifndef MINBASE_WARNINGS_H
#define MINBASE_WARNINGS_H
#pragma once

// Identification is needed everywhere but we don't want to include
// it over and over again so just make sure it was already included.
#ifndef MINBASE_IDENTIFY_H
#error Must include minbase_identify.h
#endif

#if defined( COMPILER_GCC )

// disable warning: comparing floating point with == or != is unsafe [-Wfloat-equal] 
#pragma GCC diagnostic ignored "-Wfloat-equal"
// disable warning: enumeration value 'kMouseRestoreNone' not handled in switch [-Wswitch]
#pragma GCC diagnostic ignored "-Wswitch"
// disable warning: deprecated conversion from string constant to 'char*' [-Wwrite-strings]
#pragma GCC diagnostic ignored "-Wwrite-strings"

#if __GNUC__ >= 4 && __GNUC_MINOR__ > 2
// disable warning: type qualifiers ignored on function return type [-Wignored-qualifiers]
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
// disable warning: warning: comparison is always true due to limited range of data type [-Wtype-limits]
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif

#endif // #if defined( COMPILER_GCC )

#if defined( COMPILER_SNC )

#pragma diag_suppress=1700	   // warning 1700: class "%s" has virtual functions but non-virtual destructor
#pragma diag_suppress=187	   // warning 187: pointless comparison of unsigned integer with zero

#endif // #if defined( COMPILER_SNC )

#if defined( COMPILER_MSVC32 )

// Remove warnings from warning level 4.
#pragma warning(disable : 4514) // warning C4514: 'acosl' : unreferenced inline function has been removed
#pragma warning(disable : 4100) // warning C4100: 'hwnd' : unreferenced formal parameter
#pragma warning(disable : 4127) // warning C4127: conditional expression is constant
#pragma warning(disable : 4512) // warning C4512: 'InFileRIFF' : assignment operator could not be generated
#pragma warning(disable : 4611) // warning C4611: interaction between '_setjmp' and C++ object destruction is non-portable
#pragma warning(disable : 4710) // warning C4710: function 'x' not inlined
#pragma warning(disable : 4505) // unreferenced local function has been removed
#pragma warning(disable : 4239) // nonstandard extension used : 'argument' ( conversion from class Vector to class Vector& )
#pragma warning(disable : 4097) // typedef-name 'BaseClass' used as synonym for class-name 'CFlexCycler::CBaseFlex'
#pragma warning(disable : 4324) // Padding was added at the end of a structure
#pragma warning(disable : 4244) // type conversion warning.
#pragma warning(disable : 4786)	// Disable warnings about long symbol names
#pragma warning(disable : 4250) // 'X' : inherits 'Y::Z' via dominance
#pragma warning(disable : 4748) // warning C4748: /GS can not protect parameters and local variables from local buffer overrun because optimizations are disabled in function

#if _MSC_VER >= 1300
#pragma warning(disable : 4511)	// Disable warnings about private copy constructors
#pragma warning(disable : 4121)	// warning C4121: 'symbol' : alignment of a member was sensitive to packing
#pragma warning(disable : 4530)	// warning C4530: C++ exception handler used, but unwind semantics are not enabled. Specify /EHsc (disabled due to std headers having exception syntax)
#endif

// When we port to 64 bit, we'll have to resolve the int, ptr vs size_t 32/64 bit problems...
#if !defined( _WIN64 )
#pragma warning( disable : 4267 )	// conversion from 'size_t' to 'int', possible loss of data
#pragma warning( disable : 4311 )	// pointer truncation from 'char *' to 'int'
#pragma warning( disable : 4312 )	// conversion from 'unsigned int' to 'memhandle_t' of greater size
#endif

#if _MSC_VER >= 1600 // VS 2010 and above.
//-----------------------------------------------------------------------------
// Upgrading important helpful warnings to errors
//-----------------------------------------------------------------------------
#pragma warning(error : 4789 ) // warning C4789: destination of memory copy is too small
#endif // _MSC_VER >= 1600 // VS 2010 and above.

#endif // #if defined( COMPILER_MSVC32 )

#endif // #ifndef MINBASE_WARNINGS_H
