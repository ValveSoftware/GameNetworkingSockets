//====== Copyright Valve Corporation, All rights reserved. ====================

#include "../steamnetworkingsockets_lowlevel.h"
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_ETW

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ShlObj.h>
#include <winsock2.h>

/////////////////////////////////////////////////////////////////////////////
//
// Hacks so that we use static linkage, instead of an import lib.
//
/////////////////////////////////////////////////////////////////////////////

#define EVNTAPI __stdcall
#define EventRegister EventRegister_SteamNetworkingSocketsHack
#define EventWrite EventWrite_SteamNetworkingSocketsHack
#define EventUnregister EventUnregister_SteamNetworkingSocketsHack
#include <evntprov.h>

// Typedefs for use with GetProcAddress
typedef ULONG (__stdcall *tEventRegister)( LPCGUID ProviderId, PENABLECALLBACK EnableCallback, PVOID CallbackContext, PREGHANDLE RegHandle );
typedef ULONG (__stdcall *tEventWrite)( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData);
typedef ULONG (__stdcall *tEventUnregister)( REGHANDLE RegHandle );

static tEventRegister s_pEventRegister;
static tEventWrite s_pEventWrite;
static tEventUnregister s_pEventUnregister;

ULONG EVNTAPI EventRegister( LPCGUID ProviderId, PENABLECALLBACK EnableCallback, PVOID CallbackContext, PREGHANDLE RegHandle )
{
	if ( s_pEventRegister )
		return s_pEventRegister( ProviderId, EnableCallback, CallbackContext, RegHandle );

	// We are contractually obliged to initialize this.
	*RegHandle = 0;
	return 0;
}
ULONG EVNTAPI EventWrite( REGHANDLE RegHandle, PCEVENT_DESCRIPTOR EventDescriptor, ULONG UserDataCount, PEVENT_DATA_DESCRIPTOR UserData )
{
	if ( s_pEventWrite )
		return s_pEventWrite( RegHandle, EventDescriptor, UserDataCount, UserData );
	return 0;
}
ULONG EVNTAPI EventUnregister( REGHANDLE RegHandle )
{
	if ( s_pEventUnregister )
		return s_pEventUnregister( RegHandle );
	return 0;
}

/////////////////////////////////////////////////////////////////////////////
//
// Include generated macros and stuff
//
/////////////////////////////////////////////////////////////////////////////

#include "steamnetworkingsockets_etw_events.h"

/////////////////////////////////////////////////////////////////////////////
//
// Interface to the rest of the library
//
/////////////////////////////////////////////////////////////////////////////

namespace SteamNetworkingSocketsLib {

void ETW_Init()
{

	// Find Advapi32.dll. This should always succeed.
	HMODULE pAdvapiDLL = ::LoadLibraryA( "advapi32.dll" );
	if ( pAdvapiDLL )
	{
		// Try to find the ETW functions. This will fail on XP.
		s_pEventRegister = ( tEventRegister )GetProcAddress( pAdvapiDLL, "EventRegister" );
		s_pEventWrite = ( tEventWrite )GetProcAddress( pAdvapiDLL, "EventWrite" );
		s_pEventUnregister = ( tEventUnregister )GetProcAddress( pAdvapiDLL, "EventUnregister" );

		EventRegisterValve_SteamNetworkingSockets();
	}
}

void ETW_Kill()
{
	// Unregister our providers.
	EventUnregisterValve_SteamNetworkingSockets();
}

void ETW_LongOp( const char *opName, SteamNetworkingMicroseconds usec, const char *pszInfo )
{
	EventWriteLongOp( opName, usec, pszInfo ? pszInfo : "" );
}

void ETW_UDPSendPacket( const netadr_t &adrTo, int cbPkt )
{
	EventWriteUDPSendPacket( CUtlNetAdrRender( adrTo ).String(), cbPkt );
}

void ETW_UDPRecvPacket( const netadr_t &adrFrom, int cbPkt )
{
	EventWriteUDPRecvPacket( CUtlNetAdrRender( adrFrom ).String(), cbPkt );
}

void ETW_ICESendPacket( HSteamNetConnection hConn, int cbPkt )
{
	EventWriteICESendPacket( hConn, cbPkt );
}

void ETW_ICERecvPacket( HSteamNetConnection hConn, int cbPkt )
{
	EventWriteICERecvPacket( hConn, cbPkt );
}

void ETW_ICEProcessPacket( HSteamNetConnection hConn, int cbPkt )
{
	EventWriteICEProcessPacket( hConn, cbPkt );
}

void ETW_webrtc_setsockopt( int slevel, int sopt, int value )
{
	EventWritewebrtc_setsockopt( slevel, sopt, value );
}

void ETW_webrtc_send( int length )
{
	EventWritewebrtc_send( length );
}

void ETW_webrtc_sendto( void *addr, int length )
{
	if ( !EventEnabledwebrtc_sendto() )
		return;
	netadr_t adrTo;
	adrTo.SetFromSockadr( addr, sizeof(sockaddr_storage) );
	EventWritewebrtc_sendto( CUtlNetAdrRender( adrTo ).String(), length );
}

}

#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_ETW
