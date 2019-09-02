//====== Copyright 1996-2012, Valve Corporation, All rights reserved. ======//
//
// Portable type definitions.
//
//==========================================================================//

#ifndef MINBASE_TYPES_H
#define MINBASE_TYPES_H
#pragma once

// Identification is needed everywhere but we don't want to include
// it over and over again so just make sure it was already included.
#ifndef MINBASE_IDENTIFY_H
#error Must include minbase_identify.h
#endif

typedef unsigned char    uint8;
typedef signed char      int8;

// for when we don't care about how many bits we use
typedef unsigned int uint;

typedef float  float32;
typedef double float64;

#if defined( _WIN32 ) && !defined(__GNUC__)

typedef __int16 int16;
typedef unsigned __int16 uint16;
typedef __int32 int32;
typedef unsigned __int32 uint32;
typedef __int64 int64;
typedef unsigned __int64 uint64;

typedef int socklen_t;

#else // _WIN32

typedef short int16;
typedef unsigned short uint16;
typedef int int32;
typedef unsigned int uint32;
// NOTE: We always use 'long long int' for 64-bit integers
// even though that is not always the system definition.
// For example, gcc 64-bit uses 'long int', which is a distinct
// type even though it's also a 64-bit integer.
// Always using 'long long int' makes life simpler for us because
// 'll' number suffixes and format string directives always match
// the number type, but be aware that our int64 may not always
// be the same type as the system int64_t.  There are typedefs
// below to handle that and ideally you should never have to think about it.
typedef long long int int64;
typedef unsigned long long int uint64;

#endif // else _WIN32

#ifdef PLATFORM_64BITS
typedef int64 intp;
typedef uint64 uintp;
#else
typedef int intp;
typedef uint uintp;
#endif

// We include code that uses the stdint.h types so define
// them also to make it easier to use such code without
// actually including stdint.h (which can get messy because of
// different MIN/MAX definitions).
typedef int8 int8_t;
typedef uint8 uint8_t;
typedef int16 int16_t;
typedef uint16 uint16_t;
typedef int32 int32_t;
typedef uint32 uint32_t;
// NOTE: int64_t must match the compiler stdint.h definition
// and so may not match the Steam int64.  Mixing the two is
// error-prone so always use the Steam non-_t types in Steam code.
#if defined(COMPILER_GCC) && defined(PLATFORM_64BITS) && !defined(__MINGW32__) && !defined(OSX) && !(defined(IOS) || defined(TVOS))
#define INT64_DIFFERENT_FROM_INT64_T 1
typedef long int int64_t;
typedef unsigned long int uint64_t;
#else
typedef int64 int64_t;
typedef uint64 uint64_t;
#endif

#endif // #ifndef MINBASE_TYPES_H
