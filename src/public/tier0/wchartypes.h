//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose:	All of our code is completely Unicode.  Instead of char, you should
//			use char, uint8, or char8, as explained below.
//
// $NoKeywords: $
//=============================================================================//


#ifndef WCHARTYPES_H
#define WCHARTYPES_H
#pragma once

#ifdef _INC_TCHAR
#error ("Must include tier0 type headers before tchar.h")
#endif

// Temporarily turn off Valve defines
#include "tier0/valve_off.h"

// char8
// char8 is equivalent to char, and should be used when you really need a char
// (for example, when calling an external function that's declared to take
// chars).
typedef char char8;

// uint8
// uint8 is equivalent to byte (but is preferred over byte for clarity).  Use this
// whenever you mean a byte (for example, one byte of a network packet).
typedef unsigned char uint8;
typedef unsigned char BYTE;
typedef unsigned char byte;

#ifdef WIN32
#include <tchar.h>
#include <wchar.h>
#elif defined( _PS3 )
#define _tcsstr strstr
#define _tcsicmp stricmp
#define _tcscmp strcmp
#define _tcscpy strcpy
#define _tcsncpy strncpy
#define _tcsrchr strrchr
#define _tcslen strlen
#define _tfopen fopen
#define _stprintf sprintf 
#define _ftprintf fprintf
#define _vsntprintf vsnprintf
#define _tprintf printf
#define _sntprintf _snprintf
#define _T(s) s
#else
#include <wchar.h>
#include <wctype.h>
#define _tcsstr strstr
#define _tcsicmp stricmp
#define _tcscmp strcmp
#define _tcscpy strcpy
#define _tcsncpy strncpy
#define _tcsrchr strrchr
#define _tcslen strlen
#define _tfopen fopen
#define _stprintf sprintf 
#define _ftprintf fprintf
#define _vsntprintf vsnprintf
#define _tprintf printf
#define _sntprintf snprintf
#define _T(s) s
#endif

#if defined(_UNICODE)
typedef wchar_t tchar;
#define tstring wstring
#define __TFILE__ __WFILE__
#define TCHAR_IS_WCHAR
#else
typedef char tchar;
#define tstring string
#define __TFILE__ __FILE__
#define TCHAR_IS_CHAR
#endif

#ifdef FORCED_UNICODE
#undef _UNICODE
#endif


#if defined( _MSC_VER ) || defined( WIN32 )
typedef wchar_t	uchar16;
typedef unsigned int uchar32;
#else
typedef unsigned short uchar16;
typedef wchar_t uchar32;
#endif

// Turn valve defines back on
#include "tier0/valve_on.h"


#endif // WCHARTYPES


