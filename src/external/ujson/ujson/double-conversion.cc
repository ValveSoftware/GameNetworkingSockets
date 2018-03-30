// Copyright 2010 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <limits.h>
#include <math.h>

#include "double-conversion.h"

#ifndef DOUBLE_CONVERSION_UTILS_H_
#define DOUBLE_CONVERSION_UTILS_H_

#ifndef UNIMPLEMENTED
#define UNIMPLEMENTED() (abort())
#endif
#ifndef UNREACHABLE
#define UNREACHABLE()   (abort())
#endif

// Double operations detection based on target architecture.
// Linux uses a 80bit wide floating point stack on x86. This induces double
// rounding, which in turn leads to wrong results.
// An easy way to test if the floating-point operations are correct is to
// evaluate: 89255.0/1e22. If the floating-point stack is 64 bits wide then
// the result is equal to 89255e-22.
// The best way to test this, is to create a division-function and to compare
// the output of the division with the expected result. (Inlining must be
// disabled.)
// On Linux,x86 89255e-22 != Div_double(89255.0/1e22)
#if defined(_M_X64) || defined(__x86_64__) || \
    defined(__ARMEL__) || defined(__avr32__) || \
    defined(__hppa__) || defined(__ia64__) || \
    defined(__mips__) || \
    defined(__powerpc__) || defined(__ppc__) || defined(__ppc64__) || \
    defined(__sparc__) || defined(__sparc) || defined(__s390__) || \
    defined(__SH4__) || defined(__alpha__) || \
    defined(_MIPS_ARCH_MIPS32R2) || \
    defined(__AARCH64EL__)
#define DOUBLE_CONVERSION_CORRECT_DOUBLE_OPERATIONS 1
#elif defined(_M_IX86) || defined(__i386__) || defined(__i386)
#if defined(_WIN32)
// Windows uses a 64bit wide floating point stack.
#define DOUBLE_CONVERSION_CORRECT_DOUBLE_OPERATIONS 1
#else
#undef DOUBLE_CONVERSION_CORRECT_DOUBLE_OPERATIONS
#endif  // _WIN32
#else
#error Target architecture was not detected as supported by Double-Conversion.
#endif

#if defined(__GNUC__)
#define DOUBLE_CONVERSION_UNUSED __attribute__((unused))
#else
#define DOUBLE_CONVERSION_UNUSED
#endif

#if defined(_WIN32) && !defined(__MINGW32__)

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;  // NOLINT
typedef unsigned short uint16_t;  // NOLINT
typedef int int32_t;
typedef unsigned int uint32_t;
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;
// intptr_t and friends are defined in crtdefs.h through stdio.h.

#else

#include <stdint.h>

#endif

// The following macro works on both 32 and 64-bit platforms.
// Usage: instead of writing 0x1234567890123456
//      write UINT64_2PART_C(0x12345678,90123456);
#define UINT64_2PART_C(a, b) (((static_cast<uint64_t>(a) << 32) + 0x##b##u))


// The expression ARRAY_SIZE(a) is a compile-time constant of type
// size_t which represents the number of elements of the given
// array. You should only use ARRAY_SIZE on statically allocated
// arrays.
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)                                   \
  ((sizeof(a) / sizeof(*(a))) /                         \
  static_cast<size_t>(!(sizeof(a) % sizeof(*(a)))))
#endif

namespace double_conversion {

    // Returns the maximum of the two parameters.
    template <typename T>
    static T Max(T a, T b) {
        return a < b ? b : a;
    }


    // Returns the minimum of the two parameters.
    template <typename T>
    static T Min(T a, T b) {
        return a < b ? a : b;
    }

    // The type-based aliasing rule allows the compiler to assume that pointers of
    // different types (for some definition of different) never alias each other.
    // Thus the following code does not work:
    //
    // float f = foo();
    // int fbits = *(int*)(&f);
    //
    // The compiler 'knows' that the int pointer can't refer to f since the types
    // don't match, so the compiler may cache f in a register, leaving random data
    // in fbits.  Using C++ style casts makes no difference, however a pointer to
    // char data is assumed to alias any other pointer.  This is the 'memcpy
    // exception'.
    //
    // Bit_cast uses the memcpy exception to move the bits from a variable of one
    // type of a variable of another type.  Of course the end result is likely to
    // be implementation dependent.  Most compilers (gcc-4.2 and MSVC 2005)
    // will completely optimize BitCast away.
    //
    // There is an additional use for BitCast.
    // Recent gccs will warn when they see casts that may result in breakage due to
    // the type-based aliasing rule.  If you have checked that there is no breakage
    // you can use BitCast to cast one pointer type to another.  This confuses gcc
    // enough that it can no longer see that you have cast one pointer type to
    // another thus avoiding the warning.
    template <class Dest, class Source>
    inline Dest BitCast(const Source& source) {
        // Compile time assertion: sizeof(Dest) == sizeof(Source)
        // A compile error here means your Dest and Source have different sizes.
        DOUBLE_CONVERSION_UNUSED
            typedef char VerifySizesAreEqual[sizeof(Dest) == sizeof(Source) ? 1 : -1];

        Dest dest;
        memmove(&dest, &source, sizeof(dest));
        return dest;
    }

    template <class Dest, class Source>
    inline Dest BitCast(Source* source) {
        return BitCast<Dest>(reinterpret_cast<uintptr_t>(source));
    }

}  // namespace double_conversion

#endif  // DOUBLE_CONVERSION_UTILS_H_

#ifndef DOUBLE_CONVERSION_BIGNUM_H_
#define DOUBLE_CONVERSION_BIGNUM_H_

namespace double_conversion {

    class Bignum {
    public:
        // 3584 = 128 * 28. We can represent 2^3584 > 10^1000 accurately.
        // This bignum can encode much bigger numbers, since it contains an
        // exponent.
        static const int kMaxSignificantBits = 3584;

        Bignum();
        void AssignUInt16(uint16_t value);
        void AssignUInt64(uint64_t value);
        void AssignBignum(const Bignum& other);

        void AssignDecimalString(Vector<const char> value);
        void AssignHexString(Vector<const char> value);

        void AssignPowerUInt16(uint16_t base, int exponent);

        void AddUInt16(uint16_t operand);
        void AddUInt64(uint64_t operand);
        void AddBignum(const Bignum& other);
        // Precondition: this >= other.
        void SubtractBignum(const Bignum& other);

        void Square();
        void ShiftLeft(int shift_amount);
        void MultiplyByUInt32(uint32_t factor);
        void MultiplyByUInt64(uint64_t factor);
        void MultiplyByPowerOfTen(int exponent);
        void Times10() { return MultiplyByUInt32(10); }
        // Pseudocode:
        //  int result = this / other;
        //  this = this % other;
        // In the worst case this function is in O(this/other).
        uint16_t DivideModuloIntBignum(const Bignum& other);

        bool ToHexString(char* buffer, int buffer_size) const;

        // Returns
        //  -1 if a < b,
        //   0 if a == b, and
        //  +1 if a > b.
        static int Compare(const Bignum& a, const Bignum& b);
        static bool Equal(const Bignum& a, const Bignum& b) {
            return Compare(a, b) == 0;
        }
        static bool LessEqual(const Bignum& a, const Bignum& b) {
            return Compare(a, b) <= 0;
        }
        static bool Less(const Bignum& a, const Bignum& b) {
            return Compare(a, b) < 0;
        }
        // Returns Compare(a + b, c);
        static int PlusCompare(const Bignum& a, const Bignum& b, const Bignum& c);
        // Returns a + b == c
        static bool PlusEqual(const Bignum& a, const Bignum& b, const Bignum& c) {
            return PlusCompare(a, b, c) == 0;
        }
        // Returns a + b <= c
        static bool PlusLessEqual(const Bignum& a, const Bignum& b, const Bignum& c) {
            return PlusCompare(a, b, c) <= 0;
        }
        // Returns a + b < c
        static bool PlusLess(const Bignum& a, const Bignum& b, const Bignum& c) {
            return PlusCompare(a, b, c) < 0;
        }
    private:
        typedef uint32_t Chunk;
        typedef uint64_t DoubleChunk;

        static const int kChunkSize = sizeof(Chunk) * 8;
        static const int kDoubleChunkSize = sizeof(DoubleChunk) * 8;
        // With bigit size of 28 we loose some bits, but a double still fits easily
        // into two chunks, and more importantly we can use the Comba multiplication.
        static const int kBigitSize = 28;
        static const Chunk kBigitMask = (1 << kBigitSize) - 1;
        // Every instance allocates kBigitLength chunks on the stack. Bignums cannot
        // grow. There are no checks if the stack-allocated space is sufficient.
        static const int kBigitCapacity = kMaxSignificantBits / kBigitSize;

        void EnsureCapacity(int size) {
            if (size > kBigitCapacity) {
                UNREACHABLE();
            }
        }
        void Align(const Bignum& other);
        void Clamp();
        bool IsClamped() const;
        void Zero();
        // Requires this to have enough capacity (no tests done).
        // Updates used_digits_ if necessary.
        // shift_amount must be < kBigitSize.
        void BigitsShiftLeft(int shift_amount);
        // BigitLength includes the "hidden" digits encoded in the exponent.
        int BigitLength() const { return used_digits_ + exponent_; }
        Chunk BigitAt(int index) const;
        void SubtractTimes(const Bignum& other, int factor);

        Chunk bigits_buffer_[kBigitCapacity];
        // A vector backed by bigits_buffer_. This way accesses to the array are
        // checked for out-of-bounds errors.
        Vector<Chunk> bigits_;
        int used_digits_;
        // The Bignum's value equals value(bigits_) * 2^(exponent_ * kBigitSize).
        int exponent_;

        DISALLOW_COPY_AND_ASSIGN(Bignum);
    };

}  // namespace double_conversion

#endif  // DOUBLE_CONVERSION_BIGNUM_H_

#ifndef DOUBLE_CONVERSION_DIY_FP_H_
#define DOUBLE_CONVERSION_DIY_FP_H_

namespace double_conversion {

    // This "Do It Yourself Floating Point" class implements a floating-point number
    // with a uint64 significand and an int exponent. Normalized DiyFp numbers will
    // have the most significant bit of the significand set.
    // Multiplication and Subtraction do not normalize their results.
    // DiyFp are not designed to contain special doubles (NaN and Infinity).
    class DiyFp {
    public:
        static const int kSignificandSize = 64;

        DiyFp() : f_(0), e_(0) {}
        DiyFp(uint64_t f, int e) : f_(f), e_(e) {}

        // this = this - other.
        // The exponents of both numbers must be the same and the significand of this
        // must be bigger than the significand of other.
        // The result will not be normalized.
        void Subtract(const DiyFp& other) {
            ASSERT(e_ == other.e_);
            ASSERT(f_ >= other.f_);
            f_ -= other.f_;
        }

        // Returns a - b.
        // The exponents of both numbers must be the same and this must be bigger
        // than other. The result will not be normalized.
        static DiyFp Minus(const DiyFp& a, const DiyFp& b) {
            DiyFp result = a;
            result.Subtract(b);
            return result;
        }


        // this = this * other.
        void Multiply(const DiyFp& other);

        // returns a * b;
        static DiyFp Times(const DiyFp& a, const DiyFp& b) {
            DiyFp result = a;
            result.Multiply(b);
            return result;
        }

        void Normalize() {
            ASSERT(f_ != 0);
            uint64_t f = f_;
            int e = e_;

            // This method is mainly called for normalizing boundaries. In general
            // boundaries need to be shifted by 10 bits. We thus optimize for this case.
            const uint64_t k10MSBits = UINT64_2PART_C(0xFFC00000, 00000000);
            while ((f & k10MSBits) == 0) {
                f <<= 10;
                e -= 10;
            }
            while ((f & kUint64MSB) == 0) {
                f <<= 1;
                e--;
            }
            f_ = f;
            e_ = e;
        }

        static DiyFp Normalize(const DiyFp& a) {
            DiyFp result = a;
            result.Normalize();
            return result;
        }

        uint64_t f() const { return f_; }
        int e() const { return e_; }

        void set_f(uint64_t new_value) { f_ = new_value; }
        void set_e(int new_value) { e_ = new_value; }

    private:
        static const uint64_t kUint64MSB = UINT64_2PART_C(0x80000000, 00000000);

        uint64_t f_;
        int e_;
    };

}  // namespace double_conversion

#endif  // DOUBLE_CONVERSION_DIY_FP_H_

#ifndef DOUBLE_CONVERSION_CACHED_POWERS_H_
#define DOUBLE_CONVERSION_CACHED_POWERS_H_

namespace double_conversion {

    class PowersOfTenCache {
    public:

        // Not all powers of ten are cached. The decimal exponent of two neighboring
        // cached numbers will differ by kDecimalExponentDistance.
        static const int kDecimalExponentDistance;

        static const int kMinDecimalExponent;
        static const int kMaxDecimalExponent;

        // Returns a cached power-of-ten with a binary exponent in the range
        // [min_exponent; max_exponent] (boundaries included).
        static void GetCachedPowerForBinaryExponentRange(int min_exponent,
            int max_exponent,
            DiyFp* power,
            int* decimal_exponent);

        // Returns a cached power of ten x ~= 10^k such that
        //   k <= decimal_exponent < k + kCachedPowersDecimalDistance.
        // The given decimal_exponent must satisfy
        //   kMinDecimalExponent <= requested_exponent, and
        //   requested_exponent < kMaxDecimalExponent + kDecimalExponentDistance.
        static void GetCachedPowerForDecimalExponent(int requested_exponent,
            DiyFp* power,
            int* found_exponent);
    };

}  // namespace double_conversion

#endif  // DOUBLE_CONVERSION_CACHED_POWERS_H_

#ifndef DOUBLE_CONVERSION_DOUBLE_H_
#define DOUBLE_CONVERSION_DOUBLE_H_

namespace double_conversion {

    // We assume that doubles and uint64_t have the same endianness.
    static uint64_t double_to_uint64(double d) { return BitCast<uint64_t>(d); }
    static double uint64_to_double(uint64_t d64) { return BitCast<double>(d64); }
    static uint32_t float_to_uint32(float f) { return BitCast<uint32_t>(f); }
    static float uint32_to_float(uint32_t d32) { return BitCast<float>(d32); }

    // Helper functions for doubles.
    class Double {
    public:
        static const uint64_t kSignMask = UINT64_2PART_C(0x80000000, 00000000);
        static const uint64_t kExponentMask = UINT64_2PART_C(0x7FF00000, 00000000);
        static const uint64_t kSignificandMask = UINT64_2PART_C(0x000FFFFF, FFFFFFFF);
        static const uint64_t kHiddenBit = UINT64_2PART_C(0x00100000, 00000000);
        static const int kPhysicalSignificandSize = 52;  // Excludes the hidden bit.
        static const int kSignificandSize = 53;

        Double() : d64_(0) {}
        explicit Double(double d) : d64_(double_to_uint64(d)) {}
        explicit Double(uint64_t d64) : d64_(d64) {}
        explicit Double(DiyFp diy_fp)
            : d64_(DiyFpToUint64(diy_fp)) {}

        // The value encoded by this Double must be greater or equal to +0.0.
        // It must not be special (infinity, or NaN).
        DiyFp AsDiyFp() const {
            ASSERT(Sign() > 0);
            ASSERT(!IsSpecial());
            return DiyFp(Significand(), Exponent());
        }

        // The value encoded by this Double must be strictly greater than 0.
        DiyFp AsNormalizedDiyFp() const {
            ASSERT(value() > 0.0);
            uint64_t f = Significand();
            int e = Exponent();

            // The current double could be a denormal.
            while ((f & kHiddenBit) == 0) {
                f <<= 1;
                e--;
            }
            // Do the final shifts in one go.
            f <<= DiyFp::kSignificandSize - kSignificandSize;
            e -= DiyFp::kSignificandSize - kSignificandSize;
            return DiyFp(f, e);
        }

        // Returns the double's bit as uint64.
        uint64_t AsUint64() const {
            return d64_;
        }

        // Returns the next greater double. Returns +infinity on input +infinity.
        double NextDouble() const {
            if (d64_ == kInfinity) return Double(kInfinity).value();
            if (Sign() < 0 && Significand() == 0) {
                // -0.0
                return 0.0;
            }
            if (Sign() < 0) {
                return Double(d64_ - 1).value();
            }
            else {
                return Double(d64_ + 1).value();
            }
        }

        double PreviousDouble() const {
            if (d64_ == (kInfinity | kSignMask)) return -Double::Infinity();
            if (Sign() < 0) {
                return Double(d64_ + 1).value();
            }
            else {
                if (Significand() == 0) return -0.0;
                return Double(d64_ - 1).value();
            }
        }

        int Exponent() const {
            if (IsDenormal()) return kDenormalExponent;

            uint64_t d64 = AsUint64();
            int biased_e =
                static_cast<int>((d64 & kExponentMask) >> kPhysicalSignificandSize);
            return biased_e - kExponentBias;
        }

        uint64_t Significand() const {
            uint64_t d64 = AsUint64();
            uint64_t significand = d64 & kSignificandMask;
            if (!IsDenormal()) {
                return significand + kHiddenBit;
            }
            else {
                return significand;
            }
        }

        // Returns true if the double is a denormal.
        bool IsDenormal() const {
            uint64_t d64 = AsUint64();
            return (d64 & kExponentMask) == 0;
        }

        // We consider denormals not to be special.
        // Hence only Infinity and NaN are special.
        bool IsSpecial() const {
            uint64_t d64 = AsUint64();
            return (d64 & kExponentMask) == kExponentMask;
        }

        bool IsNan() const {
            uint64_t d64 = AsUint64();
            return ((d64 & kExponentMask) == kExponentMask) &&
                ((d64 & kSignificandMask) != 0);
        }

        bool IsInfinite() const {
            uint64_t d64 = AsUint64();
            return ((d64 & kExponentMask) == kExponentMask) &&
                ((d64 & kSignificandMask) == 0);
        }

        int Sign() const {
            uint64_t d64 = AsUint64();
            return (d64 & kSignMask) == 0 ? 1 : -1;
        }

        // Precondition: the value encoded by this Double must be greater or equal
        // than +0.0.
        DiyFp UpperBoundary() const {
            ASSERT(Sign() > 0);
            return DiyFp(Significand() * 2 + 1, Exponent() - 1);
        }

        // Computes the two boundaries of this.
        // The bigger boundary (m_plus) is normalized. The lower boundary has the same
        // exponent as m_plus.
        // Precondition: the value encoded by this Double must be greater than 0.
        void NormalizedBoundaries(DiyFp* out_m_minus, DiyFp* out_m_plus) const {
            ASSERT(value() > 0.0);
            DiyFp v = this->AsDiyFp();
            DiyFp m_plus = DiyFp::Normalize(DiyFp((v.f() << 1) + 1, v.e() - 1));
            DiyFp m_minus;
            if (LowerBoundaryIsCloser()) {
                m_minus = DiyFp((v.f() << 2) - 1, v.e() - 2);
            }
            else {
                m_minus = DiyFp((v.f() << 1) - 1, v.e() - 1);
            }
            m_minus.set_f(m_minus.f() << (m_minus.e() - m_plus.e()));
            m_minus.set_e(m_plus.e());
            *out_m_plus = m_plus;
            *out_m_minus = m_minus;
        }

        bool LowerBoundaryIsCloser() const {
            // The boundary is closer if the significand is of the form f == 2^p-1 then
            // the lower boundary is closer.
            // Think of v = 1000e10 and v- = 9999e9.
            // Then the boundary (== (v - v-)/2) is not just at a distance of 1e9 but
            // at a distance of 1e8.
            // The only exception is for the smallest normal: the largest denormal is
            // at the same distance as its successor.
            // Note: denormals have the same exponent as the smallest normals.
            bool physical_significand_is_zero = ((AsUint64() & kSignificandMask) == 0);
            return physical_significand_is_zero && (Exponent() != kDenormalExponent);
        }

        double value() const { return uint64_to_double(d64_); }

        // Returns the significand size for a given order of magnitude.
        // If v = f*2^e with 2^p-1 <= f <= 2^p then p+e is v's order of magnitude.
        // This function returns the number of significant binary digits v will have
        // once it's encoded into a double. In almost all cases this is equal to
        // kSignificandSize. The only exceptions are denormals. They start with
        // leading zeroes and their effective significand-size is hence smaller.
        static int SignificandSizeForOrderOfMagnitude(int order) {
            if (order >= (kDenormalExponent + kSignificandSize)) {
                return kSignificandSize;
            }
            if (order <= kDenormalExponent) return 0;
            return order - kDenormalExponent;
        }

        static double Infinity() {
            return Double(kInfinity).value();
        }

        static double NaN() {
            return Double(kNaN).value();
        }

    private:
        static const int kExponentBias = 0x3FF + kPhysicalSignificandSize;
        static const int kDenormalExponent = -kExponentBias + 1;
        static const int kMaxExponent = 0x7FF - kExponentBias;
        static const uint64_t kInfinity = UINT64_2PART_C(0x7FF00000, 00000000);
        static const uint64_t kNaN = UINT64_2PART_C(0x7FF80000, 00000000);

        const uint64_t d64_;

        static uint64_t DiyFpToUint64(DiyFp diy_fp) {
            uint64_t significand = diy_fp.f();
            int exponent = diy_fp.e();
            while (significand > kHiddenBit + kSignificandMask) {
                significand >>= 1;
                exponent++;
            }
            if (exponent >= kMaxExponent) {
                return kInfinity;
            }
            if (exponent < kDenormalExponent) {
                return 0;
            }
            while (exponent > kDenormalExponent && (significand & kHiddenBit) == 0) {
                significand <<= 1;
                exponent--;
            }
            uint64_t biased_exponent;
            if (exponent == kDenormalExponent && (significand & kHiddenBit) == 0) {
                biased_exponent = 0;
            }
            else {
                biased_exponent = static_cast<uint64_t>(exponent + kExponentBias);
            }
            return (significand & kSignificandMask) |
                (biased_exponent << kPhysicalSignificandSize);
        }

        DISALLOW_COPY_AND_ASSIGN(Double);
    };

    class Single {
    public:
        static const uint32_t kSignMask = 0x80000000;
        static const uint32_t kExponentMask = 0x7F800000;
        static const uint32_t kSignificandMask = 0x007FFFFF;
        static const uint32_t kHiddenBit = 0x00800000;
        static const int kPhysicalSignificandSize = 23;  // Excludes the hidden bit.
        static const int kSignificandSize = 24;

        Single() : d32_(0) {}
        explicit Single(float f) : d32_(float_to_uint32(f)) {}
        explicit Single(uint32_t d32) : d32_(d32) {}

        // The value encoded by this Single must be greater or equal to +0.0.
        // It must not be special (infinity, or NaN).
        DiyFp AsDiyFp() const {
            ASSERT(Sign() > 0);
            ASSERT(!IsSpecial());
            return DiyFp(Significand(), Exponent());
        }

        // Returns the single's bit as uint64.
        uint32_t AsUint32() const {
            return d32_;
        }

        int Exponent() const {
            if (IsDenormal()) return kDenormalExponent;

            uint32_t d32 = AsUint32();
            int biased_e =
                static_cast<int>((d32 & kExponentMask) >> kPhysicalSignificandSize);
            return biased_e - kExponentBias;
        }

