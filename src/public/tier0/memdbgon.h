//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//


// SDR_PUBLIC

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

#ifndef real_free
	#define real_free( pMem ) free( pMem )
#endif

