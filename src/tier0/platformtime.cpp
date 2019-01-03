//========= Copyright 1996-2013, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

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

uint64 Plat_RelativeTickFrequency()
{
	if ( g_TickBase == 0 )
		InitTicks();
	
	return g_TickFrequency;
}

uint64 Plat_TickDiffMilliSec( uint64 StartTicks, uint64 EndTicks )
{
	// Calculate in parts to avoid overflow and lack of precision when dividing
	uint64 ulTicks = EndTicks - StartTicks;

	if ( g_TickBase == 0 )
		InitTicks();
		
	uint64 ulSeconds = ulTicks / g_TickFrequency;
	uint64 ulRemainder = ulTicks % g_TickFrequency;

	return (ulSeconds * 1000) + (ulRemainder * 1000 / g_TickFrequency);
}

uint64 Plat_TickDiffMicroSec( uint64 StartTicks, uint64 EndTicks )
{
	// Calculate in parts to avoid overflow and lack of precision when dividing
	uint64 ulTicks = EndTicks - StartTicks;

	if ( g_TickBase == 0 )
		InitTicks();

	uint64 ulSeconds = ulTicks / g_TickFrequency;
	uint64 ulRemainder = ulTicks % g_TickFrequency;
	
	return (ulSeconds * 1000000) + (ulRemainder * 1000000 / g_TickFrequency);
}

uint64 Plat_TickAddMicroSec( uint64 StartTicks, int64 lMicroSec )
{
	if ( g_TickBase == 0 )
		InitTicks();

	return StartTicks + (int64)( lMicroSec * g_TickFrequencyDouble / 1000000.0 );
}

double Plat_FloatTime()
{
	// We subtract off the tick base to keep the diff as small
	// as possible so that our conversion math is more accurate.
	uint64 Ticks = Plat_RelativeTicks(); // NOTE: inits globals
	return ((double)(int64)(Ticks - g_TickBase)) / g_TickFrequencyDouble;
}

uint32 Plat_MSTime()
{
	// We subtract off the tick base to keep the diff as small
	// as possible so that our conversion math is more accurate.
	uint64 Ticks = Plat_RelativeTicks(); // NOTE: inits globals
	return (uint32)(((Ticks - g_TickBase) * 1000) / g_TickFrequency);
}

uint64 Plat_USTime()
{
	// We subtract off the tick base to keep the diff as small
	// as possible so that our conversion math is more accurate.
	uint64 Ticks = Plat_RelativeTicks(); // NOTE: inits globals
	return (uint64)( (Ticks - g_TickBase) * g_TicksToUS );
}

uint64 Plat_MSTime64()
{
	// We subtract off the tick base to keep the diff as small
	// as possible so that our conversion math is more accurate.
	uint64 Ticks = Plat_RelativeTicks(); // NOTE: inits globals
	return ((Ticks - g_TickBase) * 1000) / g_TickFrequency;
}


#ifdef WIN32
// Wraps the thread-safe versions of asctime. buf must be at least 26 bytes 
char *Plat_asctime( const struct tm *tm, char *buf, size_t bufsize )
{
	if ( EINVAL == asctime_s( buf, bufsize, tm ) )
		return NULL;
	else
		return buf;
}
#else
// Wraps the thread-safe versions of asctime. buf must be at least 26 bytes 
char *Plat_asctime( const struct tm *tm, char *buf, size_t bufsize )
{
	return asctime_r( tm, buf );
}
#endif


#ifdef WIN32
// Wraps the thread-safe versions of ctime. buf must be at least 26 bytes 
char *Plat_ctime( const time_t *timep, char *buf, size_t bufsize )
{
	if ( EINVAL == ctime_s( buf, bufsize, timep ) )
		return NULL;
	else
		return buf;
}
#else
// Wraps the thread-safe versions of ctime. buf must be at least 26 bytes 
char *Plat_ctime( const time_t *timep, char *buf, size_t bufsize )
{
	return ctime_r( timep, buf );
}
#endif

// timezone
int32 Plat_timezone( void )
{
#if _MSC_VER < 1900
	return timezone;
#else
	long timeZone = 0;
	_get_timezone( &timeZone );
	return timeZone;
#endif
}

// daylight savings
int32 Plat_daylight( void )
{
#if _MSC_VER < 1900
	return daylight;
#else
	int daylight = 0;
	_get_daylight( &daylight );
	return daylight;
#endif
}

#ifdef WIN32
// Wraps the thread-safe versions of gmtime
struct tm *Plat_gmtime( const time_t *timep, struct tm *result )
{
	if ( EINVAL == gmtime_s( result, timep ) )
		return NULL;
	else
		return result;
}
#else
// Wraps the thread-safe versions of gmtime
struct tm *Plat_gmtime( const time_t *timep, struct tm *result )
{
	return gmtime_r( timep, result );
}
#endif

#ifdef WIN32
time_t Plat_timegm( struct tm *timeptr )
{
	return _mkgmtime( timeptr );
}
#else
time_t Plat_timegm( struct tm *timeptr )
{
	return timegm( timeptr );
}
#endif


#ifdef WIN32
// Wraps the thread-safe versions of localtime
struct tm *Plat_localtime( const time_t *timep, struct tm *result )
{
	if ( EINVAL == localtime_s( result, timep ) )
		return NULL;
	else
		return result;
}
#else
// Wraps the thread-safe versions of localtime
struct tm *Plat_localtime( const time_t *timep, struct tm *result )
{
	return localtime_r( timep, result );
}
#endif

#ifdef WIN32
uint64 Plat_AbsoluteTime( void )
{
	FILETIME WinTime;

	GetSystemTimeAsFileTime( &WinTime );
	return (((uint64)WinTime.dwHighDateTime) << 32) | WinTime.dwLowDateTime;
}
#else
uint64 Plat_AbsoluteTime( void )
{
	struct timeval UnixTime;

	gettimeofday( &UnixTime, NULL );

	// Convert from seconds since 1/1/1970 to filetime (100 nanoseconds since 1/1/1601) with this magic formula courtesy of MSDN.
	return (uint64)UnixTime.tv_sec * 10000000 +
		(uint64)UnixTime.tv_usec * 10 +
		(uint64)116444736000000000;
}
#endif

double Plat_AbsoluteTimeToFloat( uint64 abstime )
{
	return abstime * 1.0e-7 - 11644473600;
}
