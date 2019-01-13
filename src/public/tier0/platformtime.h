//========= Copyright Valve Corporation, All rights reserved. ============//
#ifndef PLATFORMTIME_H
#define PLATFORMTIME_H
#pragma once

#include "minbase/minbase_identify.h"
#include "minbase/minbase_types.h"
#include "minbase/minbase_decls.h"

// SDR_PUBLIC - Stripped out all of the stuff we don't need
#define PLATFORMTIME_INTERFACE	extern "C"

PLATFORMTIME_INTERFACE uint64			Plat_RelativeTicks();	// Returns time in raw ticks since an arbitrary start point.
PLATFORMTIME_INTERFACE double			Plat_FloatTime();		// Relative ticks to seconds (double).
PLATFORMTIME_INTERFACE uint64			Plat_USTime();			// Relative ticks to microseconds

#endif /* PLATFORMTIME_H */
