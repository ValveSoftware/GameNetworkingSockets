//========= Copyright Valve Corporation, All rights reserved. ============//

#include <tier0/dbg.h>
#include <vstdlib/strtools.h>
#include <tier1/utlvector.h>

#ifdef _WIN32
#include "winlite.h"
#endif

#ifdef POSIX
	#include <unistd.h>
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#if defined( _WIN32 ) || defined( WIN32 )
#define PATHSEPARATOR(c) ((c) == '\\' || (c) == '/')
#else	//_WIN32
#define PATHSEPARATOR(c) ('/' == (c))
#endif	//_WIN32



static char* AllocString( const char *pStr, int nMaxChars )
{
	int allocLen;
	if ( nMaxChars == -1 )
		allocLen = V_strlen( pStr ) + 1;
	else
		allocLen = Min( V_strlen(pStr), nMaxChars ) + 1;

	char *pOut = new char[allocLen];
	V_strncpy( pOut, pStr, allocLen );
	return pOut;
}

int V_strncmp( const char *s1, const char *s2, int count )
{
	Assert( count >= 0 );
	Assert( count == 0 || s1 != NULL );
	Assert( count == 0 || s2 != NULL );

	while ( count-- > 0 )
	{
		if ( *s1 != *s2 )
			return *s1 < *s2 ? -1 : 1; // string different
		if ( *s1 == '\0' )
			return 0; // null terminator hit - strings the same
		s1++;
		s2++;
	}

	return 0; // count characters compared the same
}

//-----------------------------------------------------------------------------
// Finds a string in another string with a case insensitive test w/ length validation
//-----------------------------------------------------------------------------
char const* V_strnistr( char const* pStr, char const* pSearch, int n )
{
	Assert( pStr != NULL );
	Assert( pSearch != NULL );

	if ( pStr == NULL || pSearch == NULL ) 
		return 0;

	char const* pLetter = pStr;

	// Check the entire string
	while ( *pLetter != 0 )
	{
		if ( n <= 0 )
			return 0;

		// Skip over non-matches
		if ( tolower( *pLetter ) == tolower( *pSearch ) )
		{
			int n1 = n - 1;

			// Check for match
			char const* pMatch = pLetter + 1;
			char const* pTest = pSearch + 1;
			while (*pTest != 0)
			{
				if ( n1 <= 0 )
					return 0;

				// We've run off the end; don't bother.
				if (*pMatch == 0)
					return 0;

				if ( tolower( *pMatch ) != tolower( *pTest ) )
					break;

				++pMatch;
				++pTest;
				--n1;
			}

			// Found a match!
			if (*pTest == 0)
				return pLetter;
		}

		++pLetter;
		--n;
	}

	return 0;
}

const char* V_strnchr( const char* pStr, char c, int n )
{
	char const* pLetter = pStr;
	char const* pLast = pStr + n;

	// Check the entire string
	while ( (pLetter < pLast) && (*pLetter != 0) )
	{
		if (*pLetter == c)
			return pLetter;
		++pLetter;
	}
	return NULL;
}

int V_strnicmp( const char *s1, const char *s2, int n )
{
	Assert( n >= 0 );
	Assert( n == 0 || s1 != NULL );
	Assert( n == 0 || s2 != NULL );
	
	while ( n-- > 0 )
	{
		int c1 = *s1++;
		int c2 = *s2++;

		if ( c1 != c2 )
		{
			if ( c1 >= 'a' && c1 <= 'z' )
				c1 -= ('a' - 'A');
			if ( c2 >= 'a' && c2 <= 'z' )
				c2 -= ( 'a' - 'A' );
			if ( c1 != c2 )
				return c1 < c2 ? -1 : 1;
		}
		if ( c1 == '\0' )
			return 0; // null terminator hit - strings the same
	}
	
	return 0; // n characters compared the same
}

