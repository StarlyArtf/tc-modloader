"""Golden vectors for the Float Ops text layer (plan 5.3).

Two things are pinned here, both from an independent Python implementation:

  * parsing: the correctly rounded binary32 value of a text and the editor
    diagnostics that go with it.  The expectation is computed from the exact
    decimal value (Fractions) with the same rounding rule the plan fixes
    (round-to-nearest, ties-to-even); the product path is fast_float, so the two
    agreeing is what makes the text form trustworthy.
  * formatting: the shortest decimal digits that round-trip through binary32.
    Those come from the classic shortest-representation interval: a decimal is
    accepted when the exact value of the pattern is the unique binary32 it
    rounds to, and the search walks digit counts from one up, so "shortest" is a
    property of the construction rather than a copy of Ryu's answer.

Usage (from the repository root):

    python tools/float-decimal-vectors.py            # rewrite the header
    python tools/float-decimal-vectors.py --check     # fail if out of date
"""

import argparse
import importlib.util
import pathlib
import random
import sys
from fractions import Fraction

HERE = pathlib.Path(__file__).resolve().parent


def load_float_vectors():
    """The arithmetic oracle lives in tools/float-vectors.py; reuse its
    rounding, its exact decoders and its pool of interesting patterns."""
    spec = importlib.util.spec_from_file_location("float_vectors",
                                                  HERE / "float-vectors.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

fv = load_float_vectors()

if hasattr(sys, "set_int_max_str_digits"):
    sys.set_int_max_str_digits(200000)

DIAG_OVERFLOW = 0x01
DIAG_UNDERFLOW = 0x02
DIAG_INEXACT = 0x04


def exact_value(bits):
    """The exact Fraction of a finite pattern, or None for Inf/NaN."""
    if fv.is_nan(bits) or fv.is_infinite(bits):
        return None
    sign, num, den = fv.exact_finite(bits)
    value = Fraction(num, den)
    return -value if sign else value


def rounds_to(digits, exponent, bits):
    """True when digits * 10**exponent rounds to exactly `bits`."""
    value = Fraction(digits) * Fraction(10) ** exponent
    sign = 1 if fv.is_negative(bits) else 0
    if value == 0:
        return fv.is_zero(bits)
    rounded, _ = fv.round_to_binary32(sign, value.numerator, value.denominator, fv.RNE)
    return rounded == bits


def shortest_decimal(bits):
    """(digits, leading exponent) of the shortest round-tripping decimal.

    The search walks the number of significant digits from one upwards and, for
    each count, checks the few decimals of that length around the value - the
    check is the exact rounding oracle, so a candidate is only accepted when it
    really does round back to the same pattern.
    """
    if fv.is_zero(bits):
        return "0", 0
    value = exact_value(bits)
    if value is None:
        return None
    magnitude = abs(value)
    for count in range(1, 10):
        leading = floor_log10(magnitude)
        for k in (leading, leading + 1):
            quantum_exponent = k - count + 1
            scaled = magnitude / (Fraction(10) ** quantum_exponent)
            base = floor_fraction(scaled)
            if scaled - base > Fraction(1, 2):
                base += 1
            # The closest of the few candidates wins (that is what a
            # shortest-representation algorithm is expected to deliver), and a
            # candidate is accepted only when the exact oracle rounds it back.
            ordered = sorted((base - 1, base, base + 1),
                             key=lambda m: (abs(Fraction(m) - scaled), abs(m)))
            for candidate in ordered:
                if candidate <= 0:
                    continue
                digits = str(candidate)
                if len(digits) > count + 1:
                    continue
                # Trailing zeros are not significant digits: "100" at this
                # quantum is the same number as "1" two decades up.
                trimmed = digits.rstrip("0") or "0"
                exponent = quantum_exponent + (len(digits) - len(trimmed))
                if rounds_to(int(trimmed), exponent, bits):
                    return trimmed, k
    raise AssertionError(f"no shortest decimal found for 0x{bits:08X}")


def floor_fraction(value):
    return value.numerator // value.denominator


def floor_log10(value):
    """floor(log10(value)) for a positive Fraction."""
    guess = len(str(value.numerator)) - len(str(value.denominator))
    while Fraction(10) ** guess > value:
        guess -= 1
    while Fraction(10) ** (guess + 1) <= value:
        guess += 1
    return guess


def render_shortest(digits, exponent, negative):
    sign = "-" if negative else ""
    count = len(digits)
    if digits == "0":
        return sign + "0"
    if 0 <= exponent < 7:
        integer_digits = exponent + 1
        if integer_digits >= count:
            return sign + digits + "0" * (integer_digits - count)
        return sign + digits[:integer_digits] + "." + digits[integer_digits:]
    if -5 < exponent < 0:
        return sign + "0." + "0" * (-exponent - 1) + digits
    return sign + render_scientific(digits, exponent)


def render_scientific(digits, exponent):
    body = digits[0] + "." + (digits[1:] if len(digits) > 1 else "0")
    return f"{body}e{'+' if exponent >= 0 else '-'}{abs(exponent):02d}"


def render(bits):
    """The three strings the Mod's formatters must produce for `bits`."""
    if fv.is_nan(bits):
        return "nan", "nan", f"0x{bits:08X}"
    if fv.is_infinite(bits):
        text = "-inf" if fv.is_negative(bits) else "inf"
        return text, text, f"0x{bits:08X}"
    negative = fv.is_negative(bits)
    digits, exponent = shortest_decimal(bits)
    shortest = render_shortest(digits, exponent, negative)
    scientific = ("-" if negative else "") + render_scientific(digits, exponent)
    return shortest, scientific, f"0x{bits:08X}"


def parse_expectation(text):
    """(ok, bits, diagnostics) for one text, computed here from scratch."""
    stripped = text.strip()
    if stripped.lower().startswith("bits:"):
        body = stripped[5:]
        if body[:2].lower() == "0x":
            body = body[2:]
        if 1 <= len(body) <= 8 and all(c in "0123456789abcdefABCDEF" for c in body):
            return True, int(body, 16), 0
        return False, 0, 0
    lowered = stripped.lower()
    if lowered in ("inf", "+inf", "infinity", "+infinity"):
        return True, fv.INFINITY, 0
    if lowered in ("-inf", "-infinity"):
        return True, fv.INFINITY | fv.SIGN, 0
    if lowered in ("nan", "+nan"):
        return True, fv.CANONICAL_NAN, 0
    if lowered == "-nan":
        # The sign of a NaN is not defined by IEEE, and the parser keeps it:
        # "-nan" is the negative quiet NaN, which is what the Display shows.
        return True, fv.CANONICAL_NAN | fv.SIGN, 0

    # Split into (<digits>, <decimal exponent>), then round exactly once.
    try:
        value = exact_text_value(stripped)
    except ValueError:
        return False, 0, 0
    if value is None:
        return False, 0, 0

    sign = 1 if value < 0 else 0
    magnitude = abs(value)
    diagnostics = 0
    if magnitude == 0:
        # "-0" and "-0.0" are the negative zero pattern: the sign of a zero is
        # part of the value, so the textual sign decides here.
        negative_zero = stripped.startswith("-")
        return True, (fv.SIGN if negative_zero else 0), 0
    exponent = floor_log10(magnitude)
    if exponent > 40:
        bits = fv.INFINITY | (fv.SIGN if sign else 0)
        return True, bits, DIAG_OVERFLOW | DIAG_INEXACT
    if exponent < -60:
        bits = fv.SIGN if sign else 0
        return True, bits, DIAG_UNDERFLOW | DIAG_INEXACT
    bits, flags = fv.round_to_binary32(sign, magnitude.numerator,
                                       magnitude.denominator, fv.RNE)
    if fv.is_infinite(bits):
        diagnostics |= DIAG_OVERFLOW
    elif fv.is_zero(bits):
        diagnostics |= DIAG_UNDERFLOW
    if flags & fv.FLAG_NX:
        diagnostics |= DIAG_INEXACT
    return True, bits, diagnostics


def exact_text_value(text):
    """Exact value of a C-locale decimal literal, or None when malformed."""
    index = 0
    negative = False
    if index < len(text) and text[index] in "+-":
        negative = text[index] == "-"
        index += 1
    digits = ""
    while index < len(text) and text[index].isdigit():
        digits += text[index]
        index += 1
    if index < len(text) and text[index] == ".":
        index += 1
        while index < len(text) and text[index].isdigit():
            digits += text[index]
            index += 1
    if not digits:
        return None
    exponent = 0
    if index < len(text) and text[index] in "eE":
        index += 1
        sign = 1
        if index < len(text) and text[index] in "+-":
            sign = -1 if text[index] == "-" else 1
            index += 1
        if index >= len(text) or not text[index].isdigit():
            return None
        value = 0
        while index < len(text) and text[index].isdigit():
            value = value * 10 + int(text[index])
            index += 1
            if value > 100000:
                return None
        exponent = sign * value
    if index != len(text):
        return None
    # The decimal point position: digits after it reduce the exponent.
    mantissa = text.split("e")[0].split("E")[0]
    point = mantissa.find(".")
    fractional = 0 if point < 0 else len(mantissa) - point - 1
    value = Fraction(int(digits), 1) * Fraction(10) ** (exponent - fractional)
    return -value if negative else value


def build_parse_vectors():
    texts = [
        "0", "-0", "1", "-1", "3.5", "-3.5", "0.1", "0.5", "0.25", "2", "1024",
        "16777216", "16777217", "16777218", "1e7", "1e-7", "1.5e-45", "1e-45",
        "1e-46", "3.4028235e38", "3.4028236e38", "1e39", "-1e39", "1e-9999",
        "0.0000000000000000000001", "123456789", "1234567890123456789",
        "0.30000001192092896", "0.3000000119209289", "inf", "-inf", "nan", "-nan",
        "bits:0x7FA00000", "bits:0x7FC00001", "bits:DEADBEEF", "bits:0x1", "bits:0x0",
        "bits:0xFFFFFFFF", "BITS:0x80000000", "  4.75  ", "3.5x", "", "  ",
        "1e", "e5", "0x10", "1.2.3", "--1", "1e+", "+7", "-0.0", "6.1e-8",
    ]
    # A few random texts as well, so the vector file is not only the list above.
    rng = random.Random(0x464C4F41)
    for _ in range(40):
        digits = rng.randint(1, 12)
        value = str(rng.randrange(10 ** (digits - 1), 10 ** digits))
        point = rng.randint(0, len(value))
        text = value[:point] + "." + value[point:]
        if rng.random() < 0.5:
            text += "e" + str(rng.randint(-45, 40))
        texts.append(text)
    return texts


def collect():
    parses = []
    for text in build_parse_vectors():
        ok, bits, diagnostics = parse_expectation(text)
        parses.append((text, ok, bits, diagnostics))

    rng = random.Random(0x52594F55)
    bits_pool = list(fv.build_pool())
    bits_pool += [0x00000002, 0x0D800000, 0x33800000, 0x7F7FFFFE, 0x00800001]
    bits_pool += [rng.getrandbits(32) for _ in range(220)]
    bits_pool += [(rng.getrandbits(32) & 0x807FFFFF) for _ in range(60)]
    bits_pool += [(rng.getrandbits(32) | 0x3F800000) for _ in range(60)]

    formats = []
    seen = set()
    for bits in bits_pool:
        if bits in seen:
            continue
        seen.add(bits)
        formats.append((bits,) + render(bits))

    # Every produced string must parse back to the same pattern: that is the
    # round-trip claim the Display layout makes.
    for bits, shortest, scientific, _ in formats:
        if fv.is_nan(bits) or fv.is_infinite(bits) or fv.is_zero(bits):
            continue
        for text in (shortest, scientific):
            ok, parsed, _ = parse_expectation(text)
            assert ok and parsed == bits, (
                f"round-trip failed for 0x{bits:08X}: {text!r} -> 0x{parsed:08X}")
    return parses, formats


def c_string(text):
    out = '"'
    for character in text:
        if character == '"':
            out += '\\"'
        elif character == "\\":
            out += "\\\\"
        else:
            out += character
    return out + '"'


def render_header(parses, formats):
    lines = [
        "/* Generated by tools/float-decimal-vectors.py - do not edit by hand.",
        "",
        "   Golden vectors for the Float Ops text layer (plan 5.3).  The parser",
        "   expectations come from exact decimal arithmetic in Python; the shortest",
        "   forms come from the classic shortest-round-trip interval, not from Ryu.",
        "",
        f"   parse vectors={len(parses)} format vectors={len(formats)} */",
        "",
        "#ifndef float_ops_decimal_vectors_generated_hpp",
        "#define float_ops_decimal_vectors_generated_hpp",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace tcfp_decimal_vectors {",
        "",
        "struct ParseVector {",
        "    const char* text;",
        "    bool ok;",
        "    uint32_t bits;",
        "    uint8_t diagnostics;",
        "};",
        "",
        "struct FormatVector {",
        "    uint32_t bits;",
        "    const char* shortest;",
        "    const char* scientific;",
        "    const char* hex;",
        "};",
        "",
        f"inline constexpr size_t kParseVectorCount = {len(parses)};",
        f"inline constexpr size_t kFormatVectorCount = {len(formats)};",
        "",
        "inline constexpr ParseVector kParseVectors[] = {",
    ]
    for text, ok, bits, diagnostics in parses:
        lines.append(f"    {{ {c_string(text)}, {'true ' if ok else 'false'}, "
                     f"0x{bits:08X}U, 0x{diagnostics:02X} }},")
    lines += [
        "};",
        "",
        "inline constexpr FormatVector kFormatVectors[] = {",
    ]
    for bits, shortest, scientific, hex_text in formats:
        lines.append(f"    {{ 0x{bits:08X}U, {c_string(shortest)}, "
                     f"{c_string(scientific)}, {c_string(hex_text)} }},")
    lines += [
        "};",
        "",
        "}  // namespace tcfp_decimal_vectors",
        "",
        "#endif",
        "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="generate Float Ops decimal vectors")
    parser.add_argument("--output",
                        default="tests/data/float32/decimal-vectors.generated.hpp")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    arguments = parser.parse_args()

    parses, formats = collect()
    text = render_header(parses, formats)
    path = pathlib.Path(arguments.output)

    if arguments.check:
        if not path.exists():
            print(f"FAIL {path} is missing; run tools/float-decimal-vectors.py")
            return 1
        if path.read_text(encoding="utf-8") != text:
            print(f"FAIL {path} is out of date; run tools/float-decimal-vectors.py")
            return 1
        if not arguments.quiet:
            print(f"PASS decimal vectors up to date: {len(parses)} parse, "
                  f"{len(formats)} format")
        return 0

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if not arguments.quiet:
        print(f"wrote {path}: {len(parses)} parse vectors, {len(formats)} format vectors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
