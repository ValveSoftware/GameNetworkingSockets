//========= Copyright Valve Corporation, All rights reserved. =================//
//
// Purpose: index-based hash map container
//			Use FOR_EACH_HASHMAP to iterate through CUtlHashMap.
//
//=============================================================================//

#ifndef UTLHASHMAP_H
#define UTLHASHMAP_H
#pragma once

#include <tier0/dbg.h>
#include "utlvector.h"

#define FOR_EACH_HASHMAP( mapName, iteratorName ) \
	for ( int iteratorName = 0; iteratorName < (mapName).MaxElement(); ++iteratorName ) if ( !(mapName).IsValidIndex( iteratorName ) ) continue; else

//-----------------------------------------------------------------------------
//
// Purpose:	An associative container.  Similar to std::unordered_map,
// but without STL's rather wacky interface.  Also, each item is not a separate
// allocation, so insertion of items can cause existing items to move in memory.
//
// This differs from the one in Steam by not having any default hash or equality
// class.  We will use std::hash and std::equal_to insetad of our own hand-rolled
// versions, which I suspect do not add any value (any more at least).  Valve's
// CDefEquals unfortunately is not exactly the same as std::equal_to in the way
// it handles pointers, so let's require the few uses of hashmaps in use
// to be explicit in the equality operation.
//
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H > 
class CUtlHashMap
{
protected:
	enum ReplaceExisting
	{
		False = 0,
		True = 1,
	};

public:
	using KeyType_t= K;
	using ElemType_t = T;
	using IndexType_t = int;
	using EqualityFunc_t = L;
	using HashFunc_t = H;
	static constexpr IndexType_t kInvalidIndex = -1;

	CUtlHashMap()
	{
		m_cElements = 0;
		m_nMaxElement = 0;
		m_nNeedRehashStart = 0;
		m_nNeedRehashEnd = 0;
		m_nMinBucketMask = 1;
		m_iNodeFreeListHead = kInvalidIndex;
	}

	CUtlHashMap( int cElementsExpected )
	{
		m_cElements = 0;
		m_nMaxElement = 0;
		m_nNeedRehashStart = 0;
		m_nNeedRehashEnd = 0;
		m_nMinBucketMask = 1;
		m_iNodeFreeListHead = kInvalidIndex;
		EnsureCapacity( cElementsExpected );
	}

	~CUtlHashMap()
	{
		Purge();
	}

	void CopyFullHashMap( CUtlHashMap< K, T, L, H > &target ) const
	{
		target.RemoveAll();
		FOR_EACH_HASHMAP( *this, i )
		{
			target.Insert( this->Key( i ), this->Element( i ) );
		}
	}

	// gets particular elements
	ElemType_t &		Element( IndexType_t i )			{ return m_memNodes.Element( i ).m_elem; }
	const ElemType_t &	Element( IndexType_t i ) const		{ return m_memNodes.Element( i ).m_elem; }
	ElemType_t &		operator[]( IndexType_t i )			{ return m_memNodes.Element( i ).m_elem; }
	const ElemType_t &	operator[]( IndexType_t i ) const	{ return m_memNodes.Element( i ).m_elem; }
	const KeyType_t &	Key( IndexType_t i ) const			{ return m_memNodes.Element( i ).m_key; }

	// Num elements
	IndexType_t Count() const								{ return m_cElements; }

	// Max "size" of the vector
	IndexType_t  MaxElement() const							{ return m_nMaxElement; }

	/// Checks if a node is valid and in the map.
	/// NOTE: Do not use this function on the result of Find().  That is overkill
	/// and slower.  Instead, compare the returned index against InvalidIndex().
	/// (Or better use, use one of the methods sue as FindGetPtr() or HasElement()
	/// that makes it unnecessary to deal with indices at all.
	bool  IsValidIndex( IndexType_t i ) const				{ return /* i >= 0 && i < m_nMaxElement */ (unsigned)i < (unsigned)m_nMaxElement && m_memNodes[i].m_iNextNode >= -1; }

	// Invalid index
	static constexpr IndexType_t InvalidIndex()						{ return -1; }

	// Insert method
	IndexType_t  Insert( const KeyType_t &key )								{ return FindOrInsert_Internal( key, ReplaceExisting::True ); }
	IndexType_t  Insert( KeyType_t &&key )									{ return FindOrInsert_Internal( std::move(key), ReplaceExisting::True ); }
	
	// Insert or replace the existing if found (no dupes)
	IndexType_t  Insert( const KeyType_t &key, const ElemType_t &insert )	{ return FindOrInsert_Internal( key, insert, ReplaceExisting::True ); }
	IndexType_t  Insert( const KeyType_t &key, ElemType_t &&insert )		{ return FindOrInsert_Internal( key, std::move(insert), ReplaceExisting::True ); }
	IndexType_t  Insert( KeyType_t &&key, const ElemType_t &insert )		{ return FindOrInsert_Internal( std::move(key), insert, ReplaceExisting::True ); }
	IndexType_t  Insert( KeyType_t &&key, ElemType_t &&insert )				{ return FindOrInsert_Internal( std::move(key), std::move(insert), ReplaceExisting::True ); }

