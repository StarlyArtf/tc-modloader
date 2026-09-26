/* Offline test for the Float Ops text layer (plan 5.3).

   Expectations come from tests/data/float32/decimal-vectors.generated.hpp,
   which tools/float-decimal-vectors.py produces from exact decimal arithmetic
   (parsing) and from the shortest-round-trip search (formatting).  Neither the
   generator nor this test calls fast_float or Ryu for the expectation, so the
   two implementations have to agree on their own.

   Besides the vectors this pins the contract the editors rely on:

   - illegal text is refused (ok == false) so a commit can keep the old value;
   - `bits:` stays exact and reports no diagnostics;
   - formatting and parsing round-trip for pseudo-random patterns, subnormals
     and specials included;
   - a formatter never writes past the buffer it was given. */

#include "../examples/float-ops/fp/decimal.hpp"
#include "data/float32/decimal-vectors.generated.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using tcfp::DecimalParse;
using tcfp::DisplayMode;

constexpr int kMaxReportedFailures = 25;

std::string hex32(uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

void checkParseVectors(std::vector<std::string>& failures, int& checked) {
    for (size_t index = 0; index < tcfp_decimal_vectors::kParseVectorCount; ++index) {
        const auto& vector = tcfp_decimal_vectors::kParseVectors[index];
        const DecimalParse parsed = tcfp::parse_decimal(vector.text);
        ++checked;
        bool ok = parsed.ok == vector.ok;
        if (ok && vector.ok) {
            ok = parsed.bits == vector.bits && parsed.diagnostics == vector.diagnostics;
        }
        if (ok) continue;
        if (static_cast<int>(failures.size()) >= kMaxReportedFailures) continue;
        char detail[256] = {};
        std::snprintf(detail, sizeof(detail),
                      "parse \"%s\": expected ok=%d bits=0x%08X diag=0x%02X, "
                      "got ok=%d bits=0x%08X diag=0x%02X",
                      vector.text, vector.ok ? 1 : 0, vector.bits, vector.diagnostics,
                      parsed.ok ? 1 : 0, parsed.bits, parsed.diagnostics);
        failures.push_back(detail);
    }
}

void checkFormatVectors(std::vector<std::string>& failures, int& checked) {
    for (size_t index = 0; index < tcfp_decimal_vectors::kFormatVectorCount; ++index) {
        const auto& vector = tcfp_decimal_vectors::kFormatVectors[index];
        char shortest[64] = {};
        char scientific[64] = {};
        char hex[32] = {};
        char combined[96] = {};
        tcfp::format_shortest(vector.bits, shortest, sizeof(shortest));
        tcfp::format_scientific(vector.bits, scientific, sizeof(scientific));
        tcfp::format_hex(vector.bits, hex, sizeof(hex));
        tcfp::format_value(vector.bits, DisplayMode::decimal_and_hex, combined,
                           sizeof(combined));
        ++checked;
        const std::string expected_combined =
            std::string(vector.shortest) + " " + vector.hex;
        if (std::strcmp(shortest, vector.shortest) == 0 &&
            std::strcmp(scientific, vector.scientific) == 0 &&
            std::strcmp(hex, vector.hex) == 0 &&
            std::string(combined) == expected_combined)
            continue;
        if (static_cast<int>(failures.size()) >= kMaxReportedFailures) continue;
        char detail[320] = {};
        std::snprintf(detail, sizeof(detail),
                      "format 0x%08X: expected \"%s\"/\"%s\"/\"%s\", "
                      "got \"%s\"/\"%s\"/\"%s\"",
                      vector.bits, vector.shortest, vector.scientific, vector.hex,
                      shortest, scientific, hex);
        failures.push_back(detail);
    }
}

uint32_t nextRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/* The formatter and the parser are two libraries; this checks the pair on
   patterns the vector file never names.  NaN payloads are the one part of the
   domain that cannot round-trip through text - every NaN formats as "nan" -
   so they are checked by the contract section instead, and `bits:` is the form
   that carries a payload. */
void checkRoundTrip(std::vector<std::string>& failures, int& checked) {
    uint32_t state = 0x9E3779B9u;
    for (int index = 0; index < 20000; ++index) {
        const uint32_t bits = nextRandom(state);
        if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0)
            continue; /* NaN: lossy by design */
        char text[64] = {};
        tcfp::format_shortest(bits, text, sizeof(text));
        const DecimalParse parsed = tcfp::parse_decimal(text);
        ++checked;
        if (parsed.ok && parsed.bits == bits) continue;
        if (static_cast<int>(failures.size()) >= kMaxReportedFailures) continue;
        failures.push_back("shortest round-trip failed: " + hex32(bits) + " -> \"" +
                           text + "\" -> " + hex32(parsed.bits));
    }
}

