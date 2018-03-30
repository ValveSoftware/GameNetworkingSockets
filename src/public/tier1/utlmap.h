//========= Copyright (C) 1996-2005, Valve Corporation, All rights reserved. ==//
//
// Purpose: 
//
// $Header: $
// $NoKeywords: $
//=============================================================================//

#ifndef UTLMAP_H
#define UTLMAP_H

#ifdef _WIN32
#pragma once
#endif

#include "tier0/dbg.h"
#include "utlrbtree.h"

//-----------------------------------------------------------------------------
//
// Purpose:	An associative container.
//
//-----------------------------------------------------------------------------

// This is a useful macro to iterate from start to end in order in a map
#define FOR_EACH_MAP( mapName, iteratorName ) \
	for ( MAP_INDEX_TYPE( mapName ) iteratorName = (mapName).FirstInorder(); iteratorName != (mapName).InvalidIndex(); iteratorName = (mapName).NextInorder( iteratorName ) )

// faster iteration, but in an unspecified order
#define FOR_EACH_MAP_FAST( mapName, iteratorName ) \
	for ( MAP_INDEX_TYPE( mapName ) iteratorName = 0; iteratorName < (mapName).MaxElement(); ++iteratorName ) if ( !(mapName).IsValidIndex( iteratorName ) ) continue; else

// faster iteration, but in an unspecified order
#define FOR_EACH_MAP_PTR_FAST( mapName, iteratorName ) \
	for ( MAP_INDEX_TYPE( *mapName ) iteratorName = 0; iteratorName < (mapName)->MaxElement(); ++iteratorName ) if ( !(mapName)->IsValidIndex( iteratorName ) ) continue; else

// This is a useful macro to iterate from end to start (backwards) in order in a map
#define FOR_EACH_MAP_BACK( mapName, iteratorName ) \
	for ( MAP_INDEX_TYPE( mapName ) iteratorName = (mapName).LastInorder(); iteratorName != (mapName).InvalidIndex(); iteratorName = (mapName).PrevInorder( iteratorName ) )


template <typename K, typename T, typename I = int, typename L = bool (*)( const K &, const K & ) > 
class CUtlMap
{
public:
	typedef K KeyType_t;
	typedef T ElemType_t;
	typedef I IndexType_t;
	typedef L LessFunc_t;
	
	// CUtlMap is implemented as a CUtlRBTree of Node_t elements
	struct Node_t
	{
		KeyType_t	key;
		ElemType_t	elem;
	};

	// constructor, destructor
	// Left at growSize = 0, the memory will first allocate 1 element and double in size
	// at each increment.
	// LessFunc_t is required, but may be set after the constructor using SetLessFunc() below
	CUtlMap( int growSize = 0, int initSize = 0, LessFunc_t lessfunc = 0 )
	 : m_Tree( growSize, initSize, CKeyLess( lessfunc ) )
	{
	}
	
	CUtlMap( LessFunc_t lessfunc )
	 : m_Tree( CKeyLess( lessfunc ) )
	{
	}
	
	// gets particular elements
	ElemType_t &		Element( IndexType_t i )			{ return m_Tree.Element( i ).elem; }
	const ElemType_t &	Element( IndexType_t i ) const		{ return m_Tree.Element( i ).elem; }
	ElemType_t &		operator[]( IndexType_t i )			{ return m_Tree.Element( i ).elem; }
	const ElemType_t &	operator[]( IndexType_t i ) const	{ return m_Tree.Element( i ).elem; }
	KeyType_t &			Key( IndexType_t i )				{ return m_Tree.Element( i ).key; }
	const KeyType_t &	Key( IndexType_t i ) const			{ return m_Tree.Element( i ).key; }
	ElemType_t &		ElementByLinearIndex( IndexType_t i )			{ return m_Tree.ElementByLinearIndex( i ).elem; }
	const ElemType_t &	ElementByLinearIndex( IndexType_t i ) const		{ return m_Tree.ElementByLinearIndex( i ).elem; }

	
	// Num elements
	unsigned int Count() const								{ return m_Tree.Count(); }
	
	// Max "size" of the vector
	IndexType_t  MaxElement() const							{ return m_Tree.MaxElement(); }
	
	// Checks if a node is valid and in the map
	bool  IsValidIndex( IndexType_t i ) const				{ return m_Tree.IsValidIndex( i ); }
	
	// Checks if a node is valid and in the map
	bool  IsValidLinearIndex( IndexType_t i ) const			{ return m_Tree.IsValidLinearIndex( i ); }

