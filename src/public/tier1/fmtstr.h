//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: A simple class for performing safe and in-expression sprintf-style
//			string formatting
//
// $NoKeywords: $
//=============================================================================//

#ifndef FMTSTR_H
#define FMTSTR_H

#include <stdarg.h>
#include <stdio.h>
#include "tier0/platform.h"
#include "tier0/dbg.h"
#include "vstdlib/strtools.h"

#if defined( _WIN32 )
#pragma once
#endif

#if defined(POSIX)
// clang will error if the symbol visibility for an object changes between static libraries (and/or your main dylib)
// so force the FmtStr templates and importantly the global scAsserted below to hidden (i.e don't escape the dll) forcefully
#pragma GCC visibility push(hidden)
#endif

//=============================================================================

// using macro to be compatible with GCC
#define FmtStrVSNPrintf( szBuf, nBufSize, refnFormattedLength, bQuietTruncation, pszFormat, lastArg ) \
	do \
	{ \
		va_list arg_ptr; \
		bool bTruncated = false; \
		static int scAsserted = 0; \
	\
		va_start(arg_ptr, lastArg); \
		(refnFormattedLength) = V_vsnprintfRet( (szBuf), (nBufSize), pszFormat, arg_ptr, &bTruncated ); \
		va_end(arg_ptr); \
	\
		(szBuf)[(nBufSize)-1] = 0; \
		if ( bTruncated && !(bQuietTruncation) && scAsserted < 5 ) \
		{ \
			Assert( !bTruncated ); \
			scAsserted++; \
		} \
	} \
	while (0)


//-----------------------------------------------------------------------------
//
// Purpose: String formatter with specified size
//

#ifdef _DEBUG
#define QUIET_TRUNCATION false
#else
#define QUIET_TRUNCATION true		// Force quiet for release builds
#endif

class CNumStr
{
public:

	CNumStr() { m_szBuf[0] = 0; m_nLength = 0; }
	explicit CNumStr( bool b )		{ SetBool( b ); } 

	explicit CNumStr( int8 n8 )		{ SetInt8( n8 ); }
	explicit CNumStr( uint8 un8 )	{ SetUint8( un8 );  }

	explicit CNumStr( int16 n16 )	{ SetInt16( n16 ); }
	explicit CNumStr( uint16 un16 )	{ SetUint16( un16 );  }

	explicit CNumStr( int32 n32 )	{ SetInt32( n32 ); }
	explicit CNumStr( uint32 un32 )	{ SetUint32( un32 ); }

	explicit CNumStr( int64 n64 )	{ SetInt64( n64 ); }
	explicit CNumStr( uint64 un64 )	{ SetUint64( un64 ); }

	explicit CNumStr( long l )			{ if ( sizeof( l ) == 4 ) SetInt32( (int32)l ); else SetInt64( (int64)l ); }
	explicit CNumStr( unsigned long l )	{ if ( sizeof( l ) == 4 ) SetUint32( (uint32)l ); else SetUint64( (uint64)l ); }

	explicit CNumStr( double f )	{ SetDouble( f ); }
	explicit CNumStr( float f )		{ SetFloat( f ); }

#define NUMSTR_FAST_DIGIT( digit )					{ m_nLength = 1; m_szBuf[0] = '0' + ( digit ); m_szBuf[1] = 0; return m_szBuf; }
#define NUMSTR_CHECK_FAST( val, utype )				if ( (utype)val < 10 ) NUMSTR_FAST_DIGIT( (utype)val )

