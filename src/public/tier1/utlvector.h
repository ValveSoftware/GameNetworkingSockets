//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
// $NoKeywords: $
//
// A growable array class that maintains a free list and keeps elements
// in the same location
//=============================================================================//

#ifndef UTLVECTOR_H
#define UTLVECTOR_H

#ifdef _WIN32
#pragma once
#endif


#include <string.h>

#include "tier0/memdbgoff.h"
#include <algorithm>
#include <functional>
#include <type_traits>
#include "tier0/memdbgon.h"

#include "tier0/platform.h"
#include "tier0/dbg.h"
#include "tier1/utlmemory.h"
#include "vstdlib/strtools.h"

#define FOR_EACH_VEC( vecName, iteratorName ) \
	for ( int iteratorName = 0; iteratorName < (vecName).Count(); iteratorName++ )
#define FOR_EACH_VEC_BACK( vecName, iteratorName ) \
	for ( int iteratorName = (vecName).Count()-1; iteratorName >= 0; iteratorName-- )

//-----------------------------------------------------------------------------
// The CUtlVector class:
// A growable array class which doubles in size by default.
// It will always keep all elements consecutive in memory, and may move the
// elements around in memory (via a PvRealloc) when elements are inserted or
// removed. Clients should therefore refer to the elements of the vector
// by index (they should *never* maintain pointers to elements in the vector).
//-----------------------------------------------------------------------------
template< class T, class A = CUtlMemory<T> >
class CUtlVector
{
	typedef A CAllocator;
public:
	typedef T ElemType_t;
	typedef T* iterator;
	typedef const T* const_iterator;

	// constructor, destructor
	explicit CUtlVector( int growSize = 0, int initSize = 0 );
#ifdef VALVE_RVALUE_REFS
	CUtlVector( CUtlVector&& src );
#endif // VALVE_RVALUE_REFS
	CUtlVector( T* pMemory, int allocationCount, int numElements = 0 );
#ifdef VALVE_INITIALIZER_LIST_SUPPORT
	CUtlVector( std::initializer_list<T> initializerList );
#endif // VALVE_INITIALIZER_LIST_SUPPORT
	~CUtlVector();

	// Copy the array.
	CUtlVector<T, A>& operator=( const CUtlVector<T, A> &other );

	// element access
	T& operator[]( int i );
	const T& operator[]( int i ) const;
	T& Element( int i );
	const T& Element( int i ) const;
	T& Head();
	const T& Head() const;
	T& Tail();
	const T& Tail() const;

	// STL compatible member functions. These allow easier use of std::sort
	// and they are forward compatible with the C++ 11 range-based for loops.
	iterator begin()						{ return Base(); }
	const_iterator begin() const			{ return Base(); }
	iterator end()							{ return Base() + Count(); }
	const_iterator end() const				{ return Base() + Count(); }

	// Gets the base address (can change when adding elements!)
	T* Base()								{ return m_Memory.Base(); }
	const T* Base() const					{ return m_Memory.Base(); }

	// Returns the number of elements in the vector
	int Count() const;
	bool IsEmpty() const { return (Count() == 0); }

	int CubAllocated() { return m_Memory.CubAllocated(); }
	// Is element index valid?
	bool IsValidIndex( int i ) const;
	static int InvalidIndex();

	// Adds an element, uses default constructor
	int AddToHead();
	int AddToTail();
	T *AddToTailGetPtr();
	int InsertBefore( int elem );
	int InsertAfter( int elem );

	// Adds an element, uses copy constructor
	int AddToHead( const T& src );
	int AddToTail( const T& src );
	int InsertBefore( int elem, const T& src );
	int InsertAfter( int elem, const T& src );
#ifdef VALVE_RVALUE_REFS
	int AddToTail( T&& src );
#endif

	// Adds multiple elements, uses default constructor
	int AddMultipleToHead( int num );
	int AddMultipleToTail( int num, const T *pToCopy=NULL );
	int InsertMultipleBefore( int elem, int num, const T *pToCopy=NULL );	// If pToCopy is set, then it's an array of length 'num' and
	int InsertMultipleAfter( int elem, int num, const T *pToCopy=NULL );

	// Matches desired element count by removing or adding at tail
	void SetCount( int count );

	// Calls SetSize and copies each element.
	void CopyArray( const T *pArray, int size );

	// Fast swap
	void Swap( CUtlVector< T, A > &vec );

	// Add the specified array to the tail.
	int AddVectorToTail( CUtlVector<T, A> const &src );

	// Finds an element (element needs operator== defined)
	int Find( const T& src ) const;

#ifdef VALVE_RVALUE_REFS
	template < typename TMatchFunc >
	int FindMatch( TMatchFunc&& func ) const;
#endif // VALVE_RVALUE_REFS

	bool HasElement( const T& src ) const;

	// Makes sure we have enough memory allocated to store a requested # of elements
	void EnsureCapacity( int num );

	// Makes sure we have at least this many elements
	void EnsureCount( int num );