	// Insert or replace the existing if found (no dupes)
	IndexType_t  InsertOrReplace( const KeyType_t &key, const ElemType_t &insert )	{ return FindOrInsert_Internal( key, insert, ReplaceExisting::True ); }
	IndexType_t  InsertOrReplace( const KeyType_t &key, ElemType_t &&insert )		{ return FindOrInsert_Internal( key, std::move(insert), ReplaceExisting::True ); }
	IndexType_t  InsertOrReplace( KeyType_t &&key, const ElemType_t &insert )		{ return FindOrInsert_Internal( std::move(key), insert, ReplaceExisting::True ); }
	IndexType_t  InsertOrReplace( KeyType_t &&key, ElemType_t &&insert )			{ return FindOrInsert_Internal( std::move(key), std::move(insert), ReplaceExisting::True ); }

	// Insert ALWAYS, possibly creating a dupe
	IndexType_t  InsertWithDupes( const KeyType_t &key, const ElemType_t &insert )	{ return InsertWithDupes_Internal( key, insert ); }
	IndexType_t  InsertWithDupes( const KeyType_t &key, ElemType_t &&insert )		{ return InsertWithDupes_Internal( key, std::move(insert) ); }
	IndexType_t  InsertWithDupes( KeyType_t &&key, const ElemType_t &insert )		{ return InsertWithDupes_Internal( std::move(key), insert ); }
	IndexType_t  InsertWithDupes( KeyType_t &&key, ElemType_t &&insert )			{ return InsertWithDupes_Internal( std::move(key), std::move(insert) ); }

	// Find-or-insert method, one-arg - can insert default-constructed element
	// when there is no available copy constructor or assignment operator
	IndexType_t  FindOrInsert( const KeyType_t &key )						{ return FindOrInsert_Internal( key, ReplaceExisting::False ); }
	IndexType_t  FindOrInsert( KeyType_t &&key )							{ return FindOrInsert_Internal( std::move(key), ReplaceExisting::False ); }

	// Find-or-insert method, two-arg - can insert an element when there is no
	// copy constructor for the type (but does require assignment operator)
	IndexType_t  FindOrInsert( const KeyType_t &key, const ElemType_t &insert )	{ return FindOrInsert_Internal( key, insert, ReplaceExisting::False ); }
	IndexType_t  FindOrInsert( const KeyType_t &key, ElemType_t &&insert )		{ return FindOrInsert_Internal( key, std::move(insert), ReplaceExisting::False ); }
	IndexType_t  FindOrInsert( KeyType_t &&key, const ElemType_t &insert )		{ return FindOrInsert_Internal( std::move(key), insert, ReplaceExisting::False ); }
	IndexType_t  FindOrInsert( KeyType_t &&key, ElemType_t &&insert )			{ return FindOrInsert_Internal( std::move(key), std::move(insert), ReplaceExisting::False ); }

	// Find key, insert with default value if not found.  Returns pointer to the
	// element
	ElemType_t *FindOrInsertGetPtr( const KeyType_t &key )
	{
		IndexType_t i = FindOrInsert(key);
		return &m_memNodes.Element( i ).m_elem;
	}
	ElemType_t *FindOrInsertGetPtr( KeyType_t &&key )
	{
		IndexType_t i = FindOrInsert( std::move( key ) );
		return &m_memNodes.Element( i ).m_elem;
	}

	// Finds an element.  Returns index of element, or InvalidIndex() if not found
	IndexType_t  Find( const KeyType_t &key ) const;
	
	// Finds an element, returns pointer to element or NULL if not found
	ElemType_t *FindGetPtr( const KeyType_t &key )
	{
		IndexType_t i = Find(key);
		return i == kInvalidIndex ? nullptr : &m_memNodes.Element( i ).m_elem;
	}
	const ElemType_t *FindGetPtr( const KeyType_t &key ) const
	{
		IndexType_t i = Find(key);
		return i == kInvalidIndex ? nullptr : &m_memNodes.Element( i ).m_elem;
	}

	/// Returns true if the specified *key* (not the "element"!!!) can be found
	/// This name is definitely unfortunate, but remains because of compatibility
	/// with other containers and also other Valve codebases.
	bool HasElement( const KeyType_t &key ) const { return Find( key ) != kInvalidIndex; }

	// Finds an exact key/value match, even with duplicate keys. Requires operator== for ElemType_t.
	IndexType_t  FindExact( const KeyType_t &key, const ElemType_t &elem ) const;

