"""Generate the golden vectors for the Float Ops binary32 kernel.

This file is the *independent* side of the offline numeric layer described in
docs/PLAN-float-components.md section 10.1: the kernel under test is SoftFloat
wrapped by fp/fp32.cpp, and every expectation below is computed here with exact
rational arithmetic and integer comparisons - no host float, no SoftFloat, and
no reference to the C++ implementation.

What the oracle implements, straight from the plan:

  * IEEE 754-2019 binary32, round-to-nearest-ties-to-even by default and the four
    other modes on request (2.2);
  * the five exceptions of 2.3, tininess detected after rounding, and UF only
    when the result is tiny *and* inexact;
  * canonical NaN results: every arithmetic NaN result is 0x7FC00000, a quiet
    NaN input never signals, a signaling input always does, and 0 x Inf + NaN
    still reports invalid (2.4);
  * the quiet compare relation, the ten-way classification and the 2019
    minimumNumber/maximumNumber pair, including -0 < +0 (2.5);
  * IEEE remainder whose quotient rounds to nearest-ties-to-even, independent of
    the instance's rounding mode (6.3).

A second, independent check runs before anything is written: every
round-to-nearest vector is compared against CPython's own double-precision
arithmetic followed by a single rounding to binary32.  That is a valid
second opinion for +, -, x, / and sqrt because a double carries more than 2p+2
bits for p = 24, so the double rounding is innocuous.

Usage (from the repository root):

    python tools/float-vectors.py                 # rewrite the generated header
    python tools/float-vectors.py --check         # fail if it is out of date
"""

import argparse
import math
import pathlib
import random
import struct
import sys

P = 24
EMIN = -126
EMAX = 127
SIGNIFICAND_BITS = 23
SUBNORMAL_QUANTUM = EMIN - SIGNIFICAND_BITS  # 2**-149

FLAG_NV = 0x10
FLAG_DZ = 0x08
FLAG_OF = 0x04
FLAG_UF = 0x02
FLAG_NX = 0x01

CANONICAL_NAN = 0x7FC00000
MAX_FINITE = 0x7F7FFFFF
INFINITY = 0x7F800000
SIGN = 0x80000000

# Operation codes shared with tests/float-kernel.cpp.
OP_ADD = 1
OP_SUBTRACT = 2
OP_MULTIPLY = 3
OP_DIVIDE = 4
OP_SQUARE_ROOT = 5
OP_FMA = 6
OP_REMAINDER = 7
OP_ROUND_TO_INTEGRAL_EXACT = 8
OP_COMPARE = 9
OP_CLASSIFY = 10
OP_MINIMUM_NUMBER = 11
OP_MAXIMUM_NUMBER = 12

OP_NAMES = {
    OP_ADD: "add",
    OP_SUBTRACT: "subtract",
    OP_MULTIPLY: "multiply",
    OP_DIVIDE: "divide",
    OP_SQUARE_ROOT: "square_root",
    OP_FMA: "fused_multiply_add",
    OP_REMAINDER: "remainder",
    OP_ROUND_TO_INTEGRAL_EXACT: "round_to_integral_exact",
    OP_COMPARE: "compare",
    OP_CLASSIFY: "classify",
    OP_MINIMUM_NUMBER: "minimum_number",
    OP_MAXIMUM_NUMBER: "maximum_number",
}

RNE, RNA, RTZ, RDN, RUP = 0, 1, 2, 3, 4
ROUNDING_NAMES = {RNE: "RNE", RNA: "RNA", RTZ: "RTZ", RDN: "RDN", RUP: "RUP"}
ALL_ROUNDINGS = (RNE, RNA, RTZ, RDN, RUP)

CLASS_NEG_INF = 1 << 0
CLASS_NEG_NORMAL = 1 << 1
CLASS_NEG_SUBNORMAL = 1 << 2
CLASS_NEG_ZERO = 1 << 3
CLASS_POS_ZERO = 1 << 4
CLASS_POS_SUBNORMAL = 1 << 5
CLASS_POS_NORMAL = 1 << 6
CLASS_POS_INF = 1 << 7
CLASS_SNAN = 1 << 8
CLASS_QNAN = 1 << 9

# compare packs its relation into the low four bits of the expected value.
REL_LT = 1 << 0
REL_EQ = 1 << 1
REL_GT = 1 << 2
REL_UN = 1 << 3


# --------------------------------------------------------------------------
# bit-level helpers
# --------------------------------------------------------------------------


def is_nan(bits):
    return (bits & 0x7F800000) == 0x7F800000 and (bits & 0x007FFFFF) != 0


def is_signaling_nan(bits):
    return is_nan(bits) and (bits & 0x00400000) == 0


def is_infinite(bits):
    return (bits & 0x7FFFFFFF) == INFINITY


def is_zero(bits):
    return (bits & 0x7FFFFFFF) == 0


def is_negative(bits):
    return (bits & SIGN) != 0