        uint32_t Significand() const {
            uint32_t d32 = AsUint32();
            uint32_t significand = d32 & kSignificandMask;
            if (!IsDenormal()) {
                return significand + kHiddenBit;
            }
            else {
                return significand;
            }
        }

        // Returns true if the single is a denormal.
        bool IsDenormal() const {
            uint32_t d32 = AsUint32();
            return (d32 & kExponentMask) == 0;
        }

        // We consider denormals not to be special.
        // Hence only Infinity and NaN are special.
        bool IsSpecial() const {
            uint32_t d32 = AsUint32();
            return (d32 & kExponentMask) == kExponentMask;
        }

        bool IsNan() const {
            uint32_t d32 = AsUint32();
            return ((d32 & kExponentMask) == kExponentMask) &&
                ((d32 & kSignificandMask) != 0);
        }

        bool IsInfinite() const {
            uint32_t d32 = AsUint32();
            return ((d32 & kExponentMask) == kExponentMask) &&
                ((d32 & kSignificandMask) == 0);
        }

        int Sign() const {
            uint32_t d32 = AsUint32();
            return (d32 & kSignMask) == 0 ? 1 : -1;
        }

        // Computes the two boundaries of this.
        // The bigger boundary (m_plus) is normalized. The lower boundary has the same
        // exponent as m_plus.
        // Precondition: the value encoded by this Single must be greater than 0.
        void NormalizedBoundaries(DiyFp* out_m_minus, DiyFp* out_m_plus) const {
            ASSERT(value() > 0.0);
            DiyFp v = this->AsDiyFp();
            DiyFp m_plus = DiyFp::Normalize(DiyFp((v.f() << 1) + 1, v.e() - 1));
            DiyFp m_minus;
            if (LowerBoundaryIsCloser()) {
                m_minus = DiyFp((v.f() << 2) - 1, v.e() - 2);
            }
            else {
                m_minus = DiyFp((v.f() << 1) - 1, v.e() - 1);
            }
            m_minus.set_f(m_minus.f() << (m_minus.e() - m_plus.e()));
            m_minus.set_e(m_plus.e());
            *out_m_plus = m_plus;
            *out_m_minus = m_minus;
        }

        // Precondition: the value encoded by this Single must be greater or equal
        // than +0.0.
        DiyFp UpperBoundary() const {
            ASSERT(Sign() > 0);
            return DiyFp(Significand() * 2 + 1, Exponent() - 1);
        }

        bool LowerBoundaryIsCloser() const {
            // The boundary is closer if the significand is of the form f == 2^p-1 then
            // the lower boundary is closer.
            // Think of v = 1000e10 and v- = 9999e9.
            // Then the boundary (== (v - v-)/2) is not just at a distance of 1e9 but
            // at a distance of 1e8.
            // The only exception is for the smallest normal: the largest denormal is
            // at the same distance as its successor.
            // Note: denormals have the same exponent as the smallest normals.
            bool physical_significand_is_zero = ((AsUint32() & kSignificandMask) == 0);
            return physical_significand_is_zero && (Exponent() != kDenormalExponent);
        }

        float value() const { return uint32_to_float(d32_); }

        static float Infinity() {
            return Single(kInfinity).value();
        }

        static float NaN() {
            return Single(kNaN).value();
        }

    private:
        static const int kExponentBias = 0x7F + kPhysicalSignificandSize;
        static const int kDenormalExponent = -kExponentBias + 1;
        static const int kMaxExponent = 0xFF - kExponentBias;
        static const uint32_t kInfinity = 0x7F800000;
        static const uint32_t kNaN = 0x7FC00000;

        const uint32_t d32_;

        DISALLOW_COPY_AND_ASSIGN(Single);
    };

}  // namespace double_conversion

#endif  // DOUBLE_CONVERSION_DOUBLE_H_

#ifndef DOUBLE_CONVERSION_BIGNUM_DTOA_H_
#define DOUBLE_CONVERSION_BIGNUM_DTOA_H_

namespace double_conversion {

    enum BignumDtoaMode {
        // Return the shortest correct representation.
        // For example the output of 0.299999999999999988897 is (the less accurate but
        // correct) 0.3.
        BIGNUM_DTOA_SHORTEST,
        // Same as BIGNUM_DTOA_SHORTEST but for single-precision floats.
        BIGNUM_DTOA_SHORTEST_SINGLE,
        // Return a fixed number of digits after the decimal point.
        // For instance fixed(0.1, 4) becomes 0.1000
        // If the input number is big, the output will be big.
        BIGNUM_DTOA_FIXED,
        // Return a fixed number of digits, no matter what the exponent is.
        BIGNUM_DTOA_PRECISION
    };

    // Converts the given double 'v' to ascii.
    // The result should be interpreted as buffer * 10^(point-length).
    // The buffer will be null-terminated.
    //
    // The input v must be > 0 and different from NaN, and Infinity.
    //
    // The output depends on the given mode:
    //  - SHORTEST: produce the least amount of digits for which the internal
    //   identity requirement is still satisfied. If the digits are printed
    //   (together with the correct exponent) then reading this number will give
    //   'v' again. The buffer will choose the representation that is closest to
    //   'v'. If there are two at the same distance, than the number is round up.
    //   In this mode the 'requested_digits' parameter is ignored.
    //  - FIXED: produces digits necessary to print a given number with
    //   'requested_digits' digits after the decimal point. The produced digits
    //   might be too short in which case the caller has to fill the gaps with '0's.
    //   Example: toFixed(0.001, 5) is allowed to return buffer="1", point=-2.
    //   Halfway cases are rounded up. The call toFixed(0.15, 2) thus returns
    //     buffer="2", point=0.
    //   Note: the length of the returned buffer has no meaning wrt the significance
    //   of its digits. That is, just because it contains '0's does not mean that
    //   any other digit would not satisfy the internal identity requirement.
    //  - PRECISION: produces 'requested_digits' where the first digit is not '0'.
    //   Even though the length of produced digits usually equals
    //   'requested_digits', the function is allowed to return fewer digits, in
    //   which case the caller has to fill the missing digits with '0's.
    //   Halfway cases are again rounded up.
    // 'BignumDtoa' expects the given buffer to be big enough to hold all digits
    // and a terminating null-character.
    void BignumDtoa(double v, BignumDtoaMode mode, int requested_digits,
        Vector<char> buffer, int* length, int* point);

}  // namespace double_conversion

#endif  // DOUBLE_CONVERSION_BIGNUM_DTOA_H_

#ifndef DOUBLE_CONVERSION_FAST_DTOA_H_
#define DOUBLE_CONVERSION_FAST_DTOA_H_

namespace double_conversion {

    enum FastDtoaMode {
        // Computes the shortest representation of the given input. The returned
        // result will be the most accurate number of this length. Longer
        // representations might be more accurate.
        FAST_DTOA_SHORTEST,
        // Same as FAST_DTOA_SHORTEST but for single-precision floats.
        FAST_DTOA_SHORTEST_SINGLE,
        // Computes a representation where the precision (number of digits) is
        // given as input. The precision is independent of the decimal point.
        FAST_DTOA_PRECISION
    };

    // FastDtoa will produce at most kFastDtoaMaximalLength digits. This does not
    // include the terminating '\0' character.

    //static const int kFastDtoaMaximalLength = 17;
    // Same for single-precision numbers.
    //static const int kFastDtoaMaximalSingleLength = 9;

    // Provides a decimal representation of v.
    // The result should be interpreted as buffer * 10^(point - length).
    //
    // Precondition:
    //   * v must be a strictly positive finite double.
    //
    // Returns true if it succeeds, otherwise the result can not be trusted.
    // There will be *length digits inside the buffer followed by a null terminator.
    // If the function returns true and mode equals
    //   - FAST_DTOA_SHORTEST, then
    //     the parameter requested_digits is ignored.
    //     The result satisfies
    //         v == (double) (buffer * 10^(point - length)).
    //     The digits in the buffer are the shortest representation possible. E.g.
    //     if 0.099999999999 and 0.1 represent the same double then "1" is returned
    //     with point = 0.
    //     The last digit will be closest to the actual v. That is, even if several
    //     digits might correctly yield 'v' when read again, the buffer will contain
    //     the one closest to v.
    //   - FAST_DTOA_PRECISION, then
    //     the buffer contains requested_digits digits.
    //     the difference v - (buffer * 10^(point-length)) is closest to zero for
    //     all possible representations of requested_digits digits.
    //     If there are two values that are equally close, then FastDtoa returns
    //     false.
    // For both modes the buffer must be large enough to hold the result.
    bool FastDtoa(double d,
        FastDtoaMode mode,
        int requested_digits,
        Vector<char> buffer,
        int* length,
        int* decimal_point);

}  // namespace double_conversion

#endif  // DOUBLE_CONVERSION_FAST_DTOA_H_

#ifndef DOUBLE_CONVERSION_FIXED_DTOA_H_
#define DOUBLE_CONVERSION_FIXED_DTOA_H_

namespace double_conversion {

    // Produces digits necessary to print a given number with
    // 'fractional_count' digits after the decimal point.
    // The buffer must be big enough to hold the result plus one terminating null
    // character.
    //
    // The produced digits might be too short in which case the caller has to fill
    // the gaps with '0's.
    // Example: FastFixedDtoa(0.001, 5, ...) is allowed to return buffer = "1", and
    // decimal_point = -2.
    // Halfway cases are rounded towards +/-Infinity (away from 0). The call
    // FastFixedDtoa(0.15, 2, ...) thus returns buffer = "2", decimal_point = 0.
    // The returned buffer may contain digits that would be truncated from the
    // shortest representation of the input.
    //
    // This method only works for some parameters. If it can't handle the input it
    // returns false. The output is null-terminated when the function succeeds.
    bool FastFixedDtoa(double v, int fractional_count,
        Vector<char> buffer, int* length, int* decimal_point);

}  // namespace double_conversion

#endif  // DOUBLE_CONVERSION_FIXED_DTOA_H_

#ifndef DOUBLE_CONVERSION_STRTOD_H_
#define DOUBLE_CONVERSION_STRTOD_H_

namespace double_conversion {

    // The buffer must only contain digits in the range [0-9]. It must not
    // contain a dot or a sign. It must not start with '0', and must not be empty.
    double Strtod(Vector<const char> buffer, int exponent);

    // The buffer must only contain digits in the range [0-9]. It must not
    // contain a dot or a sign. It must not start with '0', and must not be empty.
    float Strtof(Vector<const char> buffer, int exponent);

}  // namespace double_conversion

#endif  // DOUBLE_CONVERSION_STRTOD_H_

