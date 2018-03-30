/*
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "curve25519.h"

#include "test-ticks.h"

static void
curveassert_die(const unsigned char *a, const unsigned char *b, size_t len, int round, const char *failreason) {
	size_t i;
	if (round > 0)
		printf("round %d, %s\n", round, failreason);
	else
		printf("%s\n", failreason);
	printf("want: "); for (i = 0; i < len; i++) printf("%02x,", a[i]); printf("\n");
	printf("got : "); for (i = 0; i < len; i++) printf("%02x,", b[i]); printf("\n");
	printf("diff: "); for (i = 0; i < len; i++) if (a[i] ^ b[i]) printf("%02x,", a[i] ^ b[i]); else printf("  ,"); printf("\n\n");
	exit(1);
}

static void
curveassert_equal(const unsigned char *a, const unsigned char *b, size_t len, const char *failreason) {
	if (memcmp(a, b, len) == 0)
		return;
	curveassert_die(a, b, len, -1, failreason);
}

/* result of the curve25519 scalarmult |((|max| * |max|) * |max|)... 1024 times| * basepoint */

/*
static const curve25519_key curve25519_expected = {
	0x8e,0x74,0xac,0x44,0x38,0xa6,0x87,0x54,
	0xc8,0xc6,0x1b,0xa0,0x8b,0xd2,0xf7,0x7b,
	0xbb,0xc6,0x26,0xd5,0x24,0xb3,0xbe,0xa0,
	0x38,0x30,0x1d,0xec,0x2d,0x92,0xe7,0x51
};
*/

/* this is the result if the 256th bit of a point is ignored:*/
const curve25519_key curve25519_expected = {
	0x1e,0x61,0x8e,0xc0,0x2f,0x25,0x1b,0x8d,
	0x62,0xed,0x0e,0x57,0x3c,0x83,0x11,0x49,
	0x7b,0xa5,0x85,0x40,0x1a,0xcf,0xd4,0x3e,
	0x5b,0xeb,0xa8,0xb5,0xae,0x75,0x96,0x2d
};


/* shared key resulting from the private keys |max| and |mid| */
static const curve25519_key curve25519_shared = {
	0x78,0x0e,0x63,0xa6,0x58,0x5c,0x6d,0x56,
	0xf1,0xa0,0x18,0x2d,0xec,0xe6,0x96,0x3b,
	0x5b,0x4d,0x63,0x08,0x7b,0xf9,0x19,0x0e,
	0x3a,0x77,0xf5,0x27,0x9c,0xd7,0x8b,0x44
};


static void
test_main(void) {
	int i;
	static const curve25519_key max = {
		255,255,255,255,255,255,255,255,
		255,255,255,255,255,255,255,255,
		255,255,255,255,255,255,255,255,
		255,255,255,255,255,255,255,255
	};
	static const curve25519_key mid = {
		127,127,127,127,127,127,127,127,
		127,127,127,127,127,127,127,127,
		127,127,127,127,127,127,127,127,
		127,127,127,127,127,127,127,127
	};
	curve25519_key pk[2];
	curve25519_key shared[2];
	uint64_t ticks, curveticks = maxticks;

	curve25519_donna(pk[0], max, max);
	for (i = 0; i < 1023; i++)
		curve25519_donna(pk[(i & 1) ^ 1], pk[i & 1], max);
	curve25519_donna_basepoint(pk[0], pk[1]);
	curveassert_equal(curve25519_expected, pk[0], sizeof(curve25519_key), "curve25519 sanity test failed to generate correct value");

	curve25519_donna_basepoint(pk[0], max);
	curve25519_donna_basepoint(pk[1], mid);
	curve25519_donna(shared[0], max, pk[1]);
	curve25519_donna(shared[1], mid, pk[0]);
	curveassert_equal(curve25519_shared, shared[0], sizeof(curve25519_key), "curve25519 failed to generate the same shared key (1)");
	curveassert_equal(curve25519_shared, shared[1], sizeof(curve25519_key), "curve25519 failed to generate the same shared key (2)");

	for (i = 0; i < 2048; i++) {
		timeit(curve25519_donna(pk[1], pk[0], max), curveticks);
	}

	printf("%.0f ticks/curve25519 scalarmult\n", (double)curveticks);
}

int
main(void) {
	test_main();
	return 0;
}

