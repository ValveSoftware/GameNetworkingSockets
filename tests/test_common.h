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

#include <steam/steamnetworkingtypes.h>

struct SteamNetworkingIdentity;

extern ESteamNetworkingSocketsDebugOutputType g_eTestStdoutDetailLevel;

extern void TEST_InitLog( const char *pszFilename );
extern void TEST_SetStdoutDetailLevel( ESteamNetworkingSocketsDebugOutputType eDetailLevel );
extern void TEST_Printf( const char *fmt, ... );
extern void TEST_Fatal( const char *fmt, ... );
extern void TEST_Init( const SteamNetworkingIdentity *pIdentity );
extern void TEST_Kill();
extern void TEST_PumpCallbacks();

// ICE packet counters.  Defined in steamnetworkingsockets_ice_client.cpp.
// Reset at the start of each connection; printed after route selection.
namespace SteamNetworkingSocketsLib {
    extern int TEST_ICE_ctr_binding_req_send;
    extern int TEST_ICE_ctr_binding_req_recv;
    extern int TEST_ICE_ctr_binding_resp_send;
    extern int TEST_ICE_ctr_binding_resp_recv;
    extern int TEST_ICE_ctr_allocate_send;
    extern int TEST_ICE_ctr_refresh_send;
    extern int TEST_ICE_ctr_send_ind_send;
    extern int TEST_ICE_ctr_data_ind_recv;
    extern int TEST_ICE_ctr_binding_req_retx;
    extern int TEST_ICE_ctr_allocate_retx;
    extern int TEST_ICE_ctr_refresh_retx;
    extern int TEST_ICE_ctr_create_permission_retx;
    extern void TEST_ICE_ctr_Reset();
    extern void TEST_ICE_ctr_Print();
}
