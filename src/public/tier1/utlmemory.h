//===== Copyright (C) 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose: 
//
// $NoKeywords: $
//
// A growable memory class.
//===========================================================================//

#ifndef UTLMEMORY_H
#define UTLMEMORY_H

#ifdef _WIN32
#pragma once
#endif

#include "tier0/dbg.h"
#include <string.h>
#include "tier0/platform.h"
#include "tier0/memdbgon.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning (disable:4100) // 'identifier' : unreferenced formal parameter
#endif

//-----------------------------------------------------------------------------

#ifdef UTLMEMORY_TRACK
#define UTLMEMORY_TRACK_ALLOC()		MemAlloc_RegisterAllocation( "Sum of all UtlMemory", 0, m_nAllocationCount * sizeof(T), m_nAllocationCount * sizeof(T), 0 )
#define UTLMEMORY_TRACK_FREE()		if ( !m_pMemory ) ; else MemAlloc_RegisterDeallocation( "Sum of all UtlMemory", 0, m_nAllocationCount * sizeof(T), m_nAllocationCount * sizeof(T), 0 )
#else
#define UTLMEMORY_TRACK_ALLOC()		((void)0)
#define UTLMEMORY_TRACK_FREE()		((void)0)
#endif

//-----------------------------------------------------------------------------
// The CUtlMemory class:
// A growable memory class which doubles in size by default.
//-----------------------------------------------------------------------------

class CUtlMemoryBase
{
public:
	// constructor, destructor
	CUtlMemoryBase( int nSizeOfType, int nGrowSize = 0, int nInitSize = 0 );
	CUtlMemoryBase( int nSizeOfType, void* pMemory, int numElements );
	CUtlMemoryBase( int nSizeOfType, const void* pMemory, int numElements );
#ifdef VALVE_RVALUE_REFS
	CUtlMemoryBase( CUtlMemoryBase&& src );
#endif // VALVE_RVALUE_REFS
	~CUtlMemoryBase();

	// Can we use this index?
	bool IsIdxValid( int i ) const;

	// Attaches the buffer to external memory....
	void SetExternalBuffer( void * pMemory, int numElements );
	void SetExternalBuffer( const void * pMemory, int numElements );

	// Fast swap
	void Swap( CUtlMemoryBase &mem );
	void *Detach();

	// Switches the buffer from an external memory buffer to a reallocatable buffer
	// Will copy the current contents of the external buffer to the reallocatable buffer
	void ConvertToGrowableMemory( int nGrowSize );

	// Size
	int NumAllocated() const;
	int Count() const;

	int CubAllocated() const { return m_nAllocationCount * m_unSizeOfElements; }
	// Grows the memory, so that at least allocated + num elements are allocated
	void Grow( int num = 1 );

	// Makes sure we've got at least this much memory
	void EnsureCapacity( int num );

	// Memory deallocation
	void Purge();

	// Purge all but the given number of elements
	void Purge( int numElements, bool bRealloc = true );

	// is the memory externally allocated?
	bool IsExternallyAllocated() const;

	// is the memory read only?
	bool IsReadOnly() const;

	// Set the size by which the memory grows
	void SetGrowSize( int size );

protected:

	// Copy construction and assignment are not valid
	CUtlMemoryBase(const CUtlMemoryBase& rhs);
	const CUtlMemoryBase& operator=(const CUtlMemoryBase& rhs);

	enum
	{
		EXTERNAL_BUFFER_MARKER = -1,
		EXTERNAL_CONST_BUFFER_MARKER = -2,
	};

	uint32 m_unSizeOfElements;
	void * m_pMemory;
	int m_nAllocationCount;
	int m_nGrowSize;
};

template< class T >
class CUtlMemory : public CUtlMemoryBase
{
public:
	// constructor, destructor
	CUtlMemory( int nGrowSize = 0, int nInitSize = 0 );
	CUtlMemory( T* pMemory, int numElements );
	CUtlMemory( const T* pMemory, int numElements );
#ifdef VALVE_RVALUE_REFS
	CUtlMemory( CUtlMemory&& src );
#endif // VALVE_RVALUE_REFS

	// element access
	T& operator[]( int i );
	const T& operator[]( int i ) const;
	T& Element( int i );
	const T& Element( int i ) const;

	// Gets the base address (can change when adding elements!)
	T* Base();
	const T* Base() const;

private:
	
	// Copy construction and assignment are not valid
	CUtlMemory(const CUtlMemory& rhs);
	const CUtlMemory& operator=(const CUtlMemory& rhs);
};

//-----------------------------------------------------------------------------
// The CUtlMemoryFixed class:
// A fixed memory class
//-----------------------------------------------------------------------------