	inline const char* SetBool( bool b )			{ NUMSTR_FAST_DIGIT( b & 1 ); }

#ifdef _WIN32
	inline const char* SetInt8( int8 n8 )			{ NUMSTR_CHECK_FAST( n8,   uint8 )	_itoa( (int32)n8, m_szBuf, 10 ); m_nLength = V_strlen(m_szBuf); return m_szBuf; }
	inline const char* SetUint8( uint8 un8 )		{ NUMSTR_CHECK_FAST( un8,  uint8 )	_itoa( (int32)un8, m_szBuf, 10 ); m_nLength = V_strlen(m_szBuf); return m_szBuf; }
	inline const char* SetInt16( int16 n16 )		{ NUMSTR_CHECK_FAST( n16,  uint16 )	_itoa( (int32)n16, m_szBuf, 10 ); m_nLength = V_strlen(m_szBuf); return m_szBuf; }
	inline const char* SetUint16( uint16 un16 )		{ NUMSTR_CHECK_FAST( un16, uint16 )	_itoa( (int32)un16, m_szBuf, 10 ); m_nLength = V_strlen(m_szBuf); return m_szBuf; }
	inline const char* SetInt32( int32 n32 )		{ NUMSTR_CHECK_FAST( n32,  uint32 )	_itoa( n32, m_szBuf, 10 ); m_nLength = V_strlen(m_szBuf); return m_szBuf; }
	inline const char* SetUint32( uint32 un32 )		{ NUMSTR_CHECK_FAST( un32, uint32 )	_i64toa( (int64)un32, m_szBuf, 10 ); m_nLength = V_strlen(m_szBuf); return m_szBuf; }
	inline const char* SetInt64( int64 n64 )		{ NUMSTR_CHECK_FAST( n64,  uint64 )	_i64toa( n64, m_szBuf, 10 ); m_nLength = V_strlen(m_szBuf); return m_szBuf; }
	inline const char* SetUint64( uint64 un64 )		{ NUMSTR_CHECK_FAST( un64, uint64 )	_ui64toa( un64, m_szBuf, 10 ); m_nLength = V_strlen(m_szBuf); return m_szBuf; }
#else
	inline const char* SetInt8( int8 n8 )			{ NUMSTR_CHECK_FAST( n8,   uint8 )	m_nLength = V_snprintf( m_szBuf, sizeof(m_szBuf), "%d", (int32)n8 ); return m_szBuf; }
	inline const char* SetUint8( uint8 un8 )		{ NUMSTR_CHECK_FAST( un8,  uint8 )	m_nLength = V_snprintf( m_szBuf, sizeof(m_szBuf), "%d", (int32)un8 ); return m_szBuf; }
	inline const char* SetInt16( int16 n16 )		{ NUMSTR_CHECK_FAST( n16,  uint16 )	m_nLength = V_snprintf( m_szBuf, sizeof(m_szBuf), "%d", (int32)n16 ); return m_szBuf; }
	inline const char* SetUint16( uint16 un16 )		{ NUMSTR_CHECK_FAST( un16, uint16 )	m_nLength = V_snprintf( m_szBuf, sizeof(m_szBuf), "%d", (int32)un16 ); return m_szBuf; }
	inline const char* SetInt32( int32 n32 )		{ NUMSTR_CHECK_FAST( n32,  uint32 )	m_nLength = V_snprintf( m_szBuf, sizeof(m_szBuf), "%d", n32 ); return m_szBuf; }
	inline const char* SetUint32( uint32 un32 )		{ NUMSTR_CHECK_FAST( un32, uint32 )	m_nLength = V_snprintf( m_szBuf, sizeof(m_szBuf), "%u", un32 ); return m_szBuf; }
	inline const char* SetInt64( int64 n64 )		{ NUMSTR_CHECK_FAST( n64,  uint64 )	m_nLength = V_snprintf( m_szBuf, sizeof(m_szBuf), "%lld", n64 ); return m_szBuf; }
	inline const char* SetUint64( uint64 un64 )		{ NUMSTR_CHECK_FAST( un64, uint64 )	m_nLength = V_snprintf( m_szBuf, sizeof(m_szBuf), "%llu", un64 ); return m_szBuf; }
#endif

	inline const char* SetDouble( double f )		{ if ( f == 0.0  && !std::signbit( f ) ) NUMSTR_FAST_DIGIT( 0 ); if ( f == 1.0  ) NUMSTR_FAST_DIGIT( 1 ); m_nLength = V_snprintf( m_szBuf, sizeof(m_szBuf), "%.18g", f ); return m_szBuf; }
	inline const char* SetFloat( float f )			{ if ( f == 0.0f && !std::signbit( f ) ) NUMSTR_FAST_DIGIT( 0 ); if ( f == 1.0f ) NUMSTR_FAST_DIGIT( 1 ); m_nLength = V_snprintf( m_szBuf, sizeof(m_szBuf), "%.18g", f ); return m_szBuf; }

	//SDR_PUBLIC inline const char* SetHexUint64( uint64 un64 )	{ V_binarytohex( (byte *)&un64, sizeof( un64 ), m_szBuf, sizeof( m_szBuf ) ); m_nLength = V_strlen(m_szBuf); return m_szBuf; }

#undef NUMSTR_FAST_DIGIT
#undef NUMSTR_CHECK_FAST

