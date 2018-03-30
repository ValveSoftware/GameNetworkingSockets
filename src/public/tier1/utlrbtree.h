//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $Header: $
// $NoKeywords: $
//=============================================================================//

#ifndef UTLRBTREE_H
#define UTLRBTREE_H

#include "utlmemory.h"
#include "utliterator.h"
#include "vstdlib/strtools.h"


//-----------------------------------------------------------------------------
// Tool to generate a default compare function for any type that implements
// operator<, including all simple types
//-----------------------------------------------------------------------------

template <typename T >
class CDefOps
{
public:
	static bool LessFunc( const T &lhs, const T &rhs )	{ return ( lhs < rhs );	}
	static bool LessFuncCtx( const T &lhs, const T &rhs, void *pCtx )	{ return LessFunc( lhs, rhs );	}
};

#define DefLessFunc( type ) CDefOps<type>::LessFunc
#define DefLessFuncCtx( type ) CDefOps<type>::LessFuncCtx

template <typename T>
class CDefLess
{
public:
	CDefLess() {}
	CDefLess( int i ) {}
	inline bool operator()( const T &lhs, const T &rhs ) const { return ( lhs < rhs );	}
	inline bool operator!() const { return false; }
};

template <typename T>
class CDefLessReverse
{
public:
	CDefLessReverse() {}
	CDefLessReverse( int i ) {}
	inline bool operator()( const T &lhs, const T &rhs ) const { return (lhs > rhs); }
	inline bool operator!() const { return false; }
};

template <typename T>
class CDefLessPtr
{
public:
	typedef T* PointerType_t;
	CDefLessPtr() {}
	CDefLessPtr( int i ) {}
	inline bool operator()( const PointerType_t &lhs, const PointerType_t &rhs ) const { return ( *lhs < *rhs ); }
	inline bool operator!() const { return false; }
};

typedef const char * PConstChar_t;

//SDR_PUBLIC class CDefCaselessStringLess
//SDR_PUBLIC {
//SDR_PUBLIC public:
//SDR_PUBLIC 	CDefCaselessStringLess() {}
//SDR_PUBLIC 	CDefCaselessStringLess( int i ) {}
//SDR_PUBLIC 	inline bool operator()( const PConstChar_t &lhs, const PConstChar_t &rhs ) const { return ( V_stricmp(lhs, rhs) < 0 ); }
//SDR_PUBLIC 	inline bool operator!() const { return false; }
//SDR_PUBLIC };
//SDR_PUBLIC 
//SDR_PUBLIC class CDefStringLess
//SDR_PUBLIC {
//SDR_PUBLIC public:
//SDR_PUBLIC 	CDefStringLess() {}
//SDR_PUBLIC 	CDefStringLess( int i ) {}
//SDR_PUBLIC 	inline bool operator()( const PConstChar_t &lhs, const PConstChar_t &rhs ) const { return ( V_strcmp(lhs, rhs) < 0 ); }
//SDR_PUBLIC 	inline bool operator!() const { return false; }
//SDR_PUBLIC };

#define INVALID_RBTREE_IDX ((I)~0)

//-------------------------------------

//SDR_PUBLIC inline bool WideStringLessThan( const wchar_t * const &lhs, const wchar_t * const &rhs)	{ return ( V_wcscmp(lhs, rhs) < 0 );  }

inline bool StringLessThan( const char * const &lhs, const char * const &rhs)			{ return ( V_strcmp(lhs, rhs) < 0 );  }
//SDR_PUBLIC inline bool CaselessStringLessThan( const char * const &lhs, const char * const &rhs )	{ return ( V_stricmp(lhs, rhs) < 0 ); }

// Same as CaselessStringLessThan, but it ignores differences in / and \.
inline bool CaselessStringLessThanIgnoreSlashes( const char * const &lhs, const char * const &rhs )	
{ 
	const char *pa = lhs;
	const char *pb = rhs;
	while ( *pa && *pb )
	{
		char a = *pa;
		char b = *pb;
		
		// Check for dir slashes.
		if ( a == '/' || a == '\\' )
		{
			if ( b != '/' && b != '\\' )
				return ('/' < b);
		}
		else
		{
			if ( a >= 'a' && a <= 'z' )
				a = 'A' + (a - 'a');
			
			if ( b >= 'a' && b <= 'z' )
				b = 'A' + (b - 'a');
				
			if ( a > b )
				return false;
			else if ( a < b )
				return true;
		}
		++pa;
		++pb;
	}
	
	// Filenames also must be the same length.
	if ( *pa != *pb )
	{
		// If pa shorter than pb then it's "less"
		return ( !*pa );
	}

	return false;
}

//-------------------------------------
// inline these two templates to stop multiple definitions of the same code
template <> inline bool CDefOps<const char *>::LessFunc( const char * const &lhs, const char * const &rhs )	{ return StringLessThan( lhs, rhs ); }
template <> inline bool CDefOps<char *>::LessFunc( char * const &lhs, char * const &rhs )						{ return StringLessThan( lhs, rhs ); }

//-------------------------------------

#if defined( _MSC_VER ) && _MSC_VER < 1600
#define MAP_INDEX_TYPE( mapName ) int
#else
#define MAP_INDEX_TYPE( mapName ) \
	decltype( (mapName).MaxElement() )
#endif


#define FOR_EACH_RBTREE( treeName, iteratorName ) \
	for ( MAP_INDEX_TYPE(treeName) iteratorName = (treeName).FirstInorder(); iteratorName != (treeName).InvalidIndex(); iteratorName = (treeName).NextInorder( iteratorName ) )

// faster iteration, but in an unspecified order
#define FOR_EACH_RBTREE_FAST( treeName, iteratorName ) \
	for ( MAP_INDEX_TYPE(treeName) iteratorName = 0; iteratorName < (treeName).MaxElement(); ++iteratorName ) if ( !(treeName).IsValidIndex( iteratorName ) ) continue; else


template <typename RBTREE_T>
void SetDefLessFunc( RBTREE_T &RBTree )
{
	RBTree.SetLessFunc( DefLessFunc( typename RBTREE_T::KeyType_t ) );
}

// inline implementation of a function usable by CDefLess to compare two parameters in-order
#define DECLARE_INLINE_OPERATOR_LESS_2( T, p1, p2 ) bool operator<( const T &rhs ) const { if ( p1 != rhs.p1 ) return p1 < rhs.p1; return p2 < rhs.p2; }

// For use with FindClosest
// Move these to a common area if anyone else ever uses them
enum CompareOperands_t
{
	k_EEqual = 0x1,
	k_EGreaterThan = 0x2,
	k_ELessThan = 0x4,
	k_EGreaterThanOrEqualTo = k_EGreaterThan | k_EEqual,
	k_ELessThanOrEqualTo = k_ELessThan | k_EEqual,
};

template <class I>
class CDefRBTreeBalanceListener
{
public:
	void OnRotateLeft( I node, I rightNode ) {}
	void OnRotateRight( I node, I leftNode ) {}
	void OnLinkToParent( I node ) {}
	void OnPreUnlink( I node ) {}
	void OnRelinkSuccessor( I node ) {}
};

template <class T, class I>
class CRBTreeBalanceListener
{
public:
	CRBTreeBalanceListener() : m_pTarget( NULL ) { }
	CRBTreeBalanceListener( T *pTarget ) : m_pTarget( pTarget ) { }
	void OnRotateLeft( I node, I rightNode ) { if ( !m_pTarget ) return; m_pTarget->OnRotateLeft( node, rightNode); }
	void OnRotateRight( I node, I leftNode ) { if ( !m_pTarget ) return; m_pTarget->OnRotateRight( node, leftNode); }
	void OnLinkToParent( I node ) { if ( !m_pTarget ) return; m_pTarget->OnLinkToParent( node ); }
	void OnPreUnlink( I node ) { if ( !m_pTarget ) return; m_pTarget->OnPreUnlink( node ); }
	void OnRelinkSuccessor( I node ) { if ( !m_pTarget ) return; m_pTarget->OnRelinkSuccessor( node ); }

private:
	T *m_pTarget;
};

namespace CUtlRBTreeInternal
{
	template< class I >
	struct Links_t
	{
		I  m_Left;
		I  m_Right;
		I  m_Parent;
		I  m_Tag;
	};

	enum NodeColor_t
	{
		RED = 0,
		BLACK
	};
}

template < class I, class E >
class CUtlRBTreeBase
{
public:

	CUtlRBTreeBase();

	// Num elements
	unsigned int Count() const;
	
	// Max "size" of the vector
	I  MaxElement() const;

	// Gets the root
	I  Root() const;

	// Tests if root or leaf
	bool  IsRoot( I i ) const;

	// Invalid index
	static I InvalidIndex() { return INVALID_RBTREE_IDX; }

	// First pre-order
	I  FirstPreorder() const;
	I  PrevPreorder( I i ) const;

protected:

	// used in Links as the return value for InvalidIndex()
	CUtlRBTreeInternal::Links_t<I> m_Sentinel;

	// Checks if a node is valid and in the tree
	bool  _IsValidIndex( I i, size_t unNodeSize, void *pMemBase ) const;

	// Gets at the links
	CUtlRBTreeInternal::Links_t<I> const	&_Links( I i, size_t unNodeSize, void *pMemBase ) const;
	CUtlRBTreeInternal::Links_t<I>			&_Links( I i, size_t unNodeSize, void *pMemBase );      

	// Gets the children                               
	I  _Parent( I i, size_t unNodeSize, void *pMemBase ) const;
	I  _LeftChild( I i, size_t unNodeSize, void *pMemBase ) const;
	I  _RightChild( I i, size_t unNodeSize, void *pMemBase ) const;