def is_subnormal(bits):
    return (bits & 0x7F800000) == 0 and (bits & 0x007FFFFF) != 0


def exact_finite(bits):
    """(sign, numerator, denominator) of a finite value; zeros are (s, 0, 1)."""
    sign = 1 if is_negative(bits) else 0
    exponent = (bits >> 23) & 0xFF
    fraction = bits & 0x007FFFFF
    if exponent == 0:
        return sign, fraction, 1 << 149
    significand = (1 << 23) | fraction
    # value = significand * 2**(exponent - 127 - 23)
    shift = exponent - 127 - SIGNIFICAND_BITS
    if shift >= 0:
        return sign, significand << shift, 1
    return sign, significand, 1 << (-shift)


def assemble(sign, exponent, fraction):
    return (SIGN if sign else 0) | (exponent << 23) | fraction


def floor_log2_ratio(num, den):
    """floor(log2(num/den)) for positive integers num, den."""
    estimate = num.bit_length() - den.bit_length()
    if estimate >= 0:
        if num < (den << estimate):
            estimate -= 1
    elif (num << (-estimate)) < den:
        estimate -= 1
    return estimate


def round_ratio_to_int(sign, num, den, mode):
    """Round the signed exact value (-1)**sign * num/den to an integer.

    The four directed modes are about the *value*, not about the magnitude:
    roundTowardNegative sends a negative value to the next more negative
    integer, and roundTowardPositive sends it to the next less negative one.
    """
    lower = num // den                     # floor of the magnitude
    upper = lower + (1 if num % den else 0)  # ceiling of the magnitude
    if mode == RTZ:
        magnitude = lower
    elif mode == RDN:
        magnitude = upper if sign else lower
    elif mode == RUP:
        magnitude = lower if sign else upper
    else:
        quotient, remainder = divmod(num, den)
        twice = remainder << 1
        if twice > den:
            quotient += 1
        elif twice == den and (mode == RNA or (quotient & 1)):
            quotient += 1
        magnitude = quotient
    return -magnitude if sign else magnitude


def order_key(bits):
    """A monotone integer key over non-NaN patterns: key order is value order."""
    return (bits | SIGN) if (bits & SIGN) == 0 else ((~bits) & ~SIGN)


def from_order_key(key):
    key &= 0xFFFFFFFF
    return (key & ~SIGN) if (key & SIGN) else ((~key) & 0xFFFFFFFF)


def compare_bits_to_exact(bits, sign, num, den):
    """-1/0/+1 comparing a finite pattern's value with the exact value."""
    bits_sign, bits_num, bits_den = exact_finite(bits)
    left = bits_num * den * (-1 if bits_sign else 1)
    right = num * bits_den * (-1 if sign else 1)
    if left < right:
        return -1
    return 1 if left > right else 0


def neighbour_relation(key, sign, num, den):
    """compare_bits_to_exact for the neighbour at `key`, or None if it is not
    finite (the overflow region has no such neighbour inside the format)."""
    pattern = from_order_key(key)
    if is_nan(pattern) or is_infinite(pattern):
        return None
    return compare_bits_to_exact(pattern, sign, num, den)


def verify_rounding(sign, num, den, mode, bits):
    """Self-check of a result against the definition of its rounding mode.

    This is deliberately written from the definition (which side of the exact
    value the result sits on, and whether its neighbour on the other side
    crosses the exact value) rather than from the rounding code above, so a
    direction mistake in that code cannot pass unnoticed.
    """
    if is_nan(bits) or is_infinite(bits):
        return  # only the overflow paths deliver these
    relation = compare_bits_to_exact(bits, sign, num, den)
    if relation == 0:
        return  # exactly representable: every mode agrees
    key = order_key(bits)
    if mode == RDN:
        assert relation < 0, "roundTowardNegative rounded the wrong way"
        neighbour = neighbour_relation(key + 1, sign, num, den)
        assert neighbour is None or neighbour > 0, \
            "roundTowardNegative is not the next value down"
    elif mode == RUP:
        assert relation > 0, "roundTowardPositive rounded the wrong way"
        neighbour = neighbour_relation(key - 1, sign, num, den)
        assert neighbour is None or neighbour < 0, \
            "roundTowardPositive is not the next value up"
    elif mode == RTZ:
        detail = (f" [RTZ sign={sign} exact={num}/{den} result=0x{bits:08X} "
                  f"relation={relation}]")
        if sign == 0:
            assert relation < 0, "roundTowardZero grew the magnitude" + detail
            neighbour = neighbour_relation(key + 1, sign, num, den)
            assert neighbour is None or neighbour > 0, \
                "roundTowardZero is not the last value below the exact one" + detail
        else:
            assert relation > 0, "roundTowardZero grew the magnitude" + detail
            neighbour = neighbour_relation(key - 1, sign, num, den)
            assert neighbour is None or neighbour < 0, \
                "roundTowardZero is not the last value above the exact one" + detail
    else:
        # Nearest: the distance to the exact value is at most half an ulp.
        bits_sign, bits_num, bits_den = exact_finite(bits)
        difference = abs(bits_num * den - num * bits_den)
        # The ulp of the result's binade, or the fixed subnormal quantum when
        # the result is tiny.
        exponent = max(floor_log2_ratio(num, den), EMIN) - SIGNIFICAND_BITS
        shift = 1 - exponent
        if shift >= 0:
            left, right = difference << shift, bits_den * den
        else:
            left, right = difference, (bits_den * den) << (-shift)
        assert left <= right, "a nearest-mode result is more than half an ulp away"


