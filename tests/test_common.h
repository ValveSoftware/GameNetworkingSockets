// Misc stuff used in the tests
#pragma once

// Include a bunch of common headers, especially the ones that will configure
// Visual Studio memory allocation and check the _DEBUG flag.  We are
// about to slam that flag to force assert to be enabled below.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <string>

// Force asserts to be enabled, even in release build
#undef NDEBUG
#ifndef _DEBUG
	#define _DEBUG
#endif
#include <assert.h>

struct SteamNetworkingIdentity;

extern void TEST_Init();
extern void TEST_Printf( const char *fmt, ... );
extern void TEST_Fatal( const char *fmt, ... );
extern void TEST_Init( const SteamNetworkingIdentity *pIdentity );
extern void TEST_Kill();
extern void TEST_PumpCallbacks();
