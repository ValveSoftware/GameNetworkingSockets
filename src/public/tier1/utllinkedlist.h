//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: Linked list container class 
//
// $Revision: $
// $NoKeywords: $
//=============================================================================//

#ifndef UTLLINKEDLIST_H
#define UTLLINKEDLIST_H

#ifdef _WIN32
#pragma once
#endif

#include "tier0/platform.h"
#include "tier0/dbg.h"
#include "utlmemory.h"
#include "utliterator.h"


// This is a useful macro to iterate from head to tail in a CUtlLinkedList.
// CUtlLinkedListHead/Tail functions are used to ensure that these macros
// are not used on a CUtlFixedLinkedList. Use FOR_EACH_FLL for CUtlFixedLinkedList.
#define FOR_EACH_LL( listName, iteratorName ) \
	for( int iteratorName=(listName).CUtlLinkedListHead(); iteratorName != (listName).InvalidIndex(); iteratorName = (listName).Next( iteratorName ) )

#define FOR_EACH_LL_BACK( listName, iteratorName ) \
	for( int iteratorName=(listName).CUtlLinkedListTail(); iteratorName != (listName).InvalidIndex(); iteratorName = (listName).Previous( iteratorName ) )

#define FOR_EACH_LL_FAST( listName, iteratorName ) \
	for ( int iteratorName = 0; iteratorName < (listName).MaxElementIndex(); ++iteratorName ) if ( !(listName).IsValidIndex( iteratorName ) ) continue; else


#define INVALID_LLIST_IDX ((I)~0)

//-----------------------------------------------------------------------------
// class CUtlLinkedList:
// description:
//		A lovely index-based linked list! T is the class type, I is the index
//		type, which usually should be an unsigned short or smaller.
//-----------------------------------------------------------------------------

template <class T, class I = int> 
class CUtlLinkedList
{
public:
	typedef T ElemType_t;
	typedef I IndexType_t;

	// constructor, destructor
	CUtlLinkedList( int growSize = 0, int initSize = 0 );
	CUtlLinkedList( void *pMemory, int memsize );
	~CUtlLinkedList();

	// gets particular elements
	T&         Element( I i );
	T const&   Element( I i ) const;
	T&         operator[]( I i );
	T const&   operator[]( I i ) const;

	// Make sure we have a particular amount of memory
	void EnsureCapacity( int num );

	// Memory deallocation
	void Purge();

	// Delete all the elements then call Purge.
	void PurgeAndDeleteElements();
	
	int NumAllocated() const { return m_Memory.NumAllocated(); }	// Only use this if you really know what you're doing!
		
	// Insertion methods....
	I	InsertBefore( I before );
	I	InsertAfter( I after );
	I	AddToHead(); 
	I	AddToTail();

	I	InsertBefore( I before, T const& src );
	I	InsertAfter( I after, T const& src );
	I	AddToHead( T const& src ); 
	I	AddToTail( T const& src );

	// Find an element and return its index or InvalidIndex() if it couldn't be found.
	I		Find( const T &src ) const;
	
	// Look for the element. If it exists, remove it and return true. Otherwise, return false.
	bool	FindAndRemove( const T &src );

	// Removal methods
	T		RemoveFromHead();
	T		RemoveFromTail();
	void	Remove( I elem );
	void	RemoveAll();

	// swap one list for another
	void	Swap( CUtlLinkedList< T, I >  &list );

	// Allocation/deallocation methods
	// If multilist == true, then list list may contain many
	// non-connected lists, and IsInList and Head + Tail are meaningless...
	I		Alloc( bool multilist = false );
	void	Free( I elem );

	// list modification
	void	LinkBefore( I before, I elem );
	void	LinkAfter( I after, I elem );
	void	Unlink( I elem );
	void	LinkToHead( I elem );
	void	LinkToTail( I elem );

	// invalid index
	inline static I  InvalidIndex()  { return INVALID_LLIST_IDX; }
	inline static size_t ElementSize() { return sizeof(ListElem_t); }

	// list statistics
	int	Count() const;

	inline bool IsEmpty( void ) const
	{
		return ( Head() == InvalidIndex() );
	}

	I	MaxElementIndex() const;

	// Traversing the list
	I  Head() const;
	I  First() const;
	I  Tail() const;
	I  Previous( I i ) const;
	I  Next( I i ) const;

	// so we can go through a FOR_EACH_MAP
	I  FirstInorder() const { return First(); }
	I  NextInorder( I i ) const { return Next( i ); }