namespace double_conversion {

const DoubleToStringConverter& DoubleToStringConverter::EcmaScriptConverter() {
  int flags = UNIQUE_ZERO | EMIT_POSITIVE_EXPONENT_SIGN;
  static DoubleToStringConverter converter(flags,
                                           "Infinity",
                                           "NaN",
                                           'e',
                                           -6, 21,
                                           6, 0);
  return converter;
}


bool DoubleToStringConverter::HandleSpecialValues(
    double value,
    StringBuilder* result_builder) const {
  Double double_inspect(value);
  if (double_inspect.IsInfinite()) {
    if (infinity_symbol_ == NULL) return false;
    if (value < 0) {
      result_builder->AddCharacter('-');
    }
    result_builder->AddString(infinity_symbol_);
    return true;
  }
  if (double_inspect.IsNan()) {
    if (nan_symbol_ == NULL) return false;
    result_builder->AddString(nan_symbol_);
    return true;
  }
  return false;
}


void DoubleToStringConverter::CreateExponentialRepresentation(
    const char* decimal_digits,
    int length,
    int exponent,
    StringBuilder* result_builder) const {
  ASSERT(length != 0);
  result_builder->AddCharacter(decimal_digits[0]);
  if (length != 1) {
    result_builder->AddCharacter('.');
    result_builder->AddSubstring(&decimal_digits[1], length-1);
  }
  result_builder->AddCharacter(exponent_character_);
  if (exponent < 0) {
    result_builder->AddCharacter('-');
    exponent = -exponent;
  } else {
    if ((flags_ & EMIT_POSITIVE_EXPONENT_SIGN) != 0) {
      result_builder->AddCharacter('+');
    }
  }
  if (exponent == 0) {
    result_builder->AddCharacter('0');
    return;
  }
  ASSERT(exponent < 1e4);
  const int kMaxExponentLength = 5;
  char buffer[kMaxExponentLength + 1];
  buffer[kMaxExponentLength] = '\0';
  int first_char_pos = kMaxExponentLength;
  while (exponent > 0) {
    buffer[--first_char_pos] = '0' + (exponent % 10);
    exponent /= 10;
  }
  result_builder->AddSubstring(&buffer[first_char_pos],
                               kMaxExponentLength - first_char_pos);
}


void DoubleToStringConverter::CreateDecimalRepresentation(
    const char* decimal_digits,
    int length,
    int decimal_point,
    int digits_after_point,
    StringBuilder* result_builder) const {
  // Create a representation that is padded with zeros if needed.
  if (decimal_point <= 0) {
      // "0.00000decimal_rep".
    result_builder->AddCharacter('0');
    if (digits_after_point > 0) {
      result_builder->AddCharacter('.');
      result_builder->AddPadding('0', -decimal_point);
      ASSERT(length <= digits_after_point - (-decimal_point));
      result_builder->AddSubstring(decimal_digits, length);
      int remaining_digits = digits_after_point - (-decimal_point) - length;
      result_builder->AddPadding('0', remaining_digits);
    }
  } else if (decimal_point >= length) {
    // "decimal_rep0000.00000" or "decimal_rep.0000"
    result_builder->AddSubstring(decimal_digits, length);
    result_builder->AddPadding('0', decimal_point - length);
    if (digits_after_point > 0) {
      result_builder->AddCharacter('.');
      result_builder->AddPadding('0', digits_after_point);
    }
  } else {
    // "decima.l_rep000"
    ASSERT(digits_after_point > 0);
    result_builder->AddSubstring(decimal_digits, decimal_point);
    result_builder->AddCharacter('.');
    ASSERT(length - decimal_point <= digits_after_point);
    result_builder->AddSubstring(&decimal_digits[decimal_point],
                                 length - decimal_point);
    int remaining_digits = digits_after_point - (length - decimal_point);
    result_builder->AddPadding('0', remaining_digits);
  }
  if (digits_after_point == 0) {
    if ((flags_ & EMIT_TRAILING_DECIMAL_POINT) != 0) {
      result_builder->AddCharacter('.');
    }
    if ((flags_ & EMIT_TRAILING_ZERO_AFTER_POINT) != 0) {
      result_builder->AddCharacter('0');
    }
  }
}


bool DoubleToStringConverter::ToShortestIeeeNumber(
    double value,
    StringBuilder* result_builder,
    DoubleToStringConverter::DtoaMode mode) const {
  ASSERT(mode == SHORTEST || mode == SHORTEST_SINGLE);
  if (Double(value).IsSpecial()) {
    return HandleSpecialValues(value, result_builder);
  }

  int decimal_point;
  bool sign;
  const int kDecimalRepCapacity = kBase10MaximalLength + 1;
  char decimal_rep[kDecimalRepCapacity];
  int decimal_rep_length;

  DoubleToAscii(value, mode, 0, decimal_rep, kDecimalRepCapacity,
                &sign, &decimal_rep_length, &decimal_point);

  bool unique_zero = (flags_ & UNIQUE_ZERO) != 0;
  if (sign && (value != 0.0 || !unique_zero)) {
    result_builder->AddCharacter('-');
  }

  int exponent = decimal_point - 1;
  if ((decimal_in_shortest_low_ <= exponent) &&
      (exponent < decimal_in_shortest_high_)) {
    CreateDecimalRepresentation(decimal_rep, decimal_rep_length,
                                decimal_point,
                                Max(0, decimal_rep_length - decimal_point),
                                result_builder);
  } else {
    CreateExponentialRepresentation(decimal_rep, decimal_rep_length, exponent,
                                    result_builder);
  }
  return true;
}


bool DoubleToStringConverter::ToFixed(double value,
                                      int requested_digits,
                                      StringBuilder* result_builder) const {
  ASSERT(kMaxFixedDigitsBeforePoint == 60);
  const double kFirstNonFixed = 1e60;

  if (Double(value).IsSpecial()) {
    return HandleSpecialValues(value, result_builder);
  }

  if (requested_digits > kMaxFixedDigitsAfterPoint) return false;
  if (value >= kFirstNonFixed || value <= -kFirstNonFixed) return false;

  // Find a sufficiently precise decimal representation of n.
  int decimal_point;
  bool sign;
  // Add space for the '\0' byte.
  const int kDecimalRepCapacity =
      kMaxFixedDigitsBeforePoint + kMaxFixedDigitsAfterPoint + 1;
  char decimal_rep[kDecimalRepCapacity];
  int decimal_rep_length;
  DoubleToAscii(value, FIXED, requested_digits,
                decimal_rep, kDecimalRepCapacity,
                &sign, &decimal_rep_length, &decimal_point);

  bool unique_zero = ((flags_ & UNIQUE_ZERO) != 0);
  if (sign && (value != 0.0 || !unique_zero)) {
    result_builder->AddCharacter('-');
  }

  CreateDecimalRepresentation(decimal_rep, decimal_rep_length, decimal_point,
                              requested_digits, result_builder);
  return true;
}


bool DoubleToStringConverter::ToExponential(
    double value,
    int requested_digits,
    StringBuilder* result_builder) const {
  if (Double(value).IsSpecial()) {
    return HandleSpecialValues(value, result_builder);
  }

  if (requested_digits < -1) return false;
  if (requested_digits > kMaxExponentialDigits) return false;

  int decimal_point;
  bool sign;
  // Add space for digit before the decimal point and the '\0' character.
  const int kDecimalRepCapacity = kMaxExponentialDigits + 2;
  ASSERT(kDecimalRepCapacity > kBase10MaximalLength);
  char decimal_rep[kDecimalRepCapacity];
  int decimal_rep_length;

  if (requested_digits == -1) {
    DoubleToAscii(value, SHORTEST, 0,
                  decimal_rep, kDecimalRepCapacity,
                  &sign, &decimal_rep_length, &decimal_point);
  } else {
    DoubleToAscii(value, PRECISION, requested_digits + 1,
                  decimal_rep, kDecimalRepCapacity,
                  &sign, &decimal_rep_length, &decimal_point);
    ASSERT(decimal_rep_length <= requested_digits + 1);

    for (int i = decimal_rep_length; i < requested_digits + 1; ++i) {
      decimal_rep[i] = '0';
    }
    decimal_rep_length = requested_digits + 1;
  }

  bool unique_zero = ((flags_ & UNIQUE_ZERO) != 0);
  if (sign && (value != 0.0 || !unique_zero)) {
    result_builder->AddCharacter('-');
  }

  int exponent = decimal_point - 1;
  CreateExponentialRepresentation(decimal_rep,
                                  decimal_rep_length,
                                  exponent,
                                  result_builder);
  return true;
}


bool DoubleToStringConverter::ToPrecision(double value,
                                          int precision,
                                          StringBuilder* result_builder) const {
  if (Double(value).IsSpecial()) {
    return HandleSpecialValues(value, result_builder);
  }

  if (precision < kMinPrecisionDigits || precision > kMaxPrecisionDigits) {
    return false;
  }

  // Find a sufficiently precise decimal representation of n.
  int decimal_point;
  bool sign;
  // Add one for the terminating null character.
  const int kDecimalRepCapacity = kMaxPrecisionDigits + 1;
  char decimal_rep[kDecimalRepCapacity];
  int decimal_rep_length;

  DoubleToAscii(value, PRECISION, precision,
                decimal_rep, kDecimalRepCapacity,
                &sign, &decimal_rep_length, &decimal_point);
  ASSERT(decimal_rep_length <= precision);

  bool unique_zero = ((flags_ & UNIQUE_ZERO) != 0);
  if (sign && (value != 0.0 || !unique_zero)) {
    result_builder->AddCharacter('-');
  }

  // The exponent if we print the number as x.xxeyyy. That is with the
  // decimal point after the first digit.
  int exponent = decimal_point - 1;

  int extra_zero = ((flags_ & EMIT_TRAILING_ZERO_AFTER_POINT) != 0) ? 1 : 0;
  if ((-decimal_point + 1 > max_leading_padding_zeroes_in_precision_mode_) ||
      (decimal_point - precision + extra_zero >
       max_trailing_padding_zeroes_in_precision_mode_)) {
    // Fill buffer to contain 'precision' digits.
    // Usually the buffer is already at the correct length, but 'DoubleToAscii'
    // is allowed to return less characters.
    for (int i = decimal_rep_length; i < precision; ++i) {
      decimal_rep[i] = '0';
    }

    CreateExponentialRepresentation(decimal_rep,
                                    precision,
                                    exponent,
                                    result_builder);
  } else {
    CreateDecimalRepresentation(decimal_rep, decimal_rep_length, decimal_point,
                                Max(0, precision - decimal_point),
                                result_builder);
  }
  return true;
}


static BignumDtoaMode DtoaToBignumDtoaMode(
    DoubleToStringConverter::DtoaMode dtoa_mode) {
  switch (dtoa_mode) {
    case DoubleToStringConverter::SHORTEST:  return BIGNUM_DTOA_SHORTEST;
    case DoubleToStringConverter::SHORTEST_SINGLE:
        return BIGNUM_DTOA_SHORTEST_SINGLE;
    case DoubleToStringConverter::FIXED:     return BIGNUM_DTOA_FIXED;
    case DoubleToStringConverter::PRECISION: return BIGNUM_DTOA_PRECISION;
    default:
      UNREACHABLE();
  }
}


void DoubleToStringConverter::DoubleToAscii(double v,
                                            DtoaMode mode,
                                            int requested_digits,
                                            char* buffer,
                                            int buffer_length,
                                            bool* sign,
                                            int* length,
                                            int* point) {
  Vector<char> vector(buffer, buffer_length);
  ASSERT(!Double(v).IsSpecial());
  ASSERT(mode == SHORTEST || mode == SHORTEST_SINGLE || requested_digits >= 0);

  if (Double(v).Sign() < 0) {
    *sign = true;
    v = -v;
  } else {
    *sign = false;
  }

  if (mode == PRECISION && requested_digits == 0) {
    vector[0] = '\0';
    *length = 0;
    return;
  }

  if (v == 0) {
    vector[0] = '0';
    vector[1] = '\0';
    *length = 1;
    *point = 1;
    return;
  }

  bool fast_worked;
  switch (mode) {
    case SHORTEST:
      fast_worked = FastDtoa(v, FAST_DTOA_SHORTEST, 0, vector, length, point);
      break;
    case SHORTEST_SINGLE:
      fast_worked = FastDtoa(v, FAST_DTOA_SHORTEST_SINGLE, 0,
                             vector, length, point);
      break;
    case FIXED:
      fast_worked = FastFixedDtoa(v, requested_digits, vector, length, point);
      break;
    case PRECISION:
      fast_worked = FastDtoa(v, FAST_DTOA_PRECISION, requested_digits,
                             vector, length, point);
      break;
    default:
      fast_worked = false;
      UNREACHABLE();
  }
  if (fast_worked) return;

  // If the fast dtoa didn't succeed use the slower bignum version.
  BignumDtoaMode bignum_mode = DtoaToBignumDtoaMode(mode);
  BignumDtoa(v, bignum_mode, requested_digits, vector, length, point);
  vector[*length] = '\0';
}


// Consumes the given substring from the iterator.
// Returns false, if the substring does not match.
static bool ConsumeSubString(const char** current,
                             const char* end,
                             const char* substring) {
  ASSERT(**current == *substring);
  for (substring++; *substring != '\0'; substring++) {
    ++*current;
    if (*current == end || **current != *substring) return false;
  }
  ++*current;
  return true;
}


// Maximum number of significant digits in decimal representation.
// The longest possible double in decimal representation is
// (2^53 - 1) * 2 ^ -1074 that is (2 ^ 53 - 1) * 5 ^ 1074 / 10 ^ 1074
// (768 digits). If we parse a number whose first digits are equal to a
// mean of 2 adjacent doubles (that could have up to 769 digits) the result
// must be rounded to the bigger one unless the tail consists of zeros, so
// we don't need to preserve all the digits.
const int kMaxSignificantDigits = 772;


// Returns true if a nonspace found and false if the end has reached.
static inline bool AdvanceToNonspace(const char** current, const char* end) {
  while (*current != end) {
    if (**current != ' ') return true;
    ++*current;
  }
  return false;
}


static bool isDigit(int x, int radix) {
  return (x >= '0' && x <= '9' && x < '0' + radix)
      || (radix > 10 && x >= 'a' && x < 'a' + radix - 10)
      || (radix > 10 && x >= 'A' && x < 'A' + radix - 10);
}


static double SignedZero(bool sign) {
  return sign ? -0.0 : 0.0;
}


// Returns true if 'c' is a decimal digit that is valid for the given radix.
//
// The function is small and could be inlined, but VS2012 emitted a warning
// because it constant-propagated the radix and concluded that the last
// condition was always true. By moving it into a separate function the
// compiler wouldn't warn anymore.
static bool IsDecimalDigitForRadix(int c, int radix) {
  return '0' <= c && c <= '9' && (c - '0') < radix;
}

// Returns true if 'c' is a character digit that is valid for the given radix.
// The 'a_character' should be 'a' or 'A'.
//
// The function is small and could be inlined, but VS2012 emitted a warning
// because it constant-propagated the radix and concluded that the first
// condition was always false. By moving it into a separate function the
// compiler wouldn't warn anymore.
static bool IsCharacterDigitForRadix(int c, int radix, char a_character) {
  return radix > 10 && c >= a_character && c < a_character + radix - 10;
}


// Parsing integers with radix 2, 4, 8, 16, 32. Assumes current != end.
template <int radix_log_2>
static double RadixStringToIeee(const char* current,
                                const char* end,
                                bool sign,
                                bool allow_trailing_junk,
                                double junk_string_value,
                                bool read_as_double,
                                const char** trailing_pointer) {
  ASSERT(current != end);

  const int kDoubleSize = Double::kSignificandSize;
  const int kSingleSize = Single::kSignificandSize;
  const int kSignificandSize = read_as_double? kDoubleSize: kSingleSize;

  // Skip leading 0s.
  while (*current == '0') {
    ++current;
    if (current == end) {
      *trailing_pointer = end;
      return SignedZero(sign);
    }
  }

  int64_t number = 0;
  int exponent = 0;
  const int radix = (1 << radix_log_2);

  do {
    int digit;
    if (IsDecimalDigitForRadix(*current, radix)) {
      digit = static_cast<char>(*current) - '0';
    } else if (IsCharacterDigitForRadix(*current, radix, 'a')) {
      digit = static_cast<char>(*current) - 'a' + 10;
    } else if (IsCharacterDigitForRadix(*current, radix, 'A')) {
      digit = static_cast<char>(*current) - 'A' + 10;
    } else {
      if (allow_trailing_junk || !AdvanceToNonspace(&current, end)) {
        break;
      } else {
        return junk_string_value;
      }
    }

    number = number * radix + digit;
    int overflow = static_cast<int>(number >> kSignificandSize);
    if (overflow != 0) {
      // Overflow occurred. Need to determine which direction to round the
      // result.
      int overflow_bits_count = 1;
      while (overflow > 1) {
        overflow_bits_count++;
        overflow >>= 1;
      }

      int dropped_bits_mask = ((1 << overflow_bits_count) - 1);
      int dropped_bits = static_cast<int>(number) & dropped_bits_mask;
      number >>= overflow_bits_count;
      exponent = overflow_bits_count;

      bool zero_tail = true;
      for (;;) {
        ++current;
        if (current == end || !isDigit(*current, radix)) break;
        zero_tail = zero_tail && *current == '0';
        exponent += radix_log_2;
      }

      if (!allow_trailing_junk && AdvanceToNonspace(&current, end)) {
        return junk_string_value;
      }

      int middle_value = (1 << (overflow_bits_count - 1));
      if (dropped_bits > middle_value) {
        number++;  // Rounding up.
      } else if (dropped_bits == middle_value) {
        // Rounding to even to consistency with decimals: half-way case rounds
        // up if significant part is odd and down otherwise.
        if ((number & 1) != 0 || !zero_tail) {
          number++;  // Rounding up.
        }
      }

      // Rounding up may cause overflow.
      if ((number & ((int64_t)1 << kSignificandSize)) != 0) {
        exponent++;
        number >>= 1;
      }
      break;
    }
    ++current;
  } while (current != end);

  ASSERT(number < ((int64_t)1 << kSignificandSize));
  ASSERT(static_cast<int64_t>(static_cast<double>(number)) == number);

  *trailing_pointer = current;

  if (exponent == 0) {
    if (sign) {
      if (number == 0) return -0.0;
      number = -number;
    }
    return static_cast<double>(number);
  }

  ASSERT(number != 0);
  return Double(DiyFp(number, exponent)).value();
}


double StringToDoubleConverter::StringToIeee(
    const char* input,
    int length,
    int* processed_characters_count,
    bool read_as_double) const {
  const char* current = input;
  const char* end = input + length;

  *processed_characters_count = 0;

  const bool allow_trailing_junk = (flags_ & ALLOW_TRAILING_JUNK) != 0;
  const bool allow_leading_spaces = (flags_ & ALLOW_LEADING_SPACES) != 0;
  const bool allow_trailing_spaces = (flags_ & ALLOW_TRAILING_SPACES) != 0;
  const bool allow_spaces_after_sign = (flags_ & ALLOW_SPACES_AFTER_SIGN) != 0;

  // To make sure that iterator dereferencing is valid the following
  // convention is used:
  // 1. Each '++current' statement is followed by check for equality to 'end'.
  // 2. If AdvanceToNonspace returned false then current == end.
  // 3. If 'current' becomes equal to 'end' the function returns or goes to
  // 'parsing_done'.
  // 4. 'current' is not dereferenced after the 'parsing_done' label.
  // 5. Code before 'parsing_done' may rely on 'current != end'.
  if (current == end) return empty_string_value_;

  if (allow_leading_spaces || allow_trailing_spaces) {
    if (!AdvanceToNonspace(&current, end)) {
      *processed_characters_count = static_cast<int>(current - input);
      return empty_string_value_;
    }
    if (!allow_leading_spaces && (input != current)) {
      // No leading spaces allowed, but AdvanceToNonspace moved forward.
      return junk_string_value_;
    }
  }

  // The longest form of simplified number is: "-<significant digits>.1eXXX\0".
  const int kBufferSize = kMaxSignificantDigits + 10;
  char buffer[kBufferSize];  // NOLINT: size is known at compile time.
  int buffer_pos = 0;

  // Exponent will be adjusted if insignificant digits of the integer part
  // or insignificant leading zeros of the fractional part are dropped.
  int exponent = 0;
  int significant_digits = 0;
  int insignificant_digits = 0;
  bool nonzero_digit_dropped = false;

  bool sign = false;

  if (*current == '+' || *current == '-') {
    sign = (*current == '-');
    ++current;
    const char* next_non_space = current;
    // Skip following spaces (if allowed).
    if (!AdvanceToNonspace(&next_non_space, end)) return junk_string_value_;
    if (!allow_spaces_after_sign && (current != next_non_space)) {
      return junk_string_value_;
    }
    current = next_non_space;
  }

  if (infinity_symbol_ != NULL) {
    if (*current == infinity_symbol_[0]) {
      if (!ConsumeSubString(&current, end, infinity_symbol_)) {
        return junk_string_value_;
      }

      if (!(allow_trailing_spaces || allow_trailing_junk) && (current != end)) {
        return junk_string_value_;
      }
      if (!allow_trailing_junk && AdvanceToNonspace(&current, end)) {
        return junk_string_value_;
      }

      ASSERT(buffer_pos == 0);
      *processed_characters_count = static_cast<int>(current - input);
      return sign ? -Double::Infinity() : Double::Infinity();
    }
  }

  if (nan_symbol_ != NULL) {
    if (*current == nan_symbol_[0]) {
      if (!ConsumeSubString(&current, end, nan_symbol_)) {
        return junk_string_value_;
      }

      if (!(allow_trailing_spaces || allow_trailing_junk) && (current != end)) {
        return junk_string_value_;
      }
      if (!allow_trailing_junk && AdvanceToNonspace(&current, end)) {
        return junk_string_value_;
      }

      ASSERT(buffer_pos == 0);
      *processed_characters_count = static_cast<int>(current - input);
      return sign ? -Double::NaN() : Double::NaN();
    }
  }

  bool leading_zero = false;
  if (*current == '0') {
    ++current;
    if (current == end) {
      *processed_characters_count = static_cast<int>(current - input);
      return SignedZero(sign);
    }

    leading_zero = true;

    // It could be hexadecimal value.
    if ((flags_ & ALLOW_HEX) && (*current == 'x' || *current == 'X')) {
      ++current;
      if (current == end || !isDigit(*current, 16)) {
        return junk_string_value_;  // "0x".
      }

      const char* tail_pointer = NULL;
      double result = RadixStringToIeee<4>(current,
                                           end,
                                           sign,
                                           allow_trailing_junk,
                                           junk_string_value_,
                                           read_as_double,
                                           &tail_pointer);
      if (tail_pointer != NULL) {
        if (allow_trailing_spaces) AdvanceToNonspace(&tail_pointer, end);
        *processed_characters_count = static_cast<int>(tail_pointer - input);
      }
      return result;
    }

    // Ignore leading zeros in the integer part.
    while (*current == '0') {
      ++current;
      if (current == end) {
        *processed_characters_count = static_cast<int>(current - input);
        return SignedZero(sign);
      }
    }
  }

  bool octal = leading_zero && (flags_ & ALLOW_OCTALS) != 0;

  // Copy significant digits of the integer part (if any) to the buffer.
  while (*current >= '0' && *current <= '9') {
    if (significant_digits < kMaxSignificantDigits) {
      ASSERT(buffer_pos < kBufferSize);
      buffer[buffer_pos++] = static_cast<char>(*current);
      significant_digits++;
      // Will later check if it's an octal in the buffer.
    } else {
      insignificant_digits++;  // Move the digit into the exponential part.
      nonzero_digit_dropped = nonzero_digit_dropped || *current != '0';
    }
    octal = octal && *current < '8';
    ++current;
    if (current == end) goto parsing_done;
  }

  if (significant_digits == 0) {
    octal = false;
  }

  if (*current == '.') {
    if (octal && !allow_trailing_junk) return junk_string_value_;
    if (octal) goto parsing_done;

    ++current;
    if (current == end) {
      if (significant_digits == 0 && !leading_zero) {
        return junk_string_value_;
      } else {
        goto parsing_done;
      }
    }

    if (significant_digits == 0) {
      // octal = false;
      // Integer part consists of 0 or is absent. Significant digits start after
      // leading zeros (if any).
      while (*current == '0') {
        ++current;
        if (current == end) {
          *processed_characters_count = static_cast<int>(current - input);
          return SignedZero(sign);
        }
        exponent--;  // Move this 0 into the exponent.
      }
    }

    // There is a fractional part.
    // We don't emit a '.', but adjust the exponent instead.
    while (*current >= '0' && *current <= '9') {
      if (significant_digits < kMaxSignificantDigits) {
        ASSERT(buffer_pos < kBufferSize);
        buffer[buffer_pos++] = static_cast<char>(*current);
        significant_digits++;
        exponent--;
      } else {
        // Ignore insignificant digits in the fractional part.
        nonzero_digit_dropped = nonzero_digit_dropped || *current != '0';
      }
      ++current;
      if (current == end) goto parsing_done;
    }
  }

  if (!leading_zero && exponent == 0 && significant_digits == 0) {
    // If leading_zeros is true then the string contains zeros.
    // If exponent < 0 then string was [+-]\.0*...
    // If significant_digits != 0 the string is not equal to 0.
    // Otherwise there are no digits in the string.
    return junk_string_value_;
  }

  // Parse exponential part.
  if (*current == 'e' || *current == 'E') {
    if (octal && !allow_trailing_junk) return junk_string_value_;
    if (octal) goto parsing_done;
    ++current;
    if (current == end) {
      if (allow_trailing_junk) {
        goto parsing_done;
      } else {
        return junk_string_value_;
      }
    }
    char sign_char = '+'; // @FDVALVE warning C4456: declaration of 'sign' hides previous local declaration
    if (*current == '+' || *current == '-') {
      sign_char = static_cast<char>(*current);
      ++current;
      if (current == end) {
        if (allow_trailing_junk) {
          goto parsing_done;
        } else {
          return junk_string_value_;
        }
      }
    }

    if (current == end || *current < '0' || *current > '9') {
      if (allow_trailing_junk) {
        goto parsing_done;
      } else {
        return junk_string_value_;
      }
    }

    const int max_exponent = INT_MAX / 2;
    ASSERT(-max_exponent / 2 <= exponent && exponent <= max_exponent / 2);
    int num = 0;
    do {
      // Check overflow.
      int digit = *current - '0';
      if (num >= max_exponent / 10
          && !(num == max_exponent / 10 && digit <= max_exponent % 10)) {
        num = max_exponent;
      } else {
        num = num * 10 + digit;
      }
      ++current;
    } while (current != end && *current >= '0' && *current <= '9');

    exponent += (sign_char == '-' ? -num : num);
  }

  if (!(allow_trailing_spaces || allow_trailing_junk) && (current != end)) {
    return junk_string_value_;
  }
  if (!allow_trailing_junk && AdvanceToNonspace(&current, end)) {
    return junk_string_value_;
  }
  if (allow_trailing_spaces) {
    AdvanceToNonspace(&current, end);
  }

  parsing_done:
  exponent += insignificant_digits;

  if (octal) {
    double result;
    const char* tail_pointer = NULL;
    result = RadixStringToIeee<3>(buffer,
                                  buffer + buffer_pos,
                                  sign,
                                  allow_trailing_junk,
                                  junk_string_value_,
                                  read_as_double,
                                  &tail_pointer);
    ASSERT(tail_pointer != NULL);
    *processed_characters_count = static_cast<int>(current - input);
    return result;
  }

  if (nonzero_digit_dropped) {
    buffer[buffer_pos++] = '1';
    exponent--;
  }

  ASSERT(buffer_pos < kBufferSize);
  buffer[buffer_pos] = '\0';

  double converted;
  if (read_as_double) {
    converted = Strtod(Vector<const char>(buffer, buffer_pos), exponent);
  } else {
    converted = Strtof(Vector<const char>(buffer, buffer_pos), exponent);
  }
  *processed_characters_count = static_cast<int>(current - input);
  return sign? -converted: converted;
}

}  // namespace double_conversion

namespace double_conversion {

    // 2^53 = 9007199254740992.
    // Any integer with at most 15 decimal digits will hence fit into a double
    // (which has a 53bit significand) without loss of precision.
    static const int kMaxExactDoubleIntegerDecimalDigits = 15;
    // 2^64 = 18446744073709551616 > 10^19
    static const int kMaxUint64DecimalDigits = 19;

    // Max double: 1.7976931348623157 x 10^308
    // Min non-zero double: 4.9406564584124654 x 10^-324
    // Any x >= 10^309 is interpreted as +infinity.
    // Any x <= 10^-324 is interpreted as 0.
    // Note that 2.5e-324 (despite being smaller than the min double) will be read
    // as non-zero (equal to the min non-zero double).
    static const int kMaxDecimalPower = 309;
    static const int kMinDecimalPower = -324;

    // 2^64 = 18446744073709551616
    static const uint64_t kMaxUint64 = UINT64_2PART_C(0xFFFFFFFF, FFFFFFFF);


    static const double exact_powers_of_ten[] = {
        1.0,  // 10^0
        10.0,
        100.0,
        1000.0,
        10000.0,
        100000.0,
        1000000.0,
        10000000.0,
        100000000.0,
        1000000000.0,
        10000000000.0,  // 10^10
        100000000000.0,
        1000000000000.0,
        10000000000000.0,
        100000000000000.0,
        1000000000000000.0,
        10000000000000000.0,
        100000000000000000.0,
        1000000000000000000.0,
        10000000000000000000.0,
        100000000000000000000.0,  // 10^20
        1000000000000000000000.0,
        // 10^22 = 0x21e19e0c9bab2400000 = 0x878678326eac9 * 2^22
        10000000000000000000000.0
    };
    static const int kExactPowersOfTenSize = ARRAY_SIZE(exact_powers_of_ten);

    // Maximum number of significant digits in the decimal representation.
    // In fact the value is 772 (see conversions.cc), but to give us some margin
    // we round up to 780.
    static const int kMaxSignificantDecimalDigits = 780;

    static Vector<const char> TrimLeadingZeros(Vector<const char> buffer) {
        for (int i = 0; i < buffer.length(); i++) {
            if (buffer[i] != '0') {
                return buffer.SubVector(i, buffer.length());
            }
        }
        return Vector<const char>(buffer.start(), 0);
    }


    static Vector<const char> TrimTrailingZeros(Vector<const char> buffer) {
        for (int i = buffer.length() - 1; i >= 0; --i) {
            if (buffer[i] != '0') {
                return buffer.SubVector(0, i + 1);
            }
        }
        return Vector<const char>(buffer.start(), 0);
    }


    static void CutToMaxSignificantDigits(Vector<const char> buffer,
        int exponent,
        char* significant_buffer,
        int* significant_exponent) {
        for (int i = 0; i < kMaxSignificantDecimalDigits - 1; ++i) {
            significant_buffer[i] = buffer[i];
        }
        // The input buffer has been trimmed. Therefore the last digit must be
        // different from '0'.
        ASSERT(buffer[buffer.length() - 1] != '0');
        // Set the last digit to be non-zero. This is sufficient to guarantee
        // correct rounding.
        significant_buffer[kMaxSignificantDecimalDigits - 1] = '1';
        *significant_exponent =
            exponent + (buffer.length() - kMaxSignificantDecimalDigits);
    }


    // Trims the buffer and cuts it to at most kMaxSignificantDecimalDigits.
    // If possible the input-buffer is reused, but if the buffer needs to be
    // modified (due to cutting), then the input needs to be copied into the
    // buffer_copy_space.
    static void TrimAndCut(Vector<const char> buffer, int exponent,
        char* buffer_copy_space, int space_size,
        Vector<const char>* trimmed, int* updated_exponent) {
        Vector<const char> left_trimmed = TrimLeadingZeros(buffer);
        Vector<const char> right_trimmed = TrimTrailingZeros(left_trimmed);
        exponent += left_trimmed.length() - right_trimmed.length();
        if (right_trimmed.length() > kMaxSignificantDecimalDigits) {
            (void)space_size;  // Mark variable as used.
            ASSERT(space_size >= kMaxSignificantDecimalDigits);
            CutToMaxSignificantDigits(right_trimmed, exponent,
                buffer_copy_space, updated_exponent);
            *trimmed = Vector<const char>(buffer_copy_space,
                kMaxSignificantDecimalDigits);
        }
        else {
            *trimmed = right_trimmed;
            *updated_exponent = exponent;
        }
    }


    // Reads digits from the buffer and converts them to a uint64.
    // Reads in as many digits as fit into a uint64.
    // When the string starts with "1844674407370955161" no further digit is read.
    // Since 2^64 = 18446744073709551616 it would still be possible read another
    // digit if it was less or equal than 6, but this would complicate the code.
    static uint64_t ReadUint64(Vector<const char> buffer,
        int* number_of_read_digits) {
        uint64_t result = 0;
        int i = 0;
        while (i < buffer.length() && result <= (kMaxUint64 / 10 - 1)) {
            int digit = buffer[i++] - '0';
            ASSERT(0 <= digit && digit <= 9);
            result = 10 * result + digit;
        }
        *number_of_read_digits = i;
        return result;
    }


    // Reads a DiyFp from the buffer.
    // The returned DiyFp is not necessarily normalized.
    // If remaining_decimals is zero then the returned DiyFp is accurate.
    // Otherwise it has been rounded and has error of at most 1/2 ulp.
    static void ReadDiyFp(Vector<const char> buffer,
        DiyFp* result,
        int* remaining_decimals) {
        int read_digits;
        uint64_t significand = ReadUint64(buffer, &read_digits);
        if (buffer.length() == read_digits) {
            *result = DiyFp(significand, 0);
            *remaining_decimals = 0;
        }
        else {
            // Round the significand.
            if (buffer[read_digits] >= '5') {
                significand++;
            }
            // Compute the binary exponent.
            int exponent = 0;
            *result = DiyFp(significand, exponent);
            *remaining_decimals = buffer.length() - read_digits;
        }
    }


    static bool DoubleStrtod(Vector<const char> trimmed,
        int exponent,
        double* result) {
#if !defined(DOUBLE_CONVERSION_CORRECT_DOUBLE_OPERATIONS)
        // On x86 the floating-point stack can be 64 or 80 bits wide. If it is
        // 80 bits wide (as is the case on Linux) then double-rounding occurs and the
        // result is not accurate.
        // We know that Windows32 uses 64 bits and is therefore accurate.
        // Note that the ARM simulator is compiled for 32bits. It therefore exhibits
        // the same problem.
        return false;
#endif
        if (trimmed.length() <= kMaxExactDoubleIntegerDecimalDigits) {
            int read_digits;
            // The trimmed input fits into a double.
            // If the 10^exponent (resp. 10^-exponent) fits into a double too then we
            // can compute the result-double simply by multiplying (resp. dividing) the
            // two numbers.
            // This is possible because IEEE guarantees that floating-point operations
            // return the best possible approximation.
            if (exponent < 0 && -exponent < kExactPowersOfTenSize) {
                // 10^-exponent fits into a double.
                *result = static_cast<double>(ReadUint64(trimmed, &read_digits));
                ASSERT(read_digits == trimmed.length());
                *result /= exact_powers_of_ten[-exponent];
                return true;
            }
            if (0 <= exponent && exponent < kExactPowersOfTenSize) {
                // 10^exponent fits into a double.
                *result = static_cast<double>(ReadUint64(trimmed, &read_digits));
                ASSERT(read_digits == trimmed.length());
                *result *= exact_powers_of_ten[exponent];
                return true;
            }
            int remaining_digits =
                kMaxExactDoubleIntegerDecimalDigits - trimmed.length();
            if ((0 <= exponent) &&
                (exponent - remaining_digits < kExactPowersOfTenSize)) {
                // The trimmed string was short and we can multiply it with
                // 10^remaining_digits. As a result the remaining exponent now fits
                // into a double too.
                *result = static_cast<double>(ReadUint64(trimmed, &read_digits));
                ASSERT(read_digits == trimmed.length());
                *result *= exact_powers_of_ten[remaining_digits];
                *result *= exact_powers_of_ten[exponent - remaining_digits];
                return true;
            }
        }
        return false;
    }


