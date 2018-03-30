/* The code for curve25519-donna comes from https://github.com/floodyberry/curve25519-donna
   where it is explicitly placed in the public domain. This wrapper compiles it with the _sse2
   function suffix to be distinguished from the native C version. */

#define CURVE25519_SUFFIX _sse2
#define CURVE25519_SSE2

#include "curve25519.c"
