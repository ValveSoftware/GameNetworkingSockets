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
#include "tier1/utliterator.h"
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
public:
	typedef K KeyType_t;
	typedef T ElemType_t;
	typedef int IndexType_t;
	typedef L EqualityFunc_t;
	typedef H HashFunc_t;

	CUtlHashMap()
	{
		m_cElements = 0;
		m_nMaxElement = 0;
		m_nMinRehashedBucket = InvalidIndex();
		m_nMaxRehashedBucket = InvalidIndex();
		m_iNodeFreeListHead = InvalidIndex();
	}

	CUtlHashMap( int cElementsExpected )
	{
		m_cElements = 0;
		m_nMaxElement = 0;
		m_nMinRehashedBucket = InvalidIndex();
		m_nMaxRehashedBucket = InvalidIndex();
		m_iNodeFreeListHead = InvalidIndex();
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
	KeyType_t &			Key( IndexType_t i )				{ return m_memNodes.Element( i ).m_key; }
	const KeyType_t &	Key( IndexType_t i ) const			{ return m_memNodes.Element( i ).m_key; }

	// Num elements
	IndexType_t Count() const								{ return m_cElements; }

	// Max "size" of the vector
	IndexType_t  MaxElement() const							{ return m_nMaxElement; }

	// Checks if a node is valid and in the map
	bool  IsValidIndex( IndexType_t i ) const				{ return i >= 0 && i < m_nMaxElement && !IsFreeNodeID( m_memNodes[i].m_iNextNode ); }

	// Invalid index
	static IndexType_t InvalidIndex()						{ return -1; }

	// Insert method
	IndexType_t  Insert( const KeyType_t &key )								{ return InsertOrReplace( key ); }
	IndexType_t  Insert( const KeyType_t &key, const ElemType_t &insert )	{ return InsertOrReplace( key, insert ); }
	IndexType_t  InsertOrReplace( const KeyType_t &key );
	IndexType_t  InsertOrReplace( const KeyType_t &key, const ElemType_t &insert );
	IndexType_t  InsertWithDupes( const KeyType_t &key, const ElemType_t &insert );

	// Find-or-insert method, one-arg - can insert default-constructed element
	// when there is no available copy constructor or assignment operator
	IndexType_t  FindOrInsert( const KeyType_t &key );

	// Find-or-insert method, two-arg - can insert an element when there is no
	// copy constructor for the type (but does require assignment operator)
	IndexType_t  FindOrInsert( const KeyType_t &key, const ElemType_t &insert );

	// Finds an element
	IndexType_t  Find( const KeyType_t &key ) const;
	
	// Finds an exact key/value match, even with duplicate keys. Requires operator== for ElemType_t.
	IndexType_t  FindExact( const KeyType_t &key, const ElemType_t &elem ) const;

	// Find next element with same key
	IndexType_t  NextSameKey( IndexType_t i ) const;

	// has an element
	bool HasElement( const KeyType_t &key ) const			{ return Find( key ) != InvalidIndex(); }

	void EnsureCapacity( int num );
	
	const ElemType_t &FindElement( const KeyType_t &key, const ElemType_t &defaultValue ) const
	{
		IndexType_t i = Find( key );
		if ( i == InvalidIndex() )
			return defaultValue;
		return Element( i );
	}

	void RemoveAt( IndexType_t i );
	bool Remove( const KeyType_t &key )
	{
		int iMap = Find( key );
		if ( iMap != InvalidIndex() )
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
	IndexType_t InsertUnconstructed( const KeyType_t &key, IndexType_t *pExistingIndex, bool bAllowDupes );

	inline IndexType_t FreeNodeIDToIndex( IndexType_t i ) const	{ return (0-i)-3; }
	inline IndexType_t FreeNodeIndexToID( IndexType_t i ) const	{ return (-3)-i; }
	inline bool IsFreeNodeID( IndexType_t i ) const				{ return i < InvalidIndex(); }

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

	// DO NOT CHANGE Node_t WITHOUT MODIFYING IteratorNode_t INSIDE THE IteratorProxyAlias CLASS!
	struct Node_t
	{
		KeyType_t m_key;
		ElemType_t m_elem;
		int m_iNextNode;
	};
	// DO NOT CHANGE Node_t WITHOUT MODIFYING IteratorNode_t INSIDE THE IteratorProxyAlias CLASS!

	CUtlMemory<Node_t> m_memNodes;
	IndexType_t m_iNodeFreeListHead;

public:
	// STL / C++11-style iterators (unspecified / in-memory order!)
	struct IterateKeyElemProxyAlias
	{
		// Define a compatible type that uses the same key,elem names as CUtlMap.
		// This will be pointer-aliased to the Node_t elements of m_memNodes!
		struct IteratorNode_t
		{
			K key;
			T elem;
		};
		typedef IteratorNode_t ElemType_t;
		typedef typename CUtlHashMap::IndexType_t IndexType_t;

		typedef CUtlForwardIteratorImplT< IterateKeyElemProxyAlias, false > iterator;
		typedef CUtlForwardIteratorImplT< IterateKeyElemProxyAlias, true > const_iterator;

		ElemType_t &		Element( IndexType_t i )		{ return *reinterpret_cast<IteratorNode_t*>( &reinterpret_cast<CUtlHashMap*>(this)->m_memNodes.Element( i ) ); }
		const ElemType_t &	Element( IndexType_t i ) const	{ return *reinterpret_cast<const IteratorNode_t*>( &reinterpret_cast<const CUtlHashMap*>(this)->m_memNodes.Element( i ) ); }
		IndexType_t IteratorNext( IndexType_t i ) const		{ auto pSelf = reinterpret_cast<const CUtlHashMap*>(this); while ( ++i < pSelf->MaxElement() ) { if ( pSelf->IsValidIndex( i ) ) return i; } return -1; }

		iterator begin()				{ return iterator( this, IteratorNext( -1 ) ); }
		iterator end()					{ return iterator( this, -1 ); }
		const_iterator begin() const	{ return const_iterator( this, IteratorNext( -1 ) ); }
		const_iterator end() const		{ return const_iterator( this, -1 ); }
	};

	friend struct IterateKeyElemProxyAlias;

	typedef CUtlForwardIteratorImplT< IterateKeyElemProxyAlias, false > iterator;
	typedef CUtlForwardIteratorImplT< IterateKeyElemProxyAlias, true > const_iterator;
	iterator begin()				{ return reinterpret_cast<IterateKeyElemProxyAlias*>(this)->begin(); }
	iterator end()					{ return reinterpret_cast<IterateKeyElemProxyAlias*>(this)->end(); }
	const_iterator begin() const	{ return reinterpret_cast<const IterateKeyElemProxyAlias*>(this)->begin(); }
	const_iterator end() const		{ return reinterpret_cast<const IterateKeyElemProxyAlias*>(this)->end(); }

protected:
	IndexType_t m_cElements;
	IndexType_t m_nMaxElement;
	IndexType_t m_nMinRehashedBucket, m_nMaxRehashedBucket;
	EqualityFunc_t m_EqualityFunc;
	HashFunc_t m_HashFunc;
};


//-----------------------------------------------------------------------------
// Purpose: inserts a key into the map with an unconstructed element member
// (to be copy constructed or default-constructed by a wrapper function)
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline int CUtlHashMap<K,T,L,H>::InsertUnconstructed( const KeyType_t &key, int *piNodeExistingIfDupe, bool bAllowDupes )
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
		if ( iNode != InvalidIndex() )
		{
			return InvalidIndex();
		}
	}

	// make an item
	int iNewNode = AllocNode();
	m_memNodes[iNewNode].m_iNextNode = InvalidIndex();
	CopyConstruct( &m_memNodes[iNewNode].m_key, key );
	// Note: m_elem remains intentionally unconstructed here

	iBucket = basetypes::ModPowerOf2( hash, m_vecHashBuckets.Count() );

	// link ourselves in
	//	::OutputDebugStr( CFmtStr( "insert %d into bucket %d\n", key, iBucket ).Access() );
	LinkNodeIntoBucket( iBucket, iNewNode );

    // Initialized to placate the compiler's uninitialized value checking.
    if ( piNodeExistingIfDupe )
    {
        *piNodeExistingIfDupe = InvalidIndex();
    }
    
	// return the new node
	return iNewNode;
}