	// Sets the children
	void  _SetParent( I i, I parent, size_t unNodeSize, void *pMemBase );
	void  _SetLeftChild( I i, I child, size_t unNodeSize, void *pMemBase );
	void  _SetRightChild( I i, I child, size_t unNodeSize, void *pMemBase );

	bool _IsRed( I i, size_t unNodeSize, void *pMemBase ) const; 
	bool _IsBlack( I i, size_t unNodeSize, void *pMemBase ) const;

	// Tests if a node is a left or right child
	bool  _IsLeftChild( I i, size_t unNodeSize, void *pMemBase ) const;
	bool  _IsRightChild( I i, size_t unNodeSize, void *pMemBase ) const;

	// Sets/gets node color
	CUtlRBTreeInternal::NodeColor_t _Color( I i, size_t unNodeSize, void *pMemBase ) const;
	void        _SetColor( I i, CUtlRBTreeInternal::NodeColor_t c, size_t unNodeSize, void *pMemBase );

	void _RotateLeft(I i, size_t unNodeSize, void *pMemBase);
	void _RotateRight(I i, size_t unNodeSize, void *pMemBase);
	void _InsertRebalance(I i, size_t unNodeSize, void *pMemBase);
	void _RemoveRebalance(I i, size_t unNodeSize, void *pMemBase);

	void _Unlink( I elem, size_t unNodeSize, void *pMemBase );

	// Sets the children
	void  _LinkToParent( I i, I parent, bool isLeft, size_t unNodeSize, void *pMemBase );

	// Checks if the tree as a whole is valid
	bool  _IsValid( size_t unNodeSize, void *pMemBase ) const;

	I  _FirstInorder(size_t unNodeSize, void *pMemBase) const;
	I  _NextInorder( I i, size_t unNodeSize, void *pMemBase ) const;
	I  _PrevInorder( I i, size_t unNodeSize, void *pMemBase ) const;
	I  _LastInorder(size_t unNodeSize, void *pMemBase) const;

	I  _NextPreorder( I i, size_t unNodeSize, void *pMemBase ) const;
	I  _LastPreorder(size_t unNodeSize, void *pMemBase) const;

	I  _FirstPostorder(size_t unNodeSize, void *pMemBase) const;
	I  _NextPostorder( I i, size_t unNodeSize, void *pMemBase ) const;

	int _Depth( I node, size_t unNodeSize, void *pMemBase ) const;

	struct EmptyBaseOpt_t : E
	{
		EmptyBaseOpt_t() {}
		EmptyBaseOpt_t( E init ) : E( init ) {}

		void*   m_pElements;
	};
	EmptyBaseOpt_t m_data;

	inline void ResetDbgInfo( void *pMemBase )
	{
		m_data.m_pElements = pMemBase;
	}	

	I m_Root;
	I m_NumElements;
	I m_FirstFree;
	I m_TotalElements;
};

//-----------------------------------------------------------------------------
// A red-black binary search tree
//-----------------------------------------------------------------------------

template <class T, class I = int, typename L = bool (*)( const T &, const T & ), class E = CDefRBTreeBalanceListener< I > > 
class CUtlRBTree : public CUtlRBTreeBase< I, E >
{
public:
	typedef T KeyType_t;
	typedef T ElemType_t;
	typedef I IndexType_t;

	// Less func typedef
	// Returns true if the first parameter is "less" than the second
	typedef L LessFunc_t;

	typedef E BalanceListener_t;

	// constructor, destructor
	// Left at growSize = 0, the memory will first allocate 1 element and double in size
	// at each increment.
	// LessFunc_t is required, but may be set after the constructor using SetLessFunc() below
	CUtlRBTree( int growSize = 0, int initSize = 0, const LessFunc_t &lessfunc = 0 );
	CUtlRBTree( const LessFunc_t &lessfunc );
	CUtlRBTree( const BalanceListener_t &eventListener);
	~CUtlRBTree();
	
	unsigned int NumAllocated() const;

	// gets particular elements
	T&			Element( I i );
	T const		&Element( I i ) const;
	T&			operator[]( I i );
	T const		&operator[]( I i ) const;
	T&			ElementByLinearIndex( IndexType_t i );
	const T&	ElementByLinearIndex( IndexType_t i ) const;

	CUtlRBTree<T, I, L, E>& operator=( const CUtlRBTree<T, I, L, E> &other );
	
	// Gets the children                               
	I  Parent( I i ) const;
	I  LeftChild( I i ) const;
	I  RightChild( I i ) const;
	
	// Tests if a node is a left or right child
	bool  IsLeftChild( I i ) const;
	bool  IsRightChild( I i ) const;
	
	// Tests if root or leaf
	bool  IsLeaf( I i ) const;
	
	// Checks if a node is valid and in the tree
	bool  IsValidIndex( I i ) const;

	// Checks if a index is of range 0 to Count()-1
	bool  IsValidLinearIndex( I i ) const;

	// Checks if the tree as a whole is valid
	bool  IsValid() const;
	
	// returns the tree depth (not a very fast operation)
	int   Depth( I node ) const;
	int   Depth() const;
	
	// Sets the less func
	void SetLessFunc( const LessFunc_t &func );
	
	// Allocation method
	I  NewNode( bool bConstructElement );

	// Insert method (inserts in order)
	I  Insert( T const &insert, bool bInsertDuplicates = true );
	void Insert( const T *pArray, int nItems, bool bInsertDuplicates = true );
	
	// Insert with not found interface to match source engine branches
	I InsertIfNotFound(T const &insert);
	
	// FindOrInsert method (returns existing index, or inserts if not found)
	I  FindOrInsert( T const &insert );

	// Find method
	I  Find( T const &search ) const;

	// FindFirst method ( finds first inorder if there are duplicates )
	I  FindFirst( T const &search ) const;

	// First element >= key
	I  FindClosest( T const &search, CompareOperands_t eFindCriteria ) const;
	
	// Remove methods
	void     RemoveAt( I i );
	bool     Remove( T const &remove );
	void     RemoveAll();
	void	 Purge();

	bool HasElement( T const &search ) const;
			
	// Allocation, deletion
	void  FreeNode( I i );
	
	// Iteration
	I  FirstInorder() const;
	I  NextInorder( I i ) const;
	I  PrevInorder( I i ) const;
	I  LastInorder() const;
	
	I  NextPreorder( I i ) const;
	I  LastPreorder() const;
	
	I  FirstPostorder() const;
	I  NextPostorder( I i ) const;

	// If you change the search key, this can be used to reinsert the 
	// element into the tree.
	void	Reinsert( I elem );

	// swap in place
	void Swap( CUtlRBTree< T, I, L, E > &that );

	// If you build a container on top of RBTree you need this - otherwise you shouldnt use
	// Insertion, removal
	I  InsertAt( I parent, bool leftchild, bool bConstructElement );
	
	// If you build a container on top of RBTree you need this - otherwise you shouldnt use
	// Inserts a node into the tree, doesn't copy the data in.
	void FindInsertionPosition( T const &insert, bool bCheckForDuplicates, I &parent, bool &leftchild, bool &isDuplicate );

	// Makes sure we have enough memory allocated to store a requested # of elements
	void EnsureCapacity( int num );

	int CubAllocated() { return m_Elements.CubAllocated(); }

	// STL / C++11-style iterators (in-order traversal)
	typedef CUtlBidirectionalIteratorImplT< CUtlRBTree< T, I, L, E >, false > iterator;
	typedef CUtlBidirectionalIteratorImplT< CUtlRBTree< T, I, L, E >, true > const_iterator;
	iterator begin() { return iterator( this, FirstInorder() ); }
	iterator end() { return iterator( this, INVALID_RBTREE_IDX ); }
	const_iterator begin() const { return const_iterator( this, FirstInorder() ); }
	const_iterator end() const { return const_iterator( this, INVALID_RBTREE_IDX ); }
	I IteratorNext( I i ) const { return NextInorder( i ); }
	I IteratorPrev( I i ) const { return i == INVALID_RBTREE_IDX ? LastInorder() : PrevInorder( i ); }

	struct ProxyTypeIterateUnordered // "this" pointer is reinterpret_cast from CUtlRBTree!
	{
		typedef T ElemType_t;
		typedef I IndexType_t;
		T &Element( I i ) { return reinterpret_cast<CUtlRBTree*>(this)->Element( i ); }
		const T &Element( I i ) const { return reinterpret_cast<const CUtlRBTree*>(this)->Element( i ); }
		I IteratorNext( I i ) const { auto pTree = reinterpret_cast<const CUtlRBTree*>(this); while ( ++i < pTree->MaxElement() ) { if ( pTree->IsValidIndex( i ) ) return i; } return INVALID_RBTREE_IDX; }

		typedef CUtlForwardIteratorImplT< ProxyTypeIterateUnordered, false > iterator;
		typedef CUtlForwardIteratorImplT< ProxyTypeIterateUnordered, true > const_iterator;
		iterator begin() { return iterator( this, IteratorNext( (I)0 - 1 ) ); }
		iterator end() { return iterator( this, INVALID_RBTREE_IDX ); }
		const_iterator begin() const { return const_iterator( this, IteratorNext( (I)0 - 1 ) ); }
		const_iterator end() const { return const_iterator( this, INVALID_RBTREE_IDX ); }
	};
	ProxyTypeIterateUnordered &IterateUnordered() { return *reinterpret_cast<ProxyTypeIterateUnordered*>(this); }
	const ProxyTypeIterateUnordered &IterateUnordered() const { return *reinterpret_cast<const ProxyTypeIterateUnordered*>(this); }

	static bool BDiffRBTrees( const CUtlRBTree<T, I, L, E> &rbTreeBase, const CUtlRBTree<T, I, L, E> &rbTreeCompare, CUtlRBTree<T, I, L, E> *prbTreeAdditions = NULL, CUtlRBTree<T, I, L, E> *prbTreeDeletions = NULL );

#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName );
#endif // DBGFLAG_VALIDATE

protected:
	
