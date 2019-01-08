//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKES_CONFIG
#define STEAMNETWORKINGSOCKES_CONFIG

#define STEAMNETWORKINGSOCKETS_OPENSOURCE

#ifndef PLAT_EXTERN_C
	#ifdef __cplusplus
		#define PLAT_EXTERN_C extern "C"
	#else
		#define PLAT_EXTERN_C extern
	#endif
#endif

#if defined( STEAMDATAGRAMLIB_STATIC_LINK )
	#define STEAMNETWORKINGSOCKETS_INTERFACE PLAT_EXTERN_C
#else
	#if defined _WIN32 || defined __CYGWIN__
		#ifdef STEAMDATAGRAMLIB_FOREXPORT
			#ifdef __GNUC__
				#define STEAMNETWORKINGSOCKETS_INTERFACE PLAT_EXTERN_C __attribute__ ((dllexport))
			#else
				#define STEAMNETWORKINGSOCKETS_INTERFACE PLAT_EXTERN_C __declspec(dllexport) // Note: actually gcc seems to also supports this syntax.
			#endif
		#else
			#ifdef __GNUC__
				#define STEAMNETWORKINGSOCKETS_INTERFACE PLAT_EXTERN_C __attribute__ ((dllimport))
			#else
				#define STEAMNETWORKINGSOCKETS_INTERFACE PLAT_EXTERN_C __declspec(dllimport) // Note: actually gcc seems to also supports this syntax.
			#endif
		#endif
	#else
		#if __GNUC__ >= 4
			#define STEAMNETWORKINGSOCKETS_INTERFACE PLAT_EXTERN_C __attribute__ ((visibility ("default")))
		#else
			#define STEAMNETWORKINGSOCKETS_INTERFACE PLAT_EXTERN_C
		#endif
	#endif
#endif

// Callback identifiers.  These aren't actually relevant with the callback
// dispatch mechanism we are using, but they are in Steam, so let's define them
// here to keep the code the same.
enum { k_iSteamNetworkingSocketsCallbacks = 1220 };
enum { k_iSteamNetworkingMessagesCallbacks = 1250 };

#endif // #ifndef STEAMNETWORKINGSOCKETS_CONFIG
