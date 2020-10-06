//========= Copyright Valve Corporation, All rights reserved. =================//
//
// Purpose: index-based hash map container
//			Use FOR_EACH_HASHMAP to iterate through CUtlHashMap.
//
//=============================================================================//

#ifndef UTLHASHMAP_H
#define UTLHASHMAP_H

#ifdef _WIN32
#pragma once
#endif

#include "tier0/dbg.h"
#include "tier1/bitstring.h"
#include "tier1/utlvector.h"

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
		m_nMinRehashedBucket = kInvalidIndex;
		m_nMaxRehashedBucket = kInvalidIndex;
		m_iNodeFreeListHead = kInvalidIndex;
	}

	CUtlHashMap( int cElementsExpected )
	{
		m_cElements = 0;
		m_nMaxElement = 0;
		m_nMinRehashedBucket = kInvalidIndex;
		m_nMaxRehashedBucket = kInvalidIndex;
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
	void UnlinkNodeFromBucket( int iBucket, int iNewNode );
	bool RemoveNodeFromBucket( int iBucket, int iNodeToRemove );
	void IncrementalRehash();

	struct HashBucket_t
	{
		IndexType_t m_iNode;
	};
	CUtlVector<HashBucket_t> m_vecHashBuckets;

	CBitString m_bitsMigratedBuckets;

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
	IndexType_t m_nMinRehashedBucket, m_nMaxRehashedBucket;
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

	// rehash incrementally
	IncrementalRehash();

	// hash the item
	auto hash = m_HashFunc( key );

	// migrate data forward, if necessary
	int cBucketsToModAgainst = m_vecHashBuckets.Count() >> 1;
	int iBucket = basetypes::ModPowerOf2(hash, cBucketsToModAgainst);
	DbgAssert( m_nMinRehashedBucket > 0 ); // The IncrementalRehash() above prevents this case
	while ( iBucket >= m_nMinRehashedBucket
		&& !m_bitsMigratedBuckets.GetBit( iBucket ) )
	{
		RehashNodesInBucket( iBucket );
		cBucketsToModAgainst >>= 1;
		iBucket = basetypes::ModPowerOf2(hash, cBucketsToModAgainst);
	}

	// return existing node without insert, if duplicates are not permitted
	if ( !bAllowDupes && m_cElements )
	{
		// look in the bucket to see if we have a conflict
		int iBucket2 = basetypes::ModPowerOf2( hash, m_vecHashBuckets.Count() );
		IndexType_t iNode = FindInBucket( iBucket2, key );
		if ( piNodeExistingIfDupe )
		{
			*piNodeExistingIfDupe = iNode;
		}
		if ( iNode != kInvalidIndex )
		{
			return kInvalidIndex;
		}
	}

	// make an item
	int iNewNode = AllocNode();
	m_memNodes[iNewNode].m_iNextNode = kInvalidIndex;
	Construct( &m_memNodes[iNewNode].m_key, std::forward<KeyType_universal_ref>( key ) );
	// Note: m_elem remains intentionally unconstructed here
	// Note: key may have been moved depending on which constructor was called.

	iBucket = basetypes::ModPowerOf2( hash, m_vecHashBuckets.Count() );

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
		cBucketsNeeded *= 2;

	// ::OutputDebugStr( CFmtStr( "grown m_vecHashBuckets from %d to %d\n", m_vecHashBuckets.Count(), cBucketsNeeded ).Access() );

	// grow the hash buckets
	int grow = cBucketsNeeded - m_vecHashBuckets.Count();
	int iFirst = m_vecHashBuckets.AddMultipleToTail( grow );
	// clear all the new data to invalid bits
	memset( &m_vecHashBuckets[iFirst], 0xFFFFFFFF, grow*sizeof(m_vecHashBuckets[iFirst]) );
	DbgAssert( basetypes::IsPowerOf2( m_vecHashBuckets.Count() ) );

	// we'll have to rehash, all the buckets that existed before growth
	m_nMinRehashedBucket = 0;
	m_nMaxRehashedBucket = iFirst;
	if ( m_cElements > 0 )
	{
		// remove all the current bits
		m_bitsMigratedBuckets.Resize( 0 );
		// re-add new bits; these will all be reset to 0
		m_bitsMigratedBuckets.Resize( m_vecHashBuckets.Count() );
	}
	else
	{
		// no elements - no rehashing
		m_nMinRehashedBucket = m_vecHashBuckets.Count();
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
	// mark us as migrated
	m_bitsMigratedBuckets.SetBit( iBucketSrc );

	// walk the list of items, re-hashing them
	IndexType_t iNode = m_vecHashBuckets[iBucketSrc].m_iNode;
	while ( iNode != kInvalidIndex )
	{
		IndexType_t iNodeNext = m_memNodes[iNode].m_iNextNode;
		DbgAssert( iNodeNext != iNode );

		// work out where the node should go
		const KeyType_t &key = m_memNodes[iNode].m_key;
		auto hash = m_HashFunc( key );
		int iBucketDest = basetypes::ModPowerOf2( hash, m_vecHashBuckets.Count() );

		// if the hash bucket has changed, move it
		if ( iBucketDest != iBucketSrc )
		{
			//	::OutputDebugStr( CFmtStr( "moved key %d from bucket %d to %d\n", key, iBucketSrc, iBucketDest ).Access() );

			// remove from this bucket list
			UnlinkNodeFromBucket( iBucketSrc, iNode );

			// link into new bucket list
			LinkNodeIntoBucket( iBucketDest, iNode );
		}
		iNode = iNodeNext;
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

	// hash the item
	auto hash = m_HashFunc( key );

	// find the bucket
	int cBucketsToModAgainst = m_vecHashBuckets.Count();
	int iBucket = basetypes::ModPowerOf2( hash, cBucketsToModAgainst );

	// look in the bucket for the item
	int iNode = FindInBucket( iBucket, key );
	if ( iNode != kInvalidIndex )
		return iNode;

	// stop before calling ModPowerOf2( hash, 0 ), which just returns the 32-bit hash, overflowing m_vecHashBuckets
	IndexType_t cMinBucketsToModAgainst = MAX( 1, m_nMinRehashedBucket );

	// not found? we may have to look in older buckets
	cBucketsToModAgainst >>= 1;
	while ( cBucketsToModAgainst >= cMinBucketsToModAgainst)
	{
		iBucket = basetypes::ModPowerOf2( hash, cBucketsToModAgainst );

		if ( !m_bitsMigratedBuckets.GetBit( iBucket ) )
		{
			int iNode2 = FindInBucket( iBucket, key );
			if ( iNode2 != kInvalidIndex )
				return iNode2;
		}

		cBucketsToModAgainst >>= 1;
	}

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
		DbgAssert( iNode < m_nMaxElement );
		while ( iNode != kInvalidIndex )
		{
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
	if ( m_vecHashBuckets[iBucket].m_iNode != kInvalidIndex )
	{
		IndexType_t iNode = m_vecHashBuckets[iBucket].m_iNode;
		DbgAssert( iNode < m_nMaxElement );
		while ( iNode != kInvalidIndex )
		{
			// equality check
			if ( m_EqualityFunc( key, m_memNodes[iNode].m_key ) )
				return iNode;

			iNode = m_memNodes[iNode].m_iNextNode;
		}
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
// Purpose: unlinks a node from the bucket
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::UnlinkNodeFromBucket( int iBucket, int iNodeToUnlink )
{
	int iNodeNext = m_memNodes[iNodeToUnlink].m_iNextNode;

	// if it's the first node, just update the bucket to point to the new place
	int iNode = m_vecHashBuckets[iBucket].m_iNode;
	if ( iNode == iNodeToUnlink )
	{
		m_vecHashBuckets[iBucket].m_iNode = iNodeNext;
		return;
	}

	// walk the list to find where
	while ( iNode != kInvalidIndex )
	{
		if ( m_memNodes[iNode].m_iNextNode == iNodeToUnlink )
		{
			m_memNodes[iNode].m_iNextNode = iNodeNext;
			return;
		}
		iNode = m_memNodes[iNode].m_iNextNode;
	}

	// should always be valid to unlink
	DbgAssert( false );
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

	// unfortunately, we have to re-hash to find which bucket we're in
	auto hash = m_HashFunc( m_memNodes[i].m_key );
	int cBucketsToModAgainst = m_vecHashBuckets.Count();
	int iBucket = basetypes::ModPowerOf2( hash, cBucketsToModAgainst );
	if ( RemoveNodeFromBucket( iBucket, i ) )
		return;

	// wasn't found; look in older buckets
	cBucketsToModAgainst >>= 1;
	while ( cBucketsToModAgainst >= m_nMinRehashedBucket )
	{
		iBucket = basetypes::ModPowerOf2( hash, cBucketsToModAgainst );

		if ( !m_bitsMigratedBuckets.GetBit( iBucket ) )
		{
			if ( RemoveNodeFromBucket( iBucket, i ) )
				return;
		}

		cBucketsToModAgainst >>= 1;
	}

	// never found, container is busted
	DbgAssert( false );
}


//-----------------------------------------------------------------------------
// Purpose: removes a node from the bucket, return true if it was found
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline bool CUtlHashMap<K,T,L,H>::RemoveNodeFromBucket( IndexType_t iBucket, int iNodeToRemove )
{
	IndexType_t iNode = m_vecHashBuckets[iBucket].m_iNode;
	while ( iNode != kInvalidIndex )
	{
		if ( iNodeToRemove == iNode )
		{
			// found it, remove
			UnlinkNodeFromBucket( iBucket, iNodeToRemove );
			Destruct( &m_memNodes[iNode].m_key );
			Destruct( &m_memNodes[iNode].m_elem );

			// link into free list
			m_memNodes[iNode].m_iNextNode = FreeNodeIndexToID( m_iNodeFreeListHead );
			m_iNodeFreeListHead = iNode;
			m_cElements--;
			if ( m_cElements == 0 )
			{
				m_nMinRehashedBucket = m_vecHashBuckets.Count();
			}
			return true;
		}

		iNode = m_memNodes[iNode].m_iNextNode;
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
			Destruct( &m_memNodes[i].m_key );
			Destruct( &m_memNodes[i].m_elem );
		}

		m_cElements = 0;
		m_nMaxElement = 0;
		m_iNodeFreeListHead = kInvalidIndex;
		m_nMinRehashedBucket = m_vecHashBuckets.Count();
		m_nMaxRehashedBucket = kInvalidIndex;
		m_bitsMigratedBuckets.Resize( 0 );
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
			Destruct( &m_memNodes[i].m_key );
			Destruct( &m_memNodes[i].m_elem );
		}
	}

	m_cElements = 0;
	m_nMaxElement = 0;
	m_iNodeFreeListHead = kInvalidIndex;
	m_nMinRehashedBucket = kInvalidIndex;
	m_nMaxRehashedBucket = kInvalidIndex;
	m_bitsMigratedBuckets.Resize( 0 );
	m_vecHashBuckets.Purge();
	m_memNodes.Purge();
}


//-----------------------------------------------------------------------------
// Purpose: rehashes buckets
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::IncrementalRehash()
{
	if ( m_nMinRehashedBucket < m_nMaxRehashedBucket )
	{
		while ( m_nMinRehashedBucket < m_nMaxRehashedBucket )
		{
			// see if the bucket needs rehashing
			if ( m_vecHashBuckets[m_nMinRehashedBucket].m_iNode != kInvalidIndex 
				&& !m_bitsMigratedBuckets.GetBit(m_nMinRehashedBucket) )
			{
				// rehash this bucket
				RehashNodesInBucket( m_nMinRehashedBucket );
				// only actively do one - don't want to do it too fast since we may be on a rapid growth path
				++m_nMinRehashedBucket;
				break;
			}

			// nothing to rehash in that bucket - increment and look again
			++m_nMinRehashedBucket;
		}

		if ( m_nMinRehashedBucket >= m_nMaxRehashedBucket )
		{
			// we're done; don't need any bits anymore
			m_nMinRehashedBucket = m_vecHashBuckets.Count();
			m_nMaxRehashedBucket = kInvalidIndex;
			m_bitsMigratedBuckets.Resize( 0 );
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: swaps with another hash map
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline void CUtlHashMap<K,T,L,H>::Swap( CUtlHashMap<K,T,L,H> &that )
{
	m_vecHashBuckets.Swap( that.m_vecHashBuckets );
	SWAP( m_bitsMigratedBuckets, that.m_bitsMigratedBuckets );
	m_memNodes.Swap( that.m_memNodes );
	SWAP( m_iNodeFreeListHead, that.m_iNodeFreeListHead );
	SWAP( m_cElements, that.m_cElements );
	SWAP( m_nMaxElement, that.m_nMaxElement );
	SWAP( m_nMinRehashedBucket, that.m_nMinRehashedBucket );
	SWAP( m_nMaxRehashedBucket, that.m_nMaxRehashedBucket );
}

#endif // UTLHASHMAP_H
