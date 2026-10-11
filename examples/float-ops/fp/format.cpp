/* The binary16/binary32/binary64 dispatch layer behind fp/format.hpp.

   Three things live here and nowhere else:

     * the Layout of each width (where the sign, exponent and significand sit);
     * the bit-level operations that are the Mod's own contract rather than
       SoftFloat's surface - the quiet compare relation, classification, the
       2019 minimumNumber/maximumNumber pair, the bit operations and the
       Split/Make Bits fields - written once against a Layout;
     * the two integer conversions, whose out-of-range policy is decision D2
       (saturate and raise NV) rather than the library's own returns.

   Arithmetic goes to SoftFloat under the same `SoftFloatOperation` scoping the
   binary32 kernel uses: binary16 and binary64 straight to the f16/f64 entries,
   binary32 through fp32.cpp - the kernel with the golden vectors and the
   measured real-machine evidence - so this table never re-implements it.
   tests/float-kernel.cpp asserts that a binary32 call through here and the
   same call through fp32.hpp agree bit for bit. */

#include "format.hpp"

#include "environment.hpp"

namespace tcfp {
namespace {

using softfloat_environment::roundingMode;
using softfloat_environment::SoftFloatOperation;

/* ---- the three layouts -------------------------------------------------- */

const Layout kLayout16 = {
    16, 5, 10,
    UINT64_C(0x8000), UINT64_C(0x7C00), UINT64_C(0x03FF),
    UINT64_C(0x7FFF), UINT64_C(0x7C00), UINT64_C(0x0200), UINT64_C(0x7E00)};
const Layout kLayout32 = {
    32, 8, 23,
    UINT64_C(0x80000000), UINT64_C(0x7F800000), UINT64_C(0x007FFFFF),
    UINT64_C(0x7FFFFFFF), UINT64_C(0x7F800000), UINT64_C(0x00400000),
    UINT64_C(0x7FC00000)};
const Layout kLayout64 = {
    64, 11, 52,
    UINT64_C(0x8000000000000000), UINT64_C(0x7FF0000000000000),
    UINT64_C(0x000FFFFFFFFFFFFF), UINT64_C(0x7FFFFFFFFFFFFFFF),
    UINT64_C(0x7FF0000000000000), UINT64_C(0x0008000000000000),
    UINT64_C(0x7FF8000000000000)};

/* Where the integer conversions stop being able to represent a value.  Binary16
   cannot reach either bound - its largest finite value is 65504, so every
   finite binary16 is inside the int32/uint32 range and only a NaN or an
   infinity can be out of it - which is why the reachability of the bound is
   part of the table instead of a comparison every call makes. */
struct Bounds {
    const Layout* layout;
    bool integer_range_reachable;
    uint64_t two_to_31;   /* the pattern of 2^31 in this format */
    uint64_t two_to_32;   /* the pattern of 2^32 in this format */
};

const Bounds kBounds16 = {&kLayout16, false, 0, 0};
const Bounds kBounds32 = {&kLayout32, true, UINT64_C(0x4F000000),
                          UINT64_C(0x4F800000)};
const Bounds kBounds64 = {&kLayout64, true, UINT64_C(0x41E0000000000000),
                          UINT64_C(0x41F0000000000000)};

inline float16_t toF16(uint64_t bits) {
    float16_t value;
    value.v = static_cast<uint16_t>(bits);
    return value;
}

inline float64_t toF64(uint64_t bits) {
    float64_t value;
    value.v = bits;
    return value;
}

/* ---- the bit-level contract, written once against a Layout -------------- */

inline bool isNaNPattern(const Layout& layout, uint64_t bits) {
    return (bits & layout.exponent_mask) == layout.exponent_mask &&
           (bits & layout.significand_mask) != 0;
}

inline bool isSignalingNaNPattern(const Layout& layout, uint64_t bits) {
    return isNaNPattern(layout, bits) && (bits & layout.quiet_bit) == 0;
}

inline bool isInfinitePattern(const Layout& layout, uint64_t bits) {
    return (bits & layout.exponent_mask) == layout.exponent_mask &&
           (bits & layout.significand_mask) == 0;
}

inline bool isZeroPattern(const Layout& layout, uint64_t bits) {
    return (bits & layout.magnitude_mask) == 0;
}

inline bool isSubnormalPattern(const Layout& layout, uint64_t bits) {
    return (bits & layout.exponent_mask) == 0 && (bits & layout.significand_mask) != 0;
}

/* Arithmetic results never carry a NaN payload (plan 2.4): whatever NaN the
   library built is replaced by the format's canonical quiet NaN.  Bit
   operations must not come through here - they keep their payload by design. */
inline FormatResult canonicalResult(const Layout& layout, uint64_t bits, uint8_t flags) {
    if (isNaNPattern(layout, bits)) bits = layout.canonical_nan;
    return FormatResult{bits, flags};
}

FormatRelation compareBits(const Layout& layout, uint64_t a, uint64_t b) {
    FormatRelation relation{false, false, false, false, 0};
    if (isNaNPattern(layout, a) || isNaNPattern(layout, b)) {
        /* Unordered.  A quiet NaN stays quiet; a signaling NaN signals, even
           when the other operand is a quiet NaN (plan 2.5). */
        relation.un = true;
        if (isSignalingNaNPattern(layout, a) || isSignalingNaNPattern(layout, b))
            relation.flags |= fp_flag_nv;
        return relation;
    }
    const uint64_t magnitude_a = a & layout.magnitude_mask;
    const uint64_t magnitude_b = b & layout.magnitude_mask;
    if (magnitude_a == 0 && magnitude_b == 0) {
        relation.eq = true;   /* +0 and -0 compare equal */
        return relation;
    }
    const bool negative_a = (a & layout.sign_mask) != 0;
    const bool negative_b = (b & layout.sign_mask) != 0;
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

uint64_t classifyBits(const Layout& layout, uint64_t bits) {
    const bool negative = (bits & layout.sign_mask) != 0;
    const uint64_t exponent = (bits & layout.exponent_mask) >> layout.significand_bits;
    const uint64_t significand = bits & layout.significand_mask;
    const uint64_t exponent_max = layout.exponent_mask >> layout.significand_bits;
    if (exponent == exponent_max) {
        if (significand == 0)
            return negative ? fp_class_negative_infinity : fp_class_positive_infinity;
        return (significand & layout.quiet_bit) ? fp_class_quiet_nan
                                                : fp_class_signaling_nan;
    }
    if (exponent == 0) {
        if (significand == 0)
            return negative ? fp_class_negative_zero : fp_class_positive_zero;
        return negative ? fp_class_negative_subnormal : fp_class_positive_subnormal;
    }
    return negative ? fp_class_negative_normal : fp_class_positive_normal;
}

/* IEEE 754-2019 minimumNumber/maximumNumber (plan 2.5).  SoftFloat 3e has no
   such operation, so the NaN rules and the signed-zero rule live here and are
   pinned by the golden vectors rather than by the library. */
FormatResult minMaxNumber(const Layout& layout, uint64_t a, uint64_t b, bool maximum) {
    uint8_t flags = 0;
    const bool nan_a = isNaNPattern(layout, a);
    const bool nan_b = isNaNPattern(layout, b);
    if (nan_a || nan_b) {
        if (nan_a && isSignalingNaNPattern(layout, a)) flags |= fp_flag_nv;
        if (nan_b && isSignalingNaNPattern(layout, b)) flags |= fp_flag_nv;
        if (nan_a && nan_b) return FormatResult{layout.canonical_nan, flags};
        return FormatResult{nan_a ? b : a, flags};
    }
    if (isZeroPattern(layout, a) && isZeroPattern(layout, b)) {
        /* minimumNumber(-0,+0) is -0 and maximumNumber(-0,+0) is +0: with both
           operands zero the OR keeps the sign bit for the minimum and the AND
           drops it for the maximum. */
        return FormatResult{maximum ? (a & b) : (a | b), 0};
    }
    const FormatRelation relation = compareBits(layout, a, b);
    return FormatResult{maximum ? (relation.gt ? a : b) : (relation.lt ? a : b), 0};
}

FormatFields splitBits(const Layout& layout, uint64_t bits) {
    FormatFields fields{};
    fields.sign = bits >> (layout.width - 1);
    fields.exponent = (bits & layout.exponent_mask) >> layout.significand_bits;
    fields.significand = bits & layout.significand_mask;
    return fields;
}

uint64_t makeBits(const Layout& layout, uint64_t sign, uint64_t exponent,
                  uint64_t significand) {
    const uint64_t exponent_max = layout.exponent_mask >> layout.significand_bits;
    return ((sign & 1u) << (layout.width - 1)) |
           ((exponent & exponent_max) << layout.significand_bits) |
           (significand & layout.significand_mask);
}

/* The D2 policy for a conversion to an integer: NaN, an infinity and a value
   outside the destination range raise NV and return the saturated bound; a
   negative non-zero value is out of range for the unsigned destination while a
   negative zero is not.  The range test compares the *magnitude field*, which
   for a fixed sign is ordered the same way the value is. */
struct RangeVerdict {
    bool out_of_range;
    uint64_t saturated;
};

RangeVerdict integerRange(const Bounds& bounds, uint64_t bits, uint64_t positive,
                          uint64_t negative, bool unsigned_destination) {
    const Layout& layout = *bounds.layout;
    const bool is_negative = (bits & layout.sign_mask) != 0;
    const uint64_t magnitude = bits & layout.magnitude_mask;
    const uint64_t bound = is_negative ? negative : positive;
    if (isNaNPattern(layout, bits)) return {true, positive};
    if (unsigned_destination && is_negative && magnitude != 0) return {true, negative};
    if (isInfinitePattern(layout, bits)) return {true, bound};
    if (!bounds.integer_range_reachable) return {false, 0};
    if (unsigned_destination) {
        if (magnitude >= bounds.two_to_32) return {true, bound};
    } else if (magnitude > bounds.two_to_31 ||
               (magnitude == bounds.two_to_31 && !is_negative)) {
        return {true, bound};
    }
    return {false, 0};
}

/* ---- one trait per format ----------------------------------------------- */

struct Binary16 {
    static const Bounds& bounds() { return kBounds16; }
    static const Layout& layout() { return kLayout16; }

    static FormatResult add(uint64_t a, uint64_t b, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f16_add(toF16(a), toF16(b)).v;
            return canonicalResult(kLayout16, bits, operation.flags());
    }
    static FormatResult subtract(uint64_t a, uint64_t b, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f16_sub(toF16(a), toF16(b)).v;
            return canonicalResult(kLayout16, bits, operation.flags());
    }
    static FormatResult multiply(uint64_t a, uint64_t b, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f16_mul(toF16(a), toF16(b)).v;
            return canonicalResult(kLayout16, bits, operation.flags());
    }
    static FormatResult divide(uint64_t a, uint64_t b, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f16_div(toF16(a), toF16(b)).v;
            return canonicalResult(kLayout16, bits, operation.flags());
    }
    static FormatResult squareRoot(uint64_t a, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f16_sqrt(toF16(a)).v;
            return canonicalResult(kLayout16, bits, operation.flags());
    }
    static FormatResult fusedMultiplyAdd(uint64_t a, uint64_t b, uint64_t c,
                                         FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f16_mulAdd(toF16(a), toF16(b), toF16(c)).v;
        return canonicalResult(kLayout16, bits, operation.flags());
    }
    static FormatResult remainder(uint64_t a, uint64_t b) {
        /* IEEE remainder rounds the quotient to nearest-ties-to-even whatever
           the instance's mode is, so the mode is pinned here (plan 6.3). */
        SoftFloatOperation operation(FPRounding::nearest_even);
        const uint64_t bits = f16_rem(toF16(a), toF16(b)).v;
        return canonicalResult(kLayout16, bits, operation.flags());
    }
    static FormatResult roundToIntegral(uint64_t a, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f16_roundToInt(toF16(a), roundingMode(rounding), true).v;
        return canonicalResult(kLayout16, bits, operation.flags());
    }
    static FormatResult fromI32(int32_t value, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = i32_to_f16(value).v;
            return canonicalResult(kLayout16, bits, operation.flags());
    }
    static FormatResult fromU32(uint32_t value, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = ui32_to_f16(value).v;
            return canonicalResult(kLayout16, bits, operation.flags());
    }
    static FormatIntResult toI32(uint64_t a, FPRounding rounding) {
        const RangeVerdict verdict =
            integerRange(kBounds16, a, UINT64_C(0x7FFFFFFF), UINT64_C(0x80000000), false);
        if (verdict.out_of_range) return FormatIntResult{verdict.saturated, fp_flag_nv};
        SoftFloatOperation operation(rounding);
        const int32_t value = f16_to_i32(toF16(a), roundingMode(rounding), true);
        return FormatIntResult{static_cast<uint32_t>(value), operation.flags()};
    }
    static FormatIntResult toU32(uint64_t a, FPRounding rounding) {
        const RangeVerdict verdict =
            integerRange(kBounds16, a, UINT64_C(0xFFFFFFFF), UINT64_C(0), true);
        if (verdict.out_of_range) return FormatIntResult{verdict.saturated, fp_flag_nv};
        SoftFloatOperation operation(rounding);
        const uint32_t value = f16_to_ui32(toF16(a), roundingMode(rounding), true);
        return FormatIntResult{value, operation.flags()};
    }
};

struct Binary64 {
    static const Bounds& bounds() { return kBounds64; }
    static const Layout& layout() { return kLayout64; }

    static FormatResult add(uint64_t a, uint64_t b, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f64_add(toF64(a), toF64(b)).v;
            return canonicalResult(kLayout64, bits, operation.flags());
    }
    static FormatResult subtract(uint64_t a, uint64_t b, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f64_sub(toF64(a), toF64(b)).v;
            return canonicalResult(kLayout64, bits, operation.flags());
    }
    static FormatResult multiply(uint64_t a, uint64_t b, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f64_mul(toF64(a), toF64(b)).v;
            return canonicalResult(kLayout64, bits, operation.flags());
    }
    static FormatResult divide(uint64_t a, uint64_t b, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f64_div(toF64(a), toF64(b)).v;
            return canonicalResult(kLayout64, bits, operation.flags());
    }
    static FormatResult squareRoot(uint64_t a, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f64_sqrt(toF64(a)).v;
            return canonicalResult(kLayout64, bits, operation.flags());
    }
    static FormatResult fusedMultiplyAdd(uint64_t a, uint64_t b, uint64_t c,
                                         FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f64_mulAdd(toF64(a), toF64(b), toF64(c)).v;
        return canonicalResult(kLayout64, bits, operation.flags());
    }
    static FormatResult remainder(uint64_t a, uint64_t b) {
        SoftFloatOperation operation(FPRounding::nearest_even);
        const uint64_t bits = f64_rem(toF64(a), toF64(b)).v;
        return canonicalResult(kLayout64, bits, operation.flags());
    }
    static FormatResult roundToIntegral(uint64_t a, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = f64_roundToInt(toF64(a), roundingMode(rounding), true).v;
        return canonicalResult(kLayout64, bits, operation.flags());
    }
    static FormatResult fromI32(int32_t value, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = i32_to_f64(value).v;
            return canonicalResult(kLayout64, bits, operation.flags());
    }
    static FormatResult fromU32(uint32_t value, FPRounding rounding) {
        SoftFloatOperation operation(rounding);
        const uint64_t bits = ui32_to_f64(value).v;
            return canonicalResult(kLayout64, bits, operation.flags());
    }
    static FormatIntResult toI32(uint64_t a, FPRounding rounding) {
        const RangeVerdict verdict =
            integerRange(kBounds64, a, UINT64_C(0x7FFFFFFF), UINT64_C(0x80000000), false);
        if (verdict.out_of_range) return FormatIntResult{verdict.saturated, fp_flag_nv};
        SoftFloatOperation operation(rounding);
        const int32_t value = f64_to_i32(toF64(a), roundingMode(rounding), true);
        return FormatIntResult{static_cast<uint32_t>(value), operation.flags()};
    }
    static FormatIntResult toU32(uint64_t a, FPRounding rounding) {
        const RangeVerdict verdict =
            integerRange(kBounds64, a, UINT64_C(0xFFFFFFFF), UINT64_C(0), true);
        if (verdict.out_of_range) return FormatIntResult{verdict.saturated, fp_flag_nv};
        SoftFloatOperation operation(rounding);
        const uint32_t value = f64_to_ui32(toF64(a), roundingMode(rounding), true);
        return FormatIntResult{value, operation.flags()};
    }
};

/* binary32 is not re-implemented here: the same call goes to fp32.cpp, which
   owns the archived binary32 contract, its golden vectors and its measured
   real-machine evidence. */
struct Binary32 {
    static const Bounds& bounds() { return kBounds32; }
    static const Layout& layout() { return kLayout32; }

    static FormatResult add(uint64_t a, uint64_t b, FPRounding rounding) {
        const FP32Result result = fp32_add(static_cast<uint32_t>(a),
                                           static_cast<uint32_t>(b), rounding);
        return FormatResult{result.bits, result.flags};
    }
    static FormatResult subtract(uint64_t a, uint64_t b, FPRounding rounding) {
        const FP32Result result = fp32_subtract(static_cast<uint32_t>(a),
                                                static_cast<uint32_t>(b), rounding);
        return FormatResult{result.bits, result.flags};
    }
    static FormatResult multiply(uint64_t a, uint64_t b, FPRounding rounding) {
        const FP32Result result = fp32_multiply(static_cast<uint32_t>(a),
                                                static_cast<uint32_t>(b), rounding);
        return FormatResult{result.bits, result.flags};
    }
    static FormatResult divide(uint64_t a, uint64_t b, FPRounding rounding) {
        const FP32Result result = fp32_divide(static_cast<uint32_t>(a),
                                              static_cast<uint32_t>(b), rounding);
        return FormatResult{result.bits, result.flags};
    }
    static FormatResult squareRoot(uint64_t a, FPRounding rounding) {
        const FP32Result result = fp32_square_root(static_cast<uint32_t>(a), rounding);
        return FormatResult{result.bits, result.flags};
    }
    static FormatResult fusedMultiplyAdd(uint64_t a, uint64_t b, uint64_t c,
                                         FPRounding rounding) {
        const FP32Result result = fp32_fused_multiply_add(
            static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(c),
            rounding);
        return FormatResult{result.bits, result.flags};
    }
    static FormatResult remainder(uint64_t a, uint64_t b) {
        const FP32Result result = fp32_remainder(static_cast<uint32_t>(a),
                                                 static_cast<uint32_t>(b));
        return FormatResult{result.bits, result.flags};
    }
    static FormatResult roundToIntegral(uint64_t a, FPRounding rounding) {
        const FP32Result result = fp32_round_to_integral_exact(static_cast<uint32_t>(a),
                                                               rounding);
        return FormatResult{result.bits, result.flags};
    }
    static FormatResult fromI32(int32_t value, FPRounding rounding) {
        const FP32Result result = fp32_from_i32(value, rounding);
        return FormatResult{result.bits, result.flags};
    }
    static FormatResult fromU32(uint32_t value, FPRounding rounding) {
        const FP32Result result = fp32_from_u32(value, rounding);
        return FormatResult{result.bits, result.flags};
    }
    static FormatIntResult toI32(uint64_t a, FPRounding rounding) {
        const FP32IntResult result = fp32_to_i32(static_cast<uint32_t>(a), rounding);
        return FormatIntResult{result.value, result.flags};
    }
    static FormatIntResult toU32(uint64_t a, FPRounding rounding) {
        const FP32IntResult result = fp32_to_u32(static_cast<uint32_t>(a), rounding);
        return FormatIntResult{result.value, result.flags};
    }
};

/* ---- the shared half of every trait ------------------------------------- */

template <typename T>
uint64_t traitMask(uint64_t bits) {
    const Layout& layout = T::layout();
    return layout.width == 64 ? bits : (bits & ((UINT64_C(1) << layout.width) - 1));
}

template <typename T>
FormatRelation traitCompare(uint64_t a, uint64_t b) {
    return compareBits(T::layout(), traitMask<T>(a), traitMask<T>(b));
}

template <typename T>
uint64_t traitClassify(uint64_t a) {
    return classifyBits(T::layout(), traitMask<T>(a));
}

template <typename T>
FormatResult traitMinimum(uint64_t a, uint64_t b) {
    return minMaxNumber(T::layout(), traitMask<T>(a), traitMask<T>(b), false);
}

template <typename T>
FormatResult traitMaximum(uint64_t a, uint64_t b) {
    return minMaxNumber(T::layout(), traitMask<T>(a), traitMask<T>(b), true);
}

template <typename T>
uint64_t traitNegate(uint64_t a) {
    return traitMask<T>(a) ^ T::layout().sign_mask;
}

template <typename T>
uint64_t traitAbsolute(uint64_t a) {
    return traitMask<T>(a) & T::layout().magnitude_mask;
}

template <typename T>
uint64_t traitCopySign(uint64_t a, uint64_t sign_source) {
    const Layout& layout = T::layout();
    return (traitMask<T>(a) & layout.magnitude_mask) |
           (traitMask<T>(sign_source) & layout.sign_mask);
}

template <typename T>
bool traitIsNaN(uint64_t a) {
    return isNaNPattern(T::layout(), traitMask<T>(a));
}

template <typename T>
bool traitIsSignalingNaN(uint64_t a) {
    return isSignalingNaNPattern(T::layout(), traitMask<T>(a));
}

template <typename T>
bool traitIsInfinite(uint64_t a) {
    return isInfinitePattern(T::layout(), traitMask<T>(a));
}

template <typename T>
bool traitIsZero(uint64_t a) {
    return isZeroPattern(T::layout(), traitMask<T>(a));
}

template <typename T>
bool traitIsSubnormal(uint64_t a) {
    return isSubnormalPattern(T::layout(), traitMask<T>(a));
}

template <typename T>
bool traitIsNegative(uint64_t a) {
    return (traitMask<T>(a) & T::layout().sign_mask) != 0;
}

template <typename T>
FormatFields traitSplit(uint64_t a) {
    return splitBits(T::layout(), traitMask<T>(a));
}

template <typename T>
uint64_t traitMakeBits(uint64_t sign, uint64_t exponent, uint64_t significand) {
    return makeBits(T::layout(), sign, exponent, significand);
}

template <typename T>
const FormatOps* opsFor() {
    static const FormatOps ops = {
        T::layout().width,
        &T::layout(),
        &T::add,
        &T::subtract,
        &T::multiply,
        &T::divide,
        &T::squareRoot,
        &T::fusedMultiplyAdd,
        &T::remainder,
        &T::roundToIntegral,
        &traitNegate<T>,
        &traitAbsolute<T>,
        &traitCopySign<T>,
        &traitCompare<T>,
        &traitClassify<T>,
        &traitMinimum<T>,
        &traitMaximum<T>,
        &T::fromI32,
        &T::fromU32,
        &T::toI32,
        &T::toU32,
        &traitIsNaN<T>,
        &traitIsSignalingNaN<T>,
        &traitIsInfinite<T>,
        &traitIsZero<T>,
        &traitIsSubnormal<T>,
        &traitIsNegative<T>,
        &traitSplit<T>,
        &traitMakeBits<T>,
        &traitMask<T>,
    };
    return &ops;
}

}  // namespace

const Layout& layoutFor(uint32_t format) {
    if (format == kFormatBinary16) return kLayout16;
    if (format == kFormatBinary64) return kLayout64;
    return kLayout32;
}

const FormatOps* formatOps(uint32_t format) {
    if (format == kFormatBinary16) return opsFor<Binary16>();
    if (format == kFormatBinary64) return opsFor<Binary64>();
    if (format == kFormatBinary32) return opsFor<Binary32>();
    return nullptr;
}

}  // namespace tcfp
