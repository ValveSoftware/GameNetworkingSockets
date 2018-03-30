//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#ifndef UTLPRIORITYQUEUE_H
#define UTLPRIORITYQUEUE_H
#ifdef _WIN32
#pragma once
#endif

#include "utlvector.h"
template < typename T >
class CDefUtlPriorityQueueSetIndexFunc
{
public:
	inline static void SetIndex( T &heapElement, int nNewIndex ) { }
};

// T is the type stored in the queue, it must include the priority
// The head of the list contains the element with GREATEST priority
// configure the LessFunc_t to get the desired queue order
template< class T, class L = bool (*)( T const&, T const& ), class SetIndexFunc = CDefUtlPriorityQueueSetIndexFunc<T> > 
class CUtlPriorityQueue
{
public:
	// Less func typedef
	// Returns true if the first parameter is "less priority" than the second
	// Items that are "less priority" sort toward the tail of the queue
	typedef L LessFunc_t;

	typedef T ElemType_t;

	// constructor: lessfunc is required, but may be set after the constructor with
	// SetLessFunc
	CUtlPriorityQueue( int growSize = 0, int initSize = 0, LessFunc_t lessfunc = LessFunc_t() );
	CUtlPriorityQueue( T *pMemory, int numElements, LessFunc_t lessfunc = LessFunc_t() );

	// gets particular elements
	inline T const&	ElementAtHead() const { return m_heap.Element(0); }

	inline bool IsValidIndex(int index) { return m_heap.IsValidIndex(index); }

	// O(lgn) to rebalance the heap
	void		RemoveAtHead();
	void		RemoveAt( int index );

	// Update the position of the specified element in the tree for it current value O(lgn)
	void		RevaluateElement( const int index ); 
	
	// O(lgn) to rebalance heap
	void		Insert( T const &element );
	// Sets the less func
	void		SetLessFunc( LessFunc_t func );

	// Returns the count of elements in the queue
	inline int	Count() const { return m_heap.Count(); }
	
	// doesn't deallocate memory
	void		RemoveAll() { m_heap.RemoveAll(); }

	// Memory deallocation
	void		Purge() { m_heap.Purge(); }

	inline const T &	Element( int index ) const { return m_heap.Element(index); }

#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName );
#endif // DBGFLAG_VALIDATE

protected:
	CUtlVector<T>	m_heap;

	void		Swap( int index1, int index2 );
	int			PercolateDown( int nIndex );
	int			PercolateUp( int nIndex );

	// Used for sorting.
	LessFunc_t m_LessFunc;
};

template <class T, class LessFunc, class SetIndexFunc>
inline CUtlPriorityQueue<T, LessFunc, SetIndexFunc>::CUtlPriorityQueue( int growSize, int initSize, LessFunc_t lessfunc ) :
	m_heap(growSize, initSize), m_LessFunc(lessfunc)
{
}

template <class T, class LessFunc, class SetIndexFunc>
inline CUtlPriorityQueue<T, LessFunc, SetIndexFunc>::CUtlPriorityQueue( T *pMemory, int numElements, LessFunc_t lessfunc )	: 
	m_heap(pMemory, numElements), m_LessFunc(lessfunc)
{
}

template <class T, class LessFunc, class SetIndexFunc>
inline void CUtlPriorityQueue<T, LessFunc, SetIndexFunc>::RemoveAtHead()
{
	SetIndexFunc::SetIndex( m_heap[ 0 ], m_heap.InvalidIndex() );
	m_heap.FastRemove( 0 );

	if ( Count() > 0 )
	{
		SetIndexFunc::SetIndex( m_heap[ 0 ], 0 );
	}
	
	PercolateDown( 0 );
}


