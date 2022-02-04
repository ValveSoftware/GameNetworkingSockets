//========= Copyright 1996-2022, Valve LLC, All rights reserved. ============

#ifndef STEAMID_H
#define STEAMID_H
#pragma once

#include "steam/steamclientpublic.h"

#ifdef GENERICHASH_H
inline uint32 HashItem( const CSteamID &item )
{
	return HashItemAsBytes(item);
}
#endif

#endif // _H