	// Find next element with same key
	IndexType_t  NextSameKey( IndexType_t i ) const;

	void EnsureCapacity( int num );
	
	//
	// DANGER DANGER
	// This doesn't really work if you pass a temporary to defaultValue!!!
	//
	const ElemType_t &FindElement( const KeyType_t &key, const ElemType_t &defaultValue ) const
	{
		IndexType_t i = Find( key );
		if ( i == kInvalidIndex )
			return defaultValue;
		return Element( i );
	}

	void RemoveAt( IndexType_t i );
	bool Remove( const KeyType_t &key )
	{
		int iMap = Find( key );
		if ( iMap != kInvalidIndex )
		{
			RemoveAt( iMap );
			return true;
		}
		return false;
	}
	void RemoveAll();
	void Purge();

	// call delete on each element (as a pointer) and then purge
	void PurgeAndDeleteElements()
	{
		FOR_EACH_HASHMAP( *this, i )
			delete this->Element(i);
		Purge();
	}

	void Swap( CUtlHashMap< K, T, L, H > &that );

protected:
	template < typename pf_key >
	IndexType_t  FindOrInsert_Internal( pf_key &&key, ReplaceExisting bReplace );

	template < typename pf_key, typename pf_elem >
	IndexType_t  FindOrInsert_Internal( pf_key &&key, pf_elem &&insert, ReplaceExisting bReplace );

	template < typename pf_key, typename pf_elem >
	IndexType_t  InsertWithDupes_Internal( pf_key &&key, pf_elem &&insert );

	template < typename KeyType_universal_ref >
	IndexType_t InsertUnconstructed( KeyType_universal_ref &&key, IndexType_t *pExistingIndex, bool bAllowDupes );

	inline IndexType_t FreeNodeIDToIndex( IndexType_t i ) const	{ return (0-i)-3; }
	inline IndexType_t FreeNodeIndexToID( IndexType_t i ) const	{ return (-3)-i; }

	int FindInBucket( int iBucket, const KeyType_t &key ) const;
	int AllocNode();
	void RehashNodesInBucket( int iBucket );
	void LinkNodeIntoBucket( int iBucket, int iNewNode );
	bool RemoveNodeFromBucket( int iBucket, int iNodeToRemove );
	void IncrementalRehash();

	struct HashBucket_t
	{
		IndexType_t m_iNode;
	};
	CUtlVector<HashBucket_t> m_vecHashBuckets;

	struct Node_t
	{
		KeyType_t m_key;
		ElemType_t m_elem;
		int m_iNextNode;
	};

	CUtlMemory<Node_t> m_memNodes;
	IndexType_t m_iNodeFreeListHead;

	IndexType_t m_cElements;
	IndexType_t m_nMaxElement;
	IndexType_t m_nNeedRehashStart, m_nNeedRehashEnd; // Range of buckets that need to be rehashed
	IndexType_t m_nMinBucketMask; // Mask at the time we last finished completed rehashing.  So no need to check hash buckets based on mask smaller than this.
	EqualityFunc_t m_EqualityFunc;
	HashFunc_t m_HashFunc;

public:
	//
	// Range-based for loop iteration over the map.  You can iterate
	// over the keys, the values ("elements"), or both ("items").
	// This naming style comes from Python.
	//
	// Examples:
	//
	// CUtlHashMap<uint64,CUtlString> map;
	//
	// Iterate over the keys.  Your loop variable will receive
	// const KeyType_t &.  (You can use an ordinary KeyType_t
	// variable for small KeyType_t.)
	//
	//   for ( uint64 k: map.IterKeys() ) { ... }
	//
	// Iterate over the values ("elements").  Your loop variable will receive
	// [const] ElemType_t &.  (You can use an ordinary ElemType_t
	// variable for small ElemType_t if you don't need to modify the element.
	//   for ( CUtlString &v: map.IterValues() )
	//
	// Iterate over the "items" (key/value pairs) in the map.  Your
	// loop variable will receive an an ItemRef or MutableItemRef.  This is
	// a small proxy object that is very fast to copy, so you
	// will usually use plain "auto" (not auto&).  (Like std::map iterators,
	// using the actual type would be really messy and verbose, since it's a
	// template type, hence using auto.)
	//
	//   for ( auto item: map.IterItems() )
	//   {
	//       int i = item.Index();
	//       uint64 k = item.Key();
	//       CUtlString &v = item.Value();
	//   }

