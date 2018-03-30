#include "curve25519.h"
#include "curve25519-donna-portable.h"

#if defined(CURVE25519_SSE2)
#else
	#if defined(HAVE_UINT128) && !defined(CURVE25519_FORCE_32BIT)
		#define CURVE25519_64BIT
	#else
		#define CURVE25519_32BIT
	#endif
#endif

#if !defined(CURVE25519_NO_INLINE_ASM)
#endif


#if defined(CURVE25519_SSE2)
	#include "curve25519-donna-sse2.h"
#elif defined(CURVE25519_64BIT)
	#include "curve25519-donna-64bit.h"
#else
	#include "curve25519-donna-32bit.h"
#endif

#include "curve25519-donna-common.h"

#if defined(CURVE25519_SSE2)
	#include "curve25519-donna-scalarmult-sse2.h"
#else
	#include "curve25519-donna-scalarmult-base.h"
#endif

