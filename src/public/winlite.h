//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: includes windows.h, with a minimal includes set, and clearing problematic defines
//
//=============================================================================//
#ifndef WINLITE_H
#define WINLITE_H
#pragma once
#ifdef _WIN32
// 
// Prevent tons of unused windows definitions
//
#undef ARRAYSIZE
#ifndef _WIN32_WINNT
#if !defined(_SERVER)
#define _WIN32_WINNT 0x0501 // XP
#else
#define _WIN32_WINNT 0x0600 // Vista / Server 2008
#endif

#ifndef _WIN32_IE
#	define _WIN32_IE 0x0500
#else
#	if ( _WIN32_IE < 0x0500 )
#		error _WIN32_IE must exceed 5.0
#	endif
#endif

#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define NOSERVICE
#define NOMCX
#define NOIME
#define NOSOUND
#define NOCOMM
#define NORPC
#define NOCRYPT
// if we include windows.h before winsock2, we get winsock1, and that breaks
// the parts of the code where we need to use winsock2 - so include winsock2
// first...
#include <winsock2.h>
#include <windows.h>
#include <dbghelp.h>
#undef PostMessage
#undef GetCurrentTime
#undef CreateEvent
#undef CreateMutex
#ifndef DONT_PROTECT_SENDMESSAGE
#undef SendMessage
#endif
#undef ShellExecute
#undef PropertySheet
#ifndef DONT_PROTECT_MESSAGEBOX
#undef MessageBox
#endif
#undef PlaySound
#undef CreateFont
#undef SetPort
#undef GetClassName
#undef Yield
#undef GetMessage
#undef DeleteFile
#undef StartService
#undef CopyFile
#undef MoveFile
#undef END_INTERFACE
#ifndef ALLOW_ARRAYSIZE // use this is you are writing code that doesn't consume tier0
#undef ARRAYSIZE
#define ARRAYSIZE Dont_Use_ARRAYSIZE_Use_V_ARRAYSIZE
#endif
#undef min
#undef max


#endif // _WIN32
#endif // WINLITE_H