	// Checks if the map as a whole is valid
	bool  IsValid() const									{ return m_Tree.IsValid(); }
	
	// Invalid index
	static IndexType_t InvalidIndex()						{ return INVALID_RBTREE_IDX; }

	// Sets the less func
	void SetLessFunc( LessFunc_t func )
	{
		m_Tree.SetLessFunc( CKeyLess( func ) );
	}
	
	// Insert method (inserts in order)
	IndexType_t  Insert( const KeyType_t &key, const ElemType_t &insert )
	{
		Node_t node;
		node.key = key;
		node.elem = insert;
		return m_Tree.Insert( node, false );
	}
	
	IndexType_t  Insert( const KeyType_t &key )
	{
		Node_t node;
		node.key = key;
		return m_Tree.Insert( node, false );
	}

	IndexType_t  InsertWithDupes( const KeyType_t &key, const ElemType_t &insert )
	{
		Node_t node;
		node.key = key;
		node.elem = insert;
		return m_Tree.Insert( node, true );
	}

	IndexType_t  InsertWithDupes( const KeyType_t &key )
	{
		Node_t node;
		node.key = key;
		return m_Tree.Insert( node, true );
	}

	bool HasElement( const KeyType_t &key ) const
	{
		Node_t dummyNode;
		dummyNode.key = key;
		return m_Tree.HasElement( dummyNode );
	}

	// Find method
	// This finds an occurrence of key, but if there
	// are multiple you will get the highest one in the
	// tree so you can make no assumptions about its order
	IndexType_t  Find( const KeyType_t &key ) const
	{
		Node_t dummyNode;
		dummyNode.key = key;
		return m_Tree.Find( dummyNode );
	}

	// FindFirst method
	// This finds the first inorder occurrence of key
	IndexType_t  FindFirst( const KeyType_t &key ) const
	{
		Node_t dummyNode;
		dummyNode.key = key;
		return m_Tree.FindFirst( dummyNode );
	}

	// First element >= key
	IndexType_t  FindClosest( const KeyType_t &key, CompareOperands_t eFindCriteria ) const
	{
		Node_t dummyNode;
		dummyNode.key = key;
		return m_Tree.FindClosest( dummyNode, eFindCriteria );
	}

	const ElemType_t &FindElement( const KeyType_t &key, const ElemType_t &defaultValue ) const
	{
		IndexType_t i = Find( key );
		if ( i == InvalidIndex() )
			return defaultValue;
		return Element( i );
	}

	// Remove methods
	void     RemoveAt( IndexType_t i )						{ m_Tree.RemoveAt( i ); }
	bool     Remove( const KeyType_t &key )
	{
		Node_t dummyNode;
		dummyNode.key = key;
		return m_Tree.Remove( dummyNode );
	}

	// remove all members, but leave the memory allocated by the container behind for reuse
	void     RemoveAll()									{ m_Tree.RemoveAll(); }
			
	// Iteration
	IndexType_t  FirstInorder() const						{ return m_Tree.FirstInorder(); }
	IndexType_t  NextInorder( IndexType_t i ) const			{ return m_Tree.NextInorder( i ); }
	IndexType_t  PrevInorder( IndexType_t i ) const			{ return m_Tree.PrevInorder( i ); }
	IndexType_t  LastInorder() const						{ return m_Tree.LastInorder(); }		
	
	IndexType_t  PrevInorderSameKey( IndexType_t i ) const
	{
		IndexType_t iPrev = PrevInorder( i );
		if ( !IsValidIndex( iPrev ) )
			return INVALID_RBTREE_IDX;
		if ( Key(iPrev) != Key(i) )
			return INVALID_RBTREE_IDX;
		return iPrev;
	}
	IndexType_t  NextInorderSameKey( IndexType_t i ) const
	{
		IndexType_t iNext = NextInorder( i );
		if ( !IsValidIndex( iNext ) )
			return INVALID_RBTREE_IDX;
		if ( Key(iNext) != Key(i) )
			return INVALID_RBTREE_IDX;
		return iNext;
	}

	IndexType_t GetRoot() const
	{
		return m_Tree.Root();
	}

	// If you change the search key, this can be used to reinsert the 
	// element into the map.
	void	Reinsert( const KeyType_t &key, IndexType_t i )
	{
		m_Tree[i].key = key;
		m_Tree.Reinsert(i);
	}

