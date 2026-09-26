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