def overflow_result(sign, mode):
    """What the format delivers when the rounded magnitude leaves the range."""
    if mode in (RNE, RNA):
        return assemble(sign, 0xFF, 0)  # +-Inf
    if mode == RTZ:
        return MAX_FINITE | (SIGN if sign else 0)  # +-maxFinite
    if mode == RDN:
        return assemble(1, 0xFF, 0) if sign else MAX_FINITE
    return assemble(0, 0xFF, 0) if not sign else (MAX_FINITE | SIGN)  # RUP


def pack_exact(sign, num, den, inexact, mode):
    """Pack the exactly representable value num/den (num > 0) into binary32.

    `inexact` says whether the packed value differs from the operation's exact
    result; it decides NX and, together with tininess, UF.
    """
    flags = FLAG_NX if inexact else 0
    exponent = floor_log2_ratio(num, den)

    # Overflow is decided on the value rounded with an unbounded exponent, which
    # is what the caller hands over here.
    if exponent > EMAX:
        return overflow_result(sign, mode), flags | FLAG_OF | FLAG_NX

    if exponent < EMIN:
        # Tiny: the value sits on the subnormal quantum, so this is exact.
        scale = 1 << (-SUBNORMAL_QUANTUM)
        fraction = (num * scale) // den
        if inexact:
            flags |= FLAG_UF
        return assemble(sign, 0, fraction), flags

    # significand = value / 2**(exponent - 23), always an exact integer because
    # the caller rounded the value onto this format's grid.
    if exponent >= SIGNIFICAND_BITS:
        divisor = den << (exponent - SIGNIFICAND_BITS)
        assert num % divisor == 0, "pack_exact expects a value on the format's grid"
        significand = num // divisor
    else:
        scaled = num << (SIGNIFICAND_BITS - exponent)
        assert scaled % den == 0, "pack_exact expects a value on the format's grid"
        significand = scaled // den
    if significand >= (1 << P):
        significand >>= 1
        exponent += 1
        if exponent > EMAX:
            return overflow_result(sign, mode), flags | FLAG_OF | FLAG_NX
    return assemble(sign, exponent + 127, significand & 0x007FFFFF), flags


def round_to_binary32(sign, num, den, mode):
    """Correctly round the exact non-zero value (-1)**sign * num/den."""
    if num == 0:
        return assemble(sign, 0, 0), 0
    exponent = floor_log2_ratio(num, den)
    quantum = max(exponent, EMIN) - SIGNIFICAND_BITS
    if quantum >= 0:
        scaled_num, scaled_den = num, den << quantum
    else:
        scaled_num, scaled_den = num << (-quantum), den
    rounded = abs(round_ratio_to_int(sign, scaled_num, scaled_den, mode))
    if quantum >= 0:
        rounded_num, rounded_den = rounded << quantum, 1
    else:
        rounded_num, rounded_den = rounded, 1 << (-quantum)
    inexact = rounded_num * den != num * rounded_den
    if rounded == 0:
        bits = assemble(sign, 0, 0)
        verify_rounding(sign, num, den, mode, bits)
        return bits, ((FLAG_NX | FLAG_UF) if inexact else 0)
    bits, flags = pack_exact(sign, rounded_num, rounded_den, inexact, mode)
    verify_rounding(sign, num, den, mode, bits)
    return bits, flags


def verify_square_root(num, den, mode, bits):
    """Self-check for sqrt, by squaring instead of comparing irrationals."""
    if is_nan(bits) or is_infinite(bits):
        return

    def compare_with_root(candidate):
        """-1/0/+1 comparing a non-negative pattern with sqrt(num/den)."""
        if is_nan(candidate) or is_infinite(candidate) or is_negative(candidate):
            return None
        _, candidate_num, candidate_den = exact_finite(candidate)
        left = candidate_num * candidate_num * den
        right = num * candidate_den * candidate_den
        if left < right:
            return -1
        return 1 if left > right else 0

    relation = compare_with_root(bits)
    if relation == 0:
        return  # the root is exactly representable
    key = order_key(bits)
    if mode in (RTZ, RDN):
        assert relation < 0, "a downward square root rounded up"
        neighbour = compare_with_root(from_order_key(key + 1))
        assert neighbour is None or neighbour > 0, \
            "the square root is not the last value below the exact one"
    elif mode == RUP:
        assert relation > 0, "roundTowardPositive did not round the root up"
        neighbour = compare_with_root(from_order_key(key - 1))
        assert neighbour is None or neighbour < 0, \
            "the square root is not the first value above the exact one"
    else:
        # |result - sqrt(x)| <= half an ulp, checked by squaring both sides.
        _, root_num, root_den = exact_finite(bits)
        exponent = floor_log2_ratio(num, den) // 2
        if exponent - 24 >= 0:
            half_num, half_den = (1 << (exponent - 24)), 1
        else:
            half_num, half_den = 1, (1 << (24 - exponent))
        low_num = root_num * half_den - half_num * root_den
        high_num = root_num * half_den + half_num * root_den
        common_den = root_den * half_den
        assert low_num >= 0, "the half-ulp window is not where the result sits"
        assert low_num * low_num * den <= num * common_den * common_den, \
            "the square root is more than half an ulp low"
        assert num * common_den * common_den <= high_num * high_num * den, \
            "the square root is more than half an ulp high"