	// STL / C++11-style iterators 
	typedef CUtlBidirectionalIteratorImplT< CUtlLinkedList< T, I >, false > iterator;
	typedef CUtlBidirectionalIteratorImplT< CUtlLinkedList< T, I >, true > const_iterator;
	const_iterator begin() const { return const_iterator( this, Head() ); }
	const_iterator end() const { return const_iterator( this, INVALID_LLIST_IDX ); }
	iterator begin() { return iterator( this, Head() ); }
	iterator end() { return iterator( this, INVALID_LLIST_IDX ); }
	I IteratorNext( I i ) const { return Next( i ); }
	I IteratorPrev( I i ) const { return i == INVALID_LLIST_IDX ? Tail() : Previous( i ); }

	// Are nodes in the list or valid?
	bool  IsValidIndex( I i ) const;
	bool  IsInList( I i ) const;

#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName );		// Validate our internal structures
	void ValidateSelfAndElements( CValidator &validator, const char *pchName );
#endif // DBGFLAG_VALIDATE

	// The existence of these function let us verify that this is a
	// CUtlLinkedList rather than a CUtlFixedLinkedList, so that the
	// iteration macros can prevent using FOR_EACH_LL on a
	// CUtlFixedLinkedList, with the potentially too-small index var.
	I  CUtlLinkedListHead() const { return Head(); }
	I  CUtlLinkedListTail() const { return Tail(); }

protected:
	// What the linked list element looks like
	struct ListElem_t
	{
		T  m_Element;
		I  m_Previous;
		I  m_Next;

	private:
		// No copy constructor for these...
		ListElem_t( const ListElem_t& );
	};
	
	// constructs the class
	I		AllocInternal( bool multilist = false );
	void ConstructList();
	
	// Gets at the list element....
	ListElem_t& InternalElement( I i ) { return m_Memory[i]; }
	ListElem_t const& InternalElement( I i ) const { return m_Memory[i]; }

	void ResetDbgInfo()
	{
		m_pElements = m_Memory.Base();
	}
	
	// copy constructors not allowed
	CUtlLinkedList( CUtlLinkedList<T, I> const& list );
	const CUtlLinkedList& operator=(const CUtlLinkedList& rhs);
	   
	CUtlMemory<ListElem_t> m_Memory;
	I	m_Head;
	I	m_Tail;
	I	m_FirstFree;
	I	m_ElementCount;		// The number actually in the list
	I	m_TotalElements;	// The number allocated
	
	// For debugging purposes; 
	// it's in release builds so this can be used in libraries correctly
	ListElem_t  *m_pElements;
};
   
   
//-----------------------------------------------------------------------------
// constructor, destructor
//-----------------------------------------------------------------------------

template <class T, class I>
inline CUtlLinkedList<T,I>::CUtlLinkedList( int growSize, int initSize ) :
	m_Memory(growSize, initSize) 
{
	ConstructList();
	ResetDbgInfo();
}

template <class T, class I>
inline CUtlLinkedList<T,I>::CUtlLinkedList( void* pMemory, int memsize ) :
	m_Memory((ListElem_t *)pMemory, memsize/sizeof(ListElem_t))
{
	ConstructList();
	ResetDbgInfo();
}

template <class T, class I>
inline CUtlLinkedList<T,I>::~CUtlLinkedList()
{
	RemoveAll();
}

template <class T, class I>
inline void CUtlLinkedList<T,I>::ConstructList()
{
	m_Head = InvalidIndex(); 
	m_Tail = InvalidIndex();
	m_FirstFree = InvalidIndex();
	m_ElementCount = m_TotalElements = 0;
}


//-----------------------------------------------------------------------------
// gets particular elements
//-----------------------------------------------------------------------------

template <class T, class I>
inline T& CUtlLinkedList<T,I>::Element( I i )
{
	return m_Memory[i].m_Element; 
}

template <class T, class I>
inline T const& CUtlLinkedList<T,I>::Element( I i ) const
{
	return m_Memory[i].m_Element; 
}

template <class T, class I>
inline T& CUtlLinkedList<T,I>::operator[]( I i )        
{ 
	return m_Memory[i].m_Element; 
}

template <class T, class I>
inline T const& CUtlLinkedList<T,I>::operator[]( I i ) const 
{
	return m_Memory[i].m_Element; 
}

//-----------------------------------------------------------------------------
// list statistics
//-----------------------------------------------------------------------------

template <class T, class I>
inline int CUtlLinkedList<T,I>::Count() const      
{ 
	return m_ElementCount; 
}

template <class T, class I>
inline I CUtlLinkedList<T,I>::MaxElementIndex() const   
{
	return m_TotalElements;
}


//-----------------------------------------------------------------------------
// Traversing the list
//-----------------------------------------------------------------------------