    // Returns 10^exponent as an exact DiyFp.
    // The given exponent must be in the range [1; kDecimalExponentDistance[.
    static DiyFp AdjustmentPowerOfTen(int exponent) {
        ASSERT(0 < exponent);
        ASSERT(exponent < PowersOfTenCache::kDecimalExponentDistance);
        // Simply hardcode the remaining powers for the given decimal exponent
        // distance.
        ASSERT(PowersOfTenCache::kDecimalExponentDistance == 8);
        switch (exponent) {
        case 1: return DiyFp(UINT64_2PART_C(0xa0000000, 00000000), -60);
        case 2: return DiyFp(UINT64_2PART_C(0xc8000000, 00000000), -57);
        case 3: return DiyFp(UINT64_2PART_C(0xfa000000, 00000000), -54);
        case 4: return DiyFp(UINT64_2PART_C(0x9c400000, 00000000), -50);
        case 5: return DiyFp(UINT64_2PART_C(0xc3500000, 00000000), -47);
        case 6: return DiyFp(UINT64_2PART_C(0xf4240000, 00000000), -44);
        case 7: return DiyFp(UINT64_2PART_C(0x98968000, 00000000), -40);
        default:
            UNREACHABLE();
        }
    }


    // If the function returns true then the result is the correct double.
    // Otherwise it is either the correct double or the double that is just below
    // the correct double.
    static bool DiyFpStrtod(Vector<const char> buffer,
        int exponent,
        double* result) {
        DiyFp input;
        int remaining_decimals;
        ReadDiyFp(buffer, &input, &remaining_decimals);
        // Since we may have dropped some digits the input is not accurate.
        // If remaining_decimals is different than 0 than the error is at most
        // .5 ulp (unit in the last place).
        // We don't want to deal with fractions and therefore keep a common
        // denominator.
        const int kDenominatorLog = 3;
        const int kDenominator = 1 << kDenominatorLog;
        // Move the remaining decimals into the exponent.
        exponent += remaining_decimals;
        int error = (remaining_decimals == 0 ? 0 : kDenominator / 2);

        int old_e = input.e();
        input.Normalize();
        error <<= Min(old_e - input.e(), 31);

        ASSERT(exponent <= PowersOfTenCache::kMaxDecimalExponent);
        if (exponent < PowersOfTenCache::kMinDecimalExponent) {
            *result = 0.0;
            return true;
        }
        DiyFp cached_power;
        int cached_decimal_exponent;
        PowersOfTenCache::GetCachedPowerForDecimalExponent(exponent,
            &cached_power,
            &cached_decimal_exponent);

        if (cached_decimal_exponent != exponent) {
            int adjustment_exponent = exponent - cached_decimal_exponent;
            DiyFp adjustment_power = AdjustmentPowerOfTen(adjustment_exponent);
            input.Multiply(adjustment_power);
            if (kMaxUint64DecimalDigits - buffer.length() >= adjustment_exponent) {
                // The product of input with the adjustment power fits into a 64 bit
                // integer.
                ASSERT(DiyFp::kSignificandSize == 64);
            }
            else {
                // The adjustment power is exact. There is hence only an error of 0.5.
                error += kDenominator / 2;
            }
        }

        input.Multiply(cached_power);
        // The error introduced by a multiplication of a*b equals
        //   error_a + error_b + error_a*error_b/2^64 + 0.5
        // Substituting a with 'input' and b with 'cached_power' we have
        //   error_b = 0.5  (all cached powers have an error of less than 0.5 ulp),
        //   error_ab = 0 or 1 / kDenominator > error_a*error_b/ 2^64
        int error_b = kDenominator / 2;
        int error_ab = (error == 0 ? 0 : 1);  // We round up to 1.
        int fixed_error = kDenominator / 2;
        error += error_b + error_ab + fixed_error;

        old_e = input.e();
        input.Normalize();
        error <<= Min(old_e - input.e(), 31);

        // See if the double's significand changes if we add/subtract the error.
        int order_of_magnitude = DiyFp::kSignificandSize + input.e();
        int effective_significand_size =
            Double::SignificandSizeForOrderOfMagnitude(order_of_magnitude);
        int precision_digits_count =
            DiyFp::kSignificandSize - effective_significand_size;
        if (precision_digits_count + kDenominatorLog >= DiyFp::kSignificandSize) {
            // This can only happen for very small denormals. In this case the
            // half-way multiplied by the denominator exceeds the range of an uint64.
            // Simply shift everything to the right.
            int shift_amount = (precision_digits_count + kDenominatorLog) -
                DiyFp::kSignificandSize + 1;
            input.set_f(input.f() >> shift_amount);
            input.set_e(input.e() + shift_amount);
            // We add 1 for the lost precision of error, and kDenominator for
            // the lost precision of input.f().
            error = (error >> shift_amount) + 1 + kDenominator;
            precision_digits_count -= shift_amount;
        }
        // We use uint64_ts now. This only works if the DiyFp uses uint64_ts too.
        ASSERT(DiyFp::kSignificandSize == 64);
        ASSERT(precision_digits_count < 64);
        uint64_t one64 = 1;
        uint64_t precision_bits_mask = (one64 << precision_digits_count) - 1;
        uint64_t precision_bits = input.f() & precision_bits_mask;
        uint64_t half_way = one64 << (precision_digits_count - 1);
        precision_bits *= kDenominator;
        half_way *= kDenominator;
        DiyFp rounded_input(input.f() >> precision_digits_count,
            input.e() + precision_digits_count);
        if (precision_bits >= half_way + error) {
            rounded_input.set_f(rounded_input.f() + 1);
        }
        // If the last_bits are too close to the half-way case than we are too
        // inaccurate and round down. In this case we return false so that we can
        // fall back to a more precise algorithm.

        *result = Double(rounded_input).value();
        if (half_way - error < precision_bits && precision_bits < half_way + error) {
            // Too imprecise. The caller will have to fall back to a slower version.
            // However the returned number is guaranteed to be either the correct
            // double, or the next-lower double.
            return false;
        }
        else {
            return true;
        }
    }


    // Returns
    //   - -1 if buffer*10^exponent < diy_fp.
    //   -  0 if buffer*10^exponent == diy_fp.
    //   - +1 if buffer*10^exponent > diy_fp.
    // Preconditions:
    //   buffer.length() + exponent <= kMaxDecimalPower + 1
    //   buffer.length() + exponent > kMinDecimalPower
    //   buffer.length() <= kMaxDecimalSignificantDigits
    static int CompareBufferWithDiyFp(Vector<const char> buffer,
        int exponent,
        DiyFp diy_fp) {
        ASSERT(buffer.length() + exponent <= kMaxDecimalPower + 1);
        ASSERT(buffer.length() + exponent > kMinDecimalPower);
        ASSERT(buffer.length() <= kMaxSignificantDecimalDigits);
        // Make sure that the Bignum will be able to hold all our numbers.
        // Our Bignum implementation has a separate field for exponents. Shifts will
        // consume at most one bigit (< 64 bits).
        // ln(10) == 3.3219...
        ASSERT(((kMaxDecimalPower + 1) * 333 / 100) < Bignum::kMaxSignificantBits);
        Bignum buffer_bignum;
        Bignum diy_fp_bignum;
        buffer_bignum.AssignDecimalString(buffer);
        diy_fp_bignum.AssignUInt64(diy_fp.f());
        if (exponent >= 0) {
            buffer_bignum.MultiplyByPowerOfTen(exponent);
        }
        else {
            diy_fp_bignum.MultiplyByPowerOfTen(-exponent);
        }
        if (diy_fp.e() > 0) {
            diy_fp_bignum.ShiftLeft(diy_fp.e());
        }
        else {
            buffer_bignum.ShiftLeft(-diy_fp.e());
        }
        return Bignum::Compare(buffer_bignum, diy_fp_bignum);
    }


    // Returns true if the guess is the correct double.
    // Returns false, when guess is either correct or the next-lower double.
    static bool ComputeGuess(Vector<const char> trimmed, int exponent,
        double* guess) {
        if (trimmed.length() == 0) {
            *guess = 0.0;
            return true;
        }
        if (exponent + trimmed.length() - 1 >= kMaxDecimalPower) {
            *guess = Double::Infinity();
            return true;
        }
        if (exponent + trimmed.length() <= kMinDecimalPower) {
            *guess = 0.0;
            return true;
        }

        if (DoubleStrtod(trimmed, exponent, guess) ||
            DiyFpStrtod(trimmed, exponent, guess)) {
            return true;
        }
        if (*guess == Double::Infinity()) {
            return true;
        }
        return false;
    }

    double Strtod(Vector<const char> buffer, int exponent) {
        char copy_buffer[kMaxSignificantDecimalDigits];
        Vector<const char> trimmed;
        int updated_exponent;
        TrimAndCut(buffer, exponent, copy_buffer, kMaxSignificantDecimalDigits,
            &trimmed, &updated_exponent);
        exponent = updated_exponent;

        double guess;
        bool is_correct = ComputeGuess(trimmed, exponent, &guess);
        if (is_correct) return guess;

        DiyFp upper_boundary = Double(guess).UpperBoundary();
        int comparison = CompareBufferWithDiyFp(trimmed, exponent, upper_boundary);
        if (comparison < 0) {
            return guess;
        }
        else if (comparison > 0) {
            return Double(guess).NextDouble();
        }
        else if ((Double(guess).Significand() & 1) == 0) {
            // Round towards even.
            return guess;
        }
        else {
            return Double(guess).NextDouble();
        }
    }

    float Strtof(Vector<const char> buffer, int exponent) {
        char copy_buffer[kMaxSignificantDecimalDigits];
        Vector<const char> trimmed;
        int updated_exponent;
        TrimAndCut(buffer, exponent, copy_buffer, kMaxSignificantDecimalDigits,
            &trimmed, &updated_exponent);
        exponent = updated_exponent;

        double double_guess;
        bool is_correct = ComputeGuess(trimmed, exponent, &double_guess);

        float float_guess = static_cast<float>(double_guess);
        if (float_guess == double_guess) {
            // This shortcut triggers for integer values.
            return float_guess;
        }

        // We must catch double-rounding. Say the double has been rounded up, and is
        // now a boundary of a float, and rounds up again. This is why we have to
        // look at previous too.
        // Example (in decimal numbers):
        //    input: 12349
        //    high-precision (4 digits): 1235
        //    low-precision (3 digits):
        //       when read from input: 123
        //       when rounded from high precision: 124.
        // To do this we simply look at the neigbors of the correct result and see
        // if they would round to the same float. If the guess is not correct we have
        // to look at four values (since two different doubles could be the correct
        // double).

        double double_next = Double(double_guess).NextDouble();
        double double_previous = Double(double_guess).PreviousDouble();

        float f1 = static_cast<float>(double_previous);
        float f2 = float_guess;
        float f3 = static_cast<float>(double_next);
        float f4;
        if (is_correct) {
            f4 = f3;
        }
        else {
            double double_next2 = Double(double_next).NextDouble();
            f4 = static_cast<float>(double_next2);
        }
        (void)f2;  // Mark variable as used.
        ASSERT(f1 <= f2 && f2 <= f3 && f3 <= f4);

        // If the guess doesn't lie near a single-precision boundary we can simply
        // return its float-value.
        if (f1 == f4) {
            return float_guess;
        }

        ASSERT((f1 != f2 && f2 == f3 && f3 == f4) ||
            (f1 == f2 && f2 != f3 && f3 == f4) ||
            (f1 == f2 && f2 == f3 && f3 != f4));

        // guess and next are the two possible canditates (in the same way that
        // double_guess was the lower candidate for a double-precision guess).
        float guess = f1;
        float next = f4;
        DiyFp upper_boundary;
        if (guess == 0.0f) {
            float min_float = 1e-45f;
            upper_boundary = Double(static_cast<double>(min_float) / 2).AsDiyFp();
        }
        else {
            upper_boundary = Single(guess).UpperBoundary();
        }
        int comparison = CompareBufferWithDiyFp(trimmed, exponent, upper_boundary);
        if (comparison < 0) {
            return guess;
        }
        else if (comparison > 0) {
            return next;
        }
        else if ((Single(guess).Significand() & 1) == 0) {
            // Round towards even.
            return guess;
        }
        else {
            return next;
        }
    }

}  // namespace double_conversion

namespace double_conversion {

    // Represents a 128bit type. This class should be replaced by a native type on
    // platforms that support 128bit integers.
    class UInt128 {
    public:
        UInt128() : high_bits_(0), low_bits_(0) { }
        UInt128(uint64_t high, uint64_t low) : high_bits_(high), low_bits_(low) { }

        void Multiply(uint32_t multiplicand) {
            uint64_t accumulator;

            accumulator = (low_bits_ & kMask32) * multiplicand;
            uint32_t part = static_cast<uint32_t>(accumulator & kMask32);
            accumulator >>= 32;
            accumulator = accumulator + (low_bits_ >> 32) * multiplicand;
            low_bits_ = (accumulator << 32) + part;
            accumulator >>= 32;
            accumulator = accumulator + (high_bits_ & kMask32) * multiplicand;
            part = static_cast<uint32_t>(accumulator & kMask32);
            accumulator >>= 32;
            accumulator = accumulator + (high_bits_ >> 32) * multiplicand;
            high_bits_ = (accumulator << 32) + part;
            ASSERT((accumulator >> 32) == 0);
        }

        void Shift(int shift_amount) {
            ASSERT(-64 <= shift_amount && shift_amount <= 64);
            if (shift_amount == 0) {
                return;
            }
            else if (shift_amount == -64) {
                high_bits_ = low_bits_;
                low_bits_ = 0;
            }
            else if (shift_amount == 64) {
                low_bits_ = high_bits_;
                high_bits_ = 0;
            }
            else if (shift_amount <= 0) {
                high_bits_ <<= -shift_amount;
                high_bits_ += low_bits_ >> (64 + shift_amount);
                low_bits_ <<= -shift_amount;
            }
            else {
                low_bits_ >>= shift_amount;
                low_bits_ += high_bits_ << (64 - shift_amount);
                high_bits_ >>= shift_amount;
            }
        }

        // Modifies *this to *this MOD (2^power).
        // Returns *this DIV (2^power).
        int DivModPowerOf2(int power) {
            if (power >= 64) {
                int result = static_cast<int>(high_bits_ >> (power - 64));
                high_bits_ -= static_cast<uint64_t>(result) << (power - 64);
                return result;
            }
            else {
                uint64_t part_low = low_bits_ >> power;
                uint64_t part_high = high_bits_ << (64 - power);
                int result = static_cast<int>(part_low + part_high);
                high_bits_ = 0;
                low_bits_ -= part_low << power;
                return result;
            }
        }

        bool IsZero() const {
            return high_bits_ == 0 && low_bits_ == 0;
        }

        int BitAt(int position) {
            if (position >= 64) {
                return static_cast<int>(high_bits_ >> (position - 64)) & 1;
            }
            else {
                return static_cast<int>(low_bits_ >> position) & 1;
            }
        }

    private:
        static const uint64_t kMask32 = 0xFFFFFFFF;
        // Value == (high_bits_ << 64) + low_bits_
        uint64_t high_bits_;
        uint64_t low_bits_;
    };


    static const int kDoubleSignificandSize = 53;  // Includes the hidden bit.


    static void FillDigits32FixedLength(uint32_t number, int requested_length,
        Vector<char> buffer, int* length) {
        for (int i = requested_length - 1; i >= 0; --i) {
            buffer[(*length) + i] = '0' + number % 10;
            number /= 10;
        }
        *length += requested_length;
    }


    static void FillDigits32(uint32_t number, Vector<char> buffer, int* length) {
        int number_length = 0;
        // We fill the digits in reverse order and exchange them afterwards.
        while (number != 0) {
            int digit = number % 10;
            number /= 10;
            buffer[(*length) + number_length] = static_cast<char>('0' + digit);
            number_length++;
        }
        // Exchange the digits.
        int i = *length;
        int j = *length + number_length - 1;
        while (i < j) {
            char tmp = buffer[i];
            buffer[i] = buffer[j];
            buffer[j] = tmp;
            i++;
            j--;
        }
        *length += number_length;
    }


    static void FillDigits64FixedLength(uint64_t number,
        Vector<char> buffer, int* length) {
        const uint32_t kTen7 = 10000000;
        // For efficiency cut the number into 3 uint32_t parts, and print those.
        uint32_t part2 = static_cast<uint32_t>(number % kTen7);
        number /= kTen7;
        uint32_t part1 = static_cast<uint32_t>(number % kTen7);
        uint32_t part0 = static_cast<uint32_t>(number / kTen7);

        FillDigits32FixedLength(part0, 3, buffer, length);
        FillDigits32FixedLength(part1, 7, buffer, length);
        FillDigits32FixedLength(part2, 7, buffer, length);
    }


    static void FillDigits64(uint64_t number, Vector<char> buffer, int* length) {
        const uint32_t kTen7 = 10000000;
        // For efficiency cut the number into 3 uint32_t parts, and print those.
        uint32_t part2 = static_cast<uint32_t>(number % kTen7);
        number /= kTen7;
        uint32_t part1 = static_cast<uint32_t>(number % kTen7);
        uint32_t part0 = static_cast<uint32_t>(number / kTen7);

        if (part0 != 0) {
            FillDigits32(part0, buffer, length);
            FillDigits32FixedLength(part1, 7, buffer, length);
            FillDigits32FixedLength(part2, 7, buffer, length);
        }
        else if (part1 != 0) {
            FillDigits32(part1, buffer, length);
            FillDigits32FixedLength(part2, 7, buffer, length);
        }
        else {
            FillDigits32(part2, buffer, length);
        }
    }


    static void RoundUp(Vector<char> buffer, int* length, int* decimal_point) {
        // An empty buffer represents 0.
        if (*length == 0) {
            buffer[0] = '1';
            *decimal_point = 1;
            *length = 1;
            return;
        }
        // Round the last digit until we either have a digit that was not '9' or until
        // we reached the first digit.
        buffer[(*length) - 1]++;
        for (int i = (*length) - 1; i > 0; --i) {
            if (buffer[i] != '0' + 10) {
                return;
            }
            buffer[i] = '0';
            buffer[i - 1]++;
        }
        // If the first digit is now '0' + 10, we would need to set it to '0' and add
        // a '1' in front. However we reach the first digit only if all following
        // digits had been '9' before rounding up. Now all trailing digits are '0' and
        // we simply switch the first digit to '1' and update the decimal-point
        // (indicating that the point is now one digit to the right).
        if (buffer[0] == '0' + 10) {
            buffer[0] = '1';
            (*decimal_point)++;
        }
    }


    // The given fractionals number represents a fixed-point number with binary
    // point at bit (-exponent).
    // Preconditions:
    //   -128 <= exponent <= 0.
    //   0 <= fractionals * 2^exponent < 1
    //   The buffer holds the result.
    // The function will round its result. During the rounding-process digits not
    // generated by this function might be updated, and the decimal-point variable
    // might be updated. If this function generates the digits 99 and the buffer
    // already contained "199" (thus yielding a buffer of "19999") then a
    // rounding-up will change the contents of the buffer to "20000".
    static void FillFractionals(uint64_t fractionals, int exponent,
        int fractional_count, Vector<char> buffer,
        int* length, int* decimal_point) {
        ASSERT(-128 <= exponent && exponent <= 0);
        // 'fractionals' is a fixed-point number, with binary point at bit
        // (-exponent). Inside the function the non-converted remainder of fractionals
        // is a fixed-point number, with binary point at bit 'point'.
        if (-exponent <= 64) {
            // One 64 bit number is sufficient.
            ASSERT(fractionals >> 56 == 0);
            int point = -exponent;
            for (int i = 0; i < fractional_count; ++i) {
                if (fractionals == 0) break;
                // Instead of multiplying by 10 we multiply by 5 and adjust the point
                // location. This way the fractionals variable will not overflow.
                // Invariant at the beginning of the loop: fractionals < 2^point.
                // Initially we have: point <= 64 and fractionals < 2^56
                // After each iteration the point is decremented by one.
                // Note that 5^3 = 125 < 128 = 2^7.
                // Therefore three iterations of this loop will not overflow fractionals
                // (even without the subtraction at the end of the loop body). At this
                // time point will satisfy point <= 61 and therefore fractionals < 2^point
                // and any further multiplication of fractionals by 5 will not overflow.
                fractionals *= 5;
                point--;
                int digit = static_cast<int>(fractionals >> point);
                ASSERT(digit <= 9);
                buffer[*length] = static_cast<char>('0' + digit);
                (*length)++;
                fractionals -= static_cast<uint64_t>(digit) << point;
            }
            // If the first bit after the point is set we have to round up.
            if (((fractionals >> (point - 1)) & 1) == 1) {
                RoundUp(buffer, length, decimal_point);
            }
        }
        else {  // We need 128 bits.
            ASSERT(64 < -exponent && -exponent <= 128);
            UInt128 fractionals128 = UInt128(fractionals, 0);
            fractionals128.Shift(-exponent - 64);
            int point = 128;
            for (int i = 0; i < fractional_count; ++i) {
                if (fractionals128.IsZero()) break;
                // As before: instead of multiplying by 10 we multiply by 5 and adjust the
                // point location.
                // This multiplication will not overflow for the same reasons as before.
                fractionals128.Multiply(5);
                point--;
                int digit = fractionals128.DivModPowerOf2(point);
                ASSERT(digit <= 9);
                buffer[*length] = static_cast<char>('0' + digit);
                (*length)++;
            }
            if (fractionals128.BitAt(point - 1) == 1) {
                RoundUp(buffer, length, decimal_point);
            }
        }
    }


    // Removes leading and trailing zeros.
    // If leading zeros are removed then the decimal point position is adjusted.
    static void TrimZeros(Vector<char> buffer, int* length, int* decimal_point) {
        while (*length > 0 && buffer[(*length) - 1] == '0') {
            (*length)--;
        }
        int first_non_zero = 0;
        while (first_non_zero < *length && buffer[first_non_zero] == '0') {
            first_non_zero++;
        }
        if (first_non_zero != 0) {
            for (int i = first_non_zero; i < *length; ++i) {
                buffer[i - first_non_zero] = buffer[i];
            }
            *length -= first_non_zero;
            *decimal_point -= first_non_zero;
        }
    }


