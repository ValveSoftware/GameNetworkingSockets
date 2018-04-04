//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKES_CONFIG
#define STEAMNETWORKINGSOCKES_CONFIG

#define STEAMNETWORKINGSOCKETS_OPENSOURCE

#if defined( STEAMDATAGRAMLIB_STATIC_LINK )
	#define STEAMNETWORKINGSOCKETS_INTERFACE extern
#else
	#if defined _WIN32 || defined __CYGWIN__
		#ifdef STEAMDATAGRAMLIB_FOREXPORT
			#ifdef __GNUC__
				#define STEAMNETWORKINGSOCKETS_INTERFACE __attribute__ ((dllexport))
			#else
				#define STEAMNETWORKINGSOCKETS_INTERFACE __declspec(dllexport) // Note: actually gcc seems to also supports this syntax.
			#endif
		#else
			#ifdef __GNUC__
				#define STEAMNETWORKINGSOCKETS_INTERFACE __attribute__ ((dllimport))
			#else
				#define STEAMNETWORKINGSOCKETS_INTERFACE __declspec(dllimport) // Note: actually gcc seems to also supports this syntax.
			#endif
		#endif
	#else
		#if __GNUC__ >= 4
			#define STEAMNETWORKINGSOCKETS_INTERFACE __attribute__ ((visibility ("default")))
		#else
			#define STEAMNETWORKINGSOCKETS_INTERFACE
		#endif
	#endif
#endif

#endif // #ifndef STEAMNETWORKINGSOCKETS_CONFIG