	// Element removal
	void FastRemove( int elem );	// doesn't preserve order
	void Remove( int elem );		// preserves order, shifts elements
	bool FindAndRemove( const T& src );	// removes first occurrence of src, preserves order, shifts elements
	bool FindAndFastRemove( const T& src );	// removes first occurrence of src, doesn't preserve order
	void RemoveMultiple( int elem, int num );	// preserves order, shifts elements
	void RemoveMultipleFromTail( int num );
	void RemoveAll();				// doesn't deallocate memory

	// Memory deallocation
	void Purge();

	// Purges the list and calls delete on each element in it.
	void PurgeAndDeleteElements();

	// Compacts the vector to the number of elements actually in use
	void Compact();

	// Set the size by which it grows when it needs to allocate more memory.
	void SetGrowSize( int size )			{ m_Memory.SetGrowSize( size ); }

	int NumAllocated() const { return m_Memory.NumAllocated(); }	// Only use this if you really know what you're doing!
	
	// Reverses the order of elements via swaps
	void Reverse();

	// Finds an element within the list using a binary search
	// You must sort the list before using these or your results will be wrong
	int		SortedFind( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ), void *pLessContext ) const;
	int		SortedFind( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2 ) ) const;
	int		SortedFindFirst( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ), void *pLessContext ) const;
	int		SortedFindLessOrEqual( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ), void *pLessContext, int start, int end ) const;
	int		SortedFindLessOrEqual( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ), void *pLessContext ) const;
	int		SortedFindLessOrEqual( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2 ), int start, int end ) const;
	int		SortedFindLessOrEqual( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2 ) ) const;
	// Finds an element within the list using a binary search and a predicate
	// comparerPredicate has to implement: int Compare( const T &left ) const;
	template <typename T2>
	int		SortedFindIf( const T2 &comparerPredicate ) const;
	template <typename T2>
	int		SortedFindFirst( const T2 &comparerPredicate ) const;
	template <typename T2>
	const T&	FindElementIf( const T2 &comparerPredicate, const T& defaultParam ) const;

	int		SortedInsert( const T& src, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ), void *pLessContext );
	int		SortedInsert( const T& src, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2 ) );

	/// sort using std:: with a predicate. e.g. [] -> bool ( T &a, T &b ) { return a < b; }
	template <class F> void SortPredicate( F &&predicate );

	/// Sort using the default less predicate
	void Sort() { SortPredicate( std::less<T>{} ); };

	// WARNING: The less func for these Sort functions expects -1, 0 for equal, or 1. If you pass only true/false back, you won't get correct sorting.
	void Sort( int (__cdecl *pfnCompare)(const T *, const T *) );
	void Sort_s( void *context, int (__cdecl *pfnCompare)(void *, const T *, const T *) );

	// These sorts expect true/false
	void Sort( bool( __cdecl *pfnLessFunc )(const T& src1, const T& src2) );
	void Sort( bool( __cdecl *pfnLessFunc )(const T& src1, const T& src2, void *pCtx), void *pLessContext );
	void Sort_s( void *context, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ) );

#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName );					// Validate our internal structures
	void ValidateSelfAndElements( CValidator &validator, const char *pchName );		// Validate our internal structures
#endif // DBGFLAG_VALIDATE

protected:
	// Can't copy this unless we explicitly do it!
	CUtlVector( CUtlVector const& );

	// Grows the vector
	void GrowVector( int num = 1 );

	// Shifts elements....
	void ShiftElementsRight( int elem, int num = 1 );
	void ShiftElementsLeft( int elem, int num = 1 );

	CAllocator m_Memory;
	int m_Size;
};

//-----------------------------------------------------------------------------
// The CUtlVectorFixed class:
// A array class with a fixed allocation scheme
//-----------------------------------------------------------------------------

template< class T, size_t MAX_SIZE >
class CUtlVectorFixed : public CUtlVector< T, CUtlMemoryFixed<T, MAX_SIZE > >
{
	typedef CUtlVector< T, CUtlMemoryFixed<T, MAX_SIZE > > BaseClass;
public:

	// constructor, destructor
	CUtlVectorFixed( int growSize = 0, int initSize = 0 ) : BaseClass( growSize, initSize ) {}
	CUtlVectorFixed( T* pMemory, int numElements ) : BaseClass( pMemory, numElements ) {}
};

//-----------------------------------------------------------------------------
// The CCopyableUtlVectorFixed class:
// A array class that allows copy construction (so you can nest a CUtlVector inside of another one of our containers)
//  WARNING - this class lets you copy construct which can be an expensive operation if you don't carefully control when it happens
// Only use this when nesting a CUtlVector() inside of another one of our container classes (i.e a CUtlMap)
//-----------------------------------------------------------------------------
template< class T, size_t MAX_SIZE >
class CCopyableUtlVectorFixed : public CUtlVectorFixed< T, MAX_SIZE >
{
	typedef CUtlVectorFixed< T, MAX_SIZE > BaseClass;
public:
	CCopyableUtlVectorFixed( int growSize = 0, int initSize = 0 ) : BaseClass( growSize, initSize ) {}
	CCopyableUtlVectorFixed( T* pMemory, int numElements ) : BaseClass( pMemory, numElements ) {}
	CCopyableUtlVectorFixed( CCopyableUtlVectorFixed const& vec ) : BaseClass(0,0) { this->CopyArray( vec.Base(), vec.Count() ); }
};