template <class T, class I>
inline I  CUtlLinkedList<T,I>::Head() const  
{ 
	return m_Head; 
}

template <class T, class I>
inline I  CUtlLinkedList<T,I>::First() const  
{ 
	return m_Head; 
}

template <class T, class I>
inline I  CUtlLinkedList<T,I>::Tail() const  
{ 
	return m_Tail; 
}

template <class T, class I>
inline I  CUtlLinkedList<T,I>::Previous( I i ) const  
{ 
	DbgAssert( IsValidIndex(i) ); 
	return InternalElement(i).m_Previous; 
}

template <class T, class I>
inline I  CUtlLinkedList<T,I>::Next( I i ) const  
{ 
	DbgAssertMsg1( IsValidIndex(i), "CUtlLinkedList::Next: invalid index %lld\n", (int64)i ); 
	return InternalElement(i).m_Next; 
}


//-----------------------------------------------------------------------------
// Are nodes in the list or valid?
//-----------------------------------------------------------------------------

template <class T, class I>
inline bool CUtlLinkedList<T,I>::IsValidIndex( I i ) const  
{ 
	return (i < m_TotalElements) && (i >= 0) &&
		((m_Memory[i].m_Previous != i) || (m_Memory[i].m_Next == i));
}

template <class T, class I>
inline bool CUtlLinkedList<T,I>::IsInList( I i ) const
{
	return (i < m_TotalElements) && (i >= 0) && (Previous(i) != i);
}

//-----------------------------------------------------------------------------
// Makes sure we have enough memory allocated to store a requested # of elements
//-----------------------------------------------------------------------------

template< class T, class I >
inline void CUtlLinkedList<T, I>::EnsureCapacity( int num )
{
	m_Memory.EnsureCapacity(num);
	ResetDbgInfo();
}


//-----------------------------------------------------------------------------
// Deallocate memory
//-----------------------------------------------------------------------------

template <class T, class I>
inline void  CUtlLinkedList<T,I>::Purge()
{
	RemoveAll();
	m_Memory.Purge();
	m_FirstFree = InvalidIndex();
	m_TotalElements = 0;
	ResetDbgInfo();

}


template<class T, class I>
inline void CUtlLinkedList<T, I>::PurgeAndDeleteElements()
{
	int iNext;
	for( int i=Head(); i != InvalidIndex(); i=iNext )
	{
		iNext = Next(i);
		delete Element(i);
	}

	Purge();
}


//-----------------------------------------------------------------------------
// Node allocation/deallocation
//-----------------------------------------------------------------------------
template <class T, class I>
inline I CUtlLinkedList<T,I>::AllocInternal( bool multilist )
{
	I elem;
	if (m_FirstFree == InvalidIndex())
	{
		// Nothing in the free list; add.
		// Since nothing is in the free list, m_TotalElements == total # of elements
		// the list knows about.
		if (m_TotalElements == (I)m_Memory.NumAllocated())
			m_Memory.Grow();

		Assert( m_TotalElements != InvalidIndex() );

		elem = (I)m_TotalElements;
		++m_TotalElements;

		AssertFatalMsg( elem != InvalidIndex(), "CUtlLinkedList overflow!\n" );
	} 
	else
	{
		elem = m_FirstFree;
		m_FirstFree = InternalElement(m_FirstFree).m_Next;
	}
	
	if (!multilist)
		InternalElement(elem).m_Next = InternalElement(elem).m_Previous = elem;
	else
		InternalElement(elem).m_Next = InternalElement(elem).m_Previous = InvalidIndex();

	ResetDbgInfo();

	return elem;
}

template <class T, class I>
inline I CUtlLinkedList<T,I>::Alloc( bool multilist )
{
	I elem = AllocInternal( multilist );
	Construct( &Element(elem) );

	return elem;
}

template <class T, class I>
inline void  CUtlLinkedList<T,I>::Free( I elem )
{
	Assert( IsValidIndex(elem) );
	Unlink(elem);

	ListElem_t &internalElem = InternalElement(elem);
	Destruct( &internalElem.m_Element );
	internalElem.m_Next = m_FirstFree;
	m_FirstFree = elem;
}

//-----------------------------------------------------------------------------
// Insertion methods; allocates and links (uses default constructor)
//-----------------------------------------------------------------------------

template <class T, class I>
inline I CUtlLinkedList<T,I>::InsertBefore( I before )
{
	// Make a new node
	I   newNode = AllocInternal();
	
	// Link it in
	LinkBefore( before, newNode );
	
	// Construct the data
	Construct( &Element(newNode) );
	
	return newNode;
}

