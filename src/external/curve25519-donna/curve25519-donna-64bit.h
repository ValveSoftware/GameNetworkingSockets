typedef uint64_t bignum25519[5];

static const uint64_t reduce_mask_51 = ((uint64_t)1 << 51) - 1;
static const uint64_t reduce_mask_52 = ((uint64_t)1 << 52) - 1;

/* out = in */
DONNA_INLINE static void
curve25519_copy(bignum25519 out, const bignum25519 in) {
	out[0] = in[0];
	out[1] = in[1];
	out[2] = in[2];
	out[3] = in[3];
	out[4] = in[4];
}

/* out = a + b */
DONNA_INLINE static void
curve25519_add(bignum25519 out, const bignum25519 a, const bignum25519 b) {
	out[0] = a[0] + b[0];
	out[1] = a[1] + b[1];
	out[2] = a[2] + b[2];
	out[3] = a[3] + b[3];
	out[4] = a[4] + b[4];
}

static const uint64_t two54m152 = (((uint64_t)1) << 54) - 152;
static const uint64_t two54m8 = (((uint64_t)1) << 54) - 8;

/* out = a - b */
DONNA_INLINE static void
curve25519_sub(bignum25519 out, const bignum25519 a, const bignum25519 b) {
	out[0] = a[0] + two54m152 - b[0];
	out[1] = a[1] + two54m8 - b[1];
	out[2] = a[2] + two54m8 - b[2];
	out[3] = a[3] + two54m8 - b[3];
	out[4] = a[4] + two54m8 - b[4];
}


/* out = (in * scalar) */
DONNA_INLINE static void
curve25519_scalar_product(bignum25519 out, const bignum25519 in, const uint64_t scalar) {
  uint128_t a;
  uint64_t c;

#if defined(HAVE_NATIVE_UINT128)
	a = ((uint128_t) in[0]) * scalar;     out[0] = (uint64_t)a & reduce_mask_51; c = (uint64_t)(a >> 51);
	a = ((uint128_t) in[1]) * scalar + c; out[1] = (uint64_t)a & reduce_mask_51; c = (uint64_t)(a >> 51);
	a = ((uint128_t) in[2]) * scalar + c; out[2] = (uint64_t)a & reduce_mask_51; c = (uint64_t)(a >> 51);
	a = ((uint128_t) in[3]) * scalar + c; out[3] = (uint64_t)a & reduce_mask_51; c = (uint64_t)(a >> 51);
	a = ((uint128_t) in[4]) * scalar + c; out[4] = (uint64_t)a & reduce_mask_51; c = (uint64_t)(a >> 51);
	                                      out[0] += c * 19;
#else
	mul64x64_128(a, in[0], scalar)                  out[0] = lo128(a) & reduce_mask_51; shr128(c, a, 51);
	mul64x64_128(a, in[1], scalar) add128_64(a, c)  out[1] = lo128(a) & reduce_mask_51; shr128(c, a, 51);
	mul64x64_128(a, in[2], scalar) add128_64(a, c)  out[2] = lo128(a) & reduce_mask_51; shr128(c, a, 51);
	mul64x64_128(a, in[3], scalar) add128_64(a, c)  out[3] = lo128(a) & reduce_mask_51; shr128(c, a, 51);
	mul64x64_128(a, in[4], scalar) add128_64(a, c)  out[4] = lo128(a) & reduce_mask_51; shr128(c, a, 51);
	                                                out[0] += c * 19;
#endif
}

