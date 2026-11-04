/* Offline numeric test for the Float Ops binary32 kernel (plan 10.1).

   Every expectation comes from tests/data/float32/fp32-vectors.generated.hpp,
   which tools/float-vectors.py produces with an independent oracle: exact
   rational arithmetic in Python, cross-checked against CPython's double
   arithmetic for the round-to-nearest cases.  This file only compares bits and
   flags - it never computes an expected value itself, so a bug in the kernel
   cannot hide behind a matching bug in the test.

   Covered here:

   - all twelve operation entry points over the five rounding modes;
   - the five exception flags, bit by bit, next to every result;
   - the canonical-NaN policy (payloads dropped by arithmetic, preserved by the
     bit operations);
   - the flags belonging to a single call only (nothing is sticky);
   - SoftFloat's state being per-thread, which is what makes the kernel safe to
     call from the simulation thread and the UI thread at once.

   The release-candidate qualification with Berkeley TestFloat (plan 10.1) is a
   separate, heavier step; this test is the fast gate that runs on every build. */

#include "../examples/float-ops/fp/format.hpp"
#include "../examples/float-ops/fp/fp32.hpp"
#include "data/float32/fp32-vectors.generated.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using tcfp::FP32Relation;
using tcfp::FP32Result;
using tcfp::FPRounding;

constexpr int kMaxReportedFailures = 25;

struct Failure {
    std::string text;
};