	// A reference to a key/value pair in a map
	class ItemRef
	{
	protected:
		Node_t &m_node;
		const int m_idx;
	public:
		inline ItemRef( const CUtlHashMap< K, T, L, H > &map, int idx ) : m_node( const_cast< Node_t &>( map.m_memNodes[idx] ) ), m_idx(idx) {}
		ItemRef( const ItemRef &x ) = default;
		inline int Index() const { return m_idx; }
		inline const KeyType_t &Key() const { return m_node.m_key; }
		inline const ElemType_t &Element() const { return m_node.m_elem; }
	};
	struct MutableItemRef : ItemRef
	{
		inline MutableItemRef( CUtlHashMap< K, T, L, H > &map, int idx ) : ItemRef( map, idx ) {}
		MutableItemRef( const MutableItemRef &x ) = default;
		using ItemRef::Element; // const reference
		inline ElemType_t &Element() const { return this->m_node.m_elem; } // non-const reference
	};

	// Base class iterator
	class Iterator
	{
	protected:
		CUtlHashMap<K, T, L, H > &m_map;
		int m_idx;
	public:
		inline Iterator( const CUtlHashMap< K, T, L, H > &map, int idx ) : m_map( const_cast< CUtlHashMap< K, T, L, H > &>( map ) ), m_idx(idx) {}
		Iterator( const Iterator &x ) = default;
		inline bool operator==( const Iterator &x ) const { return &m_map == &x.m_map && m_idx == x.m_idx; } // Comparing the map reference is probably not necessary in 99% of cases, but needed to be correct
		inline bool operator!=( const Iterator &x ) const { return &m_map != &x.m_map || m_idx != x.m_idx; }
		inline void operator++()
		{
			if ( m_idx == kInvalidIndex )
				return;
			do
			{
				++m_idx;
				if ( m_idx >= m_map.m_nMaxElement )
				{
					m_idx = kInvalidIndex;
					break;
				}
			} while ( m_map.m_memNodes[m_idx].m_iNextNode < -1 );
		}
	};
	struct MutableIterator : Iterator
	{
		inline MutableIterator( const MutableIterator &x ) = default;
		inline MutableIterator( CUtlHashMap< K, T, L, H > &map, int idx ) : Iterator( map, idx ) {}
	};
	struct KeyIterator : Iterator
	{
		using Iterator::Iterator;
		inline const KeyType_t &operator*() { return this->m_map.m_memNodes[this->m_idx].m_key; }
	};
	struct ConstValueIterator : Iterator
	{
		using Iterator::Iterator;
		inline const ElemType_t &operator*() { return this->m_map.m_memNodes[this->m_idx].m_elem; }
	};
	struct MutableValueIterator : MutableIterator
	{
		using MutableIterator::MutableIterator;
		inline ElemType_t &operator*() { return this->m_map.m_memNodes[this->m_idx].m_elem; }
	};
	struct ConstItemIterator : Iterator
	{
		using Iterator::Iterator;
		inline ItemRef operator*() { return ItemRef( this->m_map, this->m_idx ); }
	};
	struct MutableItemIterator : public MutableIterator
	{
		using MutableIterator::MutableIterator;
		inline MutableItemRef operator*() { return MutableItemRef( this->m_map, this->m_idx ); }
	};

	// Internal type used by the IterXxx functions.  You normally won't use
	// this directly, because it will be consumed by the machinery of the
	// range-based for loop.
	template <typename TIterator>
	class Range
	{
		CUtlHashMap< K, T, L, H > &m_map;
	public:
		Range( const CUtlHashMap< K, T, L, H > &map ) : m_map( const_cast< CUtlHashMap< K, T, L, H > &>( map ) ) {}
		TIterator begin() const
		{
			int idx;
			if ( m_map.m_cElements <= 0 )
			{
				idx = kInvalidIndex;
			}
			else
			{
				idx = 0;
				while ( m_map.m_memNodes[idx].m_iNextNode < -1 )
				{
					++idx;
					Assert( idx < m_map.m_nMaxElement ); // Or else how is m_map.m_cElements > 0?
				}
			}
			return TIterator( m_map, idx );
		}
		TIterator end() const { return TIterator( m_map, kInvalidIndex ); }
	};

	/// Iterate over the keys.  You will receive a reference to the key/
	Range<KeyIterator> IterKeys() const { return Range<KeyIterator>(*this); }

	/// Iterate over the values ("elements").  You will receive a reference to the value.
	Range<ConstValueIterator> IterValues() const { return Range<ConstValueIterator>(*this); }
	Range<MutableValueIterator> IterValues() { return Range<MutableValueIterator>(*this); }

	/// Iterate over the "items" (key/value pairs).  You will receive a small reference
	/// object that is cheap to copy.
	Range<ConstItemIterator> IterItems() const { return Range<ConstItemIterator>(*this); }
	Range<MutableItemIterator> IterItems() { return Range<MutableItemIterator>(*this); }

};