//-----------------------------------------------------------------------------
// The CCopyableUtlVector class:
// A array class that allows copy construction (so you can nest a CUtlVector inside of another one of our containers)
//  WARNING - this class lets you copy construct which can be an expensive operation if you don't carefully control when it happens
// Only use this when nesting a CUtlVector() inside of another one of our container classes (i.e a CUtlMap)
//-----------------------------------------------------------------------------
template< class T, class A = CUtlMemory<T> >
class CCopyableUtlVector : public CUtlVector< T, A >
{
	typedef CUtlVector< T, A > BaseClass;
public:
	CCopyableUtlVector( int growSize = 0, int initSize = 0 ) : BaseClass( growSize, initSize ) {}
	CCopyableUtlVector( T* pMemory, int numElements ) : BaseClass( pMemory, numElements ) {}
    CCopyableUtlVector( CCopyableUtlVector const& vec ) : BaseClass(0,0) { this->CopyArray( vec.Base(), vec.Count() ); }
	CCopyableUtlVector( CUtlVector<T,A> const& vec ) : BaseClass( 0, 0 ) { this->CopyArray( vec.Base(), vec.Count() ); }
};

//-----------------------------------------------------------------------------
// constructor, destructor
//-----------------------------------------------------------------------------
template< typename T, class A >
inline CUtlVector<T, A>::CUtlVector( int growSize, int initSize )	:
	m_Memory(growSize, initSize), m_Size(0)
{
}

#ifdef VALVE_RVALUE_REFS
template< typename T, class A >
inline CUtlVector<T, A>::CUtlVector( CUtlVector<T, A>&& src )
	: m_Size( 0 )
{
	Swap( src );
}
#endif // VALVE_RVALUE_REFS

template< typename T, class A >
inline CUtlVector<T, A>::CUtlVector( T* pMemory, int allocationCount, int numElements )	:
	m_Memory(pMemory, allocationCount), m_Size(numElements)
{
}

#ifdef VALVE_INITIALIZER_LIST_SUPPORT
template< typename T, class A >
inline CUtlVector<T, A>::CUtlVector( std::initializer_list<T> initializerList ) :
	m_Size(0)
{
	EnsureCapacity( static_cast<int>( initializerList.size() ) );

	for ( const auto& v : initializerList )
		AddToTail( v );
}
#endif // VALVE_INITIALIZER_LIST_SUPPORT

template< typename T, class A >
inline CUtlVector<T, A>::~CUtlVector()
{
	Purge();
}

template< typename T, class A >
inline CUtlVector<T, A>& CUtlVector<T, A>::operator=( const CUtlVector<T, A> &other )
{
	this->CopyArray( other.Base(), other.Count() );
	return *this;
}


//-----------------------------------------------------------------------------
// element access
//-----------------------------------------------------------------------------
template< typename T, class A >
inline T& CUtlVector<T, A>::operator[]( int i )
{
	DbgAssert( IsValidIndex(i) );
	return Base()[i];
}

template< typename T, class A >
inline const T& CUtlVector<T, A>::operator[]( int i ) const
{
	DbgAssert( IsValidIndex(i) );
	return Base()[i];
}

template< typename T, class A >
inline T& CUtlVector<T, A>::Element( int i )
{
	DbgAssert( IsValidIndex(i) );
	return Base()[i];
}

template< typename T, class A >
inline const T& CUtlVector<T, A>::Element( int i ) const
{
	DbgAssert( IsValidIndex(i) );
	return Base()[i];
}

template< typename T, class A >
inline T& CUtlVector<T, A>::Head()
{
	DbgAssert( m_Size > 0 );
	return m_Memory[0];
}

template< typename T, class A >
inline const T& CUtlVector<T, A>::Head() const
{
	DbgAssert( m_Size > 0 );
	return m_Memory[0];
}

template< typename T, class A >
inline T& CUtlVector<T, A>::Tail()
{
	DbgAssert( m_Size > 0 );
	return m_Memory[m_Size - 1];
}

template< typename T, class A >
inline const T& CUtlVector<T, A>::Tail() const
{
	DbgAssert( m_Size > 0 );
	return m_Memory[m_Size - 1];
}

//-----------------------------------------------------------------------------
// Count
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::Count() const
{
	return m_Size;
}


//-----------------------------------------------------------------------------
// Is element index valid?
//-----------------------------------------------------------------------------
template< typename T, class A >
inline bool CUtlVector<T, A>::IsValidIndex( int i ) const
{
	return (i >= 0) && (i < m_Size);
}


//-----------------------------------------------------------------------------
// Returns in invalid index
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::InvalidIndex()
{
	return -1;
}


//-----------------------------------------------------------------------------
// Grows the vector
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::GrowVector( int num )
{
	if (m_Size + num > m_Memory.NumAllocated())
	{
		m_Memory.Grow( m_Size + num - m_Memory.NumAllocated() );
	}

	m_Size += num;
}