    bool FastFixedDtoa(double v,
        int fractional_count,
        Vector<char> buffer,
        int* length,
        int* decimal_point) {
        const uint32_t kMaxUInt32 = 0xFFFFFFFF;
        uint64_t significand = Double(v).Significand();
        int exponent = Double(v).Exponent();
        // v = significand * 2^exponent (with significand a 53bit integer).
        // If the exponent is larger than 20 (i.e. we may have a 73bit number) then we
        // don't know how to compute the representation. 2^73 ~= 9.5*10^21.
        // If necessary this limit could probably be increased, but we don't need
        // more.
        if (exponent > 20) return false;
        if (fractional_count > 20) return false;
        *length = 0;
        // At most kDoubleSignificandSize bits of the significand are non-zero.
        // Given a 64 bit integer we have 11 0s followed by 53 potentially non-zero
        // bits:  0..11*..0xxx..53*..xx
        if (exponent + kDoubleSignificandSize > 64) {
            // The exponent must be > 11.
            //
            // We know that v = significand * 2^exponent.
            // And the exponent > 11.
            // We simplify the task by dividing v by 10^17.
            // The quotient delivers the first digits, and the remainder fits into a 64
            // bit number.
            // Dividing by 10^17 is equivalent to dividing by 5^17*2^17.
            const uint64_t kFive17 = UINT64_2PART_C(0xB1, A2BC2EC5);  // 5^17
            uint64_t divisor = kFive17;
            int divisor_power = 17;
            uint64_t dividend = significand;
            uint32_t quotient;
            uint64_t remainder;
            // Let v = f * 2^e with f == significand and e == exponent.
            // Then need q (quotient) and r (remainder) as follows:
            //   v            = q * 10^17       + r
            //   f * 2^e      = q * 10^17       + r
            //   f * 2^e      = q * 5^17 * 2^17 + r
            // If e > 17 then
            //   f * 2^(e-17) = q * 5^17        + r/2^17
            // else
            //   f  = q * 5^17 * 2^(17-e) + r/2^e
            if (exponent > divisor_power) {
                // We only allow exponents of up to 20 and therefore (17 - e) <= 3
                dividend <<= exponent - divisor_power;
                quotient = static_cast<uint32_t>(dividend / divisor);
                remainder = (dividend % divisor) << divisor_power;
            }
            else {
                divisor <<= divisor_power - exponent;
                quotient = static_cast<uint32_t>(dividend / divisor);
                remainder = (dividend % divisor) << exponent;
            }
            FillDigits32(quotient, buffer, length);
            FillDigits64FixedLength(remainder, buffer, length);
            *decimal_point = *length;
        }
        else if (exponent >= 0) {
            // 0 <= exponent <= 11
            significand <<= exponent;
            FillDigits64(significand, buffer, length);
            *decimal_point = *length;
        }
        else if (exponent > -kDoubleSignificandSize) {
            // We have to cut the number.
            uint64_t integrals = significand >> -exponent;
            uint64_t fractionals = significand - (integrals << -exponent);
            if (integrals > kMaxUInt32) {
                FillDigits64(integrals, buffer, length);
            }
            else {
                FillDigits32(static_cast<uint32_t>(integrals), buffer, length);
            }
            *decimal_point = *length;
            FillFractionals(fractionals, exponent, fractional_count,
                buffer, length, decimal_point);
        }
        else if (exponent < -128) {
            // This configuration (with at most 20 digits) means that all digits must be
            // 0.
            ASSERT(fractional_count <= 20);
            buffer[0] = '\0';
            *length = 0;
            *decimal_point = -fractional_count;
        }
        else {
            *decimal_point = 0;
            FillFractionals(significand, exponent, fractional_count,
                buffer, length, decimal_point);
        }
        TrimZeros(buffer, length, decimal_point);
        buffer[*length] = '\0';
        if ((*length) == 0) {
            // The string is empty and the decimal_point thus has no importance. Mimick
            // Gay's dtoa and and set it to -fractional_count.
            *decimal_point = -fractional_count;
        }
        return true;
    }

}  // namespace double_conversion

namespace double_conversion {

    // The minimal and maximal target exponent define the range of w's binary
    // exponent, where 'w' is the result of multiplying the input by a cached power
    // of ten.
    //
    // A different range might be chosen on a different platform, to optimize digit
    // generation, but a smaller range requires more powers of ten to be cached.
    static const int kMinimalTargetExponent = -60;
    static const int kMaximalTargetExponent = -32;


    // Adjusts the last digit of the generated number, and screens out generated
    // solutions that may be inaccurate. A solution may be inaccurate if it is
    // outside the safe interval, or if we cannot prove that it is closer to the
    // input than a neighboring representation of the same length.
    //
    // Input: * buffer containing the digits of too_high / 10^kappa
    //        * the buffer's length
    //        * distance_too_high_w == (too_high - w).f() * unit
    //        * unsafe_interval == (too_high - too_low).f() * unit
    //        * rest = (too_high - buffer * 10^kappa).f() * unit
    //        * ten_kappa = 10^kappa * unit
    //        * unit = the common multiplier
    // Output: returns true if the buffer is guaranteed to contain the closest
    //    representable number to the input.
    //  Modifies the generated digits in the buffer to approach (round towards) w.
    static bool RoundWeed(Vector<char> buffer,
        int length,
        uint64_t distance_too_high_w,
        uint64_t unsafe_interval,
        uint64_t rest,
        uint64_t ten_kappa,
        uint64_t unit) {
        uint64_t small_distance = distance_too_high_w - unit;
        uint64_t big_distance = distance_too_high_w + unit;
        // Let w_low  = too_high - big_distance, and
        //     w_high = too_high - small_distance.
        // Note: w_low < w < w_high
        //
        // The real w (* unit) must lie somewhere inside the interval
        // ]w_low; w_high[ (often written as "(w_low; w_high)")

        // Basically the buffer currently contains a number in the unsafe interval
        // ]too_low; too_high[ with too_low < w < too_high
        //
        //  too_high - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        //                     ^v 1 unit            ^      ^                 ^      ^
        //  boundary_high ---------------------     .      .                 .      .
        //                     ^v 1 unit            .      .                 .      .
        //   - - - - - - - - - - - - - - - - - - -  +  - - + - - - - - -     .      .
        //                                          .      .         ^       .      .
        //                                          .  big_distance  .       .      .
        //                                          .      .         .       .    rest
        //                              small_distance     .         .       .      .
        //                                          v      .         .       .      .
        //  w_high - - - - - - - - - - - - - - - - - -     .         .       .      .
        //                     ^v 1 unit                   .         .       .      .
        //  w ----------------------------------------     .         .       .      .
        //                     ^v 1 unit                   v         .       .      .
        //  w_low  - - - - - - - - - - - - - - - - - - - - -         .       .      .
        //                                                           .       .      v
        //  buffer --------------------------------------------------+-------+--------
        //                                                           .       .
        //                                                  safe_interval    .
        //                                                           v       .
        //   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -     .
        //                     ^v 1 unit                                     .
        //  boundary_low -------------------------                     unsafe_interval
        //                     ^v 1 unit                                     v
        //  too_low  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        //
        //
        // Note that the value of buffer could lie anywhere inside the range too_low
        // to too_high.
        //
        // boundary_low, boundary_high and w are approximations of the real boundaries
        // and v (the input number). They are guaranteed to be precise up to one unit.
        // In fact the error is guaranteed to be strictly less than one unit.
        //
        // Anything that lies outside the unsafe interval is guaranteed not to round
        // to v when read again.
        // Anything that lies inside the safe interval is guaranteed to round to v
        // when read again.
        // If the number inside the buffer lies inside the unsafe interval but not
        // inside the safe interval then we simply do not know and bail out (returning
        // false).
        //
        // Similarly we have to take into account the imprecision of 'w' when finding
        // the closest representation of 'w'. If we have two potential
        // representations, and one is closer to both w_low and w_high, then we know
        // it is closer to the actual value v.
        //
        // By generating the digits of too_high we got the largest (closest to
        // too_high) buffer that is still in the unsafe interval. In the case where
        // w_high < buffer < too_high we try to decrement the buffer.
        // This way the buffer approaches (rounds towards) w.
        // There are 3 conditions that stop the decrementation process:
        //   1) the buffer is already below w_high
        //   2) decrementing the buffer would make it leave the unsafe interval
        //   3) decrementing the buffer would yield a number below w_high and farther
        //      away than the current number. In other words:
        //              (buffer{-1} < w_high) && w_high - buffer{-1} > buffer - w_high
        // Instead of using the buffer directly we use its distance to too_high.
        // Conceptually rest ~= too_high - buffer
        // We need to do the following tests in this order to avoid over- and
        // underflows.
        ASSERT(rest <= unsafe_interval);
        while (rest < small_distance &&  // Negated condition 1
            unsafe_interval - rest >= ten_kappa &&  // Negated condition 2
            (rest + ten_kappa < small_distance ||  // buffer{-1} > w_high
            small_distance - rest >= rest + ten_kappa - small_distance)) {
            buffer[length - 1]--;
            rest += ten_kappa;
        }

        // We have approached w+ as much as possible. We now test if approaching w-
        // would require changing the buffer. If yes, then we have two possible
        // representations close to w, but we cannot decide which one is closer.
        if (rest < big_distance &&
            unsafe_interval - rest >= ten_kappa &&
            (rest + ten_kappa < big_distance ||
            big_distance - rest > rest + ten_kappa - big_distance)) {
            return false;
        }

        // Weeding test.
        //   The safe interval is [too_low + 2 ulp; too_high - 2 ulp]
        //   Since too_low = too_high - unsafe_interval this is equivalent to
        //      [too_high - unsafe_interval + 4 ulp; too_high - 2 ulp]
        //   Conceptually we have: rest ~= too_high - buffer
        return (2 * unit <= rest) && (rest <= unsafe_interval - 4 * unit);
    }


    // Rounds the buffer upwards if the result is closer to v by possibly adding
    // 1 to the buffer. If the precision of the calculation is not sufficient to
    // round correctly, return false.
    // The rounding might shift the whole buffer in which case the kappa is
    // adjusted. For example "99", kappa = 3 might become "10", kappa = 4.
    //
    // If 2*rest > ten_kappa then the buffer needs to be round up.
    // rest can have an error of +/- 1 unit. This function accounts for the
    // imprecision and returns false, if the rounding direction cannot be
    // unambiguously determined.
    //
    // Precondition: rest < ten_kappa.
    static bool RoundWeedCounted(Vector<char> buffer,
        int length,
        uint64_t rest,
        uint64_t ten_kappa,
        uint64_t unit,
        int* kappa) {
        ASSERT(rest < ten_kappa);
        // The following tests are done in a specific order to avoid overflows. They
        // will work correctly with any uint64 values of rest < ten_kappa and unit.
        //
        // If the unit is too big, then we don't know which way to round. For example
        // a unit of 50 means that the real number lies within rest +/- 50. If
        // 10^kappa == 40 then there is no way to tell which way to round.
        if (unit >= ten_kappa) return false;
        // Even if unit is just half the size of 10^kappa we are already completely
        // lost. (And after the previous test we know that the expression will not
        // over/underflow.)
        if (ten_kappa - unit <= unit) return false;
        // If 2 * (rest + unit) <= 10^kappa we can safely round down.
        if ((ten_kappa - rest > rest) && (ten_kappa - 2 * rest >= 2 * unit)) {
            return true;
        }
        // If 2 * (rest - unit) >= 10^kappa, then we can safely round up.
        if ((rest > unit) && (ten_kappa - (rest - unit) <= (rest - unit))) {
            // Increment the last digit recursively until we find a non '9' digit.
            buffer[length - 1]++;
            for (int i = length - 1; i > 0; --i) {
                if (buffer[i] != '0' + 10) break;
                buffer[i] = '0';
                buffer[i - 1]++;
            }
            // If the first digit is now '0'+ 10 we had a buffer with all '9's. With the
            // exception of the first digit all digits are now '0'. Simply switch the
            // first digit to '1' and adjust the kappa. Example: "99" becomes "10" and
            // the power (the kappa) is increased.
            if (buffer[0] == '0' + 10) {
                buffer[0] = '1';
                (*kappa) += 1;
            }
            return true;
        }
        return false;
    }

    // Returns the biggest power of ten that is less than or equal to the given
    // number. We furthermore receive the maximum number of bits 'number' has.
    //
    // Returns power == 10^(exponent_plus_one-1) such that
    //    power <= number < power * 10.
    // If number_bits == 0 then 0^(0-1) is returned.
    // The number of bits must be <= 32.
    // Precondition: number < (1 << (number_bits + 1)).

    // Inspired by the method for finding an integer log base 10 from here:
    // http://graphics.stanford.edu/~seander/bithacks.html#IntegerLog10
    static unsigned int const kSmallPowersOfTen[] =
    { 0, 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000,
    1000000000 };

    static void BiggestPowerTen(uint32_t number,
        int number_bits,
        uint32_t* power,
        int* exponent_plus_one) {
        ASSERT(number < (1u << (number_bits + 1)));
        // 1233/4096 is approximately 1/lg(10).
        int exponent_plus_one_guess = ((number_bits + 1) * 1233 >> 12);
        // We increment to skip over the first entry in the kPowersOf10 table.
        // Note: kPowersOf10[i] == 10^(i-1).
        exponent_plus_one_guess++;
        // We don't have any guarantees that 2^number_bits <= number.
        if (number < kSmallPowersOfTen[exponent_plus_one_guess]) {
            exponent_plus_one_guess--;
        }
        *power = kSmallPowersOfTen[exponent_plus_one_guess];
        *exponent_plus_one = exponent_plus_one_guess;
    }

    // Generates the digits of input number w.
    // w is a floating-point number (DiyFp), consisting of a significand and an
    // exponent. Its exponent is bounded by kMinimalTargetExponent and
    // kMaximalTargetExponent.
    //       Hence -60 <= w.e() <= -32.
    //
    // Returns false if it fails, in which case the generated digits in the buffer
    // should not be used.
    // Preconditions:
    //  * low, w and high are correct up to 1 ulp (unit in the last place). That
    //    is, their error must be less than a unit of their last digits.
    //  * low.e() == w.e() == high.e()
    //  * low < w < high, and taking into account their error: low~ <= high~
    //  * kMinimalTargetExponent <= w.e() <= kMaximalTargetExponent
    // Postconditions: returns false if procedure fails.
    //   otherwise:
    //     * buffer is not null-terminated, but len contains the number of digits.
    //     * buffer contains the shortest possible decimal digit-sequence
    //       such that LOW < buffer * 10^kappa < HIGH, where LOW and HIGH are the
    //       correct values of low and high (without their error).
    //     * if more than one decimal representation gives the minimal number of
    //       decimal digits then the one closest to W (where W is the correct value
    //       of w) is chosen.
    // Remark: this procedure takes into account the imprecision of its input
    //   numbers. If the precision is not enough to guarantee all the postconditions
    //   then false is returned. This usually happens rarely (~0.5%).
    //
    // Say, for the sake of example, that
    //   w.e() == -48, and w.f() == 0x1234567890abcdef
    // w's value can be computed by w.f() * 2^w.e()
    // We can obtain w's integral digits by simply shifting w.f() by -w.e().
    //  -> w's integral part is 0x1234
    //  w's fractional part is therefore 0x567890abcdef.
    // Printing w's integral part is easy (simply print 0x1234 in decimal).
    // In order to print its fraction we repeatedly multiply the fraction by 10 and
    // get each digit. Example the first digit after the point would be computed by
    //   (0x567890abcdef * 10) >> 48. -> 3
    // The whole thing becomes slightly more complicated because we want to stop
    // once we have enough digits. That is, once the digits inside the buffer
    // represent 'w' we can stop. Everything inside the interval low - high
    // represents w. However we have to pay attention to low, high and w's
    // imprecision.
    static bool DigitGen(DiyFp low,
        DiyFp w,
        DiyFp high,
        Vector<char> buffer,
        int* length,
        int* kappa) {
        ASSERT(low.e() == w.e() && w.e() == high.e());
        ASSERT(low.f() + 1 <= high.f() - 1);
        ASSERT(kMinimalTargetExponent <= w.e() && w.e() <= kMaximalTargetExponent);
        // low, w and high are imprecise, but by less than one ulp (unit in the last
        // place).
        // If we remove (resp. add) 1 ulp from low (resp. high) we are certain that
        // the new numbers are outside of the interval we want the final
        // representation to lie in.
        // Inversely adding (resp. removing) 1 ulp from low (resp. high) would yield
        // numbers that are certain to lie in the interval. We will use this fact
        // later on.
        // We will now start by generating the digits within the uncertain
        // interval. Later we will weed out representations that lie outside the safe
        // interval and thus _might_ lie outside the correct interval.
        uint64_t unit = 1;
        DiyFp too_low = DiyFp(low.f() - unit, low.e());
        DiyFp too_high = DiyFp(high.f() + unit, high.e());
        // too_low and too_high are guaranteed to lie outside the interval we want the
        // generated number in.
        DiyFp unsafe_interval = DiyFp::Minus(too_high, too_low);
        // We now cut the input number into two parts: the integral digits and the
        // fractionals. We will not write any decimal separator though, but adapt
        // kappa instead.
        // Reminder: we are currently computing the digits (stored inside the buffer)
        // such that:   too_low < buffer * 10^kappa < too_high
        // We use too_high for the digit_generation and stop as soon as possible.
        // If we stop early we effectively round down.
        DiyFp one = DiyFp(static_cast<uint64_t>(1) << -w.e(), w.e());
        // Division by one is a shift.
        uint32_t integrals = static_cast<uint32_t>(too_high.f() >> -one.e());
        // Modulo by one is an and.
        uint64_t fractionals = too_high.f() & (one.f() - 1);
        uint32_t divisor;
        int divisor_exponent_plus_one;
        BiggestPowerTen(integrals, DiyFp::kSignificandSize - (-one.e()),
            &divisor, &divisor_exponent_plus_one);
        *kappa = divisor_exponent_plus_one;
        *length = 0;
        // Loop invariant: buffer = too_high / 10^kappa  (integer division)
        // The invariant holds for the first iteration: kappa has been initialized
        // with the divisor exponent + 1. And the divisor is the biggest power of ten
        // that is smaller than integrals.
        while (*kappa > 0) {
            int digit = integrals / divisor;
            ASSERT(digit <= 9);
            buffer[*length] = static_cast<char>('0' + digit);
            (*length)++;
            integrals %= divisor;
            (*kappa)--;
            // Note that kappa now equals the exponent of the divisor and that the
            // invariant thus holds again.
            uint64_t rest =
                (static_cast<uint64_t>(integrals) << -one.e()) + fractionals;
            // Invariant: too_high = buffer * 10^kappa + DiyFp(rest, one.e())
            // Reminder: unsafe_interval.e() == one.e()
            if (rest < unsafe_interval.f()) {
                // Rounding down (by not emitting the remaining digits) yields a number
                // that lies within the unsafe interval.
                return RoundWeed(buffer, *length, DiyFp::Minus(too_high, w).f(),
                    unsafe_interval.f(), rest,
                    static_cast<uint64_t>(divisor) << -one.e(), unit);
            }
            divisor /= 10;
        }

        // The integrals have been generated. We are at the point of the decimal
        // separator. In the following loop we simply multiply the remaining digits by
        // 10 and divide by one. We just need to pay attention to multiply associated
        // data (like the interval or 'unit'), too.
        // Note that the multiplication by 10 does not overflow, because w.e >= -60
        // and thus one.e >= -60.
        ASSERT(one.e() >= -60);
        ASSERT(fractionals < one.f());
        ASSERT(UINT64_2PART_C(0xFFFFFFFF, FFFFFFFF) / 10 >= one.f());
        for (;;) {
            fractionals *= 10;
            unit *= 10;
            unsafe_interval.set_f(unsafe_interval.f() * 10);
            // Integer division by one.
            int digit = static_cast<int>(fractionals >> -one.e());
            ASSERT(digit <= 9);
            buffer[*length] = static_cast<char>('0' + digit);
            (*length)++;
            fractionals &= one.f() - 1;  // Modulo by one.
            (*kappa)--;
            if (fractionals < unsafe_interval.f()) {
                return RoundWeed(buffer, *length, DiyFp::Minus(too_high, w).f() * unit,
                    unsafe_interval.f(), fractionals, one.f(), unit);
            }
        }
    }



    // Generates (at most) requested_digits digits of input number w.
    // w is a floating-point number (DiyFp), consisting of a significand and an
    // exponent. Its exponent is bounded by kMinimalTargetExponent and
    // kMaximalTargetExponent.
    //       Hence -60 <= w.e() <= -32.
    //
    // Returns false if it fails, in which case the generated digits in the buffer
    // should not be used.
    // Preconditions:
    //  * w is correct up to 1 ulp (unit in the last place). That
    //    is, its error must be strictly less than a unit of its last digit.
    //  * kMinimalTargetExponent <= w.e() <= kMaximalTargetExponent
    //
    // Postconditions: returns false if procedure fails.
    //   otherwise:
    //     * buffer is not null-terminated, but length contains the number of
    //       digits.
    //     * the representation in buffer is the most precise representation of
    //       requested_digits digits.
    //     * buffer contains at most requested_digits digits of w. If there are less
    //       than requested_digits digits then some trailing '0's have been removed.
    //     * kappa is such that
    //            w = buffer * 10^kappa + eps with |eps| < 10^kappa / 2.
    //
    // Remark: This procedure takes into account the imprecision of its input
    //   numbers. If the precision is not enough to guarantee all the postconditions
    //   then false is returned. This usually happens rarely, but the failure-rate
    //   increases with higher requested_digits.
    static bool DigitGenCounted(DiyFp w,
        int requested_digits,
        Vector<char> buffer,
        int* length,
        int* kappa) {
        ASSERT(kMinimalTargetExponent <= w.e() && w.e() <= kMaximalTargetExponent);
        ASSERT(kMinimalTargetExponent >= -60);
        ASSERT(kMaximalTargetExponent <= -32);
        // w is assumed to have an error less than 1 unit. Whenever w is scaled we
        // also scale its error.
        uint64_t w_error = 1;
        // We cut the input number into two parts: the integral digits and the
        // fractional digits. We don't emit any decimal separator, but adapt kappa
        // instead. Example: instead of writing "1.2" we put "12" into the buffer and
        // increase kappa by 1.
        DiyFp one = DiyFp(static_cast<uint64_t>(1) << -w.e(), w.e());
        // Division by one is a shift.
        uint32_t integrals = static_cast<uint32_t>(w.f() >> -one.e());
        // Modulo by one is an and.
        uint64_t fractionals = w.f() & (one.f() - 1);
        uint32_t divisor;
        int divisor_exponent_plus_one;
        BiggestPowerTen(integrals, DiyFp::kSignificandSize - (-one.e()),
            &divisor, &divisor_exponent_plus_one);
        *kappa = divisor_exponent_plus_one;
        *length = 0;

        // Loop invariant: buffer = w / 10^kappa  (integer division)
        // The invariant holds for the first iteration: kappa has been initialized
        // with the divisor exponent + 1. And the divisor is the biggest power of ten
        // that is smaller than 'integrals'.
        while (*kappa > 0) {
            int digit = integrals / divisor;
            ASSERT(digit <= 9);
            buffer[*length] = static_cast<char>('0' + digit);
            (*length)++;
            requested_digits--;
            integrals %= divisor;
            (*kappa)--;
            // Note that kappa now equals the exponent of the divisor and that the
            // invariant thus holds again.
            if (requested_digits == 0) break;
            divisor /= 10;
        }

        if (requested_digits == 0) {
            uint64_t rest =
                (static_cast<uint64_t>(integrals) << -one.e()) + fractionals;
            return RoundWeedCounted(buffer, *length, rest,
                static_cast<uint64_t>(divisor) << -one.e(), w_error,
                kappa);
        }

        // The integrals have been generated. We are at the point of the decimal
        // separator. In the following loop we simply multiply the remaining digits by
        // 10 and divide by one. We just need to pay attention to multiply associated
        // data (the 'unit'), too.
        // Note that the multiplication by 10 does not overflow, because w.e >= -60
        // and thus one.e >= -60.
        ASSERT(one.e() >= -60);
        ASSERT(fractionals < one.f());
        ASSERT(UINT64_2PART_C(0xFFFFFFFF, FFFFFFFF) / 10 >= one.f());
        while (requested_digits > 0 && fractionals > w_error) {
            fractionals *= 10;
            w_error *= 10;
            // Integer division by one.
            int digit = static_cast<int>(fractionals >> -one.e());
            ASSERT(digit <= 9);
            buffer[*length] = static_cast<char>('0' + digit);
            (*length)++;
            requested_digits--;
            fractionals &= one.f() - 1;  // Modulo by one.
            (*kappa)--;
        }
        if (requested_digits != 0) return false;
        return RoundWeedCounted(buffer, *length, fractionals, one.f(), w_error,
            kappa);
    }


