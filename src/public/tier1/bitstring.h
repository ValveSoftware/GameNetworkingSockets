//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose:		Arbitrary length bit string
//				** NOTE: This class does NOT override the bitwise operators
//						 as doing so would require overriding the operators
//						 to allocate memory for the returned bitstring.  This method
//						 would be prone to memory leaks as the calling party
//						 would have to remember to delete the memory.  Funtions
//						 are used instead to require the calling party to allocate
//						 and destroy their own memory
//
// $Workfile:     $
// $Date:         $
//
//-----------------------------------------------------------------------------
// $Log: $
//
// $NoKeywords: $
//=============================================================================//

#ifndef BITSTRING_H
#define BITSTRING_H
#pragma once

class CUtlBuffer;

//-----------------------------------------------------------------------------

// OPTIMIZE: Removed the platform independence for speed
#define LOG2_BITS_PER_INT	5
#define BITS_PER_INT		32

//-------------------------------------

extern unsigned g_BitStringEndMasks[];
inline unsigned GetEndMask( int numBits ) { return g_BitStringEndMasks[numBits % BITS_PER_INT]; }

inline int CalcNumIntsForBits( int numBits )	{ return (numBits + (BITS_PER_INT-1)) / BITS_PER_INT; }

void DebugPrintBitStringBits( const int *pInts, int nInts );
void SaveBitString(const int *pInts, int nInts, CUtlBuffer& buf);
void LoadBitString(int *pInts, int nInts, CUtlBuffer& buf);

#define BitString_Bit( bitNum ) ( 1 << ( (bitNum) & (BITS_PER_INT-1) ) )
#define BitString_Int( bitNum ) ( (bitNum) >> LOG2_BITS_PER_INT )


//-----------------------------------------------------------------------------
// template CBitStringT
//
// Defines the operations relevant to any bit array. Simply requires a base
// class that implements Size(), GetInts(), GetNumInts() & ValidateOperand()
//
// CBitString and CFixedBitString<int> are the actual classes generally used
// by clients
//

template <class BASE_OPS>
class CBitStringT : public BASE_OPS
{
public:
	CBitStringT();
	CBitStringT(int numBits);			// Must be initialized with the number of bits

	// Do NOT override bitwise operators (see note in header)
	void	And(const CBitStringT &andStr, CBitStringT *out) const;
	void	Or(const CBitStringT &orStr, CBitStringT *out) const;
	void	Xor(const CBitStringT &orStr, CBitStringT *out) const;
	
	void	Not(CBitStringT *out) const;
	
	void	Copy(CBitStringT *out) const;

	bool	IsAllClear(void) const;		// Are all bits zero?
	bool	IsAllSet(void) const;		// Are all bits one?

	bool 	GetBit( int bitNum ) const;
	void 	SetBit( int bitNum );
	void 	ClearBit(int bitNum);

	void	SetAllBits(void);			// Sets all bits
	void	ClearAllBits(void);			// Clears all bits
	
	void	DebugPrintBits(void) const;		// For debugging
	
	void	SaveBitString(CUtlBuffer& buf) const;
	void	LoadBitString(CUtlBuffer& buf);

};

//-----------------------------------------------------------------------------
// class CVariableBitStringBase
//
// Defines the operations necessary for a variable sized bit array

class CVariableBitStringBase
{
public:
	bool	IsFixedSize() const			{ return false; }
	int		Size(void) const			{ return m_numBits; }
	void	Resize( int numBits );		// resizes bit array
	
	int 		GetNumInts() const		{ return m_numInts; }
	int *		GetInts()				{ return m_numInts == 1 ? &m_iBitStringStorage : m_pInt;	}
	const int *	GetInts() const			{ return m_numInts == 1 ? &m_iBitStringStorage : m_pInt;	}

	void	Validate( class CValidator &validator, const char *pchName );

protected:
	CVariableBitStringBase();
	CVariableBitStringBase(int numBits);
	CVariableBitStringBase( const CVariableBitStringBase &from );
	CVariableBitStringBase &operator=( const CVariableBitStringBase &from );
	~CVariableBitStringBase(void);
	
