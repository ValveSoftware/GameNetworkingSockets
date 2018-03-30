//====== Copyright 1996-2012, Valve Corporation, All rights reserved. ======//
//
// Limit values for portable types.
//
//==========================================================================//

#ifndef MINBASE_LIMITS_H
#define MINBASE_LIMITS_H
#pragma once

// Identification is needed everywhere but we don't want to include
// it over and over again so just make sure it was already included.
#ifndef MINBASE_IDENTIFY_H
#error Must include minbase_identify.h
#endif

// Avoid collisions with other people including the regular CRT limits.h.
#ifdef _MSC_VER
#include <limits.h>
#if _MSC_VER >= 1800 // VS 2013
// Avoid collisions with other people including the regular CRT stdint.h.
#include <stdint.h>
#endif
#endif

#undef INT8_MAX
#undef INT8_MIN
#define INT8_MAX ((int8)0x7f)
#define INT8_MIN ((int8)0x80)

#undef UINT8_MAX
#define UINT8_MAX ((uint8)0xff)

#undef INT16_MAX
#undef INT16_MIN
#define INT16_MAX ((int16)0x7fff)
#define INT16_MIN ((int16)0x8000)

#undef UINT16_MAX
#define UINT16_MAX ((uint16)0xffff)

#undef INT32_MAX
#undef INT32_MIN
#define INT32_MAX ((int32)0x7fffffff)
#define INT32_MIN ((int32)0x80000000)

#undef UINT32_MAX
#define UINT32_MAX ((uint32)0xffffffff)

#undef INT64_MAX
#undef INT64_MIN
#define INT64_MAX ((int64)0x7fffffffffffffff)
#define INT64_MIN ((int64)0x8000000000000000)

#undef UINT64_MAX
#define UINT64_MAX ((uint64)0xffffffffffffffff)

#undef SIZE_MAX
#if defined(PLATFORM_64BITS)
#define SIZE_MAX ((size_t)UINT64_MAX)
#else
#define SIZE_MAX ((size_t)UINT32_MAX)
#endif

#undef UINT_MAX
#define UINT_MAX ((uint)-1)

#endif // #ifndef MINBASE_LIMITS_H
