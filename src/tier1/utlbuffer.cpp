//===== Copyright Valve Corporation, All rights reserved. ======//

#ifdef _MSC_VER
#pragma warning (disable : 4514)
#pragma warning (disable : 4706)
#endif

#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>
#include "vstdlib/strtools.h"
#include "tier1/utlbuffer.h"
#include "tier1/fmtstr.h"
			
// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"
			    

//-----------------------------------------------------------------------------
// Character conversions for C strings
//-----------------------------------------------------------------------------
class CUtlCStringConversion : public CUtlCharConversion
{
public:
	CUtlCStringConversion( const char nEscapeChar, const char *pDelimiter, int nCount, ConversionArray_t *pArray );

	// Finds a conversion for the passed-in string, returns length
	virtual char FindConversion( const char *pString, int *pLength );

private:
	char m_pConversion[256];
};


//-----------------------------------------------------------------------------
// Character conversions for no-escape sequence strings
//-----------------------------------------------------------------------------
class CUtlNoEscConversion : public CUtlCharConversion
{
public:
	CUtlNoEscConversion( const char nEscapeChar, const char *pDelimiter, int nCount, ConversionArray_t *pArray ) :
		CUtlCharConversion( nEscapeChar, pDelimiter, nCount, pArray ) {}

	// Finds a conversion for the passed-in string, returns length
	virtual char FindConversion( const char *pString, int *pLength ) { *pLength = 0; return 0; }
};


//-----------------------------------------------------------------------------
// List of character conversions
//-----------------------------------------------------------------------------
BEGIN_CUSTOM_CHAR_CONVERSION( CUtlCStringConversion, s_StringCharConversion, "\"", '\\' )
	{ '\n', "n" },
	{ '\t', "t" },
	{ '\v', "v" },
	{ '\b', "b" },
	{ '\r', "r" },
	{ '\f', "f" },
	{ '\a', "a" },
	{ '\\', "\\" },
	{ '\?', "\?" },
	{ '\'', "\'" },
	{ '\"', "\"" },
END_CUSTOM_CHAR_CONVERSION( CUtlCStringConversion, s_StringCharConversion, "\"", '\\' )

CUtlCharConversion *GetCStringCharConversion()
{
	return &s_StringCharConversion;
}

BEGIN_CUSTOM_CHAR_CONVERSION( CUtlNoEscConversion, s_NoEscConversion, "\"", 0x7F )
	{ 0x7F, "" },
END_CUSTOM_CHAR_CONVERSION( CUtlNoEscConversion, s_NoEscConversion, "\"", 0x7F )

CUtlCharConversion *GetNoEscCharConversion()
{
	return &s_NoEscConversion;
}


//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------
CUtlCStringConversion::CUtlCStringConversion( const char nEscapeChar, const char *pDelimiter, int nCount, ConversionArray_t *pArray ) : 
	CUtlCharConversion( nEscapeChar, pDelimiter, nCount, pArray )
{
	memset( m_pConversion, 0x0, sizeof(m_pConversion) );
	for ( int i = 0; i < nCount; ++i )
	{
		m_pConversion[ (unsigned char)pArray[i].m_pReplacementString[0] ] = pArray[i].m_nActualChar;
	}
}

// Finds a conversion for the passed-in string, returns length
char CUtlCStringConversion::FindConversion( const char *pString, int *pLength )
{
	char c = m_pConversion[ (unsigned char)pString[0] ];
	*pLength = (c != '\0') ? 1 : 0;
	return c;
}



//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------
CUtlCharConversion::CUtlCharConversion( char nEscapeChar, const char *pDelimiter, int nCount, ConversionArray_t *pArray )
{
	m_nEscapeChar = nEscapeChar;
	m_pDelimiter = pDelimiter;
	m_nCount = nCount;
	m_nDelimiterLength = V_strlen( pDelimiter );
	m_nMaxConversionLength = 0;

	memset( m_pReplacements, 0, sizeof(m_pReplacements) );

	for ( int i = 0; i < nCount; ++i )
	{
		m_pList[i] = pArray[i].m_nActualChar;
		ConversionInfo_t &info = m_pReplacements[ (unsigned char)m_pList[i] ];
		Assert( info.m_pReplacementString == 0 );
		info.m_pReplacementString = pArray[i].m_pReplacementString;
		info.m_nLength = V_strlen( info.m_pReplacementString );
		if ( info.m_nLength > m_nMaxConversionLength )
		{
			m_nMaxConversionLength = info.m_nLength;
		}
	}
}


//-----------------------------------------------------------------------------
// Escape character + delimiter
//-----------------------------------------------------------------------------
char CUtlCharConversion::GetEscapeChar() const
{
	return m_nEscapeChar;
}

const char *CUtlCharConversion::GetDelimiter() const
{
	return m_pDelimiter;
}

int CUtlCharConversion::GetDelimiterLength() const
{
	return m_nDelimiterLength;
}


//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------
const char *CUtlCharConversion::GetConversionString( char c ) const
{
	return m_pReplacements[ (unsigned char)c ].m_pReplacementString;
}

int CUtlCharConversion::GetConversionLength( char c ) const
{
	return m_pReplacements[ (unsigned char)c ].m_nLength;
}

int CUtlCharConversion::MaxConversionLength() const
{
	return m_nMaxConversionLength;
}


//-----------------------------------------------------------------------------
// Finds a conversion for the passed-in string, returns length
//-----------------------------------------------------------------------------
char CUtlCharConversion::FindConversion( const char *pString, int *pLength )
{
	for ( int i = 0; i < m_nCount; ++i )
	{
		if ( !V_strcmp( pString, m_pReplacements[ (unsigned char)m_pList[i] ].m_pReplacementString ) )
		{
			*pLength = m_pReplacements[ (unsigned char)m_pList[i] ].m_nLength;
			return m_pList[i];
		}
	}

	*pLength = 0;
	return '\0';
}


