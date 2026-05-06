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
	temp |= (*((uint32 *)&dw) << 24);
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
	#define LittleInt16( val )  ( (void)sizeof(char[sizeof(val)==2?1:-1]), (val) )
	#define LittleWord( val )   ( (void)sizeof(char[sizeof(val)==2?1:-1]), (val) )
	#define LittleInt32( val )  ( (void)sizeof(char[sizeof(val)==4?1:-1]), (val) )
	#define LittleDWord( val )  ( (void)sizeof(char[sizeof(val)==4?1:-1]), (val) )
	#define LittleQWord( val )  ( (void)sizeof(char[sizeof(val)==8?1:-1]), (val) )
	#define LittleFloat( val )	( val )

#elif defined(VALVE_BIG_ENDIAN)

	#define BigInt16( val )  ( (void)sizeof(char[sizeof(val)==2?1:-1]), (val) )
	#define BigWord( val )   ( (void)sizeof(char[sizeof(val)==2?1:-1]), (val) )
	#define BigInt32( val )  ( (void)sizeof(char[sizeof(val)==4?1:-1]), (val) )
	#define BigDWord( val )  ( (void)sizeof(char[sizeof(val)==4?1:-1]), (val) )
	#define BigQWord( val )  ( (void)sizeof(char[sizeof(val)==8?1:-1]), (val) )
	#define BigFloat( val )		( val )
	#define LittleInt16( val )	WordSwap( val )
	#define LittleWord( val )	WordSwap( val )
	#define LittleInt32( val )	DWordSwap( val )
	#define LittleDWord( val )	DWordSwap( val )
	#define LittleQWord( val )	QWordSwap( val )
	#define LittleFloat( val )	DWordSwap( val )

#else
	#error "Must define either VALVE_LITTLE_ENDIAN or VALVE_BIG_ENDIAN"
#endif

#endif // #ifndef MINBASE_ENDIAN_H
