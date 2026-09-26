/* Float Ops binary32 kernel: SoftFloat 3e behind the project's contract.

   Every arithmetic entry point does the same four things:

     1. open a SoftFloatOperation, which sets the rounding mode, pins tininess
        to after-rounding and clears the flag word;
     2. call exactly one SoftFloat function with raw bit patterns;
     3. read the five flags that call produced;
     4. canonicalize a NaN result, because the project's arithmetic never
        carries a payload (plan 2.4).

   Everything else - the quiet compare relation, classification, the 2019
   min/max pair and the predicates - is plain integer code over the same bit
   patterns.  Those operations are part of the Mod's contract rather than
   SoftFloat's library surface, and keeping them here means their semantics do
   not move when the vendored library is updated. */

#include "environment.hpp"

#include <cstdint>

namespace tcfp {
namespace {

using softfloat_environment::canonical;
using softfloat_environment::roundingMode;
using softfloat_environment::SoftFloatOperation;

inline float32_t toF32(uint32_t bits) {
    float32_t value;
    value.v = bits;
    return value;
}

inline uint32_t fromF32(float32_t value) { return value.v; }

inline bool isNaNPattern(uint32_t bits) {
    return (bits & kExponentMask) == kExponentMask &&
           (bits & kSignificandMask) != 0;
}

inline bool isSignalingNaNPattern(uint32_t bits) {
    return isNaNPattern(bits) && (bits & 0x00400000u) == 0;
}

inline bool isZeroPattern(uint32_t bits) { return (bits & kMagnitudeMask) == 0; }

/* The magnitudes where the integer conversions leave their destination range:
   binary32 2^31 and 2^32 (the exact bit patterns, so no host float is needed to
   compare against them). */
constexpr uint32_t kTwoTo31 = 0x4F000000u;
constexpr uint32_t kTwoTo32 = 0x4F800000u;

}  // namespace

/* ---- arithmetic --------------------------------------------------------- */

FP32Result fp32_add(uint32_t a, uint32_t b, FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    const uint32_t bits = fromF32(f32_add(toF32(a), toF32(b)));
    return canonical(bits, operation.flags());
}

FP32Result fp32_subtract(uint32_t a, uint32_t b, FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    const uint32_t bits = fromF32(f32_sub(toF32(a), toF32(b)));
    return canonical(bits, operation.flags());
}

FP32Result fp32_multiply(uint32_t a, uint32_t b, FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    const uint32_t bits = fromF32(f32_mul(toF32(a), toF32(b)));
    return canonical(bits, operation.flags());
}

FP32Result fp32_divide(uint32_t a, uint32_t b, FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    const uint32_t bits = fromF32(f32_div(toF32(a), toF32(b)));
    return canonical(bits, operation.flags());
}

FP32Result fp32_square_root(uint32_t a, FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    const uint32_t bits = fromF32(f32_sqrt(toF32(a)));
    return canonical(bits, operation.flags());
}

FP32Result fp32_fused_multiply_add(uint32_t a, uint32_t b, uint32_t c,
                                   FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    const uint32_t bits = fromF32(f32_mulAdd(toF32(a), toF32(b), toF32(c)));
    return canonical(bits, operation.flags());
}

FP32Result fp32_remainder(uint32_t a, uint32_t b) {
    /* IEEE remainder rounds the quotient to nearest-ties-to-even regardless of
       the instance's mode, so the mode is pinned here rather than taken as an
       argument (plan 6.3). */
    SoftFloatOperation operation(FPRounding::nearest_even);
    const uint32_t bits = fromF32(f32_rem(toF32(a), toF32(b)));
    return canonical(bits, operation.flags());
}

FP32Result fp32_round_to_integral_exact(uint32_t a, FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    const uint32_t bits =
        fromF32(f32_roundToInt(toF32(a), roundingMode(rounding), true));
    return canonical(bits, operation.flags());
}

/* ---- bit operations ----------------------------------------------------- */

uint32_t fp32_negate(uint32_t a) { return a ^ kSignMask; }

uint32_t fp32_absolute(uint32_t a) { return a & kMagnitudeMask; }

uint32_t fp32_copy_sign(uint32_t a, uint32_t sign_source) {
    return (a & kMagnitudeMask) | (sign_source & kSignMask);
}

/* ---- relation, classification, min/max ---------------------------------- */

FP32Relation fp32_compare(uint32_t a, uint32_t b) {
    FP32Relation relation{false, false, false, false, 0};
    if (isNaNPattern(a) || isNaNPattern(b)) {
        /* Unordered.  A quiet NaN stays quiet; a signaling NaN signals, even
           when the other operand is a quiet NaN (plan 2.5). */
        relation.un = true;
        if (isSignalingNaNPattern(a) || isSignalingNaNPattern(b))
            relation.flags |= fp_flag_nv;
        return relation;
    }

    const uint32_t magnitude_a = a & kMagnitudeMask;
    const uint32_t magnitude_b = b & kMagnitudeMask;
    if (magnitude_a == 0 && magnitude_b == 0) {
        relation.eq = true; /* +0 and -0 compare equal */
        return relation;
    }

    const bool negative_a = (a & kSignMask) != 0;
    const bool negative_b = (b & kSignMask) != 0;
    int order = 0;
    if (negative_a != negative_b) {
        order = negative_a ? -1 : 1;
    } else if (!negative_a) {
        /* Same sign: for these patterns the unsigned order of the magnitude
           field is the numeric order. */
        order = magnitude_a < magnitude_b ? -1 : (magnitude_a > magnitude_b ? 1 : 0);
    } else {
        order = magnitude_a < magnitude_b ? 1 : (magnitude_a > magnitude_b ? -1 : 0);
    }
    relation.lt = order < 0;
    relation.eq = order == 0;
    relation.gt = order > 0;
    return relation;
}

uint32_t fp32_classify(uint32_t a) {
    const bool negative = (a & kSignMask) != 0;
    const uint32_t exponent = (a & kExponentMask) >> 23;
    const uint32_t significand = a & kSignificandMask;
    if (exponent == 0xFF) {
        if (significand == 0)
            return negative ? fp_class_negative_infinity : fp_class_positive_infinity;
        return (significand & 0x00400000u) ? fp_class_quiet_nan
                                           : fp_class_signaling_nan;
    }
    if (exponent == 0) {
        if (significand == 0)
            return negative ? fp_class_negative_zero : fp_class_positive_zero;
        return negative ? fp_class_negative_subnormal : fp_class_positive_subnormal;
    }
    return negative ? fp_class_negative_normal : fp_class_positive_normal;
}

namespace {

/* IEEE 754-2019 minimumNumber/maximumNumber (plan 2.5).  SoftFloat 3e has no
   such operation, so the NaN rules and the signed-zero rule live here and are
   pinned by the golden vectors rather than by the library. */
FP32Result minMaxNumber(uint32_t a, uint32_t b, bool maximum) {
    uint8_t flags = 0;
    const bool nan_a = isNaNPattern(a);
    const bool nan_b = isNaNPattern(b);
    if (nan_a || nan_b) {
        if (nan_a && isSignalingNaNPattern(a)) flags |= fp_flag_nv;
        if (nan_b && isSignalingNaNPattern(b)) flags |= fp_flag_nv;
        if (nan_a && nan_b) return FP32Result{kCanonicalNaN, flags};
        return FP32Result{nan_a ? b : a, flags};
    }
    if (isZeroPattern(a) && isZeroPattern(b)) {
        /* minimumNumber(-0,+0) is -0 and maximumNumber(-0,+0) is +0: with both
           operands zero the OR keeps the sign bit for the minimum and the AND
           drops it for the maximum. */
        return FP32Result{maximum ? (a & b) : (a | b), 0};
    }
    const FP32Relation relation = fp32_compare(a, b);
    const uint32_t chosen = maximum ? (relation.gt ? a : b) : (relation.lt ? a : b);
    return FP32Result{chosen, 0};
}

}  // namespace

FP32Result fp32_minimum_number(uint32_t a, uint32_t b) {
    return minMaxNumber(a, b, false);
}

FP32Result fp32_maximum_number(uint32_t a, uint32_t b) {
    return minMaxNumber(a, b, true);
}

/* ---- predicates --------------------------------------------------------- */

bool fp32_is_nan(uint32_t a) { return isNaNPattern(a); }

bool fp32_is_signaling_nan(uint32_t a) { return isSignalingNaNPattern(a); }

bool fp32_is_infinite(uint32_t a) {
    return (a & kExponentMask) == kExponentMask && (a & kSignificandMask) == 0;
}

bool fp32_is_zero(uint32_t a) { return isZeroPattern(a); }

bool fp32_is_subnormal(uint32_t a) {
    return (a & kExponentMask) == 0 && (a & kSignificandMask) != 0;
}

bool fp32_is_negative(uint32_t a) { return (a & kSignMask) != 0; }

/* ---- integer conversion (plan 2.6, decision D2) ------------------------- */

FP32Result fp32_from_i32(int32_t value, FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    /* SoftFloat's integer-to-float path honours softfloat_roundingMode (it ends
       in softfloat_roundPackToF32 / softfloat_normRoundPackToF32), so the
       instance's mode reaches it; the only flag it can raise is NX.  The call
       and the flag read are separate statements on purpose: arguments may be
       evaluated in any order, and reading the flags first would read the
       *previous* call's word. */
    const uint32_t bits = fromF32(i32_to_f32(value));
    return canonical(bits, operation.flags());
}

FP32Result fp32_from_u32(uint32_t value, FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    const uint32_t bits = fromF32(ui32_to_f32(value));
    return canonical(bits, operation.flags());
}

/* The two integer destinations share their shape: reject what is outside the
   range with NV and the saturated bound, hand everything else to SoftFloat with
   `exact` set so a value that changed reports NX.  Doing the range test here
   (rather than leaning on the library's own overflow returns) is what makes the
   policy independent of SoftFloat's specialization: the ARM-VFPv2-defaultNaN
   build returns 0 for a NaN converted to int, and decision D2 asks for the
   saturated RISC-V-style bound instead. */
FP32IntResult fp32_to_i32(uint32_t a, FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    const bool negative = (a & kSignMask) != 0;
    const uint32_t magnitude = a & kMagnitudeMask;
    if (isNaNPattern(a)) {
        softfloat_raiseFlags(softfloat_flag_invalid);
        return FP32IntResult{0x7FFFFFFFu, operation.flags()};
    }
    if (magnitude == kInfinity || magnitude > kTwoTo31 ||
        (magnitude == kTwoTo31 && !negative)) {
        softfloat_raiseFlags(softfloat_flag_invalid);
        return FP32IntResult{negative ? 0x80000000u : 0x7FFFFFFFu, operation.flags()};
    }
    const int32_t value =
        static_cast<int32_t>(f32_to_i32(toF32(a), roundingMode(rounding), true));
    return FP32IntResult{static_cast<uint32_t>(value), operation.flags()};
}

FP32IntResult fp32_to_u32(uint32_t a, FPRounding rounding) {
    SoftFloatOperation operation(rounding);
    const bool negative = (a & kSignMask) != 0;
    const uint32_t magnitude = a & kMagnitudeMask;
    if (isNaNPattern(a)) {
        softfloat_raiseFlags(softfloat_flag_invalid);
        return FP32IntResult{0xFFFFFFFFu, operation.flags()};
    }
    if (magnitude == kInfinity) {
        softfloat_raiseFlags(softfloat_flag_invalid);
        return FP32IntResult{negative ? 0u : 0xFFFFFFFFu, operation.flags()};
    }
    /* A negative non-zero value is out of range; a negative zero is not (plan
       2.6), which is why the test is on the magnitude. */
    if (magnitude >= kTwoTo32 || (negative && magnitude != 0)) {
        softfloat_raiseFlags(softfloat_flag_invalid);
        return FP32IntResult{negative ? 0u : 0xFFFFFFFFu, operation.flags()};
    }
    const uint32_t value = static_cast<uint32_t>(
        f32_to_ui32(toF32(a), roundingMode(rounding), true));
    return FP32IntResult{value, operation.flags()};
}

}  // namespace tcfp