    // Provides a decimal representation of v.
    // Returns true if it succeeds, otherwise the result cannot be trusted.
    // There will be *length digits inside the buffer (not null-terminated).
    // If the function returns true then
    //        v == (double) (buffer * 10^decimal_exponent).
    // The digits in the buffer are the shortest representation possible: no
    // 0.09999999999999999 instead of 0.1. The shorter representation will even be
    // chosen even if the longer one would be closer to v.
    // The last digit will be closest to the actual v. That is, even if several
    // digits might correctly yield 'v' when read again, the closest will be
    // computed.
    static bool Grisu3(double v,
        FastDtoaMode mode,
        Vector<char> buffer,
        int* length,
        int* decimal_exponent) {
        DiyFp w = Double(v).AsNormalizedDiyFp();
        // boundary_minus and boundary_plus are the boundaries between v and its
        // closest floating-point neighbors. Any number strictly between
        // boundary_minus and boundary_plus will round to v when convert to a double.
        // Grisu3 will never output representations that lie exactly on a boundary.
        DiyFp boundary_minus, boundary_plus;
        if (mode == FAST_DTOA_SHORTEST) {
            Double(v).NormalizedBoundaries(&boundary_minus, &boundary_plus);
        }
        else {
            ASSERT(mode == FAST_DTOA_SHORTEST_SINGLE);
            float single_v = static_cast<float>(v);
            Single(single_v).NormalizedBoundaries(&boundary_minus, &boundary_plus);
        }
        ASSERT(boundary_plus.e() == w.e());
        DiyFp ten_mk;  // Cached power of ten: 10^-k
        int mk;        // -k
        int ten_mk_minimal_binary_exponent =
            kMinimalTargetExponent - (w.e() + DiyFp::kSignificandSize);
        int ten_mk_maximal_binary_exponent =
            kMaximalTargetExponent - (w.e() + DiyFp::kSignificandSize);
        PowersOfTenCache::GetCachedPowerForBinaryExponentRange(
            ten_mk_minimal_binary_exponent,
            ten_mk_maximal_binary_exponent,
            &ten_mk, &mk);
        ASSERT((kMinimalTargetExponent <= w.e() + ten_mk.e() +
            DiyFp::kSignificandSize) &&
            (kMaximalTargetExponent >= w.e() + ten_mk.e() +
            DiyFp::kSignificandSize));
        // Note that ten_mk is only an approximation of 10^-k. A DiyFp only contains a
        // 64 bit significand and ten_mk is thus only precise up to 64 bits.

        // The DiyFp::Times procedure rounds its result, and ten_mk is approximated
        // too. The variable scaled_w (as well as scaled_boundary_minus/plus) are now
        // off by a small amount.
        // In fact: scaled_w - w*10^k < 1ulp (unit in the last place) of scaled_w.
        // In other words: let f = scaled_w.f() and e = scaled_w.e(), then
        //           (f-1) * 2^e < w*10^k < (f+1) * 2^e
        DiyFp scaled_w = DiyFp::Times(w, ten_mk);
        ASSERT(scaled_w.e() ==
            boundary_plus.e() + ten_mk.e() + DiyFp::kSignificandSize);
        // In theory it would be possible to avoid some recomputations by computing
        // the difference between w and boundary_minus/plus (a power of 2) and to
        // compute scaled_boundary_minus/plus by subtracting/adding from
        // scaled_w. However the code becomes much less readable and the speed
        // enhancements are not terriffic.
        DiyFp scaled_boundary_minus = DiyFp::Times(boundary_minus, ten_mk);
        DiyFp scaled_boundary_plus = DiyFp::Times(boundary_plus, ten_mk);

        // DigitGen will generate the digits of scaled_w. Therefore we have
        // v == (double) (scaled_w * 10^-mk).
        // Set decimal_exponent == -mk and pass it to DigitGen. If scaled_w is not an
        // integer than it will be updated. For instance if scaled_w == 1.23 then
        // the buffer will be filled with "123" und the decimal_exponent will be
        // decreased by 2.
        int kappa;
        bool result = DigitGen(scaled_boundary_minus, scaled_w, scaled_boundary_plus,
            buffer, length, &kappa);
        *decimal_exponent = -mk + kappa;
        return result;
    }


    // The "counted" version of grisu3 (see above) only generates requested_digits
    // number of digits. This version does not generate the shortest representation,
    // and with enough requested digits 0.1 will at some point print as 0.9999999...
    // Grisu3 is too imprecise for real halfway cases (1.5 will not work) and
    // therefore the rounding strategy for halfway cases is irrelevant.
    static bool Grisu3Counted(double v,
        int requested_digits,
        Vector<char> buffer,
        int* length,
        int* decimal_exponent) {
        DiyFp w = Double(v).AsNormalizedDiyFp();
        DiyFp ten_mk;  // Cached power of ten: 10^-k
        int mk;        // -k
        int ten_mk_minimal_binary_exponent =
            kMinimalTargetExponent - (w.e() + DiyFp::kSignificandSize);
        int ten_mk_maximal_binary_exponent =
            kMaximalTargetExponent - (w.e() + DiyFp::kSignificandSize);
        PowersOfTenCache::GetCachedPowerForBinaryExponentRange(
            ten_mk_minimal_binary_exponent,
            ten_mk_maximal_binary_exponent,
            &ten_mk, &mk);
        ASSERT((kMinimalTargetExponent <= w.e() + ten_mk.e() +
            DiyFp::kSignificandSize) &&
            (kMaximalTargetExponent >= w.e() + ten_mk.e() +
            DiyFp::kSignificandSize));
        // Note that ten_mk is only an approximation of 10^-k. A DiyFp only contains a
        // 64 bit significand and ten_mk is thus only precise up to 64 bits.

        // The DiyFp::Times procedure rounds its result, and ten_mk is approximated
        // too. The variable scaled_w (as well as scaled_boundary_minus/plus) are now
        // off by a small amount.
        // In fact: scaled_w - w*10^k < 1ulp (unit in the last place) of scaled_w.
        // In other words: let f = scaled_w.f() and e = scaled_w.e(), then
        //           (f-1) * 2^e < w*10^k < (f+1) * 2^e
        DiyFp scaled_w = DiyFp::Times(w, ten_mk);

        // We now have (double) (scaled_w * 10^-mk).
        // DigitGen will generate the first requested_digits digits of scaled_w and
        // return together with a kappa such that scaled_w ~= buffer * 10^kappa. (It
        // will not always be exactly the same since DigitGenCounted only produces a
        // limited number of digits.)
        int kappa;
        bool result = DigitGenCounted(scaled_w, requested_digits,
            buffer, length, &kappa);
        *decimal_exponent = -mk + kappa;
        return result;
    }


    bool FastDtoa(double v,
        FastDtoaMode mode,
        int requested_digits,
        Vector<char> buffer,
        int* length,
        int* decimal_point) {
        ASSERT(v > 0);
        ASSERT(!Double(v).IsSpecial());

        bool result = false;
        int decimal_exponent = 0;
        switch (mode) {
        case FAST_DTOA_SHORTEST:
        case FAST_DTOA_SHORTEST_SINGLE:
            result = Grisu3(v, mode, buffer, length, &decimal_exponent);
            break;
        case FAST_DTOA_PRECISION:
            result = Grisu3Counted(v, requested_digits,
                buffer, length, &decimal_exponent);
            break;
        default:
            UNREACHABLE();
        }
        if (result) {
            *decimal_point = *length + decimal_exponent;
            buffer[*length] = '\0';
        }
        return result;
    }

}  // namespace double_conversion

namespace double_conversion {

    void DiyFp::Multiply(const DiyFp& other) {
        // Simply "emulates" a 128 bit multiplication.
        // However: the resulting number only contains 64 bits. The least
        // significant 64 bits are only used for rounding the most significant 64
        // bits.
        const uint64_t kM32 = 0xFFFFFFFFU;
        uint64_t a = f_ >> 32;
        uint64_t b = f_ & kM32;
        uint64_t c = other.f_ >> 32;
        uint64_t d = other.f_ & kM32;
        uint64_t ac = a * c;
        uint64_t bc = b * c;
        uint64_t ad = a * d;
        uint64_t bd = b * d;
        uint64_t tmp = (bd >> 32) + (ad & kM32) + (bc & kM32);
        // By adding 1U << 31 to tmp we round the final result.
        // Halfway cases will be round up.
        tmp += 1U << 31;
        uint64_t result_f = ac + (ad >> 32) + (bc >> 32) + (tmp >> 32);
        e_ += other.e_ + 64;
        f_ = result_f;
    }

}  // namespace double_conversion

namespace double_conversion {

    struct CachedPower {
        uint64_t significand;
        int16_t binary_exponent;
        int16_t decimal_exponent;
    };

    static const CachedPower kCachedPowers[] = {
            { UINT64_2PART_C(0xfa8fd5a0, 081c0288), -1220, -348 },
            { UINT64_2PART_C(0xbaaee17f, a23ebf76), -1193, -340 },
            { UINT64_2PART_C(0x8b16fb20, 3055ac76), -1166, -332 },
            { UINT64_2PART_C(0xcf42894a, 5dce35ea), -1140, -324 },
            { UINT64_2PART_C(0x9a6bb0aa, 55653b2d), -1113, -316 },
            { UINT64_2PART_C(0xe61acf03, 3d1a45df), -1087, -308 },
            { UINT64_2PART_C(0xab70fe17, c79ac6ca), -1060, -300 },
            { UINT64_2PART_C(0xff77b1fc, bebcdc4f), -1034, -292 },
            { UINT64_2PART_C(0xbe5691ef, 416bd60c), -1007, -284 },
            { UINT64_2PART_C(0x8dd01fad, 907ffc3c), -980, -276 },
            { UINT64_2PART_C(0xd3515c28, 31559a83), -954, -268 },
            { UINT64_2PART_C(0x9d71ac8f, ada6c9b5), -927, -260 },
            { UINT64_2PART_C(0xea9c2277, 23ee8bcb), -901, -252 },
            { UINT64_2PART_C(0xaecc4991, 4078536d), -874, -244 },
            { UINT64_2PART_C(0x823c1279, 5db6ce57), -847, -236 },
            { UINT64_2PART_C(0xc2109436, 4dfb5637), -821, -228 },
            { UINT64_2PART_C(0x9096ea6f, 3848984f), -794, -220 },
            { UINT64_2PART_C(0xd77485cb, 25823ac7), -768, -212 },
            { UINT64_2PART_C(0xa086cfcd, 97bf97f4), -741, -204 },
            { UINT64_2PART_C(0xef340a98, 172aace5), -715, -196 },
            { UINT64_2PART_C(0xb23867fb, 2a35b28e), -688, -188 },
            { UINT64_2PART_C(0x84c8d4df, d2c63f3b), -661, -180 },
            { UINT64_2PART_C(0xc5dd4427, 1ad3cdba), -635, -172 },
            { UINT64_2PART_C(0x936b9fce, bb25c996), -608, -164 },
            { UINT64_2PART_C(0xdbac6c24, 7d62a584), -582, -156 },
            { UINT64_2PART_C(0xa3ab6658, 0d5fdaf6), -555, -148 },
            { UINT64_2PART_C(0xf3e2f893, dec3f126), -529, -140 },
            { UINT64_2PART_C(0xb5b5ada8, aaff80b8), -502, -132 },
            { UINT64_2PART_C(0x87625f05, 6c7c4a8b), -475, -124 },
            { UINT64_2PART_C(0xc9bcff60, 34c13053), -449, -116 },
            { UINT64_2PART_C(0x964e858c, 91ba2655), -422, -108 },
            { UINT64_2PART_C(0xdff97724, 70297ebd), -396, -100 },
            { UINT64_2PART_C(0xa6dfbd9f, b8e5b88f), -369, -92 },
            { UINT64_2PART_C(0xf8a95fcf, 88747d94), -343, -84 },
            { UINT64_2PART_C(0xb9447093, 8fa89bcf), -316, -76 },
            { UINT64_2PART_C(0x8a08f0f8, bf0f156b), -289, -68 },
            { UINT64_2PART_C(0xcdb02555, 653131b6), -263, -60 },
            { UINT64_2PART_C(0x993fe2c6, d07b7fac), -236, -52 },
            { UINT64_2PART_C(0xe45c10c4, 2a2b3b06), -210, -44 },
            { UINT64_2PART_C(0xaa242499, 697392d3), -183, -36 },
            { UINT64_2PART_C(0xfd87b5f2, 8300ca0e), -157, -28 },
            { UINT64_2PART_C(0xbce50864, 92111aeb), -130, -20 },
            { UINT64_2PART_C(0x8cbccc09, 6f5088cc), -103, -12 },
            { UINT64_2PART_C(0xd1b71758, e219652c), -77, -4 },
            { UINT64_2PART_C(0x9c400000, 00000000), -50, 4 },
            { UINT64_2PART_C(0xe8d4a510, 00000000), -24, 12 },
            { UINT64_2PART_C(0xad78ebc5, ac620000), 3, 20 },
            { UINT64_2PART_C(0x813f3978, f8940984), 30, 28 },
            { UINT64_2PART_C(0xc097ce7b, c90715b3), 56, 36 },
            { UINT64_2PART_C(0x8f7e32ce, 7bea5c70), 83, 44 },
            { UINT64_2PART_C(0xd5d238a4, abe98068), 109, 52 },
            { UINT64_2PART_C(0x9f4f2726, 179a2245), 136, 60 },
            { UINT64_2PART_C(0xed63a231, d4c4fb27), 162, 68 },
            { UINT64_2PART_C(0xb0de6538, 8cc8ada8), 189, 76 },
            { UINT64_2PART_C(0x83c7088e, 1aab65db), 216, 84 },
            { UINT64_2PART_C(0xc45d1df9, 42711d9a), 242, 92 },
            { UINT64_2PART_C(0x924d692c, a61be758), 269, 100 },
            { UINT64_2PART_C(0xda01ee64, 1a708dea), 295, 108 },
            { UINT64_2PART_C(0xa26da399, 9aef774a), 322, 116 },
            { UINT64_2PART_C(0xf209787b, b47d6b85), 348, 124 },
            { UINT64_2PART_C(0xb454e4a1, 79dd1877), 375, 132 },
            { UINT64_2PART_C(0x865b8692, 5b9bc5c2), 402, 140 },
            { UINT64_2PART_C(0xc83553c5, c8965d3d), 428, 148 },
            { UINT64_2PART_C(0x952ab45c, fa97a0b3), 455, 156 },
            { UINT64_2PART_C(0xde469fbd, 99a05fe3), 481, 164 },
            { UINT64_2PART_C(0xa59bc234, db398c25), 508, 172 },
            { UINT64_2PART_C(0xf6c69a72, a3989f5c), 534, 180 },
            { UINT64_2PART_C(0xb7dcbf53, 54e9bece), 561, 188 },
            { UINT64_2PART_C(0x88fcf317, f22241e2), 588, 196 },
            { UINT64_2PART_C(0xcc20ce9b, d35c78a5), 614, 204 },
            { UINT64_2PART_C(0x98165af3, 7b2153df), 641, 212 },
            { UINT64_2PART_C(0xe2a0b5dc, 971f303a), 667, 220 },
            { UINT64_2PART_C(0xa8d9d153, 5ce3b396), 694, 228 },
            { UINT64_2PART_C(0xfb9b7cd9, a4a7443c), 720, 236 },
            { UINT64_2PART_C(0xbb764c4c, a7a44410), 747, 244 },
            { UINT64_2PART_C(0x8bab8eef, b6409c1a), 774, 252 },
            { UINT64_2PART_C(0xd01fef10, a657842c), 800, 260 },
            { UINT64_2PART_C(0x9b10a4e5, e9913129), 827, 268 },
            { UINT64_2PART_C(0xe7109bfb, a19c0c9d), 853, 276 },
            { UINT64_2PART_C(0xac2820d9, 623bf429), 880, 284 },
            { UINT64_2PART_C(0x80444b5e, 7aa7cf85), 907, 292 },
            { UINT64_2PART_C(0xbf21e440, 03acdd2d), 933, 300 },
            { UINT64_2PART_C(0x8e679c2f, 5e44ff8f), 960, 308 },
            { UINT64_2PART_C(0xd433179d, 9c8cb841), 986, 316 },
            { UINT64_2PART_C(0x9e19db92, b4e31ba9), 1013, 324 },
            { UINT64_2PART_C(0xeb96bf6e, badf77d9), 1039, 332 },
            { UINT64_2PART_C(0xaf87023b, 9bf0ee6b), 1066, 340 },
    };

#ifndef NDEBUG
    static const int kCachedPowersLength = ARRAY_SIZE(kCachedPowers);
#endif
    static const int kCachedPowersOffset = 348;  // -1 * the first decimal_exponent.
    static const double kD_1_LOG2_10 = 0.30102999566398114;  //  1 / lg(10)
    // Difference between the decimal exponents in the table above.
    const int PowersOfTenCache::kDecimalExponentDistance = 8;
    const int PowersOfTenCache::kMinDecimalExponent = -348;
    const int PowersOfTenCache::kMaxDecimalExponent = 340;

    void PowersOfTenCache::GetCachedPowerForBinaryExponentRange(
        int min_exponent,
        int max_exponent,
        DiyFp* power,
        int* decimal_exponent) {
        int kQ = DiyFp::kSignificandSize;
        double k = ceil((min_exponent + kQ - 1) * kD_1_LOG2_10);
        int foo = kCachedPowersOffset;
        int index =
            (foo + static_cast<int>(k)-1) / kDecimalExponentDistance + 1;
        ASSERT(0 <= index && index < kCachedPowersLength);
        CachedPower cached_power = kCachedPowers[index];
        ASSERT(min_exponent <= cached_power.binary_exponent);
        (void)max_exponent;  // Mark variable as used.
        ASSERT(cached_power.binary_exponent <= max_exponent);
        *decimal_exponent = cached_power.decimal_exponent;
        *power = DiyFp(cached_power.significand, cached_power.binary_exponent);
    }


    void PowersOfTenCache::GetCachedPowerForDecimalExponent(int requested_exponent,
        DiyFp* power,
        int* found_exponent) {
        ASSERT(kMinDecimalExponent <= requested_exponent);
        ASSERT(requested_exponent < kMaxDecimalExponent + kDecimalExponentDistance);
        int index =
            (requested_exponent + kCachedPowersOffset) / kDecimalExponentDistance;
        CachedPower cached_power = kCachedPowers[index];
        *power = DiyFp(cached_power.significand, cached_power.binary_exponent);
        *found_exponent = cached_power.decimal_exponent;
        ASSERT(*found_exponent <= requested_exponent);
        ASSERT(requested_exponent < *found_exponent + kDecimalExponentDistance);
    }

}  // namespace double_conversion

namespace double_conversion {

    Bignum::Bignum()
        : bigits_(bigits_buffer_, kBigitCapacity), used_digits_(0), exponent_(0) {
        for (int i = 0; i < kBigitCapacity; ++i) {
            bigits_[i] = 0;
        }
    }


    template<typename S>
    static int BitSize(S value) {
        (void)value;  // Mark variable as used.
        return 8 * sizeof(value);
    }

    // Guaranteed to lie in one Bigit.
    void Bignum::AssignUInt16(uint16_t value) {
        ASSERT(kBigitSize >= BitSize(value));
        Zero();
        if (value == 0) return;

        EnsureCapacity(1);
        bigits_[0] = value;
        used_digits_ = 1;
    }


    void Bignum::AssignUInt64(uint64_t value) {
        const int kUInt64Size = 64;

        Zero();
        if (value == 0) return;

        int needed_bigits = kUInt64Size / kBigitSize + 1;
        EnsureCapacity(needed_bigits);
        for (int i = 0; i < needed_bigits; ++i) {
            bigits_[i] = value & kBigitMask;
            value = value >> kBigitSize;
        }
        used_digits_ = needed_bigits;
        Clamp();
    }


    void Bignum::AssignBignum(const Bignum& other) {
        exponent_ = other.exponent_;
        for (int i = 0; i < other.used_digits_; ++i) {
            bigits_[i] = other.bigits_[i];
        }
        // Clear the excess digits (if there were any).
        for (int i = other.used_digits_; i < used_digits_; ++i) {
            bigits_[i] = 0;
        }
        used_digits_ = other.used_digits_;
    }


    static uint64_t ReadUInt64(Vector<const char> buffer,
        int from,
        int digits_to_read) {
        uint64_t result = 0;
        for (int i = from; i < from + digits_to_read; ++i) {
            int digit = buffer[i] - '0';
            ASSERT(0 <= digit && digit <= 9);
            result = result * 10 + digit;
        }
        return result;
    }


    void Bignum::AssignDecimalString(Vector<const char> value) {
        // 2^64 = 18446744073709551616 > 10^19
        //const int kMaxUint64DecimalDigits = 19; // @FDVALVE warning C4459: declaration of 'kMaxUint64DecimalDigits' hides global declaration
        Zero();
        int length = value.length();
        int pos = 0;
        // Let's just say that each digit needs 4 bits.
        while (length >= kMaxUint64DecimalDigits) {
            uint64_t digits = ReadUInt64(value, pos, kMaxUint64DecimalDigits);
            pos += kMaxUint64DecimalDigits;
            length -= kMaxUint64DecimalDigits;
            MultiplyByPowerOfTen(kMaxUint64DecimalDigits);
            AddUInt64(digits);
        }
        uint64_t digits = ReadUInt64(value, pos, length);
        MultiplyByPowerOfTen(length);
        AddUInt64(digits);
        Clamp();
    }


    static int HexCharValue(char c) {
        if ('0' <= c && c <= '9') return c - '0';
        if ('a' <= c && c <= 'f') return 10 + c - 'a';
        ASSERT('A' <= c && c <= 'F');
        return 10 + c - 'A';
    }


    void Bignum::AssignHexString(Vector<const char> value) {
        Zero();
        int length = value.length();

        int needed_bigits = length * 4 / kBigitSize + 1;
        EnsureCapacity(needed_bigits);
        int string_index = length - 1;
        for (int i = 0; i < needed_bigits - 1; ++i) {
            // These bigits are guaranteed to be "full".
            Chunk current_bigit = 0;
            for (int j = 0; j < kBigitSize / 4; j++) {
                current_bigit += HexCharValue(value[string_index--]) << (j * 4);
            }
            bigits_[i] = current_bigit;
        }
        used_digits_ = needed_bigits - 1;

        Chunk most_significant_bigit = 0;  // Could be = 0;
        for (int j = 0; j <= string_index; ++j) {
            most_significant_bigit <<= 4;
            most_significant_bigit += HexCharValue(value[j]);
        }
        if (most_significant_bigit != 0) {
            bigits_[used_digits_] = most_significant_bigit;
            used_digits_++;
        }
        Clamp();
    }


