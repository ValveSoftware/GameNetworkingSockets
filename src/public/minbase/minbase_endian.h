//====== Copyright 1996-2012, Valve Corporation, All rights reserved. ======//
//
// Endianness handling.
//
//==========================================================================//

#ifndef MINBASE_ENDIAN_H
#define MINBASE_ENDIAN_H
#pragma once

#if !defined(MINBASE_TYPES_H) || !defined(MINBASE_MACROS_H)
#error Must include minbase_types.h and minbase_macros.h
#endif

//-----------------------------------------------------------------------------
// Purpose: Standard functions for handling endian-ness
//-----------------------------------------------------------------------------

//-------------------------------------
// Basic swaps
//-------------------------------------

template <typename T>
inline T WordSwapC( T w )
{
	PLAT_COMPILE_TIME_ASSERT( sizeof(T) == sizeof(uint16) );
	uint16 temp;
#if defined( _MSC_VER ) || defined( __ICC )
	temp = _byteswap_ushort( *(uint16*)&w );
#else
	// This translates into a single rotate on x86/x64
	temp = *(uint16 *)&w;
	temp = (temp << 8) | (temp >> 8);
#endif
	return *((T*)&temp);
}

template <typename T>
inline T DWordSwapC( T dw )
{
	PLAT_COMPILE_TIME_ASSERT( sizeof( T ) == sizeof(uint32) );
	uint32 temp;
#if defined( _MSC_VER ) || defined( __ICC )
	temp = _byteswap_ulong( *(uint32*)&dw );
#elif defined( __clang__ ) || defined( __GNUC__ )
	temp = __builtin_bswap32( *(uint32*)&dw );
#else
	temp = *((uint32 *)&dw) 				>> 24;
	temp |= ((*((uint32 *)&dw) & 0x00FF0000) >> 8);
	temp |= ((*((uint32 *)&dw) & 0x0000FF00) << 8);
	temp |= (*((uint32 *)&dw) << 24;
#endif
   return *((T*)&temp);
}

template <typename T>
inline T QWordSwapC( T dw )
{
	PLAT_COMPILE_TIME_ASSERT( sizeof( dw ) == sizeof(uint64) );
	uint64 temp;
#if defined( _MSC_VER ) || defined( __ICC )
	temp = _byteswap_uint64( *(uint64*)&dw );
#elif defined( __clang__ ) || defined( __GNUC__ )
	temp = __builtin_bswap64( *(uint64*)&dw );
#else
	temp = (uint64)DWordSwapC( (uint32)( ( *(uint64*)&dw ) >> 32 ) );
	temp |= (uint64)DWordSwapC( (uint32)( *(uint64*)&dw ) ) << 32;
#endif
	return *((T*)&temp);
}

#define WordSwap  WordSwapC
#define DWordSwap DWordSwapC
#define QWordSwap QWordSwapC

#if defined(VALVE_LITTLE_ENDIAN)

	#define BigInt16( val )		WordSwap( val )
	#define BigWord( val )		WordSwap( val )
	#define BigInt32( val )		DWordSwap( val )
	#define BigDWord( val )		DWordSwap( val )
	#define BigQWord( val )		QWordSwap( val ) 
	#define BigFloat( val )		DWordSwap( val )
	#define LittleInt16( val )	( val )
	#define LittleWord( val )	( val )
	#define LittleInt32( val )	( val )
	#define LittleDWord( val )	( val )
	#define LittleQWord( val )	( val )

	#define LittleFloat( val )	( val )

#elif defined(VALVE_BIG_ENDIAN)

	#define BigInt16( val )		( val )
	#define BigWord( val )		( val )
	#define BigInt32( val )		( val )
	#define BigDWord( val )		( val )
	#define BigQWord( val )		( val )
	#define BigFloat( val )		( val )
	#define LittleInt16( val )	WordSwap( val )
	#define LittleWord( val )	WordSwap( val )
	#define LittleInt32( val )	DWordSwap( val )
	#define LittleDWord( val )	DWordSwap( val )
	#define LittleQWord( val )	QWordSwap( val )
	#define LittleFloat( val )	DWordSwap( val )

#else

	// @Note (toml 05-02-02): this technique expects the compiler to 
	// optimize the expression and eliminate the other path. On any new 
	// platform/compiler this should be tested.
	inline short BigInt16( int16 val )		{ int test = 1; return ( *(char *)&test == 1 ) ? WordSwap( val )  : val; }
	inline uint16 BigWord( uint16 val )		{ int test = 1; return ( *(char *)&test == 1 ) ? WordSwap( val )  : val; }
	inline int32 BigInt32( int32 val )		{ int test = 1; return ( *(char *)&test == 1 ) ? DWordSwap( val ) : val; }
	inline uint32 BigDWord( uint32 val )	{ int test = 1; return ( *(char *)&test == 1 ) ? DWordSwap( val ) : val; }
	inline uint64 BigQWord( uint64 val )	{ int test = 1; return ( *(char *)&test == 1 ) ? QWordSwap( val ) : val; }
	inline float BigFloat( float val )		{ int test = 1; return ( *(char *)&test == 1 ) ? DWordSwap( val ) : val; }
	inline short LittleInt16( int16 val )	{ int test = 1; return ( *(char *)&test == 1 ) ? val : WordSwap( val ); }
	inline uint16 LittleWord( uint16 val )	{ int test = 1; return ( *(char *)&test == 1 ) ? val : WordSwap( val ); }
    inline long LittleInt32( int32 val )		{ int test = 1; return ( *(char *)&test == 1 ) ? val : DWordSwap( val ); }
	inline uint32 LittleDWord( uint32 val )	{ int test = 1; return ( *(char *)&test == 1 ) ? val : DWordSwap( val ); }
	inline uint64 LittleQWord( uint64 val )	{ int test = 1; return ( *(char *)&test == 1 ) ? val : QWordSwap( val ); }
	inline float LittleFloat( float val )	{ int test = 1; return ( *(char *)&test == 1 ) ? val : DWordSwap( val ); }

#endif

#endif // #ifndef MINBASE_ENDIAN_H
