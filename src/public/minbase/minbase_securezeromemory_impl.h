//======== Copyright 2013, Valve Corporation, All rights reserved. ============//
//
// Purpose: Implementation of SecureZeroMemory for platforms that don't have one.
//
// $NoKeywords: $
//
//=============================================================================//

#ifndef SECUREZEROMEMORY_H
#define SECUREZEROMEMORY_H
#pragma once

#if !defined(SecureZeroMemory)
inline void SecureZeroMemory( void* pMemory, size_t nBytes )
{
	// The intent of this routine is to avoid being optimized away
	// by a compiler that sees that the buffer is no longer used,
	// so cast to volatile to indicate we want the accesses to occur.
	volatile unsigned char *pMem = (volatile unsigned char *)pMemory;
	while ( nBytes > 0 )
	{
		*pMem++ = 0;
		nBytes--;
	}
}
#endif

#endif /* SECUREZEROMEMORY_H */