void checkContract(std::vector<std::string>& failures) {
    /* Illegal text is refused so an editor can keep the previous value. */
    for (const char* text : {"", "  ", "3.5x", "0x40", "1e", "nanx", "bits:0x123456789",
                             "bits:", "1.2.3"}) {
        if (tcfp::parse_decimal(text).ok)
            failures.push_back(std::string("accepted illegal text \"") + text + "\"");
    }
    /* `bits:` is exact, including payloads that no decimal form can express. */
    for (uint32_t pattern : {UINT32_C(0x7FA00000), UINT32_C(0x7FC00001),
                             UINT32_C(0xDEADBEEF), UINT32_C(0x80000000),
                             UINT32_C(0xFFFFFFFF)}) {
        char text[32] = {};
        std::snprintf(text, sizeof(text), "bits:0x%08X", pattern);
        const DecimalParse parsed = tcfp::parse_decimal(text);
        if (!parsed.ok || parsed.bits != pattern || parsed.diagnostics != 0)
            failures.push_back(std::string("bits: form lost ") + hex32(pattern));
    }
    /* -0 is a value of its own, in both directions. */
    const DecimalParse negative_zero = tcfp::parse_decimal("-0");
    if (!negative_zero.ok || negative_zero.bits != UINT32_C(0x80000000) ||
        negative_zero.diagnostics != 0)
        failures.push_back("-0 did not parse to the negative zero pattern");
    char text[32] = {};
    tcfp::format_shortest(UINT32_C(0x80000000), text, sizeof(text));
    if (std::strcmp(text, "-0") != 0)
        failures.push_back(std::string("negative zero formatted as \"") + text + "\"");
    /* A tiny buffer must stay NUL-terminated and inside itself. */
    char small[5] = {'x', 'x', 'x', 'x', 'x'};
    const size_t written = tcfp::format_value(UINT32_C(0x40600000),
                                             DisplayMode::decimal_and_hex, small,
                                             sizeof(small));
    if (small[sizeof(small) - 1] != '\0')
        failures.push_back("a small buffer was not NUL-terminated");
    if (written <= sizeof(small) - 1)
        failures.push_back("the formatter did not report the truncated length");
    /* Overflow and underflow are diagnostics, not syntax errors (plan 5.3). */
    const DecimalParse huge = tcfp::parse_decimal("1e40");
    if (!huge.ok || !(huge.diagnostics & tcfp::diag_overflow))
        failures.push_back("\"1e40\" did not report overflow");
    const DecimalParse tiny = tcfp::parse_decimal("1e-9999");
    if (!tiny.ok || !(tiny.diagnostics & tcfp::diag_underflow))
        failures.push_back("\"1e-9999\" did not report underflow");

    /* NaN: any pattern formats as "nan" and parses back to the canonical quiet
       NaN; a payload needs the bits: form (plan 2.4 keeps payloads for bit
       operations, not for text). */
    char nan_text[32] = {};
    tcfp::format_shortest(UINT32_C(0x7FA00000), nan_text, sizeof(nan_text));
    if (std::strcmp(nan_text, "nan") != 0)
        failures.push_back("a signaling NaN did not format as \"nan\"");
    const DecimalParse nan = tcfp::parse_decimal("nan");
    if (!nan.ok || nan.bits != UINT32_C(0x7FC00000) || nan.diagnostics != 0)
        failures.push_back("\"nan\" did not parse to the canonical quiet NaN");
}

}  // namespace

int main() {
    std::vector<std::string> failures;
    int parseChecked = 0;
    int formatChecked = 0;
    int roundTripChecked = 0;

    checkParseVectors(failures, parseChecked);
    checkFormatVectors(failures, formatChecked);
    checkRoundTrip(failures, roundTripChecked);
    checkContract(failures);

    if (!failures.empty()) {
        for (const std::string& failure : failures)
            std::printf("FAIL %s\n", failure.c_str());
        if (static_cast<int>(failures.size()) >= kMaxReportedFailures)
            std::printf("     (report truncated at %d failures)\n", kMaxReportedFailures);
        std::printf("FAIL float-ops text layer: %zu failing check(s)\n", failures.size());
        return 1;
    }

    std::printf("PASS float-ops text layer: %d parse vectors (value and editor "
                "diagnostics), %d format vectors (shortest/scientific/hex), %d "
                "shortest-round-trip pairs; illegal text refused, `bits:` exact, "
                "-0 preserved, small buffers safe\n",
                parseChecked, formatChecked, roundTripChecked);
    return 0;
}
