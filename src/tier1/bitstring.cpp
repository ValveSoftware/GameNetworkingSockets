//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose:		Arbitrary length bit string
//				** NOTE: This class does NOT override the bitwise operators
//						 as doing so would require overriding the operators
//						 to allocate memory for the returned bitstring.  This method
//						 would be prone to memory leaks as the calling party
//						 would have to remember to delete the memory.  Functions
//						 are used instead to require the calling party to allocate
//						 and destroy their own memory
//
// $Workfile:     $
// $Date:         $
// $NoKeywords: $
//=============================================================================//


#include <limits.h>

#include <tier1/utlbuffer.h>
#include <tier0/dbg.h>
#include <tier1/bitstring.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// Init static vars
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Purpose: Calculate a mask for the last int in the array
// Input  : numBits - 
// Output : static int
//-----------------------------------------------------------------------------

unsigned g_BitStringEndMasks[] = 
{
	0x00000000,
	0xfffffffe,
	0xfffffffc,
	0xfffffff8,
	0xfffffff0,
	0xffffffe0,
	0xffffffc0,
	0xffffff80,
	0xffffff00,
	0xfffffe00,
	0xfffffc00,
	0xfffff800,
	0xfffff000,
	0xffffe000,
	0xffffc000,
	0xffff8000,
	0xffff0000,
	0xfffe0000,
	0xfffc0000,
	0xfff80000,
	0xfff00000,
	0xffe00000,
	0xffc00000,
	0xff800000,
	0xff000000,
	0xfe000000,
	0xfc000000,
	0xf8000000,
	0xf0000000,
	0xe0000000,
	0xc0000000,
	0x80000000,
};

//-----------------------------------------------------------------------------
// Purpose: Saves a bit string to the given file
// Input  :
// Output :
//-----------------------------------------------------------------------------
void SaveBitString(const int *pInts, int nInts, CUtlBuffer& buf)
{
	buf.EnsureCapacity( buf.TellPut() + (sizeof( int )*nInts) );
	for (int i=0;i<nInts;i++) 
	{
		buf.PutInt( pInts[i] );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Loads a bit string from the given file
// Input  :
// Output :
//-----------------------------------------------------------------------------

void LoadBitString(int *pInts, int nInts, CUtlBuffer& buf)
{
	for (int i=0; i<nInts; i++) 
	{
		pInts[i] = buf.GetInt(); 
	}
}


//-----------------------------------------------------------------------------

void CVariableBitStringBase::ValidateOperand( const CVariableBitStringBase &operand ) const
{
	Assert(Size() == operand.Size());
}

//-----------------------------------------------------------------------------
// Purpose: Resizes the bit string to a new number of bits
// Input  : resizeNumBits - 
//-----------------------------------------------------------------------------
void CVariableBitStringBase::Resize( int resizeNumBits )
{
	Assert( resizeNumBits >= 0 );

	int newIntCount = CalcNumIntsForBits( resizeNumBits );
	if ( newIntCount != GetNumInts() )
	{
		if ( GetInts() )
		{
			int oldIntCount = m_numInts;
			ReallocInts( newIntCount );
			m_numInts = newIntCount;

			if ( resizeNumBits >= Size() )
			{
				GetInts()[GetNumInts() - 1] &= ~GetEndMask();
				memset( GetInts() + oldIntCount, 0, (newIntCount - oldIntCount) * sizeof(int) );
			}
		}
		else
		{
			// Figure out how many ints are needed
			AllocInts( newIntCount );
			m_numInts = newIntCount;

			// Initialize bitstring by clearing all bits
			memset( GetInts(), 0, newIntCount * sizeof(int) );
		}
	} 
	else if ( resizeNumBits >= Size() && GetInts() )
		GetInts()[GetNumInts() - 1] &= ~GetEndMask();

	// store the new size and end mask
	m_numBits = resizeNumBits;
}

//-----------------------------------------------------------------------------
// Purpose: Allocate the storage for the ints
// Input  : numInts - 
//-----------------------------------------------------------------------------
void CVariableBitStringBase::AllocInts( int numInts )
{
	Assert( !m_pInt );

	if ( numInts == 0 )
		return;

	if ( numInts == 1 )
	{
		m_pInt = NULL;	// we will use m_iBitStringStorage
		return;
	}

	m_pInt = (int *)malloc( numInts * sizeof(int) );
}


//-----------------------------------------------------------------------------
// Purpose: Reallocate the storage for the ints
// Input  : numInts - 
//-----------------------------------------------------------------------------
void CVariableBitStringBase::ReallocInts( int numInts )
{
	Assert( GetInts() );
	if ( numInts == 0)
	{
		FreeInts();
		return;
	}

	if ( m_numInts == 1 )	// we were using m_iBitStringStorage
	{
		if ( numInts != 1 )
		{
			m_pInt = ((int *)malloc( numInts * sizeof(int) ));
			*m_pInt = m_iBitStringStorage;
		}

		return;
	}

	if ( numInts == 1 )
	{
		m_iBitStringStorage = *m_pInt;
		free( m_pInt );
		m_pInt = NULL;
		return;
	}

	m_pInt = (int *)realloc( m_pInt,  numInts * sizeof(int) );
}


//-----------------------------------------------------------------------------
// Purpose: Free storage allocated with AllocInts
//-----------------------------------------------------------------------------
void CVariableBitStringBase::FreeInts( void )
{
	if ( m_numInts > 1 )
	{
		free( m_pInt );
	}
	m_pInt = NULL;
}


#ifdef DBGFLAG_VALIDATE
//-----------------------------------------------------------------------------
// Purpose: Ensure that all of our internal structures are consistent, and
//			account for all memory that we've allocated.
// Input:	validator -		Our global validator object
//			pchName -		Our name (typically a member var in our container)
//-----------------------------------------------------------------------------
void CVariableBitStringBase::Validate( CValidator &validator, const char *pchName )
{
	validator.Push( "CVariableBitStringBase", this, pchName );
    
	if ( m_numInts > 1 )
	{
		validator.ClaimMemory( m_pInt );
	}

	validator.Pop();
}
#endif // DBGFLAG_VALIDATE
