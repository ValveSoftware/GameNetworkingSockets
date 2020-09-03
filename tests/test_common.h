// Misc stuff used in the tests
#pragma once

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
