//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#include <tier0/platform.h>

#ifdef _WIN64
#include "intrin.h"
#endif

#ifdef _WIN32
#include "excpt.h"
#endif

//#include "pch_tier0.h"
//
//#include "tier0/fasttimer.h"
//
//#if defined(_WIN32) && !defined(_XBOX)
//#include "winlite.h"
//#include "CpuTopology.h"
//#endif
//
//#ifdef _XBOX
//#include "xbox/xbox_platform.h"
//#include "xbox/xbox_win32stubs.h"
//#endif
//
//#if defined OSX
//#include <sys/sysctl.h>
//#endif
//
//const char* GetProcessorVendorId();

#if !defined( _PS3 )
static bool cpuid(uint32 function, uint32& out_eax, uint32& out_ebx, uint32& out_ecx, uint32& out_edx)
{
#if defined(GNUC)
#if defined(PLATFORM_64BITS)
	asm("mov %%rbx, %%rsi\n\t"
		"cpuid\n\t"
		"xchg %%rsi, %%rbx"
		: "=a" (out_eax),
		  "=S" (out_ebx),
		  "=c" (out_ecx),
		  "=d" (out_edx)
		: "a" (function) 
	);
#else
	asm("mov %%ebx, %%esi\n\t"
		"cpuid\n\t"
		"xchg %%esi, %%ebx"
		: "=a" (out_eax),
		  "=S" (out_ebx),
		  "=c" (out_ecx),
		  "=d" (out_edx)
		: "a" (function) 
	);
#endif
	return true;
#elif defined(_WIN64)
	int out[4];
	__cpuid( out, (int)function );
	out_eax = out[0];
	out_ebx = out[1];
	out_ecx = out[2];
	out_edx = out[3];
	return true;
#else
	bool retval = true;
	uint32 local_eax, local_ebx, local_ecx, local_edx;
	_asm pushad;

	__try
	{
        _asm
		{
			xor edx, edx		// Clue the compiler that EDX is about to be used.
            mov eax, function   // set up CPUID to return processor version and features
								//      0 = vendor string, 1 = version info, 2 = cache info
            cpuid				// code bytes = 0fh,  0a2h
            mov local_eax, eax	// features returned in eax
            mov local_ebx, ebx	// features returned in ebx
            mov local_ecx, ecx	// features returned in ecx
            mov local_edx, edx	// features returned in edx
		}
    } 
	__except(EXCEPTION_EXECUTE_HANDLER) 
	{ 
		retval = false; 
	}

	out_eax = local_eax;
	out_ebx = local_ebx;
	out_ecx = local_ecx;
	out_edx = local_edx;

	_asm popad

	return retval;
#endif
}
#endif // _PS3

// Return the Processor's vendor identification string, or "Generic_x86" if it doesn't exist on this CPU
const char* GetProcessorVendorId()
{
#if defined( _X360 ) || defined( _PS3 )
	return "PPC";
#else
	uint32 unused, VendorIDRegisters[3];

	static char VendorID[13];
	
	memset( VendorID, 0, sizeof(VendorID) );
	if( !cpuid(0,unused, VendorIDRegisters[0], VendorIDRegisters[2], VendorIDRegisters[1] ) )
	{
		_tcscpy( VendorID, _T("Generic_x86") ); 
	}
	else
	{
		memcpy( VendorID+0, &(VendorIDRegisters[0]), 4 );
		memcpy( VendorID+4, &(VendorIDRegisters[1]), 4 );
		memcpy( VendorID+8, &(VendorIDRegisters[2]), 4 );
	}

	return VendorID;
#endif
}
bool CheckMMXTechnology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false; 
#elif defined( _M_X64 ) || defined( __x86_64__ )
	return false;
#else
    uint32 eax,ebx,edx,unused;
    if( !cpuid(1,eax,ebx,unused,edx) )
		return false;

    return ( edx & 0x800000 ) != 0;
#endif
}

bool CheckSSETechnology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false; 
#elif defined( _M_X64 ) || defined (__x86_64__)
	return true;
#else
    uint32 eax,ebx,edx,unused;
    if( !cpuid(1,eax,ebx,unused,edx) )
	{
		return false;
	}

    return ( edx & 0x2000000L ) != 0;
#endif
}

bool CheckSSE2Technology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#elif defined( _M_X64 ) || defined (__x86_64__)
	return true;
