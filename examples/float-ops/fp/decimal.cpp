/* The text side of the Float Ops Mod (plan 5.3).

   Two vendored libraries do the hard parts and this file only adapts them:

   - fast_float parses the text to binary32 with correct rounding and without a
     locale, which is the contract plan 5.3 fixes;
   - Ryu produces the shortest decimal digits that round-trip through binary32,
     and the layout below turns those digits into the strings the Mod shows.

   Neither library knows the project's semantics and the kernel knows nothing
   about text: the boundary between them is a `uint32_t` bit pattern. */

#include "decimal.hpp"

#include "fast_float/fast_float.h"

extern "C" {
#include "ryu/ryu.h"
}

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace tcfp {
namespace {

constexpr uint32_t kSignMask32 = 0x80000000u;
constexpr uint32_t kExponentMask32 = 0x7F800000u;
constexpr uint32_t kSignificandMask32 = 0x007FFFFFu;
constexpr int kMaxExactDigits = 19; /* fits the exactness check below */

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/* A bounded writer: always NUL-terminated, never past the end, and its length
   keeps counting so a caller can tell "it fitted" from "it was cut off". */
struct Sink {
    char* out;
    size_t capacity;
    size_t written = 0;

    Sink(char* buffer, size_t size) : out(buffer), capacity(size) {
        if (capacity) out[0] = '\0';
    }
    void put(char c) {
        if (written + 1 < capacity) out[written] = c;
        ++written;
        if (capacity) out[written < capacity ? written : capacity - 1] = '\0';
    }
    void text(const char* value) {
        for (const char* p = value; p && *p; ++p) put(*p);
    }
    size_t finish() const { return written; }
};

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    const char lowered = lower(c);
    if (lowered >= 'a' && lowered <= 'f') return lowered - 'a' + 10;
    return -1;
}

bool startsWith(const char* text, const char* prefix) {
    while (*prefix) {
        if (lower(*text) != lower(*prefix)) return false;
        ++text;
        ++prefix;
    }
    return true;
}

/* `bits:0xXXXXXXXX` - the escape hatch for payloads and exact test vectors. */
bool parseBitsForm(const char* text, size_t length, uint32_t* out) {
    if (length <= 5 || !startsWith(text, "bits:")) return false;
    const char* p = text + 5;
    size_t remaining = length - 5;
    if (remaining > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        remaining -= 2;
    }
    if (remaining == 0 || remaining > 8) return false;
    uint32_t value = 0;
    for (size_t index = 0; index < remaining; ++index) {
        const int digit = hexValue(p[index]);
        if (digit < 0) return false;
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    *out = value;
    return true;
}

/* ---- the editor's inexact diagnostic -------------------------------------
   The text is a rational: (digits, exponent) means digits * 10**exponent, and a
   binary32 pattern is significand * 2**exponent.  Whether the two are the same
   number is therefore an exact integer comparison; 128-bit integers are enough
   for the window below, and outside it the diagnostic is simply not reported.
   Nothing else in the Mod depends on it: the parsed *value* is whatever
   fast_float correctly rounded, this only decides whether to say "inexact". */

struct DecimalDigits {
    uint64_t value = 0;
    int exponent = 0;
    int count = 0;
    bool truncated = false;
    bool valid = false;
};

DecimalDigits digitsOf(const char* text, size_t length) {
    DecimalDigits digits;
    int fractional = 0;
    int dropped = 0;
    int explicit_exponent = 0;
    bool seen_point = false;
    for (size_t index = 0; index < length; ++index) {
        const char c = text[index];
        if (c == '+' || c == '-') continue;
        if (c == '.') {
            seen_point = true;
            continue;
        }
        if (c == 'e' || c == 'E') {
            explicit_exponent = std::atoi(text + index + 1);
            break;
        }
        if (c < '0' || c > '9') break;
        digits.valid = true;
        if (seen_point) ++fractional;
        if (digits.count < kMaxExactDigits) {
            digits.value = digits.value * 10 + static_cast<uint64_t>(c - '0');
            if (digits.value) ++digits.count;
        } else {
            ++dropped;
        }
    }
    digits.truncated = dropped != 0;
    digits.exponent = explicit_exponent - fractional + dropped;
    return digits;
}

bool scaleBy10(unsigned __int128* value, int times) {
    for (int i = 0; i < times; ++i) {
        if (*value > (~static_cast<unsigned __int128>(0)) / 10) return false;
        *value *= 10;
    }
    return true;
}

bool scaleBy2(unsigned __int128* value, int times) {
    for (int i = 0; i < times; ++i) {
        if (*value >> 127) return false;
        *value <<= 1;
    }
    return true;
}

bool exactDecimalEquals(uint64_t digits, int exponent, uint32_t bits) {
    const uint32_t magnitude = bits & ~kSignMask32;
    if (digits == 0) return magnitude == 0;
    if (magnitude == 0) return false;
    const uint32_t exponent_field = (magnitude & kExponentMask32) >> 23;
    if (exponent_field == 0xFF) return false;
    const uint64_t significand =
        exponent_field ? ((UINT64_C(1) << 23) | (magnitude & kSignificandMask32))
                       : (magnitude & kSignificandMask32);
    const int binary_exponent =
        exponent_field ? (static_cast<int>(exponent_field) - 150) : -149;

    unsigned __int128 left = digits;
    unsigned __int128 right = significand;
    if (exponent > 0 && !scaleBy10(&left, exponent)) return false;
    if (exponent < 0 && !scaleBy10(&right, -exponent)) return false;
    if (binary_exponent > 0 && !scaleBy2(&right, binary_exponent)) return false;
    if (binary_exponent < 0 && !scaleBy2(&left, -binary_exponent)) return false;
    return left == right;
}

/* ---- Ryu's shortest digits ----------------------------------------------- */

struct Shortest {
    bool negative = false;
    bool special = false;
    const char* special_text = "";
    char digits[24] = {};
    int count = 0;
    /* Ryu prints `d1.d2d3...E<exponent>`, so its exponent is the weight of the
       *first* digit: the value is 0.d1d2... times 10**(exponent + 1). */
    int exponent = 0;
};

Shortest shortestOf(uint32_t bits) {
    Shortest shortest;
    float value = 0.f;
    std::memcpy(&value, &bits, sizeof(value));
    char buffer[32] = {};
    f2s_buffered(value, buffer);
    const char* p = buffer;
    if (*p == '-') {
        shortest.negative = true;
        ++p;
    }
    if (std::strcmp(p, "NaN") == 0 || std::strcmp(p, "Infinity") == 0) {
        shortest.special = true;
        shortest.special_text = std::strcmp(p, "NaN") == 0
                                    ? "nan"
                                    : (shortest.negative ? "-inf" : "inf");
        return shortest;
    }
    /* Ryu writes <digits>[.<digits>]E<exponent>. */
    int exponent = 0;
    for (; *p && *p != 'E' && *p != 'e'; ++p) {
        if (*p == '.') continue;
        if (shortest.count < static_cast<int>(sizeof(shortest.digits)) - 1)
            shortest.digits[shortest.count++] = *p;
    }
    if (*p) exponent = std::atoi(p + 1);
    shortest.digits[shortest.count] = '\0';
    shortest.exponent = exponent;
    return shortest;
}

/* Weight of the first digit, as Ryu reported it. */
int leadingExponent(const Shortest& shortest) {
    return shortest.exponent;
}

void renderScientific(const Shortest& shortest, Sink& sink, bool with_sign);

void renderShortest(const Shortest& shortest, char* out, size_t capacity) {
    Sink sink(out, capacity);
    if (shortest.special) {
        sink.text(shortest.special_text);
        return;
    }
    if (shortest.negative) sink.put('-');
    if (shortest.count == 0) {
        sink.text("0");
        return;
    }
    const int exponent = leadingExponent(shortest);
    const int count = shortest.count;
    if (exponent >= 0 && exponent < 7) {
        const int integer_digits = exponent + 1;
        if (integer_digits >= count) {
            for (int i = 0; i < count; ++i) sink.put(shortest.digits[i]);
            for (int i = count; i < integer_digits; ++i) sink.put('0');
        } else {
            for (int i = 0; i < integer_digits; ++i) sink.put(shortest.digits[i]);
            sink.put('.');
            for (int i = integer_digits; i < count; ++i) sink.put(shortest.digits[i]);
        }
        return;
    }
    if (exponent < 0 && exponent > -5) {
        sink.text("0.");
        for (int i = 0; i < -exponent - 1; ++i) sink.put('0');
        for (int i = 0; i < count; ++i) sink.put(shortest.digits[i]);
        return;
    }
    /* The sign is already written above, so the scientific layout must not add
       a second one. */
    renderScientific(shortest, sink, false);
    }

void putTwoDigitExponent(Sink& sink, int exponent) {
    sink.put(exponent < 0 ? '-' : '+');
    int value = exponent < 0 ? -exponent : exponent;
    char digits[8] = {};
    int length = 0;
    do {
        digits[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value && length < 7);
    if (length < 2) sink.put('0');
    while (length > 0) sink.put(digits[--length]);
}

void renderScientific(const Shortest& shortest, Sink& sink, bool with_sign) {
    if (shortest.special) {
        sink.text(shortest.special_text);
        return;
    }
    if (with_sign && shortest.negative) sink.put('-');
    if (shortest.count == 0) {
        sink.text("0e+00");
        return;
    }
    sink.put(shortest.digits[0]);
    sink.put('.');
    if (shortest.count > 1) {
        for (int i = 1; i < shortest.count; ++i) sink.put(shortest.digits[i]);
    } else {
        sink.put('0');
    }
    sink.put('e');
    putTwoDigitExponent(sink, leadingExponent(shortest));
}

void renderHex(uint32_t bits, Sink& sink) {
    const char* hex = "0123456789ABCDEF";
    sink.text("0x");
    for (int shift = 28; shift >= 0; shift -= 4) sink.put(hex[(bits >> shift) & 0xF]);
}

}  // namespace

DecimalParse parse_decimal(const char* text, size_t length) {
    DecimalParse result{false, 0, 0};
    if (!text) return result;
    while (length && isSpace(text[0])) {
        ++text;
        --length;
    }
    while (length && isSpace(text[length - 1])) --length;
    if (!length) return result;

    if (parseBitsForm(text, length, &result.bits)) {
        result.ok = true;
        return result;
    }

    /* fast_float follows std::from_chars, which refuses a leading plus; the
       editors accept it the way the game's own value fields do. */
    if (length && text[0] == '+') {
        ++text;
        --length;
        if (!length) return result;
    }

    /* fast_float reports how much text it consumed, so "3.5x" is a syntax error
       instead of a silent 3.5. */
    float parsed = 0.f;
    const auto answer = fast_float::from_chars(text, text + length, parsed);
    if (answer.ec == std::errc::invalid_argument) return result;
    if (answer.ptr != text + length) return result;
    std::memcpy(&result.bits, &parsed, sizeof(result.bits));
    result.ok = true;

    if (answer.ec == std::errc::result_out_of_range) {
        const uint32_t magnitude = result.bits & ~kSignMask32;
        result.diagnostics |= magnitude ? diag_overflow : diag_underflow;
    }

    const DecimalDigits digits = digitsOf(text, length);
    if (digits.valid && !digits.truncated &&
        !exactDecimalEquals(digits.value, digits.exponent, result.bits))
        result.diagnostics |= diag_inexact;
    return result;
}

DecimalParse parse_decimal(const char* text) {
    return parse_decimal(text, text ? std::strlen(text) : 0);
}

size_t format_shortest(uint32_t bits, char* out, size_t capacity) {
    const Shortest shortest = shortestOf(bits);
    renderShortest(shortest, out, capacity);
    return std::strlen(out);
}

size_t format_scientific(uint32_t bits, char* out, size_t capacity) {
    const Shortest shortest = shortestOf(bits);
    Sink sink(out, capacity);
    renderScientific(shortest, sink, true);
    return sink.finish();
}

size_t format_hex(uint32_t bits, char* out, size_t capacity) {
    Sink sink(out, capacity);
    renderHex(bits, sink);
    return sink.finish();
}

size_t format_value(uint32_t bits, DisplayMode mode, char* out, size_t capacity) {
    switch (mode) {
        case DisplayMode::shortest:
            return format_shortest(bits, out, capacity);
        case DisplayMode::scientific:
            return format_scientific(bits, out, capacity);
        case DisplayMode::hex_only:
            return format_hex(bits, out, capacity);
        case DisplayMode::decimal_and_hex:
        default:
            break;
    }
    char decimal[40] = {};
    format_shortest(bits, decimal, sizeof(decimal));
    Sink sink(out, capacity);
    sink.text(decimal);
    sink.put(' ');
    renderHex(bits, sink);
    return sink.finish();
}

const char* decimal_special_name(uint32_t bits) {
    const uint32_t magnitude = bits & ~kSignMask32;
    if (magnitude > kExponentMask32) return "nan";
    if (magnitude == kExponentMask32) return (bits & kSignMask32) ? "-inf" : "inf";
    if (magnitude == 0) return (bits & kSignMask32) ? "-0" : "0";
    return nullptr;
}

}  // namespace tcfp
