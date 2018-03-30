// Stub for just what we need

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