//-----------------------------------------------------------------------------
// Purpose: inserts and constructs a key into the map.
// Element member is left unconstructed (to be copy constructed or default-constructed by a wrapper function)
// Supports both copy and move constructors for KeyType_t via universal refs and type deduction
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
template <typename KeyType_universal_ref>
inline int CUtlHashMap<K,T,L,H>::InsertUnconstructed( KeyType_universal_ref &&key, int *piNodeExistingIfDupe, bool bAllowDupes )
{
	// make sure we have room in the hash table
	if ( m_cElements >= m_vecHashBuckets.Count() )
		EnsureCapacity( MAX( 16, m_vecHashBuckets.Count() * 2 ) );
	if ( m_cElements >= m_memNodes.Count() )
		m_memNodes.Grow( m_memNodes.Count() * 2 );

	// Do a bit of cleanup, if table is not already clean.  If statement here
	// avoids the function call in the (hopefully common!) case that the table
	// is already clean
	if ( m_nNeedRehashStart < m_nNeedRehashEnd )
		IncrementalRehash();

	// hash the item
	int hash = (int)m_HashFunc( key );

	// Make sure any buckets that might contain duplicates have been rehashed, so that we only need
	// to check one bucket below.  Also, we have the invariant that all duplicates (if they are allowed)
	// are in the same bucket.  This rehashing might not actually be necessary, because we might have
	// already done it.  But it's probably not worth keeping track of.  1.) The number of back probes
	// in normal usage is at most 1.  2.) If hashing is reasonably effective, then the number of items
	// in each bucket should be small.
	int nBucketMaskMigrate = ( m_vecHashBuckets.Count() >> 1 ) - 1;
	while ( nBucketMaskMigrate >= m_nMinBucketMask )
	{
		int iBucketMigrate = hash & nBucketMaskMigrate;
		if ( iBucketMigrate < m_nNeedRehashStart )
			break;
		RehashNodesInBucket( iBucketMigrate );
		nBucketMaskMigrate >>= 1;
	}

	int iBucket = hash & ( m_vecHashBuckets.Count()-1 );

	// return existing node without insert, if duplicates are not permitted
	if ( !bAllowDupes )
	{
		// look in the bucket to see if we have a conflict
		IndexType_t iNode = FindInBucket( iBucket, key );
		if ( iNode != kInvalidIndex )
		{
			if ( piNodeExistingIfDupe )
			{
				*piNodeExistingIfDupe = iNode;
			}
			return kInvalidIndex;
		}
	}

	// make an item
	int iNewNode = AllocNode();
	m_memNodes[iNewNode].m_iNextNode = kInvalidIndex;
	Construct( &m_memNodes[iNewNode].m_key, std::forward<KeyType_universal_ref>( key ) );
	// Note: m_elem remains intentionally unconstructed here
	// Note: key may have been moved depending on which constructor was called.

	// link ourselves in
	//	::OutputDebugStr( CFmtStr( "insert %d into bucket %d\n", key, iBucket ).Access() );
	LinkNodeIntoBucket( iBucket, iNewNode );

    // Initialized to placate the compiler's uninitialized value checking.
    if ( piNodeExistingIfDupe )
    {
        *piNodeExistingIfDupe = kInvalidIndex;
    }
    
	// return the new node
	return iNewNode;
}


//-----------------------------------------------------------------------------
// Purpose: inserts a default item into the map, no change if key already exists
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
template < typename pf_key >
inline int CUtlHashMap<K,T,L,H>::FindOrInsert_Internal( pf_key &&key, ReplaceExisting bReplace )
{
	int iNodeExisting;
	int iNodeInserted = InsertUnconstructed( std::forward<pf_key>( key ), &iNodeExisting, false /*no duplicates allowed*/ );
	// If replacing, stomp the existing one
	if ( bReplace && iNodeExisting != kInvalidIndex )
	{
		Destruct( &m_memNodes[ iNodeExisting ].m_elem );
		ValueInitializeConstruct( &m_memNodes[ iNodeExisting ].m_elem );
	}
	else if ( iNodeInserted != kInvalidIndex )
	{
		ValueInitializeConstruct( &m_memNodes[ iNodeInserted ].m_elem );
		return iNodeInserted;
	}
	return iNodeExisting;
}


//-----------------------------------------------------------------------------
// Purpose: inserts an item into the map, no change if key already exists
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
template < typename pf_key, typename pf_elem >
inline int CUtlHashMap<K,T,L,H>::FindOrInsert_Internal( pf_key &&key, pf_elem &&elem, ReplaceExisting bReplace )
{
	int iNodeExisting;
	int iNodeInserted = InsertUnconstructed( std::forward<pf_key>( key ), &iNodeExisting, false /*no duplicates allowed*/ );
	// If replacing, stomp the existing one
	if ( bReplace && iNodeExisting != kInvalidIndex )
	{
		Destruct( &m_memNodes[ iNodeExisting ].m_elem );
		Construct( &m_memNodes[ iNodeExisting ].m_elem, std::forward<pf_elem>( elem ) );
	}
	else if ( iNodeInserted != kInvalidIndex )
	{
		Construct( &m_memNodes[ iNodeInserted ].m_elem, std::forward<pf_elem>( elem ) );
		return iNodeInserted;
	}
	return iNodeExisting;
}

