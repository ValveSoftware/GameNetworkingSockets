//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: Saves the current memdbg state and enables if currently disabled
//
// $NoKeywords: $
//=============================================================================//

#if defined( MEM_OVERRIDE_SAVE )
#error memdbg_save_off/memdbg_save_on cannot be included multiple times
#endif

#define MEM_OVERRIDE_SAVE

#if !defined( MEM_OVERRIDE_ON )
#define MEM_OVERRIDE_WAS_OFF
#include <tier0/memdbgon.h>
#endif

