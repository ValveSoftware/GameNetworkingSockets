//========= Copyright 1996-2010, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//

#include <tier1/utlmemory.h>

#include "tier0/memdbgon.h"

CUtlMemoryBase::CUtlMemoryBase( int nSizeOfType, int nGrowSize, int nInitAllocationCount ) : m_pMemory(0), 
m_nAllocationCount( nInitAllocationCount ), m_nGrowSize( nGrowSize ), m_unSizeOfElements( nSizeOfType )
{
	Assert( m_unSizeOfElements > 0 );
	Assert( nGrowSize >= 0 );
	if (m_nAllocationCount)
	{
		UTLMEMORY_TRACK_ALLOC();
		m_pMemory = PvAlloc( m_nAllocationCount * m_unSizeOfElements );
	}
}


CUtlMemoryBase::CUtlMemoryBase( int nSizeOfType, void * pMemory, int numElements ) : m_pMemory(pMemory),
m_nAllocationCount( numElements ), m_unSizeOfElements( nSizeOfType )
{
	Assert( m_unSizeOfElements > 0 );
	// Special marker indicating externally supplied modifyable memory
	m_nGrowSize = EXTERNAL_BUFFER_MARKER;
}


CUtlMemoryBase::CUtlMemoryBase( int nSizeOfType, const void * pMemory, int numElements ) : m_pMemory( (void*)pMemory ),
m_nAllocationCount( numElements ),  m_unSizeOfElements( nSizeOfType )
{
	Assert( m_unSizeOfElements > 0 );
	// Special marker indicating externally supplied modifyable memory
	m_nGrowSize = EXTERNAL_CONST_BUFFER_MARKER;
}

#ifdef VALVE_RVALUE_REFS
CUtlMemoryBase::CUtlMemoryBase( CUtlMemoryBase&& src )
{
	// Default init this so when we destruct src it doesn't do anything.
	m_nGrowSize = 0;
	m_pMemory = 0;
	m_nAllocationCount = 0;
	m_unSizeOfElements = src.m_unSizeOfElements;

	Swap( src );
}
#endif // VALVE_RVALUE_REFS


CUtlMemoryBase::~CUtlMemoryBase()
{
	Purge();
}


//-----------------------------------------------------------------------------
// Fast swap
//-----------------------------------------------------------------------------
void CUtlMemoryBase::Swap( CUtlMemoryBase &mem )
{
	// Shouldn't really be swapping if types didn't match, thus sizes should match
	Assert( m_unSizeOfElements == mem.m_unSizeOfElements );

	SWAP( m_nGrowSize, mem.m_nGrowSize );
	SWAP( m_pMemory, mem.m_pMemory );
	SWAP( m_nAllocationCount, mem.m_nAllocationCount );
	SWAP( m_unSizeOfElements, mem.m_unSizeOfElements );
}


//-----------------------------------------------------------------------------
// Fast swap
//-----------------------------------------------------------------------------
void *CUtlMemoryBase::Detach()
{
	m_nAllocationCount = 0;
	void *pMemory = m_pMemory;
	m_pMemory = NULL;
	return pMemory;
}


//-----------------------------------------------------------------------------
// Switches the buffer from an external memory buffer to a reallocatable buffer
//-----------------------------------------------------------------------------
void CUtlMemoryBase::ConvertToGrowableMemory( int nGrowSize )
{
	if ( !IsExternallyAllocated() )
		return;

	m_nGrowSize = nGrowSize;
	if (m_nAllocationCount)
	{
		UTLMEMORY_TRACK_ALLOC();
		MEM_ALLOC_CREDIT_CLASS();

		int nNumBytes = m_nAllocationCount * m_unSizeOfElements;
		void *pMemory = PvAlloc( nNumBytes );
		memcpy( pMemory, m_pMemory, nNumBytes ); 
		m_pMemory = pMemory;
	}
	else
	{
		m_pMemory = NULL;
	}
}