def round_square_root(num, den, mode):
    """Correctly round sqrt(num/den) without ever computing an irrational."""
    result_exponent = floor_log2_ratio(num, den) // 2
    quantum = max(result_exponent, EMIN) - SIGNIFICAND_BITS
    if quantum >= 0:
        scaled_num, scaled_den = num, den << (2 * quantum)
    else:
        scaled_num, scaled_den = num << (-2 * quantum), den
    # The exact square of the scaled result is scaled_num / scaled_den.
    floor_value = math.isqrt(scaled_num // scaled_den)
    while (floor_value + 1) * (floor_value + 1) * scaled_den <= scaled_num:
        floor_value += 1
    while floor_value * floor_value * scaled_den > scaled_num:
        floor_value -= 1

    exact_square = scaled_num
    if floor_value * floor_value * scaled_den == exact_square:
        rounded = floor_value
    elif mode in (RTZ, RDN):
        rounded = floor_value
    elif mode == RUP:
        rounded = floor_value + 1
    else:
        # Compare sqrt(value) with floor + 1/2 by squaring both sides.
        left = 4 * exact_square
        right = scaled_den * (2 * floor_value + 1) * (2 * floor_value + 1)
        if left == right:
            rounded = floor_value + (1 if mode == RNA or (floor_value & 1) else 0)
        else:
            rounded = floor_value + (1 if left > right else 0)
    inexact = rounded * rounded * scaled_den != exact_square
    if quantum >= 0:
        rounded_num, rounded_den = rounded << quantum, 1
    else:
        rounded_num, rounded_den = rounded, 1 << (-quantum)
    if rounded == 0:
        bits = assemble(0, 0, 0)
        verify_square_root(num, den, mode, bits)
        return bits, ((FLAG_NX | FLAG_UF) if inexact else 0)
    bits, flags = pack_exact(0, rounded_num, rounded_den, inexact, mode)
    verify_square_root(num, den, mode, bits)
    return bits, flags


# --------------------------------------------------------------------------
# the operations
# --------------------------------------------------------------------------


def canonical_nan(*operands):
    """Canonical quiet NaN; NV when any of the operands is signaling."""
    flags = 0
    for bits in operands:
        if is_signaling_nan(bits):
            flags |= FLAG_NV
    return CANONICAL_NAN, flags


def add_or_subtract(a, b, mode, subtract):
    if subtract:
        b ^= SIGN
    if is_nan(a) or is_nan(b):
        return canonical_nan(a, b)
    if is_infinite(a) or is_infinite(b):
        if is_infinite(a) and is_infinite(b) and is_negative(a) != is_negative(b):
            return CANONICAL_NAN, FLAG_NV  # Inf - Inf
        return (a if is_infinite(a) else b), 0

    sign_a, num_a, den_a = exact_finite(a)
    sign_b, num_b, den_b = exact_finite(b)
    common = den_a * den_b
    total = num_a * den_b * (-1 if sign_a else 1) + num_b * den_a * (-1 if sign_b else 1)
    if total == 0:
        # x + (-x) is +0 except under roundTowardNegative; -0 + -0 stays -0.
        if sign_a and sign_b:
            return assemble(1, 0, 0), 0
        return assemble(1 if mode == RDN else 0, 0, 0), 0
    return round_to_binary32(1 if total < 0 else 0, abs(total), common, mode)


def multiply(a, b, mode):
    if is_nan(a) or is_nan(b):
        return canonical_nan(a, b)
    sign = 1 if is_negative(a) != is_negative(b) else 0
    if is_infinite(a) or is_infinite(b):
        if is_zero(a) or is_zero(b):
            return CANONICAL_NAN, FLAG_NV  # 0 x Inf
        return assemble(sign, 0xFF, 0), 0
    _, num_a, den_a = exact_finite(a)
    _, num_b, den_b = exact_finite(b)
    if num_a == 0 or num_b == 0:
        return assemble(sign, 0, 0), 0
    return round_to_binary32(sign, num_a * num_b, den_a * den_b, mode)


def divide(a, b, mode):
    if is_nan(a) or is_nan(b):
        return canonical_nan(a, b)
    sign = 1 if is_negative(a) != is_negative(b) else 0
    if is_infinite(a):
        return (CANONICAL_NAN, FLAG_NV) if is_infinite(b) else (assemble(sign, 0xFF, 0), 0)
    if is_infinite(b):
        return assemble(sign, 0, 0), 0
    _, num_a, den_a = exact_finite(a)
    _, num_b, den_b = exact_finite(b)
    if num_b == 0:
        if num_a == 0:
            return CANONICAL_NAN, FLAG_NV
        return assemble(sign, 0xFF, 0), FLAG_DZ
    if num_a == 0:
        return assemble(sign, 0, 0), 0
    return round_to_binary32(sign, num_a * den_b, den_a * num_b, mode)


def square_root(a, mode):
    if is_nan(a):
        return canonical_nan(a)
    if is_zero(a):
        return a, 0
    if is_negative(a):
        return CANONICAL_NAN, FLAG_NV  # negative finite and -Inf
    if is_infinite(a):
        return a, 0
    _, num, den = exact_finite(a)
    return round_square_root(num, den, mode)


def fused_multiply_add(a, b, c, mode):
    product_invalid = (is_infinite(a) and is_zero(b)) or (is_infinite(b) and is_zero(a))
    if is_nan(a) or is_nan(b) or is_nan(c):
        _, flags = canonical_nan(a, b, c)
        # An invalid product outranks a quiet NaN operand (plan 2.4).
        if product_invalid:
            flags |= FLAG_NV
        return CANONICAL_NAN, flags
    if product_invalid:
        return CANONICAL_NAN, FLAG_NV

    sign_a, num_a, den_a = exact_finite(a)
    sign_b, num_b, den_b = exact_finite(b)
    sign_c, num_c, den_c = exact_finite(c)

    if is_infinite(a) or is_infinite(b):
        product_sign = 1 if is_negative(a) != is_negative(b) else 0
        if is_infinite(c) and is_negative(c) != product_sign:
            return CANONICAL_NAN, FLAG_NV
        return assemble(product_sign, 0xFF, 0), 0
    if is_infinite(c):
        return c, 0

    common = den_a * den_b * den_c
    product = num_a * num_b * den_c * (-1 if sign_a ^ sign_b else 1)
    addend = num_c * den_a * den_b * (-1 if sign_c else 1)
    total = product + addend
    if total == 0:
        if (sign_a ^ sign_b) and sign_c:
            return assemble(1, 0, 0), 0
        return assemble(1 if mode == RDN else 0, 0, 0), 0
    return round_to_binary32(1 if total < 0 else 0, abs(total), common, mode)


def remainder(a, b):
    if is_nan(a) or is_nan(b):
        return canonical_nan(a, b)
    if is_infinite(a) or is_zero(b):
        return CANONICAL_NAN, FLAG_NV
    if is_infinite(b) or is_zero(a):
        return a, 0
    sign_a, num_a, den_a = exact_finite(a)
    sign_b, num_b, den_b = exact_finite(b)
    # n = nearest integer to a/b with ties to even; the remainder is exact.
    quotient_num = num_a * den_b
    quotient_den = den_a * num_b
    n = round_ratio_to_int(1 if sign_a != sign_b else 0, quotient_num, quotient_den, RNE)
    # a - n*b over the common denominator den_a*den_b, signs included.
    numerator = (num_a * den_b * (-1 if sign_a else 1)
                 - n * num_b * den_a * (-1 if sign_b else 1))
    if numerator == 0:
        return assemble(sign_a, 0, 0), 0
    return round_to_binary32(1 if numerator < 0 else 0, abs(numerator),
                             den_a * den_b, RNE)


def round_to_integral_exact(a, mode):
    if is_nan(a):
        return canonical_nan(a)
    if is_infinite(a) or is_zero(a):
        return a, 0
    sign, num, den = exact_finite(a)
    rounded = round_ratio_to_int(sign, num, den, mode)
    inexact = rounded * den != (-num if sign else num)
    flags = FLAG_NX if inexact else 0
    magnitude = abs(rounded)
    if magnitude == 0:
        return assemble(sign, 0, 0), flags
    return pack_exact(sign, magnitude, 1, inexact, mode)[0], flags


def compare(a, b):
    """Quiet relation packed as lt | eq<<1 | gt<<2 | un<<3 (plan 2.5)."""
    if is_nan(a) or is_nan(b):
        return REL_UN, (FLAG_NV if (is_signaling_nan(a) or is_signaling_nan(b)) else 0)
    if is_infinite(a) and is_infinite(b):
        if is_negative(a) != is_negative(b):
            return (REL_LT if is_negative(a) else REL_GT), 0
        return REL_EQ, 0
    if is_infinite(a):
        return (REL_LT if is_negative(a) else REL_GT), 0
    if is_infinite(b):
        return (REL_GT if is_negative(b) else REL_LT), 0
    if is_zero(a) and is_zero(b):
        return REL_EQ, 0
    sign_a, num_a, den_a = exact_finite(a)
    sign_b, num_b, den_b = exact_finite(b)
    left = num_a * den_b * (-1 if sign_a else 1)
    right = num_b * den_a * (-1 if sign_b else 1)
    if left < right:
        return REL_LT, 0
    if left > right:
        return REL_GT, 0
    return REL_EQ, 0


def classify(a):
    if is_nan(a):
        return CLASS_SNAN if is_signaling_nan(a) else CLASS_QNAN
    if is_infinite(a):
        return CLASS_NEG_INF if is_negative(a) else CLASS_POS_INF
    if is_zero(a):
        return CLASS_NEG_ZERO if is_negative(a) else CLASS_POS_ZERO
    if is_subnormal(a):
        return CLASS_NEG_SUBNORMAL if is_negative(a) else CLASS_POS_SUBNORMAL
    return CLASS_NEG_NORMAL if is_negative(a) else CLASS_POS_NORMAL


def min_max_number(a, b, maximum):
    """IEEE 754-2019 minimumNumber/maximumNumber (plan 2.5)."""
    flags = 0
    if is_signaling_nan(a) or is_signaling_nan(b):
        flags |= FLAG_NV
    nan_a, nan_b = is_nan(a), is_nan(b)
    if nan_a and nan_b:
        return CANONICAL_NAN, flags
    if nan_a:
        return b, flags
    if nan_b:
        return a, flags
    if is_zero(a) and is_zero(b):
        return ((a & b) if maximum else (a | b)), 0
    relation = compare(a, b)[0]
    if relation == REL_EQ:
        return a, 0
    if maximum:
        return (a if relation == REL_GT else b), 0
    return (a if relation == REL_LT else b), 0


def dispatch(op, a, b, c, mode):
    if op == OP_ADD:
        return add_or_subtract(a, b, mode, False)
    if op == OP_SUBTRACT:
        return add_or_subtract(a, b, mode, True)
    if op == OP_MULTIPLY:
        return multiply(a, b, mode)
    if op == OP_DIVIDE:
        return divide(a, b, mode)
    if op == OP_SQUARE_ROOT:
        return square_root(a, mode)
    if op == OP_FMA:
        return fused_multiply_add(a, b, c, mode)
    if op == OP_REMAINDER:
        return remainder(a, b)
    if op == OP_ROUND_TO_INTEGRAL_EXACT:
        return round_to_integral_exact(a, mode)
    raise ValueError(f"unsupported operation {op}")


# --------------------------------------------------------------------------
# operand pools
# --------------------------------------------------------------------------


def build_pool():
    """Hand-picked patterns: zeros, subnormal edges, binade edges, specials."""
    return [
        0x00000000, 0x80000000,                 # +0, -0
        0x00000001, 0x80000001,                 # smallest subnormals
        0x00000002, 0x00400000, 0x007FFFFF,     # subnormal middle, largest subnormal
        0x807FFFFF, 0x00400001,
        0x00800000, 0x80800000,                 # smallest normals
        0x00800001, 0x00FFFFFF, 0x01000000,     # normal edges
        0x3F800000, 0xBF800000,                 # +1, -1
        0x3F800001, 0x3F7FFFFF,                 # 1 + 1ulp, 1 - 1ulp
        0x40000000, 0xC0000000,                 # +2, -2
        0x3F000000, 0x3E800000,                 # 0.5, 0.25
        0x40400000, 0x40490FDB,                 # 3, pi
        0x4B000000, 0x4B7FFFFF,                 # 2^23, 2^24-1
        0x4CBEBC20, 0x4E6E6B28,                 # 1e8, 1e9
        0x5F000000, 0x7F7FFFFF,                 # 2^64, max finite
        0xFF7FFFFF, 0x7F800000, 0xFF800000,     # -max finite, +Inf, -Inf
        0x7FC00000, 0x7FC00001, 0xFFC00000,     # quiet NaNs
        0x7FA00000, 0x7F800001, 0xFFA00000,     # signaling NaNs
        0x33800000, 0x0D800000, 0x71000000,     # tiny and huge magnitudes
    ]


def build_triple_pool():
    """Triples that separate a fused multiply-add from mul-then-add."""
    return [
        (0x3F800000, 0x3F800001, 0xBF800000),   # 1 * (1+ulp) - 1
        (0x3F800001, 0x3F800001, 0xBF800000),
        (0x4B800001, 0x4B000001, 0xCB000000),
        (0x00800000, 0x3F000000, 0x00000001),
        (0x00000001, 0x3F800000, 0x00000001),
        (0x7F7FFFFF, 0x3F800001, 0xBF7FFFFF),
        (0x3F800000, 0x3F800000, 0x80000000),
        (0x00000000, 0x7F800000, 0x7FC00000),   # 0 x Inf + qNaN -> NV
        (0x00000000, 0x7F800000, 0x3F800000),
        (0x7F800000, 0x7F800000, 0xFF800000),   # Inf + -Inf
        (0x7FA00000, 0x3F800000, 0x3F800000),   # signaling operand
        (0x00000001, 0x00000001, 0x00000000),   # subnormal product
        (0x00800000, 0xBF800000, 0x00800000),
    ]


def random_bits(rng, count, kind="any"):
    values = []
    for _ in range(count):
        if kind == "any":
            values.append(rng.getrandbits(32))
        elif kind == "normal":
            while True:
                value = rng.getrandbits(32)
                if 1 <= ((value >> 23) & 0xFF) <= 0xFE:
                    values.append(value)
                    break
        elif kind == "tiny":
            values.append(rng.getrandbits(32) & 0x807FFFFF)
        elif kind == "close":
            base = rng.choice(build_pool())
            values.append((base + rng.randint(-4, 4)) & 0xFFFFFFFF)
        else:
            raise ValueError(kind)
    return values


# --------------------------------------------------------------------------
# vector collection
# --------------------------------------------------------------------------


def collect_vectors():
    rng = random.Random(0x46333231)  # fixed seed: the file must be reproducible
    pool = build_pool()
    vectors = []
    cross_checked = 0

    binary_pairs = []
    for index, left in enumerate(pool):
        binary_pairs.append((left, pool[(index * 7 + 3) % len(pool)]))
        binary_pairs.append((left, pool[(index * 11 + 5) % len(pool)]))
    binary_pairs += list(zip(random_bits(rng, 90), random_bits(rng, 90)))
    binary_pairs += list(zip(random_bits(rng, 30, "normal"), random_bits(rng, 30, "normal")))
    binary_pairs += list(zip(random_bits(rng, 20, "close"), random_bits(rng, 20, "close")))
    binary_pairs += list(zip(random_bits(rng, 20, "tiny"), random_bits(rng, 20, "normal")))

    for op in (OP_ADD, OP_SUBTRACT, OP_MULTIPLY, OP_DIVIDE):
        for mode in ALL_ROUNDINGS:
            for a, b in binary_pairs:
                bits, flags = dispatch(op, a, b, 0, mode)
                vectors.append((op, mode, flags, a, b, 0, bits))
                if mode == RNE:
                    cross_checked += cross_check(op, a, b, bits)

    unary_values = (pool + random_bits(rng, 60) + random_bits(rng, 30, "normal")
                    + random_bits(rng, 20, "tiny") + random_bits(rng, 20, "close"))
    for mode in ALL_ROUNDINGS:
        for a in unary_values:
            bits, flags = dispatch(OP_SQUARE_ROOT, a, 0, 0, mode)
            vectors.append((OP_SQUARE_ROOT, mode, flags, a, 0, 0, bits))
            if mode == RNE:
                cross_checked += cross_check(OP_SQUARE_ROOT, a, 0, bits)

    triples = build_triple_pool()
    triples += [(rng.choice(pool), rng.choice(pool), rng.choice(pool)) for _ in range(90)]
    triples += [(rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32))
                for _ in range(40)]
    for mode in ALL_ROUNDINGS:
        for a, b, c in triples:
            bits, flags = dispatch(OP_FMA, a, b, c, mode)
            vectors.append((OP_FMA, mode, flags, a, b, c, bits))

    for a, b in binary_pairs:
        bits, flags = dispatch(OP_REMAINDER, a, b, 0, RNE)
        vectors.append((OP_REMAINDER, RNE, flags, a, b, 0, bits))
    for mode in ALL_ROUNDINGS:
        for a in unary_values[:70]:
            bits, flags = dispatch(OP_ROUND_TO_INTEGRAL_EXACT, a, 0, 0, mode)
            vectors.append((OP_ROUND_TO_INTEGRAL_EXACT, mode, flags, a, 0, 0, bits))
    for a, b in binary_pairs[:110]:
        packed, flags = compare(a, b)
        vectors.append((OP_COMPARE, RNE, flags, a, b, 0, packed))
    for a in unary_values:
        vectors.append((OP_CLASSIFY, RNE, 0, a, 0, 0, classify(a)))
    for a, b in binary_pairs[:120]:
        for op in (OP_MINIMUM_NUMBER, OP_MAXIMUM_NUMBER):
            bits, flags = min_max_number(a, b, op == OP_MAXIMUM_NUMBER)
            vectors.append((op, RNE, flags, a, b, 0, bits))

    return vectors, cross_checked


