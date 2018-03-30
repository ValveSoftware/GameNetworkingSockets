//========= Copyright © 1996-2004, Valve LLC, All rights reserved. ============
//
// Purpose:
//
// $NoKeywords: $
//=============================================================================

#ifndef STEAMID_H
#define STEAMID_H
#ifdef _WIN32
#pragma once
#endif


#include "steam/steamtypes.h" // int64 define

#ifndef INCLUDED_STEAM2_USERID_STRUCTS

// Old Steam2 ID stuff
typedef	unsigned short		SteamInstanceID_t;		// MUST be 16 bits
typedef uint64			SteamLocalUserID_t;		// MUST be 64 bits

// Old steam2 user ID structures
typedef struct	
{
	unsigned int	Low32bits;
	unsigned int	High32bits;
}	TSteamSplitLocalUserID;

typedef struct
{
	SteamInstanceID_t		m_SteamInstanceID;

	union
	{
		SteamLocalUserID_t		As64bits;
		TSteamSplitLocalUserID	Split;
	}						m_SteamLocalUserID;

} TSteamGlobalUserID;

#define INCLUDED_STEAM2_USERID_STRUCTS
#endif

#include "steam/steamclientpublic.h"	// must be after definitions of the above structures

#ifdef GENERICHASH_H
inline uint32 HashItem( const CSteamID &item )
{
	return HashItemAsBytes(item);
}
#endif

typedef void * SteamUserIDTicketValidationHandle_t;

struct Steam2WrapperTicket_s
{
	unsigned int		m_VersionID; // = 1
	TSteamGlobalUserID  m_UserID;
	unsigned int		m_unPublicIP;
	SteamUserIDTicketValidationHandle_t m_Handle; // 
};


#endif // STEAMID_H
