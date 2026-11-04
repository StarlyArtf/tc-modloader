/* SSE/SSE2 fast paths for the existing raw-bit arithmetic contract.
   Keep rounding changes, arithmetic, status capture and restoration in one
   volatile assembly block: the compiler must not move arithmetic across the
   MXCSR boundary. No AVX/FMA or machine-specific build flags are required.

   SoftFloat remains authoritative for ties-away, special/subnormal inputs,
   overflow and tiny results (including the smallest normal binade). This
   avoids depending on x86's tininess policy at the rounding boundary. */
#ifndef float_ops_hardware_hpp
#define float_ops_hardware_hpp

#include "fp32.hpp"
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace tcfp { namespace hardware {
enum class Operation { add, subtract, multiply, divide, square_root };
struct Result { uint64_t bits; uint8_t flags; };

/* CPU arithmetic is the default. Set TC_FLOATOPS_CPU=0 before starting the
   game to force SoftFloat; the selected backend is fixed for this process. */
inline bool enabled() {
    static const bool useCpu = [] {
        const char* value = std::getenv("TC_FLOATOPS_CPU");
        return !value || value[0] == '\0' || (value[0] == '1' && value[1] == '\0');
    }();
    return useCpu;
}

#if defined(__x86_64__) && defined(__GNUC__) && !defined(TC_FLOATOPS_SOFTWARE_ONLY)
inline bool controlFor(FPRounding rounding, unsigned* control) {
    unsigned rc = 0;
    switch (rounding) {
        case FPRounding::ties_away: return false;
        case FPRounding::toward_negative: rc = 1; break;
        case FPRounding::toward_positive: rc = 2; break;
        case FPRounding::toward_zero: rc = 3; break;
        case FPRounding::nearest_even: break;
    }
    /* Mask traps, clear sticky flags, disable FTZ/DAZ; preserve the caller's
       complete environment separately, including any unmasked exceptions. */
    *control = 0x1f80u | (rc << 13);
    return true;
}

inline bool ordinaryInput(uint64_t bits, uint64_t exponent, uint64_t fraction) {
    const uint64_t e = bits & exponent;
    return e != exponent && (e != 0 || (bits & fraction) == 0);
}

inline bool accept(uint64_t bits, uint64_t exponent, uint64_t fraction,
                   unsigned status, Result* out) {
    const uint64_t e = bits & exponent;
    if (e == exponent || e == fraction + 1 || (e == 0 && (bits & fraction)) ||
        (status & (0x01u | 0x04u | 0x08u | 0x10u))) return false;
    out->bits = bits;
    out->flags = (status & 0x20u) ? fp_flag_nx : 0;
    return true;
}

#define TC_FLOATOPS_SSE(instruction) \
    __asm__ __volatile__( \
        "stmxcsr %[saved]\n\t" \
        "ldmxcsr %[control]\n\t" \
        instruction " %[other], %[value]\n\t" \
        "stmxcsr %[status]\n\t" \
        "ldmxcsr %[saved]" \
        : [saved] "=&m" (saved), [status] "=&m" (status), [value] "+x" (value) \
        : [control] "m" (control), [other] "x" (other) : "memory")

inline bool evaluate32(Operation op, uint32_t a, uint32_t b,
                       FPRounding rounding, Result* out) {
    constexpr uint64_t exponent = UINT64_C(0x7f800000), fraction = UINT64_C(0x007fffff);
    unsigned control;
    if (!controlFor(rounding, &control) || !ordinaryInput(a, exponent, fraction) ||
        (op != Operation::square_root && !ordinaryInput(b, exponent, fraction))) return false;
    float value, other;
    if (op == Operation::square_root) b = a;
    std::memcpy(&value, &a, sizeof(value));
    std::memcpy(&other, &b, sizeof(other));
    unsigned saved, status;
    switch (op) {
        case Operation::add: TC_FLOATOPS_SSE("addss"); break;
        case Operation::subtract: TC_FLOATOPS_SSE("subss"); break;
        case Operation::multiply: TC_FLOATOPS_SSE("mulss"); break;
        case Operation::divide: TC_FLOATOPS_SSE("divss"); break;
        case Operation::square_root: TC_FLOATOPS_SSE("sqrtss"); break;
        default: return false;
    }
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return accept(bits, exponent, fraction, status, out);
}

inline bool evaluate64(Operation op, uint64_t a, uint64_t b,
                       FPRounding rounding, Result* out) {
    constexpr uint64_t exponent = UINT64_C(0x7ff0000000000000);
    constexpr uint64_t fraction = UINT64_C(0x000fffffffffffff);
    unsigned control;
    if (!controlFor(rounding, &control) || !ordinaryInput(a, exponent, fraction) ||
        (op != Operation::square_root && !ordinaryInput(b, exponent, fraction))) return false;
    double value, other;
    if (op == Operation::square_root) b = a;
    std::memcpy(&value, &a, sizeof(value));
    std::memcpy(&other, &b, sizeof(other));
    unsigned saved, status;
    switch (op) {
        case Operation::add: TC_FLOATOPS_SSE("addsd"); break;
        case Operation::subtract: TC_FLOATOPS_SSE("subsd"); break;
        case Operation::multiply: TC_FLOATOPS_SSE("mulsd"); break;
        case Operation::divide: TC_FLOATOPS_SSE("divsd"); break;
        case Operation::square_root: TC_FLOATOPS_SSE("sqrtsd"); break;
        default: return false;
    }
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return accept(bits, exponent, fraction, status, out);
}
#undef TC_FLOATOPS_SSE
#else
inline bool evaluate32(Operation, uint32_t, uint32_t, FPRounding, Result*) { return false; }
inline bool evaluate64(Operation, uint64_t, uint64_t, FPRounding, Result*) { return false; }
#endif
}} // namespace tcfp::hardware
#endif
