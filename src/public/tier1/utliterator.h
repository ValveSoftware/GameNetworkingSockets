//========= Copyright (c) 1996-2016, Valve Corporation, All rights reserved. ============//
//
// Purpose: class for implementing STL-compatible iterators on CUtl* containers
//
//=============================================================================//

#ifndef UTLITERATOR_H
#define UTLITERATOR_H

#include "tier0/dbg.h"

//-----------------------------------------------------------------------------
// Forward declarations of tag types and some template helper classes
//-----------------------------------------------------------------------------

#if !defined( OSX ) && !defined( IOS ) && !defined( TVOS ) && !defined(COMPILER_CLANG)
namespace std
{
	struct forward_iterator_tag;
	struct bidirectional_iterator_tag;
	template < typename T > struct iterator_traits;
}
#else
// Apple's STL implementation uses internal namespace shenanigans. just include the header.
#include <iterator>
#endif

template < bool bSwitch, typename IfTrue_t, typename IfFalse_t >
struct CUtlIteratorBase_SelectIf
{
	typedef IfTrue_t type;
};

template < typename IfTrue_t, typename IfFalse_t >
struct CUtlIteratorBase_SelectIf< false, IfTrue_t, IfFalse_t >
{
	typedef IfFalse_t type;
};


//-----------------------------------------------------------------------------
// Container must implement ElemType_t, IndexType_t, const and non-const begin(), end(), Element(idx), and const IteratorNext(idx)
//-----------------------------------------------------------------------------
template < typename Container, bool bConstIterator >
class CUtlForwardIteratorImplT
{
public:
	typedef typename CUtlIteratorBase_SelectIf< bConstIterator, const Container, Container >::type Container_t;
	typedef typename Container::IndexType_t IndexType_t;
	
	// STL-like typedefs
	typedef std::forward_iterator_tag iterator_category;
	typedef typename CUtlIteratorBase_SelectIf< bConstIterator, const typename Container::ElemType_t, typename Container::ElemType_t >::type value_type;
	typedef value_type * pointer;
	typedef value_type & reference;
	typedef intp difference_type;

	CUtlForwardIteratorImplT() { }
	CUtlForwardIteratorImplT( const CUtlForwardIteratorImplT< Container, false >& copy_or_requalify_to_const )
		: m_pContainer( copy_or_requalify_to_const._container() ), m_iElement( copy_or_requalify_to_const._index() ) { }

	CUtlForwardIteratorImplT& operator++() // pre-increment
	{
		DbgAssert( m_pContainer && *this != m_pContainer->end() );
		m_iElement = m_pContainer->IteratorNext( m_iElement );
		return *this;
	}

	CUtlForwardIteratorImplT operator++(int) // post-increment
	{
		CUtlForwardIteratorImplT pre = *this;
		this->operator++();
		return pre;
	}

	template < bool bOtherConst >
	bool operator==( const CUtlForwardIteratorImplT< Container, bOtherConst >& other ) const
	{
		DbgAssert( m_pContainer == other._container() );
		return m_iElement == other._index();
	}

	template < bool bOtherConst >
	bool operator!=( const CUtlForwardIteratorImplT< Container, bOtherConst >& other ) const
	{
		DbgAssert( m_pContainer == other._container() );
		return m_iElement != other._index();
	}

	value_type & operator*() const { return m_pContainer->Element( m_iElement ); }
	value_type * operator->() const { return &m_pContainer->Element( m_iElement ); }

	Container_t * _container() const { return m_pContainer; }
	IndexType_t _index() const { return m_iElement; }

protected:
	Container_t *m_pContainer = nullptr;
	IndexType_t m_iElement = 0;
	 
	friend Container;
	CUtlForwardIteratorImplT( Container_t *pContainer, IndexType_t iElement ) : m_pContainer( pContainer ), m_iElement( iElement ) { }
};


//-----------------------------------------------------------------------------
// Container must implement all of the forward-iterator requirements IN ADDITION TO const IteratorPrev(idx)
//-----------------------------------------------------------------------------
template < typename Container, bool bConstIterator >
class CUtlBidirectionalIteratorImplT : public CUtlForwardIteratorImplT< Container, bConstIterator >
{
protected:
	typedef CUtlForwardIteratorImplT< Container, bConstIterator > Base;
	using Base::m_pContainer;
	using Base::m_iElement;

public:
	typedef std::bidirectional_iterator_tag iterator_category;

	CUtlBidirectionalIteratorImplT() { }
	CUtlBidirectionalIteratorImplT( const CUtlBidirectionalIteratorImplT< Container, false >& copy_or_requalify_to_const )
		: Base( copy_or_requalify_to_const ) { }
	
	CUtlBidirectionalIteratorImplT& operator++() // pre-increment
	{
		Base::operator++();
		return *this;
	}

	CUtlBidirectionalIteratorImplT operator++(int) // post-increment
	{
		CUtlBidirectionalIteratorImplT pre;
		Base::operator++();
		return pre;
	}

	CUtlBidirectionalIteratorImplT& operator--() // pre-decrement
	{
		DbgAssert( m_pContainer && *this != m_pContainer->begin() );
		m_iElement = m_pContainer->IteratorPrev( m_iElement );
		return *this;
	}

	CUtlBidirectionalIteratorImplT operator--(int) // post-decrement
	{
		CUtlBidirectionalIteratorImplT pre = *this;
		this->operator--();
		return pre;
	}

protected:
	friend Container;
	CUtlBidirectionalIteratorImplT( typename Base::Container_t *pContainer, typename Base::IndexType_t iElement ) : Base( pContainer, iElement ) { }
};


//-----------------------------------------------------------------------------
// STL iterator traits
//-----------------------------------------------------------------------------
namespace std
{
	template < typename Container, bool bConst >
	struct iterator_traits< CUtlForwardIteratorImplT< Container, bConst > >
	{
		typedef forward_iterator_tag iterator_category;
		typedef typename CUtlForwardIteratorImplT< Container, bConst >::value_type value_type;
		typedef intp difference_type;
		typedef value_type * pointer;
		typedef value_type & reference;
	};

	template < typename Container, bool bConst >
	struct iterator_traits< CUtlBidirectionalIteratorImplT< Container, bConst > >
	{
		typedef bidirectional_iterator_tag iterator_category;
		typedef typename CUtlBidirectionalIteratorImplT< Container, bConst >::value_type value_type;
		typedef intp difference_type;
		typedef const value_type * pointer;
		typedef const value_type & reference;
	};
}

#endif // header guard
