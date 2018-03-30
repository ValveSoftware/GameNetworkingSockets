#if defined(_WIN32)
	#include <windows.h>
	#include <wincrypt.h>
	typedef unsigned int uint32_t;
	typedef unsigned __int64 uint64_t;
#else
	#include <stdint.h>
#endif

#include <string.h>
#include <stdio.h>

#include "ed25519-donna.h"
#include "curve25519-ref10.h"

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
	printf("\n\n");
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
	printf("\n\n");
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
		HCRYPTPROV csp;
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



int main() {
	const size_t skmax = 1024;
	static unsigned char sk[1024][32];
	unsigned char pk[3][32];
	unsigned char *skp;
	size_t ski, pki, i;
	uint64_t ctr;

	printf("fuzzing: ");
	printf(" ref10");
	printf(" curved25519");
#if defined(ED25519_SSE2)
	printf(" curved25519-sse2");
#endif
	printf("\n\n");

	for (ctr = 0, ski = skmax;;ctr++) {
		if (ski == skmax) {
			prng((unsigned char *)sk, sizeof(sk));
			ski = 0;
		}
		skp = sk[ski++];

		pki = 0;
		crypto_scalarmult_base_ref10(pk[pki++], skp);
		curved25519_scalarmult_basepoint(pk[pki++],  skp);
		#if defined(ED25519_SSE2)
			curved25519_scalarmult_basepoint_sse2(pk[pki++], skp);
		#endif

		for (i = 1; i < pki; i++) {
			if (memcmp(pk[0], pk[i], 32) != 0) {
				printf("\n\n");
				print_bytes("sk",  skp, 32);
				print_bytes("ref10", pk[0], 32);
				print_diff("curved25519", pk[0], pk[1], 32);
				#if defined(ED25519_SSE2)
					print_diff("curved25519-sse2", pk[0], pk[2], 32);
				#endif
				exit(1);
			}
		}

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

