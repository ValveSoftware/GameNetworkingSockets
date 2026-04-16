//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: Restores memdbg state from a previous include of memdbg_save_off/memdbg_save_on
//
// $NoKeywords: $
//=============================================================================//

#if !defined( MEM_OVERRIDE_SAVE )
#error memdbg_restore included without previous include of memdbg_save_off/memdbg_save_on
#endif

#if defined( MEM_OVERRIDE_WAS_ON )
#include <tier0/memdbgon.h>
#elif defined( MEM_OVERRIDE_WAS_OFF )
#include <tier0/memdbgoff.h>
#endif

#undef MEM_OVERRIDE_SAVE
#undef MEM_OVERRIDE_WAS_ON
#undef MEM_OVERRIDE_WAS_OFF