//-----------------------------------------------------------------------------
// Finds a string in another string with a case insensitive test
//-----------------------------------------------------------------------------
char const* V_stristr( char const* pStr, char const* pSearch )
{
	Assert( pStr != NULL );
	Assert( pSearch != NULL );

	if ( pStr == NULL || pSearch == NULL ) 
		return NULL;

	char const* pLetter = pStr;

	// Check the entire string
	while ( *pLetter != 0 )
	{
		// Skip over non-matches
		if ( tolower( (unsigned char) *pLetter ) == tolower( (unsigned char) *pSearch ) )
		{
			// Check for match
			char const* pMatch = pLetter + 1;
			char const* pTest = pSearch + 1;
			while (*pTest != 0)
			{
				// We've run off the end; don't bother.
				if (*pMatch == 0)
					return 0;

				if ( tolower( (unsigned char) *pMatch ) != tolower( (unsigned char) *pTest ) )
					break;

				++pMatch;
				++pTest;
			}

			// Found a match!
			if ( *pTest == 0 )
				return pLetter;
		}

		++pLetter;
	}

	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: Convert ASCII characters to lower case in-place
//-----------------------------------------------------------------------------
char *V_strlower_fast( char *pch )
{
	for ( char *pCurrent = pch; *pCurrent; ++pCurrent )
	{
		if ( (unsigned char)(*pCurrent - 'A') <= (unsigned char)('Z' - 'A') )
			*pCurrent = *pCurrent - 'A' + 'a';
	}
	return pch;
}


//-----------------------------------------------------------------------------
// Purpose: Convert ASCII characters to upper case in-place
//-----------------------------------------------------------------------------
char *V_strupper_fast( char *pch )
{
	for ( char *pCurrent = pch; *pCurrent; ++pCurrent )
	{
		if ( (unsigned char)(*pCurrent - 'a') <= (unsigned char)('z' - 'a') )
			*pCurrent = *pCurrent - 'a' + 'A';
	}
	return pch;
}


//-----------------------------------------------------------------------------
// Purpose: copy a string while observing the maximum length of the target buffer
//
// Inputs:	pDest	- pointer to the destination string
//		pSrc	- pointer to the source string
//		maxLen	- number of bytes available at pDest
//
// This function will always leave pDest nul-terminated if maxLen > 0
//
// maxLen is a count of bytes, not a count of characters.
// maxLen includes budget for the nul terminator at pDest,
// so using maxLen = 3 would copy two characters from pSrc and add a trailing nul.
//-----------------------------------------------------------------------------
void V_strncpy( char *pDest, char const *pSrc, size_t maxLen )
{
	Assert( maxLen == 0 || pDest != NULL );
	Assert( pSrc != NULL );

	size_t nCount = maxLen;
	char *pstrDest = pDest;
	const char *pstrSource = pSrc;

	while ( 0 < nCount && 0 != ( *pstrDest++ = *pstrSource++ ) )
		nCount--;

	if ( maxLen > 0 )
		pstrDest[-1] = 0;
}

int V_snprintf( char *pDest, size_t bufferLen, char const *pFormat, ... )
{
	Assert( bufferLen > 0 );
	Assert( pDest != NULL );
	Assert( pFormat != NULL );

	va_list marker;

	va_start( marker, pFormat );
	// _vsnprintf will not write a terminator if the output string uses the entire buffer you provide
	int len = _vsnprintf( pDest, bufferLen, pFormat, marker );
	va_end( marker );

	// Len < 0 represents an overflow on windows; len > buffer length on posix
	if (( len < 0 ) || ((size_t)len >= bufferLen ) )
	{
		len = (int)(bufferLen-1);
	}
	pDest[len] = 0;

	return len;
}

int V_vsnprintf( char *pDest, int bufferLen, char const *pFormat, va_list params )
{
	Assert( bufferLen > 0 );
	Assert( pDest != NULL );
	Assert( pFormat != NULL );

	int len = _vsnprintf( pDest, bufferLen, pFormat, params );

	// Len < 0 represents an overflow on windows; len > buffer length on posix
	if (( len < 0 ) || (len >= bufferLen ) )
	{
		len = bufferLen-1;
	}
	pDest[len] = 0;

	return len;
}

int V_vsnprintfRet( char *pDest, int bufferLen, const char *pFormat, va_list params, bool *pbTruncated )
{
	Assert( bufferLen > 0 );
	Assert( bufferLen == 0 || pDest != NULL );
	Assert( pFormat != NULL );

	bool bTruncatedUnderstudy;
	if ( !pbTruncated )
		pbTruncated = &bTruncatedUnderstudy;

	int len = _vsnprintf( pDest, bufferLen, pFormat, params );

	// Len < 0 represents an overflow on windows; len > buffer length on posix
	if (( len < 0 ) || (len >= bufferLen ) )
	{
		*pbTruncated = true;
		len = bufferLen-1;
	}
	else
	{
		*pbTruncated = false;
	}

	pDest[len] = 0;

	return len;
}

//-----------------------------------------------------------------------------
// Purpose: If COPY_ALL_CHARACTERS == max_chars_to_copy then we try to add the whole pSrc to the end of pDest, otherwise
//  we copy only as many characters as are specified in max_chars_to_copy (or the # of characters in pSrc if thats's less).
// Input  : *pDest - destination buffer
//			*pSrc - string to append
//			destBufferSize - sizeof the buffer pointed to by pDest
//			max_chars_to_copy - COPY_ALL_CHARACTERS in pSrc or max # to copy
// Output : char * the copied buffer
//-----------------------------------------------------------------------------
char *V_strncat(char *pDest, const char *pSrc, size_t destBufferSize, int max_chars_to_copy )
{
	size_t charstocopy = (size_t)0;

	Assert( pDest != NULL );
	Assert( pSrc != NULL );
	
	size_t len = strlen(pDest);
	size_t srclen = strlen( pSrc );
	if ( max_chars_to_copy <= COPY_ALL_CHARACTERS )
	{
		charstocopy = srclen;
	}
	else
	{
		charstocopy = (size_t)Min( max_chars_to_copy, (int)srclen );
	}

	if ( len + charstocopy >= destBufferSize )
	{
		charstocopy = destBufferSize - len - 1;
	}

	if ( (ptrdiff_t)charstocopy <= 0 )
	{
		return pDest;
	}

	char *pOut = strncat( pDest, pSrc, charstocopy );
	return pOut;
}

void V_SplitString2( const char *pString, const char * const *pSeparators, int nSeparators, CUtlVector<char*> &outStrings, bool bIncludeEmptyStrings )
{
	outStrings.Purge();
	const char *pCurPos = pString;
	for (;;)
	{
		int iFirstSeparator = -1;
		const char *pFirstSeparator = 0;
		for ( int i=0; i < nSeparators; i++ )
		{
			const char *pTest = V_stristr( pCurPos, pSeparators[i] );
			if ( pTest && (!pFirstSeparator || pTest < pFirstSeparator) )
			{
				iFirstSeparator = i;
				pFirstSeparator = pTest;
			}
		}

		if ( pFirstSeparator )
		{
			// Split on this separator and continue on.
			int separatorLen = (int)strlen( pSeparators[iFirstSeparator] );
			if ( pFirstSeparator > pCurPos || ( pFirstSeparator == pCurPos && bIncludeEmptyStrings ) )
			{
				outStrings.AddToTail( AllocString( pCurPos, pFirstSeparator-pCurPos ) );
			}

			pCurPos = pFirstSeparator + separatorLen;
		}
		else
		{
			// Copy the rest of the string, if there's anything there
			if ( pCurPos[0] != 0 )
			{
				outStrings.AddToTail( AllocString( pCurPos, -1 ) );
			}
			return;
		}
	}
}

void V_AllocAndSplitString( const char *pString, const char *pSeparator, CUtlVector<char*> &outStrings, bool bIncludeEmptyStrings )
{
	V_SplitString2( pString, &pSeparator, 1, outStrings, bIncludeEmptyStrings );
}

//-----------------------------------------------------------------------------
void V_StripTrailingWhitespaceASCII( char *pch )
{
	if ( !pch )
		return;

	// Remember where we would terminate the string
	char *t = pch;

	// Scan until we hit the end of the string
	while ( *pch != '\0' )
	{

		// Non-whitespace?  Then assume termination immediately
		// after, if we don't find any more whitespace
		if ( !V_isspace( *pch ) )
			t = pch+1;
		++pch;
	}

	// Terminate
	*t = '\0';
}

int V_StrTrim( char *pStr )
{
	char *pSource = pStr;
	char *pDest = pStr;

	// skip white space at the beginning
	while ( *pSource != 0 && isspace( *pSource ) )
	{
		pSource++;
	}

	// copy everything else
	char *pLastWhiteBlock = NULL;
	char *pStart = pDest;
	while ( *pSource != 0 )
	{
		*pDest = *pSource++;
		if ( isspace( *pDest ) )
		{
			if ( pLastWhiteBlock == NULL )
				pLastWhiteBlock = pDest;
		}
		else
		{
			pLastWhiteBlock = NULL;
		}
		pDest++;
	}
	*pDest = 0;

	// did we end in a whitespace block?
	if ( pLastWhiteBlock != NULL )
	{
		// yep; shorten the string
		pDest = pLastWhiteBlock;
		*pLastWhiteBlock = 0;
	}

	return pDest - pStart;
}
