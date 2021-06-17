#ifndef STEAM_API_COMMON
#define STEAM_API_COMMON
#pragma once

// When using the SteamWorks SDK, this header contains declarations
// for callbacks, etc.  In the OpenSource version, it's a stub with
// just a few things.

#include "steamtypes.h"
#include "steamclientpublic.h"

enum { k_iSteamNetworkingSocketsCallbacks = 1220 };
enum { k_iSteamNetworkingMessagesCallbacks = 1250 };
enum { k_iSteamNetworkingUtilsCallbacks = 1280 };

// handle to a communication pipe to the Steam client
typedef int32 HSteamPipe;
// handle to single instance of a steam user
typedef int32 HSteamUser;

#ifdef API_GEN
# define STEAM_CLANG_ATTR(ATTR) __attribute__((annotate( ATTR )))
#else
# define STEAM_CLANG_ATTR(ATTR)
#endif

#define STEAM_METHOD_DESC(DESC) STEAM_CLANG_ATTR( "desc:" #DESC ";" )
#define STEAM_IGNOREATTR() STEAM_CLANG_ATTR( "ignore" )
#define STEAM_OUT_STRUCT() STEAM_CLANG_ATTR( "out_struct: ;" )
#define STEAM_OUT_STRING() STEAM_CLANG_ATTR( "out_string: ;" )
#define STEAM_OUT_ARRAY_CALL(COUNTER,FUNCTION,PARAMS) STEAM_CLANG_ATTR( "out_array_call:" #COUNTER "," #FUNCTION "," #PARAMS ";" )
#define STEAM_OUT_ARRAY_COUNT(COUNTER, DESC) STEAM_CLANG_ATTR( "out_array_count:" #COUNTER  ";desc:" #DESC )
#define STEAM_ARRAY_COUNT(COUNTER) STEAM_CLANG_ATTR( "array_count:" #COUNTER ";" )
#define STEAM_ARRAY_COUNT_D(COUNTER, DESC) STEAM_CLANG_ATTR( "array_count:" #COUNTER ";desc:" #DESC )
#define STEAM_BUFFER_COUNT(COUNTER) STEAM_CLANG_ATTR( "buffer_count:" #COUNTER ";" )
#define STEAM_OUT_BUFFER_COUNT(COUNTER) STEAM_CLANG_ATTR( "out_buffer_count:" #COUNTER ";" )
#define STEAM_OUT_STRING_COUNT(COUNTER) STEAM_CLANG_ATTR( "out_string_count:" #COUNTER ";" )
#define STEAM_DESC(DESC) STEAM_CLANG_ATTR("desc:" #DESC ";")
#define STEAM_CALL_RESULT(RESULT_TYPE) STEAM_CLANG_ATTR("callresult:" #RESULT_TYPE ";")
#define STEAM_CALL_BACK(RESULT_TYPE) STEAM_CLANG_ATTR("callback:" #RESULT_TYPE ";")

#endif // STEAM_API_COMMON