//-----------------------------------------------------------------------------
// constructors
//-----------------------------------------------------------------------------
CUtlBuffer::CUtlBuffer( int growSize, int initSize, int nFlags ) : 
	m_Memory( growSize, initSize ), m_Error(0)
{
	m_Get = 0;
	m_Put = 0;
	m_nTab = 0;
	m_Flags = nFlags;
	if ( (initSize != 0) && !IsReadOnly() )
	{
		m_nMaxPut = -1;
		AddNullTermination();
	}
	else
	{
		m_nMaxPut = 0;
	}
	SetOverflowFuncs( &CUtlBuffer::GetOverflow, &CUtlBuffer::PutOverflow );
}

CUtlBuffer::CUtlBuffer( const void *pBuffer, int nSize, int nFlags ) :
	m_Memory( (unsigned char*)pBuffer, nSize ), m_Error(0)
{
	DbgAssert( nSize != 0 );

	m_Get = 0;
	m_Put = 0;
	m_nTab = 0;
	m_Flags = nFlags;
	if ( IsReadOnly() )
	{
		m_nMaxPut = nSize;
		m_Put = nSize;
	}
	else
	{
		m_nMaxPut = -1;
		AddNullTermination();
	}
	SetOverflowFuncs( &CUtlBuffer::GetOverflow, &CUtlBuffer::PutOverflow );
}


//-----------------------------------------------------------------------------
// Modifies the buffer to be binary or text; Blows away the buffer and the CONTAINS_CRLF value. 
//-----------------------------------------------------------------------------
void CUtlBuffer::SetBufferType( bool bIsText, bool bContainsCRLF )
{
#ifdef _DEBUG
	if ( IsText() )
	{
		if ( bIsText )
		{
			Assert( ContainsCRLF() == bContainsCRLF );
		}
		else
		{
			Assert( ContainsCRLF() );
		}
	}
	else
	{
		if ( bIsText )
		{
			Assert( bContainsCRLF );
		}
	}
#endif

	if ( bIsText )
	{
		m_Flags |= TEXT_BUFFER;
	}
	else
	{
		m_Flags &= ~TEXT_BUFFER;
	}
	if ( bContainsCRLF )
	{
		m_Flags |= CONTAINS_CRLF;
	}
	else
	{
		m_Flags &= ~CONTAINS_CRLF;
	}
}


//-----------------------------------------------------------------------------
// Attaches the buffer to external memory....
//-----------------------------------------------------------------------------
void CUtlBuffer::SetExternalBuffer( void* pMemory, int nSize, int nInitialPut, int nFlags )
{
	m_Memory.SetExternalBuffer( (unsigned char*)pMemory, nSize );

	// Reset all indices; we just changed memory
	m_Get = 0;
	m_Put = nInitialPut;
	m_nTab = 0;
	m_Error = 0;
	m_Flags = nFlags;
	m_nMaxPut = -1;
	AddNullTermination();
}


//-----------------------------------------------------------------------------
// Purpose: Attaches to an external buffer as read only. Will purge any existing buffer data
//-----------------------------------------------------------------------------
void CUtlBuffer::SetReadOnlyBuffer( void *pMemory, int nSize )
{
	Purge();

	m_Memory.SetExternalBuffer( pMemory, nSize );
	m_Get = 0;
	m_Put = nSize;
	m_nTab = 0;
	m_Flags |= READ_ONLY;
	m_nMaxPut = nSize;
}


//-----------------------------------------------------------------------------
// Makes sure we've got at least this much memory
//-----------------------------------------------------------------------------
void CUtlBuffer::EnsureCapacity( int num )
{
	// Add one extra for the null termination
	if ( IsText() )
		num += 1;

	if ( m_Memory.IsExternallyAllocated() )
	{
		if ( IsGrowable() && ( m_Memory.NumAllocated() < num ) )
		{
			m_Memory.ConvertToGrowableMemory( 0 );
		}
		else
		{
			num -= 1;
		}
	}

	m_Memory.EnsureCapacity( num );
}


//-----------------------------------------------------------------------------
// Base get method from which all others derive
//-----------------------------------------------------------------------------
bool CUtlBuffer::Get( void* pMem, int size )
{
	if ( CheckGet( size ) )
	{
		memcpy( pMem, &m_Memory[m_Get], size );
		m_Get += size;
		return true;
	}
	else
	{
		return false;
	}
}


//-----------------------------------------------------------------------------
// This will get at least 1 byte and up to nSize bytes. 
// It will return the number of bytes actually read.
//-----------------------------------------------------------------------------
int CUtlBuffer::GetUpTo( void *pMem, int nSize )
{
	if ( CheckArbitraryPeekGet( 0, nSize ) )
	{
		memcpy( pMem, &m_Memory[m_Get], nSize );
		m_Get += nSize;
		return nSize;
	}
	return 0;	
}

	
//-----------------------------------------------------------------------------
// Eats whitespace
//-----------------------------------------------------------------------------
void CUtlBuffer::EatWhiteSpace()
{
	if ( IsText() && IsValid() )
	{
		while ( CheckGet( sizeof(char) ) )
		{
			if ( !isspace( *(const char*)PeekGet() ) )
				break;
			m_Get += sizeof(char);
		}
	}
}

	
//-----------------------------------------------------------------------------
// Eats whitespace without causing overflows
//-----------------------------------------------------------------------------
void CUtlBuffer::EatWhiteSpaceNoOverflow()
{
	if ( IsText() && IsValid() )
	{
		while ( CheckPeekGet( 0, sizeof(char) ) )
		{
			if ( !isspace( *(const char*)PeekGet() ) )
				break;
			m_Get += sizeof(char);
		}
	}
}