/* out = a * b */
DONNA_INLINE static void
curve25519_mul(bignum25519 out, const bignum25519 a, const bignum25519 b) {
#if !defined(HAVE_NATIVE_UINT128)
	uint128_t mul;
#endif
	uint128_t t[5];
	uint64_t r0,r1,r2,r3,r4,s0,s1,s2,s3,s4,c;

	r0 = b[0];
	r1 = b[1];
	r2 = b[2];
	r3 = b[3];
	r4 = b[4];

	s0 = a[0];
	s1 = a[1];
	s2 = a[2];
	s3 = a[3];
	s4 = a[4];

#if defined(HAVE_NATIVE_UINT128)
	t[0]  =  ((uint128_t) r0) * s0;
	t[1]  =  ((uint128_t) r0) * s1 + ((uint128_t) r1) * s0;
	t[2]  =  ((uint128_t) r0) * s2 + ((uint128_t) r2) * s0 + ((uint128_t) r1) * s1;
	t[3]  =  ((uint128_t) r0) * s3 + ((uint128_t) r3) * s0 + ((uint128_t) r1) * s2 + ((uint128_t) r2) * s1;
	t[4]  =  ((uint128_t) r0) * s4 + ((uint128_t) r4) * s0 + ((uint128_t) r3) * s1 + ((uint128_t) r1) * s3 + ((uint128_t) r2) * s2;
#else
	mul64x64_128(t[0], r0, s0)
	mul64x64_128(t[1], r0, s1) mul64x64_128(mul, r1, s0) add128(t[1], mul)
	mul64x64_128(t[2], r0, s2) mul64x64_128(mul, r2, s0) add128(t[2], mul) mul64x64_128(mul, r1, s1) add128(t[2], mul)
	mul64x64_128(t[3], r0, s3) mul64x64_128(mul, r3, s0) add128(t[3], mul) mul64x64_128(mul, r1, s2) add128(t[3], mul) mul64x64_128(mul, r2, s1) add128(t[3], mul)
	mul64x64_128(t[4], r0, s4) mul64x64_128(mul, r4, s0) add128(t[4], mul) mul64x64_128(mul, r3, s1) add128(t[4], mul) mul64x64_128(mul, r1, s3) add128(t[4], mul) mul64x64_128(mul, r2, s2) add128(t[4], mul)
#endif

	r1 *= 19;
	r2 *= 19;
	r3 *= 19;
	r4 *= 19;

#if defined(HAVE_NATIVE_UINT128)
	t[0] += ((uint128_t) r4) * s1 + ((uint128_t) r1) * s4 + ((uint128_t) r2) * s3 + ((uint128_t) r3) * s2;
	t[1] += ((uint128_t) r4) * s2 + ((uint128_t) r2) * s4 + ((uint128_t) r3) * s3;
	t[2] += ((uint128_t) r4) * s3 + ((uint128_t) r3) * s4;
	t[3] += ((uint128_t) r4) * s4;
#else
	mul64x64_128(mul, r4, s1) add128(t[0], mul) mul64x64_128(mul, r1, s4) add128(t[0], mul) mul64x64_128(mul, r2, s3) add128(t[0], mul) mul64x64_128(mul, r3, s2) add128(t[0], mul)
	mul64x64_128(mul, r4, s2) add128(t[1], mul) mul64x64_128(mul, r2, s4) add128(t[1], mul) mul64x64_128(mul, r3, s3) add128(t[1], mul)
	mul64x64_128(mul, r4, s3) add128(t[2], mul) mul64x64_128(mul, r3, s4) add128(t[2], mul)
	mul64x64_128(mul, r4, s4) add128(t[3], mul)
#endif

	                     r0 = lo128(t[0]) & reduce_mask_51; shr128(c, t[0], 51);
	add128_64(t[1], c)   r1 = lo128(t[1]) & reduce_mask_51; shr128(c, t[1], 51);
	add128_64(t[2], c)   r2 = lo128(t[2]) & reduce_mask_51; shr128(c, t[2], 51);
	add128_64(t[3], c)   r3 = lo128(t[3]) & reduce_mask_51; shr128(c, t[3], 51);
	add128_64(t[4], c)   r4 = lo128(t[4]) & reduce_mask_51; shr128(c, t[4], 51);
	r0 +=   c * 19; c = r0 >> 51; r0 = r0 & reduce_mask_51;
	r1 +=   c;

	out[0] = r0;
	out[1] = r1;
	out[2] = r2;
	out[3] = r3;
	out[4] = r4;
}

