//========= Copyright 1996-2013, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef PLATFORMTIME_H
#define PLATFORMTIME_H
#pragma once

#include "minbase/minbase_identify.h"
#include "minbase/minbase_types.h"
#include "minbase/minbase_decls.h"
#include "time.h"

#ifdef STATIC_TIER0
#define STATIC_PLATFORMTIME
#endif

#ifndef STATIC_PLATFORMTIME

#ifdef TIER0_DLL_EXPORT
#define PLATFORMTIME_INTERFACE	DLL_EXPORT
#else
#define PLATFORMTIME_INTERFACE	DLL_IMPORT
#endif

#else
// STATIC BUILD
#define PLATFORMTIME_INTERFACE	extern "C"
#endif

PLATFORMTIME_INTERFACE uint64			Plat_RelativeTicks();	// Returns time in raw ticks since an arbitrary start point.
PLATFORMTIME_INTERFACE uint64			Plat_RelativeTickFrequency();	// Frequency of raw ticks.
// WARNING: should only be used for small deltas (ideally less than a minute) to
// avoid overflows in math.	 Milliseconds have 1,000 more tolerance but are not immune.
PLATFORMTIME_INTERFACE uint64			Plat_TickDiffMilliSec( uint64 StartTicks, uint64 EndTicks );
PLATFORMTIME_INTERFACE uint64			Plat_TickDiffMicroSec( uint64 StartTicks, uint64 EndTicks );
PLATFORMTIME_INTERFACE uint64			Plat_TickAddMicroSec( uint64 StartTicks, int64 lMicroSec );

PLATFORMTIME_INTERFACE double			Plat_FloatTime();		// Relative ticks to seconds (double).
PLATFORMTIME_INTERFACE uint32			Plat_MSTime();			// Relative ticks to milliseconds (32-bit).
PLATFORMTIME_INTERFACE uint64			Plat_MSTime64();		// Relative ticks to milliseconds (64-bit).
PLATFORMTIME_INTERFACE uint64			Plat_USTime();			// Relative ticks to microseconds

// Returns a Windows-style absolute time since 1600 in 100ns units.
PLATFORMTIME_INTERFACE uint64			Plat_AbsoluteTime( void );

// Convert a Windows-style absolute time to UNIX epoch time with fractional seconds
PLATFORMTIME_INTERFACE double			Plat_AbsoluteTimeToFloat( uint64 );

PLATFORMTIME_INTERFACE char *			Plat_asctime( const struct tm *tm, char *buf, size_t bufsize );
PLATFORMTIME_INTERFACE char *			Plat_ctime( const time_t *timep, char *buf, size_t bufsize );
PLATFORMTIME_INTERFACE struct tm *		Plat_gmtime( const time_t *timep, struct tm *result );
PLATFORMTIME_INTERFACE time_t			Plat_timegm( struct tm *timeptr );
PLATFORMTIME_INTERFACE struct tm *		Plat_localtime( const time_t *timep, struct tm *result );
PLATFORMTIME_INTERFACE int32			Plat_timezone( void );
PLATFORMTIME_INTERFACE int32			Plat_daylight( void );

#endif /* PLATFORM_H */