//-----------------------------------------------------------------------------
// Eats C++ style comments
//-----------------------------------------------------------------------------
bool CUtlBuffer::EatCPPComment()
{
	if ( IsText() && IsValid() )
	{
		// If we don't have a a c++ style comment next, we're done
		const char *pPeek = (const char *)PeekGet( 2 * sizeof(char), 0 );
		if ( !pPeek || ( pPeek[0] != '/' ) || ( pPeek[1] != '/' ) )
			return false;

		// Deal with c++ style comments
		m_Get += 2;

		// read complete line
		for ( char c = GetChar(); IsValid(); c = GetChar() )
		{
			if ( c == '\n' )
				break;
		}
		return true;
	}
	return false;
}

	
//-----------------------------------------------------------------------------
// Peeks how much whitespace to eat
//-----------------------------------------------------------------------------
int CUtlBuffer::PeekWhiteSpace( int nOffset )
{
	if ( !IsText() || !IsValid() )
		return 0;

	while ( CheckPeekGet( nOffset, sizeof(char) ) )
	{
		if ( !isspace( *(char*)PeekGet( nOffset ) ) )
			break;
		nOffset += sizeof(char);
	}

	return nOffset;
}


//-----------------------------------------------------------------------------
// Peek size of sting to come, check memory bound
//-----------------------------------------------------------------------------
int	CUtlBuffer::PeekStringLength()
{
	if ( !IsValid() || !CheckPeekGet( 0, sizeof(char) ) )
		return 0;

	// Eat preceeding whitespace
	int nOffset = 0;
	if ( IsText() )
	{
		nOffset = PeekWhiteSpace( nOffset );
	}

	int nStartingOffset = nOffset;

	do
	{
		int nPeekAmount = 128;

		// CheckArbitraryPeekGet will set nPeekAmount to the remaining buffer if we hit the end
		// NOTE: Add 1 for the terminating zero!
		if ( !CheckArbitraryPeekGet( nOffset, nPeekAmount ) )
			return nOffset - nStartingOffset + 1;

		const char *pTest = (const char *)PeekGet( nOffset );

		if ( !IsText() )
		{
			for ( int i = 0; i < nPeekAmount; ++i )
			{
				// The +1 here is so we eat the terminating 0
				if ( pTest[i] == 0 )
					return (i + nOffset - nStartingOffset + 1);
			}
		}
		else
		{
			for ( int i = 0; i < nPeekAmount; ++i )
			{
				// The +1 here is so we eat the terminating 0
				if ( isspace(pTest[i]) || (pTest[i] == 0) )
					return (i + nOffset - nStartingOffset + 1);
			}
		}

		nOffset += nPeekAmount;

	} while ( true );
}


//-----------------------------------------------------------------------------
// Does the next bytes of the buffer match a pattern?
//-----------------------------------------------------------------------------
bool CUtlBuffer::PeekStringMatch( int nOffset, const char *pString, int nLen )
{
	if ( !CheckPeekGet( nOffset, nLen ) )
		return false;
	return !V_strncmp( (const char*)PeekGet(nOffset), pString, nLen );
}


//-----------------------------------------------------------------------------
// This version of PeekStringLength converts \" to \\ and " to \, etc.
// It also reads a " at the beginning and end of the string
//-----------------------------------------------------------------------------
int CUtlBuffer::PeekDelimitedStringLength( CUtlCharConversion *pConv, bool bActualSize )
{
	if ( !IsText() || !pConv )
		return PeekStringLength();

	// Eat preceeding whitespace
	int nOffset = 0;
	if ( IsText() )
	{
		nOffset = PeekWhiteSpace( nOffset );
	}

	if ( !PeekStringMatch( nOffset, pConv->GetDelimiter(), pConv->GetDelimiterLength() ) )
		return 0;

	// Try to read ending ", but don't accept \"
	int nActualStart = nOffset;
	nOffset += pConv->GetDelimiterLength();
	int nLen = 1;	// Starts at 1 for the '\0' termination

	do
	{
		if ( PeekStringMatch( nOffset, pConv->GetDelimiter(), pConv->GetDelimiterLength() ) )
			break;

		if ( !CheckPeekGet( nOffset, 1 ) )
			break;

		char c = *(const char*)PeekGet( nOffset );
		++nLen;
		++nOffset;
		if ( c == pConv->GetEscapeChar() )
		{
			int nLength = pConv->MaxConversionLength();
			if ( !CheckArbitraryPeekGet( nOffset, nLength ) )
				break;

			pConv->FindConversion( (const char*)PeekGet(nOffset), &nLength );
			nOffset += nLength;
		}
	}  while (true);

	return bActualSize ? nLen : nOffset - nActualStart + pConv->GetDelimiterLength() + 1;
}


//-----------------------------------------------------------------------------
// returns a pointer to the next string. works for binary buffers only
//-----------------------------------------------------------------------------
const char* CUtlBuffer::GetStringFast()
{
	if ( !IsValid() )
		return NULL; // buffer invalid

	if ( IsText() )
	{
		AssertMsg( false, "CUtlBuffer::GetStringFast: binary buffers only" );
		return NULL; // this function doesn't work in text mode
	}

	// Remember, this *includes* the null character
	// It will be 0, however, if the buffer is empty.
	int nLen = PeekStringLength();

	if ( nLen == 0 )
	{
		m_Error |= GET_OVERFLOW;
		return NULL;
	}

	const char *pString = (const char*)Base() + TellGet();

	// skip string, but not terminating 0
	SeekGet( SEEK_CURRENT, nLen - 1 );

	// Read the terminating NULL, make sure it's there
	if ( GetChar() != 0 )
	{
		AssertMsg( false, "CUtlBuffer::GetStringFast: no string termination" );
		return NULL;
	}
	
	return pString;
}

//-----------------------------------------------------------------------------
// Reads a null-terminated string
//-----------------------------------------------------------------------------
bool CUtlBuffer::GetString( char *pString, int nMaxChars )
{
	if (!IsValid())
	{
		*pString = 0;
		return false;
	}

	if ( nMaxChars < 1 )
		return false;

	// Skip leading whitespace in text mode
	if ( IsText() )
	{
		EatWhiteSpace();
	}

	// Remember, this *includes* the null character
	// It will be 0, however, if the buffer is empty.
	int nLen = PeekStringLength();

	if ( nLen == 0 )
	{
		*pString = 0;
		m_Error |= GET_OVERFLOW;
		return false;
	}
	
	// Strip off the terminating NULL
	if ( nLen <= nMaxChars )
	{
		Get( pString, nLen - 1 );
		pString[ nLen - 1 ] = 0;
	}
	else
	{
		Get( pString, nMaxChars - 1 );
		pString[ nMaxChars - 1 ] = 0;
		SeekGet( SEEK_CURRENT, nLen - 1 - nMaxChars );
		// we've had to truncate, read out but return false
		return false;
	}

	// Read the terminating NULL in binary formats
	if ( !IsText() )
	{
		char c = GetChar();
		Assert( c == 0 );
	}
	return true;
}

