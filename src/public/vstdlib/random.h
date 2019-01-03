// Stub for just what we need

#ifndef VSTDLIB_RANDOM_H
#define VSTDLIB_RANDOM_H

#include <stdlib.h>

inline void WeakRandomSeed( int x ) { srand(x); }
inline float WeakRandomFloat( float flMin, float flMax )
{
	return flMin + ( (float)rand() / (float)RAND_MAX ) * ( flMax - flMin );
}
inline int WeakRandomInt( int nMin, int nMax )
{
	return nMin + rand() % ( nMax - nMin + 1 );
}

#endif // VSTDLIB_RANDOM_H

