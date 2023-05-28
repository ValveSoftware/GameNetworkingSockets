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

#ifdef IsPosix
	#error "Running platform detection twice, or defining IsPosix too soon"
#endif

#ifdef _DEBUG
	#define IsRelease() false
	#define IsDebug() true
#else
	#define IsRelease() true
	#define IsDebug() false
#endif

//
// Detect platform and set appropriate IsXxx() macros to true
// We will set all the others to false below.
//
#if defined( _XBOX_ONE ) || defined( _GAMING_XBOX_XBOXONE )
	#define IsXboxOne() true
	#define IsConsole() true
#elif defined( _GAMING_XBOX_SCARLETT )
	#define IsXboxScarlett() true
	#define IsConsole() true
#elif defined( NN_NINTENDO_SDK )
	// NOTE: _WIN32 != "Windows".  It means we have a WIN32-like set of APIs to access locks, files, etc.
	#ifndef _WIN32
		#define IsPosix() true
	#endif
	#define IsNintendoSwitch() true
	#define IsConsole() true
#elif defined( __PROSPERO__ )
	#define IsPosix() true
	#define IsPS5() true
	#define IsConsole() true
#elif defined( __ORBIS__ )
	#define IsPosix() true
	#define IsPS4() true
	#define IsConsole() true
#elif defined( _WIN32 )
	#define IsWindows() true
#elif defined( __ANDROID__ ) || defined( ANDROID )
	#define IsAndroid() true
	#define IsPosix() true
#elif defined(__APPLE__)
	#include <TargetConditionals.h>
	// https://stackoverflow.com/questions/12132933/preprocessor-macro-for-os-x-targets
	#if TARGET_OS_TV
		#define IsTVOS() true
		#define IsPosix() true
	#elif TARGET_OS_IOS
		#define IsIOS() true
		#define IsPosix() true
	#else
		// Assume OSX
		#define SUPPORTS_IOPOLLINGHELPER
		#define IsOSX() true
		#define IsPosix() true
	#endif
#elif defined( LINUX ) || defined( __LINUX__ ) || defined(linux) || defined(__linux) || defined(__linux__)
	#define IsLinux() true
	#define IsPosix() true
#elif defined( _POSIX_VERSION ) || defined( POSIX ) || defined( VALVE_POSIX )
	#define IsPosix() true
#else
	#error Undefined platform
#endif

//
// Now define as false any of the IsXxx functions not set true above
//
#ifndef IsWindows
	#define IsWindows() false
#endif
#ifndef IsAndroid
	#define IsAndroid() false
#endif
#ifndef IsConsole
	#define IsConsole() false
#endif
#ifndef IsNintendoSwitch
	#define IsNintendoSwitch() false
#endif
#define IsX360() false
#ifndef IsXboxOne
	#define IsXboxOne() false
#endif
#ifndef IsXboxScarlett
	#define IsXboxScarlett() false
#endif
#define IsXbox() ( IsXboxOne() || IsXboxScarlett() )
#if defined( _GAMING_XBOX ) && !IsXbox()
	#error "_GAMING_XBOX_XBOXONE or _GAMING_XBOX_SCARLETT should be defined"
#endif
#define IsPS3() false
#ifndef IsPS4
	#define IsPS4() false
#endif
#ifndef IsPS5
	#define IsPS5() false
#endif
#define IsPlaystation() ( IsPS4() || IsPS5() )
#ifndef IsIOS
	#define IsIOS() false
	// Make sure we didn't get hosed by order of checks above
	#if defined(IOS) || defined(__IOS__)
		#error "IOS detection not working"
	#endif
#endif
#ifndef IsTVOS
	#define IsTVOS() false
	// Make sure we didn't get hosed by order of checks above
	#if defined(TVOS) || defined(__TVOS__)
		#error "TVOS detection not working"
	#endif
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

// Detect ARM
#ifndef IsARM
	#if defined( __arm__ ) || defined( _M_ARM ) || defined( _M_ARM64 ) || defined( PLATFORM_ARM )
		#define IsARM() true
	#else
		#define IsARM() false
	#endif
#endif

// Detect if RTTI is enabled in the current compile
#if defined(__clang__)
  #if __has_feature(cxx_rtti)
    #define RTTIEnabled() true
  #endif
#elif defined(__GNUC__)
  #if defined(__GXX_RTTI)
    #define RTTIEnabled() true
  #endif
#elif defined(_MSC_VER)
  #if defined(_CPPRTTI)
    #define RTTIEnabled() true
  #endif
#else
  #error "How to tell if RTTI is enabled?")
#endif
#ifndef RTTIEnabled
	#define RTTIEnabled() false
#endif

// SDR_PUBLIC wrap tier0 and tier1 symbols in a namespace to limit
// the possibility of clashing when statically linking.  (Especially
// with games that use the source engine!)
#define BEGIN_TIER0_NAMESPACE namespace SteamNetworkingSocketsTier0 {
#define END_TIER0_NAMESPACE } using namespace SteamNetworkingSocketsTier0;
#define BEGIN_TIER1_NAMESPACE namespace SteamNetworkingSocketsTier1 {
#define END_TIER1_NAMESPACE } using namespace SteamNetworkingSocketsTier1;

#endif // #ifndef MINBASE_IDENTIFY_H