//-----------------------------------------------------------------------------
// Reads a CRLF/LF terminated string line 
//-----------------------------------------------------------------------------
bool CUtlBuffer::GetLine( char *pString, int nMaxChars )
{
	*pString = 0;

	if ( !IsValid() || !IsText() )
		return false;
	
	if ( nMaxChars < 1 )
		return false;

	// Skip leading whitespace
	EatWhiteSpace();

	int nMaxPeekAmount = nMaxChars - 1;

	if ( !CheckArbitraryPeekGet( 0, nMaxPeekAmount ) )
		return false;

	const char *pBuffer = (const char*) PeekGet();
	int nSkipChars = 0;

	while( pBuffer && ( nSkipChars < nMaxPeekAmount ) )
	{
		char c = *pBuffer;

		nSkipChars++;

		// stop on LF or end of string
		if ( c=='\n' || c == 0 )
			break;

		// copy char but skip CRs
		if ( c != '\r' )
		{
			*pString = c;
			pString++;
		}

		pBuffer++;
	}

	*pString = 0;

	SeekGet( SEEK_CURRENT, nSkipChars );

	return true;
}

//-----------------------------------------------------------------------------
// This version of GetString converts \ to \\ and " to \", etc.
// It also places " at the beginning and end of the string
//-----------------------------------------------------------------------------
char CUtlBuffer::GetDelimitedCharInternal( CUtlCharConversion *pConv )
{
	char c = GetChar();
	if ( c == pConv->GetEscapeChar() )
	{
		int nLength = pConv->MaxConversionLength();
		if ( !CheckArbitraryPeekGet( 0, nLength ) )
			return '\0';

		c = pConv->FindConversion( (const char *)PeekGet(), &nLength );
		SeekGet( SEEK_CURRENT, nLength );
	}

	return c;
}

char CUtlBuffer::GetDelimitedChar( CUtlCharConversion *pConv )
{
	if ( !IsText() || !pConv )
		return GetChar( );
	return GetDelimitedCharInternal( pConv );
}

void CUtlBuffer::GetDelimitedString( CUtlCharConversion *pConv, char *pString, int nMaxChars )
{
	if ( !IsText() || !pConv )
	{
		GetString( pString, nMaxChars );
		return;
	}

	if (!IsValid())
	{
		*pString = 0;
		return;
	}

	if ( nMaxChars == 0 )
	{
		nMaxChars = INT_MAX;
	}

	// this will fire if, for example, you're trying to use a static utlcharconversion
	// from a static constructor which runs before the utlcharconversion is constructed
	Assert( pConv && pConv->GetDelimiterLength() > 0 );


	EatWhiteSpace();
	if ( !PeekStringMatch( 0, pConv->GetDelimiter(), pConv->GetDelimiterLength() ) )
		return;

	// Pull off the starting delimiter
	SeekGet( SEEK_CURRENT, pConv->GetDelimiterLength() );

	int nRead = 0;
	while ( IsValid() )
	{
		if ( PeekStringMatch( 0, pConv->GetDelimiter(), pConv->GetDelimiterLength() ) )
		{
			SeekGet( SEEK_CURRENT, pConv->GetDelimiterLength() );
			break;
		}

		char c = GetDelimitedCharInternal( pConv );

		if ( nRead < nMaxChars )
		{
			pString[nRead] = c;
			++nRead;
		}
	}

	if ( nRead >= nMaxChars )
	{
		nRead = nMaxChars - 1;
	}
	pString[nRead] = '\0';
}


//-----------------------------------------------------------------------------
// Checks if a get is ok
//-----------------------------------------------------------------------------
bool CUtlBuffer::CheckGet( int nSize )
{
	if ( nSize < 0 )
		return false;

	if ( m_Error & GET_OVERFLOW )
		return false;

	if ( TellMaxPut() < m_Get + nSize )
	{
		m_Error |= GET_OVERFLOW;
		return false;
	}

	if ( ( m_Get < 0 ) || (	m_Memory.NumAllocated() < m_Get + nSize ) )
	{
		if ( !OnGetOverflow( nSize ) )
		{
			m_Error |= GET_OVERFLOW;
			return false;
		}
	}

	return true;
}


//-----------------------------------------------------------------------------
// Checks if a peek get is ok
//-----------------------------------------------------------------------------
bool CUtlBuffer::CheckPeekGet( int nOffset, int nSize )
{
	if ( m_Error & GET_OVERFLOW )
		return false;

	// Checking for peek can't set the overflow flag
	bool bOk = CheckGet( nOffset + nSize );
	m_Error &= ~GET_OVERFLOW;
	return bOk;
}


//-----------------------------------------------------------------------------
// Call this to peek arbitrarily long into memory. It doesn't fail unless
// it can't read *anything* new
//-----------------------------------------------------------------------------
bool CUtlBuffer::CheckArbitraryPeekGet( int nOffset, int &nIncrement )
{
	if ( TellGet() + nOffset >= TellMaxPut() )
	{
		nIncrement = 0;
		return false;
	}

	if ( TellGet() + nOffset + nIncrement > TellMaxPut() )
	{
		nIncrement = TellMaxPut() - TellGet() - nOffset;
	}

	// NOTE: CheckPeekGet could modify TellMaxPut for streaming files
	// We have to call TellMaxPut again here
	CheckPeekGet( nOffset, nIncrement );
	int nMaxGet = TellMaxPut() - TellGet();
	if ( nMaxGet < nIncrement )
	{
		nIncrement = nMaxGet;
	}
	return (nIncrement != 0);
}