#else

    uint32 eax,ebx,edx,unused;
    if( !cpuid(1,eax,ebx,unused,edx) )
		return false;

    return ( edx & 0x04000000 ) != 0;
#endif
}

bool CheckSSE3Technology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#else

	uint32 eax,ebx,edx,ecx;
	if( !cpuid(1,eax,ebx,ecx,edx) )
		return false;

	return ( ecx & 0x00000001 ) != 0;	// bit 1 of ECX
#endif
}

bool CheckSSSE3Technology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#else

	// SSSE 3 is implemented by both Intel and AMD
	// detection is done the same way for both vendors
	uint32 eax,ebx,edx,ecx;
	if( !cpuid(1,eax,ebx,ecx,edx) )
		return false;

	return ( ecx & ( 1 << 9 ) ) != 0;	// bit 9 of ECX
#endif
}

bool CheckSSE41Technology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#else

	// SSE 4.1 is implemented by both Intel and AMD
	// detection is done the same way for both vendors

	uint32 eax,ebx,edx,ecx;
	if( !cpuid(1,eax,ebx,ecx,edx) )
		return false;

	return ( ecx & ( 1 << 19 ) ) != 0;	// bit 19 of ECX
#endif
}

bool CheckSSE42Technology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#else

	// SSE4.2 is implemented by both Intel and AMD
	// detection is done the same way for both vendors

	uint32 eax,ebx,edx,ecx;
	if( !cpuid(1,eax,ebx,ecx,edx) )
		return false;

	return ( ecx & ( 1 << 20 ) ) != 0;	// bit 20 of ECX
#endif
}


bool CheckSSE4aTechnology( void )
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#else

	// SSE 4a is an AMD-only feature

	const char *pchVendor = GetProcessorVendorId();
	if ( 0 != _stricmp( pchVendor, "AuthenticAMD" ) )
		return false;

	uint32 eax,ebx,edx,ecx;
	if( !cpuid( 0x80000001,eax,ebx,ecx,edx) )
		return false;

	return ( ecx & ( 1 << 6 ) ) != 0;	// bit 6 of ECX
#endif
}


bool Check3DNowTechnology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#elif defined( _M_X64 ) || defined( __x86_64__ )
	return false;
#else

    uint32 eax, unused;
    if( !cpuid(0x80000000,eax,unused,unused,unused) )
		return false;

    if ( eax > 0x80000000L )
    {
     	if( !cpuid(0x80000001,unused,unused,unused,eax) )
			return false;

		return ( eax & 1<<31 ) != 0;
    }
    return false;
#endif
}

bool CheckCMOVTechnology()
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#elif defined( _M_X64 ) || defined (__x86_64__)
	return true;
#else

    uint32 eax,ebx,edx,unused;
    if( !cpuid(1,eax,ebx,unused,edx) )
		return false;

    return ( edx & (1<<15) ) != 0;
#endif
}

bool CheckFCMOVTechnology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#elif defined( _M_X64 ) || defined( __x86_64__ )
	return false;
#else

    uint32 eax,ebx,edx,unused;
    if( !cpuid(1,eax,ebx,unused,edx) )
		return false;

    return ( edx & (1<<16) ) != 0;
#endif
}

bool CheckRDTSCTechnology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#elif defined( _M_X64 ) || defined( __x86_64__ )
	return true;
#else

    uint32 eax,ebx,edx,unused;
    if( !cpuid(1,eax,ebx,unused,edx) )
		return false;

    return ( edx & 0x10 ) != 0;
#endif
}

bool CheckAESTechnology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#else
	uint32 eax,ebx,edx,ecx;
	if( !cpuid(1,eax,ebx,ecx,edx) )
		return false;

	return ( ecx & ( 1 << 25 ) ) != 0;	// bit 25 of ECX
#endif
}

bool CheckAVXTechnology(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#else
	uint32 eax,ebx,edx,ecx;
	if( !cpuid(1,eax,ebx,ecx,edx) )
		return false;

	return ( ecx & ( 1 << 28 ) ) != 0;	// bit 28 of ECX
#endif
}

bool CheckCMPXCHG16BTechnology( void )
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#else
	uint32 eax, ebx, edx, ecx;
	if ( !cpuid( 1, eax, ebx, ecx, edx ) )
		return false;

	return ( ecx & ( 1 << 13 ) ) != 0;	// bit 13 of ECX
