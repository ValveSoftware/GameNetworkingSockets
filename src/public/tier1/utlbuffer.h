//====== Copyright 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose: 
//
// $NoKeywords: $
//
// Serialization/unserialization buffer
//=============================================================================//

#ifndef UTLBUFFER_H
#define UTLBUFFER_H

#ifdef _WIN32
#pragma once
#endif

#include "tier1/utlmemory.h"
#include <stdarg.h>
//SDR_PUBLIC #include "tier1/utlstring.h"

//-----------------------------------------------------------------------------
// Description of character conversions for string output
// Here's an example of how to use the macros to define a character conversion
// BEGIN_CHAR_CONVERSION( CStringConversion, '\\' )
//	{ '\n', "n" },
//	{ '\t', "t" }
// END_CHAR_CONVERSION( CStringConversion, '\\' )
//-----------------------------------------------------------------------------
class CUtlCharConversion
{
public:
	struct ConversionArray_t
	{
		char m_nActualChar;
		const char *m_pReplacementString;
	};

	CUtlCharConversion( const char nEscapeChar, const char *pDelimiter, int nCount, ConversionArray_t *pArray );
	char GetEscapeChar() const;
	const char *GetDelimiter() const;
	int GetDelimiterLength() const;

	const char *GetConversionString( char c ) const;
	int GetConversionLength( char c ) const;
	int MaxConversionLength() const;

	// Finds a conversion for the passed-in string, returns length
	virtual char FindConversion( const char *pString, int *pLength );

protected:
	struct ConversionInfo_t
	{
		int m_nLength;
		const char *m_pReplacementString;
	};

	char m_nEscapeChar;
	const char *m_pDelimiter;
	int m_nDelimiterLength;
	int m_nCount;
	int m_nMaxConversionLength;
	char m_pList[255];
	ConversionInfo_t m_pReplacements[255];
};

#define BEGIN_CHAR_CONVERSION( _name, _delimiter, _escapeChar )	\
	static CUtlCharConversion::ConversionArray_t s_pConversionArray ## _name[] = {