template< typename T, size_t SIZE >
class CUtlMemoryFixed
{
public:
	// constructor, destructor
	CUtlMemoryFixed( int nGrowSize = 0, int nInitSize = 0 )	{ Assert( nInitSize == 0 || nInitSize == SIZE ); 	}
	CUtlMemoryFixed( T* pMemory, int numElements )			{ Assert( 0 ); 										}

	// Can we use this index?
	bool IsIdxValid( int i ) const							{ return (i >= 0) && (i < SIZE); }

	// Gets the base address
	T* Base()												{ return (T*)(&m_Memory[0]); }
	const T* Base() const									{ return (const T*)(&m_Memory[0]); }

	// element access
	T& operator[]( int i )									{ Assert( IsIdxValid(i) ); return Base()[i];	}
	const T& operator[]( int i ) const						{ Assert( IsIdxValid(i) ); return Base()[i];	}
	T& Element( int i )										{ Assert( IsIdxValid(i) ); return Base()[i];	}
	const T& Element( int i ) const							{ Assert( IsIdxValid(i) ); return Base()[i];	}

	// Attaches the buffer to external memory....
	void SetExternalBuffer( T* pMemory, int numElements )	{ Assert( 0 ); }

	// Size
	int NumAllocated() const								{ return SIZE; }
	int Count() const										{ return SIZE; }

	// Grows the memory, so that at least allocated + num elements are allocated
	void Grow( int num = 1 )								{ Assert( 0 ); }

	// Makes sure we've got at least this much memory
	void EnsureCapacity( int num )							{ Assert( num <= SIZE ); }

	// Memory deallocation
	void Purge()											{}

	// Purge all but the given number of elements (NOT IMPLEMENTED IN CUtlMemoryFixed)
	void Purge( int numElements, bool bRealloc = true )		{ Assert( 0 ); }

	// is the memory externally allocated?
	bool IsExternallyAllocated() const						{ return false; }

	// Set the size by which the memory grows
	void SetGrowSize( int size )							{}

private:
	uint8 m_Memory[SIZE*sizeof(T)];
};

//-----------------------------------------------------------------------------
// constructor, destructor
//-----------------------------------------------------------------------------

template< class T >
inline CUtlMemory<T>::CUtlMemory( int nGrowSize, int nInitAllocationCount ) : CUtlMemoryBase( sizeof( T ), nGrowSize, nInitAllocationCount  )
{
	
}


template< class T >
inline CUtlMemory<T>::CUtlMemory( T* pMemory, int numElements ) : CUtlMemoryBase( sizeof( T ), (void*)pMemory, numElements )
{
}


template< class T >
inline CUtlMemory<T>::CUtlMemory( const T* pMemory, int numElements ) : CUtlMemoryBase( sizeof( T ), pMemory, numElements )
{
}


#ifdef VALVE_RVALUE_REFS
template< class T >
inline CUtlMemory<T>::CUtlMemory( CUtlMemory<T>&& src ) : CUtlMemoryBase( std::move( src ) )
{
	static_assert( sizeof( CUtlMemory<T> ) == sizeof( CUtlMemoryBase ), "Move constructor needs to be updated if there are inline members in CUtlMemory." );
}
#endif // VALVE_RVALUE_REFS

template< class T >
void SWAP( T &a, T &b )
{
#if VALVE_RVALUE_REFS
	// perform r-value reference moves, instead of value copies
	T tmp( std::move( a ) );
	a = std::move( b );
	b = std::move( tmp );
#else
	T tmp( a );
	a = b;
	b = tmp;
#endif
}


//-----------------------------------------------------------------------------
// element access
//-----------------------------------------------------------------------------
template< class T >
inline T& CUtlMemory<T>::operator[]( int i )
{
	DbgAssert( !IsReadOnly() );
	DbgAssert( IsIdxValid(i) );
	return ((T*)m_pMemory)[i];
}

template< class T >
inline const T& CUtlMemory<T>::operator[]( int i ) const
{
	DbgAssert( IsIdxValid(i) );
	return ((T*)m_pMemory)[i];
}

template< class T >
inline T& CUtlMemory<T>::Element( int i )
{
	DbgAssert( !IsReadOnly() );
	DbgAssert( IsIdxValid(i) );
	return ((T*)m_pMemory)[i];
}

template< class T >
inline const T& CUtlMemory<T>::Element( int i ) const
{
	DbgAssert( IsIdxValid(i) );
	return ((T*)m_pMemory)[i];
}


//-----------------------------------------------------------------------------
// Gets the base address (can change when adding elements!)
//-----------------------------------------------------------------------------
template< class T >
inline T* CUtlMemory<T>::Base()
{
	return (T*)m_pMemory;
}

template< class T >
inline const T *CUtlMemory<T>::Base() const
{
	return (const T*)m_pMemory;
}

#include "tier0/memdbgoff.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // UTLMEMORY_H
