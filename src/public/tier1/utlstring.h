//====== Copyright (c) 1996-2005, Valve Corporation, All rights reserved. =====
//
// Purpose: 
//
//=============================================================================

#ifndef UTLSTRING_H
#define UTLSTRING_H
#ifdef _WIN32
#pragma once
#endif

#include "tier0/dbg.h"
#include "tier0/validator.h"
#include "tier0/platform.h"
#include "vstdlib/strtools.h"
#include "tier1/generichash.h"


#include "tier0/memdbgon.h"

class CUtlString;
class CUtlStringBuilder;

//
// Maximum number of allowable characters in a CUtlString.
// V_strlen and its ilk are currently coded to return an unsigned int, but
// in many places in the code it gets treated as a signed int
//
const uint k_cchMaxString = 0x7fff0000;


//-----------------------------------------------------------------------------
// Purpose: simple wrapper class around a char *
//			relies on the small-block heap existing for efficient memory allocation
//			as compact as possible, no virtuals or extraneous data
//			to be used primarily to replace of char array buffers
//			tries to match CUtlSymbol interface wherever possible
//-----------------------------------------------------------------------------
class CUtlString
{
public:
	CUtlString();
	CUtlString( const char *pchString );
	CUtlString( CUtlString const &string );
	explicit CUtlString( size_t nPreallocateBytes );
	~CUtlString();

	// operator=
	CUtlString &operator=( CUtlString const &src );
	CUtlString &operator=( const char *pchString );

	// operator==
	bool operator==( CUtlString const &src ) const;
	bool operator==( const char *pchString ) const;
	
	// operator!=
	bool operator!=( CUtlString const &src ) const;
	bool operator!=( const char *pchString ) const;

	// operator </>, performs case sensitive comparison
	bool operator<( const CUtlString &val ) const;
	bool operator<( const char *pchString ) const;
	bool operator>( const CUtlString &val ) const;
	bool operator>( const char *pchString ) const;

	// operator+=
	CUtlString &operator+=( const char *rhs );

	// is valid?
	bool IsValid() const;

	// gets the string
	// never returns NULL, use IsValid() to see if it's never been set
	const char *String() const;
	const char *Get() const { return String(); }
	operator const char *() const { return String(); }

	// returns the string directly (could be NULL)
	// useful for doing inline operations on the string
	char *Access() { return m_pchString; }
	
	// If you want to take ownership of the ptr, you can use this.
	char *DetachRawPtr() { char *psz = m_pchString; m_pchString = NULL; return psz; }

	// append in-place, causing a re-allocation
	void Append( const char *pchAddition );
	void Append( const char *pchAddition, size_t cbLen );

	// append in-place for a single or repeated run of characters
	void AppendChar( char ch ) { Append( &ch, 1 ); }
	void AppendRepeat( char ch, int cCount );

	// sets the string
	void SetValue( const char *pchString );
	void Set( const char *pchString );
	void Clear() { SetValue( NULL ); }
	void SetPtr( char *pchString );
	void Swap( CUtlString &src );
	void Swap( CUtlStringBuilder &src );

	void ToLower();
	void ToUpper();

	void Wipe();

	// Set directly and don't look for a null terminator in pValue.
	void SetDirect( const char *pValue, size_t nChars );

	// Get the length of the string in characters.
	uint32 Length() const;
	bool IsEmpty() const;

	// Format like sprintf.
	size_t Format( PRINTF_FORMAT_STRING const char *pFormat, ... ) FMTFUNCTION( 2, 3 );

	// format, then append what we crated in the format
	size_t AppendFormat( PRINTF_FORMAT_STRING const char *pFormat, ... ) FMTFUNCTION( 2, 3 );

	// convert bytes to hex string and append
	void AppendHex( const uint8 *pbInput, size_t cubInput, bool bLowercase = true );

	// replace a single character with another, returns hit count
	size_t Replace( char chTarget, char chReplacement );

	// replace a string with another string, returns hit count
	// replacement string might be NULL or "" to remove target substring
	size_t Replace( const char *pstrTarget, const char *pstrReplacement );
	size_t ReplaceCaseless( const char *pstrTarget, const char *pstrReplacement );

	ptrdiff_t IndexOf( const char *pstrTarget ) const;
	bool BEndsWith( const char *pchString ) const;
	bool BEndsWithCaseless( const char *pchString ) const;
	bool BStartsWith( const char *pchString ) const;
	bool BStartsWithCaseless( const char *pchString ) const;

	// remove whitespace from the string; anything that is isspace()
	size_t RemoveWhitespace( );

	// trim whitespace from the beginning and end of the string
	size_t TrimWhitespace();

	// trim whitespace from the end of the string
	size_t TrimTrailingWhitespace();

	void SecureZero() { SecureZeroMemory( m_pchString, Length() ); }

#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName ) const;	// validate our internal structures
#endif // DBGFLAG_VALIDATE

	size_t FormatV( const char *pFormat, va_list args );
	size_t VAppendFormat( const char *pFormat, va_list args );
	void Truncate( size_t nChars );

	// Both TruncateUTF8 methods guarantee truncation of the string to a length less-than-or-equal-to the 
	// specified number of bytes or characters.  Both return false and truncate early if invalid UTF8 sequences 
	// are encountered before the cap is reached.
	// As a result, the string is guaranteed to be valid UTF-8 upon completion of the operation.
	bool TruncateUTF8Bytes( size_t unMaxBytes ) { return TruncateUTF8Internal( (size_t)-1, unMaxBytes ); }
	bool TruncateUTF8Chars( size_t unMaxChars ) { return TruncateUTF8Internal( unMaxChars, (size_t)-1 ); }

#ifdef VALVE_RVALUE_REFS
	// Move construction, rvalue assignment from like type
	CUtlString( CUtlString &&string ) : m_pchString( string.m_pchString ) { string.m_pchString = NULL; }
	CUtlString &operator=( CUtlString &&string ) { Swap( string ); return *this; }
	void Set( CUtlString &&string ) { Swap( string ); }

	// Move construction, rvalue assignment from CUtlStringBuilder
	explicit CUtlString( CUtlStringBuilder &&string );
	CUtlString &operator=( CUtlStringBuilder &&string );
#endif

private:
	bool TruncateUTF8Internal( size_t unMaxChars, size_t unMaxBytes );
	size_t ReplaceInternal( const char *pstrTarget, const char *pstrReplacement, const char *pfnCompare(const char*, const char*) );
	static void AssertStringTooLong();
	char *m_pchString;
};


