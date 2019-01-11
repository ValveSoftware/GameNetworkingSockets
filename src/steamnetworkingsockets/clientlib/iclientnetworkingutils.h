//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef ICLIENTNETWORKINGUTILS_H
#define ICLIENTNETWORKINGUTILS_H
#pragma once

#include <steam/isteamnetworkingutils.h>
#include "iclientnetworkingsockets.h" // for CLIENTNETWORKINGSOCKETS_OVERRIDE

/////////////////////////////////////////////////////////////////////////////
//
// IClientNetworkingUtils
//
// In Steam, this is a non-versioned interface used internally.  It only
// implements the latest version of ISteamNetworkingUtils, and we
// define adapters to convert users of old ISteamNetworkingUtils
// versions to be able to talk to this interface.
//
// Outside of Steam, this layer of version is not needed, and
// ISteamNetworkingUtils and IClientNetworkingUtils should
// be equivalent.  This layer shouldn't add any runtime cost in that case.
//
/////////////////////////////////////////////////////////////////////////////

class IClientNetworkingUtils
#ifndef STEAMNETWORKINGSOCKETS_STEAM
: public ISteamNetworkingUtils
#endif
{
	virtual SteamNetworkingMicroseconds GetLocalTimestamp() CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool CheckPingDataUpToDate( float flMaxAgeSeconds ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual float GetLocalPingLocation( SteamNetworkPingLocation_t &result ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual bool IsPingMeasurementInProgress() CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int EstimatePingTimeBetweenTwoLocations( const SteamNetworkPingLocation_t &location1, const SteamNetworkPingLocation_t &location2 ) CLIENTNETWORKINGSOCKETS_OVERRIDE  = 0;
	virtual int EstimatePingTimeFromLocalHost( const SteamNetworkPingLocation_t &remoteLocation ) CLIENTNETWORKINGSOCKETS_OVERRIDE  = 0;
	virtual void ConvertPingLocationToString( const SteamNetworkPingLocation_t &location, char *pszBuf, int cchBufSize ) CLIENTNETWORKINGSOCKETS_OVERRIDE  = 0;
	virtual bool ParsePingLocationString( const char *pszString, SteamNetworkPingLocation_t &result ) CLIENTNETWORKINGSOCKETS_OVERRIDE  = 0;
	virtual int GetPingToDataCenter( SteamNetworkingPOPID popID, SteamNetworkingPOPID *pViaRelayPoP ) CLIENTNETWORKINGSOCKETS_OVERRIDE  = 0;
	virtual int GetDirectPingToPOP( SteamNetworkingPOPID popID ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int GetPOPCount() CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
	virtual int GetPOPList( SteamNetworkingPOPID *list, int nListSz ) CLIENTNETWORKINGSOCKETS_OVERRIDE = 0;
};

#endif // ICLIENTNETWORKINGUTILS_H
