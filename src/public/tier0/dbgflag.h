//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose:	This file sets all of our debugging flags.  It should be 
//			called before all other header files.
//
// $NoKeywords: $
//=============================================================================//

#ifndef DBGFLAG_H
#define DBGFLAG_H
#pragma once


// Here are all the flags we support:
// DBGFLAG_MEMORY:			Enables our memory debugging system, which overrides malloc & free
// DBGFLAG_MEMORY_NEWDEL:	Enables new / delete tracking for memory debug system.  Requires DBGFLAG_MEMORY to be enabled.
// DBGFLAG_VALIDATE:		Enables our recursive validation system for checking integrity and memory leaks
// DBGFLAG_ASSERT:			Turns Assert on or off (when off, it isn't compiled at all)
// DBGFLAG_ASSERTFATAL:		Turns AssertFatal on or off (when off, it isn't compiled at all)
// DBGFLAG_ASSERTDLG:		Turns assert dialogs on or off and debug breaks on or off when not under the debugger.
//								(Dialogs will always be on when process is being debugged.)

#undef DBGFLAG_MEMORY
#undef DBGFLAG_MEMORY_NEWDEL
#undef DBGFLAG_VALIDATE
#undef DBGFLAG_ASSERT
#undef DBGFLAG_ASSERTFATAL
#undef DBGFLAG_ASSERTDLG

#if defined(_DEBUG) && !defined(NO_MALLOC_OVERRIDE)

//-----------------------------------------------------------------------------
// Default flags for debug builds
//-----------------------------------------------------------------------------

#define DBGFLAG_MEMORY
#define DBGFLAG_MEMORY_NEWDEL	
#ifdef STEAM
#define DBGFLAG_VALIDATE
#define DBGFLAG_MINIDUMPONASSERT
#endif
#define DBGFLAG_ASSERT
#define DBGFLAG_ASSERTFATAL
#define DBGFLAG_ASSERTDLG


#else // !_DEBUG

//-----------------------------------------------------------------------------
// Default flags for release builds
//-----------------------------------------------------------------------------

#undef DBGFLAG_ASSERTDLG		// no assert dialogs in release.  (NOTE: Dialogs will always be on when process is being debugged.)


#ifdef STEAM 
#if ( !defined(_PS3) || defined(_DEBUG) )
#define DBGFLAG_VALIDATE		// validate is enabled in release, but only for counting memory, not for validating it
#endif
#endif // STEAM

// Asserts are off for release on PS3
#if ( !defined(_PS3) || defined(_DEBUG) )
#define DBGFLAG_ASSERT			// asserts are enabled in release (but no dialog; they just spew to console)
#define DBGFLAG_MINIDUMPONASSERT
#define DBGFLAG_ASSERTFATAL		// fatal asserts are enabled in relase (no dialog; spew to console then app exit)
#endif


#endif // _DEBUG

// Make it possible to disable validation in debug builds
#ifdef DISABLE_DBGFLAG_VALIDATE
#undef DBGFLAG_VALIDATE
#endif

#endif // DBGFLAG_H