//-----------------------------------------------------------------------------
// Attaches the buffer to external memory....
//-----------------------------------------------------------------------------
void CUtlMemoryBase::SetExternalBuffer( void * pMemory, int numElements )
{
	// Blow away any existing allocated memory
	Purge();

	m_pMemory = pMemory;
	m_nAllocationCount = numElements;

	// Indicate that we don't own the memory
	m_nGrowSize = EXTERNAL_BUFFER_MARKER;
}


void CUtlMemoryBase::SetExternalBuffer( const void* pMemory, int numElements )
{
	// Blow away any existing allocated memory
	Purge();

	m_pMemory = const_cast<void*>( pMemory );
	m_nAllocationCount = numElements;

	// Indicate that we don't own the memory
	m_nGrowSize = EXTERNAL_CONST_BUFFER_MARKER;
}


//-----------------------------------------------------------------------------
// is the memory externally allocated?
//-----------------------------------------------------------------------------
bool CUtlMemoryBase::IsExternallyAllocated() const
{
	return (m_nGrowSize < 0);
}


//-----------------------------------------------------------------------------
// is the memory read only?
//-----------------------------------------------------------------------------
bool CUtlMemoryBase::IsReadOnly() const
{
	return (m_nGrowSize == EXTERNAL_CONST_BUFFER_MARKER);
}


void CUtlMemoryBase::SetGrowSize( int nSize )
{
	Assert( !IsExternallyAllocated() );
	Assert( nSize >= 0 );
	m_nGrowSize = nSize;
}


//-----------------------------------------------------------------------------
// Size
//-----------------------------------------------------------------------------
int CUtlMemoryBase::NumAllocated() const
{
	return m_nAllocationCount;
}


int CUtlMemoryBase::Count() const
{
	return m_nAllocationCount;
}


//-----------------------------------------------------------------------------
// Is element index valid?
//-----------------------------------------------------------------------------
bool CUtlMemoryBase::IsIdxValid( int i ) const
{
	return (i >= 0) && (i < m_nAllocationCount);
}


//-----------------------------------------------------------------------------
// Grows the memory
//-----------------------------------------------------------------------------
int UtlMemory_CalcNewAllocationCount( int nAllocationCount, int nGrowSize, int nNewSize, int nBytesItem )
{
	if ( nGrowSize )
	{ 
		nAllocationCount = ((1 + ((nNewSize - 1) / nGrowSize)) * nGrowSize);
	}
	else 
	{
		if ( !nAllocationCount )
		{
			if ( nBytesItem > 0 )
			{
				// Compute an allocation which is at least as big as a cache line...
				nAllocationCount = (31 + nBytesItem) / nBytesItem;
			}
			else
			{
				// Should be impossible, but if hit try to grow an amount that may be large
				// enough for most cases and thus avoid both divide by zero above as well as
				// likely memory corruption afterwards.
				AssertMsg1( false, "nBytesItem is %d in UtlMemory_CalcNewAllocationCount", nBytesItem );
				nAllocationCount = 256;
			}
		}

		// Cap growth to avoid high-end doubling insanity (1 GB -> 2 GB -> overflow)
		int nMaxGrowStep = Max( 1, 256*1024*1024 / ( nBytesItem > 0 ? nBytesItem : 1 ) );
		while (nAllocationCount < nNewSize)
		{
#ifndef _XBOX
			// Grow by doubling, but at most 256 MB at a time.
			nAllocationCount += Min( nAllocationCount, nMaxGrowStep );
#else
			int nNewAllocationCount = ( nAllocationCount * 9) / 8; // 12.5 %
			if ( nNewAllocationCount > nAllocationCount )
				nAllocationCount = nNewAllocationCount;
			else
				nAllocationCount *= 2;
#endif
		}
	}

	return nAllocationCount;
}


