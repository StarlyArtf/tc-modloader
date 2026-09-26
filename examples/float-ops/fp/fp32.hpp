/* The IEEE 754 binary32 kernel of the Float Ops Mod.

   Values cross this boundary as raw 32-bit patterns only - never as a host
   `float`, and never as anything the game has to know about.  That keeps the
   semantics exactly the ones docs/PLAN-float-components.md fixes: rounding is
   chosen per call, the five exception flags are returned per call, and NaN
   results are canonical.

   The three shape rules that follow from the plan:

   - one rounding mode per operation, defaulting to nearest-ties-to-even;
   - the flags describe *this* operation only (plan 2.3), so nothing is sticky;
   - only the arithmetic entry points can raise a flag.  Negate, Absolute,
     CopySign, compare and classify are bit operations: they preserve payloads
     and never signal, which is what makes Split/Make Bits components possible
     later (plan 2.4). */

#ifndef float_ops_fp32_hpp
#define float_ops_fp32_hpp

#include <cstdint>

namespace tcfp {

/* Rounding modes, plan 2.2.  The numeric values are part of the archive: an
   instance configuration stores them, and the compact UI codes (RNE/RNA/RTZ/
   RDN/RUP) map onto these five in this order. */
enum class FPRounding : uint8_t {
    nearest_even = 0,
    ties_away = 1,
    toward_zero = 2,
    toward_negative = 3,
    toward_positive = 4,
};

/* The five exceptions, one bit each (plan 2.3).  The value is what an
   arithmetic component publishes on its Flags[5] output. */
enum FPFlags : uint8_t {
    fp_flag_nv = 0x10,  /* invalid operation */
    fp_flag_dz = 0x08,  /* divide by zero */
    fp_flag_of = 0x04,  /* overflow */
    fp_flag_uf = 0x02,  /* underflow */
    fp_flag_nx = 0x01,  /* inexact */
};

/* The canonical quiet NaN every arithmetic result uses (plan 2.4).  Bit
   operations keep whatever payload they were given. */
constexpr uint32_t kCanonicalNaN = 0x7FC00000u;
constexpr uint32_t kSignMask = 0x80000000u;
constexpr uint32_t kMagnitudeMask = 0x7FFFFFFFu;
constexpr uint32_t kExponentMask = 0x7F800000u;
constexpr uint32_t kSignificandMask = 0x007FFFFFu;
constexpr uint32_t kInfinity = 0x7F800000u;

struct FP32Result {
    uint32_t bits;
    uint8_t flags;
};

/* FP32 Compare is a quiet relation (plan 2.5): exactly one of lt/eq/gt is set
   for numbers, and a NaN input sets un instead.  `le` is derived by the caller
   as lt | eq, so it does not get a pin of its own. */
struct FP32Relation {
    bool lt;
    bool eq;
    bool gt;
    bool un;
    uint8_t flags;
};

/* FP32 Classify, plan 2.5: the bit order below is the wire order of the
   component's Class[10] output. */
enum FPClassBits : uint32_t {
    fp_class_negative_infinity = 1u << 0,
    fp_class_negative_normal = 1u << 1,
    fp_class_negative_subnormal = 1u << 2,
    fp_class_negative_zero = 1u << 3,
    fp_class_positive_zero = 1u << 4,
    fp_class_positive_subnormal = 1u << 5,
    fp_class_positive_normal = 1u << 6,
    fp_class_positive_infinity = 1u << 7,
    fp_class_signaling_nan = 1u << 8,
    fp_class_quiet_nan = 1u << 9,
};

/* ---- arithmetic: flags come from the operation itself ------------------- */

FP32Result fp32_add(uint32_t a, uint32_t b, FPRounding rounding);
FP32Result fp32_subtract(uint32_t a, uint32_t b, FPRounding rounding);
FP32Result fp32_multiply(uint32_t a, uint32_t b, FPRounding rounding);
FP32Result fp32_divide(uint32_t a, uint32_t b, FPRounding rounding);
FP32Result fp32_square_root(uint32_t a, FPRounding rounding);
/* a x b + c with a single rounding (plan 2.2). */
FP32Result fp32_fused_multiply_add(uint32_t a, uint32_t b, uint32_t c,
                                   FPRounding rounding);
/* IEEE remainder: the quotient is rounded to nearest-ties-to-even whatever
   the instance's rounding mode is, so this entry point takes no mode and the
   component carries no rounding badge (plan 6.3). */
FP32Result fp32_remainder(uint32_t a, uint32_t b);
/* Round to an integral value; NX is raised when the value changed (plan 6.3). */
FP32Result fp32_round_to_integral_exact(uint32_t a, FPRounding rounding);

/* ---- bit operations: no flags, no canonicalization --------------------- */

uint32_t fp32_negate(uint32_t a);
uint32_t fp32_absolute(uint32_t a);
uint32_t fp32_copy_sign(uint32_t a, uint32_t sign_source);

/* ---- relations, classification and the two 2019 min/max operations ----- */

FP32Relation fp32_compare(uint32_t a, uint32_t b);
uint32_t fp32_classify(uint32_t a);

FP32Result fp32_minimum_number(uint32_t a, uint32_t b);
FP32Result fp32_maximum_number(uint32_t a, uint32_t b);

/* ---- integer conversion (plan 2.6, decision D2) ----------------------- */

/* An integer result: the value in the low 32 bits (two's complement for the
   signed conversion) plus the flags this operation raised. */
struct FP32IntResult {
    uint32_t value;
    uint8_t flags;
};

/* Integer to binary32.  Every 32-bit integer is a valid input, so these never
   raise NV; they round per the instance's mode and raise NX when the integer
   was not representable exactly. */
FP32Result fp32_from_i32(int32_t value, FPRounding rounding);
FP32Result fp32_from_u32(uint32_t value, FPRounding rounding);

/* Binary32 to integer, with the saturating policy decision D2 fixes: NaN, an
   infinity and every value outside the destination range set NV and return the
   saturated bound (INT32_MAX, INT32_MIN, UINT32_MAX or 0); in-range values
   round per the instance's mode and raise NX when the value changed; a negative
   zero converts to 0 without NV, and a negative non-zero value is out of range
   for the unsigned destination. */
FP32IntResult fp32_to_i32(uint32_t a, FPRounding rounding);
FP32IntResult fp32_to_u32(uint32_t a, FPRounding rounding);

/* ---- predicates -------------------------------------------------------- */

bool fp32_is_nan(uint32_t a);
/* IEEE signaling: exponent all ones, significand non-zero, quiet bit clear. */
bool fp32_is_signaling_nan(uint32_t a);
bool fp32_is_infinite(uint32_t a);
bool fp32_is_zero(uint32_t a);
bool fp32_is_subnormal(uint32_t a);
bool fp32_is_negative(uint32_t a);

}  // namespace tcfp

#endif