//-----------------------------------------------------------------------------
// Purpose: constructor
//-----------------------------------------------------------------------------
inline CUtlString::CUtlString() : m_pchString( NULL )
{
}


//-----------------------------------------------------------------------------
// Purpose: constructor
//-----------------------------------------------------------------------------
inline CUtlString::CUtlString( size_t nPreallocateBytes ) 
{
	if ( nPreallocateBytes > 0 )
	{
		if ( nPreallocateBytes > k_cchMaxString )
			AssertStringTooLong();
		m_pchString = (char*) PvAlloc( nPreallocateBytes );
		m_pchString[0] = 0;
	}
	else
	{
		m_pchString = NULL;
	}
}


//-----------------------------------------------------------------------------
// Purpose: constructor
//-----------------------------------------------------------------------------
inline CUtlString::CUtlString( const char *pchString ) : m_pchString( NULL )
{
	SetValue( pchString );
}


//-----------------------------------------------------------------------------
// Purpose: constructor
//-----------------------------------------------------------------------------
inline CUtlString::CUtlString( CUtlString const &string ) : m_pchString( NULL )
{
	SetValue( string.String() );
}


//-----------------------------------------------------------------------------
// Purpose: destructor
//-----------------------------------------------------------------------------
inline CUtlString::~CUtlString()
{
	FreePv( m_pchString );
}


//-----------------------------------------------------------------------------
// Purpose: ask if the string has anything in it
//-----------------------------------------------------------------------------
inline bool CUtlString::IsEmpty() const
{
	if ( m_pchString == NULL )
		return true;

	return m_pchString[0] == 0;
}


//-----------------------------------------------------------------------------
// Purpose: assignment
//-----------------------------------------------------------------------------
inline CUtlString &CUtlString::operator=( const char *pchString )
{
	SetValue( pchString );
	return *this;
}


