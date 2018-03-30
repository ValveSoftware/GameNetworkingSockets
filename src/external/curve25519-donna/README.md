[curve25519](http://cr.yp.to/ecdh.html) is an elliptic curve, developed by 
[Dan Bernstein](http://cr.yp.to/djb.html), for fast 
[Diffie-Hellman](http://en.wikipedia.org/wiki/Diffie-Hellman) key agreement. 
DJB's [original implementation](http://cr.yp.to/ecdh.html) was written in a 
language of his own devising called [qhasm](http://cr.yp.to/qhasm.html). 
The original qhasm source isn't available, only the x86 32-bit assembly output.

This project provides performant, portable 32-bit & 64-bit implementations. 
All implementations are of course constant time in regard to secret data.

#### Performance 

Compilers versions are gcc 4.6.3, icc 13.1.1, clang 3.4-1~exp1.

Counts are in thousands of cycles.

Note that SSE2 performance may be less impressive on AMD & older CPUs with slower SSE ops!

##### E5200 @ 2.5ghz, march=core2

<table>
<thead><tr><th>Version</th><th>gcc</th><th>icc</th><th>clang</th></tr></thead>
<tbody>
<tr><td>64-bit SSE2  </td><td>  278k</td><td>  265k</td><td>  302k</td></tr>
<tr><td>64-bit       </td><td>  273k</td><td>  271k</td><td>  377k</td></tr>
<tr><td>32-bit SSE2  </td><td>  304k</td><td>  289k</td><td>  317k</td></tr>
<tr><td>32-bit       </td><td> 1417k</td><td>  845k</td><td>  981k</td></tr>
</tbody>
</table>

##### E3-1270 @ 3.4ghz, march=corei7-avx

<table>
<thead><tr><th>Version</th><th>gcc</th><th>icc</th><th>clang</th></tr></thead>
<tbody>
<tr><td>64-bit       </td><td>  201k</td><td>  192k</td><td>  233k</td></tr>
<tr><td>64-bit SSE2  </td><td>  201k</td><td>  201k</td><td>  261k</td></tr>
<tr><td>32-bit SSE2  </td><td>  238k</td><td>  225k</td><td>  250k</td></tr>
<tr><td>32-bit       </td><td> 1293k</td><td>  822k</td><td>  848k</td></tr>
</tbody>
</table>

#### Compilation

No configuration is needed.

##### 32-bit

	gcc curve25519.c -m32 -O3 -c

##### 64-bit

	gcc curve25519.c -m64 -O3 -c

##### SSE2

	gcc curve25519.c -m32 -O3 -c -DCURVE25519_SSE2 -msse2
	gcc curve25519.c -m64 -O3 -c -DCURVE25519_SSE2

clang, icc, and msvc are also supported

##### Named Versions

Define CURVE25519_SUFFIX to append a suffix to public functions, e.g. 
`-DCURVE25519_SUFFIX=_sse2` to create curve25519_donna_sse2 and 
curve25519_donna_basepoint_sse2.

#### Usage

To use the code, link against `curve25519.o` and:

	#include "curve25519.h"

To generate a private/secret key, generate 32 cryptographically random bytes: 

	curve25519_key sk;
	randombytes(sk, sizeof(curve25519_key));

Manual clamping is not needed, and it is actually not possible to use unclamped
keys due to the code taking advantage of the clamped bits internally.

To generate the public key from the private/secret key:

	curve25519_key pk;
	curve25519_donna_basepoint(pk, sk);

To generate a shared key with your private/secret key and someone elses public key:

	curve25519_key shared;
	curve25519_donna(shared, mysk, yourpk);

And hash `shared` with a cryptographic hash before using, or e.g. pass `shared` through
HSalsa20/HChacha as NaCl does.

#### Testing

Fuzzing against a reference implemenation is now available. See [fuzz/README](fuzz/README.md).

Building `curve25519.c` and linking with `test.c` will run basic sanity tests and benchmark curve25519_donna.

#### Papers

[djb's curve25519 paper](http://cr.yp.to/ecdh/curve25519-20060209.pdf)

#### License

Public Domain, or MIT