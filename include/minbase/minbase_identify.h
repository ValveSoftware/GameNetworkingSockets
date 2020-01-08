//====== Copyright 1996-2012, Valve Corporation, All rights reserved. ======//
//
// Compiler/platform identification to provide a consistent set of
// macros for places that need to specialize behavior.
//
//==========================================================================//

#ifndef MINBASE_IDENTIFY_H
#define MINBASE_IDENTIFY_H
#pragma once

#if defined(_LP64) || defined(__x86_64__) || defined(__arm64__) || defined(__aarch64__) || defined(_WIN64)
	#define X64BITS
	#define PLATFORM_64BITS
#endif // __x86_64__

#ifdef SN_TARGET_PS3

	#define _PS3 1
	#define COMPILER_PS3 1
	#define PLATFORM_PS3 1
	#define PLATFORM_PPC 1

	// There are 2 compilers for the PS3: GCC and the SN Systems compiler.
	// They are mostly similar, but in a few places we need to distinguish between the two.
	#if defined( __SNC__ )
		#define COMPILER_SNC 1
	#elif defined( __GCC__ )
		#define COMPILER_GCC 1
	#else
		#error "Unrecognized PS3 compiler; either __SNC__ or __GCC__ must be defined"
	#endif

#endif // SN_TARGET_PS3 

#if !defined(COMPILER_GCC) && (defined(__GCC__) || defined(__GNUC__))
	#define COMPILER_GCC 1
#endif

#if !defined(COMPILER_CLANG) && defined(__clang__)
    #define COMPILER_CLANG 1
#endif

#ifdef _MSC_VER
	#define COMPILER_MSVC32 1
	#if defined( _M_X64 )
		#define COMPILER_MSVC64 1
	#endif
#endif

#if ( defined(LINUX) || defined(OSX) || defined(ANDROID) ) && !defined(POSIX)
	#define POSIX
#endif

#if defined(_WIN32) && !defined(WINDED)
	#if defined(_M_IX86)
		#define __i386__	1
	#elif defined(_M_X64)
		#define __x86_64__	1
	#endif
#endif

#if ( (defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || defined(__LITTLE_ENDIAN__) || defined(__i386__) || defined( __x86_64__ ) || defined(__arm__) || defined(__arm64__) || defined(__aarch64__) || defined(_XBOX) ) && !defined(VALVE_LITTLE_ENDIAN)
#define VALVE_LITTLE_ENDIAN 1
#endif

#if ( (defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || defined(__BIG_ENDIAN__) || defined( _SGI_SOURCE ) || defined( _PS3 ) ) && !defined(VALVE_BIG_ENDIAN)
#define VALVE_BIG_ENDIAN 1
#endif

#if defined( VALVE_LITTLE_ENDIAN ) == defined( VALVE_BIG_ENDIAN )
	#error "Cannot determine endianness of platform!"
#endif



// Detect C++11 support for "rvalue references" / "move semantics" / other C++11 (and up) stuff
#if defined(_MSC_VER)
	#if _MSC_VER >= 1600
		#define VALVE_RVALUE_REFS 1
	#endif
	#if _MSC_VER >= 1800
		#define VALVE_INITIALIZER_LIST_SUPPORT 1
		#define VALVE_EXPLICIT_CONVERSION_OP 1
	#endif
#elif defined(__clang__)
	#if __has_extension(cxx_rvalue_references)
		#define VALVE_RVALUE_REFS 1
	#endif
	#if __has_feature(cxx_generalized_initializers)
		#define VALVE_INITIALIZER_LIST_SUPPORT 1
	#endif
	#if __has_feature(cxx_explicit_conversions)
		#define VALVE_EXPLICIT_CONVERSION_OP 1
	#endif
#elif defined(__GNUC__)
	#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6 )
		#if defined(__GXX_EXPERIMENTAL_CXX0X__)
			#define VALVE_RVALUE_REFS 1
			#define VALVE_INITIALIZER_LIST_SUPPORT 1
			#define VALVE_EXPLICIT_CONVERSION_OP 1
		#endif
	#endif
#endif


#ifdef _RETAIL
	#define IsRetail() true 
#else
	#define IsRetail() false
#endif

#ifdef _DEBUG
	#define IsRelease() false
	#define IsDebug() true
#else
	#define IsRelease() true
	#define IsDebug() false
#endif

#if defined( _XBOX_ONE )
	#define IsXboxOne() true
	#define IsConsole() true
#elif defined( NN_NINTENDO_SDK )
	#if !defined(POSIX) && !defined(_WIN32)
		#define POSIX
	#endif
	#define IsNintendoSwitch() true
	#define IsConsole() true
#elif defined( _WIN32 )
	#define IsWindows() true
	#define IsPC() true
#elif defined( _PS3 )
	#define IsConsole() true
	#define IsPosix() true
	#define IsPS3() true
#elif defined(POSIX)
	#define IsPC() true
	#define IsPosix() true
	#ifdef LINUX
		#define IsLinux() true
	#endif
	#ifdef OSX
		#define SUPPORTS_IOPOLLINGHELPER
		#define IsOSX() true
	#endif
#else
	#error Undefined platform
#endif

#ifndef IsWindows
	#define IsWindows() false
#endif
#ifndef IsPC
	#define IsPC() false
#endif
#ifndef IsConsole
	#define IsConsole() false
#endif
#ifndef IsNintendoSwitch
	#define IsNintendoSwitch() false
#endif
#ifndef IsXboxOne
	#define IsXboxOne() false
#endif
#ifndef IsLinux
	#define IsLinux() false
#endif
#ifndef IsPosix
	#define IsPosix() false
#endif
#ifndef IsOSX
	#define IsOSX() false
#endif
#ifndef IsPS3
	#define IsPS3() false
#endif
#ifndef IsX360
	#define IsX360() false
#endif
#ifndef IsARM
	#ifdef __arm__
		#define IsARM() true
	#else
		#define IsARM() false
	#endif
#endif
#ifndef IsAndroid
	#ifdef ANDROID
		#define IsAndroid() true
	#else
		#define IsAndroid() false
	#endif
#endif


#endif // #ifndef MINBASE_IDENTIFY_H