//-----------------------------------------------------------------------------
// Purpose: assignment
//-----------------------------------------------------------------------------
inline CUtlString &CUtlString::operator=( CUtlString const &src )
{
	SetValue( src.String() );
	return *this;
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlString::operator==( CUtlString const &src ) const
{
	return !V_strcmp( String(), src.String() );
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlString::operator==( const char *pchString ) const
{
	return !V_strcmp( String(), pchString );
}

//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlString::operator!=( CUtlString const &src ) const
{
	return !( *this == src );
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlString::operator!=( const char *pchString ) const
{
	return !( *this == pchString );
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlString::operator<( CUtlString const &val ) const
{
	return operator<( val.String() );
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlString::operator<( const char *pchString ) const
{
	return V_strcmp( String(), pchString ) < 0;
}

//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlString::operator>( CUtlString const &val ) const
{
	return V_strcmp( String(), val.String() ) > 0;
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlString::operator>( const char *pchString ) const
{
	return V_strcmp( String(), pchString ) > 0;
}


//-----------------------------------------------------------------------------
// Return a string with this string and rhs joined together.
inline CUtlString& CUtlString::operator+=( const char *rhs )
{
	Append( rhs );
	return *this;
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string is not null
//-----------------------------------------------------------------------------
inline bool CUtlString::IsValid() const
{
	return ( m_pchString != NULL );
}


//-----------------------------------------------------------------------------
// Purpose: data accessor
//-----------------------------------------------------------------------------
inline const char *CUtlString::String() const
{
	return m_pchString ? m_pchString : "";
}


//-----------------------------------------------------------------------------
// Purpose: Sets the string to be the new value, taking a copy of it
//-----------------------------------------------------------------------------
inline void CUtlString::SetValue( const char *pchString )
{
	if ( m_pchString != pchString )
	{
		FreePv( m_pchString );

		if ( pchString && *pchString )
		{
			size_t nLength = 1 + strlen( pchString );
			if ( nLength > k_cchMaxString )
				AssertStringTooLong( );
			m_pchString = (char*)PvAlloc( nLength );
			V_memcpy( m_pchString, pchString, nLength );
		}
		else
		{
			m_pchString = NULL;
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Converts the string to lower case in-place. Not necessarily clean
//          about all possibly localization issues.
//-----------------------------------------------------------------------------
inline void CUtlString::ToLower()
{
	if ( m_pchString != NULL )
	{
		for ( int i = 0; m_pchString[i]; i++ )
		{
			m_pchString[i] = (char)tolower( (int)(unsigned char)m_pchString[i] );
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: Converts the string to upper case in-place. Not necessarily clean
//          about all possibly localization issues.
//-----------------------------------------------------------------------------
inline void CUtlString::ToUpper()
{
	if ( m_pchString != NULL )
	{
		for ( int i = 0; m_pchString[i]; i++ )
		{
			m_pchString[i] = ( char )toupper( ( int )( unsigned char )m_pchString[i] );
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: Clear the string from memory, then free it.
//-----------------------------------------------------------------------------
inline void CUtlString::Wipe()
{
	//
	// Overwrite the current buffer
	//
	if ( m_pchString )
	{
		SecureZeroMemory( m_pchString, V_strlen( m_pchString ) );
	}
	SetValue( NULL );
}


//-----------------------------------------------------------------------------
// Purpose: Set directly and don't look for a null terminator in pValue.
//-----------------------------------------------------------------------------
inline void CUtlString::SetDirect( const char *pValue, size_t nChars )
{
	FreePv( m_pchString );
	m_pchString = NULL;

	if ( nChars > 0 )
	{
		if ( nChars + 1 > k_cchMaxString )
			AssertStringTooLong();
		m_pchString = (char*)PvAlloc( nChars + 1 );
		V_memcpy( m_pchString, pValue, nChars );
		m_pchString[nChars] = 0;
	}
}


//-----------------------------------------------------------------------------
// Purpose: Sets the string to be the new value, taking a copy of it
//-----------------------------------------------------------------------------
inline void CUtlString::Set( const char *pchString )
{
	SetValue( pchString );
}


//-----------------------------------------------------------------------------
// Purpose: Sets the string to be the new value, taking ownership of the pointer
//-----------------------------------------------------------------------------
inline void CUtlString::SetPtr( char *pchString )
{
	FreePv( m_pchString );
	m_pchString = pchString;
}


inline uint32 CUtlString::Length() const
{
	if ( !m_pchString )
		return 0;

	return (uint32) V_strlen( m_pchString );
}


//-----------------------------------------------------------------------------
// Purpose: format something sprintf() style, and take it as the new value of this CUtlString
//-----------------------------------------------------------------------------
inline size_t CUtlString::Format( const char *pFormat, ... )
{
	va_list args;
	va_start( args, pFormat );
	size_t len = FormatV( pFormat, args );
	va_end( args );
	return len;
}

//-----------------------------------------------------------------------------
// format a string and append the result to the string we hold
//-----------------------------------------------------------------------------
inline size_t CUtlString::AppendFormat( const char *pFormat, ... )
{
	va_list args;
	va_start( args, pFormat );
	size_t len = VAppendFormat( pFormat, args );
	va_end( args );
	return len;
}

//-----------------------------------------------------------------------------
// Purpose: concatenate the provided string to our current content
//-----------------------------------------------------------------------------
inline void CUtlString::Append( const char *pchAddition )
{
	if ( pchAddition && pchAddition[0] )
	{
		size_t cchLen = V_strlen( pchAddition );
		Append( pchAddition, cchLen );
	}
}


//-----------------------------------------------------------------------------
// Purpose: concatenate the provided string to our current content
//			when the additional string length is known
//-----------------------------------------------------------------------------
inline void CUtlString::Append( const char *pchAddition, size_t cbLen )
{
	if ( m_pchString == NULL )
	{
		SetDirect( pchAddition, cbLen );
	}
	else if ( pchAddition && pchAddition[0] )
	{
		size_t cbOld = V_strlen( m_pchString );
		if ( 1 + cbOld + cbLen > k_cchMaxString )
			AssertStringTooLong();
		char *pstrNew = (char *) PvAlloc( 1 + cbOld + cbLen );
		
		V_memcpy( pstrNew, m_pchString, cbOld );
		V_memcpy( pstrNew + cbOld, pchAddition, cbLen );
		pstrNew[ cbOld + cbLen ] = '\0';

		FreePv( m_pchString );
		m_pchString = pstrNew;
	}
}


//-----------------------------------------------------------------------------
// Purpose: repeat the passed character a specified number of times and
//			concatenate those characters to our existing content
//-----------------------------------------------------------------------------
inline void CUtlString::AppendRepeat( char ch, int cCount )
{
	if ( m_pchString == NULL )
	{
		if ( cCount + 1 > k_cchMaxString )
			AssertStringTooLong();
		char *pchNew = (char *) PvAlloc( cCount + 1 );
		for ( int n = 0; n < cCount; n++ )
			pchNew[n] = ch;
		pchNew[cCount] = 0;
		m_pchString = pchNew;
	}
	else
	{
		size_t cbOld = strlen( m_pchString );
		if ( 1 + cbOld + cCount > k_cchMaxString )
			AssertStringTooLong();
		char *pchNew = (char *) PvAlloc( 1 + cbOld + cCount );

		V_memcpy( pchNew, m_pchString, cbOld );
		for ( int n = 0; n < cCount; n++ )
			pchNew[n + cbOld] = ch;
		pchNew[cCount + cbOld] = 0;

		FreePv( m_pchString );
		m_pchString = pchNew;
	}
}


//-----------------------------------------------------------------------------
// Purpose: Swaps string contents
//-----------------------------------------------------------------------------
inline void CUtlString::Swap( CUtlString &src )
{
	char *tmp = src.m_pchString;
	src.m_pchString = m_pchString;
	m_pchString = tmp;
}


//-----------------------------------------------------------------------------
// Purpose: replace all occurrences of one character with another
//-----------------------------------------------------------------------------
inline size_t CUtlString::Replace( char chTarget, char chReplacement )
{
	size_t cReplacements = 0;

	if ( m_pchString != NULL )
	{
		for ( char *pstrWalker = m_pchString; *pstrWalker != 0; pstrWalker++ )
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
inline void CUtlString::Truncate( size_t nChars )
{
	if ( !m_pchString )
		return;

	size_t nLen = V_strlen( m_pchString );
	if ( nLen <= nChars )
		return;

	m_pchString[nChars] = '\0';
}


//-----------------------------------------------------------------------------
// Data and memory validation
//-----------------------------------------------------------------------------
#ifdef DBGFLAG_VALIDATE
inline void CUtlString::Validate( CValidator &validator, const char *pchName ) const
{
#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), (void*) this, pchName );
#else
	validator.Push( typeid(*this).name(), (void*) this, pchName );
#endif

	if ( NULL != m_pchString )
		validator.ClaimMemory( m_pchString );

	validator.Pop();
}
#endif // DBGFLAG_VALIDATE


//-----------------------------------------------------------------------------
// Purpose: Case insensitive CUtlString comparison
//-----------------------------------------------------------------------------
class CDefCaselessUtlStringEquals
{
public:
	CDefCaselessUtlStringEquals() {}
	CDefCaselessUtlStringEquals( int i ) {}
	inline bool operator()( const CUtlString &lhs, const CUtlString &rhs ) const
	{ 
		return ( V_stricmp( lhs.String(), rhs.String() ) == 0 );
	}
	inline bool operator!() const { return false; }
};
class CDefCaselessUtlStringLess
{
public:
	CDefCaselessUtlStringLess() {}
	CDefCaselessUtlStringLess( int i ) {}
	inline bool operator()( const CUtlString &lhs, const CUtlString &rhs ) const
	{ 
		return ( V_stricmp( lhs.String(), rhs.String() ) < 0 );
	}
	inline bool operator!() const { return false; }
};

// Hash specialization for CUtlStrings
template<>
struct HashFunctor<CUtlString>
{
	uint32 operator()(const CUtlString &strKey) const
	{
		return HashString( strKey.String() );
	}
};

struct HashFunctorUtlStringCaseless
{
	typedef uint32 TargetType;
	TargetType operator()(const CUtlString &strKey) const
	{
		return HashStringCaseless( strKey.String() );
	}
};

// HashItem() overload that works automatically with our hash containers
template<>
inline uint32 HashItem( const CUtlString &item )
{
	return HashString( item );
}


//-----------------------------------------------------------------------------
// Purpose: General purpose string class good for when it
//			is rarely expected to be empty, and/or will undergo
//			many modifications/appends.
//-----------------------------------------------------------------------------
class CUtlStringBuilder
{
public:
	CUtlStringBuilder();
	CUtlStringBuilder( const char *pchString );
	CUtlStringBuilder( CUtlStringBuilder const &string );
	explicit CUtlStringBuilder( size_t nPreallocateBytes );

	~CUtlStringBuilder();

#ifdef VALVE_RVALUE_REFS
	// Create a new CUtlString by concatenating any number of arguments. This will call the
	// appropriate += operator if one exists.
	template < typename ...Types >
	static CUtlString Concat( Types&&... args )
	{
		CUtlStringBuilder sBuilder;
		ConcatHelper( sBuilder, std::forward<Types>( args )... );
		return sBuilder.DetachString();
	}
#endif // VALVE_RVALUE_REFS

	// operator=
	CUtlStringBuilder &operator=( const CUtlStringBuilder &src );
	CUtlStringBuilder &operator=( const char *pchString );

	// operator==
	bool operator==( CUtlStringBuilder const &src ) const;
	bool operator==( const char *pchString ) const;

	// operator!=
	bool operator!=( CUtlStringBuilder const &src ) const;
	bool operator!=( const char *pchString ) const;

	// operator </>, performs case sensitive comparison
	bool operator<( const CUtlStringBuilder &val ) const;
	bool operator<( const char *pchString ) const;
	bool operator>( const CUtlStringBuilder &val ) const;
	bool operator>( const char *pchString ) const;

	// operator+=
	CUtlStringBuilder &operator+=( const char *rhs );

	// is valid?
	bool IsValid() const;

	// gets the string
	// never returns NULL, use IsValid() to see if it's never been set
	const char *String() const;
	const char *Get() const { return String(); }
	operator const char *() const { return String(); }

	// returns the string directly (could be NULL)
	// useful for doing inline operations on the string
	char *Access() 
	{
		// aggressive warning that there has been a bad error;
		// probably should not be retrieving the string by pointer.
		Assert(!m_data.HasError());

		if ( !IsValid() || ( Capacity() == 0 ) ) 
			return NULL;

		return m_data.Access(); 
	}

	// return false if capacity can't be set. If false is returned,
	// the error state is set.
	bool EnsureCapacity( size_t nLength );

	// append in-place, causing a re-allocation
	void Append( const char *pchAddition );
	void Append( const char *pchAddition, size_t cbLen );
	void Append( const CUtlStringBuilder &str ) { Append( str.String(), str.Length() ); }
	void AppendChar( char ch ) { Append( &ch, 1 ); }
	void AppendRepeat( char ch, int cCount );

	// sets the string
	void SetValue( const char *pchString );
	void Set( const char *pchString );
	void Clear() { m_data.Clear(); }
	void SetPtr( char *pchString );
	void SetPtr( char *pchString, size_t nLength );
	void Swap( CUtlStringBuilder &src );
	void Swap( CUtlString &src );

	// Set directly and don't look for a null terminator in pValue.
	// nChars is the string length. "abcd" nChars==3 would copy and null
	// terminate "abc" in the string object.
	void SetDirect( const char *pchString, size_t nChars );

	// Get the length of the string in characters.
	uint32 Length() const;
	bool IsEmpty() const;
	size_t Capacity() const { return m_data.Capacity(); } // how much room is there to scribble

	// Format like sprintf.
	size_t Format( PRINTF_FORMAT_STRING const char *pFormat, ... ) FMTFUNCTION( 2, 3 );
	// format, then append what we crated in the format
	size_t AppendFormat( PRINTF_FORMAT_STRING const char *pFormat, ... ) FMTFUNCTION( 2, 3 );

	// convert bytes to hex string and append
	void AppendHex( const uint8 *pbInput, size_t cubInput, bool bLowercase = true );

	// replace a single character with another, returns hit count
	size_t Replace( char chTarget, char chReplacement );

	// replace a string with another string, returns hit count
	// replacement string might be NULL or "" to remove target substring
	size_t Replace( const char *pstrTarget, const char *pstrReplacement );
	size_t ReplaceCaseless( const char *pstrTarget, const char *pstrReplacement );

	ptrdiff_t IndexOf( const char *pstrTarget ) const;
	bool BEndsWith( const char *pchString ) const;
	bool BEndsWithCaseless( const char *pchString ) const;
	bool BStartsWith( const char *pchString ) const;
	bool BStartsWithCaseless( const char *pchString ) const;

	// remove whitespace from the string; anything that is isspace()
	size_t RemoveWhitespace( );

	// trim whitespace from the beginning and end of the string
	size_t TrimWhitespace( );

	// trim whitespace from the end of the string
	size_t TrimTrailingWhitespace();

	// Allows setting the size to anything under the current
	// capacity.  Typically should not be used unless there was a specific
	// reason to scribble on the string. Will not touch the string contents,
	// but will append a NULL. Returns true if the length was changed.
	bool SetLength( size_t nLen );

	// Transfer ownership to a PvAlloc-allocated string, clearing internal contents.
	char *DetachRawPtr( size_t *pnLen = NULL, size_t *pnCapacity = NULL );
	
	// Transfer ownership to an anonymous CUtlString, clearing internal contents.
	// Can be efficiently implemented by compilers that support C++11 rvalue refs.
	CUtlString DetachString();

	// For operations that are long and/or complex - if something fails
	// along the way, the error will be set and can be queried at the end.
	// The string is undefined in the error state, but will likely hold the
	// last value before the error occurred.  The string is cleared
	// if ClearError() is called.  The error can be set be the user, and it
	// will also be set if a dynamic allocation fails in string operations
	// where it needs to grow the capacity.
	void SetError()			{ m_data.SetError(true); }
	void ClearError()		{ m_data.ClearError(); }
	bool HasError() const	{ return m_data.HasError(); }

#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName ) const;	// validate our internal structures
#endif // DBGFLAG_VALIDATE

	size_t VFormat( const char *pFormat, va_list args );
	size_t VAppendFormat( const char *pFormat, va_list args );
	void Truncate( size_t nChars );

	// Access() With no assertion check - should only be used for tests
	char *AccessNoAssert() 
	{
		if ( !IsValid() ) 
			return NULL;
		return m_data.Access(); 
	}

	// SetError() With no assertion check - should only be used for tests
	void SetErrorNoAssert() { m_data.SetError(false); }

#ifdef VALVE_RVALUE_REFS
	// Move construction, rvalue assignment from same type
	CUtlStringBuilder( CUtlStringBuilder &&string ) { m_data.Raw = string.m_data.Raw; string.m_data.Construct(); }
	CUtlStringBuilder &operator=( CUtlStringBuilder &&string ) { Swap( string ); return *this; }
	void Set( CUtlStringBuilder &&string ) { Swap( string ); }

	// Move construction, rvalue assignment from CUtlString
	explicit CUtlStringBuilder( CUtlString &&string ) { m_data.Construct(); Swap( string ); }
	CUtlStringBuilder &operator=( CUtlString &&string ) { Swap( string ); return *this; }
	void Set( CUtlString &&string ) { Swap( string ); }
#endif

private:
#ifdef VALVE_RVALUE_REFS
	// Append one element to our string builder at a time. Supports any argument type that CUtlStringBuilder +=
	// supports.
	template < typename FirstType, typename ...Types >
	static void ConcatHelper( CUtlStringBuilder& sBuilder, FirstType&& arg, Types&&... args )
	{
		sBuilder += arg;
		return ConcatHelper( sBuilder, std::forward<Types>( args )... );
	}

	// Empty recursive end for when we run out of varargs.
	static void ConcatHelper( CUtlStringBuilder& sBuilder ) { }
#endif // VALVE_RVALUE_REFS

	size_t ReplaceInternal( const char *pstrTarget, const char *pstrReplacement, const char *pfnCompare(const char*, const char*)  );
	operator bool () const	{ return IsValid(); }

	// nChars is the number of characters you want, NOT including the null
	char *PrepareBuffer( size_t nChars, bool bCopyOld = false, size_t nMinCapacity = 0 )
	{
		char *pszString = NULL;
		size_t nCapacity = m_data.Capacity();
		if ( ( nChars <= nCapacity ) && ( nMinCapacity <= nCapacity ) )
		{
			// early out leaving it all alone, just update the length,
			// even if it shortens an existing heap string to a width
			// that would fit in the stack buffer.
			pszString = m_data.SetLength( nChars );

			// SetLength will have added the null. Pointer might
			// be NULL if there is an error state and no buffer
			Assert( !pszString ||  pszString[nChars] == '\0' );

			return pszString;
		}

		if ( HasError() )
			return NULL;

		// Need to actually adjust the capacity
		return InternalPrepareBuffer( nChars, bCopyOld, Max( nChars, nMinCapacity ) );
	}

	char *InternalPrepareBuffer( size_t nChars, bool bCopyOld, size_t nMinCapacity );

	enum 
	{  
		// These are set as bitflags in the sentinel byte (aka space-remaining byte) of Data
		STRING_TYPE_SENTINEL = 0x80,
		STRING_TYPE_ERROR = 0x40,

		// The max length of a string changes depending on 32- or 64-bit struct definitions
		MAX_STACK_STRLEN = ( sizeof(void*) == 4 ? 15 : 23 )
	};

	union Data
	{
		struct _Raw
		{
			// 16 bytes on 32-bit, 24 bytes on 64-bit
			uintp m_raw[ sizeof(void*) == 4 ? 4 : 3 ];
		} Raw;

		struct _Heap
		{	//  sentinel == 0xff if Heap is the active union item
		private:
			char *m_pchString;
			uint32 m_nLength;
			uint32 m_nCapacity; // without trailing null; ie: m_pchString[m_nCapacity] = '\0' is not out of bounds
			uint8  scrap[sizeof(void*)-1];
			uint8  sentinel;
		public:
			friend union Data;
			friend char *CUtlStringBuilder::InternalPrepareBuffer(size_t, bool, size_t);
		} Heap;

		struct _Stack
		{
		private:
			// last byte is doing a hack double duty.  It holds how many bytes 
			// are left in the string; so when the string is 'full' it will be
			// '0' and thus suffice as the terminating null.  This is why
			// we hold remaining chars instead of 'string length'
			char m_szString[MAX_STACK_STRLEN+1];
		public:
			uint8 BytesLeft() const { return (uint8)(m_szString[MAX_STACK_STRLEN]); }
			void SetBytesLeft( char n ) { m_szString[MAX_STACK_STRLEN] = n; }

			friend char *CUtlStringBuilder::InternalPrepareBuffer(size_t, bool, size_t);
			friend union Data;
		} Stack;

		// set to a clear state without looking at the current state
		void Construct() 
		{
			Stack.m_szString[0] = '\0'; 
			Stack.SetBytesLeft( MAX_STACK_STRLEN ); 
		}

		// If we have heap allocated data, free it
		void FreeHeap()
		{
			if ( IsHeap() ) 
				FreePv( Heap.m_pchString );
		}

		// Back to a clean state, but retain the error state.
		void Clear() 
		{
			if ( HasError() )
				return;

			FreeHeap();
			Heap.m_pchString = NULL;

			Construct();
		}

		bool IsHeap() const { return ( (Heap.sentinel & STRING_TYPE_SENTINEL) != 0 ); }

		char *Access() { return IsHeap() ? Heap.m_pchString : Stack.m_szString; }
		const char *String() const { return IsHeap() ? Heap.m_pchString : Stack.m_szString; }

		size_t Length() const { return IsHeap() ? Heap.m_nLength : ( (size_t)MAX_STACK_STRLEN - Stack.BytesLeft() ); }
		bool IsEmpty() const 
		{
			if ( IsHeap() )
				return Heap.m_nLength == 0;
			else
				return Stack.BytesLeft() == (uint8)MAX_STACK_STRLEN; // empty if all the bytes are available
		}

		size_t Capacity() const { return IsHeap() ? Heap.m_nCapacity : (size_t)MAX_STACK_STRLEN; }

		// Internally the code often needs the char * after setting the length, so just return it
		// from here for conveniences. Returns NULL if nChars is invalid (larger than capacity).
		char *SetLength( size_t nChars );

		// Give the string away and set to an empty state
		char *DetachHeapString( size_t &nLen, size_t &nCapacity );

		void SetPtr( char *pchString, size_t nLength );

		// Set the string to an error state
		void SetError( bool bEnableAssert );

		// clear the error state and reset the string
		void ClearError();

		// If string is in the heap and the error bit is set in the sentinel
		// the error state is true.
		bool HasError() const { return IsHeap() && ( (Heap.sentinel & STRING_TYPE_ERROR) != 0 ); }

		// If it's stack based, get it to the heap and return if it is
		// successfully on the heap (or already was)
		bool MoveToHeap();

	private:
		//-----------------------------------------------------------------------------
		// Purpose: Needed facts for string class to work
		//-----------------------------------------------------------------------------
		void StaticAssertTests()
		{
			// If this fails when the heap sentinel and where the stack string stores its bytes left
			// aren't aliases.  This is needed so that regardless of how the 'sentinel' to mark
			// that the string is on the heap is set, it is set as expected on both sides of the union.
			COMPILE_TIME_ASSERT( offsetof( _Heap, sentinel ) == (offsetof( _Stack, m_szString ) + MAX_STACK_STRLEN) );
			COMPILE_TIME_ASSERT( sizeof( _Heap ) == sizeof( _Stack ) );
			COMPILE_TIME_ASSERT( sizeof( _Stack ) == sizeof( _Raw ) );

			// Lots of code assumes it can look at m_data.Stack.m_nBytesLeft for an empty string; which
			// means that it will equal MAX_STACK_STRLEN.  Therefor it must be a different value than
			// the STRING_TYPE_SENTINEL which will be set if the string is in the heap.
			COMPILE_TIME_ASSERT( MAX_STACK_STRLEN < STRING_TYPE_SENTINEL );
			COMPILE_TIME_ASSERT( MAX_STACK_STRLEN < STRING_TYPE_ERROR );

			// this is a no brainer, and I don't know anywhere in the world this isn't true,
			// but this code does take this dependency.
			COMPILE_TIME_ASSERT( 0 == '\0');
		}
	};

private: // data
	Data m_data;

	friend class CUtlString;
};

//-----------------------------------------------------------------------------
// Purpose: constructor
//-----------------------------------------------------------------------------
inline CUtlStringBuilder::CUtlStringBuilder()
{
	m_data.Construct();
}


//-----------------------------------------------------------------------------
// Purpose: constructor
//-----------------------------------------------------------------------------
inline CUtlStringBuilder::CUtlStringBuilder( size_t nPreallocateBytes ) 
{
	if ( nPreallocateBytes <= MAX_STACK_STRLEN )
	{
		m_data.Construct();
	}
	else
	{
		m_data.Construct();
		PrepareBuffer( 0, false, nPreallocateBytes );
	}
}


//-----------------------------------------------------------------------------
// Purpose: constructor
//-----------------------------------------------------------------------------
inline CUtlStringBuilder::CUtlStringBuilder( const char *pchString )
{
	m_data.Construct();
	SetDirect( pchString, pchString ? strlen( pchString ) : 0 );
}


//-----------------------------------------------------------------------------
// Purpose: constructor
//-----------------------------------------------------------------------------
inline CUtlStringBuilder::CUtlStringBuilder( CUtlStringBuilder const &string )
{
	m_data.Construct();
	SetDirect( string.String(), string.Length() );

	// attempt the copy before checking for error. On the off chance there
	// is data there that can be set, it will help with debugging.
	if ( string.HasError() )
		m_data.SetError( false );
}


//-----------------------------------------------------------------------------
// Purpose: destructor
//-----------------------------------------------------------------------------
inline CUtlStringBuilder::~CUtlStringBuilder()
{
	m_data.FreeHeap();
}


//-----------------------------------------------------------------------------
// Purpose: Pre-Widen a string to an expected length
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::EnsureCapacity( size_t nLength )
{
	return PrepareBuffer( Length(), true, nLength ) != NULL;
}


//-----------------------------------------------------------------------------
// Purpose: ask if the string has anything in it
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::IsEmpty() const
{
	return m_data.IsEmpty();
}

//-----------------------------------------------------------------------------
// Purpose: assignment
//-----------------------------------------------------------------------------
inline CUtlStringBuilder &CUtlStringBuilder::operator=( const char *pchString )
{
	SetDirect( pchString, pchString ? strlen( pchString ) : 0 );
	return *this;
}

//-----------------------------------------------------------------------------
// Purpose: assignment
//-----------------------------------------------------------------------------
inline CUtlStringBuilder &CUtlStringBuilder::operator=( CUtlStringBuilder const &src )
{
	if ( &src != this )
	{
		SetDirect( src.String(), src.Length() );
		// error propagate
		if ( src.HasError() )
			m_data.SetError( false );
	}

	return *this;
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::operator==( CUtlStringBuilder const &src ) const
{
	return !V_strcmp( String(), src.String() );
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::operator==( const char *pchString ) const
{
	return !V_strcmp( String(), pchString );
}

//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::operator!=( CUtlStringBuilder const &src ) const
{
	return !( *this == src );
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::operator!=( const char *pchString ) const
{
	return !( *this == pchString );
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::operator<( CUtlStringBuilder const &val ) const
{
	return operator<( val.String() );
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::operator<( const char *pchString ) const
{
	return V_strcmp( String(), pchString ) < 0;
}

//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::operator>( CUtlStringBuilder const &val ) const
{
	return V_strcmp( String(), val.String() ) > 0;
}


//-----------------------------------------------------------------------------
// Purpose: comparison
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::operator>( const char *pchString ) const
{
	return V_strcmp( String(), pchString ) > 0;
}


//-----------------------------------------------------------------------------
// Return a string with this string and rhs joined together.
inline CUtlStringBuilder& CUtlStringBuilder::operator+=( const char *rhs )
{
	Append( rhs );
	return *this;
}


//-----------------------------------------------------------------------------
// Purpose: returns true if the string is not null
//-----------------------------------------------------------------------------
inline bool CUtlStringBuilder::IsValid() const
{
	return !HasError();
}


//-----------------------------------------------------------------------------
// Purpose: data accessor
//-----------------------------------------------------------------------------
inline const char *CUtlStringBuilder::String() const
{
	const char *pszString = m_data.String();
	if ( pszString )
		return pszString;

	// pszString can be NULL in the error state. For const char*
	// never return NULL.
	return "";
}


//-----------------------------------------------------------------------------
// Purpose: Sets the string to be the new value, taking a copy of it
//-----------------------------------------------------------------------------
inline void CUtlStringBuilder::SetValue( const char *pchString )
{
	size_t nLen = ( pchString ? strlen( pchString ) : 0 );
	SetDirect( pchString, nLen );
}


//-----------------------------------------------------------------------------
// Purpose: Set directly and don't look for a null terminator in pValue.
//-----------------------------------------------------------------------------
inline void CUtlStringBuilder::SetDirect( const char *pchSource, size_t nChars )
{
	if ( HasError() )
		return;

	if ( m_data.IsHeap() && Get() == pchSource )
		return;

	if ( !pchSource || !nChars )
	{
		m_data.Clear();
		return;
	}

	char *pszString = PrepareBuffer( nChars );

	if ( pszString )
	{
		V_memcpy( pszString, pchSource, nChars );
		// PrepareBuffer already allocated space for the terminating null, 
		// and inserted it for us. Make sure we didn't clobber it.
		// Also assign it anyways so we don't risk the caller having a buffer
		// running into random bytes.
#ifdef _DEBUG
		// Suppress a bogus noisy warning:
		// warning C6385: Invalid data: accessing 'pszString', the readable size is 'nChars' bytes, but '1001' bytes might be read
		ANALYZE_SUPPRESS(6385);
		Assert( pszString[nChars] == '\0' );
		pszString[nChars] = '\0';
#endif
	}
}


//-----------------------------------------------------------------------------
// Purpose: Sets the string to be the new value, taking a copy of it
//-----------------------------------------------------------------------------
inline void CUtlStringBuilder::Set( const char *pchString )
{
	SetValue( pchString );
}


//-----------------------------------------------------------------------------
// Purpose: Sets the string to be the new value, taking ownership of the pointer
//-----------------------------------------------------------------------------
inline void CUtlStringBuilder::SetPtr( char *pchString )
{
	size_t nLength = pchString ? strlen( pchString ) : 0;
	SetPtr( pchString, nLength );
}

//-----------------------------------------------------------------------------
// Purpose: Sets the string to be the new value, taking ownership of the pointer
//			This API will clear the error state if it was set.
//-----------------------------------------------------------------------------
inline void CUtlStringBuilder::SetPtr( char *pchString, size_t nLength )
{
	m_data.Clear();

	if ( !pchString || !nLength )
	{
		FreePv( pchString ); // we don't hang onto empty strings.
		return;
	}

	m_data.SetPtr( pchString, nLength );
}

//-----------------------------------------------------------------------------
// Purpose: return the conceptual 'strlen' of the string.
//-----------------------------------------------------------------------------
inline uint32 CUtlStringBuilder::Length() const
{
	return (uint32)m_data.Length();
}


//-----------------------------------------------------------------------------
// Purpose: format something sprintf() style, and take it as the new value of this CUtlStringBuilder
//-----------------------------------------------------------------------------
inline size_t CUtlStringBuilder::Format( const char *pFormat, ... )
{
	va_list args;
	va_start( args, pFormat );
	size_t nLen = VFormat( pFormat, args );
	va_end( args );
	return nLen;
}


//-----------------------------------------------------------------------------
// Purpose: Helper for Format() method
//-----------------------------------------------------------------------------
inline size_t CUtlStringBuilder::VFormat( const char *pFormat, va_list args )
{
	if ( HasError() )
		return 0;

	int len = 0;
#ifdef _WIN32
	// how much space will we need?
	len = _vscprintf( pFormat, args );
#else

	// ISO spec defines the NULL/0 case as being valid and will return the
	// needed length. Verified on PS3 as well.  Ignore that bsd/linux/mac
	// have vasprintf which will allocate a buffer. We'd rather have the
	// self growing buffer management ourselves. Even the best implementations
	// There does not seem to be a magic vasprintf that is significantly
	// faster than 2 passes (some guess and get lucky).

    // Scope ReuseArgs.
    {
        CReuseVaList ReuseArgs( args );
        len = vsnprintf( NULL, 0, pFormat, ReuseArgs.m_ReuseList );
    }
#endif
	if ( len > 0 )
	{
		// get it
		char *pszString = PrepareBuffer( len, true );
		if ( pszString )
			len = _vsnprintf( pszString, len + 1, pFormat, args );
		else
			len = 0;
	}
	
	Assert( len > 0 || HasError() );
	return len;
}


//-----------------------------------------------------------------------------
// format a string and append the result to the string we hold
//-----------------------------------------------------------------------------
inline size_t CUtlStringBuilder::AppendFormat( const char *pFormat, ... )
{
	va_list args;
	va_start( args, pFormat );
	size_t nLen = VAppendFormat( pFormat, args );
	va_end( args );
	return nLen;
}



//-----------------------------------------------------------------------------
// Purpose: implementation helper for AppendFormat()
//-----------------------------------------------------------------------------
inline size_t CUtlStringBuilder::VAppendFormat( const char *pFormat, va_list args )
{
	if ( HasError() )
		return 0;

	int len = 0;
#ifdef _WIN32
	// how much space will we need?
	len = _vscprintf( pFormat, args );
#else

	// ISO spec defines the NULL/0 case as being valid and will return the
	// needed length. Verified on PS3 as well.  Ignore that bsd/linux/mac
	// have vasprintf which will allocate a buffer. We'd rather have the
	// self growing buffer management ourselves. Even the best implementations
	// There does not seem to be a magic vasprintf that is significantly
	// faster than 2 passes (some guess and get lucky).

    // Scope ReuseArgs.
    {
        CReuseVaList ReuseArgs( args );
        len = vsnprintf( NULL, 0, pFormat, ReuseArgs.m_ReuseList );
    }
#endif
	size_t nOldLen = Length();

	if ( len > 0 )
	{
		// get it

		char *pszString = PrepareBuffer( nOldLen + len, true );
		if ( pszString )
			len = _vsnprintf( &pszString[nOldLen], len + 1, pFormat, args );
		else
			len = 0;
	}

	Assert( len > 0 || HasError() );
	return nOldLen + len;
}

//-----------------------------------------------------------------------------
// Purpose: concatenate the provided string to our current content
//-----------------------------------------------------------------------------
inline void CUtlStringBuilder::Append( const char *pchAddition )
{
	if ( pchAddition && pchAddition[0] )
	{
		size_t cchLen = strlen( pchAddition );
		Append( pchAddition, cchLen );
	}
}


//-----------------------------------------------------------------------------
// Purpose: concatenate the provided string to our current content
//			when the additional string length is known
//-----------------------------------------------------------------------------
inline void CUtlStringBuilder::Append( const char *pchAddition, size_t cbLen )
{
	if ( pchAddition && cbLen )
	{
		if ( IsEmpty() )
		{
			SetDirect( pchAddition, cbLen );
		}
		else
		{
			size_t cbOld = Length();
			char *pstrNew = PrepareBuffer( cbOld + cbLen, true );

			if ( pstrNew )
				V_memcpy( pstrNew + cbOld, pchAddition, cbLen );
		}
	}
}


//-----------------------------------------------------------------------------
// Purpose: append a repeated series of a single character
//-----------------------------------------------------------------------------
inline void CUtlStringBuilder::AppendRepeat( char ch, int cCount )
{
	size_t cbOld = Length();
	char *pstrNew = PrepareBuffer( cbOld + cCount, true );
	if ( pstrNew )
		memset( pstrNew + cbOld, ch, cCount );
}


//-----------------------------------------------------------------------------
// Data and memory validation
//-----------------------------------------------------------------------------
#ifdef DBGFLAG_VALIDATE
inline void CUtlStringBuilder::Validate( CValidator &validator, const char *pchName ) const
{
#ifdef _WIN32
	validator.Push( typeid(*this).raw_name(), (void*) this, pchName );
#else
	validator.Push( typeid(*this).name(), (void*) this, pchName );
#endif

	if ( m_data.IsHeap() )
		validator.ClaimMemory( (void*) m_data.String() );

	validator.Pop();
}
#endif // DBGFLAG_VALIDATE


//-----------------------------------------------------------------------------



//-----------------------------------------------------------------------------
// The CUtlFmtString class:
// 		Simple helper class to create a format string from a constructor
//		placed in its own class to avoid ambiguous calls to constructor
//-----------------------------------------------------------------------------
class CUtlFmtString : public CUtlStringBuilder
{
public:
	CUtlFmtString( PRINTF_FORMAT_STRING const char *pchFormat, ... ) FMTFUNCTION( 2, 3 )
	{
		va_list args;
		va_start( args, pchFormat );
		VFormat( pchFormat, args );
		va_end( args );
	}
};


//-----------------------------------------------------------------------------
// Purpose: version of CUtlString that will zero it's memory on destruct
//-----------------------------------------------------------------------------
class CUtlStringAutoWipe : public CUtlString
{
public:
	CUtlStringAutoWipe() : CUtlString() {}
	CUtlStringAutoWipe( const char *pchString ) : CUtlString( pchString ) {}
	CUtlStringAutoWipe( CUtlString const &string ) : CUtlString( string ) {}
	~CUtlStringAutoWipe()
	{
		Wipe();
	}
};


//-----------------------------------------------------------------------------
// The CUtlStringWrap class:
// 		Transforms a const char* string into a const CUtlString object without
//		performing any buffer allocations or string copies. Can be passed
//		wherever a const CUtlString parameter is expected.
//-----------------------------------------------------------------------------
class CUtlStringWrap
{
public:
	explicit CUtlStringWrap( const char * pszString )
	{
		m_str.SetPtr( const_cast<char*>( pszString ) );
	}
	
	~CUtlStringWrap()
	{
		m_str.DetachRawPtr();
	}
	
	operator const CUtlString & () const
	{
		return m_str;
	}

private:
	CUtlString m_str;
};


//-----------------------------------------------------------------------------
// The CUtlAllocation class:
// A single allocation in the style of CUtlMemory/CUtlString/CUtlBuffer
//			as compact as possible, no virtuals or extraneous data
//			to be used primarily to replace CUtlBuffer
//-----------------------------------------------------------------------------
class CUtlAllocation
{
public:

	// constructor, destructor
	CUtlAllocation()			
	{ 
		m_pMemory = NULL;
	}
	CUtlAllocation( const void *pMemory, int cub )			
	{ 
		m_pMemory = NULL;
		Copy( pMemory, cub );
	}
	CUtlAllocation( CUtlAllocation const &src )
	{ 
		m_pMemory = NULL;
		Copy( src );
	}
	~CUtlAllocation()
	{
		Purge();
	}

	CUtlAllocation &operator=( CUtlAllocation const &src )
	{
		Copy( src );
		return *this;
	}

	bool operator==( CUtlAllocation const &src )
	{
		if ( Count() != src.Count() )
			return false;
		return V_memcmp( Base(), src.Base(), Count() ) == 0;
	}

	void Copy( const void *pMemory, int cub )
	{
		if ( cub == 0 || pMemory == NULL )
		{
			Purge();
			return;
		}
		if ( cub != Count() )
		{
			Purge();
			m_pMemory = (ActualMemory_t *)PvAlloc( cub + sizeof( int ) ); 										
			m_pMemory->cub = cub;
		}
		V_memcpy( Base(), pMemory, cub );
	}
	// Gets the base address
	uint8* Base()
	{ 
		if ( m_pMemory == NULL ) 
			return NULL; 
		return m_pMemory->rgub; 
	}
	const uint8* Base() const
	{ 
		if ( m_pMemory == NULL ) 
			return NULL; 
		return m_pMemory->rgub; 
	}

	// Size
	int Count() const
	{ 
		if ( m_pMemory == NULL ) 
			return 0; 
		return m_pMemory->cub; 
	}

	// Memory deallocation
	void Purge()											
	{ 
		FreePv( m_pMemory ); 
		m_pMemory = NULL; 
	}

	void Copy( const CUtlAllocation &alloc )
	{
		Copy( alloc.Base(), alloc.Count() );
	}

	void Swap( CUtlAllocation &alloc )
	{
		ActualMemory_t *pTemp = m_pMemory;
		m_pMemory = alloc.m_pMemory;
		alloc.m_pMemory = pTemp;
	}

	void Alloc( int cub )
	{
		Purge();
		m_pMemory = (ActualMemory_t *)PvAlloc( cub + sizeof( int ) ); 										
		m_pMemory->cub = cub;
	}

#ifdef DBGFLAG_VALIDATE
	void Validate( CValidator &validator, const char *pchName )
	{
		validator.Push( "CUtlAllocation", this, pchName );

		if ( NULL != m_pMemory )
			validator.ClaimMemory( m_pMemory );

		validator.Pop();
	}
#endif // DBGFLAG_VALIDATE

private:
	struct ActualMemory_t
	{
		int cub;
		uint8 rgub[4];	// i'd prefer to make this 0 but the compiler whines when i do
	};

	ActualMemory_t *m_pMemory;
};

// Hash specialization for CUtlStringBuilders
template<>
struct HashFunctor<CUtlStringBuilder>
{
	uint32 operator()(const CUtlStringBuilder &strKey) const
	{
		return HashString( strKey.String(), strKey.Length() );
	}
};

#endif // UTLSTRING_H
