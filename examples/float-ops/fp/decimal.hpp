/* Text in, bit patterns out - and the other way round (plan 5.3).

   The kernel never sees text: a Constant editor parses its text once, when the
   edit is committed, and the instance's configuration keeps the resulting
   uint32_t.  Display formats on the CPU and hands the finished string to the
   renderer, so nothing is parsed or formatted inside a simulation cycle.

   Parsing is fast_float (correctly rounded, round-to-nearest-ties-to-even,
   locale-independent) plus one project-specific form, `bits:0xXXXXXXXX`, for
   sNaN payloads and exact test vectors.  Formatting uses Ryu's shortest
   round-trip digits for binary32; the project decides how to lay them out.

   Diagnostics of a parse (overflow, underflow, inexact) are for the editor:
   they are never published on a component's Flags[5] output (plan 5.3). */

#ifndef float_ops_fp_decimal_hpp
#define float_ops_fp_decimal_hpp

#include <cstddef>
#include <cstdint>

namespace tcfp {

/* Editor-side diagnostics, one bit each.  The value is the text's, not an
   operation's. */
enum DecimalDiagnostic : uint8_t {
    diag_overflow = 0x01,
    diag_underflow = 0x02,
    diag_inexact = 0x04,
};

struct DecimalParse {
    bool ok;              /* false only for a syntax error: keep the old value */
    uint32_t bits;
    uint8_t diagnostics;  /* DecimalDiagnostic bits, empty for `bits:` forms */
};

/* Accepts ordinary decimal, scientific notation, inf/-inf/nan/-nan/-0 and
   `bits:0xXXXXXXXX` (the 0x optional, 1..8 hex digits).  Surrounding white
   space is ignored; anything else is a syntax error.  A value that overflows
   or underflows the format is *not* a syntax error: it parses to +-Inf or to
   zero and reports the diagnostic (plan 5.3). */
DecimalParse parse_decimal(const char* text, size_t length);
DecimalParse parse_decimal(const char* text);

/* Display preferences (plan 6.1 and 8.1).  The numeric values are archived. */
enum class DisplayMode : uint8_t {
    decimal_and_hex = 0, /* "3.5  0x40600000" */
    shortest = 1,        /* "3.5" */
    scientific = 2,      /* "3.5e+00" */
    hex_only = 3,        /* "0x40600000" */
};

/* All formatters write a NUL-terminated string and return its length (without
   the terminator).  A buffer that is too small is filled with as much as fits
   and still terminated, never overrun. */
size_t format_shortest(uint32_t bits, char* out, size_t capacity);
size_t format_scientific(uint32_t bits, char* out, size_t capacity);
size_t format_hex(uint32_t bits, char* out, size_t capacity);
size_t format_value(uint32_t bits, DisplayMode mode, char* out, size_t capacity);

/* `nan`, `inf`, `-inf` and the signed zero are part of the contract; these name
   them the same way the formatters do. */
const char* decimal_special_name(uint32_t bits); /* nullptr for finite values */

}  // namespace tcfp

#endif
