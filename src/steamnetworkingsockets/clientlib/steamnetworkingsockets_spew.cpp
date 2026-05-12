//====== Copyright Valve Corporation, All rights reserved. ====================
//
// Diagnostic output/logging ("spew") for SteamNetworkingSockets.
//
#include "steamnetworkingsockets_lowlevel.h"
#include "../steamnetworkingsockets_internal.h"
#include <tier0/valve_tracelogging.h>
#include <tier0/memdbgoff.h>

TRACELOGGING_DECLARE_PROVIDER( HTraceLogging_SteamNetworkingSockets );

namespace SteamNetworkingSocketsLib {

static ShortDurationLock s_systemSpewLock( "SystemSpew", LockDebugInfo::k_nOrder_Max ); // Never take another lock while holding this
SteamNetworkingMicroseconds g_usecLastRateLimitSpew;
int g_nRateLimitSpewCount;
ESteamNetworkingSocketsDebugOutputType g_eAppSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_Msg; // Option selected by app
ESteamNetworkingSocketsDebugOutputType g_eDefaultGroupSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_Msg; // Effective value
FSteamNetworkingSocketsDebugOutput g_pfnDebugOutput = nullptr;
void (*g_pfnPreFormatSpewHandler)( ESteamNetworkingSocketsDebugOutputType eType, bool bFmt, const char* pstrFile, int nLine, const char *pMsg, va_list ap ) = SteamNetworkingSockets_DefaultPreFormatDebugOutputHandler;
static bool s_bSpewInitted = false;

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SYSTEMSPEW
static FILE *g_pFileSystemSpew;
static SteamNetworkingMicroseconds g_usecSystemLogFileOpened;
static bool s_bNeedToFlushSystemSpew = false;
static ESteamNetworkingSocketsDebugOutputType g_eSystemSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_None; // Option selected by the "system" (environment variable, etc)
#else
constexpr ESteamNetworkingSocketsDebugOutputType g_eSystemSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_None; // Option selected by the "system" (environment variable, etc)
#endif

void InitSpew()
{
	ShortDurationScopeLock scopeLock( s_systemSpewLock );

	// First time, check environment variables and set system spew level
	if ( !s_bSpewInitted )
	{
		s_bSpewInitted = true;

		#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SYSTEMSPEW
			const char *STEAMNETWORKINGSOCKETS_LOG_LEVEL = getenv( "STEAMNETWORKINGSOCKETS_LOG_LEVEL" );
			if ( !V_isempty( STEAMNETWORKINGSOCKETS_LOG_LEVEL ) )
			{
				switch ( atoi( STEAMNETWORKINGSOCKETS_LOG_LEVEL ) )
				{
					case 0: g_eSystemSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_None; break;
					case 1: g_eSystemSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_Warning; break;
					case 2: g_eSystemSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_Msg; break;
					case 3: g_eSystemSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_Verbose; break;
					case 4: g_eSystemSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_Debug; break;
					case 5: g_eSystemSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_Everything; break;
				}

				if ( g_eSystemSpewLevel > k_ESteamNetworkingSocketsDebugOutputType_None )
				{

					// What log file to use?
					const char *pszLogFile = getenv( "STEAMNETWORKINGSOCKETS_LOG_FILE" );
					if ( !pszLogFile )
						pszLogFile = "steamnetworkingsockets.log" ;

					// Try to open file.  Use binary mode, since we want to make sure we control
					// when it is flushed to disk
					g_pFileSystemSpew = fopen( pszLogFile, "wb" );
					if ( g_pFileSystemSpew )
					{
						g_usecSystemLogFileOpened = SteamNetworkingSockets_GetLocalTimestamp();
						time_t now = time(nullptr);
						fprintf( g_pFileSystemSpew, "Log opened, time %lld %s", (long long)now, ctime( &now ) );

						// if they ask for verbose, turn on some other groups, by default
						if ( g_eSystemSpewLevel >= k_ESteamNetworkingSocketsDebugOutputType_Verbose )
						{
							GlobalConfig::LogLevel_P2PRendezvous.m_value.m_defaultValue = g_eSystemSpewLevel;
							GlobalConfig::LogLevel_P2PRendezvous.m_value.Set( g_eSystemSpewLevel );

							GlobalConfig::LogLevel_PacketGaps.m_value.m_defaultValue = g_eSystemSpewLevel-1;
							GlobalConfig::LogLevel_PacketGaps.m_value.Set( g_eSystemSpewLevel-1 );
						}
					}
					else
					{
						// Failed
						g_eSystemSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_None;
					}
				}
			}
		#endif // #ifdef STEAMNETWORKINGSOCKETS_ENABLE_SYSTEMSPEW
	}

	g_eDefaultGroupSpewLevel = std::max( g_eSystemSpewLevel, g_eAppSpewLevel );

}

void KillSpew()
{
	ShortDurationScopeLock scopeLock( s_systemSpewLock );
	g_eDefaultGroupSpewLevel = g_eAppSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_None;
	g_pfnDebugOutput = nullptr;
	s_bSpewInitted = false;

	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SYSTEMSPEW
		g_eSystemSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_None;
		s_bNeedToFlushSystemSpew = false;
		if ( g_pFileSystemSpew )
		{
			fclose( g_pFileSystemSpew );
			g_pFileSystemSpew = nullptr;
		}
	#endif
}

#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SYSTEMSPEW
static void FlushSystemSpewLocked()
{
	s_systemSpewLock.AssertHeldByCurrentThread();
	if ( s_bNeedToFlushSystemSpew )
	{
		if ( g_pFileSystemSpew )
			fflush( g_pFileSystemSpew );
		s_bNeedToFlushSystemSpew = false;
	}
}
#endif

void FlushSystemSpew()
{
#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SYSTEMSPEW
	if ( s_bNeedToFlushSystemSpew ) // Read the flag without taking the lock first as an optimization, as most of the time it will not be set
	{
		ShortDurationScopeLock scopeLock( s_systemSpewLock );
		FlushSystemSpewLocked();
	}
#endif
}


void ReallySpewTypeFmt( int eType, const char *pMsg, ... )
{
	va_list ap;
	va_start( ap, pMsg );
	(*g_pfnPreFormatSpewHandler)( ESteamNetworkingSocketsDebugOutputType(eType), true, nullptr, 0, pMsg, ap );
	va_end( ap );
}

void SteamNetworkingSockets_SetDebugOutputFunction( ESteamNetworkingSocketsDebugOutputType eDetailLevel, FSteamNetworkingSocketsDebugOutput pfnFunc )
{
	if ( pfnFunc && eDetailLevel > k_ESteamNetworkingSocketsDebugOutputType_None )
	{
		SteamNetworkingSocketsLib::g_pfnDebugOutput = pfnFunc;
		SteamNetworkingSocketsLib::g_eAppSpewLevel = ESteamNetworkingSocketsDebugOutputType( eDetailLevel );
	}
	else
	{
		SteamNetworkingSocketsLib::g_pfnDebugOutput = nullptr;
		SteamNetworkingSocketsLib::g_eAppSpewLevel = k_ESteamNetworkingSocketsDebugOutputType_None;
	}

	SteamNetworkingSocketsLib::InitSpew();
}

} // namespace SteamNetworkingSocketsLib