//-----------------------------------------------------------------------------
// Purpose: inserts element no matter what, even if key already exists
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
template < typename pf_key, typename pf_elem >
inline int CUtlHashMap<K,T,L,H>::InsertWithDupes_Internal( pf_key &&key, pf_elem &&insert )
{
	int iNodeInserted = InsertUnconstructed( std::forward<pf_key>( key ), NULL, true /*duplicates allowed!*/ ); // copies key
	if ( iNodeInserted != kInvalidIndex )
	{
		Construct( &m_memNodes[ iNodeInserted ].m_elem, std::forward<pf_elem>( insert ) );
	}
	return iNodeInserted;
}


//-----------------------------------------------------------------------------
// Purpose: grows the map to fit the specified amount
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::EnsureCapacity( int amount )
{
	m_memNodes.EnsureCapacity( amount );
	// ::OutputDebugStr( CFmtStr( "grown m_memNodes from %d to %d\n", m_cElements, m_memNodes.Count() ).Access() );

	if ( amount <= m_vecHashBuckets.Count() )
		return;
	int cBucketsNeeded = MAX( 16, m_vecHashBuckets.Count() );
	while ( cBucketsNeeded < amount )
		cBucketsNeeded <<= 1;
	DbgAssert( ( cBucketsNeeded & (cBucketsNeeded-1) ) == 0 ); // It's a power of 2

	// ::OutputDebugStr( CFmtStr( "grown m_vecHashBuckets from %d to %d\n", m_vecHashBuckets.Count(), cBucketsNeeded ).Access() );

	// grow the hash buckets
	int grow = cBucketsNeeded - m_vecHashBuckets.Count();
	int iFirst = m_vecHashBuckets.AddMultipleToTail( grow );
	// clear all the new data to invalid bits
	memset( &m_vecHashBuckets[iFirst], 0xFFFFFFFF, grow*sizeof(m_vecHashBuckets[iFirst]) );
	DbgAssert( m_vecHashBuckets.Count() == cBucketsNeeded );

	// Mark appropriate range for rehashing
	if ( m_cElements > 0 )
	{
		// we'll have to rehash, all the buckets that existed before growth
		m_nNeedRehashStart = 0;
		m_nNeedRehashEnd = iFirst;
	}
	else
	{
		// no elements - no rehashing!
		m_nNeedRehashStart = m_vecHashBuckets.Count();
		m_nNeedRehashEnd = m_nNeedRehashStart;
		m_nMinBucketMask = m_nNeedRehashStart-1;
	}
}


//-----------------------------------------------------------------------------
// Purpose: gets a new node, from the free list if possible
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline int CUtlHashMap<K,T,L,H>::AllocNode()
{
	// if we're out of free elements, get the max
	if ( m_cElements == m_nMaxElement )
	{
		m_cElements++;
		return m_nMaxElement++;
	}

	// pull from the free list
	DbgAssert( m_iNodeFreeListHead != kInvalidIndex );
	int iNewNode = m_iNodeFreeListHead;
	m_iNodeFreeListHead = FreeNodeIDToIndex( m_memNodes[iNewNode].m_iNextNode );
	m_cElements++;
	return iNewNode;
}