//-----------------------------------------------------------------------------
// Peek part of the butt
//-----------------------------------------------------------------------------
const void* CUtlBuffer::PeekGet( int nMaxSize, int nOffset )
{
	if ( !CheckPeekGet( nOffset, nMaxSize ) )
		return NULL;
	return &m_Memory[ m_Get + nOffset ];
}


//-----------------------------------------------------------------------------
// Change where I'm reading
//-----------------------------------------------------------------------------
bool CUtlBuffer::SeekGet( SeekType_t type, int offset )	
{
	switch( type )
	{
	case SEEK_HEAD:						 
		m_Get = offset; 
		break;

	case SEEK_CURRENT:
		m_Get += offset;
		break;

	case SEEK_TAIL:
		m_Get = m_nMaxPut - offset;
		break;
	}

	if ( m_Get > m_nMaxPut )
	{
		m_Error |= GET_OVERFLOW;
		return false;
	}
	else
	{
		m_Error &= ~GET_OVERFLOW;
		return true;
	}
}


//-----------------------------------------------------------------------------
// Parse...
//-----------------------------------------------------------------------------

int CUtlBuffer::VaScanf( const char* pFmt, va_list list )
{
	Assert( pFmt );
	if ( m_Error || !IsText() )
		return 0;
	
	int numScanned = 0;
	int nLength;
	char c;
	char* pEnd;
	while ( (c = *pFmt++) )
	{
		// Stop if we hit the end of the buffer
		if ( m_Get >= TellMaxPut() )
		{
			m_Error |= GET_OVERFLOW;
			break;
		}

		switch (c)
		{
		case ' ':
			// eat all whitespace
			EatWhiteSpace();
			break;

		case '%':
			{
				// Conversion character... try to convert baby!
				char type = *pFmt++;
				if (type == 0)
					return numScanned;

				switch(type)
				{
				case 'c':
					{
						char* ch = va_arg( list, char * );
						if ( CheckPeekGet( 0, sizeof(char) ) )
						{
							*ch = *(const char*)PeekGet();
							++m_Get;
						}
						else
						{
							*ch = 0;
							return numScanned;
						}
					}
				break;

				case 'i':
				case 'd':
					{
						int* i = va_arg( list, int * );

						// NOTE: This is not bullet-proof; it assumes numbers are < 128 characters
						nLength = 128;
						if ( !CheckArbitraryPeekGet( 0, nLength ) )
						{
							*i = 0;
							return numScanned;
						}

						*i = strtol( (char*)PeekGet(), &pEnd, 10 );
						int nBytesRead = (int)( pEnd - (char*)PeekGet() );
						if ( nBytesRead == 0 )
							return numScanned;
						m_Get += nBytesRead;
					}
					break;
				
				case 'x':
					{
						int* i = va_arg( list, int * );

						// NOTE: This is not bullet-proof; it assumes numbers are < 128 characters
						nLength = 128;
						if ( !CheckArbitraryPeekGet( 0, nLength ) )
						{
							*i = 0;
							return numScanned;
						}

						*i = strtol( (char*)PeekGet(), &pEnd, 16 );
						int nBytesRead = (int)( pEnd - (char*)PeekGet() );
						if ( nBytesRead == 0 )
							return numScanned;
						m_Get += nBytesRead;
					}
					break;
					
				case 'u':
					{
						unsigned int* u = va_arg( list, unsigned int *);

						// NOTE: This is not bullet-proof; it assumes numbers are < 128 characters
						nLength = 128;
						if ( !CheckArbitraryPeekGet( 0, nLength ) )
						{
							*u = 0;
							return numScanned;
						}

						*u = strtoul( (char*)PeekGet(), &pEnd, 10 );
						int nBytesRead = (int)( pEnd - (char*)PeekGet() );
						if ( nBytesRead == 0 )
							return numScanned;
						m_Get += nBytesRead;
					}
					break;
					
				case 'f':
					{
						float* f = va_arg( list, float *);

						// NOTE: This is not bullet-proof; it assumes numbers are < 128 characters
						nLength = 128;
						if ( !CheckArbitraryPeekGet( 0, nLength ) )
						{
							*f = 0.0f;
							return numScanned;
						}

						*f = (float)strtod( (char*)PeekGet(), &pEnd );
						int nBytesRead = (int)( pEnd - (char*)PeekGet() );
						if ( nBytesRead == 0 )
							return numScanned;
						m_Get += nBytesRead;
					}
					break;
					
				case 's':
					{
						char* s = va_arg( list, char * );
						GetString( s, INT_MAX );
					}
					break;

				default:
					{
						// unimplemented scanf type
						Assert(0);
						return numScanned;
					}
				}

				++numScanned;
			}
			break;

		default:
			{
				// Here we have to match the format string character
				// against what's in the buffer or we're done.
				if ( !CheckPeekGet( 0, sizeof(char) ) )
					return numScanned;

				if ( c != *(const char*)PeekGet() )
					return numScanned;

				++m_Get;
			}
		}
	}
	return numScanned;
}

int CUtlBuffer::Scanf( const char* pFmt, ... )
{
	va_list args;

	va_start( args, pFmt );
	int count = VaScanf( pFmt, args );
	va_end( args );

	return count;
}


//-----------------------------------------------------------------------------
// Advance the get index until after the particular string is found
// Do not eat whitespace before starting. Return false if it failed
//-----------------------------------------------------------------------------
bool CUtlBuffer::GetToken( const char *pToken )
{
	Assert( pToken );

	// Look for the token
	int nLen = V_strlen( pToken );

	int nSizeToCheck = SizeAllocated() - TellGet();

	int nGet = TellGet();
	do
	{
		int nMaxSize = TellMaxPut() - TellGet();
		if ( nMaxSize < nSizeToCheck )
		{
			nSizeToCheck = nMaxSize;
		}
		if ( nLen > nSizeToCheck )
			break;

		if ( !CheckPeekGet( 0, nSizeToCheck ) )
			break;

		const char *pBufStart = (const char*)PeekGet();
		const char *pFoundEnd = V_strnistr( pBufStart, pToken, nSizeToCheck );
		if ( pFoundEnd )
		{
			size_t nOffset = (size_t)pFoundEnd - (size_t)pBufStart;
			SeekGet( CUtlBuffer::SEEK_CURRENT, static_cast<int>( nOffset + nLen ) );
			return true;
		}

		SeekGet( CUtlBuffer::SEEK_CURRENT, nSizeToCheck - nLen + 1);
		nSizeToCheck = SizeAllocated() - nLen + 1;

	} while ( true );

	SeekGet( CUtlBuffer::SEEK_HEAD, nGet );
	return false;
}