//-----------------------------------------------------------------------------
// Reverses the order of elements
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::Reverse()
{
	T* pBase = Base();
	int iRight = m_Size - 1;
	for( int iLeft = 0; iLeft < m_Size / 2; iLeft++ )
	{
		SWAP( pBase[iLeft], pBase[iRight] );
		iRight--;
	}
}


//-----------------------------------------------------------------------------
// finds a particular element
// You must sort the list before using or your results will be wrong
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::SortedFind( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ), void *pLessContext ) const
{
	int start = 0, stop = Count() - 1;
	while (start <= stop)
	{
		int mid = (start + stop) >> 1;
		if ( pfnLessFunc( Base()[mid], search, pLessContext ) )
		{
			start = mid + 1;
		}
		else if ( pfnLessFunc( search, Base()[mid], pLessContext ) )
		{
			stop = mid - 1;
		}
		else
		{
			return mid;
		}
	}
	return InvalidIndex();
}


//-----------------------------------------------------------------------------
// finds a particular element
// You must sort the list before using or your results will be wrong
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::SortedFind( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2 ) ) const
{
	int start = 0, stop = Count() - 1;
	while (start <= stop)
	{
		int mid = (start + stop) >> 1;
		if ( pfnLessFunc( Base()[mid], search ) )
		{
			start = mid + 1;
		}
		else if ( pfnLessFunc( search, Base()[mid] ) )
		{
			stop = mid - 1;
		}
		else
		{
			return mid;
		}
	}
	return InvalidIndex();
}



//-----------------------------------------------------------------------------
// finds the FIRST matching element ( assumes dupes )
// You must sort the list before using or your results will be wrong
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::SortedFindFirst( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ), void *pLessContext ) const
{
	int start = 0, stop = Count() - 1;
	while (start <= stop)
	{
		int mid = (start + stop) >> 1;
		if ( pfnLessFunc( Base()[mid], search, pLessContext ) )
		{
			start = mid + 1;
		}
		else if ( pfnLessFunc( search, Base()[mid], pLessContext ) )
		{
			stop = mid - 1;
		}
		else
		{
			// found a match - but we want the first one - keep looking
			if ( start == mid )
				return mid;
			stop = mid;
		}
	}
	return InvalidIndex();
}


//-----------------------------------------------------------------------------
// Implementation of upper_bound(). Finds the element with the highest index
// that is less than or equal to what you're looking for.
// You must sort the list before using or your results will be wrong
// This takes a range in the vector to search, end is inclusive (Count() - 1)
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::SortedFindLessOrEqual( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ), void *pLessContext, int start, int stop ) const
{
	while (start <= stop)
	{
		int mid = (start + stop) >> 1;
		if ( pfnLessFunc( Base()[mid], search, pLessContext ) )
		{
			start = mid + 1;
		}
		else if ( pfnLessFunc( search, Base()[mid], pLessContext ) )
		{
			stop = mid - 1;
		}
		else
		{
			// found a match - but we want the last one - keep looking
			if( stop == mid )
				return mid;

			if( mid == start )
			{
				// This means we have just start and stop elements left to check
				if( stop > mid && pfnLessFunc( search, Base()[mid + 1], pLessContext ) )
					return mid;
				else
					return mid + 1;
			}
			else
			{
				start = mid;
			}
		}
	}
	return stop;
}

//-----------------------------------------------------------------------------
// Implementation of upper_bound(). Finds the element with the highest index
// that is less than or equal to what you're looking for.
// You must sort the list before using or your results will be wrong
// Searches the entire vector
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::SortedFindLessOrEqual( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ), void *pLessContext ) const
{
	return SortedFindLessOrEqual( search, pfnLessFunc, pLessContext, 0, Count() - 1 );
}

template< typename T, class A >
inline int CUtlVector<T, A>::SortedInsert( const T& src, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ), void *pLessContext )
{
	int pos = SortedFindLessOrEqual( src, pfnLessFunc, pLessContext ) + 1;
	GrowVector();
	ShiftElementsRight(pos);
	Construct<T>( &Element(pos), src );
	return pos;
}

template< typename T, class A >
template <class F>
inline void CUtlVector<T, A>::SortPredicate( F &&predicate )
{
	std::sort( begin(), end(), predicate );
}


//-----------------------------------------------------------------------------
// sorted find, with no context pointer
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::SortedFindLessOrEqual( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2 ), int start, int stop ) const
{
	while (start <= stop)
	{
		int mid = (start + stop) >> 1;
		if ( pfnLessFunc( Base()[mid], search ) )
		{
			start = mid + 1;
		}
		else if ( pfnLessFunc( search, Base()[mid] ) )
		{
			stop = mid - 1;
		}
		else
		{
			// found a match - but we want the last one - keep looking
			if( stop == mid )
				return mid;

			if( mid == start )
			{
				// This means we have just start and stop elements left to check
				if( stop > mid && pfnLessFunc( search, Base()[mid + 1] ) )
					return mid;
				else
					return mid + 1;
			}
			else
			{
				start = mid;
			}
		}
	}
	return stop;
}

//-----------------------------------------------------------------------------
// sorted find, with no context pointer
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::SortedFindLessOrEqual( const T& search, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2 ) ) const
{
	return SortedFindLessOrEqual( search, pfnLessFunc, 0, Count() - 1 );
}