	void 		ValidateOperand( const CVariableBitStringBase &operand ) const;

	unsigned	GetEndMask() const		{ return ::GetEndMask( Size() ); }

private:

	int		m_numBits;					// Number of bits in the bitstring
	int		m_numInts;					// Number of ints to needed to store bitstring
	int		m_iBitStringStorage;		// If the bit string fits in one int, it goes here
	int		*m_pInt;					// Array of ints containing the bitstring

	void	AllocInts( int numInts );	// Free the allocated bits
	void	ReallocInts( int numInts );
	void	FreeInts( void );			// Free the allocated bits
};

//-----------------------------------------------------------------------------
// class CFixedBitStringBase
//
// Defines the operations necessary for a fixed sized bit array. 
// 

template <int bits> struct BitCountToEndMask_t { };
template <> struct BitCountToEndMask_t< 0> { enum { MASK = 0x00000000 }; };
template <> struct BitCountToEndMask_t< 1> { enum { MASK = 0xfffffffe }; };
template <> struct BitCountToEndMask_t< 2> { enum { MASK = 0xfffffffc }; };
template <> struct BitCountToEndMask_t< 3> { enum { MASK = 0xfffffff8 }; };
template <> struct BitCountToEndMask_t< 4> { enum { MASK = 0xfffffff0 }; };
template <> struct BitCountToEndMask_t< 5> { enum { MASK = 0xffffffe0 }; };
template <> struct BitCountToEndMask_t< 6> { enum { MASK = 0xffffffc0 }; };
template <> struct BitCountToEndMask_t< 7> { enum { MASK = 0xffffff80 }; };
template <> struct BitCountToEndMask_t< 8> { enum { MASK = 0xffffff00 }; };
template <> struct BitCountToEndMask_t< 9> { enum { MASK = 0xfffffe00 }; };
template <> struct BitCountToEndMask_t<10> { enum { MASK = 0xfffffc00 }; };
template <> struct BitCountToEndMask_t<11> { enum { MASK = 0xfffff800 }; };
template <> struct BitCountToEndMask_t<12> { enum { MASK = 0xfffff000 }; };
template <> struct BitCountToEndMask_t<13> { enum { MASK = 0xffffe000 }; };
template <> struct BitCountToEndMask_t<14> { enum { MASK = 0xffffc000 }; };
template <> struct BitCountToEndMask_t<15> { enum { MASK = 0xffff8000 }; };
template <> struct BitCountToEndMask_t<16> { enum { MASK = 0xffff0000 }; };
template <> struct BitCountToEndMask_t<17> { enum { MASK = 0xfffe0000 }; };
template <> struct BitCountToEndMask_t<18> { enum { MASK = 0xfffc0000 }; };
template <> struct BitCountToEndMask_t<19> { enum { MASK = 0xfff80000 }; };
template <> struct BitCountToEndMask_t<20> { enum { MASK = 0xfff00000 }; };
template <> struct BitCountToEndMask_t<21> { enum { MASK = 0xffe00000 }; };
template <> struct BitCountToEndMask_t<22> { enum { MASK = 0xffc00000 }; };
template <> struct BitCountToEndMask_t<23> { enum { MASK = 0xff800000 }; };
template <> struct BitCountToEndMask_t<24> { enum { MASK = 0xff000000 }; };
template <> struct BitCountToEndMask_t<25> { enum { MASK = 0xfe000000 }; };
template <> struct BitCountToEndMask_t<26> { enum { MASK = 0xfc000000 }; };
template <> struct BitCountToEndMask_t<27> { enum { MASK = 0xf8000000 }; };
template <> struct BitCountToEndMask_t<28> { enum { MASK = 0xf0000000 }; };
template <> struct BitCountToEndMask_t<29> { enum { MASK = 0xe0000000 }; };
template <> struct BitCountToEndMask_t<30> { enum { MASK = 0xc0000000 }; };
template <> struct BitCountToEndMask_t<31> { enum { MASK = 0x80000000 }; };

//-------------------------------------