//-----------------------------------------------------------------------------
// (For text buffers only)
// Parse a token from the buffer:
// Grab all text that lies between a starting delimiter + ending delimiter
// (skipping whitespace that leads + trails both delimiters).
// Note the delimiter checks are case-insensitive.
// If successful, the get index is advanced and the function returns true,
// otherwise the index is not advanced and the function returns false.
//-----------------------------------------------------------------------------
bool CUtlBuffer::ParseToken( const char *pStartingDelim, const char *pEndingDelim, char* pString, int nMaxLen )
{
	int nCharsToCopy = 0;
	int nCurrentGet = 0;
	int nTokenStart = 0;

	size_t nEndingDelimLen;

	// Starting delimiter is optional
	char emptyBuf = '\0';
	if ( !pStartingDelim )
	{
		pStartingDelim = &emptyBuf;
	}

	// Ending delimiter is not
	Assert( pEndingDelim && pEndingDelim[0] );
	nEndingDelimLen = V_strlen( pEndingDelim );

	int nStartGet = TellGet();
	EatWhiteSpace( );
	while ( *pStartingDelim )
	{
		char nCurrChar = *pStartingDelim++;
		if ( !isspace(nCurrChar) )
		{
			if ( tolower( GetChar() ) != tolower( nCurrChar ) )
				goto parseFailed;
		}
		else
		{
			EatWhiteSpace();
		}
	}

	EatWhiteSpace();
	nTokenStart = TellGet();
	if ( !GetToken( pEndingDelim ) )
		goto parseFailed;

	nCurrentGet = TellGet();
	nCharsToCopy = static_cast<int>( (nCurrentGet - nEndingDelimLen) - nTokenStart );
	if ( nCharsToCopy >= nMaxLen )
	{
		nCharsToCopy = nMaxLen - 1;
	}

	if ( nCharsToCopy > 0 )
	{
		SeekGet( CUtlBuffer::SEEK_HEAD, nTokenStart );
		Get( pString, nCharsToCopy );
		if ( !IsValid() )
			goto parseFailed;

		// Eat trailing whitespace
		for ( ; nCharsToCopy > 0; --nCharsToCopy )
		{
			if ( !isspace( pString[ nCharsToCopy-1 ] ) )
				break;
		}
	}
	
	if ( nCharsToCopy >= 0 )
		 pString[ nCharsToCopy ] = '\0';

	// Advance the Get index
	SeekGet( CUtlBuffer::SEEK_HEAD, nCurrentGet );
	return nCharsToCopy > 0;

parseFailed:
	// Revert the get index
	SeekGet( SEEK_HEAD, nStartGet );
	pString[0] = '\0';
	return false;
}


//-----------------------------------------------------------------------------
// Serialization
//-----------------------------------------------------------------------------
void CUtlBuffer::Put( const void *pMem, int size )
{
	if ( size > 0 && CheckPut( size ) )
	{
		if ( pMem != &m_Memory[m_Put] )
			memcpy( &m_Memory[m_Put], pMem, size );
		m_Put += size;

		AddNullTermination();
	}
}


//-----------------------------------------------------------------------------
// Writes a null-terminated string
//-----------------------------------------------------------------------------
void CUtlBuffer::PutString( const char* pString )
{
	if (!IsText())
	{
		if ( pString )
		{
			// Not text? append a null at the end.
			int nLen = V_strlen( pString ) + 1;
			Put( pString, nLen * sizeof(char) );
			return;
		}
		else
		{
			PUT_BIN_DATA(char, 0);
		}
	}
	else if (pString)
	{
		int nTabCount = ( m_Flags & AUTO_TABS_DISABLED ) ? 0 : m_nTab;
		if ( nTabCount > 0 )
		{
			if ( WasLastCharacterCR() )
			{
				PutTabs();
			}

			const char* pEndl = strchr( pString, '\n' );
			while ( pEndl )
			{
				size_t nSize = (size_t)pEndl - (size_t)pString + sizeof(char);
				Put( pString, static_cast<int>( nSize ) );
				pString = pEndl + 1;
				if ( *pString )
				{
					PutTabs();
					pEndl = strchr( pString, '\n' );
				}
				else
				{
					pEndl = NULL;
				}
			}
		}
		size_t nLen = V_strlen( pString );
		if ( nLen )
		{
			Put( pString, static_cast<int>( nLen * sizeof(char) ) );
		}
	}
}


//-----------------------------------------------------------------------------
// This version of PutString never appends a null, normal PutString does in binary buffers
//-----------------------------------------------------------------------------
void CUtlBuffer::PutStringWithoutNull( const char* pString )
{
	// Not text? append a null at the end.
	int nLen = V_strlen( pString );
	Put( pString, nLen * sizeof( char ) );
}

//-----------------------------------------------------------------------------
// This version of PutString converts \ to \\ and " to \", etc.
// It also places " at the beginning and end of the string
//-----------------------------------------------------------------------------
inline void CUtlBuffer::PutDelimitedCharInternal( CUtlCharConversion *pConv, char c )
{
	int l = pConv->GetConversionLength( c );
	if ( l == 0 )
	{
		PutChar( c );
	}
	else
	{
		PutChar( pConv->GetEscapeChar() );
		Put( pConv->GetConversionString( c ), l );
	}
}

void CUtlBuffer::PutDelimitedChar( CUtlCharConversion *pConv, char c )
{
	if ( !IsText() || !pConv )
	{
		PutChar( c );
		return;
	}

	PutDelimitedCharInternal( pConv, c );
}