  	struct Node_t : public CUtlRBTreeInternal::Links_t<I>
	{
		T  m_Data;
	};

	//
	// Inline functions that just pass straight into base, but with extra params.  
	//
	// This is done so the bulk of the code can be in the base class, but interfaces to 
	// these within child class code can be sane.
	//

	inline CUtlRBTreeInternal::Links_t<I> const	&Links( I i ) const { return _Links( i, sizeof(Node_t), (void*)m_Elements.Base() ); }
	inline CUtlRBTreeInternal::Links_t<I>		&Links( I i ){ return this->_Links( i, sizeof(Node_t), (void*)m_Elements.Base() ); }   

	inline void  SetParent( I i, I parent ) { return _SetParent( i, parent, sizeof(Node_t), (void*)m_Elements.Base() ); } 
	inline void  SetLeftChild( I i, I child  ) { return this->_SetLeftChild( i, child, sizeof(Node_t), (void*)m_Elements.Base() ); } 
	inline void  SetRightChild( I i, I child  ) { return this->_SetRightChild( i, child, sizeof(Node_t), (void*)m_Elements.Base() ); } 
	
	// Checks if a link is red or black
	inline bool IsRed( I i ) const { return _IsRed( i, sizeof(Node_t), (void*)m_Elements.Base() ); } 
	inline bool IsBlack( I i ) const { return _IsBlack( i, sizeof(Node_t), (void*)m_Elements.Base() ); } 

	// Sets/gets node color
	inline CUtlRBTreeInternal::NodeColor_t Color( I i ) const { return _Color( i, sizeof(Node_t), (void*)m_Elements.Base() ); }
	inline void        SetColor( I i, CUtlRBTreeInternal::NodeColor_t c ) { return _SetColor( i, c, sizeof(Node_t), (void*)m_Elements.Base() ); }

	// operations required to preserve tree balance
	inline void RotateLeft(I i) { _RotateLeft( i, sizeof(Node_t), (void*)m_Elements.Base() ); }
	inline void RotateRight(I i) { _RotateRight( i, sizeof(Node_t), (void*)m_Elements.Base() ); }
	inline void InsertRebalance(I i) { _InsertRebalance( i, sizeof(Node_t), (void*)m_Elements.Base() ); }
	inline void RemoveRebalance(I i) { _RemoveRebalance( i, sizeof(Node_t), (void*)m_Elements.Base() ); }

	inline void	Unlink( I elem ) { this->_Unlink( elem, sizeof(Node_t), (void*)m_Elements.Base() ); }

	// Sets the children
	inline void  LinkToParent( I i, I parent, bool isLeft ) { this->_LinkToParent( i, parent, isLeft, sizeof(Node_t), (void*)m_Elements.Base() ); }


	// copy constructors not allowed
	CUtlRBTree( CUtlRBTree<T, I, L, E> const &tree );
	
	// Remove and add back an element in the tree.
	void	Link( I elem );

	// Used for sorting.
	LessFunc_t m_LessFunc;
	
	CUtlMemory<Node_t> m_Elements;		    
};


//-----------------------------------------------------------------------------
// constructor, destructor
//-----------------------------------------------------------------------------

template< class I, class E >
CUtlRBTreeBase< I, E >::CUtlRBTreeBase()
{
	m_Root = INVALID_RBTREE_IDX;
	m_NumElements = 0;
	m_TotalElements = 0;
	m_FirstFree = INVALID_RBTREE_IDX;

	m_Sentinel.m_Left = m_Sentinel.m_Right = m_Sentinel.m_Parent = INVALID_RBTREE_IDX;
	m_Sentinel.m_Tag = CUtlRBTreeInternal::BLACK;
}

template <class T, class I, typename L, class E>
inline CUtlRBTree<T, I, L, E>::CUtlRBTree( int growSize, int initSize, const LessFunc_t &lessfunc ) : 
	CUtlRBTreeBase< I, E >(),
	m_LessFunc( lessfunc ),
	m_Elements( growSize, initSize )
{
	this->ResetDbgInfo( m_Elements.Base() );
}

template <class T, class I, typename L, class E>
inline CUtlRBTree<T, I, L, E>::CUtlRBTree( const LessFunc_t &lessfunc ) : 
	CUtlRBTreeBase< I, E >(),
	m_Elements( 0, 0 ), 
	m_LessFunc( lessfunc )
{
	this->ResetDbgInfo( m_Elements.Base() );
}

template <class T, class I, typename L, class E>
inline CUtlRBTree<T, I, L, E>::CUtlRBTree( const BalanceListener_t &eventListener) :
	CUtlRBTreeBase< I, E >(),
	m_Elements( 0, 0 ), 
	m_LessFunc( NULL )
{
	CUtlRBTreeBase< I, E>::m_data = eventListener;
	this->ResetDbgInfo( m_Elements.Base() );
}

template <class T, class I, typename L, class E> 
inline CUtlRBTree<T, I, L, E>::~CUtlRBTree()
{
	RemoveAll();
}

//-----------------------------------------------------------------------------
// gets particular elements
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
inline T &CUtlRBTree<T, I, L, E>::Element( I i )        
{ 
	return m_Elements[i].m_Data; 
}

template <class T, class I, typename L, class E>
inline T const &CUtlRBTree<T, I, L, E>::Element( I i ) const  
{ 
	return m_Elements[i].m_Data; 
}

template <class T, class I, typename L, class E>
inline T &CUtlRBTree<T, I, L, E>::operator[]( I i )        
{ 
	return Element(i); 
}

template <class T, class I, typename L, class E>
inline T const &CUtlRBTree<T, I, L, E>::operator[]( I i ) const  
{ 
	return Element(i); 
}

template <class T, class I, typename L, class E>
inline T &CUtlRBTree<T, I, L, E>::ElementByLinearIndex( IndexType_t i )
{
	IndexType_t cElementsSeen = 0;
	IndexType_t iterator = 0;
	for ( ; iterator < CUtlRBTree<T,I,L,E>::MaxElement(); ++iterator )
	{
		if ( !IsValidIndex( iterator ) ) continue; else
		{
			if ( cElementsSeen++ == i )
				break;
		}
	}
	return m_Elements[iterator].m_Data;
}

template <class T, class I, typename L, class E>
inline const T &CUtlRBTree<T, I, L, E>::ElementByLinearIndex( IndexType_t i ) const
{
	IndexType_t cElementsSeen = 0;
	IndexType_t iterator = 0;
	for ( ; iterator < CUtlRBTree<T,I,L,E>::MaxElement(); ++iterator )
	{
		if ( !IsValidIndex( iterator ) ) continue; else
		{
			if ( cElementsSeen == i )
				break;
			cElementsSeen++;
		}
	}
	return m_Elements[iterator].m_Data;
}


template <class T, class I, typename L, class E>
inline bool CUtlRBTree<T, I, L, E>::IsValidLinearIndex( IndexType_t i ) const
{
	return i >= 0 && i < (IndexType_t)CUtlRBTree<T,I,L,E>::Count();
}


template <class T, class I, typename L, class E>
inline CUtlRBTree<T, I, L, E>& CUtlRBTree<T, I, L, E>::operator=( const CUtlRBTree<T, I, L, E> &other )
{
	RemoveAll();
	EnsureCapacity( other.Count() );
	m_LessFunc = other.m_LessFunc;
	
	FOR_EACH_RBTREE_FAST( other, i )
	{
		Insert( other[i] );
	}
	
	return *this;
}

//-----------------------------------------------------------------------------
//
// various accessors
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Gets the root
//-----------------------------------------------------------------------------

template < class I, class E >
inline	I  CUtlRBTreeBase< I, E >::Root() const             
{ 
	return m_Root; 
}
	
//-----------------------------------------------------------------------------
// Num elements
//-----------------------------------------------------------------------------

template < class I, class E >
inline	unsigned int CUtlRBTreeBase< I, E >::Count() const          
{ 
	return (unsigned int)m_NumElements; 
}

//-----------------------------------------------------------------------------
// Num elements allocated
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
inline	unsigned int CUtlRBTree<T, I, L, E>::NumAllocated() const          
{ 
	return (unsigned int)m_Elements.NumAllocated();
}

//-----------------------------------------------------------------------------
// Max "size" of the vector
//-----------------------------------------------------------------------------

template < class I, class E >
inline	I  CUtlRBTreeBase< I, E >::MaxElement() const       
{ 
	return (I)m_TotalElements; 
}
	

//-----------------------------------------------------------------------------
// Gets the children                               
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
inline	I CUtlRBTree<T, I, L, E>::Parent( I i ) const      
{ 
	return CUtlRBTreeBase< I, E >::_Parent( i, sizeof( Node_t ), (void *)m_Elements.Base() );
}

template < class I, class E >
inline	I CUtlRBTreeBase< I, E >::_Parent( I i, size_t unNodeSize, void *pMemBase ) const      
{ 
	return _Links(i, unNodeSize, pMemBase ).m_Parent; 
}


template <class T, class I, typename L, class E>
inline	I CUtlRBTree<T, I, L, E>::LeftChild( I i ) const   
{ 
	return CUtlRBTreeBase< I, E >::_LeftChild( i, sizeof( Node_t ), (void *)m_Elements.Base() );
}

template < class I, class E >
inline	I CUtlRBTreeBase< I, E >::_LeftChild( I i, size_t unNodeSize, void *pMemBase ) const      
{ 
	return _Links(i, unNodeSize, pMemBase ).m_Left; 
}