    void Bignum::AddUInt64(uint64_t operand) {
        if (operand == 0) return;
        Bignum other;
        other.AssignUInt64(operand);
        AddBignum(other);
    }


    void Bignum::AddBignum(const Bignum& other) {
        ASSERT(IsClamped());
        ASSERT(other.IsClamped());

        // If this has a greater exponent than other append zero-bigits to this.
        // After this call exponent_ <= other.exponent_.
        Align(other);

        // There are two possibilities:
        //   aaaaaaaaaaa 0000  (where the 0s represent a's exponent)
        //     bbbbb 00000000
        //   ----------------
        //   ccccccccccc 0000
        // or
        //    aaaaaaaaaa 0000
        //  bbbbbbbbb 0000000
        //  -----------------
        //  cccccccccccc 0000
        // In both cases we might need a carry bigit.

        EnsureCapacity(1 + Max(BigitLength(), other.BigitLength()) - exponent_);
        Chunk carry = 0;
        int bigit_pos = other.exponent_ - exponent_;
        ASSERT(bigit_pos >= 0);
        for (int i = 0; i < other.used_digits_; ++i) {
            Chunk sum = bigits_[bigit_pos] + other.bigits_[i] + carry;
            bigits_[bigit_pos] = sum & kBigitMask;
            carry = sum >> kBigitSize;
            bigit_pos++;
        }

        while (carry != 0) {
            Chunk sum = bigits_[bigit_pos] + carry;
            bigits_[bigit_pos] = sum & kBigitMask;
            carry = sum >> kBigitSize;
            bigit_pos++;
        }
        used_digits_ = Max(bigit_pos, used_digits_);
        ASSERT(IsClamped());
    }


    void Bignum::SubtractBignum(const Bignum& other) {
        ASSERT(IsClamped());
        ASSERT(other.IsClamped());
        // We require this to be bigger than other.
        ASSERT(LessEqual(other, *this));

        Align(other);

        int offset = other.exponent_ - exponent_;
        Chunk borrow = 0;
        int i;
        for (i = 0; i < other.used_digits_; ++i) {
            ASSERT((borrow == 0) || (borrow == 1));
            Chunk difference = bigits_[i + offset] - other.bigits_[i] - borrow;
            bigits_[i + offset] = difference & kBigitMask;
            borrow = difference >> (kChunkSize - 1);
        }
        while (borrow != 0) {
            Chunk difference = bigits_[i + offset] - borrow;
            bigits_[i + offset] = difference & kBigitMask;
            borrow = difference >> (kChunkSize - 1);
            ++i;
        }
        Clamp();
    }


    void Bignum::ShiftLeft(int shift_amount) {
        if (used_digits_ == 0) return;
        exponent_ += shift_amount / kBigitSize;
        int local_shift = shift_amount % kBigitSize;
        EnsureCapacity(used_digits_ + 1);
        BigitsShiftLeft(local_shift);
    }


    void Bignum::MultiplyByUInt32(uint32_t factor) {
        if (factor == 1) return;
        if (factor == 0) {
            Zero();
            return;
        }
        if (used_digits_ == 0) return;

        // The product of a bigit with the factor is of size kBigitSize + 32.
        // Assert that this number + 1 (for the carry) fits into double chunk.
        ASSERT(kDoubleChunkSize >= kBigitSize + 32 + 1);
        DoubleChunk carry = 0;
        for (int i = 0; i < used_digits_; ++i) {
            DoubleChunk product = static_cast<DoubleChunk>(factor)* bigits_[i] + carry;
            bigits_[i] = static_cast<Chunk>(product & kBigitMask);
            carry = (product >> kBigitSize);
        }
        while (carry != 0) {
            EnsureCapacity(used_digits_ + 1);
            bigits_[used_digits_] = carry & kBigitMask;
            used_digits_++;
            carry >>= kBigitSize;
        }
    }


    void Bignum::MultiplyByUInt64(uint64_t factor) {
        if (factor == 1) return;
        if (factor == 0) {
            Zero();
            return;
        }
        ASSERT(kBigitSize < 32);
        uint64_t carry = 0;
        uint64_t low = factor & 0xFFFFFFFF;
        uint64_t high = factor >> 32;
        for (int i = 0; i < used_digits_; ++i) {
            uint64_t product_low = low * bigits_[i];
            uint64_t product_high = high * bigits_[i];
            uint64_t tmp = (carry & kBigitMask) + product_low;
            bigits_[i] = tmp & kBigitMask;
            carry = (carry >> kBigitSize) + (tmp >> kBigitSize) +
                (product_high << (32 - kBigitSize));
        }
        while (carry != 0) {
            EnsureCapacity(used_digits_ + 1);
            bigits_[used_digits_] = carry & kBigitMask;
            used_digits_++;
            carry >>= kBigitSize;
        }
    }


    void Bignum::MultiplyByPowerOfTen(int exponent) {
        const uint64_t kFive27 = UINT64_2PART_C(0x6765c793, fa10079d);
        const uint16_t kFive1 = 5;
        const uint16_t kFive2 = kFive1 * 5;
        const uint16_t kFive3 = kFive2 * 5;
        const uint16_t kFive4 = kFive3 * 5;
        const uint16_t kFive5 = kFive4 * 5;
        const uint16_t kFive6 = kFive5 * 5;
        const uint32_t kFive7 = kFive6 * 5;
        const uint32_t kFive8 = kFive7 * 5;
        const uint32_t kFive9 = kFive8 * 5;
        const uint32_t kFive10 = kFive9 * 5;
        const uint32_t kFive11 = kFive10 * 5;
        const uint32_t kFive12 = kFive11 * 5;
        const uint32_t kFive13 = kFive12 * 5;
        const uint32_t kFive1_to_12[] =
        { kFive1, kFive2, kFive3, kFive4, kFive5, kFive6,
        kFive7, kFive8, kFive9, kFive10, kFive11, kFive12 };

        ASSERT(exponent >= 0);
        if (exponent == 0) return;
        if (used_digits_ == 0) return;

        // We shift by exponent at the end just before returning.
        int remaining_exponent = exponent;
        while (remaining_exponent >= 27) {
            MultiplyByUInt64(kFive27);
            remaining_exponent -= 27;
        }
        while (remaining_exponent >= 13) {
            MultiplyByUInt32(kFive13);
            remaining_exponent -= 13;
        }
        if (remaining_exponent > 0) {
            MultiplyByUInt32(kFive1_to_12[remaining_exponent - 1]);
        }
        ShiftLeft(exponent);
    }


    void Bignum::Square() {
        ASSERT(IsClamped());
        int product_length = 2 * used_digits_;
        EnsureCapacity(product_length);

        // Comba multiplication: compute each column separately.
        // Example: r = a2a1a0 * b2b1b0.
        //    r =  1    * a0b0 +
        //        10    * (a1b0 + a0b1) +
        //        100   * (a2b0 + a1b1 + a0b2) +
        //        1000  * (a2b1 + a1b2) +
        //        10000 * a2b2
        //
        // In the worst case we have to accumulate nb-digits products of digit*digit.
        //
        // Assert that the additional number of bits in a DoubleChunk are enough to
        // sum up used_digits of Bigit*Bigit.
        if ((1 << (2 * (kChunkSize - kBigitSize))) <= used_digits_) {
            UNIMPLEMENTED();
        }
        DoubleChunk accumulator = 0;
        // First shift the digits so we don't overwrite them.
        int copy_offset = used_digits_;
        for (int i = 0; i < used_digits_; ++i) {
            bigits_[copy_offset + i] = bigits_[i];
        }
        // We have two loops to avoid some 'if's in the loop.
        for (int i = 0; i < used_digits_; ++i) {
            // Process temporary digit i with power i.
            // The sum of the two indices must be equal to i.
            int bigit_index1 = i;
            int bigit_index2 = 0;
            // Sum all of the sub-products.
            while (bigit_index1 >= 0) {
                Chunk chunk1 = bigits_[copy_offset + bigit_index1];
                Chunk chunk2 = bigits_[copy_offset + bigit_index2];
                accumulator += static_cast<DoubleChunk>(chunk1)* chunk2;
                bigit_index1--;
                bigit_index2++;
            }
            bigits_[i] = static_cast<Chunk>(accumulator)& kBigitMask;
            accumulator >>= kBigitSize;
        }
        for (int i = used_digits_; i < product_length; ++i) {
            int bigit_index1 = used_digits_ - 1;
            int bigit_index2 = i - bigit_index1;
            // Invariant: sum of both indices is again equal to i.
            // Inner loop runs 0 times on last iteration, emptying accumulator.
            while (bigit_index2 < used_digits_) {
                Chunk chunk1 = bigits_[copy_offset + bigit_index1];
                Chunk chunk2 = bigits_[copy_offset + bigit_index2];
                accumulator += static_cast<DoubleChunk>(chunk1)* chunk2;
                bigit_index1--;
                bigit_index2++;
            }
            // The overwritten bigits_[i] will never be read in further loop iterations,
            // because bigit_index1 and bigit_index2 are always greater
            // than i - used_digits_.
            bigits_[i] = static_cast<Chunk>(accumulator)& kBigitMask;
            accumulator >>= kBigitSize;
        }
        // Since the result was guaranteed to lie inside the number the
        // accumulator must be 0 now.
        ASSERT(accumulator == 0);

        // Don't forget to update the used_digits and the exponent.
        used_digits_ = product_length;
        exponent_ *= 2;
        Clamp();
    }


    void Bignum::AssignPowerUInt16(uint16_t base, int power_exponent) {
        ASSERT(base != 0);
        ASSERT(power_exponent >= 0);
        if (power_exponent == 0) {
            AssignUInt16(1);
            return;
        }
        Zero();
        int shifts = 0;
        // We expect base to be in range 2-32, and most often to be 10.
        // It does not make much sense to implement different algorithms for counting
        // the bits.
        while ((base & 1) == 0) {
            base >>= 1;
            shifts++;
        }
        int bit_size = 0;
        int tmp_base = base;
        while (tmp_base != 0) {
            tmp_base >>= 1;
            bit_size++;
        }
        int final_size = bit_size * power_exponent;
        // 1 extra bigit for the shifting, and one for rounded final_size.
        EnsureCapacity(final_size / kBigitSize + 2);

        // Left to Right exponentiation.
        int mask = 1;
        while (power_exponent >= mask) mask <<= 1;

        // The mask is now pointing to the bit above the most significant 1-bit of
        // power_exponent.
        // Get rid of first 1-bit;
        mask >>= 2;
        uint64_t this_value = base;

        bool delayed_multipliciation = false;
        const uint64_t max_32bits = 0xFFFFFFFF;
        while (mask != 0 && this_value <= max_32bits) {
            this_value = this_value * this_value;
            // Verify that there is enough space in this_value to perform the
            // multiplication.  The first bit_size bits must be 0.
            if ((power_exponent & mask) != 0) {
                uint64_t base_bits_mask =
                    ~((static_cast<uint64_t>(1) << (64 - bit_size)) - 1);
                bool high_bits_zero = (this_value & base_bits_mask) == 0;
                if (high_bits_zero) {
                    this_value *= base;
                }
                else {
                    delayed_multipliciation = true;
                }
            }
            mask >>= 1;
        }
        AssignUInt64(this_value);
        if (delayed_multipliciation) {
            MultiplyByUInt32(base);
        }

        // Now do the same thing as a bignum.
        while (mask != 0) {
            Square();
            if ((power_exponent & mask) != 0) {
                MultiplyByUInt32(base);
            }
            mask >>= 1;
        }

        // And finally add the saved shifts.
        ShiftLeft(shifts * power_exponent);
    }


    // Precondition: this/other < 16bit.
    uint16_t Bignum::DivideModuloIntBignum(const Bignum& other) {
        ASSERT(IsClamped());
        ASSERT(other.IsClamped());
        ASSERT(other.used_digits_ > 0);

        // Easy case: if we have less digits than the divisor than the result is 0.
        // Note: this handles the case where this == 0, too.
        if (BigitLength() < other.BigitLength()) {
            return 0;
        }

        Align(other);

        uint16_t result = 0;

        // Start by removing multiples of 'other' until both numbers have the same
        // number of digits.
        while (BigitLength() > other.BigitLength()) {
            // This naive approach is extremely inefficient if `this` divided by other
            // is big. This function is implemented for doubleToString where
            // the result should be small (less than 10).
            ASSERT(other.bigits_[other.used_digits_ - 1] >= ((1 << kBigitSize) / 16));
            ASSERT(bigits_[used_digits_ - 1] < 0x10000);
            // Remove the multiples of the first digit.
            // Example this = 23 and other equals 9. -> Remove 2 multiples.
            result += static_cast<uint16_t>(bigits_[used_digits_ - 1]);
            SubtractTimes(other, bigits_[used_digits_ - 1]);
        }

        ASSERT(BigitLength() == other.BigitLength());

        // Both bignums are at the same length now.
        // Since other has more than 0 digits we know that the access to
        // bigits_[used_digits_ - 1] is safe.
        Chunk this_bigit = bigits_[used_digits_ - 1];
        Chunk other_bigit = other.bigits_[other.used_digits_ - 1];

        if (other.used_digits_ == 1) {
            // Shortcut for easy (and common) case.
            int quotient = this_bigit / other_bigit;
            bigits_[used_digits_ - 1] = this_bigit - other_bigit * quotient;
            ASSERT(quotient < 0x10000);
            result += static_cast<uint16_t>(quotient);
            Clamp();
            return result;
        }

        int division_estimate = this_bigit / (other_bigit + 1);
        ASSERT(division_estimate < 0x10000);
        result += static_cast<uint16_t>(division_estimate);
        SubtractTimes(other, division_estimate);

        if (other_bigit * (division_estimate + 1) > this_bigit) {
            // No need to even try to subtract. Even if other's remaining digits were 0
            // another subtraction would be too much.
            return result;
        }

        while (LessEqual(other, *this)) {
            SubtractBignum(other);
            result++;
        }
        return result;
    }


    template<typename S>
    static int SizeInHexChars(S number) {
        ASSERT(number > 0);
        int result = 0;
        while (number != 0) {
            number >>= 4;
            result++;
        }
        return result;
    }


    static char HexCharOfValue(int value) {
        ASSERT(0 <= value && value <= 16);
        if (value < 10) return static_cast<char>(value + '0');
        return static_cast<char>(value - 10 + 'A');
    }


    bool Bignum::ToHexString(char* buffer, int buffer_size) const {
        ASSERT(IsClamped());
        // Each bigit must be printable as separate hex-character.
        ASSERT(kBigitSize % 4 == 0);
        const int kHexCharsPerBigit = kBigitSize / 4;

        if (used_digits_ == 0) {
            if (buffer_size < 2) return false;
            buffer[0] = '0';
            buffer[1] = '\0';
            return true;
        }
        // We add 1 for the terminating '\0' character.
        int needed_chars = (BigitLength() - 1) * kHexCharsPerBigit +
            SizeInHexChars(bigits_[used_digits_ - 1]) + 1;
        if (needed_chars > buffer_size) return false;
        int string_index = needed_chars - 1;
        buffer[string_index--] = '\0';
        for (int i = 0; i < exponent_; ++i) {
            for (int j = 0; j < kHexCharsPerBigit; ++j) {
                buffer[string_index--] = '0';
            }
        }
        for (int i = 0; i < used_digits_ - 1; ++i) {
            Chunk current_bigit = bigits_[i];
            for (int j = 0; j < kHexCharsPerBigit; ++j) {
                buffer[string_index--] = HexCharOfValue(current_bigit & 0xF);
                current_bigit >>= 4;
            }
        }
        // And finally the last bigit.
        Chunk most_significant_bigit = bigits_[used_digits_ - 1];
        while (most_significant_bigit != 0) {
            buffer[string_index--] = HexCharOfValue(most_significant_bigit & 0xF);
            most_significant_bigit >>= 4;
        }
        return true;
    }


    Bignum::Chunk Bignum::BigitAt(int index) const {
        if (index >= BigitLength()) return 0;
        if (index < exponent_) return 0;
        return bigits_[index - exponent_];
    }


    int Bignum::Compare(const Bignum& a, const Bignum& b) {
        ASSERT(a.IsClamped());
        ASSERT(b.IsClamped());
        int bigit_length_a = a.BigitLength();
        int bigit_length_b = b.BigitLength();
        if (bigit_length_a < bigit_length_b) return -1;
        if (bigit_length_a > bigit_length_b) return +1;
        for (int i = bigit_length_a - 1; i >= Min(a.exponent_, b.exponent_); --i) {
            Chunk bigit_a = a.BigitAt(i);
            Chunk bigit_b = b.BigitAt(i);
            if (bigit_a < bigit_b) return -1;
            if (bigit_a > bigit_b) return +1;
            // Otherwise they are equal up to this digit. Try the next digit.
        }
        return 0;
    }


    int Bignum::PlusCompare(const Bignum& a, const Bignum& b, const Bignum& c) {
        ASSERT(a.IsClamped());
        ASSERT(b.IsClamped());
        ASSERT(c.IsClamped());
        if (a.BigitLength() < b.BigitLength()) {
            return PlusCompare(b, a, c);
        }
        if (a.BigitLength() + 1 < c.BigitLength()) return -1;
        if (a.BigitLength() > c.BigitLength()) return +1;
        // The exponent encodes 0-bigits. So if there are more 0-digits in 'a' than
        // 'b' has digits, then the bigit-length of 'a'+'b' must be equal to the one
        // of 'a'.
        if (a.exponent_ >= b.BigitLength() && a.BigitLength() < c.BigitLength()) {
            return -1;
        }

        Chunk borrow = 0;
        // Starting at min_exponent all digits are == 0. So no need to compare them.
        int min_exponent = Min(Min(a.exponent_, b.exponent_), c.exponent_);
        for (int i = c.BigitLength() - 1; i >= min_exponent; --i) {
            Chunk chunk_a = a.BigitAt(i);
            Chunk chunk_b = b.BigitAt(i);
            Chunk chunk_c = c.BigitAt(i);
            Chunk sum = chunk_a + chunk_b;
            if (sum > chunk_c + borrow) {
                return +1;
            }
            else {
                borrow = chunk_c + borrow - sum;
                if (borrow > 1) return -1;
                borrow <<= kBigitSize;
            }
        }
        if (borrow == 0) return 0;
        return -1;
    }


    void Bignum::Clamp() {
        while (used_digits_ > 0 && bigits_[used_digits_ - 1] == 0) {
            used_digits_--;
        }
        if (used_digits_ == 0) {
            // Zero.
            exponent_ = 0;
        }
    }


    bool Bignum::IsClamped() const {
        return used_digits_ == 0 || bigits_[used_digits_ - 1] != 0;
    }


    void Bignum::Zero() {
        for (int i = 0; i < used_digits_; ++i) {
            bigits_[i] = 0;
        }
        used_digits_ = 0;
        exponent_ = 0;
    }


    void Bignum::Align(const Bignum& other) {
        if (exponent_ > other.exponent_) {
            // If "X" represents a "hidden" digit (by the exponent) then we are in the
            // following case (a == this, b == other):
            // a:  aaaaaaXXXX   or a:   aaaaaXXX
            // b:     bbbbbbX      b: bbbbbbbbXX
            // We replace some of the hidden digits (X) of a with 0 digits.
            // a:  aaaaaa000X   or a:   aaaaa0XX
            int zero_digits = exponent_ - other.exponent_;
            EnsureCapacity(used_digits_ + zero_digits);
            for (int i = used_digits_ - 1; i >= 0; --i) {
                bigits_[i + zero_digits] = bigits_[i];
            }
            for (int i = 0; i < zero_digits; ++i) {
                bigits_[i] = 0;
            }
            used_digits_ += zero_digits;
            exponent_ -= zero_digits;
            ASSERT(used_digits_ >= 0);
            ASSERT(exponent_ >= 0);
        }
    }


    void Bignum::BigitsShiftLeft(int shift_amount) {
        ASSERT(shift_amount < kBigitSize);
        ASSERT(shift_amount >= 0);
        Chunk carry = 0;
        for (int i = 0; i < used_digits_; ++i) {
            Chunk new_carry = bigits_[i] >> (kBigitSize - shift_amount);
            bigits_[i] = ((bigits_[i] << shift_amount) + carry) & kBigitMask;
            carry = new_carry;
        }
        if (carry != 0) {
            bigits_[used_digits_] = carry;
            used_digits_++;
        }
    }


    void Bignum::SubtractTimes(const Bignum& other, int factor) {
        ASSERT(exponent_ <= other.exponent_);
        if (factor < 3) {
            for (int i = 0; i < factor; ++i) {
                SubtractBignum(other);
            }
            return;
        }
        Chunk borrow = 0;
        int exponent_diff = other.exponent_ - exponent_;
        for (int i = 0; i < other.used_digits_; ++i) {
            DoubleChunk product = static_cast<DoubleChunk>(factor)* other.bigits_[i];
            DoubleChunk remove = borrow + product;
            Chunk difference = bigits_[i + exponent_diff] - (remove & kBigitMask);
            bigits_[i + exponent_diff] = difference & kBigitMask;
            borrow = static_cast<Chunk>((difference >> (kChunkSize - 1)) +
                (remove >> kBigitSize));
        }
        for (int i = other.used_digits_ + exponent_diff; i < used_digits_; ++i) {
            if (borrow == 0) return;
            Chunk difference = bigits_[i] - borrow;
            bigits_[i] = difference & kBigitMask;
            borrow = difference >> (kChunkSize - 1);
        }
        Clamp();
    }


}  // namespace double_conversion

namespace double_conversion {

    static int NormalizedExponent(uint64_t significand, int exponent) {
        ASSERT(significand != 0);
        while ((significand & Double::kHiddenBit) == 0) {
            significand = significand << 1;
            exponent = exponent - 1;
        }
        return exponent;
    }


    // Forward declarations:
    // Returns an estimation of k such that 10^(k-1) <= v < 10^k.
    static int EstimatePower(int exponent);
    // Computes v / 10^estimated_power exactly, as a ratio of two bignums, numerator
    // and denominator.
    static void InitialScaledStartValues(uint64_t significand,
        int exponent,
        bool lower_boundary_is_closer,
        int estimated_power,
        bool need_boundary_deltas,
        Bignum* numerator,
        Bignum* denominator,
        Bignum* delta_minus,
        Bignum* delta_plus);
    // Multiplies numerator/denominator so that its values lies in the range 1-10.
    // Returns decimal_point s.t.
    //  v = numerator'/denominator' * 10^(decimal_point-1)
    //     where numerator' and denominator' are the values of numerator and
    //     denominator after the call to this function.
    static void FixupMultiply10(int estimated_power, bool is_even,
        int* decimal_point,
        Bignum* numerator, Bignum* denominator,
        Bignum* delta_minus, Bignum* delta_plus);
    // Generates digits from the left to the right and stops when the generated
    // digits yield the shortest decimal representation of v.
    static void GenerateShortestDigits(Bignum* numerator, Bignum* denominator,
        Bignum* delta_minus, Bignum* delta_plus,
        bool is_even,
        Vector<char> buffer, int* length);
    // Generates 'requested_digits' after the decimal point.
    static void BignumToFixed(int requested_digits, int* decimal_point,
        Bignum* numerator, Bignum* denominator,
        Vector<char>(buffer), int* length);
    // Generates 'count' digits of numerator/denominator.
    // Once 'count' digits have been produced rounds the result depending on the
    // remainder (remainders of exactly .5 round upwards). Might update the
    // decimal_point when rounding up (for example for 0.9999).
    static void GenerateCountedDigits(int count, int* decimal_point,
        Bignum* numerator, Bignum* denominator,
        Vector<char>(buffer), int* length);