//-----------------------------------------------------------------------------
// Purpose: inserts a default item into the map, no change if key already exists
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline int CUtlHashMap<K,T,L,H>::FindOrInsert( const KeyType_t &key )
{
	int iNodeExisting;
	int iNodeInserted = InsertUnconstructed( key, &iNodeExisting, false /*no duplicates allowed*/ );
	if ( iNodeInserted != InvalidIndex() )
	{
		Construct( &m_memNodes[ iNodeInserted ].m_elem );
		return iNodeInserted;
	}
	return iNodeExisting;
}


//-----------------------------------------------------------------------------
// Purpose: inserts an item into the map, no change if key already exists
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline int CUtlHashMap<K,T,L,H>::FindOrInsert( const KeyType_t &key, const ElemType_t &insert )
{
	int iNodeExisting;
	int iNodeInserted = InsertUnconstructed( key, &iNodeExisting, false /*no duplicates allowed*/ );
	if ( iNodeInserted != InvalidIndex() )
	{
		CopyConstruct( &m_memNodes[ iNodeInserted ].m_elem, insert );
		return iNodeInserted;
	}
	return iNodeExisting;
}


//-----------------------------------------------------------------------------
// Purpose: inserts an item into the map, replaces existing item with same key
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline int CUtlHashMap<K,T,L,H>::InsertOrReplace( const KeyType_t &key )
{
	int iNodeExisting;
	int iNodeInserted = InsertUnconstructed( key, &iNodeExisting, false /*no duplicates allowed*/ );
	if ( iNodeInserted != InvalidIndex() )
	{
		Construct( &m_memNodes[ iNodeInserted ].m_elem );
		return iNodeInserted;
	}
	return iNodeExisting;
}


