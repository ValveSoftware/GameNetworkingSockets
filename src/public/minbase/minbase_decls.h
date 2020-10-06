//====== Copyright 1996-2012, Valve Corporation, All rights reserved. ======//
//
// Portable declaration decorations.
//
//==========================================================================//

#ifndef MINBASE_DECLS_H
#define MINBASE_DECLS_H
#pragma once

// Identification is needed everywhere but we don't want to include
// it over and over again so just make sure it was already included.
#ifndef MINBASE_IDENTIFY_H
#error Must include minbase_identify.h
#endif

// Use this to specify that a function is an override of a virtual function.
// This lets the compiler catch cases where you meant to override a virtual
// function but you accidentally changed the function signature and created
// an overloaded function. Usage in function declarations is like this:
// int GetData() const OVERRIDE;
#ifndef OVERRIDE
#define OVERRIDE override
#endif

// This can be used to ensure the size of pointers to members when declaring
// a pointer type for a class that has only been forward declared
#ifdef _MSC_VER
	#define SINGLE_INHERITANCE __single_inheritance
	#define MULTIPLE_INHERITANCE __multiple_inheritance
#else
	#define SINGLE_INHERITANCE
	#define MULTIPLE_INHERITANCE
#endif

#ifdef _MSC_VER
	#define NO_VTABLE __declspec( novtable )
#else
	#define NO_VTABLE
#endif

// This can be used to declare an abstract (interface only) class.
// Classes marked abstract should not be instantiated.  If they are, and access violation will occur.
//
// Example of use:
//
// abstract_class CFoo
// {
//      ...
// }
//
// MSDN __declspec(novtable) documentation: http://msdn.microsoft.com/library/default.asp?url=/library/en-us/vclang/html/_langref_novtable.asp
//
// Note: NJS: This is not enabled for regular PC, due to not knowing the implications of exporting a class with no no vtable.
//       It's probable that this shouldn't be an issue, but an experiment should be done to verify this.
//
#ifndef _XBOX
	#define abstract_class class
#else
	#define abstract_class class NO_VTABLE
#endif

// C functions for external declarations that call the appropriate C++ methods
#ifndef EXPORT
	#ifdef _WIN32
		#define EXPORT	_declspec( dllexport )
	#else
		#define EXPORT	/* */
	#endif
#endif

#if _MSC_VER >= 1400

#define NOALIAS __declspec(noalias)
#define RESTRICT				__restrict
#define RESTRICT_FUNC			__declspec(restrict)

#else	// _MSC_VER >= 1400

#define NOALIAS
#define RESTRICT
#define RESTRICT_FUNC

#endif	// _MSC_VER >= 1400

// Linux had a few areas where it didn't construct objects in the same order that Windows does.
// So when CVProfile::CVProfile() would access g_pMemAlloc, it would crash because the allocator wasn't initalized yet.
#if defined( GNUC ) || defined ( COMPILER_GCC ) || defined( COMPILER_SNC )
	#define CONSTRUCT_EARLY __attribute__((init_priority(101)))
#else
	#define CONSTRUCT_EARLY
#endif

#ifdef _WIN32
	#define SELECTANY __declspec(selectany)
#elif defined(GNUC) || defined ( COMPILER_GCC ) || defined( COMPILER_SNC )
	#define SELECTANY __attribute__((weak))
#else
	#define SELECTANY static
#endif

#undef DLL_EXPORT
#undef DLL_IMPORT

#if defined(_WIN32) && !defined(_XBOX)
#define PLAT_DECL_EXPORT __declspec( dllexport )
#define PLAT_DECL_IMPORT __declspec( dllimport )
#elif defined(GNUC) || defined(COMPILER_GCC)
#define PLAT_DECL_EXPORT __attribute__((visibility("default")))
#define PLAT_DECL_IMPORT
#elif defined(_XBOX) || defined(COMPILER_SNC)
#define PLAT_DECL_EXPORT
#define PLAT_DECL_IMPORT
#else
#error "Unsupported Platform."
#endif

#if defined(_XBOX)
#define DLL_EXPORT extern
#define DLL_IMPORT extern

#define DLL_CLASS_EXPORT
#define DLL_CLASS_IMPORT

#define DLL_GLOBAL_EXPORT
#define DLL_GLOBAL_IMPORT
#else
// Used for dll exporting and importing
#define DLL_EXPORT extern "C" PLAT_DECL_EXPORT
#define DLL_IMPORT extern "C" PLAT_DECL_IMPORT

// Can't use extern "C" when DLL exporting a class
#define DLL_CLASS_EXPORT PLAT_DECL_EXPORT
#define DLL_CLASS_IMPORT PLAT_DECL_IMPORT

#define DLL_GLOBAL_EXPORT PLAT_DECL_EXPORT
#define DLL_GLOBAL_IMPORT extern PLAT_DECL_IMPORT
#endif

#ifdef FASTCALL
#undef FASTCALL
#endif

// Used for standard calling conventions
#if defined(_WIN32)
	#define STDCALL				    __stdcall
	#define FASTCALL				__fastcall
#elif defined(_PS3)
	#if defined( COMPILER_SNC )
		#define  STDCALL
		#define  __stdcall
	#elif (CROSS_PLATFORM_VERSION >= 1) && !defined( PLATFORM_64BITS ) && !defined( COMPILER_PS3 )
		#define  STDCALL			__attribute__ ((__stdcall__))
	#else
		#define  STDCALL
		#define  __stdcall			__attribute__ ((__stdcall__))
	#endif
    #define FASTCALL
#elif defined(POSIX)
    #define __stdcall
    #define __cdecl
    #define STDCALL
    #define FASTCALL
#endif

#if defined(_WIN32)
	#define NOINLINE			    __declspec(noinline)
	#define NORETURN				__declspec(noreturn)
	#define FORCEINLINE			    __forceinline
#elif defined(GNUC) || defined(COMPILER_GCC) || defined(COMPILER_SNC)
	#define NOINLINE				__attribute__ ((noinline))
	#define NORETURN				__attribute__ ((noreturn))
    #if defined(COMPILER_GCC) || defined(COMPILER_SNC)
	    #define FORCEINLINE          inline __attribute__ ((always_inline))
    	// GCC 3.4.1 has a bug in supporting forced inline of templated functions
    	// this macro lets us not force inlining in that case
    	#define FORCEINLINE_TEMPLATE inline
    #else
        #define FORCEINLINE          inline
        #define FORCEINLINE_TEMPLATE inline
    #endif
#endif

// We have some template functions declared in header files that
// only need static scope. Older MSVC version lets you use 'static' qualifiers
// but GCC and new MSVC does not, where we need to use 'inline' to avoid multiple definitions.
#ifdef COMPILER_MSVC32
#if _MSC_VER <= 1910
#define STATIC_TEMPLATE_INLINE static
#else
#define STATIC_TEMPLATE_INLINE inline
#endif
#else
#define STATIC_TEMPLATE_INLINE inline
#endif

#if !defined(UNALIGNED)
#if defined(_M_IA64) || defined(_M_AMD64)
#define UNALIGNED __unaligned
#else
#define UNALIGNED
#endif
#endif

#endif // #ifndef MINBASE_DECLS_H