//-----------------------------------------------------------------------------
// finds the FIRST matching element ( assumes dupes )
// You must sort the list before using or your results will be wrong
// comparerPredicate has to implement: int Compare( const T &src ) const;
//-----------------------------------------------------------------------------
template< typename T, class A >
template <typename T2>
inline int CUtlVector<T, A>::SortedFindFirst( const T2 &comparerPredicate ) const
{
	int start = 0, stop = Count() - 1;
	while ( start <= stop )
	{
		int mid = ( start + stop ) >> 1;
		int nResult = comparerPredicate.Compare( Base()[mid] );
		if ( nResult < 0 )
		{
			start = mid + 1;
		}
		else if ( nResult > 0 )
		{
			stop = mid - 1;
		}
		else
		{
			// found a match - but we want the first one - keep looking
			if ( start == mid )
				return mid;
			stop = mid;
		}
	}
	return InvalidIndex();
}


//-----------------------------------------------------------------------------
// sorted find with a comparer predicate
// comparerPredicate has to implement: int Compare( const T &src ) const;
//-----------------------------------------------------------------------------
template< typename T, class A >
template <typename T2>
inline int CUtlVector<T, A>::SortedFindIf( const T2 &comparerPredicate ) const
{
	int start = 0, stop = Count() - 1;
	while (start <= stop)
	{
		int mid = (start + stop) >> 1;
		int nResult = comparerPredicate.Compare( Base()[mid] );
		if ( nResult < 0 )
		{
			start = mid + 1;
		}
		else if ( nResult > 0 )
		{
			stop = mid - 1;
		}
		else
		{
			return mid;
		}
	}
	return InvalidIndex();
}

//-----------------------------------------------------------------------------
// unsorted find with a comparer predicate
// comparerPredicate has to implement: bool ( const T &src ) const;
//-----------------------------------------------------------------------------
template< typename T, class A >
template <typename T2>
inline const T&	CUtlVector<T, A>::FindElementIf( const T2 &comparerPredicate, const T& defaultParam ) const
{
	FOR_EACH_VEC(*this, index)
	{
		if ( comparerPredicate( Base()[index] ) )
		{
			return Base()[index];
		}
	}
	return defaultParam;
}


template< typename T, class A >
inline int CUtlVector<T, A>::SortedInsert( const T& src, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2 ) )
{
	int pos = SortedFindLessOrEqual( src, pfnLessFunc ) + 1;
	GrowVector();
	ShiftElementsRight(pos);
	Construct<T>( &Element(pos), src );
	return pos;
}


//-----------------------------------------------------------------------------
// Sorts the vector
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::Sort( int (__cdecl *pfnCompare)(const T *, const T *) )
{
	std::sort( begin(), end(),
			   [pfnCompare] ( const T& a, const T& b ) -> bool
			   {
					// Some of our comparison functions are misbehaving when comparing an object to itself. Rather
					// than wait for each function to get hit and then fixing them we short-circuit this particular
					// case here.
					if ( &a == &b )
						return false;

#ifdef DEBUG
					// In debug, run the comparison both ways. If we have a sort comparator that isn't correct,
					// some std::sort() implementations will wander off the edge of the list, so having these
					// assertions fire for any input data is a crash waiting to happen.
					const auto A_Cmp_B = (*pfnCompare)( &a, &b );
					const auto B_Cmp_A = (*pfnCompare)( &b, &a );

					if ( A_Cmp_B == 0 )
					{
						// If A == B then B == A or we have a bug in the comparison function.
						Assert( A_Cmp_B == B_Cmp_A );
					}
					else
					{
						// If A < B, then B > A or we have a bug in the comparison function.
						Assert( (A_Cmp_B > 0) == (B_Cmp_A < 0) );
					}
#endif
				  
					return (*pfnCompare)( &a, &b ) < 0;
			   } );
}


//-----------------------------------------------------------------------------
// Sorts the vector
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::Sort( bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2 ) )
{
	std::sort( begin(), end(),
			   [pfnLessFunc] ( const T& a, const T& b ) -> bool
			   {
					// Some of our comparison functions are misbehaving when comparing an object to itself. Rather
					// than wait for each function to get hit and then fixing them we short-circuit this particular
					// case here.
					if ( &a == &b )
						return false;

#ifdef DEBUG
					// In debug, run the comparison both ways. If we have a sort comparator that isn't correct,
					// some std::sort() implementations will wander off the edge of the list, so having these
					// assertions fire for any input data is a crash waiting to happen.
					const auto A_Less_B = (*pfnLessFunc)( a, b );
					const auto B_Less_A = (*pfnLessFunc)( b, a );

					// A can be less than B, B can be less than A, or they can be equal, but A and B can't both
					// be less than each other.
					Assert( !A_Less_B || !B_Less_A );
#endif

					return (*pfnLessFunc)( a, b );
			   } );
}


