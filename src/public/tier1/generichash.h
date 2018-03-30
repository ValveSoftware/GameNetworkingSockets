//======= Copyright � 2005-2011, Valve Corporation, All rights reserved. =========
//
// Public domain MurmurHash3 by Austin Appleby is a very solid general-purpose
// hash with a 32-bit output. References:
// http://code.google.com/p/smhasher/ (home of MurmurHash3)
// https://sites.google.com/site/murmurhash/avalanche
// http://www.strchr.com/hash_functions 
//
// Variant Pearson Hash general purpose hashing algorithm described
// by Cargill in C++ Report 1994. Generates a 16-bit result.
// Now relegated to PearsonHash namespace, not recommended for use;
// still around in case someone needs value compatibility with old code.
//
//=============================================================================

#ifndef GENERICHASH_H
#define GENERICHASH_H

#if defined(_WIN32)
#pragma once
#endif

#if !defined(OSX) && !defined(_MINIMUM_BUILD_)
//
//	Note that CEG builds with _MINIMUM_BUILD_ defined because it must remain CRT agnostic - 
//	hence we eliminate this CRT reference.
//
#include <type_traits>
#endif

uint32 MurmurHash3_32( const void *key, size_t len, uint32 seed, bool bCaselessStringVariant = false );
void MurmurHash3_128( const void * key, const int len, const uint32 seed, void * out );

inline uint32 HashString( const char *pszKey, size_t len )
{
	return MurmurHash3_32( pszKey, len, 1047 /*anything will do for a seed*/, false );
}

inline uint32 HashStringCaseless( const char *pszKey, size_t len )
{
	return MurmurHash3_32( pszKey, len, 1047 /*anything will do for a seed*/, true );
}

#if	!defined(_MINIMUM_BUILD_)
inline uint32 HashString( const char *pszKey )
{
	return HashString( pszKey, strlen( pszKey ) );
}

inline uint32 HashStringCaseless( const char *pszKey )
{
	return HashStringCaseless( pszKey, strlen( pszKey ) );
}
#endif

inline uint32 HashInt64( uint64 h )
{
	// roughly equivalent to MurmurHash3_32( &lower32, sizeof(uint32), upper32_as_seed )...
	// theory being that most of the entropy is in the lower 32 bits and we still mix
	// everything together at the end, so not fully shuffling upper32 is not a big deal

    //
	//	On the 32 bit compiler in various modes, this form of the expression generates
	//	a CRT call to do a 64bit shift in 32 bit registers.  
	//	Well I need this code to compile without any CRT references - so use this expression instead.
	//
#if defined(_MSC_VER) && defined(_M_IX86) && defined(_MINIMUM_BUILD_)
	uint32 h1 = reinterpret_cast<uint32*>( &h )[1];
#else
    uint32  h1 = static_cast<uint32>( h>>32 );
#endif


	uint32 k1 = (uint32)h;

	k1 *= 0xcc9e2d51;
	k1 = (k1 << 15) | (k1 >> 17);
	k1 *= 0x1b873593;

	h1 ^= k1;
	h1 = (h1 << 13) | (h1 >> 19);
	h1 = h1*5+0xe6546b64;

	h1 ^= h1 >> 16;
	h1 *= 0x85ebca6b;
	h1 ^= h1 >> 13;
	h1 *= 0xc2b2ae35;
	h1 ^= h1 >> 16;

	return h1;
}

inline uint32 HashInt( uint32 h )
{
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;
	return h;
}



template <typename T>
inline uint32 HashItemAsBytes( const T&item )
{
	if ( sizeof(item) == sizeof(uint32) )
		return HashInt( *(uint32*)&item );

	if ( sizeof(item) == sizeof(uint64) )
		return HashInt64( *(uint64*)&item );

	return MurmurHash3_32( &item, sizeof(item), 1047 );
}

template <typename T>
inline uint32 HashItem( const T &item )
{
#if !defined(_WIN32) || !defined(_MINIMUM_BUILD_)
	// If you hit this assert, you have likely used a class such as CUtlHashMap with a non-trivial "Key" type
	// that we don't know how to hash by default.
	//
	// If it is a simple structure and you are SURE there is no inter-member padding, then you should use
	// HashFunctorUnPaddedStructure< YourKeyType > in your decl of CUtlHashMap as the 4th template param.
	//
	// If there is padding, or if you need to include things pointed to or whatever, then you must define
	// your own HashFunctor<> explicit instantiation that does all the stuff you want to hash the object.
	COMPILE_TIME_ASSERT( std::is_integral<T>::value || std::is_enum<T>::value || std::is_pointer<T>::value );
#endif
	return HashItemAsBytes( item );
}


template<typename T>
struct HashFunctor
{
	typedef uint32 TargetType;
	TargetType operator()(const T &key) const
	{
		return HashItem( key );
	}
};

#if	!defined(_MINIMUM_BUILD_)
template<>
struct HashFunctor<char *>
{
	typedef uint32 TargetType;
	TargetType operator()(const char *key) const
	{
		return HashString( key );
	}
};

template<>
struct HashFunctor<const char *>
{
	typedef uint32 TargetType;
	TargetType operator()(const char *key) const
	{
		return HashString( key );
	}
};

struct HashFunctorStringCaseless
{
	typedef uint32 TargetType;
	TargetType operator()(const char *key) const
	{
		return HashStringCaseless( key );
	}
};

template<class T>
struct HashFunctorUnpaddedStructure
{
	typedef uint32 TargetType;
	TargetType operator()(const T &key) const
	{
		return HashItemAsBytes( key );
	}
};
#endif	// _MINIMUM_BUILD_

//-----------------------------------------------------------------------------

#if	!defined(_MINIMUM_BUILD_)
namespace PearsonHash
{
	unsigned FASTCALL HashString( const char *pszKey );
	unsigned FASTCALL HashStringCaseless( const char *pszKey );
}
#endif


#endif /* !GENERICHASH_H */