#define END_CHAR_CONVERSION( _name, _delimiter, _escapeChar ) \
	}; \
	CUtlCharConversion _name( _escapeChar, _delimiter, sizeof( s_pConversionArray ## _name ) / sizeof( CUtlCharConversion::ConversionArray_t ), s_pConversionArray ## _name );

#define BEGIN_CUSTOM_CHAR_CONVERSION( _className, _name, _delimiter, _escapeChar ) \
	static CUtlCharConversion::ConversionArray_t s_pConversionArray ## _name[] = {

#define END_CUSTOM_CHAR_CONVERSION( _className, _name, _delimiter, _escapeChar ) \
	}; \
	_className _name( _escapeChar, _delimiter, sizeof( s_pConversionArray ## _name ) / sizeof( CUtlCharConversion::ConversionArray_t ), s_pConversionArray ## _name );

//-----------------------------------------------------------------------------
// Character conversions for C strings
//-----------------------------------------------------------------------------
CUtlCharConversion *GetCStringCharConversion();

//-----------------------------------------------------------------------------
// Character conversions for quoted strings, with no escape sequences
//-----------------------------------------------------------------------------
CUtlCharConversion *GetNoEscCharConversion();


//-----------------------------------------------------------------------------
// Macro to set overflow functions easily
//-----------------------------------------------------------------------------
#define SetUtlBufferOverflowFuncs( _get, _put )	\
	SetOverflowFuncs( static_cast <UtlBufferOverflowFunc_t>( _get ), static_cast <UtlBufferOverflowFunc_t>( _put ) )


//-----------------------------------------------------------------------------
// Command parsing..
//-----------------------------------------------------------------------------
class CUtlBuffer
{
public:
	enum SeekType_t
	{
		SEEK_HEAD = 0,
		SEEK_CURRENT,
		SEEK_TAIL
	};

	// flags
	enum BufferFlags_t
	{
		TEXT_BUFFER = 0x1,			// Describes how get + put work (as strings, or binary)
		EXTERNAL_GROWABLE = 0x2,	// This is used w/ external buffers and causes the utlbuf to switch to reallocatable memory if an overflow happens when Putting.
		CONTAINS_CRLF = 0x4,		// For text buffers only, does this contain \n or \n\r?
		READ_ONLY = 0x8,			// For external buffers; prevents null termination from happening.
		AUTO_TABS_DISABLED = 0x10,	// Used to disable/enable push/pop tabs
		LITTLE_ENDIAN_BUFFER = 0x20,// ensures that data is stored in little endian format
		BIG_ENDIAN_BUFFER = 0x40,	// ensures that data is stored in big endian format
	};

	// Overflow functions when a get or put overflows
	typedef bool (CUtlBuffer::*UtlBufferOverflowFunc_t)( int nSize );

	// Constructors for growable + external buffers for serialization/unserialization
	CUtlBuffer( int growSize = 0, int initSize = 0, int nFlags = 0 );
	CUtlBuffer( const void* pBuffer, int size, int nFlags = 0 );

	unsigned char	GetFlags() const;

	// NOTE: This will assert if you attempt to recast it in a way that
	// is not compatible. The only valid conversion is binary-> text w/CRLF
	void			SetBufferType( bool bIsText, bool bContainsCRLF );

	// Makes sure we've got at least this much memory
	void			EnsureCapacity( int num );

	// Attaches the buffer to external memory....
	void			SetExternalBuffer( void* pMemory, int nSize, int nInitialPut, int nFlags = 0 );

	// Attaches to an external buffer as read only. Will purge any existing buffer data
	void			SetReadOnlyBuffer( void *pMemory, int nSize );

	// Controls endian-ness of binary utlbufs
	// Resets the buffer; but doesn't free memory
	void			Clear();

	// Clears out the buffer; frees memory
	void			Purge();

	void			Swap( CUtlBuffer &buf );
	void			TakeOwnershipOfMemory( CUtlMemory<uint8> &mem );
	void			ReleaseToMemory( CUtlMemory<uint8> &mem, int *punCurrentPut );

	// detaches the memory, returns it, and clears the members (but detached memory is preserved)
	void *			DetachAndClear();

	// copies data from another buffer
	void			CopyBuffer( const CUtlBuffer &buffer );
	void			CopyBuffer( const void *pubData, int cubData );


	// Read stuff out.
	// Binary mode: it'll just read the bits directly in, and characters will be
	//		read for strings until a null character is reached.
	// Text mode: it'll parse the file, turning text #s into real numbers.
	//		GetString will read a string until a space is reached

	char			GetChar();
	uint8			GetUint8();
	short			GetShort();
	unsigned short	GetUnsignedShort();
	int				GetInt();
	int				GetIntHex();
	unsigned int	GetUnsignedInt();
	int16			GetInt16();
	uint64			GetUnsignedInt64();
	int64			GetInt64();
	float			GetFloat();
	double			GetDouble();
	bool			GetString( char *pString, int nMaxLen );
	bool			GetLine( char *pString, int nMaxLen );
	const char*		GetStringFast(); // binary mode only
	bool			Get( void* pMem, int size );

	// This will get at least 1 byte and up to nSize bytes. 
	// It will return the number of bytes actually read.
	int				GetUpTo( void *pMem, int nSize );

	// This version of GetString converts \" to \\ and " to \, etc.
	// It also reads a " at the beginning and end of the string
	void			GetDelimitedString( CUtlCharConversion *pConv, char *pString, int nMaxChars );
	char			GetDelimitedChar( CUtlCharConversion *pConv );

	// This will return the # of characters of the string about to be read out
	// NOTE: The count will *include* the terminating 0!!
	// In binary mode, it's the number of characters until the next 0
	// In text mode, it's the number of characters until the next space.
	int				PeekStringLength();

	// This version of PeekStringLength converts \" to \\ and " to \, etc.
	// It also reads a " at the beginning and end of the string
	// NOTE: The count will *include* the terminating 0!!
	// In binary mode, it's the number of characters until the next 0
	// In text mode, it's the number of characters between "s (checking for \")
	// Specifying false for bActualSize will return the pre-translated number of characters
	// including the delimiters and the escape characters. So, \n counts as 2 characters when bActualSize == false
	// and only 1 character when bActualSize == true
	int				PeekDelimitedStringLength( CUtlCharConversion *pConv, bool bActualSize = true );

	// Just like scanf, but doesn't work in binary mode
	int				Scanf( SCANF_FORMAT_STRING const char* pFmt, ... );
	int				VaScanf( const char* pFmt, va_list list );

	// Eats white space, advances Get index
	void			EatWhiteSpace();
	// Eats white space, advances Get index - won't overflow if file ends with whitespace.
	void			EatWhiteSpaceNoOverflow();

	// Eats C++ style comments
	bool			EatCPPComment();

	// (For text buffers only)
	// Parse a token from the buffer:
	// Grab all text that lies between a starting delimiter + ending delimiter
	// (skipping whitespace that leads + trails both delimiters).
	// If successful, the get index is advanced and the function returns true,
	// otherwise the index is not advanced and the function returns false.
	bool			ParseToken( const char *pStartingDelim, const char *pEndingDelim, char* pString, int nMaxLen );

	// Advance the get index until after the particular string is found
	// Do not eat whitespace before starting. Return false if it failed
	// String test is case-insensitive.
	bool			GetToken( const char *pToken );

	// Write stuff in
	// Binary mode: it'll just write the bits directly in, and strings will be
	//		written with a null terminating character
	// Text mode: it'll convert the numbers to text versions
	//		PutString will not write a terminating character
	void			PutChar( char c );
	void			PutUint8( uint8 ub );
	void			PutShort( short s );
	void			PutUnsignedShort( unsigned short us );
	void			PutInt( int i );
	void			PutUnsignedInt( unsigned int u );
	void			PutInt16( int16 s16 );
	void			PutUnsignedInt64( uint64 u64 );
	void			PutInt64( int64 u64 );
	void			PutFloat( float f );
	void			PutDouble( double d );
	void			PutString( const char* pString );
	void			PutStringWithoutNull( const char* pString );
	void			Put( const void* pMem, int size );

	// This version of PutString converts \ to \\ and " to \", etc.
	// It also places " at the beginning and end of the string
	void			PutDelimitedString( CUtlCharConversion *pConv, const char *pString );
	void			PutDelimitedChar( CUtlCharConversion *pConv, char c );

	// Just like printf, writes a terminating zero in binary mode
	void			Printf( PRINTF_FORMAT_STRING const char* pFmt, ... ) FMTFUNCTION( 2, 3 );
	void			VaPrintf( const char* pFmt, va_list list );

	// What am I writing (put)/reading (get)?
	void* PeekPut( int offset = 0 );
	const void* PeekGet( ) const;
	const void* PeekGet( int offset ) const;
	const void* PeekGet( int nMaxSize, int nOffset );

	// Reserve at least nBytes and return the pointer to the start of the reserved area.
	// Like EnsureCapacity(TellPut()+nBytes) but non-exact; preserves geometric growth.
	void *ReservePut( int nBytes );

	// How many bytes remain to be read?
	// NOTE: This is not accurate for streaming text files; it overshoots
	int GetBytesRemaining() const;

	// Where am I writing (put)/reading (get)?	
	int  TellPut() const;
	int  TellGet() const;

	// Change where I'm writing (put)/reading (get)
	void SeekPut( SeekType_t type, int offset );
	bool SeekGet( SeekType_t type, int offset );

	// Buffer base
	const void* Base() const;
	void* Base();
	// Returns the base as a const char*, only valid in text mode.
	const char *String() const;
	// Copies the content of the buffer into a string (valid for binary and text mode).
	//SDR_PUBLIC void CopyToString( CUtlString &strText ) const;

	// memory allocation size, does *not* reflect size written or read,
	//	use TellPut or TellGet for that
	int Size() const; // FIXME will delete soon
	int SizeAllocated() const;

	// Am I a text buffer?
	bool IsText() const;

	// Am I externally allocated (may not be growable, check below)
	bool IsExternallyAllocated() const;

	// Can I grow if I'm externally allocated?
	bool IsGrowable() const;

	// Am I valid? (overflow or underflow error), Once invalid it stays invalid
	bool IsValid() const;

	// Do I contain carriage return/linefeeds? 
	bool ContainsCRLF() const;

	// Am I read-only
	bool IsReadOnly() const;

	// Converts a buffer from a CRLF buffer to a CR buffer (and back)
	// Returns false if no conversion was necessary (and outBuf is left untouched)
	// If the conversion occurs, outBuf will be cleared.
	bool ConvertCRLF( CUtlBuffer &outBuf );

	// Push/pop pretty-printing tabs
	void PushTab();
	void PopTab();

	// Temporarily disables pretty print
	void EnableTabs( bool bEnable );

	// Securely erases buffer
	void SecureZero() { SecureZeroMemory( m_Memory.Base(), m_Memory.Count() ); }

protected:
	// error flags
	enum
	{
		PUT_OVERFLOW = 0x1,
		GET_OVERFLOW = 0x2,
		MAX_ERROR_FLAG = GET_OVERFLOW,
	};

	void SetOverflowFuncs( UtlBufferOverflowFunc_t getFunc, UtlBufferOverflowFunc_t putFunc );

	bool OnPutOverflow( int nSize );
	bool OnGetOverflow( int nSize );

protected:
	// Checks if a get/put is ok
	bool CheckPut( int size );
	bool CheckGet( int size );

	void AddNullTermination( );

	// Methods to help with pretty-printing
	bool WasLastCharacterCR();
	void PutTabs();

	// Help with delimited stuff
	char GetDelimitedCharInternal( CUtlCharConversion *pConv );
	void PutDelimitedCharInternal( CUtlCharConversion *pConv, char c );

	// Default overflow funcs
	bool PutOverflow( int nSize );
	bool GetOverflow( int nSize );

	// Does the next bytes of the buffer match a pattern?
	bool PeekStringMatch( int nOffset, const char *pString, int nLen );

	// How much whitespace should I skip?
	int PeekWhiteSpace( int nOffset );

	// Checks if a peek get is ok
	bool CheckPeekGet( int nOffset, int nSize );

	// Call this to peek arbitrarily long into memory. It doesn't fail unless
	// it can't read *anything* new
	bool CheckArbitraryPeekGet( int nOffset, int &nIncrement );

	CUtlMemory<uint8> m_Memory;
	int m_Get;
	int m_Put;

	int m_nMaxPut;
	uint16 m_nTab;

	uint8 m_Error;
	uint8 m_Flags;

	UtlBufferOverflowFunc_t m_GetOverflowFunc;
	UtlBufferOverflowFunc_t m_PutOverflowFunc;

private:
	// Returns max amount of data written. Used internally but externally you should use put / seek
	int TellMaxPut() const;

	// Copy construction and assignment are not valid
	CUtlBuffer(const CUtlBuffer& rhs);
	const CUtlBuffer& operator=(const CUtlBuffer& rhs);
};


//-----------------------------------------------------------------------------
// Where am I reading?
//-----------------------------------------------------------------------------
inline int CUtlBuffer::TellGet() const
{
	return m_Get;
}


//-----------------------------------------------------------------------------
// How many bytes remain to be read?
//-----------------------------------------------------------------------------
inline int CUtlBuffer::GetBytesRemaining() const
{
	return m_nMaxPut - TellGet();
}


//-----------------------------------------------------------------------------
// What am I reading?
//-----------------------------------------------------------------------------
inline const void* CUtlBuffer::PeekGet( int offset ) const
{
	return &m_Memory[ m_Get + offset ];
}

//-----------------------------------------------------------------------------
// no offset so optimizer can resolve the function
//-----------------------------------------------------------------------------
inline const void* CUtlBuffer::PeekGet() const
{
	return &m_Memory[ m_Get ];
}

//-----------------------------------------------------------------------------
// Reserve nBytes at the put location and return pointer; follow with SeekPut
//-----------------------------------------------------------------------------
inline void *CUtlBuffer::ReservePut( int nBytes )
{
	return CheckPut( nBytes ) ? PeekPut() : NULL;
}

//-----------------------------------------------------------------------------
// Unserialization
//-----------------------------------------------------------------------------
#if defined(__arm__) || defined(__arm64__)
#define COPY_TYPE( _type, _val ) V_memcpy( &_val, PeekGet(), sizeof( _type ) )
#else
#define COPY_TYPE( _type, _val ) _val = *(_type *)PeekGet()
#endif
#define GET_TYPE( _type, _val, _fmt )			\
	if ( !IsText() )							\
	{											\
		if (CheckGet( sizeof(_type) ))			\
		{										\
			if ( (sizeof(_type)>1) && (m_Flags & CUtlBuffer::LITTLE_ENDIAN_BUFFER ) )	\
			{									\
				if( sizeof(_type) == 2 )		\
				{								\
					_val = LittleWord( (uint16)*(_type *)PeekGet() ); \
				}								\
				else if ( sizeof(_type) == 4 )	\
				{								\
					_val = LittleDWord( (uint32)*(_type *)PeekGet() ); \
				}								\
				else if ( sizeof(_type) == 8 )	\
				{								\
					_val = LittleQWord( (uint64)*(_type *)PeekGet() );	\
				}								\
				else							\
				{								\
					Assert( !"Type not supported" ); \
				}								\
			}									\
			else if ( (sizeof(_type)>1) && (m_Flags & CUtlBuffer::BIG_ENDIAN_BUFFER ) )	\
			{									\
				if( sizeof(_type) == 2 )		\
				{								\
					_val = BigWord( (uint16)*(_type *)PeekGet() ); \
				}								\
				else if ( sizeof(_type) == 4 )	\
				{								\
					_val = BigDWord( (uint32)*(_type *)PeekGet() ); \
				}								\
				else if ( sizeof(_type) == 8 )	\
				{								\
					_val = BigQWord( (uint64)*(_type *)PeekGet() );	\
				}								\
				else							\
				{								\
					Assert( !"Type not supported" ); \
				}								\
			}									\
			else								\
			{									\
				COPY_TYPE( _type, _val );		\
			}									\
			m_Get += sizeof(_type);				\
		}										\
		else									\
		{										\
			_val = 0;							\
		}										\
	}											\
	else										\
	{											\
		_val = 0;								\
		Scanf( _fmt, &_val );					\
	}


//-----------------------------------------------------------------------------
// Where am I writing?
//-----------------------------------------------------------------------------
inline unsigned char CUtlBuffer::GetFlags() const
{ 
	return m_Flags; 
}

	
//-----------------------------------------------------------------------------
// Where am I writing?
//-----------------------------------------------------------------------------
inline int CUtlBuffer::TellPut() const
{
	return m_Put;
}


//-----------------------------------------------------------------------------
// What's the most I've ever written?
//-----------------------------------------------------------------------------
inline int CUtlBuffer::TellMaxPut( ) const
{
	return m_nMaxPut;
}


//-----------------------------------------------------------------------------
// What am I reading?
//-----------------------------------------------------------------------------
inline void* CUtlBuffer::PeekPut( int offset )
{
	return &m_Memory[m_Put + offset];
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Various put methods
//-----------------------------------------------------------------------------
#define PUT_BIN_DATA( _type, _val )		\
	if ( CheckPut( sizeof(_type) ) )	\
	{										\
		if ( (sizeof(_type)>1) && (m_Flags & CUtlBuffer::LITTLE_ENDIAN_BUFFER ) )	\
		{									\
			if( sizeof(_type) == 2 )		\
			{								\
				*(_type *)PeekPut() = (_type)LittleWord( (uint16)_val ); \
			}								\
			else if ( sizeof(_type) == 4 )	\
			{								\
				*(_type *)PeekPut() = (_type)LittleDWord( (uint32)_val ); \
			}								\
			else if ( sizeof(_type) == 8 )	\
			{								\
				*(_type *)PeekPut() = (_type)LittleQWord( (uint64)_val ); \
			}								\
			else							\
			{								\
				Assert( !"Type not supported" ); \
			}								\
		}									\
		else if ( (sizeof(_type)>1) && (m_Flags & CUtlBuffer::BIG_ENDIAN_BUFFER ) )	\
		{									\
			if( sizeof(_type) == 2 )		\
			{								\
				*(_type *)PeekPut() = (_type)BigWord( (uint16)_val );   \
			}								\
			else if ( sizeof(_type) == 4 )	\
			{								\
				*(_type *)PeekPut() = (_type)BigDWord( (uint32)_val );  \
			}								\
			else if ( sizeof(_type) == 8 )	\
			{								\
				*(_type *)PeekPut() = (_type)BigQWord( (uint64)_val );	\
			}								\
			else							\
			{								\
				Assert( !"Type not supported" ); \
			}								\
		}									\
		else								\
		{									\
			*(_type *)PeekPut() = _val;		\
		}									\
		m_Put += sizeof(_type);				\
		AddNullTermination();				\
	}										\


#define PUT_TYPE( _type, _val )			\
	if (!IsText())						\
	{									\
		PUT_BIN_DATA( _type, _val );	\
	}									\
	else								\
	{									\
		PutString( CNumStr( _val ) );	\
	}


//-----------------------------------------------------------------------------
// Methods to help with pretty-printing
//-----------------------------------------------------------------------------
inline bool CUtlBuffer::WasLastCharacterCR()
{
	if ( !IsText() || (TellPut() == 0) )
		return false;
	return ( *( const char * )PeekPut( -1 ) == '\n' );
}

inline void CUtlBuffer::PutTabs()
{
	int nTabCount = ( m_Flags & AUTO_TABS_DISABLED ) ? 0 : m_nTab;
	for (int i = nTabCount; --i >= 0; )
	{
		PUT_BIN_DATA( char, '\t' );
	}
}


//-----------------------------------------------------------------------------
// Push/pop pretty-printing tabs
//-----------------------------------------------------------------------------
inline void CUtlBuffer::PushTab( )
{
	++m_nTab;
}

inline void CUtlBuffer::PopTab()
{
	if ( m_nTab )
	{
		m_nTab--;
	}
}


//-----------------------------------------------------------------------------
// Temporarily disables pretty print
//-----------------------------------------------------------------------------
inline void CUtlBuffer::EnableTabs( bool bEnable )
{
	if ( bEnable )
	{
		m_Flags &= ~AUTO_TABS_DISABLED;
	}
	else
	{
		m_Flags |= AUTO_TABS_DISABLED; 
	}
}


//-----------------------------------------------------------------------------
// Am I a text buffer?
//-----------------------------------------------------------------------------
inline bool CUtlBuffer::IsText() const 
{ 
	return (m_Flags & TEXT_BUFFER) != 0; 
}


//-----------------------------------------------------------------------------
// Can I grow if I'm externally allocated?
//-----------------------------------------------------------------------------
inline bool CUtlBuffer::IsGrowable() const 
{ 
	return (m_Flags & EXTERNAL_GROWABLE) != 0; 
}

//-----------------------------------------------------------------------------
// Am I externally allocated (may need to check IsGrowable or can't grow buffer)
//-----------------------------------------------------------------------------
inline bool CUtlBuffer::IsExternallyAllocated() const
{
	return m_Memory.IsExternallyAllocated();
}

//-----------------------------------------------------------------------------
// Am I valid? (overflow or underflow error), Once invalid it stays invalid
//-----------------------------------------------------------------------------
inline bool CUtlBuffer::IsValid() const 
{ 
	return m_Error == 0; 
}


//-----------------------------------------------------------------------------
// Do I contain carriage return/linefeeds? 
//-----------------------------------------------------------------------------
inline bool CUtlBuffer::ContainsCRLF() const 
{ 
	return IsText() && ((m_Flags & CONTAINS_CRLF) != 0); 
} 


//-----------------------------------------------------------------------------
// Am I read-only
//-----------------------------------------------------------------------------
inline bool CUtlBuffer::IsReadOnly() const
{
	return (m_Flags & READ_ONLY) != 0; 
}


//-----------------------------------------------------------------------------
// Buffer base and size
//-----------------------------------------------------------------------------
inline const void* CUtlBuffer::Base() const	
{ 
	return m_Memory.Base(); 
}

inline void* CUtlBuffer::Base()
{
	return m_Memory.Base(); 
}

// Returns the base as a const char*, only valid in text mode.
inline const char *CUtlBuffer::String() const
{
	Assert( IsText() );
	const char *pchReturn = reinterpret_cast<const char*>( m_Memory.Base() );
	// Never return NULL
	if ( pchReturn )
		return pchReturn;
	else
		return "";
}

//SDR_PUBLIC inline void CUtlBuffer::CopyToString( CUtlString &strText ) const
//SDR_PUBLIC {
//SDR_PUBLIC 	strText.SetDirect( (const char *)Base(), TellMaxPut() );
//SDR_PUBLIC }


inline int CUtlBuffer::Size() const // FIXME delete soon
{ 
	return m_Memory.NumAllocated(); 
}
inline int CUtlBuffer::SizeAllocated() const
{
	return m_Memory.NumAllocated();
}


//-----------------------------------------------------------------------------
// Clears out the buffer; does not free memory
//-----------------------------------------------------------------------------
inline void CUtlBuffer::Clear()
{
	m_Get = 0;
	m_Put = 0;
	m_Error = 0;
	m_nMaxPut = -1;
	AddNullTermination();
}

//-----------------------------------------------------------------------------
// Clears out the buffer; frees memory
//-----------------------------------------------------------------------------
inline void CUtlBuffer::Purge()
{
	m_Get = 0;
	m_Put = 0;
	m_nMaxPut = 0;
	m_Error = 0;
	m_Memory.Purge();
}

inline void CUtlBuffer::CopyBuffer( const CUtlBuffer &buffer )
{
	CopyBuffer( buffer.Base(), buffer.TellPut() );
}

inline void	CUtlBuffer::CopyBuffer( const void *pubData, int cubData )
{
	Clear();
	if ( cubData )
	{
		Put( pubData, cubData );
	}
}


/// CUtlBuffer that will wipe upon destruction
//
/// WARNING: This is only intended for simple use cases where the caller
/// can easily pre-allocate.  For example, it won't wipe if the buffer needs
/// to be relocated as a result of realloc.  Or if you pas it to a function
/// via a CUtlBuffer&, and CUtlBuffer::Purge is invoked directly.  Etc.
class CAutoWipeBuffer : public CUtlBuffer
{
public:
	CAutoWipeBuffer() {}
	explicit CAutoWipeBuffer( int cbInit ) : CUtlBuffer( 0, cbInit, 0 ) {}
	~CAutoWipeBuffer() { Purge(); }

	void Clear()
	{
		SecureZeroMemory( Base(), SizeAllocated() );
		CUtlBuffer::Clear();
	}

	void Purge()
	{
		Clear();
		CUtlBuffer::Purge();
	}
};

#endif // UTLBUFFER_H