/* out = in^(2 * count) */
DONNA_INLINE static void
curve25519_square_times(bignum25519 out, const bignum25519 in, uint64_t count) {
#if !defined(HAVE_NATIVE_UINT128)
	uint128_t mul;
#endif
	uint128_t t[5];
	uint64_t r0,r1,r2,r3,r4,c;
	uint64_t d0,d1,d2,d4,d419;

	r0 = in[0];
	r1 = in[1];
	r2 = in[2];
	r3 = in[3];
	r4 = in[4];

	do {
		d0 = r0 * 2;
		d1 = r1 * 2;
		d2 = r2 * 2 * 19;
		d419 = r4 * 19;
		d4 = d419 * 2;

#if defined(HAVE_NATIVE_UINT128)
		t[0] = ((uint128_t) r0) * r0 + ((uint128_t) d4) * r1 + (((uint128_t) d2) * (r3     ));
		t[1] = ((uint128_t) d0) * r1 + ((uint128_t) d4) * r2 + (((uint128_t) r3) * (r3 * 19));
		t[2] = ((uint128_t) d0) * r2 + ((uint128_t) r1) * r1 + (((uint128_t) d4) * (r3     ));
		t[3] = ((uint128_t) d0) * r3 + ((uint128_t) d1) * r2 + (((uint128_t) r4) * (d419   ));
		t[4] = ((uint128_t) d0) * r4 + ((uint128_t) d1) * r3 + (((uint128_t) r2) * (r2     ));
#else
		mul64x64_128(t[0], r0, r0) mul64x64_128(mul, d4, r1) add128(t[0], mul) mul64x64_128(mul, d2,      r3) add128(t[0], mul)
		mul64x64_128(t[1], d0, r1) mul64x64_128(mul, d4, r2) add128(t[1], mul) mul64x64_128(mul, r3, r3 * 19) add128(t[1], mul)
		mul64x64_128(t[2], d0, r2) mul64x64_128(mul, r1, r1) add128(t[2], mul) mul64x64_128(mul, d4,      r3) add128(t[2], mul)
		mul64x64_128(t[3], d0, r3) mul64x64_128(mul, d1, r2) add128(t[3], mul) mul64x64_128(mul, r4,    d419) add128(t[3], mul)
		mul64x64_128(t[4], d0, r4) mul64x64_128(mul, d1, r3) add128(t[4], mul) mul64x64_128(mul, r2,      r2) add128(t[4], mul)
#endif

		                     r0 = lo128(t[0]) & reduce_mask_51; shr128(c, t[0], 51);
		add128_64(t[1], c)   r1 = lo128(t[1]) & reduce_mask_51; shr128(c, t[1], 51);
		add128_64(t[2], c)   r2 = lo128(t[2]) & reduce_mask_51; shr128(c, t[2], 51);
		add128_64(t[3], c)   r3 = lo128(t[3]) & reduce_mask_51; shr128(c, t[3], 51);
		add128_64(t[4], c)   r4 = lo128(t[4]) & reduce_mask_51; shr128(c, t[4], 51);
		r0 +=   c * 19; c = r0 >> 51; r0 = r0 & reduce_mask_51;
		r1 +=   c;
	} while(--count);

	out[0] = r0;
	out[1] = r1;
	out[2] = r2;
	out[3] = r3;
	out[4] = r4;
}

