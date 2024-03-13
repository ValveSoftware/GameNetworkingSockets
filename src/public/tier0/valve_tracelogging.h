//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Define a very thin wrapper around ETW TraceLogging.  This is the newest
// way Microsoft has made to emit ETW events that does not require you to
// define your events in a resource file and make a manifest.
//
// Goals of this wrapper layer:
//
// - Use dynamic linking so that we can run on older versions of Windows
// - Define stubs when VALVE_DISABLE_TRACELOGGING is defined or on non-Windows platform.
// - NON-GOAL: Try to provide any sort of ETW-like behaviour on non-Windows platforms.
//
// Leaf code usage:
//
// - Include this header as late as possible in your .cpp file.
//   Avoiding including it in a header file.
//   (Unfortunately, we have to include Windows headers)
//
// - Use TraceLoggingWrite( ... ) to emit an event.  See the documentation linked
//   to below, or search for the code for examples, for all of the arguments.
//
// - To emit an "activity" (an activity has a start and stop event)
// 
//   - For most simple cases, us TRACELOGGING_ACTIVITY_SCOPE( provider, name, [additional_info...] )
//
//     example usage:
//     {
//         TRACELOGGING_ACTIVITY_SCOPE( g_HMyProvider, MyFunction, TraceLoggingInt32( interstingIntData, "InterestingData" ) );
//         MyFunction( interstingIntData );
//     }
// 
//   - Declare a TraceLoggingThreadActivity<Provider> variable and use TraceLoggingWriteStart (and optionally TraceLoggingWriteStop)
//     E.g. the above is equivalent to:
//     {
//         TraceLoggingThreadActivity<g_HMyProvider> activity;
//         TraceLoggingWriteStart( activity, "MyFunction", TraceLoggingInt32( interstingIntData, "InterestingData" ) );
//         MyFunction( interstingIntData );
//     }
//
// - Use IsTraceLoggingProviderEnabled if there is significant "setup" code that
//   is needed only when tracing, that you want to avoid when tracing is not enabled.
//
// - Use '#if IsTraceLoggingEnabled()' to stub code entirely at compile time
//
// - For simple one-line calls of TraceLoggingWrite, there is no need for
//   either IsTraceLoggingEnabled() or IsTraceLoggingProviderEnabled()
//
// NOTES:
//
// - Documentation is here:
//   https://learn.microsoft.com/en-us/windows/win32/api/_tracelogging/
//
// - Name your provider something like "Valve.Steam.MyComponent"
//
// - Define your provider GUID using the standard hashing method, and then your
//   system can be enabled by name instead of by GUID!
//
//   https://learn.microsoft.com/en-us/windows/win32/api/traceloggingprovider/nf-traceloggingprovider-tracelogging_define_provider
//
//   Run this powershell: [System.Diagnostics.Tracing.EventSource]::new("Valve.Steam.MyComponent").Guid
//
// - The TraceLogging documentation claims that trace logging provider handles should not be shared
//   across DLL boundaries.

#ifndef VALVE_TRACELOGGING_H
#define VALVE_TRACELOGGING_H

#include "platform.h"

