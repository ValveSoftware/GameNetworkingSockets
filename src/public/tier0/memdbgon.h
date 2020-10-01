//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//

// This file should be included before any code that calls malloc, free, and realloc,
// so that those calls will be redirected to the custom allocator, if
// STEAMNETWORKINGSOCKETS_ENABLE_MEM_OVERRIDE is defined.

#if defined(STEAMNETWORKINGSOCKETS_ENABLE_MEM_OVERRIDE) && !defined(MEM_OVERRIDE_ON)
	#define MEM_OVERRIDE_ON

	#define malloc( s ) SteamNetworkingSockets_Malloc( s )
	#define realloc( p, s ) SteamNetworkingSockets_Realloc( p, s )
	#define free( p ) SteamNetworkingSockets_Free( p )

	#define calloc DO_NOT_USE_CALLOC
	#define strdup DO_NOT_USE_STRDUP
#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_MEM_OVERRIDE

// One-time declarations
#ifndef MEMDBG_ON_INCLUDED
	#define MEMDBG_ON_INCLUDED

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_MEM_OVERRIDE
		extern void *SteamNetworkingSockets_Malloc( size_t s );
		extern void *SteamNetworkingSockets_Realloc( void *p, size_t s );
		extern void SteamNetworkingSockets_Free( void *p );
	#endif
#endif

// Misc Steam codebase compatibility stuff.  Not used in the standalone library
#ifndef PvAlloc
	#define PvAlloc( cub )  malloc( cub )
#endif
#ifndef PvRealloc
	#define PvRealloc( pv, cub ) realloc( pv, cub )
#endif
#ifndef FreePv
	#define FreePv( pv ) free( pv )
#endif
#ifndef MEM_ALLOC_CREDIT_CLASS
	#define MEM_ALLOC_CREDIT_CLASS()
#endif

