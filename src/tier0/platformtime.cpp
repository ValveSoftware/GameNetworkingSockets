//========= Copyright Valve Corporation, All rights reserved. ============//
#include "../public/tier0/platformtime.h"

#ifdef WIN32
#include "winlite.h"
#include <time.h>
#include <errno.h>
#else

#include <sys/time.h>
#include <unistd.h>
#if defined OSX
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif
#if defined(LINUX) || defined(ANDROID)
#include <time.h>
#endif

#endif


#if defined(LINUX) || defined(ANDROID)
static const uint64 g_TickFrequency = 1000000000;
static const double g_TickFrequencyDouble = 1.0e9;
//static const double g_TicksToS = 1.0 / g_TickFrequencyDouble;
//static const double g_TicksToMS = 1.0e3 / g_TickFrequencyDouble;
static const double g_TicksToUS = 1.0e6 / g_TickFrequencyDouble;
#else
static uint64 g_TickFrequency;
static double g_TickFrequencyDouble;
//static double g_TicksToS;
//static double g_TicksToMS;
static double g_TicksToUS;
#endif

// NOTE:
// If Plat_RelativeTicks, Plat_MSTime, etc. are called by a
// global constructor in another file then g_TickBase may be
// already initialized before executing the following line!
// InitTicks returns existing value of g_TickBase, if set.
static uint64 InitTicks();
static uint64 g_TickBase = InitTicks();

#ifdef WIN32
static uint64 g_TickLastReturned_XPWorkaround;
#endif

static uint64 InitTicks()
{
	if ( g_TickBase != 0 )
		return g_TickBase;
	
#if defined(WIN32)
	LARGE_INTEGER Large;
	QueryPerformanceFrequency(&Large);
	g_TickFrequency = Large.QuadPart;
	g_TickFrequencyDouble = (double)g_TickFrequency;
	// Before Windows Vista, multicore system QPC can be non-monotonic 
	QueryPerformanceCounter( &Large );
	g_TickBase = g_TickLastReturned_XPWorkaround = Large.QuadPart;
#elif defined(OSX)
	mach_timebase_info_data_t TimebaseInfo;
	mach_timebase_info(&TimebaseInfo);
	g_TickFrequencyDouble = (double) TimebaseInfo.denom / (double) TimebaseInfo.numer * 1.0e9;
	g_TickFrequency = (uint64)( g_TickFrequencyDouble + 0.5 );
	g_TickBase = mach_absolute_time();
#elif defined(LINUX) || defined(ANDROID)
	// TickFrequency is constant since clock_gettime always returns nanoseconds
	timespec TimeSpec;
	clock_gettime( CLOCK_MONOTONIC, &TimeSpec );
	g_TickBase = (uint64)TimeSpec.tv_sec * 1000000000 + TimeSpec.tv_nsec;
#else
#error Unknown platform
#endif

	#if !defined(LINUX) && !defined(ANDROID)
		//g_TicksToS = 1.0 / g_TickFrequencyDouble;
		//g_TicksToMS = 1.0e3 / g_TickFrequencyDouble;
		g_TicksToUS = 1.0e6 / g_TickFrequencyDouble;
	#endif

	return g_TickBase;
}

uint64 Plat_RelativeTicks()
{
	if ( g_TickBase == 0 )
		InitTicks();
	
	uint64 Ticks;

#if defined(WIN32)
	LARGE_INTEGER Large;
	QueryPerformanceCounter(&Large);
	Ticks = Large.QuadPart;
	// On WinXP w/ multi-core CPU, ticks can go slightly backwards. Fixed in Vista+
	if ( Ticks < g_TickLastReturned_XPWorkaround )
	{
		Ticks = g_TickLastReturned_XPWorkaround;
	}
	else
	{
		g_TickLastReturned_XPWorkaround = Ticks;
	}
#elif defined(OSX)
	Ticks = mach_absolute_time();
#elif defined(LINUX) || defined(ANDROID)
	timespec TimeSpec;
	clock_gettime( CLOCK_MONOTONIC, &TimeSpec );
	Ticks = (uint64)TimeSpec.tv_sec * 1000000000 + TimeSpec.tv_nsec;
#else
#error Unknown platform
#endif

	return Ticks;
}

double Plat_FloatTime()
{
	// We subtract off the tick base to keep the diff as small
	// as possible so that our conversion math is more accurate.
	uint64 Ticks = Plat_RelativeTicks(); // NOTE: inits globals
	return ((double)(int64)(Ticks - g_TickBase)) / g_TickFrequencyDouble;
}

uint64 Plat_USTime()
{
	// We subtract off the tick base to keep the diff as small
	// as possible so that our conversion math is more accurate.
	uint64 Ticks = Plat_RelativeTicks(); // NOTE: inits globals
	return (uint64)( (Ticks - g_TickBase) * g_TicksToUS );
}