	operator const char *() const { return m_szBuf; }
	char *Access() { return m_szBuf; }
	const char* String() const { return m_szBuf; }
	int Length() const { return m_nLength; }
	
	void AddQuotes()
	{
		Assert( m_nLength + 2 <= V_ARRAYSIZE(m_szBuf) );
		memmove( m_szBuf+1, m_szBuf, m_nLength );
		m_szBuf[0] = '"';
		m_szBuf[m_nLength+1] = '"';
		m_nLength+=2;
		m_szBuf[m_nLength]=0;
	}

protected:
	char m_szBuf[28]; // long enough to hold 18 digits of precision, a decimal, a - sign, e+### suffix, and quotes
	int m_nLength;

};

#define FMTSTR_STD_LEN 256

// CFMTSTR_MAX_STACK_ALLOC is protected so it can be specialized for architecture,
// bitness, or whatnot.
#ifndef CFMTSTR_MAX_STACK_ALLOC
#define CFMTSTR_MAX_STACK_ALLOC 1024
#endif

template<int SIZE_BUF, bool ON_STACK>
class CStrFmtSpecialized
{
};

template<int SIZE_BUF>
class CStrFmtSpecialized<SIZE_BUF, /*ON_STACK=*/true>
{
public:
	char m_szBuf[SIZE_BUF];
};

template<int SIZE_BUF>
class CStrFmtSpecialized<SIZE_BUF, /*ON_STACK=*/false>
{
public:
	CStrFmtSpecialized()
	{
		m_szBuf = new char[SIZE_BUF];
	}

	~CStrFmtSpecialized() 
	{
		delete [] m_szBuf;
	}

	char *m_szBuf;
};

template <int SIZE_BUF, bool QT = QUIET_TRUNCATION, bool ON_STACK = (SIZE_BUF <= CFMTSTR_MAX_STACK_ALLOC ) >
class CFmtStrN : public CStrFmtSpecialized< SIZE_BUF, ON_STACK >
{
	typedef CStrFmtSpecialized< SIZE_BUF, ON_STACK > BaseClass;
public:
	CFmtStrN()	
	{ 
		BaseClass::m_szBuf[0] = 0; 
		m_nLength = 0;
	}
	
	// Standard C formatting
	CFmtStrN( PRINTF_FORMAT_STRING const char *pszFormat, ... ) FMTFUNCTION( 2, 3 )
	{
		FmtStrVSNPrintf( BaseClass::m_szBuf, SIZE_BUF, m_nLength, QT, pszFormat, pszFormat );
	}

	// Use this for pass-through formatting
	CFmtStrN( const char ** ppszFormat, ... ) 	
	{
		FmtStrVSNPrintf( BaseClass::m_szBuf, SIZE_BUF, m_nLength, QT, *ppszFormat, ppszFormat ); 
	}

	// Explicit reformat
	const char *Format( PRINTF_FORMAT_STRING const char *pszFormat, ... ) FMTFUNCTION( 2, 3 )
	{
		FmtStrVSNPrintf( BaseClass::m_szBuf, SIZE_BUF, m_nLength, QT, pszFormat, pszFormat );
		return BaseClass::m_szBuf;
	}

	// Use for access
	operator const char *() const				{ return BaseClass::m_szBuf; }
	const char *Get() const						{ return BaseClass::m_szBuf; }
	const char *String() const					{ return BaseClass::m_szBuf; }

	char *Access()								{ return BaseClass::m_szBuf; }
	int Length() const { return m_nLength; }

	CFmtStrN< SIZE_BUF, QT, ON_STACK > & operator=( const char *pchValue )
	{
		if ( !QT )
		{
			int nLen = V_strlen( pchValue );
			AssertMsg( nLen < SIZE_BUF-1, "Truncation in CFmtStr operator=" );
		}

		m_nLength = CopyStringLength( BaseClass::m_szBuf, pchValue, SIZE_BUF );
		return *this;
	}

	CFmtStrN< SIZE_BUF, QT, ON_STACK > & operator+=( const char *pchValue )
	{
		Append( pchValue );
		return *this; 
	}

	void Clear()
	{
		BaseClass::m_szBuf[0] = 0; 
		m_nLength = 0;
	}