DONNA_INLINE static void
curve25519_square(bignum25519 out, const bignum25519 in) {
#if !defined(HAVE_NATIVE_UINT128)
	uint128_t mul;
#endif
	uint128_t t[5];
	uint64_t r0,r1,r2,r3,r4,c;
	uint64_t d0,d1,d2,d4,d419;

	r0 = in[0];
	r1 = in[1];
	r2 = in[2];
	r3 = in[3];
	r4 = in[4];

	d0 = r0 * 2;
	d1 = r1 * 2;
	d2 = r2 * 2 * 19;
	d419 = r4 * 19;
	d4 = d419 * 2;

#if defined(HAVE_NATIVE_UINT128)
	t[0] = ((uint128_t) r0) * r0 + ((uint128_t) d4) * r1 + (((uint128_t) d2) * (r3     ));
	t[1] = ((uint128_t) d0) * r1 + ((uint128_t) d4) * r2 + (((uint128_t) r3) * (r3 * 19));
	t[2] = ((uint128_t) d0) * r2 + ((uint128_t) r1) * r1 + (((uint128_t) d4) * (r3     ));
	t[3] = ((uint128_t) d0) * r3 + ((uint128_t) d1) * r2 + (((uint128_t) r4) * (d419   ));
	t[4] = ((uint128_t) d0) * r4 + ((uint128_t) d1) * r3 + (((uint128_t) r2) * (r2     ));
#else
	mul64x64_128(t[0], r0, r0) mul64x64_128(mul, d4, r1) add128(t[0], mul) mul64x64_128(mul, d2,      r3) add128(t[0], mul)
	mul64x64_128(t[1], d0, r1) mul64x64_128(mul, d4, r2) add128(t[1], mul) mul64x64_128(mul, r3, r3 * 19) add128(t[1], mul)
	mul64x64_128(t[2], d0, r2) mul64x64_128(mul, r1, r1) add128(t[2], mul) mul64x64_128(mul, d4,      r3) add128(t[2], mul)
	mul64x64_128(t[3], d0, r3) mul64x64_128(mul, d1, r2) add128(t[3], mul) mul64x64_128(mul, r4,    d419) add128(t[3], mul)
	mul64x64_128(t[4], d0, r4) mul64x64_128(mul, d1, r3) add128(t[4], mul) mul64x64_128(mul, r2,      r2) add128(t[4], mul)
#endif

	                     r0 = lo128(t[0]) & reduce_mask_51; shr128(c, t[0], 51);
	add128_64(t[1], c)   r1 = lo128(t[1]) & reduce_mask_51; shr128(c, t[1], 51);
	add128_64(t[2], c)   r2 = lo128(t[2]) & reduce_mask_51; shr128(c, t[2], 51);
	add128_64(t[3], c)   r3 = lo128(t[3]) & reduce_mask_51; shr128(c, t[3], 51);
	add128_64(t[4], c)   r4 = lo128(t[4]) & reduce_mask_51; shr128(c, t[4], 51);
	r0 +=   c * 19; c = r0 >> 51; r0 = r0 & reduce_mask_51;
	r1 +=   c;

	out[0] = r0;
	out[1] = r1;
	out[2] = r2;
	out[3] = r3;
	out[4] = r4;
}


/* Take a little-endian, 32-byte number and expand it into polynomial form */
DONNA_INLINE static void
curve25519_expand(bignum25519 out, const unsigned char *in) {
	static const union { uint8_t b[2]; uint16_t s; } endian_check = {{1,0}};
	uint64_t x0,x1,x2,x3;

	if (endian_check.s == 1) {
		x0 = *(uint64_t *)(in + 0);
		x1 = *(uint64_t *)(in + 8);
		x2 = *(uint64_t *)(in + 16);
		x3 = *(uint64_t *)(in + 24);
	} else {
		#define F(s)                         \
			((((uint64_t)in[s + 0])      ) | \
			 (((uint64_t)in[s + 1]) <<  8) | \
			 (((uint64_t)in[s + 2]) << 16) | \
			 (((uint64_t)in[s + 3]) << 24) | \
			 (((uint64_t)in[s + 4]) << 32) | \
			 (((uint64_t)in[s + 5]) << 40) | \
			 (((uint64_t)in[s + 6]) << 48) | \
			 (((uint64_t)in[s + 7]) << 56))

		x0 = F(0);
		x1 = F(8);
		x2 = F(16);
		x3 = F(24);
	}

	out[0] = x0 & reduce_mask_51; x0 = (x0 >> 51) | (x1 << 13);
	out[1] = x0 & reduce_mask_51; x1 = (x1 >> 38) | (x2 << 26);
	out[2] = x1 & reduce_mask_51; x2 = (x2 >> 25) | (x3 << 39);
	out[3] = x2 & reduce_mask_51; x3 = (x3 >> 12);
	out[4] = x3 & reduce_mask_51; /* ignore the top bit */
}