#endif
}

bool CheckLAHFSAHFTechnology( void )
{
#if defined( _X360 ) || defined( _PS3 )
	return false;
#else
	uint32 eax, ebx, edx, ecx;
	if ( !cpuid( 0x80000000, eax, ebx, ecx, edx ) )
		return false;

	if ( eax <= 0x80000000 )
		return false;

	if ( !cpuid( 0x80000001, eax, ebx, ecx, edx ) )
		return false;

	return ( ecx & 0x00000001 ) != 0;	// bit 0 of ECX
#endif
}

#if 0 // SDR_PUBLIC

bool CheckPrefetchWTechnology( void )
{
#if defined(_WIN32)

	uint32 eax, ebx, edx, ecx;
	if ( !cpuid( 0x80000000, eax, ebx, ecx, edx ) )
		return false;

	if ( eax <= 0x80000000 )
		return false;

	if ( !cpuid( 0x80000001, eax, ebx, ecx, edx ) )
		return false;

	bool bPrefetchW = ( ( ecx & ( 1 << 8 ) ) != 0 );	// bit 8 of ECX

	if ( bPrefetchW )
	{
		return true;
	}

	bool bIllegal = false;

	__try
	{
		static const unsigned int s_data = 0xabcd0123;

		_m_prefetchw( &s_data );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER )
	{
		bIllegal = true;
	}

	return !bIllegal;

#else
	return false;
#endif
}


// Return the Processor's brand string, or "Unknown" if it is not supported on this CPU
const char* GetProcessorBrand()
{
#if defined( _X360 ) || defined( _PS3 )
	return "Unknown";
#else

	// 48 character long with null termination
	static char CPUBrand[48];
	memset( CPUBrand, 0, sizeof( CPUBrand ) );

	uint32 extendedIDs, reserved[3];
	if ( !cpuid( 0x80000000, extendedIDs, reserved[0], reserved[1], reserved[2] ) )
	{
		_tcscpy( CPUBrand, _T( "Unknown" ) );
	}
	else
	{
		// If this CPU supports the brand string, call cpuid three more times to build up the string
		// The string will be 48 characters long with null termination
		if ( extendedIDs >= 0x80000004 )
		{
			uint32_t CPUBrandRegisters[4];
			if ( cpuid( 0x80000002, CPUBrandRegisters[0], CPUBrandRegisters[1], CPUBrandRegisters[2], CPUBrandRegisters[3] ) )
			{
				memcpy( CPUBrand, &( CPUBrandRegisters[0] ), 16 );
			}

			if ( cpuid( 0x80000003, CPUBrandRegisters[0], CPUBrandRegisters[1], CPUBrandRegisters[2], CPUBrandRegisters[3] ) )
			{
				memcpy( CPUBrand + 16, &( CPUBrandRegisters[0] ), 16 );
			}
			
			if ( cpuid( 0x80000004, CPUBrandRegisters[0], CPUBrandRegisters[1], CPUBrandRegisters[2], CPUBrandRegisters[3] ) )
			{
				memcpy( CPUBrand + 32, &( CPUBrandRegisters[0] ), 16 );
			}
		}
		else
		{
			_tcscpy( CPUBrand, _T( "Unknown" ) );
		}
	}

	return CPUBrand;
#endif
}



#if !defined( _X360 ) && !defined( _PS3 )
void CpuidWrapper( uint32 param, uint32 *pRegisters )
{
	uint32 CPUIDRegisters[4];
	if ( cpuid(param  ,CPUIDRegisters[0], CPUIDRegisters[1], CPUIDRegisters[2], CPUIDRegisters[3] ) )
		memcpy( pRegisters, &CPUIDRegisters[0], sizeof(CPUIDRegisters) );
}
#endif

const uint32* GetProcessorDetailBlob( int32 *pcunDetail )
{
#if defined( _X360 ) || defined( _PS3 )
	*pcunDetail = 0;
	return NULL;
#else
	static uint32 DetailBlob[4*8]; // 128 bytes
	memset( DetailBlob, 0, sizeof( DetailBlob ) );
	
	int iun = 0;
	CpuidWrapper( 0, &DetailBlob[iun] ); iun += 4;
	CpuidWrapper( 1, &DetailBlob[iun] ); iun += 4;
	CpuidWrapper( 2, &DetailBlob[iun] ); iun += 4;
	CpuidWrapper( 4, &DetailBlob[iun] ); iun += 4;
	CpuidWrapper( 5, &DetailBlob[iun] ); iun += 4;
	CpuidWrapper( 6, &DetailBlob[iun] ); iun += 4;
	CpuidWrapper( 9, &DetailBlob[iun] ); iun += 4;
	CpuidWrapper( 0xA, &DetailBlob[iun] ); iun += 4;

	*pcunDetail = sizeof( DetailBlob );
	return DetailBlob;
#endif
}