//-----------------------------------------------------------------------------
// Sorts the vector
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::Sort( bool( __cdecl *pfnLessFunc )(const T& src1, const T& src2, void *pCtx), void *pLessContext )
{
	std::sort( begin(), end(),
			[pfnLessFunc, pLessContext] ( const T& a, const T& b ) -> bool
			{
				// Some of our comparison functions are misbehaving when comparing an object to itself. Rather
				// than wait for each function to get hit and then fixing them we short-circuit this particular
				// case here.
				if ( &a == &b )
					return false;

#ifdef DEBUG
				// In debug, run the comparison both ways. If we have a sort comparator that isn't correct,
				// some std::sort() implementations will wander off the edge of the list, so having these
				// assertions fire for any input data is a crash waiting to happen.
				const auto A_Less_B = (*pfnLessFunc)( a, b, pLessContext);
				const auto B_Less_A = (*pfnLessFunc)( b, a, pLessContext );

				// A can be less than B, B can be less than A, or they can be equal, but A and B can't both
				// be less than each other.
				Assert( !A_Less_B || !B_Less_A );
#endif

				return (*pfnLessFunc)( a, b, pLessContext);
			} );
}


//-----------------------------------------------------------------------------
// Sorts the vector
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::Sort_s( void *context, int (__cdecl *pfnCompare)(void *,const T *, const T *) )
{
	std::sort( begin(), end(),
			   [context, pfnCompare] ( const T& a, const T& b ) -> bool
			   {
					// Some of our comparison functions are misbehaving when comparing an object to itself. Rather
					// than wait for each function to get hit and then fixing them we short-circuit this particular
					// case here.
					if ( &a == &b )
						return false;

#ifdef DEBUG
					// In debug, run the comparison both ways. If we have a sort comparator that isn't correct,
					// some std::sort() implementations will wander off the edge of the list, so having these
					// assertions fire for any input data is a crash waiting to happen.
					const auto A_Cmp_B = (*pfnCompare)( context, &a, &b );
					const auto B_Cmp_A = (*pfnCompare)( context, &b, &a );

					if ( A_Cmp_B == 0 )
					{
						// If A == B then B == A or we have a bug in the comparison function.
						Assert( A_Cmp_B == B_Cmp_A );
					}
					else
					{
						// If A < B, then B > A or we have a bug in the comparison function.
						Assert( (A_Cmp_B > 0) == (B_Cmp_A < 0) );
					}
#endif
				  
					return (*pfnCompare)( context, &a, &b ) < 0;
			  } );
}


//-----------------------------------------------------------------------------
// Sorts the vector
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::Sort_s( void *context, bool (__cdecl *pfnLessFunc)( const T& src1, const T& src2, void *pCtx ) )
{
	std::sort( begin(), end(),
			   [context, pfnLessFunc] ( const T& a, const T& b ) -> bool
			   {
					// Some of our comparison functions are misbehaving when comparing an object to itself. Rather
					// than wait for each function to get hit and then fixing them we short-circuit this particular
					// case here.
					if ( &a == &b )
						return false;

#ifdef DEBUG
					// In debug, run the comparison both ways. If we have a sort comparator that isn't correct,
					// some std::sort() implementations will wander off the edge of the list, so having these
					// assertions fire for any input data is a crash waiting to happen.
					const auto A_Less_B = (*pfnLessFunc)( a, b, context );
					const auto B_Less_A = (*pfnLessFunc)( b, a, context );

					// A can be less than B, B can be less than A, or they can be equal, but A and B can't both
					// be less than each other.
					Assert( !A_Less_B || !B_Less_A );
#endif

					return (*pfnLessFunc)( a, b, context );
			   } );
}


//-----------------------------------------------------------------------------
// Makes sure we have enough memory allocated to store a requested # of elements
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::EnsureCapacity( int num )
{
	m_Memory.EnsureCapacity(num);
}


//-----------------------------------------------------------------------------
// Makes sure we have at least this many elements
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::EnsureCount( int num )
{
	if (Count() < num)
		AddMultipleToTail( num - Count() );
}


//-----------------------------------------------------------------------------
// Shifts elements
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::ShiftElementsRight( int elem, int num )
{
	DbgAssert( IsValidIndex(elem) || ( m_Size == 0 ) || ( num == 0 ));
	int numToMove = m_Size - elem - num;
	if ((numToMove > 0) && (num > 0))
		memmove( (void*)&Element(elem+num), (void *)&Element(elem), numToMove * sizeof(T) );
}

template< typename T, class A >
inline void CUtlVector<T, A>::ShiftElementsLeft( int elem, int num )
{
	DbgAssert( IsValidIndex(elem) || ( m_Size == 0 ) || ( num == 0 ));
	int numToMove = m_Size - elem - num;
	if ((numToMove > 0) && (num > 0))
	{
		memmove( (void*)&Element(elem), (void*)&Element(elem+num), numToMove * sizeof(T) );

#ifdef _DEBUG
		memset( (void*)&Element(m_Size-num), 0xDD, num * sizeof(T) );
#endif
	}
}


//-----------------------------------------------------------------------------
// Adds an element, uses default constructor
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::AddToHead()
{
	return InsertBefore(0);
}

template< typename T, class A >
inline int CUtlVector<T, A>::AddToTail()
{
	return InsertBefore( m_Size );
}

