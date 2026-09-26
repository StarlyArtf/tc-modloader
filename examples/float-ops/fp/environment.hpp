/* SoftFloat 3e, wired to the way this project wants to use it.

   Everything here is about *state*: which rounding mode a call runs under,
   which tininess rule applies, where the exception flags are read from, and
   what happens to a NaN the library hands back.  The arithmetic itself stays
   in fp32.cpp.

   SoftFloat keeps one rounding mode, one tininess mode and one flag word per
   thread (fp/softfloat/platform.h defines THREAD_LOCAL, and
   softfloat_state.c picks that up).  `SoftFloatOperation` therefore makes each
   call self-contained: it sets the two modes and clears the flags on entry,
   and reads the five flags this project publishes when the call is done.  No
   host rounding state - the MXCSR of the game's thread, the compiler's
   contraction, FTZ/DAZ - can influence a result, and no flag survives into the
   next operation.

   The specialization is ARM-VFPv2-defaultNaN (see
   third_party/berkeley-softfloat-3/README.md): its NaN results are the
   canonical 0x7FC00000 already, and `canonical()` below is the second half of
   the same promise - whatever NaN comes back from an arithmetic operation is
   replaced by that pattern, so the same inputs give the same bits on every
   build. */

#ifndef float_ops_fp_environment_hpp
#define float_ops_fp_environment_hpp

#include "softfloat/platform.h"

extern "C" {
#include "softfloat.h"
}

#include <cstdint>

#include "fp32.hpp"

namespace tcfp {
namespace softfloat_environment {

/* SoftFloat's rounding constants are a superset of the five IEEE 754 modes the
   Mod exposes (plan 2.2).  Keep this a plain switch: a new mode must be a
   deliberate edit here, not a value that happens to type-check. */
inline uint_fast8_t roundingMode(FPRounding rounding) {
    switch (rounding) {
        case FPRounding::nearest_even:
            return softfloat_round_near_even;
        case FPRounding::ties_away:
            return softfloat_round_near_maxMag;
        case FPRounding::toward_zero:
            return softfloat_round_minMag;
        case FPRounding::toward_negative:
            return softfloat_round_min;
        case FPRounding::toward_positive:
            return softfloat_round_max;
    }
    return softfloat_round_near_even;
}

/* One SoftFloat call's worth of state.

   Usage is deliberately scoped: construct it, call exactly one SoftFloat
   function, then read flags() before it goes out of scope.  It cannot be
   copied, so it is hard to hold on to the state by accident. */
class SoftFloatOperation {
public:
    explicit SoftFloatOperation(FPRounding rounding) {
        softfloat_roundingMode = roundingMode(rounding);
        /* The project's contract is tininess after rounding (plan 2.2); the
           specialization's own default is before-rounding, so this line is
           load-bearing rather than a repetition of the default. */
        softfloat_detectTininess = softfloat_tininess_afterRounding;
        softfloat_exceptionFlags = 0;
    }

    SoftFloatOperation(const SoftFloatOperation&) = delete;
    SoftFloatOperation& operator=(const SoftFloatOperation&) = delete;

    /* SoftFloat's flag word to the five bits this project publishes.  Note the
       rename: SoftFloat calls the flag it raises for a finite dividend over
       zero `softfloat_flag_infinite`, and that is this project's DZ. */
    uint8_t flags() const {
        const uint_fast8_t raw = softfloat_exceptionFlags;
        uint8_t out = 0;
        if (raw & softfloat_flag_invalid) out |= fp_flag_nv;
        if (raw & softfloat_flag_infinite) out |= fp_flag_dz;
        if (raw & softfloat_flag_overflow) out |= fp_flag_of;
        if (raw & softfloat_flag_underflow) out |= fp_flag_uf;
        if (raw & softfloat_flag_inexact) out |= fp_flag_nx;
        return out;
    }
};

/* Arithmetic results never carry a NaN payload: whatever NaN the library built
   is replaced by the canonical quiet NaN.  Bit operations must not go through
   here - they keep their payload by design (plan 2.4). */
inline FP32Result canonical(uint32_t bits, uint8_t flags) {
    if ((bits & kExponentMask) == kExponentMask &&
        (bits & kSignificandMask) != 0)
        bits = kCanonicalNaN;
    return FP32Result{bits, flags};
}

}  // namespace softfloat_environment
}  // namespace tcfp

#endif