def to_double(bits):
    sign = -1.0 if is_negative(bits) else 1.0
    exponent = (bits >> 23) & 0xFF
    fraction = bits & 0x007FFFFF
    if exponent == 0:
        return sign * fraction * 2.0 ** -149
    if exponent == 0xFF:
        return sign * float("inf") if fraction == 0 else float("nan")
    return sign * (1.0 + fraction / (1 << 23)) * 2.0 ** (exponent - 127)


def cross_check(op, a, b, bits):
    """Second opinion for round-to-nearest: CPython doubles, one rounding.

    Returns 1 when the check ran and agreed, 0 when it did not apply.
    """
    if is_nan(a) or is_nan(b):
        return 0
    try:
        if op == OP_ADD:
            value = to_double(a) + to_double(b)
        elif op == OP_SUBTRACT:
            value = to_double(a) - to_double(b)
        elif op == OP_MULTIPLY:
            value = to_double(a) * to_double(b)
        elif op == OP_DIVIDE:
            value = to_double(a) / to_double(b)
        elif op == OP_SQUARE_ROOT:
            root = to_double(a)
            if root < 0:
                return 0
            value = math.sqrt(root)
        else:
            return 0
    except (ZeroDivisionError, OverflowError, ValueError):
        return 0
    if math.isnan(value):
        expected = CANONICAL_NAN
    else:
        try:
            expected = struct.unpack("<I", struct.pack("<f", value))[0]
        except OverflowError:
            expected = INFINITY | (SIGN if value < 0 else 0)
    if expected != bits:
        raise AssertionError(
            f"oracle disagrees with double arithmetic: {OP_NAMES[op]} "
            f"0x{a:08X} 0x{b:08X} -> 0x{bits:08X} vs 0x{expected:08X}")
    return 1