//-----------------------------------------------------------------------------
// Purpose: inserts an item into the map, replaces existing item with same key
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline int CUtlHashMap<K,T,L,H>::InsertOrReplace( const KeyType_t &key, const ElemType_t &insert )
{
	int iNodeExisting;
	int iNodeInserted = InsertUnconstructed( key, &iNodeExisting, false /*no duplicates allowed*/ );
	if ( iNodeInserted != InvalidIndex() )
	{
		CopyConstruct( &m_memNodes[ iNodeInserted ].m_elem, insert );
		return iNodeInserted;
	}
	m_memNodes[ iNodeExisting ].m_elem = insert;
	return iNodeExisting;
}

//-----------------------------------------------------------------------------
// Purpose: inserts element no matter what, even if key already exists
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline int CUtlHashMap<K,T,L,H>::InsertWithDupes( const KeyType_t &key, const ElemType_t &insert )
{
	int iNodeInserted = InsertUnconstructed( key, NULL, true /*duplicates allowed!*/ );
	if ( iNodeInserted != InvalidIndex() )
	{
		CopyConstruct( &m_memNodes[ iNodeInserted ].m_elem, insert );
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
	DbgAssert( m_iNodeFreeListHead != InvalidIndex() );
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
	while ( iNode != InvalidIndex() )
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
		return InvalidIndex();

	// hash the item
	auto hash = m_HashFunc( key );

	// find the bucket
	int cBucketsToModAgainst = m_vecHashBuckets.Count();
	int iBucket = basetypes::ModPowerOf2( hash, cBucketsToModAgainst );

	// look in the bucket for the item
	int iNode = FindInBucket( iBucket, key );
	if ( iNode != InvalidIndex() )
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
			if ( iNode2 != InvalidIndex() )
				return iNode2;
		}

		cBucketsToModAgainst >>= 1;
	}

	return InvalidIndex();	
}


//-----------------------------------------------------------------------------
// Purpose: searches for an item by key and element equality, returning the index handle
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H>
inline int CUtlHashMap<K, T, L, H>::FindExact( const KeyType_t &key, const ElemType_t &elem ) const
{
	int iNode = Find( key );
	while ( iNode != InvalidIndex() )
	{
		if ( elem == m_memNodes[iNode].m_elem )
			return iNode;
		iNode = NextSameKey( iNode );
	}
	return InvalidIndex();
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
		while ( iNode != InvalidIndex() )
		{
			// equality check
			if ( m_EqualityFunc( key, m_memNodes[iNode].m_key ) )
				return iNode;

			iNode = m_memNodes[iNode].m_iNextNode;
		}
	}
	return InvalidIndex();
}


//-----------------------------------------------------------------------------
// Purpose: searches for an item by key, returning the index handle
//-----------------------------------------------------------------------------
template <typename K, typename T, typename L, typename H> 
inline int CUtlHashMap<K,T,L,H>::FindInBucket( int iBucket, const KeyType_t &key ) const
{
	if ( m_vecHashBuckets[iBucket].m_iNode != InvalidIndex() )
	{
		IndexType_t iNode = m_vecHashBuckets[iBucket].m_iNode;
		DbgAssert( iNode < m_nMaxElement );
		while ( iNode != InvalidIndex() )
		{
			// equality check
			if ( m_EqualityFunc( key, m_memNodes[iNode].m_key ) )
				return iNode;

			iNode = m_memNodes[iNode].m_iNextNode;
		}
	}

	return InvalidIndex();
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
	while ( iNode != InvalidIndex() )
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
	while ( iNode != InvalidIndex() )
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
		m_iNodeFreeListHead = InvalidIndex();
		m_nMinRehashedBucket = m_vecHashBuckets.Count();
		m_nMaxRehashedBucket = InvalidIndex();
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
	m_iNodeFreeListHead = InvalidIndex();
	m_nMinRehashedBucket = InvalidIndex();
	m_nMaxRehashedBucket = InvalidIndex();
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
			if ( m_vecHashBuckets[m_nMinRehashedBucket].m_iNode != InvalidIndex() 
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
			m_nMaxRehashedBucket = InvalidIndex();
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