template <class T, class I, typename L, class E>
inline	I CUtlRBTree<T, I, L, E>::RightChild( I i ) const  
{ 
	return CUtlRBTreeBase< I, E >::_RightChild( i, sizeof( Node_t ), (void *)m_Elements.Base() );
}

template < class I, class E >
inline	I CUtlRBTreeBase< I, E >::_RightChild( I i, size_t unNodeSize, void *pMemBase ) const      
{ 
	return _Links(i, unNodeSize, pMemBase ).m_Right; 
}
	
//-----------------------------------------------------------------------------
// Tests if a node is a left or right child
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
inline	bool CUtlRBTree<T, I, L, E>::IsLeftChild( I i ) const 
{ 
	return _IsLeftChild( i, sizeof( Node_t ), (void*)m_Elements.Base() );
}

template < class I, class E >
inline	bool CUtlRBTreeBase< I, E >::_IsLeftChild( I i, size_t unNodeSize, void *pMemBase ) const 
{ 
	return _LeftChild(_Parent(i, unNodeSize, pMemBase), unNodeSize, pMemBase ) == i; 
}



template <class T, class I, typename L, class E>
inline	bool CUtlRBTree<T, I, L, E>::IsRightChild( I i ) const
{ 
	return _IsRightChild( i, sizeof( Node_t ), (void*)m_Elements.Base() );
}

template < class I, class E >
inline	bool CUtlRBTreeBase< I, E >::_IsRightChild( I i, size_t unNodeSize, void *pMemBase ) const 
{ 
	return _RightChild(_Parent(i, unNodeSize, pMemBase), unNodeSize, pMemBase ) == i; 
}
	

//-----------------------------------------------------------------------------
// Tests if root or leaf
//-----------------------------------------------------------------------------

template < class I, class E >
inline	bool CUtlRBTreeBase< I, E >::IsRoot( I i ) const     
{ 
	return i == m_Root; 
}

template <class T, class I, typename L, class E>
inline	bool CUtlRBTree<T, I, L, E>::IsLeaf( I i ) const     
{ 
	return (LeftChild(i) == INVALID_RBTREE_IDX ) && (RightChild(i) == INVALID_RBTREE_IDX ); 
}
	

//-----------------------------------------------------------------------------
// Checks if a node is valid and in the tree
// the sign-comparison supression is due to comparing index val < 0, which
// is never true for unsigned types, but specializing the template seems
// like overkill
//-----------------------------------------------------------------------------

template < class T, class I, typename L, class E>
inline	bool CUtlRBTree< T, I, L, E>::IsValidIndex( I i ) const 
{ 
	return this->_IsValidIndex( i, sizeof( Node_t ), (void*)m_Elements.Base() );
}

template < class I, class E>
inline	bool CUtlRBTreeBase< I, E>::_IsValidIndex( I i, size_t unNodeSize, void *pMemBase ) const 
{ 
	// gcc correctly notices that in many places we instantiate the template
	// with an unsigned index type, and the i < 0 check is never true
#if defined (GNUC) || defined( COMPILER_SNC )
	if ( i == INVALID_RBTREE_IDX || i >= MaxElement() ) // its outside the bounds of the base container
#else
	if ( i < 0 || i >= MaxElement() ) // its outside the bounds of the base container
#endif
	{
		return false;
	}
	return _LeftChild(i, unNodeSize, pMemBase) != i; 
}

	
//-----------------------------------------------------------------------------
// returns the tree depth (not a very fast operation)
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
inline int CUtlRBTree<T, I, L, E>::Depth() const           
{ 
	return Depth(CUtlRBTreeBase< I, E >::Root()); 
}

//-----------------------------------------------------------------------------
// Sets the children
//-----------------------------------------------------------------------------

template < class I, class E >
inline void  CUtlRBTreeBase< I, E >::_SetParent( I i, I parent, size_t unSizeOfNode, void *pMemBase )       
{ 
	_Links( i, unSizeOfNode, pMemBase ).m_Parent = parent; 
}


template < class I, class E >
inline void  CUtlRBTreeBase< I, E >::_SetLeftChild( I i, I child, size_t unSizeOfNode, void *pMemBase )       
{ 
	_Links( i, unSizeOfNode, pMemBase ).m_Left = child; 
}

template < class I, class E >
inline void  CUtlRBTreeBase< I, E >::_SetRightChild( I i, I child, size_t unSizeOfNode, void *pMemBase )       
{ 
	_Links( i, unSizeOfNode, pMemBase ).m_Right = child; 
}

//-----------------------------------------------------------------------------
// Gets at the links
//-----------------------------------------------------------------------------

template < class I, class E >
inline typename CUtlRBTreeInternal::Links_t<I> const &CUtlRBTreeBase< I, E >::_Links( I i, size_t unNodeSize, void *pMemBase ) const 
{
	return (i != INVALID_RBTREE_IDX ) ? *(CUtlRBTreeInternal::Links_t<I> *)( (byte*)pMemBase + (unNodeSize*i) ) : m_Sentinel; 
}

template < class I, class E >
inline typename CUtlRBTreeInternal::Links_t<I> &CUtlRBTreeBase< I, E >::_Links( I i, size_t unNodeSize, void *pMemBase )       
{ 
	DbgAssert(i != INVALID_RBTREE_IDX ); 
	return *(CUtlRBTreeInternal::Links_t<I> *)( (byte*)pMemBase + (unNodeSize*i) );
}

//-----------------------------------------------------------------------------
// Checks if a link is red or black
//-----------------------------------------------------------------------------

template < class I, class E >
inline bool CUtlRBTreeBase< I, E >::_IsRed( I i, size_t unNodeSize, void *pMemBase ) const                
{ 
	return (_Links(i, unNodeSize, pMemBase).m_Tag == CUtlRBTreeInternal::RED); 
}

template < class I, class E >
inline bool CUtlRBTreeBase< I, E >::_IsBlack( I i, size_t unNodeSize, void *pMemBase ) const             
{ 
	return (_Links(i, unNodeSize, pMemBase).m_Tag == CUtlRBTreeInternal::BLACK); 
}


//-----------------------------------------------------------------------------
// Sets/gets node color
//-----------------------------------------------------------------------------

template < class I, class E >
inline CUtlRBTreeInternal::NodeColor_t CUtlRBTreeBase< I, E >::_Color( I i, size_t unNodeSize, void *pMemBase ) const            
{ 
	return (CUtlRBTreeInternal::NodeColor_t)_Links( i, unNodeSize, pMemBase ).m_Tag; 
}

template < class I, class E >
inline void CUtlRBTreeBase< I, E >::_SetColor( I i, CUtlRBTreeInternal::NodeColor_t c, size_t unNodeSize, void *pMemBase ) 
{ 
	_Links( i, unNodeSize, pMemBase ).m_Tag = (I)c; 
}

//-----------------------------------------------------------------------------
// Allocates/ deallocates nodes
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
I  CUtlRBTree<T, I, L, E>::NewNode( bool bConstructElement )
{
	I newElem;
	
	// Nothing in the free list; add.
	if (CUtlRBTreeBase< I, E >::m_FirstFree == INVALID_RBTREE_IDX )
	{
		if (m_Elements.NumAllocated() == CUtlRBTreeBase< I, E >::m_TotalElements)
			m_Elements.Grow();
		newElem = CUtlRBTreeBase< I, E >::m_TotalElements++;
	}
	else
	{
		newElem = CUtlRBTreeBase< I, E >::m_FirstFree;
		CUtlRBTreeBase< I, E >::m_FirstFree = RightChild(CUtlRBTreeBase< I, E >::m_FirstFree);
	}
	
#ifdef _DEBUG
	// reset links to invalid....
	CUtlRBTreeInternal::Links_t<I> &node = Links(newElem);
	node.m_Left = node.m_Right = node.m_Parent = INVALID_RBTREE_IDX;
#endif
	
	if ( bConstructElement )
		Construct( &Element(newElem) );
	this->ResetDbgInfo( m_Elements.Base() );
	
	return newElem;
}

template <class T, class I, typename L, class E>
void  CUtlRBTree<T, I, L, E>::FreeNode( I i )
{
	DbgAssert( IsValidIndex(i) && (i != INVALID_RBTREE_IDX) );
	Destruct( &Element(i) );
	SetLeftChild( i, i ); // indicates it's in not in the tree
	SetRightChild( i, CUtlRBTreeBase< I, E >::m_FirstFree );
	CUtlRBTreeBase< I, E >::m_FirstFree = i;
}


//-----------------------------------------------------------------------------
// Rotates node i to the left
//-----------------------------------------------------------------------------

template < class I, class E >
void CUtlRBTreeBase< I, E >::_RotateLeft(I elem, size_t unNodeSize, void *pMemBase ) 
{
	I rightchild = _RightChild(elem, unNodeSize, pMemBase );
	_SetRightChild( elem, _LeftChild(rightchild, unNodeSize, pMemBase), unNodeSize, pMemBase );
	if (_LeftChild(rightchild, unNodeSize, pMemBase) != INVALID_RBTREE_IDX )
		_SetParent( _LeftChild(rightchild, unNodeSize, pMemBase), elem, unNodeSize, pMemBase );

	if (rightchild != INVALID_RBTREE_IDX )
		_SetParent( rightchild, _Parent(elem, unNodeSize, pMemBase), unNodeSize, pMemBase );
	if (!IsRoot(elem))
	{
		if (_IsLeftChild(elem, unNodeSize, pMemBase))
			_SetLeftChild( _Parent(elem, unNodeSize, pMemBase), rightchild, unNodeSize, pMemBase );
		else
			_SetRightChild( _Parent(elem, unNodeSize, pMemBase), rightchild, unNodeSize, pMemBase );
	}
	else
		m_Root = rightchild;

	_SetLeftChild( rightchild, elem, unNodeSize, pMemBase );
	if (elem != INVALID_RBTREE_IDX )
		_SetParent( elem, rightchild, unNodeSize, pMemBase );

	m_data.OnRotateLeft( elem, rightchild );
}