void CUtlBuffer::PutDelimitedString( CUtlCharConversion *pConv, const char *pString )
{
	if ( !IsText() || !pConv )
	{
		PutString( pString );
		return;
	}

	if ( WasLastCharacterCR() )
	{
		PutTabs();
	}
	Put( pConv->GetDelimiter(), pConv->GetDelimiterLength() );

	int nLen = pString ? V_strlen( pString ) : 0;
	for ( int i = 0; i < nLen; ++i )
	{
		PutDelimitedCharInternal( pConv, pString[i] );
	}

	if ( WasLastCharacterCR() )
	{
		PutTabs();
	}
	Put( pConv->GetDelimiter(), pConv->GetDelimiterLength() );
}


void CUtlBuffer::VaPrintf( const char* pFmt, va_list list )
{
	char temp[2048];
#ifdef _DEBUG	
	int nLen = 
#endif
		V_vsnprintf( temp, sizeof( temp ), pFmt, list );
#ifdef _DEBUG	
	Assert( nLen < 2048 );
#endif
	PutString( temp );
}

void CUtlBuffer::Printf( const char* pFmt, ... )
{
	va_list args;

	va_start( args, pFmt );
	VaPrintf( pFmt, args );
	va_end( args );
}


//-----------------------------------------------------------------------------
// Calls the overflow functions
//-----------------------------------------------------------------------------
void CUtlBuffer::SetOverflowFuncs( UtlBufferOverflowFunc_t getFunc, UtlBufferOverflowFunc_t putFunc )
{
	m_GetOverflowFunc = getFunc;
	m_PutOverflowFunc = putFunc;
}

	
//-----------------------------------------------------------------------------
// Calls the overflow functions
//-----------------------------------------------------------------------------
bool CUtlBuffer::OnPutOverflow( int nSize )
{
	return (this->*m_PutOverflowFunc)( nSize );
}

bool CUtlBuffer::OnGetOverflow( int nSize )
{
	return (this->*m_GetOverflowFunc)( nSize );
}

	
//-----------------------------------------------------------------------------
// Checks if a put is ok
//-----------------------------------------------------------------------------
bool CUtlBuffer::PutOverflow( int nSize )
{
	if ( m_Memory.IsExternallyAllocated() )
	{
		if ( !IsGrowable() )
			return false;

		m_Memory.ConvertToGrowableMemory( 0 );
	}

	int nGrowDelta = (m_Put + nSize) - m_Memory.NumAllocated();

	if ( nGrowDelta >  0 )
	{
		m_Memory.Grow( nGrowDelta );
	}
		
	return true;
}

bool CUtlBuffer::GetOverflow( int nSize )
{
	return false;
}
	

//-----------------------------------------------------------------------------
// Checks if a put is ok
//-----------------------------------------------------------------------------
bool CUtlBuffer::CheckPut( int nSize )
{
	Assert( nSize >= 0 );
	if ( ( m_Error & PUT_OVERFLOW ) || IsReadOnly() || nSize < 0 )
		return false;

	Assert( m_Put >= 0 );
	if ( nSize <= m_Memory.NumAllocated() - m_Put )
		return true;

	if ( OnPutOverflow( nSize ) )
		return true;

	m_Error |= PUT_OVERFLOW;
	return false;
}

void CUtlBuffer::SeekPut( SeekType_t type, int offset )	
{
	switch( type )
	{
	case SEEK_HEAD:						 
		Assert( offset >= 0 );
		m_Put = offset;
		break;

	case SEEK_CURRENT:
		Assert( offset >= -m_Put && offset <= INT_MAX-m_Put );
		m_Put += offset;
		break;

	case SEEK_TAIL:
		Assert( offset != INT_MIN && offset <= m_nMaxPut && -offset <= INT_MAX-m_nMaxPut );
		m_Put = m_nMaxPut - offset;
		break;
	}

	AddNullTermination();
}


//-----------------------------------------------------------------------------
// null terminate the buffer
//-----------------------------------------------------------------------------
void CUtlBuffer::AddNullTermination( void )
{
	Assert( m_Put >= 0 );
	if ( m_Put > m_nMaxPut )
	{
		if ( !IsReadOnly() && ((m_Error & PUT_OVERFLOW) == 0) && IsText()  )
		{
			// Add null termination value
			if ( CheckPut( 1 ) )
			{
				m_Memory[m_Put] = 0;
			}
			else
			{
				// Restore the overflow state, it was valid before...
				m_Error &= ~PUT_OVERFLOW;
			}
		}
		m_nMaxPut = m_Put;
	}		
}


//-----------------------------------------------------------------------------
// Converts a buffer from a CRLF buffer to a CR buffer (and back)
// Returns false if no conversion was necessary (and outBuf is left untouched)
// If the conversion occurs, outBuf will be cleared.
//-----------------------------------------------------------------------------
bool CUtlBuffer::ConvertCRLF( CUtlBuffer &outBuf )
{
	if ( !IsText() || !outBuf.IsText() )
		return false;

	if ( ContainsCRLF() == outBuf.ContainsCRLF() )
		return false;

	int nInCount = TellMaxPut();

	outBuf.Purge();
	outBuf.EnsureCapacity( nInCount );

	bool bFromCRLF = ContainsCRLF();

	// Start reading from the beginning
	int nGet = TellGet();
	int nPut = TellPut();
	int nGetDelta = 0;
	int nPutDelta = 0;

	const char *pBase = (const char*)Base();
	int nCurrGet = 0;
	while ( nCurrGet < nInCount )
	{
		const char *pCurr = &pBase[nCurrGet];
		if ( bFromCRLF )
		{
			const char *pNext = V_strnistr( pCurr, "\r\n", nInCount - nCurrGet );
			if ( !pNext )
			{
				outBuf.Put( pCurr, nInCount - nCurrGet );
				break;
			}

			int nBytes = static_cast<int>( (size_t)pNext - (size_t)pCurr );
			outBuf.Put( pCurr, nBytes );
			outBuf.PutChar( '\n' );
			nCurrGet += nBytes + 2;
			if ( nGet >= nCurrGet - 1 )
			{
				--nGetDelta;
			}
			if ( nPut >= nCurrGet - 1 )
			{
				--nPutDelta;
			}
		}
		else
		{
			const char *pNext = V_strnchr( pCurr, '\n', nInCount - nCurrGet );
			if ( !pNext )
			{
				outBuf.Put( pCurr, nInCount - nCurrGet );
				break;
			}

			int nBytes = static_cast<int>( (size_t)pNext - (size_t)pCurr );
			outBuf.Put( pCurr, nBytes );
			outBuf.PutChar( '\r' );
			outBuf.PutChar( '\n' );
			nCurrGet += nBytes + 1;
			if ( nGet >= nCurrGet )
			{
				++nGetDelta;
			}
			if ( nPut >= nCurrGet )
			{
				++nPutDelta;
			}
		}
	}

	Assert(	nPut + nPutDelta <= outBuf.TellMaxPut() );

	outBuf.SeekGet( SEEK_HEAD, nGet + nGetDelta ); 
	outBuf.SeekPut( SEEK_HEAD, nPut + nPutDelta ); 

	return true;
}


