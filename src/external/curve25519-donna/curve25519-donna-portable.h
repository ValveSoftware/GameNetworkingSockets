#include "curve25519-donna-portable-identify.h"

#define mul32x32_64(a,b) (((uint64_t)(a))*(b))

#ifdef ALIGN
#undef ALIGN
#endif

/* platform */
#if defined(COMPILER_MSVC)
	#include <intrin.h>
	#if !defined(_DEBUG)
		#undef mul32x32_64
		#define mul32x32_64(a,b) __emulu(a,b)
	#endif
	#undef inline
	#define inline __forceinline
	#define DONNA_INLINE __forceinline
	#define DONNA_NOINLINE __declspec(noinline)
	#define ALIGN(x) __declspec(align(x))
	#define ROTL32(a,b) _rotl(a,b)
	#define ROTR32(a,b) _rotr(a,b)
#else
	#include <sys/param.h>
	#define DONNA_INLINE inline __attribute__((always_inline))
	#define DONNA_NOINLINE __attribute__((noinline))
	#define ALIGN(x) __attribute__((aligned(x)))
	#define ROTL32(a,b) (((a) << (b)) | ((a) >> (32 - b)))
	#define ROTR32(a,b) (((a) >> (b)) | ((a) << (32 - b)))
#endif

/* uint128_t */
#if defined(CPU_64BITS) && !defined(ED25519_FORCE_32BIT)
	#if defined(COMPILER_CLANG) && (COMPILER_CLANG >= 30100)
		#define HAVE_NATIVE_UINT128
		typedef unsigned __int128 uint128_t;
	#elif defined(COMPILER_MSVC)
		#define HAVE_UINT128
		typedef struct uint128_t {
			uint64_t lo, hi;
		} uint128_t;
		#define mul64x64_128(out,a,b) out.lo = _umul128(a,b,&out.hi);
		#define shr128_pair(out,hi,lo,shift) out = __shiftright128(lo, hi, shift);
		#define shl128_pair(out,hi,lo,shift) out = __shiftleft128(lo, hi, shift);
		#define shr128(out,in,shift) shr128_pair(out, in.hi, in.lo, shift)
		#define shl128(out,in,shift) shl128_pair(out, in.hi, in.lo, shift)
		#define add128(a,b) { uint64_t p = a.lo; a.lo += b.lo; a.hi += b.hi + (a.lo < p); }
		#define add128_64(a,b) { uint64_t p = a.lo; a.lo += b; a.hi += (a.lo < p); }
		#define lo128(a) (a.lo)
		#define hi128(a) (a.hi)
	#elif defined(COMPILER_GCC) && !defined(HAVE_NATIVE_UINT128)
		#if defined(__SIZEOF_INT128__)
			#define HAVE_NATIVE_UINT128
			typedef unsigned __int128 uint128_t;
		#elif (COMPILER_GCC >= 40400)
			#define HAVE_NATIVE_UINT128
			typedef unsigned uint128_t __attribute__((mode(TI)));
		#elif defined(CPU_X86_64)
			#define HAVE_UINT128
			typedef struct uint128_t {
				uint64_t lo, hi;
			} uint128_t;
			#define mul64x64_128(out,a,b) __asm__ ("mulq %3" : "=a" (out.lo), "=d" (out.hi) : "a" (a), "rm" (b));
			#define shr128_pair(out,hi,lo,shift) __asm__ ("shrdq %2,%1,%0" : "+r" (lo) : "r" (hi), "J" (shift)); out = lo;
			#define shl128_pair(out,hi,lo,shift) __asm__ ("shldq %2,%1,%0" : "+r" (hi) : "r" (lo), "J" (shift)); out = hi;
			#define shr128(out,in,shift) shr128_pair(out,in.hi, in.lo, shift)
			#define shl128(out,in,shift) shl128_pair(out,in.hi, in.lo, shift)
			#define add128(a,b) __asm__ ("addq %4,%2; adcq %5,%3" : "=r" (a.hi), "=r" (a.lo) : "1" (a.lo), "0" (a.hi), "rm" (b.lo), "rm" (b.hi) : "cc");
			#define add128_64(a,b) __asm__ ("addq %4,%2; adcq $0,%3" : "=r" (a.hi), "=r" (a.lo) : "1" (a.lo), "0" (a.hi), "rm" (b) : "cc");
			#define lo128(a) (a.lo)
			#define hi128(a) (a.hi)
		#endif
	#endif

	#if defined(HAVE_NATIVE_UINT128)
		#define HAVE_UINT128
		#define mul64x64_128(out,a,b) out = (uint128_t)a * b;
		#define shr128_pair(out,hi,lo,shift) out = (uint64_t)((((uint128_t)hi << 64) | lo) >> (shift));
		#define shl128_pair(out,hi,lo,shift) out = (uint64_t)(((((uint128_t)hi << 64) | lo) << (shift)) >> 64);
		#define shr128(out,in,shift) out = (uint64_t)(in >> (shift));
		#define shl128(out,in,shift) out = (uint64_t)((in << shift) >> 64);
		#define add128(a,b) a += b;
		#define add128_64(a,b) a += (uint64_t)b;
		#define lo128(a) ((uint64_t)a)
		#define hi128(a) ((uint64_t)(a >> 64))
	#endif

	#if !defined(HAVE_UINT128)
		#error Need a uint128_t implementation!
	#endif
#endif

#include <stdlib.h>
#include <string.h>