	// replace an element if the key already exists; otherwise, insert it
	// note that this will leak element is a pointer type as
	// there is no chance to delete the previous element
	IndexType_t InsertOrReplace( const KeyType_t &key, const ElemType_t &insert )
	{
		// Insert already provides InsertOrReplace behavior
		return Insert( key, insert );
	}

	// find element if the key already exists; otherwise, insert it
	IndexType_t FindOrInsert( const KeyType_t &key, const ElemType_t &insert )
	{
		Node_t node;
		node.key = key;
		node.elem = insert;
		return m_Tree.FindOrInsert( node );
	}

	// swap in place
	void Swap( CUtlMap< K, T, I, L > &that )
	{
		m_Tree.Swap( that.m_Tree );
	}

	// Makes sure we have enough memory allocated to store a requested # of elements
	void EnsureCapacity( int num )
	{
		m_Tree.EnsureCapacity( num );
	}

	// purge, which will free memory in the underlying container implementation
	void Purge()
	{
		m_Tree.Purge();
	}

	// call delete on each element (as a pointer) and then purge
	void PurgeAndDeleteElements()
	{
		FOR_EACH_MAP_FAST( *this, i )
			delete this->Element(i);
		Purge();
	}
	
	int CubAllocated() { return m_Tree.CubAllocated(); }


#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName );
	void ValidateSelfAndElements( CValidator &validator, const char *pchName );
#endif // DBGFLAG_VALIDATE


protected:

	// Disallow copy construction and assignment for now
	CUtlMap( const CUtlMap &that );
	CUtlMap& operator=( const CUtlMap &that );

	class CKeyLess
	{
	public:
		CKeyLess( LessFunc_t lessFunc ) : m_LessFunc(lessFunc) {}

		bool operator!() const
		{
			return !m_LessFunc;
		}

		bool operator()( const Node_t &left, const Node_t &right ) const
		{
			return m_LessFunc( left.key, right.key );
		}

		LessFunc_t m_LessFunc;
	};

	typedef CUtlRBTree<Node_t, I, CKeyLess> CTree;

	CTree *AccessTree()	{ return &m_Tree; }

public:
	// STL / C++11-style iterators (iterating Node_t elements of the underlying RBTree)
	typedef typename CTree::iterator iterator;
	typedef typename CTree::const_iterator const_iterator;
	iterator begin() { return m_Tree.begin(); }
	iterator end() { return m_Tree.end(); }
	const_iterator begin() const { return m_Tree.begin(); }
	const_iterator end() const { return m_Tree.end(); }

	// STL / C++11-style iterator using storage order (effectively unordered, but faster)
	typedef typename CTree::ProxyTypeIterateUnordered ProxyTypeIterateUnordered;
	typedef typename ProxyTypeIterateUnordered::iterator unordered_iterator;
	typedef typename ProxyTypeIterateUnordered::const_iterator unordered_const_iterator;
	ProxyTypeIterateUnordered& IterateUnordered() { return m_Tree.IterateUnordered(); }
	const ProxyTypeIterateUnordered& IterateUnordered() const { return m_Tree.IterateUnordered(); }

protected:
	CTree 	   m_Tree;
};

// Same as CUtlMap, but less func defaults to be CDefLess instead of
// function pointer.
template <typename K, typename T, typename L = CDefLess<K> >
using CUtlOrderedMap = CUtlMap< K, T, int, L >;

//-----------------------------------------------------------------------------
// Data and memory validation
//-----------------------------------------------------------------------------
#ifdef DBGFLAG_VALIDATE
template <typename K, typename T, typename I, typename L > 
void CUtlMap<K, T, I, L>::Validate( CValidator &validator, const char *pchName )
{
#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), this, pchName );
#else
	validator.Push( typeid(*this).name(), this, pchName );
#endif

	m_Tree.Validate( validator, "m_Tree" );

	validator.Pop();
}
#endif

#ifdef DBGFLAG_VALIDATE

template <typename K, typename T, typename I, typename L > 
void CUtlMap<K, T, I, L>::ValidateSelfAndElements( CValidator &validator, const char *pchName )
{
#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), this, pchName );
#else
	validator.Push( typeid(*this).name(), this, pchName );
#endif

	CValidateHelper< T >	functor( validator );

	m_Tree.Validate( validator, "m_Tree" );

	FOR_EACH_MAP_FAST( *this, i )
	{
		Key( i ).Validate( validator, "Keys" );
		functor( Element( i ), "Elements" );
	}

	validator.Pop();
}
#endif

//-----------------------------------------------------------------------------

#endif // UTLMAP_H