#ifdef _WINDOWS

	// NOTE - for compile consistency, we always include the windows headers, even if tracelogging is not enabled

	// !KLUDGE! Always Target Windows 7.
	//#ifndef WINVER
		#undef WINVER
		#define WINVER 0x0601
	//#endif

	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <windows.h>
	//#include <ShlObj.h>
	//#include <winsock2.h>
	#include <winmeta.h>

	#ifdef VALVE_DISABLE_TRACELOGGING
		#define IsTraceLoggingEnabled() false
	#else
		#define IsTraceLoggingEnabled() true

		#define EVNTAPI
		#define EventEnabled EventEnabled_ValveDoNotUse
		#define EventProviderEnabled EventProviderEnabled_ValveDoNotUse
		#define EventRegister EventRegister_Valve
		#define EventUnregister EventUnregister_Valve
		#define EventWrite EventWrite_ValveDoNotUse
		#define EventWriteTransfer EventWriteTransfer_Valve
		#define EventActivityIdControl EventActivityIdControl_Valve

		#include <TraceLoggingProvider.h>
		#include <TraceLoggingActivity.h>

		//PLATFORM_INTERFACE BOOLEAN Plat_EventEnabled( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor );
		//inline BOOLEAN EVNTAPI EventEnabled( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor )
		//{
		//	return Plat_EventEnabled( RegHandle, EventDescriptor );
		//}
		//
		//PLATFORM_INTERFACE BOOLEAN Plat_EventProviderEnabled( REGHANDLE RegHandle, UCHAR Level, ULONGLONG Keyword );
		//inline BOOLEAN EVNTAPI EventProviderEnabled( REGHANDLE RegHandle, UCHAR Level, ULONGLONG Keyword )
		//{
		//	return Plat_EventProviderEnabled( RegHandle, Level, Keyword )
		//}

		PLATFORM_INTERFACE ULONG Plat_EventRegister( LPCGUID ProviderId, PENABLECALLBACK EnableCallback, PVOID CallbackContext, PREGHANDLE RegHandle );
		inline ULONG EVNTAPI EventRegister( LPCGUID ProviderId, PENABLECALLBACK EnableCallback, PVOID CallbackContext, PREGHANDLE RegHandle )
		{
			return Plat_EventRegister( ProviderId, EnableCallback, CallbackContext, RegHandle );
		}

		PLATFORM_INTERFACE ULONG Plat_EventUnregister( REGHANDLE RegHandle );
		inline ULONG EVNTAPI EventUnregister( REGHANDLE RegHandle )
		{
			return Plat_EventUnregister( RegHandle );
		}

		//PLATFORM_INTERFACE ULONG __stdcall Plat_EventWrite( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData );
		//inline ULONG EVNTAPI EventWrite( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData )
		//{
		//	return Plat_EventWrite( RegHandle, EventDescriptor, UserDataCount, UserData );
		//}

		PLATFORM_INTERFACE ULONG __stdcall Plat_EventWriteTransfer( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, LPCGUID ActivityId, LPCGUID RelatedActivityId, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData );
		inline ULONG EVNTAPI EventWriteTransfer( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, LPCGUID ActivityId, LPCGUID RelatedActivityId, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData )
		{
			return Plat_EventWriteTransfer( RegHandle, EventDescriptor, ActivityId, RelatedActivityId, UserDataCount, UserData );
		}

		PLATFORM_INTERFACE ULONG __stdcall Plat_EventActivityIdControl( ULONG ControlCode, LPGUID ActivityId );
		inline ULONG EVNTAPI EventActivityIdControl( ULONG ControlCode, LPGUID ActivityId )
		{
			return Plat_EventActivityIdControl( ControlCode, ActivityId );
		}

		#define IsTraceLoggingProviderEnabled( hProvider ) TraceLoggingProviderEnabled( (hProvider), 0, 0 )

		// TRACELOGGING_DEFINE_PROVIDER_AUTOREGISTER is the same as TRACELOGGING_DEFINE_PROVIDER, but it
		// automatically registers and unregisters the provider at static init/shutdown time
		#define TRACELOGGING_DEFINE_PROVIDER_AUTOREGISTER( hprovider, providerName, providerId, ... ) \
			TRACELOGGING_DEFINE_PROVIDER( hprovider, providerName, providerId, __VA_ARGS__ ); \
			ValveTraceLoggingAutoRegister< hprovider > hprovider##_autoRegister

		template< TraceLoggingHProvider const& provider > 
		struct ValveTraceLoggingAutoRegister
		{
			ValveTraceLoggingAutoRegister() { TraceLoggingRegister( provider ); }
			~ValveTraceLoggingAutoRegister() { TraceLoggingUnregister( provider ); }
		};

		// TRACELOGGING_ACTIVITY_SCOPE is shorthand for a common situation
		// Note: activity_name should not be quoted
		#define TRACELOGGING_ACTIVITY_SCOPE( hprovider, activity_name, ... ) \
			TraceLoggingThreadActivity<hprovider> traceloggingscopevar_##activity_name; \
			TraceLoggingWriteStart( traceloggingscopevar_##activity_name, #activity_name, __VA_ARGS__ ); \
			RunCodeAtScopeExit( TraceLoggingWriteStop( traceloggingscopevar_##activity_name, #activity_name ) )

	#endif

#else
	#define IsTraceLoggingEnabled() false
#endif

//
// Define stubs when trace logging is not enabled
//
#if !IsTraceLoggingEnabled()

	struct _tlgProvider_t;
	typedef struct _tlgProvider_t const* TraceLoggingHProvider;

	#define TRACELOGGING_DECLARE_PROVIDER( hprovider ) extern "C" const TraceLoggingHProvider hprovider;
	#define TRACELOGGING_DEFINE_PROVIDER( hprovider, ... ) extern "C" const TraceLoggingHProvider hprovider = nullptr;
	#define TRACELOGGING_DEFINE_PROVIDER_AUTOREGISTER( hprovider, ... ) TRACELOGGING_DEFINE_PROVIDER( hprovider )
	#define TraceLoggingRegister( ... ) ((void)0)
	#define TraceLoggingUnregister( ... ) ((void)0)
	#define TraceLoggingWrite( ... ) ((void)0)
	#define IsTraceLoggingProviderEnabled( hprovider ) false
	#define TraceLoggingWriteStart( activity, name, ...) ((void)0)
	#define TraceLoggingWriteTagged( activity, name, ...) ((void)0)
	#define TraceLoggingWriteStop( activity, name, ...) ((void)0)
	#define TRACELOGGING_ACTIVITY_SCOPE( hprovider , activity_name, ... ) ((void)0)

	template< TraceLoggingHProvider const& provider > class TraceLoggingActivity {};
	template< TraceLoggingHProvider const& provider > class TraceLoggingThreadActivity {};
#endif

#endif // _H