//-----------------------------------------------------------------------------
// Rotates node i to the right
//-----------------------------------------------------------------------------

template < class I, class E >
void CUtlRBTreeBase< I, E >::_RotateRight(I elem, size_t unNodeSize, void *pMemBase ) 
{
	I leftchild = _LeftChild(elem, unNodeSize, pMemBase);
	_SetLeftChild( elem, _RightChild(leftchild, unNodeSize, pMemBase), unNodeSize, pMemBase );
	if (_RightChild(leftchild, unNodeSize, pMemBase) != INVALID_RBTREE_IDX )
		_SetParent( _RightChild(leftchild, unNodeSize, pMemBase), elem, unNodeSize, pMemBase );

	if (leftchild != INVALID_RBTREE_IDX )
		_SetParent( leftchild, _Parent(elem, unNodeSize, pMemBase), unNodeSize, pMemBase );
	if (!IsRoot(elem))
	{
		if (_IsRightChild(elem, unNodeSize, pMemBase))
			_SetRightChild( _Parent(elem, unNodeSize, pMemBase), leftchild, unNodeSize, pMemBase );
		else
			_SetLeftChild( _Parent(elem, unNodeSize, pMemBase), leftchild, unNodeSize, pMemBase );
	}
	else
		m_Root = leftchild;

	_SetRightChild( leftchild, elem, unNodeSize, pMemBase );
	if (elem != INVALID_RBTREE_IDX )
		_SetParent( elem, leftchild, unNodeSize, pMemBase );

	m_data.OnRotateRight( elem, leftchild );
}


//-----------------------------------------------------------------------------
// Rebalances the tree after an insertion
//-----------------------------------------------------------------------------

template < class I, class E >
inline void CUtlRBTreeBase< I, E >::_InsertRebalance(I elem, size_t unNodeSize, void *pMemBase)
{
	while ( !IsRoot(elem) && (_Color(_Parent(elem, unNodeSize, pMemBase), unNodeSize, pMemBase) == CUtlRBTreeInternal::RED) )
	{
		I parent = _Parent(elem, unNodeSize, pMemBase);
		I grandparent = _Parent(parent, unNodeSize, pMemBase);

		/* we have a violation */
		if (_IsLeftChild(parent, unNodeSize, pMemBase))
		{
			I uncle = _RightChild(grandparent, unNodeSize, pMemBase);
			if (_IsRed(uncle, unNodeSize, pMemBase)) 
			{
				/* uncle is RED */
				_SetColor(parent, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase);
				_SetColor(uncle, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase);
				_SetColor(grandparent, CUtlRBTreeInternal::RED, unNodeSize, pMemBase);
				elem = grandparent;
			} 
			else 
			{
				/* uncle is BLACK */
				if (_IsRightChild(elem, unNodeSize, pMemBase))
				{
					/* make x a left child, will change parent and grandparent */
					elem = parent;
					_RotateLeft(elem, unNodeSize, pMemBase);
					parent = _Parent(elem, unNodeSize, pMemBase);
					grandparent = _Parent(parent, unNodeSize, pMemBase);
				}
				/* recolor and rotate */
				_SetColor(parent, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase);
				_SetColor(grandparent, CUtlRBTreeInternal::RED, unNodeSize, pMemBase);
				_RotateRight(grandparent, unNodeSize, pMemBase);
			}
		} 
		else 
		{
			/* mirror image of above code */
			I uncle = _LeftChild(grandparent, unNodeSize, pMemBase);
			if (_IsRed(uncle, unNodeSize, pMemBase)) 
			{
				/* uncle is RED */
				_SetColor(parent, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase);
				_SetColor(uncle, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase);
				_SetColor(grandparent, CUtlRBTreeInternal::RED, unNodeSize, pMemBase);
				elem = grandparent;
			} 
			else 
			{
				/* uncle is BLACK */
				if (_IsLeftChild(elem, unNodeSize, pMemBase))
				{
					/* make x a right child, will change parent and grandparent */
					elem = parent;
					_RotateRight(parent, unNodeSize, pMemBase);
					parent = _Parent(elem, unNodeSize, pMemBase);
					grandparent = _Parent(parent, unNodeSize, pMemBase);
				}
				/* recolor and rotate */
				_SetColor(parent, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase);
				_SetColor(grandparent, CUtlRBTreeInternal::RED, unNodeSize, pMemBase);
				_RotateLeft(grandparent, unNodeSize, pMemBase);
			}
		}
	}
	_SetColor( m_Root, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase );
}


//-----------------------------------------------------------------------------
// Insert a node into the tree
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
I CUtlRBTree<T, I, L, E>::InsertAt( I parent, bool leftchild, bool bConstructElement )
{
	I i = NewNode( bConstructElement );
	LinkToParent( i, parent, leftchild );
	++CUtlRBTreeBase< I, E >::m_NumElements;
	return i;
}

template < class I, class E >
void CUtlRBTreeBase<I, E>::_LinkToParent( I i, I parent, bool isLeft, size_t unNodeSize, void *pMemBase )
{
	CUtlRBTreeInternal::Links_t<I> &elem = _Links(i, unNodeSize, pMemBase );
	elem.m_Parent = parent;
	elem.m_Left = elem.m_Right = INVALID_RBTREE_IDX;
	elem.m_Tag = CUtlRBTreeInternal::RED;
	
	/* insert node in tree */
	if (parent != INVALID_RBTREE_IDX) 
	{
		if (isLeft)
			_Links(parent, unNodeSize, pMemBase).m_Left = i;
		else
			_Links(parent, unNodeSize, pMemBase).m_Right = i;
	} 
	else 
	{
		m_Root = i;
	}
	m_data.OnLinkToParent( i );
	
	_InsertRebalance(i, unNodeSize, pMemBase );	

	DbgAssert(_IsValid( unNodeSize, pMemBase ));
}

//-----------------------------------------------------------------------------
// Rebalance the tree after a deletion
//-----------------------------------------------------------------------------

template <class I, class E>
void CUtlRBTreeBase< I, E>::_RemoveRebalance(I elem, size_t unNodeSize, void *pMemBase ) 
{
	while (elem != m_Root && _IsBlack(elem, unNodeSize, pMemBase)) 
	{
		I parent = _Parent(elem, unNodeSize, pMemBase);
		
		// If elem is the left child of the parent
		if (elem == _LeftChild(parent, unNodeSize, pMemBase)) 
		{
			// Get our sibling
			I sibling = _RightChild(parent, unNodeSize, pMemBase);
			if (_IsRed(sibling, unNodeSize, pMemBase)) 
            {
				_SetColor(sibling, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase);
				_SetColor(parent, CUtlRBTreeInternal::RED, unNodeSize, pMemBase);
				_RotateLeft(parent, unNodeSize, pMemBase);
				
				// We may have a new parent now
				parent = _Parent(elem, unNodeSize, pMemBase);
				sibling = _RightChild(parent, unNodeSize, pMemBase);
            }
			if ( (_IsBlack(_LeftChild(sibling, unNodeSize, pMemBase), unNodeSize, pMemBase)) && (_IsBlack(_RightChild(sibling, unNodeSize, pMemBase), unNodeSize, pMemBase)) ) 
            {
				if (sibling != INVALID_RBTREE_IDX)
					_SetColor(sibling, CUtlRBTreeInternal::RED, unNodeSize, pMemBase);
				elem = parent;
            }
			else
            {
				if (_IsBlack(_RightChild(sibling, unNodeSize, pMemBase), unNodeSize, pMemBase))
				{
					_SetColor(_LeftChild(sibling, unNodeSize, pMemBase), CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase);
					_SetColor(sibling, CUtlRBTreeInternal::RED, unNodeSize, pMemBase);
					_RotateRight(sibling, unNodeSize, pMemBase);
					
					// rotation may have changed this
					parent = _Parent(elem, unNodeSize, pMemBase);
					sibling = _RightChild(parent, unNodeSize, pMemBase);
				}
				_SetColor( sibling, _Color(parent, unNodeSize, pMemBase), unNodeSize, pMemBase );
				_SetColor( parent, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase );
				_SetColor( _RightChild(sibling, unNodeSize, pMemBase), CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase );
				_RotateLeft( parent, unNodeSize, pMemBase );
				elem = m_Root;
            }
		}
		else 
		{
			// Elem is the right child of the parent
			I sibling = _LeftChild(parent, unNodeSize, pMemBase);
			if (_IsRed(sibling, unNodeSize, pMemBase)) 
            {
				_SetColor(sibling, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase);
				_SetColor(parent, CUtlRBTreeInternal::RED, unNodeSize, pMemBase);
				_RotateRight(parent, unNodeSize, pMemBase);
				
				// We may have a new parent now
				parent = _Parent(elem, unNodeSize, pMemBase);
				sibling = _LeftChild(parent, unNodeSize, pMemBase);
            }
			if ( (_IsBlack(_RightChild(sibling, unNodeSize, pMemBase), unNodeSize, pMemBase)) && (_IsBlack(_LeftChild(sibling, unNodeSize, pMemBase), unNodeSize, pMemBase)) )
            {
				if (sibling != INVALID_RBTREE_IDX) 
					_SetColor( sibling, CUtlRBTreeInternal::RED, unNodeSize, pMemBase );
				elem = parent;
            } 
			else 
            {
				if (_IsBlack(_LeftChild(sibling, unNodeSize, pMemBase), unNodeSize, pMemBase))
				{
					_SetColor( _RightChild(sibling, unNodeSize, pMemBase), CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase );
					_SetColor( sibling, CUtlRBTreeInternal::RED, unNodeSize, pMemBase );
					_RotateLeft( sibling, unNodeSize, pMemBase );
					
					// rotation may have changed this
					parent = _Parent(elem, unNodeSize, pMemBase);
					sibling = _LeftChild(parent, unNodeSize, pMemBase);
				}
				_SetColor( sibling, _Color(parent, unNodeSize, pMemBase), unNodeSize, pMemBase );
				_SetColor( parent, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase );
				_SetColor( _LeftChild(sibling, unNodeSize, pMemBase), CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase );
				_RotateRight( parent, unNodeSize, pMemBase );
				elem = m_Root;
            }
		}
	}
	_SetColor( elem, CUtlRBTreeInternal::BLACK, unNodeSize, pMemBase );
}

