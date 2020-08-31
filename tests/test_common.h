// Misc stuff used in the tests
#pragma once

struct SteamNetworkingIdentity;

extern void TEST_Init();
extern void TEST_Printf( const char *fmt, ... );
extern void TEST_Init( const SteamNetworkingIdentity *pIdentity );
extern void TEST_Kill();
extern void TEST_PumpCallbacks();
