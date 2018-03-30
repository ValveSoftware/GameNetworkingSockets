#include "curve25519-donna.h"

#if !defined(CURVE25519_SUFFIX)
#define CURVE25519_SUFFIX 
#endif

#define CURVE25519_FN3(fn,suffix) fn##suffix
#define CURVE25519_FN2(fn,suffix) CURVE25519_FN3(fn,suffix)
#define CURVE25519_FN(fn)         CURVE25519_FN2(fn,CURVE25519_SUFFIX)

void
CURVE25519_FN(curve25519_donna) (curve25519_key mypublic, const curve25519_key secret, const curve25519_key basepoint) {
	curve25519_key e;
	size_t i;

	for (i = 0;i < 32;++i) e[i] = secret[i];
	e[0] &= 0xf8;
	e[31] &= 0x7f;
	e[31] |= 0x40;
	curve25519_scalarmult_donna(mypublic, e, basepoint);
}

void
CURVE25519_FN(curve25519_donna_basepoint) (curve25519_key mypublic, const curve25519_key secret) {
	static const curve25519_key basepoint = {9};
	CURVE25519_FN(curve25519_donna)(mypublic, secret, basepoint);
}