template <class T, class I>
inline I CUtlLinkedList<T,I>::InsertAfter( I after )
{
	// Make a new node
	I   newNode = AllocInternal();
	
	// Link it in
	LinkAfter( after, newNode );
	
	// Construct the data
	Construct( &Element(newNode) );
	
	return newNode;
}

template <class T, class I>
inline I CUtlLinkedList<T,I>::AddToHead() 
{ 
	return InsertAfter( InvalidIndex() ); 
}

template <class T, class I>
inline I CUtlLinkedList<T,I>::AddToTail() 
{ 
	return InsertBefore( InvalidIndex() ); 
}


//-----------------------------------------------------------------------------
// Swap one list for another
//-----------------------------------------------------------------------------

template <class T, class I>
inline void CUtlLinkedList<T,I>::Swap( CUtlLinkedList< T, I >  &list )
{
	m_Memory.Swap( list.m_Memory );

	SWAP( m_Head, list.m_Head );
	SWAP( m_Tail, list.m_Tail );
	SWAP( m_FirstFree, list.m_FirstFree );
	SWAP( m_ElementCount, list.m_ElementCount );
	SWAP( m_TotalElements, list.m_TotalElements );

	SWAP( m_pElements, list.m_pElements );
}


//-----------------------------------------------------------------------------
// Insertion methods; allocates and links (uses copy constructor)
//-----------------------------------------------------------------------------

template <class T, class I>
inline I CUtlLinkedList<T,I>::InsertBefore( I before, T const& src )
{
	// Make a new node
	I   newNode = AllocInternal();
	
	// Link it in
	LinkBefore( before, newNode );
	
	// Construct the data
	CopyConstruct( &Element(newNode), src );
	
	return newNode;
}

template <class T, class I>
inline I CUtlLinkedList<T,I>::InsertAfter( I after, T const& src )
{
	// Make a new node
	I   newNode = AllocInternal();
	
	// Link it in
	LinkAfter( after, newNode );
	
	// Construct the data
	CopyConstruct( &Element(newNode), src );
	
	return newNode;
}

template <class T, class I>
inline I CUtlLinkedList<T,I>::AddToHead( T const& src ) 
{ 
	return InsertAfter( InvalidIndex(), src ); 
}

template <class T, class I>
inline I CUtlLinkedList<T,I>::AddToTail( T const& src ) 
{ 
	return InsertBefore( InvalidIndex(), src ); 
}


//-----------------------------------------------------------------------------
// Removal methods
//-----------------------------------------------------------------------------

template<class T, class I>
inline I CUtlLinkedList<T,I>::Find( const T &src ) const
{
	for ( I i=Head(); i != InvalidIndex(); i = Next( i ) )
	{
		if ( Element( i ) == src )
			return i;
	}
	return InvalidIndex();
}


template<class T, class I>
inline bool CUtlLinkedList<T,I>::FindAndRemove( const T &src )
{
	I i = Find( src );
	if ( i == InvalidIndex() )
	{
		return false;
	}
	else
	{
		Remove( i );
		return true;
	}
}

template <class T, class I>
inline T	CUtlLinkedList<T,I>::RemoveFromHead()
{
	T element = Element( Head() );
	Remove( Head() );
	return element;
}

template <class T, class I>
inline T	CUtlLinkedList<T,I>::RemoveFromTail()
{
	T element = Element( Tail() );
	Remove( Tail() );
	return element;
}

template <class T, class I>
inline void  CUtlLinkedList<T,I>::Remove( I elem )
{
	Free( elem );
}

template <class T, class I>
inline void  CUtlLinkedList<T,I>::RemoveAll()
{
	if (m_TotalElements == 0)
		return;

	// Put everything into the free list
	I prev = InvalidIndex();
	for (int i = (int)m_TotalElements; --i >= 0; )
	{
		// Invoke the destructor
		if (IsValidIndex((I)i))
			Destruct( &Element((I)i) );
		
		// next points to the next free list item
		InternalElement((I)i).m_Next = prev;
		
		// Indicates it's in the free list
		InternalElement((I)i).m_Previous = (I)i;
		prev = (I)i;
	}
	
	// First free points to the first element
	m_FirstFree = 0;
	
	// Clear everything else out
	m_Head = InvalidIndex(); 
	m_Tail = InvalidIndex();
	m_ElementCount = 0;
}


//-----------------------------------------------------------------------------
// list modification
//-----------------------------------------------------------------------------