std::string hex32(uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

FPRounding roundingFromCode(uint8_t code) {
    switch (code) {
        case 0: return FPRounding::nearest_even;
        case 1: return FPRounding::ties_away;
        case 2: return FPRounding::toward_zero;
        case 3: return FPRounding::toward_negative;
        case 4: return FPRounding::toward_positive;
    }
    return FPRounding::nearest_even;
}

const char* roundingName(uint8_t code) {
    switch (code) {
        case 0: return "RNE";
        case 1: return "RNA";
        case 2: return "RTZ";
        case 3: return "RDN";
        case 4: return "RUP";
    }
    return "???";
}

const char* opName(uint16_t op) {
    switch (op) {
        case tcfp_vectors::op_add: return "add";
        case tcfp_vectors::op_subtract: return "subtract";
        case tcfp_vectors::op_multiply: return "multiply";
        case tcfp_vectors::op_divide: return "divide";
        case tcfp_vectors::op_square_root: return "square_root";
        case tcfp_vectors::op_fused_multiply_add: return "fused_multiply_add";
        case tcfp_vectors::op_remainder: return "remainder";
        case tcfp_vectors::op_round_to_integral_exact: return "round_to_integral_exact";
        case tcfp_vectors::op_compare: return "compare";
        case tcfp_vectors::op_classify: return "classify";
        case tcfp_vectors::op_minimum_number: return "minimum_number";
        case tcfp_vectors::op_maximum_number: return "maximum_number";
    }
    return "?";
}

/* Runs one vector.  A vector's `bits` is the expected result pattern, except
   for compare (relation packed into four bits) and classify (ten-bit class
   word). */
struct VectorOutcome {
    uint32_t bits = 0;
    uint8_t flags = 0;
    bool flagsUsed = true;
};

VectorOutcome runVector(const tcfp_vectors::Vector& vector) {
    const FPRounding rounding = roundingFromCode(vector.rounding);
    VectorOutcome outcome{};
    switch (vector.op) {
        case tcfp_vectors::op_add: {
            const FP32Result result = tcfp::fp32_add(vector.a, vector.b, rounding);
            outcome.bits = result.bits;
            outcome.flags = result.flags;
            break;
        }
        case tcfp_vectors::op_subtract: {
            const FP32Result result = tcfp::fp32_subtract(vector.a, vector.b, rounding);
            outcome.bits = result.bits;
            outcome.flags = result.flags;
            break;
        }
        case tcfp_vectors::op_multiply: {
            const FP32Result result = tcfp::fp32_multiply(vector.a, vector.b, rounding);
            outcome.bits = result.bits;
            outcome.flags = result.flags;
            break;
        }
        case tcfp_vectors::op_divide: {
            const FP32Result result = tcfp::fp32_divide(vector.a, vector.b, rounding);
            outcome.bits = result.bits;
            outcome.flags = result.flags;
            break;
        }
        case tcfp_vectors::op_square_root: {
            const FP32Result result = tcfp::fp32_square_root(vector.a, rounding);
            outcome.bits = result.bits;
            outcome.flags = result.flags;
            break;
        }
        case tcfp_vectors::op_fused_multiply_add: {
            const FP32Result result =
                tcfp::fp32_fused_multiply_add(vector.a, vector.b, vector.c, rounding);
            outcome.bits = result.bits;
            outcome.flags = result.flags;
            break;
        }
        case tcfp_vectors::op_remainder: {
            const FP32Result result = tcfp::fp32_remainder(vector.a, vector.b);
            outcome.bits = result.bits;
            outcome.flags = result.flags;
            break;
        }
        case tcfp_vectors::op_round_to_integral_exact: {
            const FP32Result result =
                tcfp::fp32_round_to_integral_exact(vector.a, rounding);
            outcome.bits = result.bits;
            outcome.flags = result.flags;
            break;
        }
        case tcfp_vectors::op_compare: {
            const FP32Relation relation = tcfp::fp32_compare(vector.a, vector.b);
            uint32_t packed = 0;
            if (relation.lt) packed |= 1u << 0;
            if (relation.eq) packed |= 1u << 1;
            if (relation.gt) packed |= 1u << 2;
            if (relation.un) packed |= 1u << 3;
            outcome.bits = packed;
            outcome.flags = relation.flags;
            break;
        }
        case tcfp_vectors::op_classify:
            outcome.bits = tcfp::fp32_classify(vector.a);
            outcome.flagsUsed = false;
            break;
        case tcfp_vectors::op_minimum_number: {
            const FP32Result result = tcfp::fp32_minimum_number(vector.a, vector.b);
            outcome.bits = result.bits;
            outcome.flags = result.flags;
            break;
        }
        case tcfp_vectors::op_maximum_number: {
            const FP32Result result = tcfp::fp32_maximum_number(vector.a, vector.b);
            outcome.bits = result.bits;
            outcome.flags = result.flags;
            break;
        }
        default:
            break;
    }
    return outcome;
}

std::string describe(const tcfp_vectors::Vector& vector,
                     const VectorOutcome& outcome) {
    std::string text = opName(vector.op);
    text += " ";
    text += roundingName(vector.rounding);
    text += " a=" + hex32(vector.a) + " b=" + hex32(vector.b) + " c=" + hex32(vector.c);
    text += " expected=" + hex32(vector.bits) + "/0x";
    char flags[8];
    std::snprintf(flags, sizeof(flags), "%02X", vector.flags);
    text += flags;
    text += " got=" + hex32(outcome.bits);
    if (outcome.flagsUsed) {
        std::snprintf(flags, sizeof(flags), "%02X", outcome.flags);
        text += "/0x";
        text += flags;
    }
    return text;
}

bool runVectors(std::vector<std::string>& failures, int& checked, int& perOpCounts) {
    int opCounts[16] = {};
    for (size_t index = 0; index < tcfp_vectors::kFp32VectorCount; ++index) {
        const tcfp_vectors::Vector& vector = tcfp_vectors::kFp32Vectors[index];
        const VectorOutcome outcome = runVector(vector);
        ++checked;
        if (vector.op < 16) ++opCounts[vector.op];
        const bool bitsMatch = outcome.bits == vector.bits;
        const bool flagsMatch = !outcome.flagsUsed || outcome.flags == vector.flags;
        if (!bitsMatch || !flagsMatch) {
            if (static_cast<int>(failures.size()) < kMaxReportedFailures)
                failures.push_back(describe(vector, outcome));
        }
    }
    perOpCounts = 0;
    for (int op = 1; op <= 12; ++op) {
        if (!opCounts[op]) continue;
        std::printf("  %-26s %5d vector(s)\n", opName(static_cast<uint16_t>(op)),
                    opCounts[op]);
        perOpCounts += opCounts[op];
    }
    return failures.empty();
}

/* The plan fixes the NaN policy: arithmetic drops payloads, bit operations keep
   them.  These cases are not in the generated file because they are about the
   project's contract rather than about IEEE arithmetic. */
bool checkNanPolicy(std::vector<std::string>& failures) {
    const uint32_t quiet_payload = 0x7FC00001;
    const uint32_t quiet_negative = 0xFFC00000;
    struct ArithmeticCase {
        const char* name;
        FP32Result result;
    };
    const ArithmeticCase cases[] = {
        {"add(qNaN payload, 1.0)", tcfp::fp32_add(quiet_payload, 0x3F800000,
                                                  FPRounding::nearest_even)},
        {"multiply(qNaN payload, 2.0)", tcfp::fp32_multiply(quiet_payload, 0x40000000,
                                                            FPRounding::nearest_even)},
        {"divide(-qNaN, 2.0)", tcfp::fp32_divide(quiet_negative, 0x40000000,
                                                 FPRounding::nearest_even)},
        {"square_root(qNaN payload)", tcfp::fp32_square_root(quiet_payload,
                                                             FPRounding::nearest_even)},
        {"fma(qNaN payload, 2.0, 3.0)",
         tcfp::fp32_fused_multiply_add(quiet_payload, 0x40000000, 0x40400000,
                                       FPRounding::nearest_even)},
        {"remainder(qNaN payload, 3.0)", tcfp::fp32_remainder(quiet_payload, 0x40400000)},
        {"round_to_integral(qNaN payload)",
         tcfp::fp32_round_to_integral_exact(quiet_payload, FPRounding::nearest_even)},
    };
    for (const ArithmeticCase& item : cases) {
        if (item.result.bits != tcfp::kCanonicalNaN) {
            failures.push_back(std::string(item.name) + " did not canonicalize: " +
                               hex32(item.result.bits));
        }
        if (item.result.flags & tcfp::fp_flag_nv) {
            failures.push_back(std::string(item.name) + " raised invalid for a quiet NaN");
        }
    }
    if (tcfp::fp32_negate(quiet_payload) != (quiet_payload ^ tcfp::kSignMask)) {
        failures.push_back("negate did not preserve the payload");
    }
    if (tcfp::fp32_absolute(quiet_negative) != 0x7FC00000u) {
        /* absolute() only clears the sign bit, so this payload-free NaN is fine,
           but the negative payload case is the interesting one below. */
        failures.push_back("absolute changed more than the sign of a NaN");
    }
    if (tcfp::fp32_absolute(0xFFC00001u) != 0x7FC00001u) {
        failures.push_back("absolute did not preserve the payload");
    }
    if (tcfp::fp32_copy_sign(0x7FC00001u, 0x80000000u) != 0xFFC00001u) {
        failures.push_back("copy_sign did not preserve the payload");
    }
    return failures.empty();
}

/* Flags belong to one call: a divide-by-zero must not colour the next exact op,
   and an operation that raises nothing must report zero. */
bool checkFlagIsolation(std::vector<std::string>& failures) {
    const FP32Result division = tcfp::fp32_divide(0x3F800000, 0x00000000,
                                                  FPRounding::nearest_even);
    if (division.flags != tcfp::fp_flag_dz) {
        failures.push_back("1.0/0.0 did not report exactly DZ: " +
                           hex32(division.flags));
    }
    const FP32Result exact = tcfp::fp32_add(0x3F800000, 0x3F800000,
                                            FPRounding::nearest_even);
    if (exact.flags != 0 || exact.bits != 0x40000000u) {
        failures.push_back("the flag word is sticky: 1.0+1.0 reported " +
                           hex32(exact.flags));
    }
    const FP32Result second = tcfp::fp32_divide(0x3F800000, 0x00000000,
                                                FPRounding::nearest_even);
    if (second.flags != tcfp::fp_flag_dz) {
        failures.push_back("a repeated 1.0/0.0 lost its DZ flag");
    }
    return failures.empty();
}

/* One rounding mode per call, whatever the caller did before. */
bool checkModeIsolation(std::vector<std::string>& failures) {
    const uint32_t one = 0x3F800000;
    const uint32_t above_one = one + 1; /* 1 + 2**-23 */
    const uint32_t half_ulp = 0x33000000; /* 2**-25 */
    const uint32_t sum = tcfp::fp32_add(one, half_ulp, FPRounding::toward_zero).bits;
    if (sum != one)
        failures.push_back("toward_zero did not truncate: " + hex32(sum));
    const uint32_t up = tcfp::fp32_add(one, half_ulp, FPRounding::toward_positive).bits;
    if (up != above_one)
        failures.push_back("toward_positive did not round up: " + hex32(up));
    /* The same call again, after a different mode, has to use its own mode. */
    const uint32_t again = tcfp::fp32_add(one, half_ulp, FPRounding::toward_zero).bits;
    if (again != one)
        failures.push_back("a previous rounding mode leaked: " + hex32(again));
    return failures.empty();
}

/* SoftFloat's rounding mode and flag word are per-thread (platform.h defines
   THREAD_LOCAL).  If that ever stopped being true, two threads using different
   modes would corrupt each other's results with high probability. */
bool checkThreadIsolation(std::vector<std::string>& failures) {
    constexpr int kIterations = 200000;
    const uint32_t one = 0x3F800000;
    const uint32_t half_ulp = 0x33000000;
    std::atomic<int> mismatches{0};
    auto worker = [&](FPRounding rounding, uint32_t expected) {
        for (int index = 0; index < kIterations; ++index) {
            if (tcfp::fp32_add(one, half_ulp, rounding).bits != expected) {
                mismatches.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    };
    std::thread down(worker, FPRounding::toward_zero, one);
    std::thread up(worker, FPRounding::toward_positive, one + 1);
    down.join();
    up.join();
    if (mismatches.load() != 0) {
        failures.push_back("two threads corrupted each other's rounding mode: " +
                           std::to_string(mismatches.load()) + " mismatch(es)");
        return false;
    }
    return true;
}

/* Integer conversion (plan 2.6, decision D2).  TestFloat does not cover these
   project decisions, so the policy the plan fixes is written out here instead
   of being read out of a library: saturating bounds, which inputs raise NV,
   which raise only NX, and that a negative zero converts without any flag. */
bool checkConversions(std::vector<std::string>& failures) {
    auto expectBits = [&](const char* name, uint32_t actual, uint32_t expected) {
        if (actual != expected)
            failures.push_back(std::string(name) + ": got " + hex32(actual) + ", want " +
                               hex32(expected));
    };
    auto expectFlags = [&](const char* name, uint8_t actual, uint8_t expected) {
        if (actual != expected)
            failures.push_back(std::string(name) + ": flags " + hex32(actual) + ", want " +
                               hex32(expected));
    };

    /* Integer to binary32.  Every integer is a legal input, so NV never
       appears; a value that cannot be represented exactly reports NX and takes
       the instance's rounding mode. */
    struct ToFloatCase {
        const char* name;
        int32_t input;
        FPRounding rounding;
        uint32_t bits;
        uint8_t flags;
    };
    const ToFloatCase toFloat[] = {
        {"i32 0", 0, FPRounding::nearest_even, 0x00000000u, 0},
        {"i32 1", 1, FPRounding::nearest_even, 0x3F800000u, 0},
        {"i32 -1", -1, FPRounding::nearest_even, 0xBF800000u, 0},
        {"i32 2^24", 16777216, FPRounding::nearest_even, 0x4B800000u, 0},
        /* 2^24+1 is the first integer without an exact binary32 form. */
        {"i32 2^24+1 (RNE)", 16777217, FPRounding::nearest_even, 0x4B800000u,
         tcfp::fp_flag_nx},
        {"i32 2^24+1 (RUP)", 16777217, FPRounding::toward_positive, 0x4B800001u,
         tcfp::fp_flag_nx},
        {"i32 INT32_MAX (RNE)", INT32_MAX, FPRounding::nearest_even, 0x4F000000u,
         tcfp::fp_flag_nx},
        {"i32 INT32_MIN", INT32_MIN, FPRounding::nearest_even, 0xCF000000u, 0},
    };
    for (const ToFloatCase& item : toFloat) {
        const FP32Result result = tcfp::fp32_from_i32(item.input, item.rounding);
        expectBits((std::string(item.name) + " bits").c_str(), result.bits, item.bits);
        expectFlags((std::string(item.name) + " flags").c_str(), result.flags, item.flags);
    }
    expectBits("u32 UINT32_MAX (RNE)", tcfp::fp32_from_u32(0xFFFFFFFFu, FPRounding::nearest_even).bits,
               0x4F800000u);
    expectFlags("u32 UINT32_MAX (RNE) flags",
                tcfp::fp32_from_u32(0xFFFFFFFFu, FPRounding::nearest_even).flags,
                tcfp::fp_flag_nx);
    expectBits("u32 UINT32_MAX (RTZ)", tcfp::fp32_from_u32(0xFFFFFFFFu, FPRounding::toward_zero).bits,
               0x4F7FFFFFu);
    expectBits("u32 2^31", tcfp::fp32_from_u32(0x80000000u, FPRounding::nearest_even).bits,
               0x4F000000u);

    /* Binary32 to signed integer: in-range values round per the mode and report
       NX when the value changed; NaN, infinities and out-of-range values report
       NV and land on the saturated bound. */
    struct ToIntCase {
        const char* name;
        uint32_t input;
        FPRounding rounding;
        uint32_t value;
        uint8_t flags;
    };
    const ToIntCase toInt[] = {
        {"i32(1.5) RNE", 0x3FC00000u, FPRounding::nearest_even, 2u, tcfp::fp_flag_nx},
        {"i32(1.5) RNA", 0x3FC00000u, FPRounding::ties_away, 2u, tcfp::fp_flag_nx},
        {"i32(1.5) RTZ", 0x3FC00000u, FPRounding::toward_zero, 1u, tcfp::fp_flag_nx},
        {"i32(1.5) RDN", 0x3FC00000u, FPRounding::toward_negative, 1u, tcfp::fp_flag_nx},
        {"i32(1.5) RUP", 0x3FC00000u, FPRounding::toward_positive, 2u, tcfp::fp_flag_nx},
        {"i32(-2.5) RNE", 0xC0200000u, FPRounding::nearest_even, 0xFFFFFFFEu,
         tcfp::fp_flag_nx},
        {"i32(-2.5) RDN", 0xC0200000u, FPRounding::toward_negative, 0xFFFFFFFDu,
         tcfp::fp_flag_nx},
        {"i32(2^30)", 0x4E800000u, FPRounding::nearest_even, 0x40000000u, 0},
        {"i32(-0)", 0x80000000u, FPRounding::nearest_even, 0u, 0},
        {"i32(-2^31)", 0xCF000000u, FPRounding::nearest_even, 0x80000000u, 0},
        {"i32(2^31)", 0x4F000000u, FPRounding::nearest_even, 0x7FFFFFFFu,
         tcfp::fp_flag_nv},
        {"i32(-2^31-128)", 0xCF000080u, FPRounding::nearest_even, 0x80000000u,
         tcfp::fp_flag_nv},
        {"i32(NaN)", 0x7FC00001u, FPRounding::nearest_even, 0x7FFFFFFFu,
         tcfp::fp_flag_nv},
        {"i32(+inf)", 0x7F800000u, FPRounding::nearest_even, 0x7FFFFFFFu,
         tcfp::fp_flag_nv},
        {"i32(-inf)", 0xFF800000u, FPRounding::nearest_even, 0x80000000u,
         tcfp::fp_flag_nv},
    };
    for (const ToIntCase& item : toInt) {
        const tcfp::FP32IntResult result = tcfp::fp32_to_i32(item.input, item.rounding);
        expectBits((std::string(item.name) + " value").c_str(), result.value, item.value);
        expectFlags((std::string(item.name) + " flags").c_str(), result.flags, item.flags);
    }

    /* Binary32 to unsigned integer: a negative non-zero value is out of range,
       a negative zero is not (plan 2.6). */
    const ToIntCase toUnsigned[] = {
        {"u32(1.5) RNE", 0x3FC00000u, FPRounding::nearest_even, 2u, tcfp::fp_flag_nx},
        {"u32(4294967040)", 0x4F7FFFFFu, FPRounding::nearest_even, 4294967040u, 0},
        {"u32(-0)", 0x80000000u, FPRounding::nearest_even, 0u, 0},
        {"u32(-1)", 0xBF800000u, FPRounding::nearest_even, 0u, tcfp::fp_flag_nv},
        {"u32(2^32)", 0x4F800000u, FPRounding::nearest_even, 0xFFFFFFFFu,
         tcfp::fp_flag_nv},
        {"u32(NaN)", 0x7FC00000u, FPRounding::nearest_even, 0xFFFFFFFFu,
         tcfp::fp_flag_nv},
        {"u32(+inf)", 0x7F800000u, FPRounding::nearest_even, 0xFFFFFFFFu,
         tcfp::fp_flag_nv},
        {"u32(-inf)", 0xFF800000u, FPRounding::nearest_even, 0u, tcfp::fp_flag_nv},
    };
    for (const ToIntCase& item : toUnsigned) {
        const tcfp::FP32IntResult result = tcfp::fp32_to_u32(item.input, item.rounding);
        expectBits((std::string(item.name) + " value").c_str(), result.value, item.value);
        expectFlags((std::string(item.name) + " flags").c_str(), result.flags, item.flags);
    }

    /* Round trip: every integer a conversion can represent exactly has to come
       back through the other direction unchanged. */
    const int32_t roundTrip[] = {0, 1, -1, 255, -255, 65535, -65536, 16777216, -16777216,
                                 2147483520, -2147483648};
    for (int32_t value : roundTrip) {
        const FP32Result forward = tcfp::fp32_from_i32(value, FPRounding::nearest_even);
        const tcfp::FP32IntResult back = tcfp::fp32_to_i32(forward.bits,
                                                           FPRounding::nearest_even);
        if (back.value != static_cast<uint32_t>(value) || back.flags != 0)
            failures.push_back("i32 round trip changed " + std::to_string(value) +
                               " into " + std::to_string(static_cast<int32_t>(back.value)));
    }
    return failures.empty();
}

/* The width table (fp/format.hpp): the same twelve operations in binary16,
   binary32 and binary64 behind one entry point.  Every expectation below is a
   value the standard fixes - 1/3 and 0.1+0.2 in their own formats, the
   saturated integer bounds of decision D2, the signed-zero rule of the 2019
   minimumNumber/maximumNumber pair - so this file still never computes an
   expected result itself.

   The binary32 rows double as the agreement check: this table delegates
   binary32 to fp32.cpp, and the two must answer with the same bits and the
   same flags. */
void checkWidthTable(std::vector<std::string>& failures) {
    auto expectBits = [&failures](const std::string& name, uint64_t actual, uint64_t wanted) {
        if (actual != wanted) {
            char buffer[192];
            std::snprintf(buffer, sizeof(buffer), "width table: %s gave 0x%llX, wanted 0x%llX",
                          name.c_str(), static_cast<unsigned long long>(actual),
                          static_cast<unsigned long long>(wanted));
            failures.push_back(buffer);
        }
    };
    auto expectFlags = [&failures](const std::string& name, uint8_t actual, uint8_t wanted) {
        if (actual != wanted) {
            char buffer[192];
            std::snprintf(buffer, sizeof(buffer), "width table: %s flags 0x%02X, wanted 0x%02X",
                          name.c_str(), actual, wanted);
            failures.push_back(buffer);
        }
    };
    auto expectBool = [&failures](const std::string& name, bool actual, bool wanted) {
        if (actual != wanted)
            failures.push_back("width table: " + name + (actual ? " held" : " did not hold"));
    };

    /* Only the three widths exist; anything else is refused rather than
       quietly treated as binary32. */
    for (uint32_t format : {0u, 8u, 24u, 128u})
        expectBool(std::to_string(format) + " is not a format",
                   tcfp::formatOps(format) == nullptr, true);
    for (uint32_t format : {16u, 32u, 64u}) {
        const tcfp::FormatOps* ops = tcfp::formatOps(format);
        expectBool(std::to_string(format) + " has an implementation", ops != nullptr, true);
        if (!ops) continue;
        expectBits(std::to_string(format) + " reports its own width",
                   ops->layout->width, format);
    }
    expectBits("the width box cycles 16->32", tcfp::nextFormat(16), 32);
    expectBits("the width box cycles 32->64", tcfp::nextFormat(32), 64);
    expectBits("the width box cycles 64->16", tcfp::nextFormat(64), 16);

    /* ---- binary16 ---- */
    const tcfp::FormatOps& f16 = *tcfp::formatOps(16);
    tcfp::FormatResult result = f16.add(0x3C00, 0x3C00, tcfp::FPRounding::nearest_even);
    expectBits("f16 1+1", result.bits, 0x4000);
    expectFlags("f16 1+1", result.flags, 0);
    result = f16.subtract(0x3C00, 0x3C00, tcfp::FPRounding::nearest_even);
    expectBits("f16 1-1", result.bits, 0x0000);
    expectFlags("f16 1-1", result.flags, 0);
    result = f16.divide(0x3C00, 0x4200, tcfp::FPRounding::nearest_even);
    expectBits("f16 1/3", result.bits, 0x3555);
    expectFlags("f16 1/3", result.flags, tcfp::fp_flag_nx);
    result = f16.square_root(0x4400, tcfp::FPRounding::nearest_even);
    expectBits("f16 sqrt(4)", result.bits, 0x4000);
    expectFlags("f16 sqrt(4)", result.flags, 0);
    /* The largest finite binary16 doubled overflows to the default NaN's
       neighbour, the infinity, and says so. */
    result = f16.add(0x7BFF, 0x7BFF, tcfp::FPRounding::nearest_even);
    expectBits("f16 65504+65504", result.bits, 0x7C00);
    expectFlags("f16 65504+65504", result.flags, tcfp::fp_flag_of | tcfp::fp_flag_nx);
    /* An exact subnormal result is not an underflow (tininess after rounding,
       and the flag asks for an inexact result too). */
    result = f16.multiply(0x0400, 0x3800, tcfp::FPRounding::nearest_even);
    expectBits("f16 2^-14 * 0.5", result.bits, 0x0200);
    expectFlags("f16 2^-14 * 0.5", result.flags, 0);
    result = f16.fused_multiply_add(0x3C00, 0x4000, 0xC000, tcfp::FPRounding::nearest_even);
    expectBits("f16 1*2 + -2", result.bits, 0x0000);
    expectFlags("f16 1*2 + -2", result.flags, 0);
    result = f16.round_to_integral_exact(0x3E00, tcfp::FPRounding::nearest_even);
    expectBits("f16 round(1.5) RNE", result.bits, 0x4000);
    expectFlags("f16 round(1.5) RNE", result.flags, tcfp::fp_flag_nx);
    result = f16.remainder(0x4500, 0x4000);
    expectBits("f16 5 rem 2", result.bits, 0x3C00);
    expectFlags("f16 5 rem 2", result.flags, 0);
    /* Arithmetic never carries a NaN payload (plan 2.4). */
    result = f16.add(0x7E01, 0x3C00, tcfp::FPRounding::nearest_even);
    expectBits("f16 qNaN + 1 is canonical", result.bits, 0x7E00);
    expectFlags("f16 qNaN + 1 is canonical", result.flags, 0);

    const tcfp::FormatRelation relation = f16.compare(0x8000, 0x0000);
    expectBool("f16 -0 == +0", relation.eq, true);
    expectBool("f16 -0 == +0 is ordered", relation.un, false);
    const tcfp::FormatRelation unordered = f16.compare(0x7C01, 0x3C00);
    expectBool("f16 sNaN is unordered", unordered.un, true);
    expectFlags("f16 sNaN signals", unordered.flags, tcfp::fp_flag_nv);
    expectBits("f16 classify -0", f16.classify(0x8000), tcfp::fp_class_negative_zero);
    expectBits("f16 classify 65504", f16.classify(0x7BFF), tcfp::fp_class_positive_normal);
    expectBits("f16 classify subnormal", f16.classify(0x0001),
               tcfp::fp_class_positive_subnormal);
    expectBits("f16 classify qNaN", f16.classify(0x7E00), tcfp::fp_class_quiet_nan);
    expectBool("f16 is_nan(qNaN)", f16.is_nan(0x7E00), true);
    expectBool("f16 is_signaling_nan(sNaN)", f16.is_signaling_nan(0x7C01), true);
    expectBool("f16 is_signaling_nan(qNaN)", f16.is_signaling_nan(0x7E00), false);
    expectBool("f16 is_infinite(+inf)", f16.is_infinite(0x7C00), true);
    expectBool("f16 is_subnormal(min subnormal)", f16.is_subnormal(0x0001), true);
    expectBool("f16 is_negative(-1)", f16.is_negative(0xBC00), true);
    expectBits("f16 negate(1)", f16.negate(0x3C00), 0xBC00);
    expectBits("f16 absolute(-1)", f16.absolute(0xBC00), 0x3C00);
    expectBits("f16 copy_sign(1,-0)", f16.copy_sign(0x3C00, 0x8000), 0xBC00);
    expectBits("f16 minimum(-0,+0)", f16.minimum_number(0x8000, 0x0000).bits, 0x8000);
    expectBits("f16 maximum(-0,+0)", f16.maximum_number(0x8000, 0x0000).bits, 0x0000);
    expectBits("f16 mask keeps 16 bits", f16.mask(UINT64_C(0x1234FFFF)), 0xFFFF);

    /* The integer conversions, including the two directions of decision D2. */
    result = f16.from_i32(-3, tcfp::FPRounding::nearest_even);
    expectBits("f16 from -3", result.bits, 0xC200);
    expectFlags("f16 from -3", result.flags, 0);
    result = f16.from_u32(1000, tcfp::FPRounding::nearest_even);
    expectBits("f16 from 1000", result.bits, 0x63D0);
    expectFlags("f16 from 1000", result.flags, 0);
    tcfp::FormatIntResult integer = f16.to_i32(0x4000, tcfp::FPRounding::nearest_even);
    expectBits("f16 2 -> i32", integer.value, 2);
    expectFlags("f16 2 -> i32", integer.flags, 0);
    /* Every finite binary16 fits an int32, so only a NaN or an infinity can be
       out of range here. */
    integer = f16.to_i32(0x7BFF, tcfp::FPRounding::nearest_even);
    expectBits("f16 65504 -> i32", integer.value, 65504);
    expectFlags("f16 65504 -> i32", integer.flags, 0);
    integer = f16.to_i32(0x7E00, tcfp::FPRounding::nearest_even);
    expectBits("f16 NaN -> i32", integer.value, UINT64_C(0x7FFFFFFF));
    expectFlags("f16 NaN -> i32", integer.flags, tcfp::fp_flag_nv);
    integer = f16.to_i32(0xFC00, tcfp::FPRounding::nearest_even);
    expectBits("f16 -inf -> i32", integer.value, UINT64_C(0x80000000));
    expectFlags("f16 -inf -> i32", integer.flags, tcfp::fp_flag_nv);
    integer = f16.to_u32(0xBC00, tcfp::FPRounding::nearest_even);
    expectBits("f16 -1 -> u32", integer.value, 0);
    expectFlags("f16 -1 -> u32", integer.flags, tcfp::fp_flag_nv);
    integer = f16.to_u32(0x8000, tcfp::FPRounding::nearest_even);
    expectBits("f16 -0 -> u32", integer.value, 0);
    expectFlags("f16 -0 -> u32", integer.flags, 0);

    /* ---- binary64 ---- */
    const tcfp::FormatOps& f64 = *tcfp::formatOps(64);
    result = f64.add(UINT64_C(0x3FF0000000000000), UINT64_C(0x3FF0000000000000),
                     tcfp::FPRounding::nearest_even);
    expectBits("f64 1+1", result.bits, UINT64_C(0x4000000000000000));
    expectFlags("f64 1+1", result.flags, 0);
    result = f64.add(UINT64_C(0x3FB999999999999A), UINT64_C(0x3FC999999999999A),
                     tcfp::FPRounding::nearest_even);
    expectBits("f64 0.1+0.2", result.bits, UINT64_C(0x3FD3333333333334));
    expectFlags("f64 0.1+0.2", result.flags, tcfp::fp_flag_nx);
    result = f64.divide(UINT64_C(0x3FF0000000000000), UINT64_C(0x4008000000000000),
                        tcfp::FPRounding::nearest_even);
    expectBits("f64 1/3", result.bits, UINT64_C(0x3FD5555555555555));
    expectFlags("f64 1/3", result.flags, tcfp::fp_flag_nx);
    result = f64.square_root(UINT64_C(0x4000000000000000), tcfp::FPRounding::nearest_even);
    expectBits("f64 sqrt(2)", result.bits, UINT64_C(0x3FF6A09E667F3BCD));
    expectFlags("f64 sqrt(2)", result.flags, tcfp::fp_flag_nx);
    result = f64.multiply(UINT64_C(0x3FF0000000000000), UINT64_C(0x0000000000000001),
                          tcfp::FPRounding::nearest_even);
    expectBits("f64 1 * min subnormal", result.bits, UINT64_C(0x0000000000000001));
    expectFlags("f64 1 * min subnormal", result.flags, 0);
    expectBits("f64 classify -inf", f64.classify(UINT64_C(0xFFF0000000000000)),
               tcfp::fp_class_negative_infinity);
    expectBits("f64 classify max finite", f64.classify(UINT64_C(0x7FEFFFFFFFFFFFFF)),
               tcfp::fp_class_positive_normal);
    const tcfp::FormatRelation wide = f64.compare(UINT64_C(0x3FF0000000000000),
                                                  UINT64_C(0x4000000000000000));
    expectBool("f64 1 < 2", wide.lt, true);
    expectBits("f64 negate(1)", f64.negate(UINT64_C(0x3FF0000000000000)),
               UINT64_C(0xBFF0000000000000));
    expectBits("f64 minimum(-0,+0)", f64.minimum_number(0, UINT64_C(0x8000000000000000)).bits,
               UINT64_C(0x8000000000000000));
    expectBits("f64 maximum(-0,+0)", f64.maximum_number(0, UINT64_C(0x8000000000000000)).bits, 0);

    result = f64.from_i32(-1, tcfp::FPRounding::nearest_even);
    expectBits("f64 from -1", result.bits, UINT64_C(0xBFF0000000000000));
    expectFlags("f64 from -1", result.flags, 0);
    result = f64.from_u32(0xFFFFFFFFu, tcfp::FPRounding::nearest_even);
    expectBits("f64 from 4294967295", result.bits, UINT64_C(0x41EFFFFFFFE00000));
    expectFlags("f64 from 4294967295", result.flags, 0);
    /* Here the int32 range *is* reachable, so the saturating policy shows. */
    integer = f64.to_i32(UINT64_C(0x41E0000000000000), tcfp::FPRounding::nearest_even);
    expectBits("f64 2^31 -> i32", integer.value, UINT64_C(0x7FFFFFFF));
    expectFlags("f64 2^31 -> i32", integer.flags, tcfp::fp_flag_nv);
    integer = f64.to_i32(UINT64_C(0xC1E0000000000000), tcfp::FPRounding::nearest_even);
    expectBits("f64 -2^31 -> i32", integer.value, UINT64_C(0x80000000));
    expectFlags("f64 -2^31 -> i32", integer.flags, 0);
    integer = f64.to_i32(UINT64_C(0xC1E0000000000001), tcfp::FPRounding::nearest_even);
    expectBits("f64 below -2^31 -> i32", integer.value, UINT64_C(0x80000000));
    expectFlags("f64 below -2^31 -> i32", integer.flags, tcfp::fp_flag_nv);
    integer = f64.to_u32(UINT64_C(0x41EFFFFFFFE00000), tcfp::FPRounding::nearest_even);
    expectBits("f64 4294967295 -> u32", integer.value, UINT64_C(0xFFFFFFFF));
    expectFlags("f64 4294967295 -> u32", integer.flags, 0);
    integer = f64.to_u32(UINT64_C(0x41F0000000000000), tcfp::FPRounding::nearest_even);
    expectBits("f64 2^32 -> u32", integer.value, UINT64_C(0xFFFFFFFF));
    expectFlags("f64 2^32 -> u32", integer.flags, tcfp::fp_flag_nv);

    /* Split Bits and Make Bits are inverses in every width, which is what makes
       one component definition able to serve all three. */
    const uint64_t patterns16[] = {0x3C00, 0x3555, 0x8001, 0x7E00, 0xFC00};
    for (uint64_t pattern : patterns16) {
        const tcfp::FormatFields fields = f16.split(pattern);
        expectBits("f16 make(split(x))", f16.make_bits(fields.sign, fields.exponent,
                                                      fields.significand),
                   pattern);
    }
    const uint64_t patterns64[] = {UINT64_C(0x3FF0000000000000), UINT64_C(0x8000000000000001),
                                   UINT64_C(0x7FF8000000000000),
                                   UINT64_C(0xFFF0000000000000)};
    for (uint64_t pattern : patterns64) {
        const tcfp::FormatFields fields = f64.split(pattern);
        expectBits("f64 make(split(x))", f64.make_bits(fields.sign, fields.exponent,
                                                      fields.significand),
                   pattern);
    }

    /* ---- binary32 delegates, so the two must agree exactly ---- */
    const tcfp::FormatOps& f32 = *tcfp::formatOps(32);
    struct Agreement {
        uint32_t a;
        uint32_t b;
        tcfp::FPRounding rounding;
    };
    const Agreement additions[] = {
        {0x3F800000u, 0x3F800000u, tcfp::FPRounding::nearest_even},
        {0x3F800000u, 0x33800000u, tcfp::FPRounding::nearest_even},
        {0x7F7FFFFFu, 0x7F7FFFFFu, tcfp::FPRounding::nearest_even},
        {0x3F800000u, 0xBF800000u, tcfp::FPRounding::toward_positive},
        {0x7FC00001u, 0x3F800000u, tcfp::FPRounding::nearest_even},
        {0x00000001u, 0x00000001u, tcfp::FPRounding::nearest_even},
    };
    for (const Agreement& item : additions) {
        const FP32Result expected = tcfp::fp32_add(item.a, item.b, item.rounding);
        const tcfp::FormatResult actual = f32.add(item.a, item.b, item.rounding);
        expectBits("f32 agrees with fp32.cpp", actual.bits, expected.bits);
        expectFlags("f32 agrees with fp32.cpp", actual.flags, expected.flags);
    }
    const uint32_t classifyPatterns[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x7F800000u,
                                         0xFF800000u, 0x7FC00000u, 0x7F800001u, 0x3F800000u};
    for (uint32_t pattern : classifyPatterns)
        expectBits("f32 classify agrees with fp32.cpp", f32.classify(pattern),
                   tcfp::fp32_classify(pattern));
    const uint32_t comparePatterns[][2] = {
        {0x3F800000u, 0x40000000u}, {0x80000000u, 0x00000000u}, {0x7FC00000u, 0x3F800000u},
        {0xBF800000u, 0xBF7FFFFFu}, {0x7F800001u, 0x7FC00000u},
    };
    for (const auto& pair : comparePatterns) {
        const FP32Relation expected = tcfp::fp32_compare(pair[0], pair[1]);
        const tcfp::FormatRelation actual = f32.compare(pair[0], pair[1]);
        expectBool("f32 compare agrees with fp32.cpp (lt)", actual.lt == expected.lt, true);
        expectBool("f32 compare agrees with fp32.cpp (eq)", actual.eq == expected.eq, true);
        expectBool("f32 compare agrees with fp32.cpp (gt)", actual.gt == expected.gt, true);
        expectBool("f32 compare agrees with fp32.cpp (un)", actual.un == expected.un, true);
        expectFlags("f32 compare agrees with fp32.cpp (flags)", actual.flags, expected.flags);
    }
    const int32_t integerPatterns[] = {0, 1, -1, 16777216, -2147483647 - 1, 2147483520};
    for (int32_t value : integerPatterns) {
        const tcfp::FormatResult actual = f32.from_i32(value, tcfp::FPRounding::nearest_even);
        const FP32Result expected = tcfp::fp32_from_i32(value, tcfp::FPRounding::nearest_even);
        expectBits("f32 from_i32 agrees with fp32.cpp", actual.bits, expected.bits);
        expectFlags("f32 from_i32 agrees with fp32.cpp", actual.flags, expected.flags);
    }
}

}  // namespace

int main() {
    std::vector<std::string> failures;
    int checked = 0;
    int perOp = 0;

    std::printf("float-ops binary32 kernel: %zu golden vectors\n",
                tcfp_vectors::kFp32VectorCount);
    runVectors(failures, checked, perOp);
    checkNanPolicy(failures);
    checkFlagIsolation(failures);
    checkModeIsolation(failures);
    checkConversions(failures);
    checkWidthTable(failures);
    const bool threadsOk = checkThreadIsolation(failures);

    if (!failures.empty()) {
        for (const std::string& failure : failures)
            std::printf("FAIL %s\n", failure.c_str());
        if (static_cast<int>(failures.size()) >= kMaxReportedFailures)
            std::printf("     (report truncated at %d failures)\n", kMaxReportedFailures);
        std::printf("FAIL float-ops binary32 kernel: %zu failing check(s) over %d vectors\n",
                    failures.size(), checked);
        return 1;
    }

    std::printf("PASS float-ops binary32 kernel: %d vectors matched bit for bit "
                "(values and the five flags) across 12 operations and 5 rounding "
                "modes; canonical NaN, per-call flags and per-call rounding hold; "
                "SoftFloat state is per-thread (%s)\n",
                checked, threadsOk ? "two threads, 200000 operations each" : "not checked");
    return 0;
}