void CUtlMemoryBase::Grow( int num )
{
	Assert( num > 0 );

	if ( IsExternallyAllocated() )
	{
		// Can't grow a buffer whose memory was externally allocated 
		Assert(0);
		return;
	}

	// Make sure we have at least numallocated + num allocations.
	// Use the grow rules specified for this memory (in m_nGrowSize)
	int nAllocationRequested = m_nAllocationCount + num;

	UTLMEMORY_TRACK_FREE();

	m_nAllocationCount = UtlMemory_CalcNewAllocationCount( m_nAllocationCount, m_nGrowSize, nAllocationRequested, m_unSizeOfElements );

	UTLMEMORY_TRACK_ALLOC();
	if (m_pMemory)
	{
		m_pMemory = PvRealloc( m_pMemory, m_nAllocationCount * m_unSizeOfElements );
	}
	else
	{
		m_pMemory = PvAlloc( m_nAllocationCount * m_unSizeOfElements );
	}
}


//-----------------------------------------------------------------------------
// Makes sure we've got at least this much memory
//-----------------------------------------------------------------------------
void CUtlMemoryBase::EnsureCapacity( int num )
{
	if (m_nAllocationCount >= num)
		return;

	if ( IsExternallyAllocated() )
	{
		// Can't grow a buffer whose memory was externally allocated 
		Assert(0);
		return;
	}

	UTLMEMORY_TRACK_FREE();

	m_nAllocationCount = num;

	UTLMEMORY_TRACK_ALLOC();

	if (m_pMemory)
	{
		m_pMemory = PvRealloc( m_pMemory, m_nAllocationCount * m_unSizeOfElements );
	}
	else
	{
		m_pMemory = PvAlloc( m_nAllocationCount * m_unSizeOfElements );
	}
}


//-----------------------------------------------------------------------------
// Memory deallocation
//-----------------------------------------------------------------------------
void CUtlMemoryBase::Purge()
{
	if ( !IsExternallyAllocated() )
	{
		if (m_pMemory)
		{
			UTLMEMORY_TRACK_FREE();
			FreePv( m_pMemory );
			m_pMemory = 0;
		}
		m_nAllocationCount = 0;
	}
}


void CUtlMemoryBase::Purge( int numElements, bool bRealloc )
{
	Assert( numElements >= 0 );

	if( numElements > m_nAllocationCount )
	{
		// Ensure this isn't a grow request in disguise.
		Assert( numElements <= m_nAllocationCount );
		return;
	}

	// If we have zero elements, simply do a purge:
	if( numElements == 0 )
	{
		Purge();
		return;
	}

	if ( IsExternallyAllocated() )
	{
		// Can't shrink a buffer whose memory was externally allocated, fail silently like purge 
		return;
	}

	// If the number of elements is the same as the allocation count, we are done.
	if( numElements == m_nAllocationCount )
	{
		return;
	}

	if( !m_pMemory )
	{
		// Allocation count is non zero, but memory is null.
		Assert( m_pMemory );
		return;
	}

	if ( bRealloc )
	{
		UTLMEMORY_TRACK_FREE();

		m_nAllocationCount = numElements;

		UTLMEMORY_TRACK_ALLOC();

		// Allocation count > 0, shrink it down.
		MEM_ALLOC_CREDIT_CLASS();
		m_pMemory = PvRealloc( m_pMemory, m_nAllocationCount * m_unSizeOfElements );
	}
	else
	{
		// Some of the tracking may be wrong as we are changing the size but are not reallocating.
		m_nAllocationCount = numElements;
	}
}


//-----------------------------------------------------------------------------
// Data and memory validation
//-----------------------------------------------------------------------------
#ifdef DBGFLAG_VALIDATE
void CUtlMemoryBase::Validate( CValidator &validator, const char *pchName )
{

#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), this, pchName );
#else
	validator.Push( typeid(*this).name(), this, pchName );
#endif

	if ( NULL != m_pMemory )
		validator.ClaimMemory( m_pMemory );

	validator.Pop();
}
#endif // DBGFLAG_VALIDATE