template <class T, class I>
inline void  CUtlLinkedList<T,I>::LinkBefore( I before, I elem )
{
	Assert( IsValidIndex(elem) );
	
	// Unlink it if it's in the list at the moment
	Unlink(elem);
	
	ListElem_t& newElem = InternalElement(elem);
	
	// The element *after* our newly linked one is the one we linked before.
	newElem.m_Next = before;
	
	if (before == InvalidIndex())
	{
		// In this case, we're linking to the end of the list, so reset the tail
		newElem.m_Previous = m_Tail;
		m_Tail = elem;
	}
	else
	{
		// Here, we're not linking to the end. Set the prev pointer to point to
		// the element we're linking.
		Assert( IsInList(before) );
		ListElem_t& beforeElem = InternalElement(before);
		newElem.m_Previous = beforeElem.m_Previous;
		beforeElem.m_Previous = elem;
	}
	
	// Reset the head if we linked to the head of the list
	if (newElem.m_Previous == InvalidIndex())
		m_Head = elem;
	else
		InternalElement(newElem.m_Previous).m_Next = elem;
	
	// one more element baby
	++m_ElementCount;
}

template <class T, class I>
inline void  CUtlLinkedList<T,I>::LinkAfter( I after, I elem )
{
	Assert( IsValidIndex(elem) );
	
	// Unlink it if it's in the list at the moment
	if ( IsInList(elem) )
		Unlink(elem);
	
	ListElem_t& newElem = InternalElement(elem);
	
	// The element *before* our newly linked one is the one we linked after
	newElem.m_Previous = after;
	if (after == InvalidIndex())
	{
		// In this case, we're linking to the head of the list, reset the head
		newElem.m_Next = m_Head;
		m_Head = elem;
	}
	else
	{
		// Here, we're not linking to the end. Set the next pointer to point to
		// the element we're linking.
		Assert( IsInList(after) );
		ListElem_t& afterElem = InternalElement(after);
		newElem.m_Next = afterElem.m_Next;
		afterElem.m_Next = elem;
	}
	
	// Reset the tail if we linked to the tail of the list
	if (newElem.m_Next == InvalidIndex())
		m_Tail = elem;
	else
		InternalElement(newElem.m_Next).m_Previous = elem;
	
	// one more element baby
	++m_ElementCount;
}

template <class T, class I>
inline void  CUtlLinkedList<T,I>::Unlink( I elem )
{
	Assert( IsValidIndex(elem) );
	if (IsInList(elem))
	{
		ListElem_t *pBase = m_Memory.Base();
		ListElem_t *pOldElem = &pBase[elem];
		
		// If we're the first guy, reset the head
		// otherwise, make our previous node's next pointer = our next
		if ( pOldElem->m_Previous != INVALID_LLIST_IDX )
		{
			pBase[pOldElem->m_Previous].m_Next = pOldElem->m_Next;
		}
		else
		{
			m_Head = pOldElem->m_Next;
		}
		
		// If we're the last guy, reset the tail
		// otherwise, make our next node's prev pointer = our prev
		if ( pOldElem->m_Next != INVALID_LLIST_IDX )
		{
			pBase[pOldElem->m_Next].m_Previous = pOldElem->m_Previous;
		}
		else
		{
			m_Tail = pOldElem->m_Previous;
		}
		
		// This marks this node as not in the list, 
		// but not in the free list either
		pOldElem->m_Previous = pOldElem->m_Next = elem;
		
		// One less puppy
		--m_ElementCount;
	}
}

template <class T, class I>
inline void CUtlLinkedList<T,I>::LinkToHead( I elem ) 
{
	LinkAfter( InvalidIndex(), elem ); 
}

template <class T, class I>
inline void CUtlLinkedList<T,I>::LinkToTail( I elem ) 
{
	LinkBefore( InvalidIndex(), elem ); 
}


#ifdef DBGFLAG_VALIDATE
template <class T, class I>
inline void CUtlLinkedList<T, I>::Validate( CValidator &validator, const char *pchName )
{
#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), this, pchName );
#else
	validator.Push( typeid(*this).name(), this, pchName );
#endif

	m_Memory.Validate( validator, "m_Memory" );

	validator.Pop();
}

template <class T, class I>
inline void CUtlLinkedList<T, I>::ValidateSelfAndElements( CValidator &validator, const char *pchName )
{
#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), this, pchName );
#else
	validator.Push( typeid(*this).name(), this, pchName );
#endif

	m_Memory.Validate( validator, "m_Memory" );

	CValidateHelper< T >	functor( validator );

	FOR_EACH_LL( *this, i )
	{
		functor( Element(i), pchName );
	}

	validator.Pop();
}

#endif // DBGFLAG_VALIDATE

   
#endif // UTLLINKEDLIST_H