    void BignumDtoa(double v, BignumDtoaMode mode, int requested_digits,
        Vector<char> buffer, int* length, int* decimal_point) {
        ASSERT(v > 0);
        ASSERT(!Double(v).IsSpecial());
        uint64_t significand;
        int exponent;
        bool lower_boundary_is_closer;
        if (mode == BIGNUM_DTOA_SHORTEST_SINGLE) {
            float f = static_cast<float>(v);
            ASSERT(f == v);
            significand = Single(f).Significand();
            exponent = Single(f).Exponent();
            lower_boundary_is_closer = Single(f).LowerBoundaryIsCloser();
        }
        else {
            significand = Double(v).Significand();
            exponent = Double(v).Exponent();
            lower_boundary_is_closer = Double(v).LowerBoundaryIsCloser();
        }
        bool need_boundary_deltas =
            (mode == BIGNUM_DTOA_SHORTEST || mode == BIGNUM_DTOA_SHORTEST_SINGLE);

        bool is_even = (significand & 1) == 0;
        int normalized_exponent = NormalizedExponent(significand, exponent);
        // estimated_power might be too low by 1.
        int estimated_power = EstimatePower(normalized_exponent);

        // Shortcut for Fixed.
        // The requested digits correspond to the digits after the point. If the
        // number is much too small, then there is no need in trying to get any
        // digits.
        if (mode == BIGNUM_DTOA_FIXED && -estimated_power - 1 > requested_digits) {
            buffer[0] = '\0';
            *length = 0;
            // Set decimal-point to -requested_digits. This is what Gay does.
            // Note that it should not have any effect anyways since the string is
            // empty.
            *decimal_point = -requested_digits;
            return;
        }

        Bignum numerator;
        Bignum denominator;
        Bignum delta_minus;
        Bignum delta_plus;
        // Make sure the bignum can grow large enough. The smallest double equals
        // 4e-324. In this case the denominator needs fewer than 324*4 binary digits.
        // The maximum double is 1.7976931348623157e308 which needs fewer than
        // 308*4 binary digits.
        ASSERT(Bignum::kMaxSignificantBits >= 324 * 4);
        InitialScaledStartValues(significand, exponent, lower_boundary_is_closer,
            estimated_power, need_boundary_deltas,
            &numerator, &denominator,
            &delta_minus, &delta_plus);
        // We now have v = (numerator / denominator) * 10^estimated_power.
        FixupMultiply10(estimated_power, is_even, decimal_point,
            &numerator, &denominator,
            &delta_minus, &delta_plus);
        // We now have v = (numerator / denominator) * 10^(decimal_point-1), and
        //  1 <= (numerator + delta_plus) / denominator < 10
        switch (mode) {
        case BIGNUM_DTOA_SHORTEST:
        case BIGNUM_DTOA_SHORTEST_SINGLE:
            GenerateShortestDigits(&numerator, &denominator,
                &delta_minus, &delta_plus,
                is_even, buffer, length);
            break;
        case BIGNUM_DTOA_FIXED:
            BignumToFixed(requested_digits, decimal_point,
                &numerator, &denominator,
                buffer, length);
            break;
        case BIGNUM_DTOA_PRECISION:
            GenerateCountedDigits(requested_digits, decimal_point,
                &numerator, &denominator,
                buffer, length);
            break;
        default:
            UNREACHABLE();
        }
        buffer[*length] = '\0';
    }


    // The procedure starts generating digits from the left to the right and stops
    // when the generated digits yield the shortest decimal representation of v. A
    // decimal representation of v is a number lying closer to v than to any other
    // double, so it converts to v when read.
    //
    // This is true if d, the decimal representation, is between m- and m+, the
    // upper and lower boundaries. d must be strictly between them if !is_even.
    //           m- := (numerator - delta_minus) / denominator
    //           m+ := (numerator + delta_plus) / denominator
    //
    // Precondition: 0 <= (numerator+delta_plus) / denominator < 10.
    //   If 1 <= (numerator+delta_plus) / denominator < 10 then no leading 0 digit
    //   will be produced. This should be the standard precondition.
    static void GenerateShortestDigits(Bignum* numerator, Bignum* denominator,
        Bignum* delta_minus, Bignum* delta_plus,
        bool is_even,
        Vector<char> buffer, int* length) {
        // Small optimization: if delta_minus and delta_plus are the same just reuse
        // one of the two bignums.
        if (Bignum::Equal(*delta_minus, *delta_plus)) {
            delta_plus = delta_minus;
        }
        *length = 0;
        for (;;) {
            uint16_t digit;
            digit = numerator->DivideModuloIntBignum(*denominator);
            ASSERT(digit <= 9);  // digit is a uint16_t and therefore always positive.
            // digit = numerator / denominator (integer division).
            // numerator = numerator % denominator.
            buffer[(*length)++] = static_cast<char>(digit + '0');

            // Can we stop already?
            // If the remainder of the division is less than the distance to the lower
            // boundary we can stop. In this case we simply round down (discarding the
            // remainder).
            // Similarly we test if we can round up (using the upper boundary).
            bool in_delta_room_minus;
            bool in_delta_room_plus;
            if (is_even) {
                in_delta_room_minus = Bignum::LessEqual(*numerator, *delta_minus);
            }
            else {
                in_delta_room_minus = Bignum::Less(*numerator, *delta_minus);
            }
            if (is_even) {
                in_delta_room_plus =
                    Bignum::PlusCompare(*numerator, *delta_plus, *denominator) >= 0;
            }
            else {
                in_delta_room_plus =
                    Bignum::PlusCompare(*numerator, *delta_plus, *denominator) > 0;
            }
            if (!in_delta_room_minus && !in_delta_room_plus) {
                // Prepare for next iteration.
                numerator->Times10();
                delta_minus->Times10();
                // We optimized delta_plus to be equal to delta_minus (if they share the
                // same value). So don't multiply delta_plus if they point to the same
                // object.
                if (delta_minus != delta_plus) {
                    delta_plus->Times10();
                }
            }
            else if (in_delta_room_minus && in_delta_room_plus) {
                // Let's see if 2*numerator < denominator.
                // If yes, then the next digit would be < 5 and we can round down.
                int compare = Bignum::PlusCompare(*numerator, *numerator, *denominator);
                if (compare < 0) {
                    // Remaining digits are less than .5. -> Round down (== do nothing).
                }
                else if (compare > 0) {
                    // Remaining digits are more than .5 of denominator. -> Round up.
                    // Note that the last digit could not be a '9' as otherwise the whole
                    // loop would have stopped earlier.
                    // We still have an assert here in case the preconditions were not
                    // satisfied.
                    ASSERT(buffer[(*length) - 1] != '9');
                    buffer[(*length) - 1]++;
                }
                else {
                    // Halfway case.
                    // TODO(floitsch): need a way to solve half-way cases.
                    //   For now let's round towards even (since this is what Gay seems to
                    //   do).

                    if ((buffer[(*length) - 1] - '0') % 2 == 0) {
                        // Round down => Do nothing.
                    }
                    else {
                        ASSERT(buffer[(*length) - 1] != '9');
                        buffer[(*length) - 1]++;
                    }
                }
                return;
            }
            else if (in_delta_room_minus) {
                // Round down (== do nothing).
                return;
            }
            else {  // in_delta_room_plus
                // Round up.
                // Note again that the last digit could not be '9' since this would have
                // stopped the loop earlier.
                // We still have an ASSERT here, in case the preconditions were not
                // satisfied.
                ASSERT(buffer[(*length) - 1] != '9');
                buffer[(*length) - 1]++;
                return;
            }
        }
    }


    // Let v = numerator / denominator < 10.
    // Then we generate 'count' digits of d = x.xxxxx... (without the decimal point)
    // from left to right. Once 'count' digits have been produced we decide wether
    // to round up or down. Remainders of exactly .5 round upwards. Numbers such
    // as 9.999999 propagate a carry all the way, and change the
    // exponent (decimal_point), when rounding upwards.
    static void GenerateCountedDigits(int count, int* decimal_point,
        Bignum* numerator, Bignum* denominator,
        Vector<char> buffer, int* length) {
        ASSERT(count >= 0);
        for (int i = 0; i < count - 1; ++i) {
            uint16_t digit;
            digit = numerator->DivideModuloIntBignum(*denominator);
            ASSERT(digit <= 9);  // digit is a uint16_t and therefore always positive.
            // digit = numerator / denominator (integer division).
            // numerator = numerator % denominator.
            buffer[i] = static_cast<char>(digit + '0');
            // Prepare for next iteration.
            numerator->Times10();
        }
        // Generate the last digit.
        uint16_t digit;
        digit = numerator->DivideModuloIntBignum(*denominator);
        if (Bignum::PlusCompare(*numerator, *numerator, *denominator) >= 0) {
            digit++;
        }
        ASSERT(digit <= 10);
        buffer[count - 1] = static_cast<char>(digit + '0');
        // Correct bad digits (in case we had a sequence of '9's). Propagate the
        // carry until we hat a non-'9' or til we reach the first digit.
        for (int i = count - 1; i > 0; --i) {
            if (buffer[i] != '0' + 10) break;
            buffer[i] = '0';
            buffer[i - 1]++;
        }
        if (buffer[0] == '0' + 10) {
            // Propagate a carry past the top place.
            buffer[0] = '1';
            (*decimal_point)++;
        }
        *length = count;
    }


    // Generates 'requested_digits' after the decimal point. It might omit
    // trailing '0's. If the input number is too small then no digits at all are
    // generated (ex.: 2 fixed digits for 0.00001).
    //
    // Input verifies:  1 <= (numerator + delta) / denominator < 10.
    static void BignumToFixed(int requested_digits, int* decimal_point,
        Bignum* numerator, Bignum* denominator,
        Vector<char>(buffer), int* length) {
        // Note that we have to look at more than just the requested_digits, since
        // a number could be rounded up. Example: v=0.5 with requested_digits=0.
        // Even though the power of v equals 0 we can't just stop here.
        if (-(*decimal_point) > requested_digits) {
            // The number is definitively too small.
            // Ex: 0.001 with requested_digits == 1.
            // Set decimal-point to -requested_digits. This is what Gay does.
            // Note that it should not have any effect anyways since the string is
            // empty.
            *decimal_point = -requested_digits;
            *length = 0;
            return;
        }
        else if (-(*decimal_point) == requested_digits) {
            // We only need to verify if the number rounds down or up.
            // Ex: 0.04 and 0.06 with requested_digits == 1.
            ASSERT(*decimal_point == -requested_digits);
            // Initially the fraction lies in range (1, 10]. Multiply the denominator
            // by 10 so that we can compare more easily.
            denominator->Times10();
            if (Bignum::PlusCompare(*numerator, *numerator, *denominator) >= 0) {
                // If the fraction is >= 0.5 then we have to include the rounded
                // digit.
                buffer[0] = '1';
                *length = 1;
                (*decimal_point)++;
            }
            else {
                // Note that we caught most of similar cases earlier.
                *length = 0;
            }
            return;
        }
        else {
            // The requested digits correspond to the digits after the point.
            // The variable 'needed_digits' includes the digits before the point.
            int needed_digits = (*decimal_point) + requested_digits;
            GenerateCountedDigits(needed_digits, decimal_point,
                numerator, denominator,
                buffer, length);
        }
    }


    // Returns an estimation of k such that 10^(k-1) <= v < 10^k where
    // v = f * 2^exponent and 2^52 <= f < 2^53.
    // v is hence a normalized double with the given exponent. The output is an
    // approximation for the exponent of the decimal approimation .digits * 10^k.
    //
    // The result might undershoot by 1 in which case 10^k <= v < 10^k+1.
    // Note: this property holds for v's upper boundary m+ too.
    //    10^k <= m+ < 10^k+1.
    //   (see explanation below).
    //
    // Examples:
    //  EstimatePower(0)   => 16
    //  EstimatePower(-52) => 0
    //
    // Note: e >= 0 => EstimatedPower(e) > 0. No similar claim can be made for e<0.
    static int EstimatePower(int exponent) {
        // This function estimates log10 of v where v = f*2^e (with e == exponent).
        // Note that 10^floor(log10(v)) <= v, but v <= 10^ceil(log10(v)).
        // Note that f is bounded by its container size. Let p = 53 (the double's
        // significand size). Then 2^(p-1) <= f < 2^p.
        //
        // Given that log10(v) == log2(v)/log2(10) and e+(len(f)-1) is quite close
        // to log2(v) the function is simplified to (e+(len(f)-1)/log2(10)).
        // The computed number undershoots by less than 0.631 (when we compute log3
        // and not log10).
        //
        // Optimization: since we only need an approximated result this computation
        // can be performed on 64 bit integers. On x86/x64 architecture the speedup is
        // not really measurable, though.
        //
        // Since we want to avoid overshooting we decrement by 1e10 so that
        // floating-point imprecisions don't affect us.
        //
        // Explanation for v's boundary m+: the computation takes advantage of
        // the fact that 2^(p-1) <= f < 2^p. Boundaries still satisfy this requirement
        // (even for denormals where the delta can be much more important).

        const double k1Log10 = 0.30102999566398114;  // 1/lg(10)

        // For doubles len(f) == 53 (don't forget the hidden bit).
        const int kSignificandSize = Double::kSignificandSize;
        double estimate = ceil((exponent + kSignificandSize - 1) * k1Log10 - 1e-10);
        return static_cast<int>(estimate);
    }


    // See comments for InitialScaledStartValues.
    static void InitialScaledStartValuesPositiveExponent(
        uint64_t significand, int exponent,
        int estimated_power, bool need_boundary_deltas,
        Bignum* numerator, Bignum* denominator,
        Bignum* delta_minus, Bignum* delta_plus) {
        // A positive exponent implies a positive power.
        ASSERT(estimated_power >= 0);
        // Since the estimated_power is positive we simply multiply the denominator
        // by 10^estimated_power.

        // numerator = v.
        numerator->AssignUInt64(significand);
        numerator->ShiftLeft(exponent);
        // denominator = 10^estimated_power.
        denominator->AssignPowerUInt16(10, estimated_power);

        if (need_boundary_deltas) {
            // Introduce a common denominator so that the deltas to the boundaries are
            // integers.
            denominator->ShiftLeft(1);
            numerator->ShiftLeft(1);
            // Let v = f * 2^e, then m+ - v = 1/2 * 2^e; With the common
            // denominator (of 2) delta_plus equals 2^e.
            delta_plus->AssignUInt16(1);
            delta_plus->ShiftLeft(exponent);
            // Same for delta_minus. The adjustments if f == 2^p-1 are done later.
            delta_minus->AssignUInt16(1);
            delta_minus->ShiftLeft(exponent);
        }
    }


    // See comments for InitialScaledStartValues
    static void InitialScaledStartValuesNegativeExponentPositivePower(
        uint64_t significand, int exponent,
        int estimated_power, bool need_boundary_deltas,
        Bignum* numerator, Bignum* denominator,
        Bignum* delta_minus, Bignum* delta_plus) {
        // v = f * 2^e with e < 0, and with estimated_power >= 0.
        // This means that e is close to 0 (have a look at how estimated_power is
        // computed).

        // numerator = significand
        //  since v = significand * 2^exponent this is equivalent to
        //  numerator = v * / 2^-exponent
        numerator->AssignUInt64(significand);
        // denominator = 10^estimated_power * 2^-exponent (with exponent < 0)
        denominator->AssignPowerUInt16(10, estimated_power);
        denominator->ShiftLeft(-exponent);

        if (need_boundary_deltas) {
            // Introduce a common denominator so that the deltas to the boundaries are
            // integers.
            denominator->ShiftLeft(1);
            numerator->ShiftLeft(1);
            // Let v = f * 2^e, then m+ - v = 1/2 * 2^e; With the common
            // denominator (of 2) delta_plus equals 2^e.
            // Given that the denominator already includes v's exponent the distance
            // to the boundaries is simply 1.
            delta_plus->AssignUInt16(1);
            // Same for delta_minus. The adjustments if f == 2^p-1 are done later.
            delta_minus->AssignUInt16(1);
        }
    }


    // See comments for InitialScaledStartValues
    static void InitialScaledStartValuesNegativeExponentNegativePower(
        uint64_t significand, int exponent,
        int estimated_power, bool need_boundary_deltas,
        Bignum* numerator, Bignum* denominator,
        Bignum* delta_minus, Bignum* delta_plus) {
        // Instead of multiplying the denominator with 10^estimated_power we
        // multiply all values (numerator and deltas) by 10^-estimated_power.

        // Use numerator as temporary container for power_ten.
        Bignum* power_ten = numerator;
        power_ten->AssignPowerUInt16(10, -estimated_power);

        if (need_boundary_deltas) {
            // Since power_ten == numerator we must make a copy of 10^estimated_power
            // before we complete the computation of the numerator.
            // delta_plus = delta_minus = 10^estimated_power
            delta_plus->AssignBignum(*power_ten);
            delta_minus->AssignBignum(*power_ten);
        }

        // numerator = significand * 2 * 10^-estimated_power
        //  since v = significand * 2^exponent this is equivalent to
        // numerator = v * 10^-estimated_power * 2 * 2^-exponent.
        // Remember: numerator has been abused as power_ten. So no need to assign it
        //  to itself.
        ASSERT(numerator == power_ten);
        numerator->MultiplyByUInt64(significand);

        // denominator = 2 * 2^-exponent with exponent < 0.
        denominator->AssignUInt16(1);
        denominator->ShiftLeft(-exponent);

        if (need_boundary_deltas) {
            // Introduce a common denominator so that the deltas to the boundaries are
            // integers.
            numerator->ShiftLeft(1);
            denominator->ShiftLeft(1);
            // With this shift the boundaries have their correct value, since
            // delta_plus = 10^-estimated_power, and
            // delta_minus = 10^-estimated_power.
            // These assignments have been done earlier.
            // The adjustments if f == 2^p-1 (lower boundary is closer) are done later.
        }
    }


    // Let v = significand * 2^exponent.
    // Computes v / 10^estimated_power exactly, as a ratio of two bignums, numerator
    // and denominator. The functions GenerateShortestDigits and
    // GenerateCountedDigits will then convert this ratio to its decimal
    // representation d, with the required accuracy.
    // Then d * 10^estimated_power is the representation of v.
    // (Note: the fraction and the estimated_power might get adjusted before
    // generating the decimal representation.)
    //
    // The initial start values consist of:
    //  - a scaled numerator: s.t. numerator/denominator == v / 10^estimated_power.
    //  - a scaled (common) denominator.
    //  optionally (used by GenerateShortestDigits to decide if it has the shortest
    //  decimal converting back to v):
    //  - v - m-: the distance to the lower boundary.
    //  - m+ - v: the distance to the upper boundary.
    //
    // v, m+, m-, and therefore v - m- and m+ - v all share the same denominator.
    //
    // Let ep == estimated_power, then the returned values will satisfy:
    //  v / 10^ep = numerator / denominator.
    //  v's boundarys m- and m+:
    //    m- / 10^ep == v / 10^ep - delta_minus / denominator
    //    m+ / 10^ep == v / 10^ep + delta_plus / denominator
    //  Or in other words:
    //    m- == v - delta_minus * 10^ep / denominator;
    //    m+ == v + delta_plus * 10^ep / denominator;
    //
    // Since 10^(k-1) <= v < 10^k    (with k == estimated_power)
    //  or       10^k <= v < 10^(k+1)
    //  we then have 0.1 <= numerator/denominator < 1
    //           or    1 <= numerator/denominator < 10
    //
    // It is then easy to kickstart the digit-generation routine.
    //
    // The boundary-deltas are only filled if the mode equals BIGNUM_DTOA_SHORTEST
    // or BIGNUM_DTOA_SHORTEST_SINGLE.

    static void InitialScaledStartValues(uint64_t significand,
        int exponent,
        bool lower_boundary_is_closer,
        int estimated_power,
        bool need_boundary_deltas,
        Bignum* numerator,
        Bignum* denominator,
        Bignum* delta_minus,
        Bignum* delta_plus) {
        if (exponent >= 0) {
            InitialScaledStartValuesPositiveExponent(
                significand, exponent, estimated_power, need_boundary_deltas,
                numerator, denominator, delta_minus, delta_plus);
        }
        else if (estimated_power >= 0) {
            InitialScaledStartValuesNegativeExponentPositivePower(
                significand, exponent, estimated_power, need_boundary_deltas,
                numerator, denominator, delta_minus, delta_plus);
        }
        else {
            InitialScaledStartValuesNegativeExponentNegativePower(
                significand, exponent, estimated_power, need_boundary_deltas,
                numerator, denominator, delta_minus, delta_plus);
        }

        if (need_boundary_deltas && lower_boundary_is_closer) {
            // The lower boundary is closer at half the distance of "normal" numbers.
            // Increase the common denominator and adapt all but the delta_minus.
            denominator->ShiftLeft(1);  // *2
            numerator->ShiftLeft(1);    // *2
            delta_plus->ShiftLeft(1);   // *2
        }
    }


    // This routine multiplies numerator/denominator so that its values lies in the
    // range 1-10. That is after a call to this function we have:
    //    1 <= (numerator + delta_plus) /denominator < 10.
    // Let numerator the input before modification and numerator' the argument
    // after modification, then the output-parameter decimal_point is such that
    //  numerator / denominator * 10^estimated_power ==
    //    numerator' / denominator' * 10^(decimal_point - 1)
    // In some cases estimated_power was too low, and this is already the case. We
    // then simply adjust the power so that 10^(k-1) <= v < 10^k (with k ==
    // estimated_power) but do not touch the numerator or denominator.
    // Otherwise the routine multiplies the numerator and the deltas by 10.
    static void FixupMultiply10(int estimated_power, bool is_even,
        int* decimal_point,
        Bignum* numerator, Bignum* denominator,
        Bignum* delta_minus, Bignum* delta_plus) {
        bool in_range;
        if (is_even) {
            // For IEEE doubles half-way cases (in decimal system numbers ending with 5)
            // are rounded to the closest floating-point number with even significand.
            in_range = Bignum::PlusCompare(*numerator, *delta_plus, *denominator) >= 0;
        }
        else {
            in_range = Bignum::PlusCompare(*numerator, *delta_plus, *denominator) > 0;
        }
        if (in_range) {
            // Since numerator + delta_plus >= denominator we already have
            // 1 <= numerator/denominator < 10. Simply update the estimated_power.
            *decimal_point = estimated_power + 1;
        }
        else {
            *decimal_point = estimated_power;
            numerator->Times10();
            if (Bignum::Equal(*delta_minus, *delta_plus)) {
                delta_minus->Times10();
                delta_plus->AssignBignum(*delta_minus);
            }
            else {
                delta_minus->Times10();
                delta_plus->Times10();
            }
        }
    }

}  // namespace double_conversion

