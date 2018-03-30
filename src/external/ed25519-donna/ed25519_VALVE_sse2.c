/*========= Copyright © Valve Corporation, All rights reserved =======================*/


#define ED25519_SSE2
#define ED25519_SUFFIX _sse2

/* hackery to avoid double-definitions of functions */
#define NO_ED25519_RANDOMBYTES_IMPL
#define ed25519_randombytes_unsafe_sse2 ed25519_randombytes_unsafe

/* hackery to avoid double-definitions of global test buffer */
#define batch_point_buffer test_batch_point_buffer_sse2

#include "ed25519_VALVE.c"