#if !defined( _PS3 ) && !defined( _X360 )
// Returns non-zero if Hyper-Threading Technology is supported on the processors and zero if not.  This does not mean that 
// Hyper-Threading Technology is necessarily enabled.
static bool HTSupported(void)
{

	static const unsigned int HT_BIT		 = 0x10000000;  // EDX[28] - Bit 28 set indicates Hyper-Threading Technology is supported in hardware.
	static const unsigned int FAMILY_ID     = 0x0f00;      // EAX[11:8] - Bit 11 thru 8 contains family processor id
	static const unsigned int EXT_FAMILY_ID = 0x0f00000;	// EAX[23:20] - Bit 23 thru 20 contains extended family  processor id
	static const unsigned int PENTIUM4_ID   = 0x0f00;		// Pentium 4 family processor id

	uint32 unused,
        reg_eax = 0, 
        reg_edx = 0,
        vendor_id[3] = {0, 0, 0};

	// verify cpuid instruction is supported
	if( !cpuid(0,unused, vendor_id[0],vendor_id[2],vendor_id[1]) 
	 || !cpuid(1,reg_eax,unused,unused,reg_edx) )
	 return false;

	//  Check to see if this is a Pentium 4 or later processor
	if (((reg_eax & FAMILY_ID) ==  PENTIUM4_ID) || (reg_eax & EXT_FAMILY_ID))
		if (vendor_id[0] == 0x756e6547 && vendor_id[1] == 0x49656e69 && vendor_id[2] == 0x6c65746e )
			return (reg_edx & HT_BIT) != 0;	// Genuine Intel Processor with Hyper-Threading Technology

	return false;  // This is not a genuine Intel processor.
}
#endif // _PS3

// Returns the number of logical processors per physical processors.
static uint8 LogicalProcessorsPerPackage(void)
{
#if defined( _X360 ) || defined( _PS3 )
	return 2; // 
#else

	const unsigned NUM_LOGICAL_BITS = 0x00FF0000; // EBX[23:16] indicate number of logical processors per package

    uint32 unused,
        reg_ebx = 0;
    if (!HTSupported()) 
		return 1; 

	if( !cpuid(1,unused,reg_ebx,unused,unused) )
		return 1;

	return (uint8) ((reg_ebx & NUM_LOGICAL_BITS) >> 16);
#endif
}

// Measure the processor clock speed by sampling the cycle count, waiting
// for some fraction of a second, then measuring the elapsed number of cycles.
static int64 CalculateClockSpeed()
{
#if defined( _X360 ) || defined(_PS3)
	// Xbox360 and PS3 have the same clock speed and share a lot of characteristics on PPU
	return 3200000000LL;
#elif defined( _WIN32 )
	int64 freq = 0;
	HKEY hKey = NULL;
	uint uRet = RegOpenKeyExA( HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey );
	if ( ERROR_SUCCESS == uRet )
	{
		uint iSpeed = 0;
		DWORD dwType = REG_NONE;
		DWORD cubValue = sizeof( iSpeed );
		uRet = RegQueryValueExA( hKey, "~MHz", NULL, &dwType, (LPBYTE)&iSpeed, &cubValue );
		if ( ERROR_SUCCESS == uRet && dwType == REG_DWORD )
		{
			freq = iSpeed;
			freq *= 1000000;
		}
		RegCloseKey( hKey );
	}
	if ( freq == 0 )
	{
		// We are seeing Divide-by-zero crashes on some Windows machines that seem
		// to only be possible if we are getting a clock speed of zero.
		// Return a plausible speed and get on with our day.
		freq = 2000000000;
	}
	return freq;

#elif defined(POSIX)
	uint64 CalculateCPUFreq(void); // from cpu_posix.cpp
	int64 freq =(int64)CalculateCPUFreq();
	if ( freq == 0 ) // couldn't calculate clock speed
	{
		Error( "Unable to determine CPU Frequency. Try defining CPU_MHZ.\n" );
	}
	return freq;
#endif
}