template< typename T, class A >
inline T *CUtlVector<T, A>::AddToTailGetPtr()
{
	return &Element( AddToTail() );
}

template< typename T, class A >
inline int CUtlVector<T, A>::InsertAfter( int elem )
{
	return InsertBefore( elem + 1 );
}

template< typename T, class A >
inline int CUtlVector<T, A>::InsertBefore( int elem )
{
	// Can insert at the end
	Assert( (elem == Count()) || IsValidIndex(elem) );

	GrowVector();
	ShiftElementsRight(elem);
	Construct( &Element(elem) );
	return elem;
}


//-----------------------------------------------------------------------------
// Adds an element, uses copy constructor
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::AddToHead( const T& src )
{
	return InsertBefore( 0, src );
}

template< typename T, class A >
inline int CUtlVector<T, A>::AddToTail( const T& src )
{
	return InsertBefore( m_Size, src );
}

template< typename T, class A >
inline int CUtlVector<T, A>::InsertAfter( int elem, const T& src )
{
	return InsertBefore( elem + 1, src );
}

template< typename T, class A >
inline int CUtlVector<T, A>::InsertBefore( int elem, const T& src )
{
	// Can't insert something that's in the list... reallocation may hose us
	Assert( (&src < Base()) || (&src >= (Base() + Count()) ) );

	// Can insert at the end
	Assert( (elem == Count()) || IsValidIndex(elem) );

	GrowVector();
	ShiftElementsRight(elem);
	Construct( &Element(elem), src );
	return elem;
}

#ifdef VALVE_RVALUE_REFS
// Optimized AddToTail path with move constructor.
template< typename T, class A >
inline int CUtlVector<T, A>::AddToTail( T&& src )
{
	// Can't insert something that's in the list... reallocation may hose us
	Assert( (&src < Base()) || (&src >= (Base() + Count())) );
	int elem = m_Size;
	GrowVector();
	Construct( &Element( elem ), std::forward<T>( src ) );
	return elem;
}
#endif


//-----------------------------------------------------------------------------
// Adds multiple elements, uses default constructor
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::AddMultipleToHead( int num )
{
	return InsertMultipleBefore( 0, num );
}

template< typename T, class A >
inline int CUtlVector<T, A>::AddMultipleToTail( int num, const T *pToCopy )
{
	// Can't insert something that's in the list... reallocation may hose us
	Assert( !pToCopy || (pToCopy + num <= Base()) || (pToCopy >= (Base() + Count()) ) );

	return InsertMultipleBefore( m_Size, num, pToCopy );
}

template< typename T, class A >
inline int CUtlVector<T, A>::InsertMultipleAfter( int elem, int num, const T *pToCopy )
{
	return InsertMultipleBefore( elem + 1, num, pToCopy );
}

template< typename T, class A >
inline void CUtlVector<T, A>::SetCount( int count )
{
	if ( count > m_Size )
	{
		int i = m_Size;
		GrowVector( count - m_Size );
		for ( ; i < m_Size; ++i )
		{
			Construct( m_Memory.Base() + i );
		}
	}
	else if ( count >= 0 )
	{
		int nToRemove = m_Size - count;
		m_Size = count;
		while ( nToRemove-- )
		{
			Destruct( m_Memory.Base() + m_Size + nToRemove );
		}
	}
	else
	{
		Assert( count >= 0 );
	}
}

template< typename T, class A >
inline void CUtlVector<T, A>::CopyArray( const T *pArray, int size )
{
	// Can't insert something that's in the list... reallocation may hose us
	Assert( !pArray || (Base() >= (pArray + size)) || (pArray >= (Base() + Count()) ) );

	SetCount( size );
	for( int i=0; i < size; i++ )
	{
		m_Memory.Base()[i] = pArray[i];
	}
}

template< typename T, class A >
inline void CUtlVector<T, A>::Swap( CUtlVector< T, A > &vec )
{
	m_Memory.Swap( vec.m_Memory );
	SWAP( m_Size, vec.m_Size );
}

template< typename T, class A >
inline int CUtlVector<T, A>::AddVectorToTail( CUtlVector const &src )
{
	return AddMultipleToTail( src.Count(), src.Base() );
}

template< typename T, class A >
inline int CUtlVector<T, A>::InsertMultipleBefore( int elem, int num, const T *pToInsert )
{
	if( num == 0 )
		return elem;

	// Can insert at the end
	Assert( (elem == Count()) || IsValidIndex(elem) );

	GrowVector(num);
	ShiftElementsRight(elem, num);

	// Invoke default constructors
	for (int i = 0; i < num; ++i)
		Construct( &Element(elem+i) );

	// Copy stuff in?
	if ( pToInsert )
	{
		for ( int i=0; i < num; i++ )
		{
			Element( elem+i ) = pToInsert[i];
		}
	}

	return elem;
}


//-----------------------------------------------------------------------------
// Finds an element (element needs operator== defined)
//-----------------------------------------------------------------------------
template< typename T, class A >
inline int CUtlVector<T, A>::Find( const T& src ) const
{
	for ( int i = 0; i < Count(); ++i )
	{
		if (Base()[i] == src)
			return i;
	}
	return InvalidIndex();
}