template <class T, class LessFunc, class SetIndexFunc>
inline void CUtlPriorityQueue<T, LessFunc, SetIndexFunc>::RemoveAt( int index )
{
	Assert(m_heap.IsValidIndex(index));
	SetIndexFunc::SetIndex( m_heap[ index ], m_heap.InvalidIndex() );
	m_heap.FastRemove( index );		

	if ( index < Count() )
	{
		SetIndexFunc::SetIndex( m_heap[ index ], index );
	}

	RevaluateElement( index );
}

template <class T, class LessFunc, class SetIndexFunc>
inline void CUtlPriorityQueue<T, LessFunc, SetIndexFunc >::RevaluateElement( int nStartingIndex )
{	
	int index = PercolateDown( nStartingIndex );

	// If index is still the same as the starting index, then the specified element was larger than 
	// its children, so it could be larger than its parent, so treat this like an insertion and swap
	// the node with its parent until it is no longer larger than its parent.
	if ( index == nStartingIndex )
	{
		PercolateUp( index );
	}
}

template< class T, class LessFunc, class SetIndexFunc >
inline int CUtlPriorityQueue<T, LessFunc, SetIndexFunc >::PercolateDown( int index )
{
	int count = Count();
	
	int half = count/2;
	int larger = index;
	while ( index < half )
	{
		int child = ((index+1) * 2) - 1;	// if we wasted an element, this math would be more compact (1 based array)
		if ( child < count )
		{
			// Item has been filtered down to its proper place, terminate.
			if ( m_LessFunc( m_heap[index], m_heap[child] ) )
			{
				// mark the potential swap and check the other child
				larger = child;
			}
		}
		// go to sibling
		child++;
		if ( child < count )
		{
			// If this child is larger, swap it instead
			if ( m_LessFunc( m_heap[larger], m_heap[child] ) )
				larger = child;
		}
		
		if ( larger == index )
			break;

		// swap with the larger child
		Swap( index, larger );
		index = larger;
	}

	return index;
}

template< class T, class LessFunc, class SetIndexFunc >
inline int CUtlPriorityQueue<T, LessFunc, SetIndexFunc >::PercolateUp( int index )
{
	if ( index >= Count() )
		return index;

	while ( index != 0 )
	{
		int parent = ((index+1) / 2) - 1;
		if ( m_LessFunc( m_heap[index], m_heap[parent] ) )
			break;

		// swap with parent and repeat
		Swap( parent, index );
		index = parent;
	}

	return index;
}

template< class T, class LessFunc, class SetIndexFunc >
inline void CUtlPriorityQueue<T, LessFunc, SetIndexFunc >::Insert( T const &element )
{
	int index = m_heap.AddToTail();
	m_heap[index] = element;	
	SetIndexFunc::SetIndex( m_heap[ index ], index );

	PercolateUp( index );
}

template <class T, class LessFunc, class SetIndexFunc>
void CUtlPriorityQueue<T, LessFunc, SetIndexFunc>::Swap( int index1, int index2 )
{
	T tmp = m_heap[index1];
	m_heap[index1] = m_heap[index2];
	m_heap[index2] = tmp;
	SetIndexFunc::SetIndex( m_heap[ index1 ], index1 );
	SetIndexFunc::SetIndex( m_heap[ index2 ], index2 );
}

template <class T, class LessFunc, class SetIndexFunc>
void CUtlPriorityQueue<T, LessFunc, SetIndexFunc>::SetLessFunc( LessFunc_t lessfunc )
{
	m_LessFunc = lessfunc;
}

//-----------------------------------------------------------------------------
// Data and memory validation
//-----------------------------------------------------------------------------
#ifdef DBGFLAG_VALIDATE
template <class T, class LessFunc, class SetIndexFunc>
void CUtlPriorityQueue<T, LessFunc, SetIndexFunc>::Validate( CValidator &validator, const char *pchName )
{
#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), this, pchName );
#else
	validator.Push( typeid(*this).name(), this, pchName );
#endif

	m_heap.Validate( validator, "m_heap" );

	validator.Pop();
}
#endif // DBGFLAG_VALIDATE

#endif // UTLPRIORITYQUEUE_H
