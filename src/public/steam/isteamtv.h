//====== Copyright © 1996-2014 Valve Corporation, All rights reserved. =======
//
// Purpose: interface to Steam Video
//
//=============================================================================

#ifndef ISTEAMTV_H
#define ISTEAMTV_H
#ifdef _WIN32
#pragma once
#endif

#include "steam_api_common.h"

// callbacks
#if defined( VALVE_CALLBACK_PACK_SMALL )
#pragma pack( push, 4 )
#elif defined( VALVE_CALLBACK_PACK_LARGE )
#pragma pack( push, 8 )
#else
#error steam_api_common.h should define VALVE_CALLBACK_PACK_xxx
#endif


enum { k_cchBroadcastGameDataMax =  8 * 1024 };

// What the interaction behavior of the region is
enum ESteamTVRegionBehavior
{
	k_ESteamVideoRegionBehaviorInvalid = -1,
	k_ESteamVideoRegionBehaviorHover = 0,
	k_ESteamVideoRegionBehaviorClickPopup = 1,
	k_ESteamVideoRegionBehaviorClickSurroundingRegion = 2,
};

// Size of the region, normalized to 1920x1080
struct SteamTVRegion_t
{
	uint32 unMinX;
	uint32 unMinY;
	uint32 unMaxX;
	uint32 unMaxY;
};

//-----------------------------------------------------------------------------
// Purpose: SteamTV API
//-----------------------------------------------------------------------------
class ISteamTV
{
public:

	// Returns true if user is uploading a live broadcast
	virtual bool IsBroadcasting( int *pnNumViewers ) = 0;

	// Broadcast game data
	virtual void AddBroadcastGameData( const char *pchKey, const char *pchValue ) = 0;
	virtual void RemoveBroadcastGameData( const char *pchKey ) = 0;

	// Timeline marker
	virtual void AddTimelineMarker( const char *pchTemplateName, bool bPersistent, uint8 nColorR, uint8 nColorG, uint8 nColorB ) = 0;
	virtual void RemoveTimelineMarker() = 0;

	// Regions 
	virtual uint32 AddRegion( const char *pchElementName, const char *pchTimelineDataSection, const SteamTVRegion_t *pSteamTVRegion, ESteamTVRegionBehavior eSteamTVRegionBehavior ) = 0;
	virtual void RemoveRegion( uint32 unRegionHandle ) = 0;
};

#define STEAMTV_INTERFACE_VERSION "STEAMTV_INTERFACE_V002"

// Global interface accessor
inline ISteamTV *SteamTV();
STEAM_DEFINE_USER_INTERFACE_ACCESSOR( ISteamTV *, SteamTV, STEAMTV_INTERFACE_VERSION );

STEAM_CALLBACK_BEGIN( BroadcastUploadStart_t, k_iSteamVideoCallbacks + 4 )
	STEAM_CALLBACK_MEMBER( 0, bool, m_bIsRTMP )
STEAM_CALLBACK_END(1)

STEAM_CALLBACK_BEGIN( BroadcastUploadStop_t, k_iSteamVideoCallbacks + 5 )
	STEAM_CALLBACK_MEMBER( 0, EBroadcastUploadResult, m_eResult )
STEAM_CALLBACK_END(1)

#pragma pack( pop )

#endif // ISTEAMVIDEO_H