template < class I, class E>
void CUtlRBTreeBase<I, E>::_Unlink( I elem, size_t unNodeSize, void *pMemBase )
{
	Assert( _IsValidIndex( elem, unNodeSize, pMemBase ) );
	if ( _IsValidIndex( elem, unNodeSize, pMemBase ) )
	{
		I x, y;
		
		if ((_LeftChild(elem, unNodeSize, pMemBase) == INVALID_RBTREE_IDX) || 
			(_RightChild(elem, unNodeSize, pMemBase) == INVALID_RBTREE_IDX))
		{
			/* y has a NIL node as a child */
			y = elem;
		}
		else
		{
			/* find tree successor with a NIL node as a child */
			y = _RightChild(elem, unNodeSize, pMemBase);
			while (_LeftChild(y, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
			{
				Assert( _IsValidIndex( y, unNodeSize, pMemBase ) );
				if ( !_IsValidIndex( y, unNodeSize, pMemBase ) )
					return;

				y = _LeftChild(y, unNodeSize, pMemBase);
			}
		}

		// Need to notify any listeners that we are going to unlink this node
		m_data.OnPreUnlink( y );
		
		/* x is y's only child */
		if (_LeftChild(y, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
			x = _LeftChild(y, unNodeSize, pMemBase);
		else
			x = _RightChild(y, unNodeSize, pMemBase);
		
		/* remove y from the parent chain */
		if (x != INVALID_RBTREE_IDX)
			_SetParent( x, _Parent(y, unNodeSize, pMemBase), unNodeSize, pMemBase );
		if (!IsRoot(y))
		{
			if (_IsLeftChild(y, unNodeSize, pMemBase))
				_SetLeftChild( _Parent(y, unNodeSize, pMemBase), x, unNodeSize, pMemBase );
			else
				_SetRightChild( _Parent(y, unNodeSize, pMemBase), x, unNodeSize, pMemBase );
		}
		else
			m_Root = x;
		
		// need to store this off now, we'll be resetting y's color
		CUtlRBTreeInternal::NodeColor_t ycolor = _Color(y, unNodeSize, pMemBase);
		if (y != elem)
		{
			// Standard implementations copy the data around, we cannot here.
			// Hook in y to link to the same stuff elem used to.
			_SetParent( y, _Parent(elem, unNodeSize, pMemBase), unNodeSize, pMemBase );
			_SetRightChild( y, _RightChild(elem, unNodeSize, pMemBase), unNodeSize, pMemBase );
			_SetLeftChild( y, _LeftChild(elem, unNodeSize, pMemBase), unNodeSize, pMemBase );
			
			if (!IsRoot(elem))
				if (_IsLeftChild(elem, unNodeSize, pMemBase))
					_SetLeftChild( _Parent(elem, unNodeSize, pMemBase), y, unNodeSize, pMemBase );
				else
					_SetRightChild( _Parent(elem, unNodeSize, pMemBase), y, unNodeSize, pMemBase );
				else
					m_Root = y;
				
				if (_LeftChild(y, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
					_SetParent( _LeftChild(y, unNodeSize, pMemBase), y, unNodeSize, pMemBase );
				if (_RightChild(y, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
					_SetParent( _RightChild(y, unNodeSize, pMemBase), y, unNodeSize, pMemBase );
				
				_SetColor( y, _Color(elem, unNodeSize, pMemBase), unNodeSize, pMemBase );

			m_data.OnRelinkSuccessor( y );
		}
		
		if ((x != INVALID_RBTREE_IDX) && (ycolor == CUtlRBTreeInternal::BLACK))
			_RemoveRebalance(x, unNodeSize, pMemBase);
	}
}

template <class T, class I, typename L, class E>
void CUtlRBTree<T, I, L, E>::Link( I elem )
{
	if ( elem != INVALID_RBTREE_IDX )
	{
		I parent;
		bool leftchild;
		bool duplicate;

		FindInsertionPosition( Element( elem ), false,  parent, leftchild, duplicate );

		LinkToParent( elem, parent, leftchild );
	}
}

//-----------------------------------------------------------------------------
// Delete a node from the tree
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
void CUtlRBTree<T, I, L, E>::RemoveAt(I elem) 
{
	Assert( IsValidIndex( elem ) );
	if ( IsValidIndex( elem ) )
	{
		Unlink( elem );

		FreeNode(elem);
		--CUtlRBTreeBase< I, E >::m_NumElements;
	}
}


//-----------------------------------------------------------------------------
// remove a node in the tree
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E> bool CUtlRBTree<T, I, L, E>::Remove( T const &search )
{
	I node = Find( search );
	if (node != INVALID_RBTREE_IDX)
	{
		RemoveAt(node);
		return true;
	}
	return false;
}


//-----------------------------------------------------------------------------
// Removes all nodes from the tree
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
void CUtlRBTree<T, I, L, E>::RemoveAll()
{
	// Just iterate through the whole list and add to free list
	// much faster than doing all of the rebalancing
	// also, do it so the free list is pointing to stuff in order
	// to get better cache coherence when re-adding stuff to this tree.
	I prev = INVALID_RBTREE_IDX;
	for (int i = (int)CUtlRBTreeBase< I, E >::m_TotalElements; --i >= 0; )
	{
		I idx = (I)i;
		if (IsValidIndex(idx))
			Destruct( &Element(idx) );
		SetRightChild( idx, prev );
		SetLeftChild( idx, idx );
		prev = idx;
	}
	CUtlRBTreeBase< I, E >::m_FirstFree = CUtlRBTreeBase< I, E >::m_TotalElements ? (I)0 : INVALID_RBTREE_IDX;
	CUtlRBTreeBase< I, E >::m_Root = INVALID_RBTREE_IDX;
	CUtlRBTreeBase< I, E >::m_NumElements = 0;
}


//-----------------------------------------------------------------------------
// purge
//-----------------------------------------------------------------------------
template <class T, class I, typename L, class E>
void CUtlRBTree<T, I, L, E>::Purge()
{
	RemoveAll();
	CUtlRBTreeBase< I, E >::m_FirstFree = INVALID_RBTREE_IDX;
	CUtlRBTreeBase< I, E >::m_TotalElements = 0;
	m_Elements.Purge();
	this->ResetDbgInfo( m_Elements.Base() );
}

//-----------------------------------------------------------------------------
// iteration
//-----------------------------------------------------------------------------
template <class T, class I, typename L, class E>
inline I CUtlRBTree<T, I, L, E>::FirstInorder() const
{
	return CUtlRBTreeBase<I,E>::_FirstInorder( sizeof( Node_t ), (void *)m_Elements.Base() );
}

template <class I, class E>
I CUtlRBTreeBase<I, E>::_FirstInorder( size_t unNodeSize, void *pMemBase ) const
{
	I i = m_Root;
	while (_LeftChild(i, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
		i = _LeftChild(i, unNodeSize, pMemBase);
	return i;
}


template <class T, class I, typename L, class E>
inline I CUtlRBTree<T, I, L, E>::NextInorder( I i ) const
{
	return CUtlRBTreeBase<I,E>::_NextInorder( i, sizeof( Node_t), (void *)m_Elements.Base() );
}

template <class I, class E>
I CUtlRBTreeBase<I, E>::_NextInorder( I i, size_t unNodeSize, void *pMemBase ) const
{
	DbgAssert(_IsValidIndex(i, unNodeSize, pMemBase ));
	
	if (_RightChild(i, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
	{
		i = _RightChild(i, unNodeSize, pMemBase);
		while (_LeftChild(i, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
			i = _LeftChild(i, unNodeSize, pMemBase);
		return i;
	}
	
	I parent = _Parent(i, unNodeSize, pMemBase);
	while (_IsRightChild(i, unNodeSize, pMemBase))
	{
		i = parent;
		if (i == INVALID_RBTREE_IDX) break;
		parent = _Parent(i, unNodeSize, pMemBase);
	}
	return parent;
}

template <class T, class I, typename L, class E>
inline I CUtlRBTree<T, I, L, E>::PrevInorder( I i ) const
{
	return CUtlRBTreeBase<I,E>::_PrevInorder( i, sizeof( Node_t), (void *)m_Elements.Base() );
}


template <class I, class E>
I CUtlRBTreeBase<I, E>::_PrevInorder( I i, size_t unNodeSize, void *pMemBase ) const
{
	DbgAssert(_IsValidIndex(i, unNodeSize, pMemBase));
	
	if (_LeftChild(i, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
	{
		i = _LeftChild(i, unNodeSize, pMemBase);
		while (_RightChild(i, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
			i = _RightChild(i, unNodeSize, pMemBase);
		return i;
	}
	
	I parent = _Parent(i, unNodeSize, pMemBase);
	while (_IsLeftChild(i, unNodeSize, pMemBase))
	{
		i = parent;
		if (i == INVALID_RBTREE_IDX) break;
		parent = _Parent(i, unNodeSize, pMemBase);
	}
	return parent;
}

template <class T, class I, typename L, class E>
inline I CUtlRBTree<T, I, L, E>::LastInorder() const
{
	return CUtlRBTreeBase<I,E>::_LastInorder( sizeof( Node_t), (void *)m_Elements.Base() );
}

template <class I, class E>
I CUtlRBTreeBase<I, E>::_LastInorder( size_t unNodeSize, void *pMemBase ) const
{
	I i = m_Root;
	while (_RightChild(i, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
		i = _RightChild(i, unNodeSize, pMemBase);
	return i;
}

template <class I, class E>
I CUtlRBTreeBase< I, E >::FirstPreorder() const
{
	return m_Root;
}

template <class T, class I, typename L, class E>
inline I CUtlRBTree<T, I, L, E>::NextPreorder( I i ) const
{
	return CUtlRBTreeBase<I,E>::_NextPreorder( i, sizeof( Node_t), (void *)m_Elements.Base() );
}

template <class I, class E>
I CUtlRBTreeBase<I, E>::_NextPreorder( I i, size_t unNodeSize, void *pMemBase  ) const
{
	if (_LeftChild(i, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
		return _LeftChild(i, unNodeSize, pMemBase);
	
	if (_RightChild(i, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
		return _RightChild(i, unNodeSize, pMemBase);
	
	I parent = _Parent(i, unNodeSize, pMemBase);
	while( parent != INVALID_RBTREE_IDX)
	{
		if ( _IsLeftChild(i, unNodeSize, pMemBase) && (_RightChild(parent) != INVALID_RBTREE_IDX) )
			return _RightChild(parent, unNodeSize, pMemBase);
		i = parent;
		parent = _Parent(parent, unNodeSize, pMemBase);
	}
	return INVALID_RBTREE_IDX;
}

template <class I, class E>
I CUtlRBTreeBase<I, E>::PrevPreorder( I i ) const
{
	Assert(0);  // not implemented yet
	return INVALID_RBTREE_IDX;
}

template <class T, class I, typename L, class E>
inline I CUtlRBTree<T, I, L, E>::LastPreorder() const
{
	return CUtlRBTreeBase<I,E>::_LastPreorder( sizeof( Node_t), (void *)m_Elements.Base() );
}

template < class I, class E >
I CUtlRBTreeBase<I, E>::_LastPreorder( size_t unNodeSize, void *pMemBase ) const
{
	I i = m_Root;
	for (;;)
	{
		while (_RightChild(i, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
			i = _RightChild(i, unNodeSize, pMemBase);
		
		if (_LeftChild(i, unNodeSize, pMemBase) != INVALID_RBTREE_IDX)
			i = _LeftChild(i, unNodeSize, pMemBase);
		else
			break;
	}
	return i;
}

template <class T, class I, typename L, class E>
inline I CUtlRBTree<T, I, L, E>::FirstPostorder() const
{
	return CUtlRBTreeBase<I,E>::_FirstPostorder( sizeof( Node_t), (void *)m_Elements.Base() );
}

template < class I, class E>
I CUtlRBTreeBase<I, E>::_FirstPostorder(size_t unNodeSize, void *pMemBase) const
{
	I i = m_Root;
	while (!_IsLeaf(i, unNodeSize, pMemBase))
	{
		if (_LeftChild(i, unNodeSize, pMemBase))
			i = _LeftChild(i, unNodeSize, pMemBase);
		else
			i = _RightChild(i, unNodeSize, pMemBase);
	}
	return i;
}

template <class T, class I, typename L, class E>
inline I CUtlRBTree<T, I, L, E>::NextPostorder( I i ) const
{
	return CUtlRBTreeBase<I,E>::_NextPostorder( sizeof( Node_t), (void *)m_Elements.Base() );
}

template <class I, class E>
I CUtlRBTreeBase<I, E>::_NextPostorder( I i, size_t unNodeSize, void *pMemBase ) const
{

	I parent = _Parent(i, unNodeSize, pMemBase);
	if (parent == INVALID_RBTREE_IDX)
		return INVALID_RBTREE_IDX;
	
	if (_IsRightChild(i, unNodeSize, pMemBase))
		return parent;
	
	if (_RightChild(parent, unNodeSize, pMemBase) == INVALID_RBTREE_IDX)
		return parent;
	
	i = _RightChild(parent, unNodeSize, pMemBase);
	while (!_IsLeaf(i, unNodeSize, pMemBase))
	{
		if (_LeftChild(i, unNodeSize, pMemBase))
			i = _LeftChild(i, unNodeSize, pMemBase);
		else
			i = _RightChild(i, unNodeSize, pMemBase);
	}
	return i;
}


template <class T, class I, typename L, class E>
void CUtlRBTree<T, I, L, E>::Reinsert( I elem )
{
	Unlink( elem );
	Link( elem );
}


//-----------------------------------------------------------------------------
// returns the tree depth (not a very fast operation)
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
inline int CUtlRBTree<T, I, L, E>::Depth( I node ) const
{
	return CUtlRBTreeBase<I,E>::_Depth( node, sizeof( Node_t ), (void*)m_Elements.Base() );
}

template <class I, class E>
int CUtlRBTreeBase<I, E>::_Depth( I node, size_t unNodeSize, void *pMemBase ) const
{
	if (node == INVALID_RBTREE_IDX)
		return 0;
	
	int depthright = _Depth( _RightChild(node, unNodeSize, pMemBase), unNodeSize, pMemBase );
	int depthleft = _Depth( _LeftChild(node, unNodeSize, pMemBase), unNodeSize, pMemBase );
	return ( MAX(depthright, depthleft) + 1 );
}


//-----------------------------------------------------------------------------
// Makes sure the tree is valid after every operation
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>
inline bool CUtlRBTree<T, I, L, E>::IsValid() const
{
	return CUtlRBTreeBase<I, E>::_IsValid( sizeof( Node_t), (void *)m_Elements.Base() );
}

template <class I, class E>
bool CUtlRBTreeBase<I, E>::_IsValid( size_t unNodeSize, void *pMemBase ) const
{
	if ( !Count() )
		return true;
	
	if ((Root() >= MaxElement()) || ( _Parent( Root(), unNodeSize, pMemBase ) != INVALID_RBTREE_IDX )) 
		goto InvalidTree;
	
#ifdef UTLTREE_PARANOID
	
	// First check to see that mNumEntries matches reality.
	// count items on the free list
	int numFree = 0;
	int curr = m_FirstFree;
	while (curr != INVALID_RBTREE_IDX)
	{
		++numFree;
		curr = _RightChild(curr, unNodeSize, pMemBase);
		if ( (curr > MaxElement()) && (curr != INVALID_RBTREE_IDX) )
			goto InvalidTree;
	}
	if (MaxElement() - numFree != Count())
		goto InvalidTree;
	
	// iterate over all elements, looking for validity 
	// based on the self pointers
	int numFree2 = 0;
	for (curr = 0; curr < MaxElement(); ++curr)
	{
		if (!IsValidIndex(curr))
			++numFree2;
		else
		{
			int right = _RightChild(curr, unNodeSize, pMemBase);
			int left = _LeftChild(curr, unNodeSize, pMemBase);
			if ((right == left) && (right != INVALID_RBTREE_IDX) )
				goto InvalidTree;
			
			if (right != INVALID_RBTREE_IDX)
            {
				if (!_IsValidIndex(right, unNodeSize, pMemBase)) 
					goto InvalidTree;
				if (_Parent(right, unNodeSize, pMemBase) != curr) 
					goto InvalidTree;
				if (_IsRed(curr, unNodeSize, pMemBase) && _IsRed(right, unNodeSize, pMemBase)) 
					goto InvalidTree;
            }
			
			if (left != INVALID_RBTREE_IDX)
            {
				if (!_IsValidIndex(left, unNodeSize, pMemBase)) 
					goto InvalidTree;
				if (_Parent(left, unNodeSize, pMemBase) != curr) 
					goto InvalidTree;
				if (_IsRed(curr, unNodeSize, pMemBase) && _IsRed(left, unNodeSize, pMemBase)) 
					goto InvalidTree;
            }
		}
	}
	if (numFree2 != numFree)
		goto InvalidTree;
	
#endif // UTLTREE_PARANOID
	
	return true;
	
InvalidTree:
	return false;
}


//-----------------------------------------------------------------------------
// Sets the less func
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E>  
void CUtlRBTree<T, I, L, E>::SetLessFunc( const typename CUtlRBTree<T, I, L, E>::LessFunc_t &func )
{
	if (!m_LessFunc)
		m_LessFunc = func;
	else
	{
		// need to re-sort the tree here....
		Assert(0);
	}
}


//-----------------------------------------------------------------------------
// inserts a node into the tree
//-----------------------------------------------------------------------------

// Inserts a node into the tree, doesn't copy the data in.
template <class T, class I, typename L, class E> 
void CUtlRBTree<T, I, L, E>::FindInsertionPosition( T const &insert, bool bCheckForDuplicates, I &parent, bool &leftchild, bool &isDuplicate )
{
	Assert( m_LessFunc );
	
	/* find where node belongs */
	I current = CUtlRBTreeBase< I, E >::m_Root;
	parent = INVALID_RBTREE_IDX;
	leftchild = false;
	isDuplicate = false;
	while (current != INVALID_RBTREE_IDX) 
	{
		parent = current;
		if ( m_LessFunc( insert, Element(current) ) )
		{
			leftchild = true; current = LeftChild(current);
		}
		else if ( bCheckForDuplicates && !m_LessFunc( Element(current), insert ) )
		{	
			// we know that insert >= current, and current >= insert, 
			// hence insert == current - this item is already in the tree
			leftchild = false; isDuplicate = true; current = INVALID_RBTREE_IDX;
		}
		else
		{
			leftchild = false; current = RightChild(current);
		}
	}
}

template <class T, class I, typename L, class E> 
I CUtlRBTree<T, I, L, E>::Insert( T const &insert, bool bInsertDuplicates )
{
	// use copy constructor to copy it in
	I parent;
	bool leftchild;
	bool isDuplicate;
	// note that the bCheckForDuplicates arg to FindInsertionPosition is !bInsertDuplicates
	FindInsertionPosition( insert, !bInsertDuplicates, parent, leftchild, isDuplicate );
	if ( !isDuplicate || bInsertDuplicates )
	{
		I newNode = InsertAt( parent, leftchild, false );  // don't construct the element as we are about to overwrite it in the copy construct
		CopyConstruct( &Element( newNode ), insert );
		return newNode;
	}
	else
	{
		// update the node in place
		Element( parent ) = insert;
		return parent;
	}
}


template <class T, class I, typename L, class E>
I CUtlRBTree<T, I, L, E>::InsertIfNotFound(T const &insert)
{
	// use copy constructor to copy it in
	I parent;
	bool leftchild;
	bool isDuplicate;

	FindInsertionPosition(insert, true /*bCheckForDuplicates*/, parent, leftchild, isDuplicate);
	if ( !isDuplicate )
	{
		I newNode = InsertAt(parent, leftchild, false);  // don't construct the element as we are about to overwrite it in the copy construct
		CopyConstruct(&Element(newNode), insert);
		return newNode;
	}
	else
	{
		return INVALID_RBTREE_IDX;
	}
}


template <class T, class I, typename L, class E> 
void CUtlRBTree<T, I, L, E>::Insert( const T *pArray, int nItems, bool bInsertDuplicates )
{
	while ( nItems-- )
	{
		Insert( *pArray++, bInsertDuplicates );
	}
}


template <class T, class I, typename L, class E> 
I CUtlRBTree<T, I, L, E>::FindOrInsert( T const &insert )
{
	// use copy constructor to copy it in
	I parent;
	bool leftchild;
	bool isDuplicate;
	FindInsertionPosition( insert, true /*bCheckForDuplicates*/, parent, leftchild, isDuplicate );
	if ( !isDuplicate )
	{
		I newNode = InsertAt( parent, leftchild, false );  // don't construct the element as we are about to overwrite it in the copy construct
		CopyConstruct( &Element( newNode ), insert );
		return newNode;
	}
	else
	{
		// return existing node without updating
		return parent;
	}
}

//-----------------------------------------------------------------------------
// returns true if the node exists in the tree
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E> 
bool CUtlRBTree<T, I, L, E>::HasElement( T const &search ) const
{
	return Find( search ) != INVALID_RBTREE_IDX;
}


//-----------------------------------------------------------------------------
// finds a node in the tree
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E> 
I CUtlRBTree<T, I, L, E>::Find( T const &search ) const
{
	Assert( m_LessFunc );
	
	I current = CUtlRBTreeBase< I, E >::m_Root;
	while (current != INVALID_RBTREE_IDX) 
	{
		if (m_LessFunc( search, Element(current) ))
			current = LeftChild(current);
		else if (m_LessFunc( Element(current), search ))
			current = RightChild(current);
		else 
			break;
	}
	return current;
}


//-----------------------------------------------------------------------------
// finds a the first node (inorder) with this key in the tree
//-----------------------------------------------------------------------------

template <class T, class I, typename L, class E> 
I CUtlRBTree<T, I, L, E>::FindFirst( T const &search ) const
{
	Assert( m_LessFunc );
	
	I current = CUtlRBTreeBase< I, E >::m_Root;
	I best =INVALID_RBTREE_IDX;
	while (current != INVALID_RBTREE_IDX) 
	{
		if (m_LessFunc( search, Element(current) ))
			current = LeftChild(current);
		else if (m_LessFunc( Element(current), search ))
			current = RightChild(current);
		else
		{
			best = current;
			current = LeftChild(current);
		}
	}
	return best;
}


//-----------------------------------------------------------------------------
// swap in place
//-----------------------------------------------------------------------------
template <class T, class I, typename L, class E> 
void CUtlRBTree<T, I, L, E>::Swap( CUtlRBTree< T, I, L, E > &that )
{
	m_Elements.Swap( that.m_Elements );
	SWAP( m_LessFunc, that.m_LessFunc );
	SWAP( CUtlRBTreeBase< I, E >::m_Root, that.m_Root );
	SWAP( CUtlRBTreeBase< I, E >::m_NumElements, that.m_NumElements );
	SWAP( CUtlRBTreeBase< I, E >::m_FirstFree, that.m_FirstFree );
	SWAP( CUtlRBTreeBase< I, E >::m_TotalElements, that.m_TotalElements );
	SWAP( CUtlRBTreeBase< I, E >::m_data, that.m_data );
}


//-----------------------------------------------------------------------------
// finds the closest node to the key supplied
//-----------------------------------------------------------------------------
template <class T, class I, typename L, class E> 
I CUtlRBTree<T, I, L, E>::FindClosest( T const &search, CompareOperands_t eFindCriteria ) const
{
	Assert( m_LessFunc );
	Assert( (eFindCriteria & ( k_EGreaterThan | k_ELessThan )) ^ ( k_EGreaterThan | k_ELessThan ) );

	I current = CUtlRBTreeBase< I, E >::m_Root;
	I best = INVALID_RBTREE_IDX;

	while (current != INVALID_RBTREE_IDX) 
	{
		if (m_LessFunc( search, Element(current) ))
		{
			// current node is > key
			if ( eFindCriteria & k_EGreaterThan )
				best = current;
			current = LeftChild(current);
		}
		else if (m_LessFunc( Element(current), search ))
		{
			// current node is < key
			if ( eFindCriteria & k_ELessThan )
				best = current;
			current = RightChild(current);
		}
		else 
		{
			// exact match
			if ( eFindCriteria & k_EEqual )
			{
				best = current;
				break;
			}
			else if ( eFindCriteria & k_EGreaterThan )
			{
				current = RightChild(current);
			}
			else if ( eFindCriteria & k_ELessThan )
			{
				current = LeftChild(current);
			}
		}
	}
	return best;
}

//-----------------------------------------------------------------------------
// Makes sure we have enough memory allocated to store a requested # of elements
//-----------------------------------------------------------------------------
template <class T, class I, typename L, class E> 
void CUtlRBTree<T, I, L, E>::EnsureCapacity( int num )
{
	m_Elements.EnsureCapacity(num);
	this->ResetDbgInfo( m_Elements.Base() );
}



//-----------------------------------------------------------------------------
// Purpose: Utility function for diffing RBTrees
//-----------------------------------------------------------------------------
template <class T, class I, typename L, class E>
bool CUtlRBTree<T, I, L, E>::BDiffRBTrees( const CUtlRBTree<T, I, L, E> &rbTreeBase, const CUtlRBTree<T, I, L, E> &rbTreeCompare, CUtlRBTree<T, I, L, E> *prbTreeAdditions /* = NULL */, CUtlRBTree<T, I, L, E> *prbTreeDeletions /* = NULL */ )
{
	I iBase = rbTreeBase.FirstInorder();
	I iCompare = rbTreeCompare.FirstInorder();

	bool bDiffer = false;
	bool bStopOnFirstDifference = ( !prbTreeAdditions && !prbTreeDeletions );

	const CUtlRBTree<T, I, L, E>::LessFunc_t lessFunc = rbTreeBase.m_LessFunc;
	Assert( lessFunc );

	// can we do this the easy way?
	if ( bStopOnFirstDifference && rbTreeBase.Count() != rbTreeCompare.Count() )
		return true;

	if ( prbTreeAdditions && !prbTreeAdditions->m_LessFunc )
		prbTreeAdditions->SetLessFunc( lessFunc );
	if ( prbTreeDeletions && !prbTreeDeletions->m_LessFunc )
		prbTreeDeletions->SetLessFunc( lessFunc );

	while( rbTreeBase.IsValidIndex( iBase ) || rbTreeCompare.IsValidIndex( iCompare ) || ( bStopOnFirstDifference && bDiffer ) )
	{
		const T *pValBase = NULL;
		const T *pValCompare = NULL;
		if ( rbTreeBase.IsValidIndex( iBase ) )
		{
			pValBase = &(rbTreeBase[iBase]);
		}
		if ( rbTreeCompare.IsValidIndex( iCompare ) )
		{
			pValCompare = &(rbTreeCompare[iCompare]);
		}

		if ( pValCompare && ( !pValBase || lessFunc( *pValCompare, *pValBase ) ) )
		{
			if ( prbTreeAdditions )
				prbTreeAdditions->Insert( *pValCompare );
			bDiffer = true;
			iCompare = rbTreeCompare.NextInorder( iCompare );
		}
		else if ( pValBase && ( !pValCompare || lessFunc( *pValBase, *pValCompare ) ) )
		{
			if ( prbTreeDeletions )
				prbTreeDeletions->Insert( *pValBase );
			bDiffer = true;
			iBase = rbTreeBase.NextInorder( iBase );
		}
		else
		{
			// we got values for both, advance both
			iBase = rbTreeBase.NextInorder( iBase );
			iCompare = rbTreeCompare.NextInorder( iCompare );
		}
	}

	return bDiffer;
}

//-----------------------------------------------------------------------------
// Data and memory validation
//-----------------------------------------------------------------------------
#ifdef DBGFLAG_VALIDATE
template <class T, class I, typename L, class E> 
void CUtlRBTree<T, I, L, E>::Validate( CValidator &validator, const char *pchName )
{
#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), this, pchName );
#else
	validator.Push( typeid(*this).name(), this, pchName );
#endif

	m_Elements.Validate( validator, "m_Elements" );

	validator.Pop();
}
#endif // DBGFLAG_VALIDATE


#endif // UTLRBTREE_H