#endif // #if 0 // SDR_PUBLIC


const CPUInformation& GetCPUInformation()
{
	// Initialize m_Size to zero just to be paranoid
	static CPUInformation pi = {};

	// Has the structure already been initialized and filled out?
	if( pi.m_Size == sizeof(pi) )
		return pi;

	// Redundant, but just in case the user somehow messes with the size.
	memset(&pi, 0x0, sizeof(pi));

	// Fill out the structure, and return it:
	pi.m_Size = sizeof(pi);

//	// Grab the processor frequency:
//	pi.m_Speed = CalculateClockSpeed();
//	
//
//#if defined( _X360 )
//	pi.m_nPhysicalProcessors = 3;
//	pi.m_nLogicalProcessors  = 6;
//#elif defined( _PS3 )
//	pi.m_nPhysicalProcessors = 1;
//	pi.m_nLogicalProcessors  = 2;
//#elif defined(_WIN32) && !defined(_X360) 
//
//	// GetSystemInfo returns number of logical processors... mostly. Can undercount for unknown reasons.
//	SYSTEM_INFO si;
//	ZeroMemory( &si, sizeof(si) );
//	GetSystemInfo( &si );
//	uint unLogicalCores = si.dwNumberOfProcessors;
//
//	// CpuTopology class does an entirely different thing to get processor core counts.
//	// For Vista and beyond, it does GetLogicalProcessorInfo; for XP it does strange,
//	// vendor-specific things with CPUID and ASIC package information. We don't really
//	// trust it a whole lot.
//	CpuTopology topo;
//	uint unPhysicalCores = topo.NumberOfSystemCores();
//	uint unLogicalCoresAlt1 = topo.NumberOfLogicalCores();
//
//	// GetProcessAffinityMask queries the system affinity mask, one bit per enabled logical core.
//	uint unLogicalCoresAlt2 = 0;
//	DWORD_PTR procAffinity = 0, sysAffinity = 0;
//	if ( GetProcessAffinityMask( GetCurrentProcess(), &procAffinity, &sysAffinity ) )
//	{
//		// Count bits
//		for ( uint64 x = sysAffinity; x; x /= 2 )
//		{
//			unLogicalCoresAlt2 += (x & 1);
//		}
//	}
//
//	// Enforce sane relationships between physical and logical cores, and pick the
//	// "best" logical core count since the failure case of any of the three methods
//	// is under-reporting, not over-reporting.
//	unPhysicalCores = MAX( unPhysicalCores, 1 );
//	unLogicalCores = MAX( unLogicalCores, unPhysicalCores );
//	unLogicalCores = MAX( unLogicalCores, unLogicalCoresAlt1 );
//	unLogicalCores = MAX( unLogicalCores, unLogicalCoresAlt2 );
//
//	pi.m_nPhysicalProcessors = unPhysicalCores;
//	pi.m_nLogicalProcessors = unLogicalCores;
//	
//elif defined(_XBOX)
//	pi.m_nPhysicalProcessors = 1;
//	pi.m_nLogicalProcessors  = 1;
//elif defined(LINUX)
//
//	pi.m_nLogicalProcessors = 0;
//	pi.m_nPhysicalProcessors = 0;
//	const int k_cMaxProcessors = 256;
//	bool rgbProcessors[k_cMaxProcessors];
//   bool rgbCores[k_cMaxProcessors];
//	memset( rgbProcessors, 0, sizeof( rgbProcessors ) );
//	memset( rgbCores, 0, sizeof( rgbCores ) );
//	int cCoreIdCount = 0;
//
//	FILE *fpCpuInfo = fopen( "/proc/cpuinfo", "r" );
//	if ( fpCpuInfo )
//	{
//		char rgchLine[256];
//		while ( fgets( rgchLine, sizeof( rgchLine ), fpCpuInfo ) )
//		{
//			if ( !strncasecmp( rgchLine, "processor", strlen( "processor" ) ) )
//			{
//				pi.m_nLogicalProcessors++;
//			}
//			if ( !strncasecmp( rgchLine, "core id", strlen( "core id" ) ) )
//			{
//               //
//               // Core IDs are not necessarily sequential, so we just
//               // record the ones we see and count up the unique IDs
//               // at the end.
//               //
//				char *pchValue = strchr( rgchLine, ':' );
//               unsigned int cCoreId = (unsigned int)atoi( pchValue + 1 );
//               if (cCoreId < k_cMaxProcessors)
//               {
//                   rgbCores[cCoreId] = true;
//				}
//			}
//			if ( !strncasecmp( rgchLine, "physical id", strlen( "physical id" ) ) )
//			{
//				// it seems (based on survey data) that we can see
//				// processor N (N > 0) when it's the only processor in
//				// the system.  so keep track of each processor
//				char *pchValue = strchr( rgchLine, ':' );
//				int cPhysicalId = atoi( pchValue + 1 );
//				if ( cPhysicalId < k_cMaxProcessors )
//					rgbProcessors[cPhysicalId] = true;
//			}
//			/* this code will tell us how many physical chips are in the machine, but we want
//			   core count, so for the moment, each processor counts as both logical and physical.
//			if ( !strncasecmp( rgchLine, "physical id ", strlen( "physical id " ) ) )
//			{
//				char *pchValue = strchr( rgchLine, ':' );
//				pi.m_nPhysicalProcessors = max( pi.m_nPhysicalProcessors, atol( pchValue ) );
//			}
//			*/
//		}
//		fclose( fpCpuInfo );
//
//		// count the number of unique core IDs
//		for ( int i = 0; i < k_cMaxProcessors; i++ )
//       {
//           if (rgbCores[i])
//           {
//               ++cCoreIdCount;
//			}
//		}
//		for ( int i = 0; i < k_cMaxProcessors; i++ )
//			if ( rgbProcessors[i] )
//				pi.m_nPhysicalProcessors++;
//		pi.m_nPhysicalProcessors *= cCoreIdCount;
//
//       // Older processors may not have core IDs or physical IDs.
//       // For example, a simple uniprocessor might not have anything
//       // since it's just a simple uniprocessor.  If we didn't find
//       // a reasonable physical count just assume it matches the
//       // logical count.
//       if ( pi.m_nPhysicalProcessors == 0 )
//       {
//           pi.m_nPhysicalProcessors = pi.m_nLogicalProcessors;
//       }
//	}
//	else
//	{
//		pi.m_nLogicalProcessors = 1;
//		pi.m_nPhysicalProcessors = 1;
//		Assert( !"couldn't read cpu information from /proc/cpuinfo" );
//	}
//
//elif defined(OSX)
//	size_t len = 2;
//	int mib[len];
//	mib[0] = CTL_HW;
//	size_t size;
//	int physicalCPUS = 1, logicalCPUS = 1;
//
//	sysctlnametomib("hw.physicalcpu", mib, &len);
//	size = sizeof(physicalCPUS);
//	sysctl( mib, len, &physicalCPUS, &size, NULL, 0 );
//
//	sysctlnametomib("hw.logicalcpu", mib, &len);
//	size = sizeof(logicalCPUS);
//	sysctl( mib, len, &logicalCPUS, &size, NULL, 0 );
//
//	pi.m_nPhysicalProcessors = physicalCPUS > 0 ? physicalCPUS : 1;
//	pi.m_nLogicalProcessors  = logicalCPUS > 0 ?  logicalCPUS : 1;
//
//else
//
//	pi.m_nPhysicalProcessors = 1;
//	pi.m_nLogicalProcessors = 1;
//
//endif

	// Determine Processor Features:
	pi.m_bRDTSC        = CheckRDTSCTechnology();
	pi.m_bCMOV         = CheckCMOVTechnology();
	pi.m_bFCMOV        = CheckFCMOVTechnology();
	pi.m_bMMX          = CheckMMXTechnology();
	pi.m_bSSE          = CheckSSETechnology();
	pi.m_bSSE2         = CheckSSE2Technology();
	pi.m_bSSE3         = CheckSSE3Technology();
	pi.m_bSSSE3		   = CheckSSSE3Technology();
	pi.m_bSSE4a        = CheckSSE4aTechnology();
	pi.m_bSSE41        = CheckSSE41Technology();
	pi.m_bSSE42        = CheckSSE42Technology();
	pi.m_b3DNow        = Check3DNowTechnology();
	pi.m_bAES		   = CheckAESTechnology();
	pi.m_bAVX		   = CheckAVXTechnology();
	pi.m_bCMPXCHG16B   = CheckCMPXCHG16BTechnology();
	pi.m_bLAHFSAHF	   = CheckLAHFSAHFTechnology();
//	pi.m_bPrefetchW	   = CheckPrefetchWTechnology();
	pi.m_szProcessorID = GetProcessorVendorId();
	//pi.m_szProcessorBrand = GetProcessorBrand();
	//pi.m_punProcessDetail = GetProcessorDetailBlob( &pi.m_cunProcessDetail );

	return pi;
}