template <int NUM_BITS>
class CFixedBitStringBase
{
public:
	bool	IsFixedSize() const			{ return true; }
	int		Size(void) const			{ return NUM_BITS; }
	void	Resize( int numBits )		{ Assert(numBits == NUM_BITS); }// for syntatic consistency (for when using templates)
	
	int 		GetNumInts() const		{ return NUM_INTS; }
	int *		GetInts()				{ return m_Ints;	}
	const int *	GetInts() const			{ return m_Ints;	}

protected:
	CFixedBitStringBase()				{}
	CFixedBitStringBase(int numBits)	{ Assert( numBits == NUM_BITS ); } // doesn't make sense, really. Supported to simplify templates & allow easy replacement of variable 
	
	void 		ValidateOperand( const CFixedBitStringBase<NUM_BITS> &operand ) const	{ } // no need, compiler does so statically

public: // for test code
	unsigned	GetEndMask() const		{ return static_cast<unsigned>( BitCountToEndMask_t<NUM_BITS % BITS_PER_INT>::MASK ); }

private:
	enum
	{
		NUM_INTS = (NUM_BITS + (BITS_PER_INT-1)) / BITS_PER_INT
	};

	int m_Ints[(NUM_BITS + (BITS_PER_INT-1)) / BITS_PER_INT];
};

//-----------------------------------------------------------------------------
//
// The actual classes used
//

// inheritance instead of typedef to allow forward declarations
class CBitString : public CBitStringT<CVariableBitStringBase>
{
public:
	CBitString()
	{
	}
	
	CBitString(int numBits)
	 : CBitStringT<CVariableBitStringBase>(numBits)
	{
	}
};

//-----------------------------------------------------------------------------

template < int NUM_BITS >
class CFixedBitString : public CBitStringT< CFixedBitStringBase<NUM_BITS> >
{
public:
	CFixedBitString()
	{
	}
	
	CFixedBitString(int numBits)
	 : CBitStringT< CFixedBitStringBase<NUM_BITS> >(numBits)
	{
	}
};

//-----------------------------------------------------------------------------

inline CVariableBitStringBase::CVariableBitStringBase()
{
	memset( this, 0, sizeof( *this ) );
}

//-----------------------------------------------------------------------------

inline CVariableBitStringBase::CVariableBitStringBase(int numBits)
{
	Assert( numBits );
	m_numBits	= numBits;

	// Figure out how many ints are needed
	m_numInts = CalcNumIntsForBits( numBits );
	m_pInt = NULL;
	AllocInts( m_numInts );
}

//-----------------------------------------------------------------------------

inline CVariableBitStringBase::CVariableBitStringBase( const CVariableBitStringBase &from )
{
	if ( from.m_numInts )
	{
		m_numBits = from.m_numBits;
		m_numInts = from.m_numInts;
		m_pInt = NULL;
		AllocInts( m_numInts );
		memcpy( GetInts(), from.GetInts(), m_numInts * sizeof(int) );
	}
	else
		memset( this, 0, sizeof( *this ) );
}

//-----------------------------------------------------------------------------

inline CVariableBitStringBase &CVariableBitStringBase::operator=( const CVariableBitStringBase &from )
{
	Resize( from.Size() );
	if ( GetInts() )
		memcpy( GetInts(), from.GetInts(), m_numInts * sizeof(int) );
	return (*this);
}


//-----------------------------------------------------------------------------
// Purpose: Destructor
// Input  :
// Output :
//-----------------------------------------------------------------------------

inline CVariableBitStringBase::~CVariableBitStringBase(void)
{
	FreeInts();
}

//-----------------------------------------------------------------------------

template <class BASE_OPS>
inline CBitStringT<BASE_OPS>::CBitStringT()
{
	// undef this is ints are not 4 bytes
	// generate a compile error if sizeof(int) is not 4 (HACK: can't use the preprocessor so use the compiler)
	
	COMPILE_TIME_ASSERT( sizeof(int)==4 );
	
	// Initialize bitstring by clearing all bits
	ClearAllBits();
}

