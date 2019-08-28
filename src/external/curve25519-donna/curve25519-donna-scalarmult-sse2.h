
/* Calculates nQ where Q is the x-coordinate of a point on the curve
 *
 *   mypublic: the packed little endian x coordinate of the resulting curve point
 *   n: a little endian, 32-byte number
 *   basepoint: a packed little endian point of the curve
 */
static void
curve25519_scalarmult_donna(curve25519_key mypublic, const curve25519_key n, const curve25519_key basepoint) {
	bignum25519 ALIGN(16) nqx = {1}, nqpqz = {1}, nqz = {0}, nqpqx, zmone;
	packed32bignum25519 qx, qz, pqz, pqx;
	packed64bignum25519 nq, sq, sqscalar, prime, primex, primez, nqpq;
	bignum25519mulprecomp preq;
	uint32_t bit, lastbit, i;

	curve25519_expand(nqpqx, basepoint);
	curve25519_mul_precompute(&preq, nqpqx);

	/* do bits 254..3 */
	for (i = 254, lastbit = 0; i >= 3; i--) {
		bit = (n[i/8] >> (i & 7)) & 1;
		curve25519_swap_conditional(nqx, nqpqx, bit ^ lastbit);
		curve25519_swap_conditional(nqz, nqpqz, bit ^ lastbit);
		lastbit = bit;

		curve25519_tangle32(qx, nqx, nqpqx); /* qx = [nqx,nqpqx] */
		curve25519_tangle32(qz, nqz, nqpqz); /* qz = [nqz,nqpqz] */

		curve25519_add_packed32(pqx, qx, qz); /* pqx = [nqx+nqz,nqpqx+nqpqz] */
		curve25519_sub_packed32(pqz, qx, qz); /* pqz = [nqx-nqz,nqpqx-nqpqz] */

		curve25519_make_nqpq(primex, primez, pqx, pqz); /* primex = [nqx+nqz,nqpqx+nqpqz], primez = [nqpqx-nqpqz,nqx-nqz] */
		curve25519_mul_packed64(prime, primex, primez); /* prime = [nqx+nqz,nqpqx+nqpqz] * [nqpqx-nqpqz,nqx-nqz] */
		curve25519_addsub_packed64(prime); /* prime = [prime.x+prime.z,prime.x-prime.z] */
		curve25519_square_packed64(nqpq, prime); /* nqpq = prime^2 */
		curve25519_untangle64(nqpqx, nqpqz, nqpq);
		curve25519_mul_precomputed(nqpqz, nqpqz, &preq); /* nqpqz = nqpqz * q */

		/* (((sq.x-sq.z)*121665)+sq.x) * (sq.x-sq.z) is equivalent to (sq.x*121666-sq.z*121665) * (sq.x-sq.z) */
		curve25519_make_nq(nq, pqx, pqz); /* nq = [nqx+nqz,nqx-nqz] */
		curve25519_square_packed64(sq, nq); /* sq = nq^2 */
		curve25519_121665_packed64(sqscalar, sq); /* sqscalar = sq * [121666,121665] */
		curve25519_final_nq(nq, sq, sqscalar); /* nq = [sq.x,sqscalar.x-sqscalar.z] * [sq.z,sq.x-sq.z] */
		curve25519_untangle64(nqx, nqz, nq);
	};

	/* it's possible to get rid of this swap with the swap in the above loop 
	   at the bottom instead of the top, but compilers seem to optimize better this way */
	curve25519_swap_conditional(nqx, nqpqx, bit);
	curve25519_swap_conditional(nqz, nqpqz, bit);

	/* do bits 2..0 */
	for (i = 0; i < 3; i++) {
		curve25519_compute_nq(nq, nqx, nqz);
		curve25519_square_packed64(sq, nq); /* sq = nq^2 */
		curve25519_121665_packed64(sqscalar, sq); /* sqscalar = sq * [121666,121665] */
		curve25519_final_nq(nq, sq, sqscalar); /* nq = [sq.x,sqscalar.x-sqscalar.z] * [sq.z,sq.x-sq.z] */
		curve25519_untangle64(nqx, nqz, nq);
	}

	curve25519_recip(zmone, nqz);
	curve25519_mul(nqz, nqx, zmone);
	curve25519_contract(mypublic, nqz);
}