//void GetPortableSystemInformation( SPortableSystemInformation *Info )
//{
//	const CPUInformation& CpuInfo = GetCPUInformation();
//
//	memset( Info, 0, sizeof( *Info ) );
//
//	Info->m_nMagic = PORTABLE_SYSTEM_INFORMATION_MAGIC;
//	
//	Info->m_nLogicalProcessors = CpuInfo.m_nLogicalProcessors;
//	Info->m_nPhysicalProcessors = CpuInfo.m_nPhysicalProcessors;
//	
//	Info->m_nProcessorFeatures = 0;
//    if ( CpuInfo.m_bRDTSC )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_RDTSC;
//	}
//    if ( CpuInfo.m_bCMOV )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_CMOV;
//	}
//    if ( CpuInfo.m_bFCMOV )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_FCMOV;
//	}
//    if ( CpuInfo.m_bSSE )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_SSE;
//	}
//    if ( CpuInfo.m_bSSE2 )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_SSE2;
//	}
//    if ( CpuInfo.m_bSSE3 )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_SSE3;
//	}
//    if ( CpuInfo.m_bSSSE3 )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_SSSE3;
//	}
//    if ( CpuInfo.m_bSSE41 )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_SSE41;
//	}
//    if ( CpuInfo.m_bSSE42 )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_SSE42;
//	}
//    if ( CpuInfo.m_bSSE4a )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_SSE4A;
//	}
//    if ( CpuInfo.m_b3DNow )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_3DNOW;
//	}
//    if ( CpuInfo.m_bMMX )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_MMX;
//	}
//    if ( CpuInfo.m_bHT )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_HT;
//	}
//    if ( CpuInfo.m_bAES )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_AES;
//	}
//    if ( CpuInfo.m_bAVX )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_AVX;
//	}
//	if ( CpuInfo.m_bCMPXCHG16B )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_CMPXCHG16B;
//	}
//	if ( CpuInfo.m_bLAHFSAHF )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_LAHFSAHF;
//	}
//	if ( CpuInfo.m_bPrefetchW )
//	{
//		Info->m_nProcessorFeatures |= PROC_FEATURE_PREFETCHW;
//	}
//
//	Info->m_nProcessorCyclesPerSecond = CpuInfo.m_Speed;
//
//#if defined(__x86_64__)
//	Info->m_nProcessorType = k_EProcessorTypeX64;
//	Info->m_nPageSize = 4096;
//#elif defined(__i386__)
//	Info->m_nProcessorType = k_EProcessorTypeX86;
//	Info->m_nPageSize = 4096;
//#else
//	Info->m_nProcessorType = k_EProcessorTypeUnknown;
//#endif
//	
//	Info->m_nProcessorFlags = PROC_FLAG_VENDOR_ID_IS_STR;
//
//	if (CpuInfo.m_cunProcessDetail >= 5 * sizeof(Info->m_nProcessorSignature))
//	{
//		Info->m_nProcessorSignature = CpuInfo.m_punProcessDetail[4];
//	}
//
//#if defined(_WIN32)
//
//	SYSTEM_INFO WinSysInfo;
//
//	GetSystemInfo( &WinSysInfo );
//
//	Info->m_nMinimumValidAddress = (uintp)WinSysInfo.lpMinimumApplicationAddress;
//	Info->m_nMaximumValidAddress = (uintp)WinSysInfo.lpMaximumApplicationAddress;
//
//#endif
//    
//	// We've zeroed the struct so if this strncpy doesn't
//	// terminate we'll still have a terminator at the end of the array.
//	strncpy( (char*)&Info->m_ProcessorVendorId,
//			 CpuInfo.m_szProcessorID,
//			 sizeof( Info->m_ProcessorVendorId ) - 1 );
//
//	strncpy( Info->m_szProcessorBrand, CpuInfo.m_szProcessorBrand, sizeof( Info->m_szProcessorBrand ) );
//}
