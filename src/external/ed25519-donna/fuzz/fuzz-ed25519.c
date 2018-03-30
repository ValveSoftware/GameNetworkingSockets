#if defined(_WIN32)
	#include <windows.h>
	#include <wincrypt.h>
	typedef unsigned int uint32_t;
#else
	#include <stdint.h>
#endif

#include <string.h>
#include <stdio.h>

#include "ed25519-donna.h"
#include "ed25519-ref10.h"

static void
print_diff(const char *desc, const unsigned char *a, const unsigned char *b, size_t len) {
	size_t p = 0;
	unsigned char diff;
	printf("%s diff:\n", desc);
	while (len--) {
		diff = *a++ ^ *b++;
		if (!diff)
			printf("____,");
		else
			printf("0x%02x,", diff);
		if ((++p & 15) == 0)
			printf("\n");
	}
	printf("\n");
}

static void
print_bytes(const char *desc, const unsigned char *bytes, size_t len) {
	size_t p = 0;
	printf("%s:\n", desc);
	while (len--) {
		printf("0x%02x,", *bytes++);
		if ((++p & 15) == 0)
			printf("\n");
	}
	printf("\n");
}


/* chacha20/12 prng */
void
prng(unsigned char *out, size_t bytes) {
	static uint32_t state[16];
	static int init = 0;
	uint32_t x[16], t;
	size_t i;

	if (!init) {
	#if defined(_WIN32)
		HCRYPTPROV csp = NULL;
		if (!CryptAcquireContext(&csp, 0, 0, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
			printf("CryptAcquireContext failed\n");
			exit(1);
		}
		if (!CryptGenRandom(csp, (DWORD)sizeof(state), (BYTE*)state)) {
			printf("CryptGenRandom failed\n");
			exit(1);
		}
		CryptReleaseContext(csp, 0);
	#else
		FILE *f = NULL;
		f = fopen("/dev/urandom", "rb");
		if (!f) {
			printf("failed to open /dev/urandom\n");
			exit(1);
		}
		if (fread(state, sizeof(state), 1, f) != 1) {
			printf("read error on /dev/urandom\n");
			exit(1);
		}
	#endif
		init = 1;
	}

	while (bytes) {
		for (i = 0; i < 16; i++) x[i] = state[i];

		#define rotl32(x,k) ((x << k) | (x >> (32 - k)))
		#define quarter(a,b,c,d) \
			x[a] += x[b]; t = x[d]^x[a]; x[d] = rotl32(t,16); \
			x[c] += x[d]; t = x[b]^x[c]; x[b] = rotl32(t,12); \
			x[a] += x[b]; t = x[d]^x[a]; x[d] = rotl32(t, 8); \
			x[c] += x[d]; t = x[b]^x[c]; x[b] = rotl32(t, 7);

		for (i = 0; i < 12; i += 2) {
			quarter( 0, 4, 8,12)
			quarter( 1, 5, 9,13)
			quarter( 2, 6,10,14)
			quarter( 3, 7,11,15)
			quarter( 0, 5,10,15)
			quarter( 1, 6,11,12)
			quarter( 2, 7, 8,13)
			quarter( 3, 4, 9,14)
		};

		if (bytes <= 64) {
			memcpy(out, x, bytes);
			bytes = 0;
		} else {
			memcpy(out, x, 64);
			bytes -= 64;
			out += 64;
		}

		/* don't need a nonce, so last 4 words are the counter. 2^136 bytes can be generated */
		if (!++state[12]) if (!++state[13]) if (!++state[14]) ++state[15];
	}
}

typedef struct random_data_t {
	unsigned char sk[32];
	unsigned char m[128];
} random_data;

typedef struct generated_data_t {
	unsigned char pk[32];
	unsigned char sig[64];
	int valid;
} generated_data;

static void
print_generated(const char *desc, generated_data *g) {
	printf("%s:\n", desc);
	print_bytes("pk", g->pk, 32);
	print_bytes("sig", g->sig, 64);
	printf("valid: %s\n\n", g->valid ? "no" : "yes");
}

static void
print_generated_diff(const char *desc, const generated_data *base, generated_data *g) {
	printf("%s:\n", desc);
	print_diff("pk", base->pk, g->pk, 32);
	print_diff("sig", base->sig, g->sig, 64);
	printf("valid: %s\n\n", (base->valid == g->valid) ? "___" : (g->valid ? "no" : "yes"));
}

int main() {
	const size_t rndmax = 128;
	static random_data rnd[128];
	static generated_data gen[3];
	random_data *r;
	generated_data *g;
	unsigned long long dummylen;
	unsigned char dummysk[64];
	unsigned char dummymsg[2][128+64];
	size_t rndi, geni, i, j;
	uint64_t ctr;

	printf("fuzzing: ");
	printf(" ref10");
	printf(" ed25519-donna");
#if defined(ED25519_SSE2)
	printf(" ed25519-donna-sse2");
#endif
	printf("\n\n");

	for (ctr = 0, rndi = rndmax;;ctr++) {
		if (rndi == rndmax) {
			prng((unsigned char *)rnd, sizeof(rnd));
			rndi = 0;
		}
		r = &rnd[rndi++];

		/* ref10, lots of horrible gymnastics to work around the wonky api */
		geni = 0;
		g = &gen[geni++];
		memcpy(dummysk, r->sk, 32); /* pk is appended to the sk, need to copy the sk to a larger buffer */
		crypto_sign_pk_ref10(dummysk + 32, dummysk);
		memcpy(g->pk, dummysk + 32, 32); 
		crypto_sign_ref10(dummymsg[0], &dummylen, r->m, 128, dummysk);
		memcpy(g->sig, dummymsg[0], 64); /* sig is placed in front of the signed message */
		g->valid = crypto_sign_open_ref10(dummymsg[1], &dummylen, dummymsg[0], 128 + 64, g->pk);

		/* ed25519-donna */
		g = &gen[geni++];
		ed25519_publickey(r->sk, g->pk);
		ed25519_sign(r->m, 128, r->sk, g->pk, g->sig);
		g->valid = ed25519_sign_open(r->m, 128, g->pk, g->sig);

		#if defined(ED25519_SSE2)
			/* ed25519-donna-sse2 */
			g = &gen[geni++];
			ed25519_publickey_sse2(r->sk, g->pk);
			ed25519_sign_sse2(r->m, 128, r->sk, g->pk, g->sig);
			g->valid = ed25519_sign_open_sse2(r->m, 128, g->pk, g->sig);
		#endif

		/* compare implementations 1..geni against the reference */
		for (i = 1; i < geni; i++) {
			if (memcmp(&gen[0], &gen[i], sizeof(generated_data)) != 0) {
				printf("\n\n");
				print_bytes("sk", r->sk, 32);
				print_bytes("m", r->m, 128);
				print_generated("ref10", &gen[0]);
				print_generated_diff("ed25519-donna", &gen[0], &gen[1]);
				#if defined(ED25519_SSE2)
					print_generated_diff("ed25519-donna-sse2", &gen[0], &gen[2]);
				#endif
				exit(1);
			}
		}

		/* print out status */
		if (ctr && (ctr % 0x1000 == 0)) {
			printf(".");
			if ((ctr % 0x20000) == 0) {
				printf(" [");
				for (i = 0; i < 8; i++)
					printf("%02x", (unsigned char)(ctr >> ((7 - i) * 8)));
				printf("]\n");
			}
		}
	}
}