//-----------------------------------------------------------------------------
// Purpose: takes a bucket of nodes and re-hashes them into a more optimal bucket
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::RehashNodesInBucket( int iBucketSrc )
{

	// walk the list of items, re-hashing them
	IndexType_t *pLink = &m_vecHashBuckets[iBucketSrc].m_iNode;
	for (;;)
	{
		IndexType_t iNode = *pLink;
		if ( iNode == kInvalidIndex )
			break;
		Node_t &node = m_memNodes[iNode];
		DbgAssert( node.m_iNextNode != iNode );

		// work out where the node should go
		int hash = (int)m_HashFunc( node.m_key );
		int iBucketDest = hash & (m_vecHashBuckets.Count()-1);

		// if the hash bucket has changed, move it
		if ( iBucketDest != iBucketSrc )
		{
			//	::OutputDebugStr( CFmtStr( "moved key %d from bucket %d to %d\n", key, iBucketSrc, iBucketDest ).Access() );

			// remove from this bucket list
			*pLink = node.m_iNextNode;

			// link into new bucket list
			LinkNodeIntoBucket( iBucketDest, iNode );
		}
		else
		{
			pLink = &node.m_iNextNode;
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: searches for an item by key, returning the index handle
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline int CUtlHashMap<K,T,L,H>::Find( const KeyType_t &key ) const
{
	if ( m_cElements == 0 )
		return kInvalidIndex;

	// Rehash incrementally.  Note that this is really a "const"
	// function, since it is only shuffling around the buckets.  The
	// items do not move in memory, and their index does not change.
	// So this can be called during iteration, etc.  The buckets are
	// invisible to the app code.
	//
	// It's a better tradeoff to make this function slightly slower
	// until we get rehashed, to make sure that we do eventually get
	// rehashed, even if we have stopped Inserting.  This minimizes
	// the number of back probes that need to be made.
	//
	// NOTE: This means that you cannot call the "read-only" Find()
	// function from different threads at the same time!
	if ( m_nNeedRehashStart < m_nNeedRehashEnd )
		(const_cast<CUtlHashMap<K,T,L,H> *>( this ))->IncrementalRehash();

	// hash the item
	int hash = (int)m_HashFunc( key );

	// find the bucket
	int cBucketsMask = m_vecHashBuckets.Count()-1;
	int iBucket = hash & cBucketsMask;
	do 
	{

		// Look in the bucket for the item
		int iNode = FindInBucket( iBucket, key );
		if ( iNode != kInvalidIndex )
			return iNode;

		// Not found.  Might be in an older bucket.
		cBucketsMask >>= 1;
		if ( cBucketsMask < m_nMinBucketMask )
			break;
		iBucket = hash & cBucketsMask;
	} while ( iBucket >= m_nNeedRehashStart );

	return kInvalidIndex;	
}


//-----------------------------------------------------------------------------
// Purpose: searches for an item by key and element equality, returning the index handle
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H>
inline int CUtlHashMap<K, T, L, H>::FindExact( const KeyType_t &key, const ElemType_t &elem ) const
{
	int iNode = Find( key );
	while ( iNode != kInvalidIndex )
	{
		if ( elem == m_memNodes[iNode].m_elem )
			return iNode;
		iNode = NextSameKey( iNode );
	}
	return kInvalidIndex;
}


//-----------------------------------------------------------------------------
// Purpose: find the next element with the same key, if insertwithdupes was used
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H>
inline int CUtlHashMap<K, T, L, H>::NextSameKey( IndexType_t i ) const
{
	if ( m_memNodes.IsIdxValid( i ) )
	{
		const KeyType_t &key = m_memNodes[i].m_key;
		IndexType_t iNode = m_memNodes[i].m_iNextNode;
		while ( iNode != kInvalidIndex )
		{
			DbgAssert( iNode < m_nMaxElement );

			// equality check
			if ( m_EqualityFunc( key, m_memNodes[iNode].m_key ) )
				return iNode;

			iNode = m_memNodes[iNode].m_iNextNode;
		}
	}
	return kInvalidIndex;
}


//-----------------------------------------------------------------------------
// Purpose: searches for an item by key, returning the index handle
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline int CUtlHashMap<K,T,L,H>::FindInBucket( int iBucket, const KeyType_t &key ) const
{
	IndexType_t iNode = m_vecHashBuckets[iBucket].m_iNode;
	while ( iNode != kInvalidIndex )
	{
		DbgAssert( iNode < m_nMaxElement );

		// equality check
		const Node_t &node = m_memNodes[iNode];
		if ( m_EqualityFunc( key, node.m_key ) )
			return iNode;

		iNode = node.m_iNextNode;
	}

	return kInvalidIndex;
}


//-----------------------------------------------------------------------------
// Purpose: links a node into a bucket
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::LinkNodeIntoBucket( int iBucket, int iNewNode )
{
	// add into the start of the bucket's list
	m_memNodes[iNewNode].m_iNextNode = m_vecHashBuckets[iBucket].m_iNode;
	m_vecHashBuckets[iBucket].m_iNode = iNewNode;
}


//-----------------------------------------------------------------------------
// Purpose: removes a single item from the map
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::RemoveAt( IndexType_t i )
{
	if ( !IsValidIndex( i ) )
	{
		Assert( false );
		return;
	}

	// Rehash incrementally
	if ( m_nNeedRehashStart < m_nNeedRehashEnd )
		IncrementalRehash();

	// unfortunately, we have to re-hash to find which bucket we're in
	int hash = (int)m_HashFunc( m_memNodes[i].m_key );
	int nBucketMask = m_vecHashBuckets.Count()-1;
	if ( RemoveNodeFromBucket( hash & nBucketMask, i ) )
		return;

	// wasn't found; look in older buckets
	for (;;)
	{
		nBucketMask >>= 1;
		if ( nBucketMask < m_nMinBucketMask )
			break;
		int iBucket = hash & nBucketMask;
		if ( iBucket < m_nNeedRehashStart )
			break;
		if ( RemoveNodeFromBucket( iBucket, i ) )
			return;
	}

	// never found, container is busted
	Assert( false );
}


//-----------------------------------------------------------------------------
// Purpose: removes a node from the bucket, return true if it was found
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline bool CUtlHashMap<K,T,L,H>::RemoveNodeFromBucket( IndexType_t iBucket, int iNodeToRemove )
{
	// walk the list of items
	IndexType_t *pLink = &m_vecHashBuckets[iBucket].m_iNode;
	for (;;)
	{
		IndexType_t iNode = *pLink;
		if ( iNode == kInvalidIndex )
			break;
		Node_t &node = m_memNodes[iNode];
		DbgAssert( node.m_iNextNode != iNode );

		if ( iNodeToRemove == iNode )
		{
			// found it, remove
			*pLink = node.m_iNextNode;
			Destruct( &node.m_key );
			Destruct( &node.m_elem );

			// link into free list
			node.m_iNextNode = FreeNodeIndexToID( m_iNodeFreeListHead );
			m_iNodeFreeListHead = iNode;
			m_cElements--;
			if ( m_cElements == 0 )
			{
				// No items left in container, so no rehashing necessary
				m_nNeedRehashStart = m_vecHashBuckets.Count();
				m_nNeedRehashEnd = m_nNeedRehashStart;
				m_nMinBucketMask = m_vecHashBuckets.Count()-1;
			}
			return true;
		}

		pLink = &node.m_iNextNode;
	}

	return false;
}


//-----------------------------------------------------------------------------
// Purpose: removes all items from the hash map
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::RemoveAll()
{
	if ( m_cElements > 0 )
	{
		FOR_EACH_HASHMAP( *this, i )
		{
			Node_t &node = m_memNodes[i];
			Destruct( &node.m_key );
			Destruct( &node.m_elem );
		}

		m_cElements = 0;
		m_nMaxElement = 0;
		m_iNodeFreeListHead = kInvalidIndex;
		m_nNeedRehashStart = m_vecHashBuckets.Count();
		m_nNeedRehashEnd = m_nNeedRehashStart;
		DbgAssert( m_vecHashBuckets.Count() >= 2 );
		m_nMinBucketMask = m_vecHashBuckets.Count()-1;
		memset( m_vecHashBuckets.Base(), 0xFF, m_vecHashBuckets.Count() * sizeof(HashBucket_t) );
	}
}


//-----------------------------------------------------------------------------
// Purpose: removes all items from the hash map and frees all memory
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::Purge()
{
	if ( m_cElements > 0 )
	{
		FOR_EACH_HASHMAP( *this, i )
		{
			Node_t &node = m_memNodes[i];
			Destruct( &node.m_key );
			Destruct( &node.m_elem );
		}
	}

	m_cElements = 0;
	m_nMaxElement = 0;
	m_iNodeFreeListHead = kInvalidIndex;
	m_nNeedRehashStart = 0;
	m_nNeedRehashEnd = 0;
	m_nMinBucketMask = 1;
	m_vecHashBuckets.Purge();
	m_memNodes.Purge();
}


//-----------------------------------------------------------------------------
// Purpose: rehashes buckets
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::IncrementalRehash()
{
	// Each call site should check this, to avoid the function call in the
	// common case where the table is already clean.
	DbgAssert( m_nNeedRehashStart < m_nNeedRehashEnd );

	do
	{
		int iBucketSrc = m_nNeedRehashStart;
		++m_nNeedRehashStart;

		// Bucket empty?
		if ( m_vecHashBuckets[iBucketSrc].m_iNode != kInvalidIndex )
		{
			RehashNodesInBucket( iBucketSrc );

			// only actively do one - don't want to do it too fast since we may be on a rapid growth path
			if ( m_nNeedRehashStart < m_nNeedRehashEnd )
				return;
			break;
		}
	} while ( m_nNeedRehashStart < m_nNeedRehashEnd );

	// We're done; don't need any bits anymore
	DbgAssert( m_vecHashBuckets.Count() >= 2 );
	m_nNeedRehashStart = m_vecHashBuckets.Count();
	m_nNeedRehashEnd = m_nNeedRehashStart;
	m_nMinBucketMask = m_vecHashBuckets.Count()-1;
}


//-----------------------------------------------------------------------------
// Purpose: swaps with another hash map
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::Swap( CUtlHashMap<K,T,L,H> &that )
{
	m_vecHashBuckets.Swap( that.m_vecHashBuckets );
	m_memNodes.Swap( that.m_memNodes );
	SWAP( m_iNodeFreeListHead, that.m_iNodeFreeListHead );
	SWAP( m_cElements, that.m_cElements );
	SWAP( m_nMaxElement, that.m_nMaxElement );
	SWAP( m_nNeedRehashStart, that.m_nNeedRehashStart );
	SWAP( m_nNeedRehashEnd, that.m_nNeedRehashEnd );
	SWAP( m_nMinBucketMask, that.m_nMinBucketMask );
}

#endif // UTLHASHMAP_H