/* Take a fully reduced polynomial form number and contract it into a
 * little-endian, 32-byte array
 */
DONNA_INLINE static void
curve25519_contract(unsigned char *out, const bignum25519 input) {
	uint64_t t[5];
	uint64_t f, i;

	t[0] = input[0];
	t[1] = input[1];
	t[2] = input[2];
	t[3] = input[3];
	t[4] = input[4];

	#define curve25519_contract_carry() \
		t[1] += t[0] >> 51; t[0] &= reduce_mask_51; \
		t[2] += t[1] >> 51; t[1] &= reduce_mask_51; \
		t[3] += t[2] >> 51; t[2] &= reduce_mask_51; \
		t[4] += t[3] >> 51; t[3] &= reduce_mask_51;

	#define curve25519_contract_carry_full() curve25519_contract_carry() \
		t[0] += 19 * (t[4] >> 51); t[4] &= reduce_mask_51;

	#define curve25519_contract_carry_final() curve25519_contract_carry() \
		t[4] &= reduce_mask_51;

	curve25519_contract_carry_full()
	curve25519_contract_carry_full()

	/* now t is between 0 and 2^255-1, properly carried. */
	/* case 1: between 0 and 2^255-20. case 2: between 2^255-19 and 2^255-1. */
	t[0] += 19;
	curve25519_contract_carry_full()

	/* now between 19 and 2^255-1 in both cases, and offset by 19. */
	t[0] += 0x8000000000000 - 19;
	t[1] += 0x8000000000000 - 1;
	t[2] += 0x8000000000000 - 1;
	t[3] += 0x8000000000000 - 1;
	t[4] += 0x8000000000000 - 1;

	/* now between 2^255 and 2^256-20, and offset by 2^255. */
	curve25519_contract_carry_final()

	#define write51full(n,shift) \
		f = ((t[n] >> shift) | (t[n+1] << (51 - shift))); \
		for (i = 0; i < 8; i++, f >>= 8) *out++ = (unsigned char)f;
	#define write51(n) write51full(n,13*n)

	write51(0)
	write51(1)
	write51(2)
	write51(3)

	#undef curve25519_contract_carry
	#undef curve25519_contract_carry_full
	#undef curve25519_contract_carry_final
	#undef write51full
	#undef write51
}

/*
 * Swap the contents of [qx] and [qpx] iff @swap is non-zero
 */
DONNA_INLINE static void
curve25519_swap_conditional(bignum25519 x, bignum25519 qpx, uint64_t iswap) {
	const uint64_t swap = (uint64_t)(-(int64_t)iswap);
	uint64_t x0,x1,x2,x3,x4;

	x0 = swap & (x[0] ^ qpx[0]); x[0] ^= x0; qpx[0] ^= x0;
	x1 = swap & (x[1] ^ qpx[1]); x[1] ^= x1; qpx[1] ^= x1;
	x2 = swap & (x[2] ^ qpx[2]); x[2] ^= x2; qpx[2] ^= x2;
	x3 = swap & (x[3] ^ qpx[3]); x[3] ^= x3; qpx[3] ^= x3;
	x4 = swap & (x[4] ^ qpx[4]); x[4] ^= x4; qpx[4] ^= x4;

}

