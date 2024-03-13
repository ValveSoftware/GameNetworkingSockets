//====== Copyright Valve Corporation, All rights reserved. ====================

#include <tier0/valve_tracelogging.h>
#if IsTraceLoggingEnabled()

/////////////////////////////////////////////////////////////////////////////
//
// Hacks to avoid linking with an import lib and forcing a dependency
// on a certain version of Windows
//
/////////////////////////////////////////////////////////////////////////////

// Typedefs for use with GetProcAddress
typedef ULONG (__stdcall *tEventRegister)( LPCGUID ProviderId, PENABLECALLBACK EnableCallback, PVOID CallbackContext, PREGHANDLE RegHandle );
static tEventRegister s_pEventRegister;

typedef ULONG (__stdcall *tEventUnregister)( REGHANDLE RegHandle );
static tEventUnregister s_pEventUnregister;

//typedef ULONG (__stdcall *tEventWrite)( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData );
//static ULONG __stdcall Dummy_EventWrite( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData ) { return 0; }
//static tEventWrite s_pEventWrite = Dummy_EventWrite; // Always non-NULL

typedef ULONG (__stdcall *tEventWriteTransfer)( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, LPCGUID ActivityId, LPCGUID RelatedActivityId, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData );
static ULONG __stdcall Dummy_EventWriteTransfer( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, LPCGUID ActivityId, LPCGUID RelatedActivityId, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData ) { return 0; }
static tEventWriteTransfer s_pEventWriteTransfer = Dummy_EventWriteTransfer; // Always non-NULL

typedef ULONG (__stdcall *tEventActivityIdControl)( ULONG ControlCode, LPGUID ActivityId );
static ULONG __stdcall Dummy_EventActivityIdControl( ULONG ControlCode, LPGUID ActivityId ) { return ERROR_NOT_SUPPORTED; }
static tEventActivityIdControl s_pEventActivityIdControl = Dummy_EventActivityIdControl; // Always non-NULL

PLATFORM_INTERFACE ULONG Plat_EventRegister( LPCGUID ProviderId, PENABLECALLBACK EnableCallback, PVOID CallbackContext, PREGHANDLE RegHandle )
{

	// One time init
	static bool once = [](){

		// Find Advapi32.dll. This should always succeed.
		HMODULE pAdvapiDLL = ::LoadLibraryA( "advapi32.dll" );
		if ( pAdvapiDLL )
		{

			// EventWriteTransfer is the newest function we require.  Don't check for the others if it's missing
			tEventWriteTransfer pEventWriteTranfer = ( tEventWriteTransfer )GetProcAddress( pAdvapiDLL, "EventWriteTransfer" );
			tEventActivityIdControl pEventActivityIdControl = ( tEventActivityIdControl )GetProcAddress( pAdvapiDLL, "EventActivityIdControl" );
			if ( pEventWriteTranfer && pEventActivityIdControl )
			{

				s_pEventRegister = ( tEventRegister )GetProcAddress( pAdvapiDLL, "EventRegister" );
				s_pEventUnregister = ( tEventUnregister )GetProcAddress( pAdvapiDLL, "EventUnregister" );
				s_pEventWriteTransfer = pEventWriteTranfer;
				s_pEventActivityIdControl = pEventActivityIdControl;
			}
		}

		return true;
	}();

	if ( s_pEventRegister )
		return s_pEventRegister( ProviderId, EnableCallback, CallbackContext, RegHandle );

	// We are contractually obliged to initialize this.
	*RegHandle = 0;
	return 0;
}

PLATFORM_INTERFACE ULONG Plat_EventUnregister( REGHANDLE RegHandle )
{
	if ( s_pEventUnregister )
		return s_pEventUnregister( RegHandle );
	return 0;
}

// These wrappers are the most perf-sensitive.  So we make their function pointer
// always be non-NULL.  Because the function signature matches (including __stdcall),
// these should be implemented with a single 'jmp [blah]' instruction.  At the time
// this comment was written, I examined the assembly of a release build, and it was
// optimal

//PLATFORM_INTERFACE ULONG __stdcall Plat_EventWrite( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData )
//{
//	return (*s_pEventWrite)( RegHandle, EventDescriptor, UserDataCount, UserData );
//}
PLATFORM_INTERFACE ULONG __stdcall Plat_EventWriteTransfer( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, LPCGUID ActivityId, LPCGUID RelatedActivityId, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData )
{
	return (*s_pEventWriteTransfer)( RegHandle, EventDescriptor, ActivityId, RelatedActivityId, UserDataCount, UserData );
}
PLATFORM_INTERFACE ULONG __stdcall Plat_EventActivityIdControl( ULONG ControlCode, LPGUID ActivityId )
{
	return (*s_pEventActivityIdControl)( ControlCode, ActivityId );
}

#endif // #if IsTraceLoggingEnabled()