# --------------------------------------------------------------------------
# output
# --------------------------------------------------------------------------


def render(vectors, cross_checked):
    counts = {}
    for vector in vectors:
        counts[vector[0]] = counts.get(vector[0], 0) + 1
    lines = [
        "/* Generated by tools/float-vectors.py - do not edit by hand.",
        "",
        "   Golden vectors for the Float Ops binary32 kernel.  Every expectation comes",
        "   from an independent oracle (exact rational arithmetic in Python; see the",
        "   generator's docstring), never from the C++ kernel under test.",
        "",
        "   `bits` is the expected result pattern.  For op_compare it packs the",
        "   relation as lt|eq<<1|gt<<2|un<<3 (plan 2.5); for op_classify it is the",
        "   ten-bit class word in the plan's bit order. */",
        "",
        "#ifndef float_ops_fp32_vectors_generated_hpp",
        "#define float_ops_fp32_vectors_generated_hpp",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace tcfp_vectors {",
        "",
        "enum Op : uint16_t {",
        "    op_add = 1,",
        "    op_subtract = 2,",
        "    op_multiply = 3,",
        "    op_divide = 4,",
        "    op_square_root = 5,",
        "    op_fused_multiply_add = 6,",
        "    op_remainder = 7,",
        "    op_round_to_integral_exact = 8,",
        "    op_compare = 9,",
        "    op_classify = 10,",
        "    op_minimum_number = 11,",
        "    op_maximum_number = 12,",
        "};",
        "",
        "/* Rounding codes are the archive values of tcfp::FPRounding. */",
        "struct Vector {",
        "    uint16_t op;",
        "    uint8_t rounding;",
        "    uint8_t flags;",
        "    uint32_t a;",
        "    uint32_t b;",
        "    uint32_t c;",
        "    uint32_t bits;",
        "};",
        "",
        f"inline constexpr size_t kFp32VectorCount = {len(vectors)};",
        f"inline constexpr size_t kFp32CrossChecked = {cross_checked};",
        "",
        "inline constexpr Vector kFp32Vectors[] = {",
    ]
    for op, mode, flags, a, b, c, bits in vectors:
        lines.append(f"    {{ {op}, {mode}, 0x{flags:02X}, 0x{a:08X}U, 0x{b:08X}U, "
                     f"0x{c:08X}U, 0x{bits:08X}U }},")
    lines += [
        "};",
        "",
    ]
    for op in sorted(counts):
        lines.append(f"/* {OP_NAMES[op]}: {counts[op]} vectors */")
    lines += [
        "",
        "}  // namespace tcfp_vectors",
        "",
        "#endif",
        "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="generate Float Ops golden vectors")
    parser.add_argument("--output", default="tests/data/float32/fp32-vectors.generated.hpp")
    parser.add_argument("--check", action="store_true",
                        help="verify the file on disk matches the oracle")
    parser.add_argument("--quiet", action="store_true")
    arguments = parser.parse_args()

    vectors, cross_checked = collect_vectors()
    text = render(vectors, cross_checked)
    path = pathlib.Path(arguments.output)

    if arguments.check:
        if not path.exists():
            print(f"FAIL {path} is missing; run tools/float-vectors.py")
            return 1
        if path.read_text(encoding="utf-8") != text:
            print(f"FAIL {path} is out of date; run tools/float-vectors.py")
            return 1
        if not arguments.quiet:
            print(f"PASS float vectors up to date: {len(vectors)} vectors, "
                  f"{cross_checked} cross-checked against double arithmetic")
        return 0

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if not arguments.quiet:
        print(f"wrote {path}: {len(vectors)} vectors, "
              f"{cross_checked} cross-checked against double arithmetic")
    return 0


if __name__ == "__main__":
    sys.exit(main())