#ifdef VALVE_RVALUE_REFS
template< typename T, class A >
template < typename TMatchFunc >
inline int CUtlVector<T, A>::FindMatch( TMatchFunc&& func ) const
{
	for ( int i = 0; i < Count(); ++i )
	{
		if ( func( (*this)[i] ) )
			return i;
	}
	return InvalidIndex();
}
#endif // VALVE_RVALUE_REFS

template< typename T, class A >
inline bool CUtlVector<T, A>::HasElement( const T& src ) const
{
	return ( Find(src) != InvalidIndex() );
}


//-----------------------------------------------------------------------------
// Element removal
//-----------------------------------------------------------------------------
template< typename T, class A >
inline void CUtlVector<T, A>::FastRemove( int elem )
{
	Assert( IsValidIndex(elem) );

	Destruct( &Base()[elem] );
	if (m_Size > 0)
	{
		if ( elem != m_Size - 1 )
		{
			// explicitly cast the first + second param here to void * to stop the compiler
			// noticing that we are memsetting the this pointer for non-trivial classes
			memcpy( (void *)&Base()[elem], (void *)&Base()[m_Size-1], sizeof(T) );
		}
		--m_Size;
	}
}

template< typename T, class A >
inline void CUtlVector<T, A>::Remove( int elem )
{
	Destruct( &Element(elem) );
	ShiftElementsLeft(elem);
	--m_Size;
}

template< typename T, class A >
inline bool CUtlVector<T, A>::FindAndRemove( const T& src )
{
	int elem = Find( src );
	if ( elem != InvalidIndex() )
	{
		Remove( elem );
		return true;
	}
	return false;
}

template< typename T, class A >
inline bool CUtlVector<T, A>::FindAndFastRemove( const T& src )
{
	int elem = Find( src );
	if ( elem != InvalidIndex() )
	{
		FastRemove( elem );
		return true;
	}
	return false;
}

template< typename T, class A >
void CUtlVector<T, A>::RemoveMultiple( int elem, int num )
{
	Assert( elem >= 0 && num >= 0 && INT_MAX - elem >= num );

#if defined(COMPILER_GCC) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 5))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-overflow"
#endif

	Assert( elem + num <= Count() );

#if defined(COMPILER_GCC) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 5))
#pragma GCC diagnostic pop
#endif

	for (int i = elem + num; --i >= elem; )
		Destruct(&Base()[i]);

	ShiftElementsLeft(elem, num);
	m_Size -= num;
}

template< typename T, class A >
inline void CUtlVector<T, A>::RemoveMultipleFromTail( int num )
{
	int nToRemove = Min( m_Size, num );
	if ( nToRemove > 0 )
	{
		m_Size -= nToRemove;
		while ( nToRemove-- )
		{
			Destruct( m_Memory.Base() + m_Size + nToRemove );
		}
	}
	else
	{
		Assert( num >= 0 );
	}
}

template< typename T, class A >
inline void CUtlVector<T, A>::RemoveAll()
{
	for (int i = m_Size; --i >= 0; )
	{
		Destruct( m_Memory.Base() + i );
	}
	m_Size = 0;
}




//-----------------------------------------------------------------------------
// Memory deallocation
//-----------------------------------------------------------------------------

template< typename T, class A >
inline void CUtlVector<T, A>::Purge()
{
	RemoveAll();
	m_Memory.Purge();
}


template< typename T, class A >
inline void CUtlVector<T, A>::PurgeAndDeleteElements()
{
	for( int i=0; i < m_Size; i++ )
	{
		delete Element(i);
	}
	Purge();
}

template< typename T, class A >
inline void CUtlVector<T, A>::Compact()
{
	m_Memory.Purge(m_Size);
}


// A vector class for storing pointers, so that the elements pointed to by the pointers are deleted
// on exit.
template<class T> class CUtlVectorAutoPurge : public CUtlVector< T, CUtlMemory< T > >
{
public:
	~CUtlVectorAutoPurge( void )
	{
		this->PurgeAndDeleteElements();
	}

};

//-----------------------------------------------------------------------------
// Data and memory validation
//-----------------------------------------------------------------------------
#ifdef DBGFLAG_VALIDATE
template< typename T, class A >
inline void CUtlVector<T, A>::Validate( CValidator &validator, const char *pchName )
{
#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), this, pchName );
#else
	validator.Push( typeid(*this).name(), this, pchName );
#endif

	m_Memory.Validate( validator, "m_Memory" );

	validator.Pop();
}

template< typename T, class A >
inline void CUtlVector<T, A>::ValidateSelfAndElements( CValidator &validator, const char *pchName )
{
#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), this, pchName );
#else
	validator.Push( typeid(*this).name(), this, pchName );
#endif

	m_Memory.Validate( validator, "m_Memory" );

	FOR_EACH_VEC( *this, i )
	{
		T &element = Element( i );
		ValidateObj( element );
	}

	validator.Pop();
}

#endif // DBGFLAG_VALIDATE


#endif // CCVECTOR_H
