//========= Copyright 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: Larger string functions go here.
//
// $Header: $
// $NoKeywords: $
//=============================================================================//

#ifndef _XBOX
#pragma warning (disable:4514)
#endif

#include "tier1/utlstring.h"
#include "tier0/t0constants.h"
#include "tier1/utlvector.h"

#include "tier0/memdbgon.h"


//-----------------------------------------------------------------------------
// Purpose: Helper: Find s substring
//-----------------------------------------------------------------------------
static ptrdiff_t IndexOf( const char *pstrToSearch, const char *pstrTarget )
{
	const char *pstrHit = V_strstr( pstrToSearch, pstrTarget );
	if ( pstrHit == NULL )
	{
		return -1;	// Not found.
	}
	return ( pstrHit - pstrToSearch );
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string ends with the string passed in
//-----------------------------------------------------------------------------
static bool BEndsWith( const char *pstrToSearch, const char *pstrToFind, bool bCaseless )
{
	if ( !pstrToSearch )
		return false;

	if ( !pstrToFind )
		return true;

	size_t nThisLength = V_strlen( pstrToSearch );
	size_t nThatLength = V_strlen( pstrToFind );

	if ( nThatLength == 0 )
		return true;

	if ( nThatLength > nThisLength )
		return false;

	size_t nIndex = nThisLength - nThatLength;

	if ( bCaseless )
		return V_stricmp( pstrToSearch + nIndex, pstrToFind ) == 0;
	else
		return V_strcmp( pstrToSearch + nIndex, pstrToFind ) == 0;
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string starts with the string passed in
//-----------------------------------------------------------------------------
static bool BStartsWith( const char *pstrToSearch, const char *pstrToFind, bool bCaseless )
{
	if ( !pstrToSearch )
		return false;

	if ( !pstrToFind )
		return true;

	if ( bCaseless )
	{
		int nThatLength = V_strlen( pstrToFind );

		if ( nThatLength == 0 )
			return true;

		return V_strnicmp( pstrToSearch, pstrToFind, nThatLength ) == 0;
	}
	else
		return V_strstr( pstrToSearch, pstrToFind ) == pstrToSearch;
}


//-----------------------------------------------------------------------------
// Purpose: Helper: kill all whitespace.
//-----------------------------------------------------------------------------
static size_t RemoveWhitespace( char *pszString )
{
	if ( pszString == NULL )
		return 0;

	char *pstrDest = pszString;
	size_t cRemoved = 0;
	for ( char *pstrWalker = pszString; *pstrWalker != 0; pstrWalker++ )
	{
		if ( !isspace( (unsigned char)*pstrWalker ) ) 
		{
			*pstrDest = *pstrWalker;
			pstrDest++;
		}
		else
			cRemoved += 1;
	}
	*pstrDest = 0;

	return cRemoved;
}


#ifdef VALVE_RVALUE_REFS
//-----------------------------------------------------------------------------
// Purpose: move-constructor from CUtlStringBuilder to CUtlString
//-----------------------------------------------------------------------------
CUtlString::CUtlString( class CUtlStringBuilder &&string )
{
	m_pchString = string.DetachRawPtr();
}

//-----------------------------------------------------------------------------
// Purpose: move-assignment from CUtlStringBuilder to CUtlString
//-----------------------------------------------------------------------------
CUtlString & CUtlString::operator=(class CUtlStringBuilder &&string)
{
	SetPtr( string.DetachRawPtr() );
	return *this;
}
#endif


//-----------------------------------------------------------------------------
// Purpose: Helper for Format() method
//-----------------------------------------------------------------------------
size_t CUtlString::FormatV( const char *pFormat, va_list args )
{
	size_t len = 0;
#ifdef _WIN32
	// how much space will we need?
	size_t mlen = _vscprintf( pFormat, args );

	// get it
	FreePv( m_pchString );
	m_pchString = NULL;
	m_pchString = (char*) PvAlloc( mlen+1 );

	// format into that space, which is certainly enough
	len = _vsnprintf( m_pchString, mlen+1, pFormat, args );

	Assert( len >= 0 );
	Assert( len <= mlen );

#elif defined ( _PS3 )

	// ignore the PS3 documentation about vsnprintf returning -1 when the string is too small. vsprintf seems to do the right thing (least at time of
	// implementation) and returns the number of characters needed when you pass in a buffer that is too small

	FreePv( m_pchString );
	m_pchString = NULL;	

	len = vsnprintf( NULL, 0, pFormat, args );
	if ( len > 0 )
	{
		m_pchString = (char*) PvAlloc( len + 1 );
		len = vsnprintf( m_pchString, len + 1, pFormat, args );
	}

#elif defined( POSIX )

	char *buf = NULL;
	len = vasprintf( &buf, pFormat, args );

	// Len < 0 represents an overflow
	if( buf )
	{
        // We need to get the string into PvFree-compatible memory, which
        // we can't assume is directly interoperable with the malloc memory
        // that vasprintf returned (definitely not compatible with a debug
        // allocator, for example).
        Set( buf );
        real_free( buf );
	}

#else
#error "define vsnprintf type."
#endif
	return len;
}


//-----------------------------------------------------------------------------
// Purpose: implementation helper for AppendFormat()
//-----------------------------------------------------------------------------
size_t CUtlString::VAppendFormat( const char *pFormat, va_list args )
{
	size_t len = 0;
	char *pstrFormatted = NULL;
#ifdef _WIN32
	// how much space will we need?
	size_t mlen = _vscprintf( pFormat, args );

	// get it
	pstrFormatted = (char*) PvAlloc( mlen+1 );

	// format into that space, which is certainly enough
	len = _vsnprintf( pstrFormatted, mlen+1, pFormat, args );

	Assert( len > 0 );
	Assert( len <= mlen );

#elif defined ( _PS3 )

	// ignore the PS3 documentation about vsnprintf returning -1 when the string is too small. vsprintf seems to do the right thing (least at time of
	// implementation) and returns the number of characters needed when you pass in a buffer that is too small

	len = vsnprintf( NULL, 0, pFormat, args );
	if ( len > 0 )
	{
		pstrFormatted = (char*) PvAlloc( len + 1 );
		len = vsnprintf( pstrFormatted, len + 1, pFormat, args );
	}

#elif defined( POSIX )

	len = vasprintf( &pstrFormatted, pFormat, args );

#else
#error "define vsnprintf type."
#endif

	// if we ended with a formatted string, append and free it
	if ( pstrFormatted != NULL )
	{
		Append( pstrFormatted, len );
#if defined( POSIX )
		real_free( pstrFormatted );
#else
        FreePv( pstrFormatted );
#endif
	}

	return len;
}


//-----------------------------------------------------------------------------
// Purpose: replace all occurrences of one string with another
//			replacement string may be NULL or "" to remove target string
//-----------------------------------------------------------------------------
size_t CUtlString::Replace( const char *pstrTarget, const char *pstrReplacement )
{
	return ReplaceInternal( pstrTarget, pstrReplacement, V_strstr );
}


//-----------------------------------------------------------------------------
// Purpose: replace all occurrences of one string with another
//			replacement string may be NULL or "" to remove target string
//-----------------------------------------------------------------------------
size_t CUtlString::ReplaceCaseless( const char *pstrTarget, const char *pstrReplacement )
{
	return ReplaceInternal( pstrTarget, pstrReplacement, V_stristr );
}

//-----------------------------------------------------------------------------
// Purpose: replace all occurrences of one string with another
//			replacement string may be NULL or "" to remove target string
//-----------------------------------------------------------------------------
size_t CUtlString::ReplaceInternal( const char *pstrTarget, const char *pstrReplacement, const char *pfnCompare(const char*, const char*) )
{
	size_t cReplacements = 0;
	if ( pstrReplacement == NULL )
		pstrReplacement = "";

	size_t nTargetLength = V_strlen( pstrTarget );
	size_t nReplacementLength = V_strlen( pstrReplacement );

	if ( m_pchString != NULL && pstrTarget != NULL  )
	{
		// walk the string counting hits
		const char *pstrHit = m_pchString;
		for ( pstrHit = pfnCompare( pstrHit, pstrTarget ); pstrHit != NULL && *pstrHit != 0; /* inside */ )
		{
			cReplacements++;
			// look for the next target and keep looping
			pstrHit = pfnCompare( pstrHit + nTargetLength, pstrTarget );
		}

		// if we didn't miss, get to work
		if ( cReplacements > 0 )
		{
			// reallocate only once; how big will we need?
			size_t nNewLength = 1 + V_strlen( m_pchString ) + cReplacements * ( nReplacementLength - nTargetLength );

			char *pstrNew = (char*) PvAlloc( nNewLength );
			if ( nNewLength == 1 )
			{
				// shortcut simple case, even if rare
				*pstrNew = 0;
			}
			else
			{
				const char *pstrPreviousHit = NULL;
				char *pstrDestination = pstrNew;
				pstrHit = m_pchString;
				size_t cActualReplacements = 0;
				for ( pstrHit = pfnCompare( m_pchString, pstrTarget ); pstrHit != NULL && *pstrHit != 0; /* inside */ )
				{
					cActualReplacements++;

					// copy from the previous hit to the match
					if ( pstrPreviousHit == NULL )
						pstrPreviousHit = m_pchString;
					memcpy( pstrDestination, pstrPreviousHit, pstrHit - pstrPreviousHit );
					pstrDestination += ( pstrHit - pstrPreviousHit );

					// push the replacement string in
					memcpy( pstrDestination, pstrReplacement, nReplacementLength );
					pstrDestination += nReplacementLength;

					pstrPreviousHit = pstrHit + nTargetLength;
					pstrHit = pfnCompare( pstrPreviousHit, pstrTarget );
				}

				while ( pstrPreviousHit != NULL && *pstrPreviousHit != 0 )
				{
					*pstrDestination = *pstrPreviousHit;
					pstrDestination++;
					pstrPreviousHit++;
				}
				*pstrDestination = 0;

				Assert( pstrNew + nNewLength == pstrDestination + 1);
				Assert( cActualReplacements == cReplacements );
			}

			// release the old string, set the new one
			FreePv( m_pchString );
			m_pchString = pstrNew;

		}
	}

	return cReplacements;
}

//-----------------------------------------------------------------------------
// Purpose: Indicates if the target string exists in this instance.
//			The index is negative if the target string is not found, otherwise it is the index in the string.
//-----------------------------------------------------------------------------
ptrdiff_t CUtlString::IndexOf( const char *pstrTarget ) const
{
	return ::IndexOf( String(), pstrTarget );
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string ends with the string passed in
//-----------------------------------------------------------------------------
bool CUtlString::BEndsWith( const char *pchString ) const
{
	return ::BEndsWith( String(), pchString, false );
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string ends with the string passed in (caseless)
//-----------------------------------------------------------------------------
bool CUtlString::BEndsWithCaseless( const char *pchString ) const
{
	return ::BEndsWith( String(), pchString, true );
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string starts with the string passed in
//-----------------------------------------------------------------------------
bool CUtlString::BStartsWith( const char *pchString ) const
{
	return ::BStartsWith( String(), pchString, false );
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string ends with the string passed in (caseless)
//-----------------------------------------------------------------------------
bool CUtlString::BStartsWithCaseless( const char *pchString ) const
{
	return ::BStartsWith( String(), pchString, true );
}


//-----------------------------------------------------------------------------
// Purpose: 
//			remove whitespace -- anything that is isspace() -- from the string
//-----------------------------------------------------------------------------
size_t CUtlString::RemoveWhitespace()
{
	return ::RemoveWhitespace( m_pchString );
}


//-----------------------------------------------------------------------------
// Purpose: 
//			trim whitespace from front and back of string
//-----------------------------------------------------------------------------
size_t CUtlString::TrimWhitespace()
{
	if ( m_pchString == NULL )
		return 0;

	int cChars = V_StrTrim( m_pchString );
	return cChars;
}


//-----------------------------------------------------------------------------
// Purpose: 
//			trim whitespace from back of string
//-----------------------------------------------------------------------------
size_t CUtlString::TrimTrailingWhitespace()
{
	if ( m_pchString == NULL )
		return 0;

	uint32 cChars = Length();
	if ( cChars == 0 )
		return 0;

	char *pCur = &m_pchString[cChars - 1];
	while ( pCur >= m_pchString && isspace( *pCur ) )
	{
		*pCur = '\0';
		pCur--;
	}

	return pCur - m_pchString + 1;
}


//-----------------------------------------------------------------------------
// Purpose: out-of-line assertion to keep code generation size down
//-----------------------------------------------------------------------------
void CUtlString::AssertStringTooLong()
{
	AssertMsg( false, "Assertion failed: length > k_cchMaxString" );
}


//-----------------------------------------------------------------------------
// Purpose: format binary data as hex characters, appending to existing data
//-----------------------------------------------------------------------------
void CUtlString::AppendHex( const uint8 *pbInput, size_t cubInput, bool bLowercase /*= true*/ )
{
	if ( !cubInput )
		return;

	size_t existingLen = Length();
	if ( existingLen >= k_cchMaxString || cubInput*2 >= k_cchMaxString - existingLen )
	{
		Assert( existingLen < k_cchMaxString && cubInput * 2 < k_cchMaxString - existingLen );
		return;
	}

	const char *pchHexLookup = bLowercase ? "0123456789abcdef" : "0123456789ABCDEF";
	CUtlString newValue( existingLen + cubInput * 2 + 1 );
	V_memcpy( newValue.Access(), Access(), existingLen );
	char *pOut = newValue.Access() + existingLen;
	for ( ; cubInput; --cubInput, ++pbInput )
	{
		uint8 val = *pbInput;
		*pOut++ = pchHexLookup[val >> 4];
		*pOut++ = pchHexLookup[val & 15];
	}
	*pOut = '\0';
	Swap( newValue );
}


// Catch invalid UTF-8 sequences and return false if found, or true if the sequence is correct
static bool BVerifyValidUTF8Continuation( size_t unStart, size_t unContinuationLength, const uint8 *pbCharacters )
{
	for ( size_t i = 0; i < unContinuationLength; ++ i )
	{
		// Make sure byte is of the form 10xxxxxx
		// Note: this also catches an unexpected NULL terminator and prevents us from overrunning the string
		if ( ( pbCharacters[i + unStart] & 0xC0 ) != 0x80 )
			return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Caps the string to the specified number of bytes/chars,
// while respecting UTF-8 character blocks. Resulting string will be
// strictly less than unMaxChars and unMaxBytes.
//-----------------------------------------------------------------------------
bool CUtlString::TruncateUTF8Internal( size_t unMaxChars, size_t unMaxBytes )
{
	if ( !m_pchString )
		return false;

	const uint8 *pbCharacters = ( const uint8 * )m_pchString;
	size_t unBytes = 0;
	size_t unChars = 0;
	bool bSuccess = true;
	
	while ( unBytes < unMaxBytes && unChars < unMaxChars && pbCharacters[unBytes] != '\0' )
	{
		if ( ( pbCharacters[unBytes] & 0x80 ) == 0 )
		{
			// standard ASCII
			unBytes ++;
		}
		else if ( ( pbCharacters[unBytes] & 0xE0 ) == 0xC0 ) // check for 110xxxxx bit pattern, indicates 2 byte character
		{
			if ( !BVerifyValidUTF8Continuation( unBytes, 1, pbCharacters + 1 ) )
			{
				bSuccess = false;
				break;
			}

			unBytes += 2;
		}
		else if ( ( pbCharacters[unBytes] & 0xF0 ) == 0xE0 ) // check for 1110xxxx bit pattern, indicates 3 byte character
		{
			if ( !BVerifyValidUTF8Continuation( unBytes, 2, pbCharacters + 1 ) )
			{
				bSuccess = false;
				break;
			}

			unBytes += 3;
		}
		else if ( ( pbCharacters[unBytes] & 0xF8 ) == 0xF0 ) // check for 11110xxx bit pattern, indicates 4 byte character
		{
			if ( !BVerifyValidUTF8Continuation( unBytes, 3, pbCharacters + 1 ) )
			{
				bSuccess = false;
				break;
			}

			unBytes += 4;
		}
		else if ( ( pbCharacters[unBytes] & 0xFC ) == 0xF8 ) // check for 111110xx bit pattern, indicates 5 byte character
		{
			if ( !BVerifyValidUTF8Continuation( unBytes, 4, pbCharacters + 1 ) )
			{
				bSuccess = false;
				break;
			}

			unBytes += 5;
		}
		else if ( ( pbCharacters[unBytes] & 0xFE ) == 0xFC ) // check for 1111110x bit pattern, indicates 6 byte character
		{
			if ( !BVerifyValidUTF8Continuation( unBytes, 5, pbCharacters + 1 ) )
			{
				bSuccess = false;
				break;
			}

			unBytes += 6;
		}
		else
		{
			// Unexpected character
			bSuccess = false;
			break;
		}

		unChars ++;
	}

	m_pchString[unBytes] = '\0';

	return bSuccess;
}


//-----------------------------------------------------------------------------
// Purpose: spill routine for making sure our buffer is big enough for an
//			incoming string set/modify.
//-----------------------------------------------------------------------------
char *CUtlStringBuilder::InternalPrepareBuffer( size_t nChars, bool bCopyOld, size_t nMinCapacity )
{
	Assert( nMinCapacity > Capacity() );
	Assert( nMinCapacity >= nChars );
	// Don't use this class if you want a single 2GB+ string.
	static const size_t k_nMaxStringSize = 0x7FFFFFFFu;
	Assert( nMinCapacity <= k_nMaxStringSize );

	if ( nMinCapacity > k_nMaxStringSize )
	{
		SetError();
		return NULL;
	}

	bool bWasHeap = m_data.IsHeap();
	// add this to whatever we are going to grow so we don't start out too slow
	char *pszString = NULL;
	if ( nMinCapacity > MAX_STACK_STRLEN )
	{
		// Allocate 1.5 times what is requested, plus a small initial ramp
		// value so we don't spend too much time re-allocating tiny buffers.
		// A good allocator will prevent this anyways, but this makes it safer.
		// We cap it at +1 million to not get crazy.  Code actually avoides
		// computing power of two numbers since allocations almost always
		// have header/bookkeeping overhead. Don't do the dynamic sizing
		// if the user asked for a specific capacity.
		static const int k_nInitialMinRamp = 32;
		size_t nNewSize;
		if ( nMinCapacity > nChars )
			nNewSize = nMinCapacity;
		else
			nNewSize = nChars + Min<size_t>( (nChars >> 1) + k_nInitialMinRamp, k_nMillion );

		char *pszOld = m_data.Access();
		size_t nLenOld = m_data.Length();

		// order of operations is very important per comment
		// above. Make sure we copy it before changing m_data
		// in any way
		if ( bWasHeap && bCopyOld )
		{
			// maybe we'll get lucky and get the same buffer back.
			pszString = (char*) PvRealloc( pszOld, nNewSize + 1 );
			if ( !pszString )
			{
				SetError();
				return NULL;
			}
		}
		else // Either it's already on the stack; or we don't need to copy
		{
			// if the current pointer is on the heap, we aren't doing a copy
			// (or we would have used the previous realloc code. So
			// if we aren't doing a copy, don't use realloc since it will
			// copy the data if it needs to make a new allocation.
			if ( bWasHeap )
				FreePv( pszOld );

			pszString = (char*) PvAlloc( nNewSize + 1 );
			if ( !pszString )
			{
				SetError();
				return NULL;
			}

			// still need to do the copy if we are going from small buffer to large
			if ( bCopyOld )
				memcpy( pszString, pszOld, nLenOld ); // null will be added at end of func.
		}

		// just in case the user grabs .Access() and scribbles over the terminator at
		// 'length', make sure they don't run off the rails as long as they obey Capacity.
		// We don't offer this protection for the 'on stack' string.
		pszString[nNewSize] = '\0';

		m_data.Heap.m_pchString = pszString;
		m_data.Heap.m_nCapacity = (uint32)nNewSize; // capacity is the max #chars, not including the null.
		m_data.Heap.m_nLength = (uint32)nChars;
		m_data.Heap.sentinel = STRING_TYPE_SENTINEL;
	}
	else
	{
		// Rare case. Only happens if someone did a SetPtr with a length
		// less than MAX_STACK_STRLEN, or maybe a .Replace() shrunk the
		// length down.
		pszString = m_data.Stack.m_szString;
		m_data.Stack.SetBytesLeft( MAX_STACK_STRLEN - (uint8)nChars );

		if ( bWasHeap )
		{
			char *pszOldString = m_data.Heap.m_pchString;
			if ( bCopyOld )
				memcpy( pszString, pszOldString, nChars ); // null will be added at end of func.

			FreePv( pszOldString );
		}
	}

	pszString[nChars] = '\0';
	return pszString;
}


//-----------------------------------------------------------------------------
// Purpose: replace all occurrences of one string with another
//			replacement string may be NULL or "" to remove target string
//-----------------------------------------------------------------------------
size_t CUtlStringBuilder::Replace( const char *pstrTarget, const char *pstrReplacement )
{
	return ReplaceInternal( pstrTarget, pstrReplacement, V_strstr );
}


//-----------------------------------------------------------------------------
// Purpose: replace all occurrences of one character with another
//-----------------------------------------------------------------------------
size_t CUtlStringBuilder::Replace( char chTarget, char chReplacement )
{
	size_t cReplacements = 0;

	if ( !IsEmpty() && !HasError() )
	{
		char *pszString = Access();
		for ( char *pstrWalker = pszString; *pstrWalker != 0; pstrWalker++ )
		{
			if ( *pstrWalker == chTarget )
			{
				*pstrWalker = chReplacement;
				cReplacements++;
			}
		}
	}

	return cReplacements;
}


//-----------------------------------------------------------------------------
// Purpose: Truncates the string to the specified number of characters
//-----------------------------------------------------------------------------
void CUtlStringBuilder::Truncate( size_t nChars )
{
	if ( IsEmpty() || HasError() )
		return;

	size_t nLen = Length();
	if ( nLen <= nChars )
		return;

	// we may be shortening enough to fit in the small buffer, but
	// the external buffer is already allocated, so just keep using it.
	m_data.SetLength( nChars );
}


//-----------------------------------------------------------------------------
// Purpose: replace all occurrences of one string with another
//			replacement string may be NULL or "" to remove target string
//-----------------------------------------------------------------------------
size_t CUtlStringBuilder::ReplaceCaseless( const char *pstrTarget, const char *pstrReplacement )
{
	return ReplaceInternal( pstrTarget, pstrReplacement, V_stristr );
}


//-----------------------------------------------------------------------------
// Purpose: replace all occurrences of one string with another
//			replacement string may be NULL or "" to remove target string
//-----------------------------------------------------------------------------
size_t CUtlStringBuilder::ReplaceInternal( const char *pstrTarget, const char *pstrReplacement, const char *pfnCompare(const char*, const char*)  )
{
	if ( HasError() )
		return 0;

	if ( pstrReplacement == NULL )
		pstrReplacement = "";

	size_t nTargetLength = V_strlen( pstrTarget );
	size_t nReplacementLength = V_strlen( pstrReplacement );

	CUtlVector<const char *> vecMatches;
	vecMatches.EnsureCapacity( 8 );

	if ( !IsEmpty() && pstrTarget && *pstrTarget )
	{
		char *pszString = Access();

		// walk the string counting hits
		const char *pstrHit = pszString;
		for ( pstrHit = pfnCompare( pstrHit, pstrTarget ); pstrHit != NULL && *pstrHit != 0; /* inside */ )
		{
			vecMatches.AddToTail( pstrHit );
			// look for the next target and keep looping
			pstrHit = pfnCompare( pstrHit + nTargetLength, pstrTarget );
		}

		// if we didn't miss, get to work
		if ( vecMatches.Count() > 0 )
		{
			// reallocate only once; how big will we need?
			size_t nOldLength = Length();
			size_t nNewLength = nOldLength + ( vecMatches.Count() * ( nReplacementLength - nTargetLength ) );

			if ( nNewLength == 0 )
			{
				// shortcut simple case, even if rare
				m_data.Clear();
			}
			else if ( nNewLength > nOldLength )
			{
				// New string will be bigger than the old, but don't re-alloc unless
				// it is also larger than capacity.  If it fits in capacity, we will
				// be adjusting the string 'in place'.  The replacement string is larger
				// than the target string, so if we copied front to back we would screw up
				// the existing data in the 'in place' case.
				char *pstrNew;
				if ( nNewLength > Capacity() )
				{
					pstrNew = (char*) PvAlloc( nNewLength + 1 );
					if ( !pstrNew )
					{
						SetError();
						return 0;
					}
				}
				else
				{
					pstrNew = PrepareBuffer( nNewLength );
					Assert( pstrNew == pszString );
				}

				const char *pstrPreviousHit = pszString + nOldLength; // end of original string
				char *pstrDestination = pstrNew + nNewLength; // end of target
				*pstrDestination = '\0';
				// Go backwards as noted above.
				FOR_EACH_VEC_BACK( vecMatches, i )
				{
					pstrHit = vecMatches[i];
					size_t nRemainder = pstrPreviousHit - (pstrHit + nTargetLength);
					// copy the bit after the match, back up the destination and move forward from the hit
					memmove( pstrDestination - nRemainder, pstrPreviousHit-nRemainder, nRemainder );
					pstrDestination -= ( nRemainder + nReplacementLength );

					// push the replacement string in
					memcpy( pstrDestination, pstrReplacement, nReplacementLength );
					pstrPreviousHit = pstrHit;
				}

				// copy trailing stuff
				size_t nRemainder = pstrPreviousHit - pszString;
				pstrDestination -= nRemainder;
				if ( pstrDestination != pszString )
				{
					memmove( pstrDestination, pszString, nRemainder );
				}
				
				Assert( pstrNew == pstrDestination );

				// Need to set the pointer if we did were larger than capacity.
				if ( pstrNew != pszString )
					SetPtr( pstrNew, nNewLength );
			}
			else // new is shorter than or same length as old, move in place
			{
				char *pstrNew = Access();
				char *pstrPreviousHit = pstrNew;
				char *pstrDestination = pstrNew;
				FOR_EACH_VEC( vecMatches, i )
				{
					pstrHit = vecMatches[i];
					if ( pstrDestination != pstrPreviousHit )
					{
						// memmove very important as it is ok with overlaps.
						memmove( pstrDestination, pstrPreviousHit, pstrHit - pstrPreviousHit );
					}
					pstrDestination += ( pstrHit - pstrPreviousHit );
					memcpy( pstrDestination, pstrReplacement, nReplacementLength );
					pstrDestination += nReplacementLength;
					pstrPreviousHit = const_cast<char*>(pstrHit) + nTargetLength;
				}

				// copy trailing stuff
				if ( pstrDestination != pstrPreviousHit )
				{
					// memmove very important as it is ok with overlaps.
					size_t nRemainder = (pstrNew + nOldLength) - pstrPreviousHit;
					memmove( pstrDestination, pstrPreviousHit, nRemainder );
				}

				Assert( PrepareBuffer( nNewLength ) == pstrNew );
			}

		}
	}

	return vecMatches.Count();
}

//-----------------------------------------------------------------------------
// Purpose: Indicates if the target string exists in this instance.
//			The index is negative if the target string is not found, otherwise it is the index in the string.
//-----------------------------------------------------------------------------
ptrdiff_t CUtlStringBuilder::IndexOf( const char *pstrTarget ) const
{
	return ::IndexOf( String(), pstrTarget );
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string ends with the string passed in
//-----------------------------------------------------------------------------
bool CUtlStringBuilder::BEndsWith( const char *pchString ) const
{
	return ::BEndsWith( String(), pchString, false );
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string ends with the string passed in (caseless)
//-----------------------------------------------------------------------------
bool CUtlStringBuilder::BEndsWithCaseless( const char *pchString ) const
{
	return ::BEndsWith( String(), pchString, true );
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string starts with the string passed in
//-----------------------------------------------------------------------------
bool CUtlStringBuilder::BStartsWith( const char *pchString ) const
{
	return ::BStartsWith( String(), pchString, false );
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string ends with the string passed in (caseless)
//-----------------------------------------------------------------------------
bool CUtlStringBuilder::BStartsWithCaseless( const char *pchString ) const
{
	return ::BStartsWith( String(), pchString, true );
}


//-----------------------------------------------------------------------------
// Purpose: format binary data as hex characters, appending to existing data
//-----------------------------------------------------------------------------
void CUtlStringBuilder::AppendHex( const uint8 *pbInput, size_t cubInput, bool bLowercase /* = true */ )
{
	size_t cbOld = Length();
	char *pstrNew = PrepareBuffer( cbOld + cubInput*2, true );
	if ( pstrNew )
	{
		const char *pchHexLookup = bLowercase ? "0123456789abcdef" : "0123456789ABCDEF";
		char *pOut = pstrNew + cbOld;
		for ( ; cubInput; --cubInput, ++pbInput )
		{
			uint8 val = *pbInput;
			*pOut++ = pchHexLookup[val >> 4];
			*pOut++ = pchHexLookup[val & 15];
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
//			remove whitespace -- anything that is isspace() -- from the string
//-----------------------------------------------------------------------------
size_t CUtlStringBuilder::RemoveWhitespace( )
{
	if ( HasError() )
		return 0;

	char *pstrDest = m_data.Access();
	size_t cRemoved = ::RemoveWhitespace(pstrDest);

	size_t nNewLength = m_data.Length() - cRemoved;

	if ( cRemoved )
		SetLength( nNewLength );

	pstrDest = m_data.Access();
	Assert( pstrDest[nNewLength] == '\0' ); // SetLength should have set this

	return cRemoved;
}


//-----------------------------------------------------------------------------
// Purpose:	Allows setting the size to anything under the current
//			capacity.  Typically should not be used unless there was a specific
//			reason to scribble on the string. Will not touch the string contents,
//			but will append a NULL. Returns true if the length was changed.
//-----------------------------------------------------------------------------
bool CUtlStringBuilder::SetLength( size_t nLen )
{
	if ( nLen == 0 )
	{
		bool bRet = Length() > 0;
		Clear();
		return bRet;
	}

	return m_data.SetLength( nLen ) != NULL;
}


//-----------------------------------------------------------------------------
// Purpose:	Convert to heap string if needed, and give it away.
//-----------------------------------------------------------------------------
char *CUtlStringBuilder::DetachRawPtr( size_t *pnLen, size_t *pnCapacity )
{
	size_t nLen = 0;
	size_t nCapacity = 0;
	char *psz = m_data.DetachHeapString( nLen, nCapacity );

	if ( pnLen )
		*pnLen = nLen;

	if ( pnCapacity )
		*pnCapacity = nCapacity;

	return psz;
}


//-----------------------------------------------------------------------------
// Purpose:	Convert to heap string if needed, and give it away.
//-----------------------------------------------------------------------------
CUtlString CUtlStringBuilder::DetachString()
{
	CUtlString ret;
	if ( Length() )
	{
		size_t nLen = 0;
		size_t nCapacity = 0;
		ret.SetPtr( m_data.DetachHeapString( nLen, nCapacity ) );
	}
	return ret;
}



//-----------------------------------------------------------------------------
// Purpose: 
//			trim whitespace from front and back of string
//-----------------------------------------------------------------------------
size_t CUtlStringBuilder::TrimWhitespace()
{
	if ( HasError() )
		return 0;

	char *pchString = m_data.Access();
	int cChars = V_StrTrim( pchString );

	SetLength( cChars );

	return cChars;
}


//-----------------------------------------------------------------------------
// Purpose: 
//			trim whitespace from back of string
//-----------------------------------------------------------------------------
size_t CUtlStringBuilder::TrimTrailingWhitespace()
{
	if ( HasError() )
		return 0;

	char *pchString = m_data.Access();
	uint32 cChars = Length();
	if ( cChars == 0 )
		return 0;

	char *pCur = &pchString[cChars - 1];
	while ( pCur >= pchString && isspace( *pCur ) )
	{
		*pCur = '\0';
		pCur--;
	}

	size_t ret = pCur - pchString + 1;
	SetLength( ret );

	return ret;
}


//-----------------------------------------------------------------------------
// Purpose: Swaps string contents
//-----------------------------------------------------------------------------
void CUtlStringBuilder::Swap( CUtlStringBuilder &src )
{
	// swapping m_data.Raw instead of '*this' prevents having to
	// copy dynamic strings. Important that m_data doesn't know
	// any lifetime rules about its members (ie: it should not have
	// a destructor that frees the dynamic string pointer).
	Data temp;
	temp.Raw = src.m_data.Raw;
	src.m_data.Raw = m_data.Raw;
	m_data.Raw = temp.Raw;
}


//-----------------------------------------------------------------------------
// Purpose: Swaps string contents between CUtlString and CUtlStringBuilder
//-----------------------------------------------------------------------------
void CUtlStringBuilder::Swap( CUtlString &src )
{
	char *sz = src.DetachRawPtr();
	if ( !IsEmpty() )
	{
		src.SetPtr( DetachRawPtr() );
	}
	if ( sz && sz[0] )
	{
		m_data.FreeHeap();
		m_data.SetPtr( sz, V_strlen( sz ) );
	}
}


//-----------------------------------------------------------------------------
// Purpose: Swaps string contents between CUtlString and CUtlStringBuilder
//-----------------------------------------------------------------------------
void CUtlString::Swap( CUtlStringBuilder &src )
{
	src.Swap( *this );
}


//-----------------------------------------------------------------------------
// Purpose: adjust length and add null terminator, within capacity bounds
//-----------------------------------------------------------------------------
char *CUtlStringBuilder::Data::SetLength( size_t nChars )
{
	// heap/stack must be set correctly, and will not
	// be changed by this routine.
	if ( IsHeap() )
	{
		if ( !Heap.m_pchString || nChars > Heap.m_nCapacity )
			return NULL;
		Heap.m_nLength = (uint32)nChars;
		Heap.m_pchString[nChars] = '\0';
		return Heap.m_pchString;
	}
	if ( nChars > MAX_STACK_STRLEN )
		return NULL;
	Stack.m_szString[nChars] = '\0';
	Stack.SetBytesLeft( MAX_STACK_STRLEN - (uint8)nChars );
	return Stack.m_szString;
}


//-----------------------------------------------------------------------------
// Purpose: Give the string away and set to an empty state
//-----------------------------------------------------------------------------
char *CUtlStringBuilder::Data::DetachHeapString( size_t &nLen, size_t &nCapacity )
{
	MoveToHeap();

	if ( HasError() )
	{
		nLen = 0;
		nCapacity = 0;
		return NULL;
	}

	nLen = Heap.m_nLength;
	nCapacity = Heap.m_nCapacity;
	char *psz = Heap.m_pchString;
	Construct();
	return psz;
}


//-----------------------------------------------------------------------------
// Purpose:	Allows setting the raw pointer and taking ownership
//-----------------------------------------------------------------------------
void CUtlStringBuilder::Data::SetPtr( char *pchString, size_t nLength )
{
	// We don't care about the error state since we are totally replacing
	// the string.

	// ok, length may be small enough to fit in our short buffer
	// but we've already got a dynamically allocated string, so let
	// it be in the heap buffer anyways.
	Heap.m_pchString = pchString;
	Heap.m_nCapacity = (uint32)nLength;
	Heap.m_nLength = (uint32)nLength;
	Heap.sentinel = STRING_TYPE_SENTINEL;

	// their buffer must have room for the null
	Heap.m_pchString[nLength] = '\0';
}


//-----------------------------------------------------------------------------
// Purpose:	Enable the error state, moving the string to the heap if
//			it isn't there.
//-----------------------------------------------------------------------------
void CUtlStringBuilder::Data::SetError( bool bEnableAssert )
{
	if ( HasError() )
		return;

	// This is not meant to be used as a status bit. Setting the error state should
	// mean something very unexpected happened that you would want a call stack for.
	// That is why this asserts unconditionally when the state is being flipped.
	if ( bEnableAssert )
		AssertMsg( false, "Error State on string being set." );

	MoveToHeap();

	Heap.sentinel = ( STRING_TYPE_SENTINEL | STRING_TYPE_ERROR );
}


//-----------------------------------------------------------------------------
// Purpose:	Set string to empty state
//-----------------------------------------------------------------------------
void CUtlStringBuilder::Data::ClearError()
{
	if ( HasError() )
	{
		Heap.sentinel = STRING_TYPE_SENTINEL;
		Clear();
	}
}


//-----------------------------------------------------------------------------
// Purpose:	If the string is on the stack, move it to the heap.
//			create a null heap string if memory can't be allocated.
//			Callers of this /need/ the string to be in the heap state
//			when done.
//-----------------------------------------------------------------------------
bool CUtlStringBuilder::Data::MoveToHeap()
{
	bool bSuccess = true;

	if ( !IsHeap() )
	{
		// try to recover the string at the point of failure, to help with debugging
		size_t nLen = Length();
		char *pszHeapString = (char*)PvAlloc( nLen+1 );
		if ( pszHeapString )
		{
			// get the string copy before corrupting the stack union
			char *pszStackString = Access();
			memcpy( pszHeapString, pszStackString, nLen	);
			pszHeapString[nLen] = 0;

			Heap.m_pchString = pszHeapString;
			Heap.m_nLength = (uint32)nLen;
			Heap.m_nCapacity = (uint32)nLen;
			Heap.sentinel = STRING_TYPE_SENTINEL;
		}
		else
		{
			Heap.m_pchString = NULL;
			Heap.m_nLength = 0;
			Heap.m_nCapacity = 0;
			bSuccess = false;
			Heap.sentinel = ( STRING_TYPE_SENTINEL | STRING_TYPE_ERROR );
		}

	}

	return bSuccess;
}

#if defined(POSIX) && !defined(NO_MALLOC_OVERRIDE)

#include "tier0/memdbgoff.h"

// Some places call CRT routines that return malloc'ed memory,
// so we need a way to call the real CRT free.
void real_free( void *pMem )
{
    free( pMem );
}

#endif