using namespace SteamNetworkingSocketsLib;

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetPreFormatDebugOutputHandler(
	ESteamNetworkingSocketsDebugOutputType eDetailLevel,
	void (*pfn_Handler)( ESteamNetworkingSocketsDebugOutputType eType, bool bFmt, const char* pstrFile, int nLine, const char *pMsg, va_list ap )
)
{
	g_eDefaultGroupSpewLevel = eDetailLevel;
	g_pfnPreFormatSpewHandler = pfn_Handler;
}

STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_DefaultPreFormatDebugOutputHandler( ESteamNetworkingSocketsDebugOutputType eType, bool bFmt, const char* pstrFile, int nLine, const char *pMsg, va_list ap )
{
	// Do the formatting
	char buf[ 2048 ];
	int szBuf = sizeof(buf);
	char *msgDest = buf;
	if ( pstrFile )
	{

		// Skip to "/src/"
		for (char const* s = pstrFile; *s; ++s)
		{
			if (
				(s[0] == '/' || s[0] == '\\')
				&& s[1] == 's'
				&& s[2] == 'r'
				&& s[3] == 'c'
				&& (s[4] == '/' || s[4] == '\\')
			) {
				pstrFile = s + 1;
				break;
			}
		}

		int l = V_sprintf_safe( buf, "%s(%d): ", pstrFile, nLine );
		szBuf -= l;
		msgDest += l;
	}

	if ( bFmt )
		V_vsnprintf( msgDest, szBuf, pMsg, ap );
	else
		V_strncpy( msgDest, pMsg, szBuf );

	// Gah, some, but not all, of our code has newlines on the end
	V_StripTrailingWhitespaceASCII( buf );

	// Emit an ETW event.  Unfortunately, TraceLoggingLevel requires a constant argument
	if ( IsTraceLoggingProviderEnabled( HTraceLogging_SteamNetworkingSockets ) )
	{
		if ( eType <= k_ESteamNetworkingSocketsDebugOutputType_Error )
		{
			TraceLoggingWrite(
				HTraceLogging_SteamNetworkingSockets,
				"Spew",
				TraceLoggingLevel( WINEVENT_LEVEL_ERROR ),
				TraceLoggingString( buf, "Msg" )
			);
		}
		else if ( eType == k_ESteamNetworkingSocketsDebugOutputType_Warning )
		{
			TraceLoggingWrite(
				HTraceLogging_SteamNetworkingSockets,
				"Spew",
				TraceLoggingLevel( WINEVENT_LEVEL_WARNING ),
				TraceLoggingString( buf, "Msg" )
			);
		}
		else if ( eType >= k_ESteamNetworkingSocketsDebugOutputType_Verbose )
		{
			TraceLoggingWrite(
				HTraceLogging_SteamNetworkingSockets,
				"Spew",
				TraceLoggingLevel( WINEVENT_LEVEL_VERBOSE ),
				TraceLoggingString( buf, "Msg" )
			);
		}
		else
		{
			TraceLoggingWrite(
				HTraceLogging_SteamNetworkingSockets,
				"Spew",
				TraceLoggingLevel( WINEVENT_LEVEL_INFO ),
				TraceLoggingString( buf, "Msg" )
			);
		}
	}

	// Spew to log file?
	#ifdef STEAMNETWORKINGSOCKETS_ENABLE_SYSTEMSPEW
		if ( eType <= g_eSystemSpewLevel && g_pFileSystemSpew )
		{
			ShortDurationScopeLock scopeLock( s_systemSpewLock ); // WARNING - these locks are not re-entrant, so if we assert while holding it, we could deadlock!
			if ( eType <= g_eSystemSpewLevel && g_pFileSystemSpew )
			{

				// Write
				SteamNetworkingMicroseconds usecLogTime = SteamNetworkingSockets_GetLocalTimestamp() - g_usecSystemLogFileOpened;
				fprintf( g_pFileSystemSpew, "%8.3f %s\n", usecLogTime*1e-6, buf );

				// Queue to flush when we we think we can afford to hit the disk synchronously
				s_bNeedToFlushSystemSpew = true;

				// Flush certain critical messages things immediately
				if ( eType <= k_ESteamNetworkingSocketsDebugOutputType_Error )
					FlushSystemSpewLocked();
			}
		}
	#endif

	// Invoke callback
	FSteamNetworkingSocketsDebugOutput pfnDebugOutput = g_pfnDebugOutput;
	if ( pfnDebugOutput )
		pfnDebugOutput( eType, buf );
}