//-----------------------------------------------------------------------------
template <class BASE_OPS>
inline CBitStringT<BASE_OPS>::CBitStringT(int numBits)
 : BASE_OPS( numBits )
{
	// undef this is ints are not 4 bytes
	// generate a compile error if sizeof(int) is not 4 (HACK: can't use the preprocessor so use the compiler)
	
	COMPILE_TIME_ASSERT( sizeof(int)==4 );
	
	// Initialize bitstring by clearing all bits
	ClearAllBits();
}

//-----------------------------------------------------------------------------

template <class BASE_OPS>
inline bool CBitStringT<BASE_OPS>::GetBit( int bitNum ) const
{
	Assert( bitNum >= 0 && bitNum < this->Size() );
	const int *pInt = this->GetInts() + BitString_Int( bitNum );
	return ( ( *pInt & BitString_Bit( bitNum ) ) != 0 );
}

//-----------------------------------------------------------------------------

template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::SetBit( int bitNum )			
{
	Assert( bitNum >= 0 && bitNum < this->Size() );
	int *pInt = this->GetInts() + BitString_Int( bitNum );
	*pInt |= BitString_Bit( bitNum );
}

//-----------------------------------------------------------------------------

template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::ClearBit(int bitNum)		
{
	Assert( bitNum >= 0 && bitNum < this->Size() );
	int *pInt = this->GetInts() + BitString_Int( bitNum );
	*pInt &= ~BitString_Bit( bitNum );
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  :
// Output :
//-----------------------------------------------------------------------------
template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::And(const CBitStringT &addStr, CBitStringT *out) const
{
	this->ValidateOperand( addStr );
	this->ValidateOperand( *out );
	
	int *	   pDest		= out->GetInts();
	const int *pOperand1	= this->GetInts();
	const int *pOperand2	= addStr.GetInts();

	for (int i = this->GetNumInts() - 1; i >= 0 ; --i) 
	{
		pDest[i] = pOperand1[i] & pOperand2[i];
	}
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  :
// Output :
//-----------------------------------------------------------------------------
template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::Or(const CBitStringT &orStr, CBitStringT *out) const
{
	this->ValidateOperand( orStr );
	this->ValidateOperand( *out );

	int *	   pDest		= out->GetInts();
	const int *pOperand1	= this->GetInts();
	const int *pOperand2	= orStr.GetInts();

	for (int i = this->GetNumInts() - 1; i >= 0; --i) 
	{
		pDest[i] = pOperand1[i] | pOperand2[i];
	}
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  :
// Output :
//-----------------------------------------------------------------------------
template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::Xor(const CBitStringT &xorStr, CBitStringT *out) const
{
	int *	   pDest		= out->GetInts();
	const int *pOperand1	= this->GetInts();
	const int *pOperand2	= xorStr.GetInts();

	for (int i = this->GetNumInts() - 1; i >= 0; --i) 
	{
		pDest[i] = pOperand1[i] ^ pOperand2[i];
	}
}

//-----------------------------------------------------------------------------
// Purpose:
// Input  :
// Output :
//-----------------------------------------------------------------------------
template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::Not(CBitStringT *out) const
{
	this->ValidateOperand( *out );

	int *	   pDest	= out->GetInts();
	const int *pOperand	= this->GetInts();

	for (int i = this->GetNumInts() - 1; i >= 0; --i) 
	{
		pDest[i] = ~(pOperand[i]);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Copy a bit string
// Input  :
// Output :
//-----------------------------------------------------------------------------
template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::Copy(CBitStringT *out) const
{
	this->ValidateOperand( *out );
	Assert( out != this );
	
	memcpy( out->GetInts(), this->GetInts(), this->GetNumInts() * sizeof( int ) );
}

//-----------------------------------------------------------------------------
// Purpose: Are all bits zero?
// Input  :
// Output :
//-----------------------------------------------------------------------------
template <class BASE_OPS>
inline bool CBitStringT<BASE_OPS>::IsAllClear(void) const
{
	// Number of available bits may be more than the number
	// actually used, so make sure to mask out unused bits
	// before testing for zero
	(const_cast<CBitStringT *>(this))->GetInts()[this->GetNumInts()-1] &= ~CBitStringT<BASE_OPS>::GetEndMask(); // external semantics of const retained

	for (int i = this->GetNumInts() - 1; i >= 0; --i) 
	{
		if ( this->GetInts()[i] !=0 ) 
		{
			return false;
		}
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Are all bits set?
// Input  :
// Output :
//-----------------------------------------------------------------------------
template <class BASE_OPS>
inline bool CBitStringT<BASE_OPS>::IsAllSet(void) const
{
	// Number of available bits may be more than the number
	// actually used, so make sure to mask out unused bits
	// before testing for set bits
	(const_cast<CBitStringT *>(this))->GetInts()[this->GetNumInts()-1] |= CBitStringT<BASE_OPS>::GetEndMask();  // external semantics of const retained

	for (int i = this->GetNumInts() - 1; i >= 0; --i) 
	{
		if ( this->GetInts()[i] != ~0 ) 
		{
			return false;
		}
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Sets all bits
// Input  :
// Output :
//-----------------------------------------------------------------------------
template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::SetAllBits(void)		
{
	if ( this->GetInts() )
		memset( this->GetInts(), 0xff, this->GetNumInts() * sizeof(int) );
}

//-----------------------------------------------------------------------------
// Purpose: Clears all bits
// Input  :
// Output :
//-----------------------------------------------------------------------------
template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::ClearAllBits(void)		
{
	if ( this->GetInts() )
		memset( this->GetInts(), 0, this->GetNumInts() * sizeof(int) );
}

//-----------------------------------------------------------------------------

template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::DebugPrintBits(void) const
{
	(const_cast<CBitStringT *>(this))->GetInts()[this->GetNumInts()-1] &= ~CBitStringT<BASE_OPS>::GetEndMask(); // external semantics of const retained
	DebugPrintBitStringBits( this->GetInts(), this->GetNumInts() );
}

//-----------------------------------------------------------------------------

template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::SaveBitString(CUtlBuffer& buf) const
{
	(const_cast<CBitStringT *>(this))->GetInts()[this->GetNumInts()-1] &= ~CBitStringT<BASE_OPS>::GetEndMask(); // external semantics of const retained
	::SaveBitString( this->GetInts(), this->GetNumInts(), buf );
}

//-----------------------------------------------------------------------------

template <class BASE_OPS>
inline void CBitStringT<BASE_OPS>::LoadBitString(CUtlBuffer& buf) 
{
	(const_cast<CBitStringT *>(this))->GetInts()[this->GetNumInts()-1] &= ~CBitStringT<BASE_OPS>::GetEndMask(); 
	::LoadBitString( this->GetInts(), this->GetNumInts(), buf );
}

//-----------------------------------------------------------------------------
// @Note (toml 11-09-02): these methods are a nod to a heavy user of the
// bit string, AI conditions. This assumes MAX_CONDITIONS == 128

template<> 
inline void CBitStringT< CFixedBitStringBase<128> >::And(const CBitStringT &addStr, CBitStringT *out) const
{
	int *	   pDest		= out->GetInts();
	const int *pOperand1	= this->GetInts();
	const int *pOperand2	= addStr.GetInts();

	pDest[0] = pOperand1[0] & pOperand2[0];
	pDest[1] = pOperand1[1] & pOperand2[1];
	pDest[2] = pOperand1[2] & pOperand2[2];
	pDest[3] = pOperand1[3] & pOperand2[3];
}

template<> 
inline bool CBitStringT< CFixedBitStringBase<128> >::IsAllClear(void) const
{
	const int *pInts = this->GetInts();

	return ( pInts[0] == 0 && pInts[1] == 0 && pInts[2] == 0 && pInts[3] == 0 );
}

template<> 
inline void CBitStringT< CFixedBitStringBase<128> >::Copy(CBitStringT *out) const
{
	int *	   pDest = out->GetInts();
	const int *pInts = GetInts();

	pDest[0] = pInts[0];
	pDest[1] = pInts[1];
	pDest[2] = pInts[2];
	pDest[3] = pInts[3];
}

//=============================================================================

#endif // BITSTRING_H