//-----------------------------------------------------------------------------
// Fast swap
//-----------------------------------------------------------------------------
void CUtlBuffer::Swap( CUtlBuffer &buf )
{
	SWAP( m_Get, buf.m_Get );
	SWAP( m_Put, buf.m_Put );
	SWAP( m_nMaxPut, buf.m_nMaxPut );
	SWAP( m_Error, buf.m_Error );
	m_Memory.Swap( buf.m_Memory );
}


//-----------------------------------------------------------------------------
// Take ownership of mem from a CUtlMemory
//-----------------------------------------------------------------------------
void CUtlBuffer::TakeOwnershipOfMemory( CUtlMemory<uint8> &mem )
{
	m_Get = 0;
	m_Put = mem.Count();
	m_nMaxPut = mem.Count();
	m_Error = 0;
	m_Memory.Swap( mem );
	mem.Purge();
}


//-----------------------------------------------------------------------------
// Release our memory to a CUtlMemory
// The punCurrentPut parameter is because that information (how much of the memory
// allocated has been written to) is lost when transferring into a CUtlMemory
//-----------------------------------------------------------------------------
void CUtlBuffer::ReleaseToMemory( CUtlMemory<uint8> &mem, int *punCurrentPut )
{
	*punCurrentPut = m_Put;
	m_Memory.Swap( mem );
	Purge();
}



//-----------------------------------------------------------------------------
// memory access
//-----------------------------------------------------------------------------
void *CUtlBuffer::DetachAndClear()
{
	void *pubData = m_Memory.Detach();

	m_Get = 0;
	m_Put = 0;
	m_Error = 0;
	m_nMaxPut = -1;
	AddNullTermination();

	return pubData;
}


void CUtlBuffer::PutChar( char c )
{
	if ( WasLastCharacterCR() )
	{
		PutTabs();
	}

	PUT_BIN_DATA( char, c );
}

void CUtlBuffer::PutUint8( uint8 ub )
{
	PUT_TYPE( uint8, ub );
}

void CUtlBuffer::PutUnsignedInt64( uint64 ub )
{
	PUT_TYPE( uint64, ub );
}

void CUtlBuffer::PutInt64( int64 ub )
{
	PUT_TYPE( int64, ub );
}

void CUtlBuffer::PutInt16( int16 s16 )
{
	PUT_TYPE( int16, s16 );
}

void  CUtlBuffer::PutShort( short s )
{
	PUT_TYPE( short, s );
}

void CUtlBuffer::PutUnsignedShort( unsigned short s )
{
	PUT_TYPE( unsigned short, s );
}

void CUtlBuffer::PutInt( int i )
{
	PUT_TYPE( int, i );
}

void CUtlBuffer::PutUnsignedInt( unsigned int u )
{
	PUT_TYPE( unsigned int, u );
}

void CUtlBuffer::PutFloat( float f )
{
	PUT_TYPE( float, f );
}

void CUtlBuffer::PutDouble( double d )
{
	PUT_TYPE( double, d );
}

char CUtlBuffer::GetChar()
{
	char c = 0;
	if ( CheckGet( 1 ) ) // sets get overflow error bit on failure
	{
		c = *(char*) PeekGet();
		m_Get += 1;
	}
	return c;
}

uint8 CUtlBuffer::GetUint8()
{
	// %u Scanf writes to a 32-bit number
	uint32 ub;
	GET_TYPE( uint8, ub, "%u" );
	return (uint8)ub;
}

uint64 CUtlBuffer::GetUnsignedInt64()
{
	uint64 ub;
	GET_TYPE( uint64, ub, "%llu" );
	return ub;
}

int64 CUtlBuffer::GetInt64()
{
	int64 ub;
	GET_TYPE( int64, ub, "%lld" );
	return ub;
}

int16 CUtlBuffer::GetInt16()
{
	// %d Scanf writes to a 32-bit number
	int32 s16;
	GET_TYPE( int16, s16, "%d" );
	return (int16)s16;
}

short CUtlBuffer::GetShort()
{
	// %d Scanf writes to a 32-bit number
	int32 s;
	GET_TYPE( short, s, "%d" );
	return (short)s;
}

unsigned short CUtlBuffer::GetUnsignedShort()
{
	uint32 s;
	GET_TYPE( unsigned short, s, "%u" );
	return (unsigned short)s;
}

int CUtlBuffer::GetInt()
{
	int i;
	GET_TYPE( int, i, "%d" );
	return i;
}

int CUtlBuffer::GetIntHex()
{
	int i;
	GET_TYPE( int, i, "%x" );
	return i;
}

unsigned int CUtlBuffer::GetUnsignedInt()
{
	unsigned int u;
	GET_TYPE( unsigned int, u, "%u" );
	return u;
}

float CUtlBuffer::GetFloat()
{
	float f;
	GET_TYPE( float, f, "%f" );
	return f;
}

double CUtlBuffer::GetDouble()
{
	double d;
	GET_TYPE( double, d, "%f" );
	return d;
}