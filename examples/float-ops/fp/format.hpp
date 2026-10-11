/* The three IEEE 754 formats behind one entry point.

   The Mod's components are format-parametric: an instance stores which width
   (binary16, binary32 or binary64) it works in, and every operation asks this
   table for the implementation of that width.  That is what makes
   `local.float-ops` a *width-selectable* float family instead of three
   families of components: one Constant, one Add, one Display, and the width
   box in their corner picks the format the instance computes in.

   The split of responsibility is deliberate:

     * binary32 keeps calling fp32.cpp - the kernel that has the golden
       vectors, the measured real-machine evidence and the archived rounding
       contract.  This table does not re-implement it;
     * binary16 and binary64 go straight to the same vendored SoftFloat 3e
       subset (fp/softfloat-sources.txt), so their arithmetic, their flags and
       their NaN rules come from the same library and the same
       `SoftFloatOperation` scoping as binary32;
     * the integer code that is *not* SoftFloat - the quiet compare relation,
       classification, minimumNumber/maximumNumber, the bit operations and the
       bit-field split - is written once against a Layout (sign/exponent/
       significand widths) so all three formats share it.  Its binary32 answers
       are pinned against fp32.cpp's by tests/float-kernel.cpp.

   Values cross this boundary as raw bit patterns in a uint64_t, exactly like
   the binary32 kernel: never as a host `float`, so nothing about the game's
   MXCSR, FTZ/DAZ or the compiler's contraction can reach a result. */

#ifndef float_ops_format_hpp
#define float_ops_format_hpp

#include "fp32.hpp"

#include <cstdint>

namespace tcfp {

/* The three widths the Mod offers, as the numbers a configuration stores.
   They are the format byte's whole vocabulary: anything else is refused when
   the configuration is decoded (components.cpp), never silently treated as 32. */
constexpr uint32_t kFormatBinary16 = 16;
constexpr uint32_t kFormatBinary32 = 32;
constexpr uint32_t kFormatBinary64 = 64;

inline bool knownFormat(uint32_t format) {
    return format == kFormatBinary16 || format == kFormatBinary32 ||
           format == kFormatBinary64;
}

/* Where the fields are, per format.  Everything the bit-level code needs is
   derived here once, so a new format is a new row and not a new set of masks
   sprinkled through the operations. */
struct Layout {
    uint32_t width;            /* 16, 32 or 64 */
    uint32_t exponent_bits;    /* 5, 8 or 11 */
    uint32_t significand_bits; /* 10, 23 or 52 */
    uint64_t sign_mask;
    uint64_t exponent_mask;
    uint64_t significand_mask;
    uint64_t magnitude_mask;   /* everything but the sign */
    uint64_t infinity;         /* exponent all ones, significand zero */
    uint64_t quiet_bit;
    uint64_t canonical_nan;    /* the quiet NaN every arithmetic result uses */
};

const Layout& layoutFor(uint32_t format);

/* One binary32 value's worth of storage: the low `width` bits of `bits` are
   the pattern, so a 16-bit pattern read out of a wider wire needs no
   second type. */
inline uint64_t truncate(uint32_t format, uint64_t bits) {
    if (format == kFormatBinary16) return bits & 0xFFFFu;
    if (format == kFormatBinary32) return bits & 0xFFFFFFFFu;
    return bits;
}

/* ---- results ------------------------------------------------------------ */

struct FormatResult {
    uint64_t bits;
    uint8_t flags;
};

struct FormatIntResult {
    uint64_t value;   /* the low `width` bits hold the integer */
    uint8_t flags;
};

/* The quiet relation (plan 2.5): exactly one of lt/eq/gt for numbers, and un
   for an unordered pair.  `le` is derived by the caller as lt | eq. */
struct FormatRelation {
    bool lt;
    bool eq;
    bool gt;
    bool un;
    uint8_t flags;
};

/* The bit fields of one value, for the Split/Make Bits pair. */
struct FormatFields {
    uint64_t sign;
    uint64_t exponent;
    uint64_t significand;
};

/* Everything a component can ask of one format.  The table is constant and
   lives for the process's lifetime; callers hold the reference, never a copy. */
struct FormatOps {
    uint32_t format;
    const Layout* layout;

    FormatResult (*add)(uint64_t, uint64_t, FPRounding);
    FormatResult (*subtract)(uint64_t, uint64_t, FPRounding);
    FormatResult (*multiply)(uint64_t, uint64_t, FPRounding);
    FormatResult (*divide)(uint64_t, uint64_t, FPRounding);
    FormatResult (*square_root)(uint64_t, FPRounding);
    FormatResult (*fused_multiply_add)(uint64_t, uint64_t, uint64_t, FPRounding);
    FormatResult (*remainder)(uint64_t, uint64_t);
    FormatResult (*round_to_integral_exact)(uint64_t, FPRounding);

    /* Bit operations: no flags, no canonicalization, payloads preserved. */
    uint64_t (*negate)(uint64_t);
    uint64_t (*absolute)(uint64_t);
    uint64_t (*copy_sign)(uint64_t, uint64_t);

    FormatRelation (*compare)(uint64_t, uint64_t);
    uint64_t (*classify)(uint64_t);
    FormatResult (*minimum_number)(uint64_t, uint64_t);
    FormatResult (*maximum_number)(uint64_t, uint64_t);

    FormatResult (*from_i32)(int32_t, FPRounding);
    FormatResult (*from_u32)(uint32_t, FPRounding);
    FormatIntResult (*to_i32)(uint64_t, FPRounding);
    FormatIntResult (*to_u32)(uint64_t, FPRounding);

    bool (*is_nan)(uint64_t);
    bool (*is_signaling_nan)(uint64_t);
    bool (*is_infinite)(uint64_t);
    bool (*is_zero)(uint64_t);
    bool (*is_subnormal)(uint64_t);
    bool (*is_negative)(uint64_t);

    FormatFields (*split)(uint64_t);
    uint64_t (*make_bits)(uint64_t sign, uint64_t exponent, uint64_t significand);

    /* The bit mask a component applies to an input pin before it is used (a
       16-bit pattern arriving on a 64-bit wire must not keep its upper bits). */
    uint64_t (*mask)(uint64_t);
};

/* The implementation of one format, or nullptr when the number is not one of
   the three.  Callers decode the configuration's format byte through
   knownFormat() first, so a nullptr here is a programming error, not a
   player-visible state. */
const FormatOps* formatOps(uint32_t format);

/* The next format in the width box's own order (16 -> 32 -> 64 -> 16). */
inline uint32_t nextFormat(uint32_t format) {
    if (format == kFormatBinary16) return kFormatBinary32;
    if (format == kFormatBinary32) return kFormatBinary64;
    return kFormatBinary16;
}

}  // namespace tcfp

#endif