	void AppendFormat( PRINTF_FORMAT_STRING const char *pchFormat, ... ) FMTFUNCTION( 2, 3 )
	{
		char *pchEnd = BaseClass::m_szBuf + m_nLength;
		int nFormattedLength = 0;
		FmtStrVSNPrintf( pchEnd, SIZE_BUF - m_nLength, nFormattedLength, QT, pchFormat, pchFormat );
		m_nLength = m_nLength + nFormattedLength;
	}

	void AppendFormatV( const char *pchFormat, va_list args );
	void Append( const char *pchValue )
	{
		if ( !QT )
		{
			int nLen = V_strlen( pchValue );
			AssertMsg( nLen < ( SIZE_BUF - m_nLength ), "Truncation in CFmtStr::Append" );
		}

		char *pchDest = BaseClass::m_szBuf + m_nLength;
		const char *pchSource = pchValue;
		int cbRemaining = SIZE_BUF - m_nLength;
		int nAppendedLength = CopyStringLength( pchDest, pchSource, cbRemaining );

		m_nLength = m_nLength + nAppendedLength;
	}

	void AppendIndent( uint32 unCount, char chIndent = '\t' );
	void Set( const char *pchValue, int nSize = -1 );

protected:
	int CopyStringLength( char *pstrDest, const char *pstrSource, int nMaxLength )
	{
		int nCount = nMaxLength;
		char *pstrStart = pstrDest;

		while ( 0 < nCount && 0 != ( *pstrDest++ = *pstrSource++ ) )
			nCount--;

		int nLength = 0;
		if ( nMaxLength > 0 )
		{
			pstrDest[-1] = 0;
			nLength = pstrDest - pstrStart - 1;
		}

		return nLength;
	}

protected:
	int m_nLength;
};


template< int SIZE_BUF, bool QT, bool ON_STACK >
void CFmtStrN< SIZE_BUF, QT, ON_STACK >::AppendIndent( uint32 unCount, char chIndent )
{
	Assert( Length() + unCount < SIZE_BUF );
	if( Length() + unCount >= SIZE_BUF )
		unCount = SIZE_BUF - (1+Length());
	for ( uint32 x = 0; x < unCount; x++ )
	{
		BaseClass::m_szBuf[ m_nLength++ ] = chIndent;
	}
	BaseClass::m_szBuf[ m_nLength ] = '\0';
}

template< int SIZE_BUF, bool QT, bool ON_STACK >
void CFmtStrN< SIZE_BUF, QT, ON_STACK >::AppendFormatV( const char *pchFormat, va_list args )
{
	int cubPrinted = V_vsnprintf( BaseClass::m_szBuf+Length(), SIZE_BUF - Length(), pchFormat, args );
	m_nLength += cubPrinted;
}


template< int SIZE_BUF, bool QT, bool ON_STACK >
void CFmtStrN< SIZE_BUF, QT, ON_STACK >::Set( const char *pchValue, int nSize )
{
	int nMaxLength =  nSize == -1 ? SIZE_BUF-1 : Min( nSize, SIZE_BUF-1 );
	m_nLength = CopyStringLength( BaseClass::m_szBuf, pchValue, nMaxLength );
}

#if defined(POSIX)
#pragma GCC visibility pop
#endif

//-----------------------------------------------------------------------------
//
// Purpose: Default-sized string formatter
//

typedef CFmtStrN< FMTSTR_STD_LEN > CFmtStr;
typedef CFmtStrN< FMTSTR_STD_LEN, true > CFmtStrQuietTruncation;
typedef CFmtStrN<32> CFmtStr32;
typedef CFmtStrN<1024> CFmtStr1024;
typedef CFmtStrN<8192> CFmtStrMax;

//=============================================================================

const int k_cchFormattedDate = 64;
const int k_cchFormattedTime = 32;
bool BGetLocalFormattedDateAndTime( time_t timeVal, char *pchDate, int cubDate, char *pchTime, int cubTime, bool bIncludeSeconds = false, bool bShortDateFormat = false );
bool BGetLocalFormattedDate( time_t timeVal, char *pchDate, int cubDate, bool bShortDateFormat = false );
bool BGetLocalFormattedTime( time_t timeVal, char *pchTime, int cubTime, bool bIncludeSeconds = false );
bool BGetLocalFormattedHourFromInt( int nHour, char *pchHour, int cubHour );

#endif // FMTSTR_H
