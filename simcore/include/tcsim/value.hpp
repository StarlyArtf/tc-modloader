#pragma once
/* L0 of the sandbox simulator (docs/PLAN-sandbox-simulator.md section 4.1):
   four-state logic values with drive strength, and the resolution table that
   turns several drivers on one net into one value per bit.

   The semantics are deliberately the ones a Verilog simulator implements, so
   that `simcore` can be cross-checked against iverilog (section 8.2): a net
   with no driver is Z, a wire driven by an unevaluated gate is X, two drivers
   that disagree at the same strength resolve to X, and the stronger driver
   wins when they differ.  tools/simcore-iverilog.ps1 runs that comparison. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tcsim {

enum class Logic : uint8_t { kZero = 0, kOne = 1, kX = 2, kZ = 3 };
enum class Strength : uint8_t { kHighZ = 0, kWeak = 1, kStrong = 2 };

inline char logicChar(Logic value) {
    switch (value) {
        case Logic::kZero: return '0';
        case Logic::kOne: return '1';
        case Logic::kX: return 'x';
        case Logic::kZ: return 'z';
    }
    return 'x';
}
inline const char* strengthName(Strength strength) {
    switch (strength) {
        case Strength::kHighZ: return "highz";
        case Strength::kWeak: return "weak";
        case Strength::kStrong: return "strong";
    }
    return "strong";
}
inline bool parseLogic(char text, Logic* out) {
    if (!out) return false;
    switch (text) {
        case '0': *out = Logic::kZero; return true;
        case '1': *out = Logic::kOne; return true;
        case 'x': case 'X': *out = Logic::kX; return true;
        case 'z': case 'Z': *out = Logic::kZ; return true;
        default: return false;
    }
}
inline bool parseStrength(const std::string& text, Strength* out) {
    if (!out) return false;
    if (text == "strong") { *out = Strength::kStrong; return true; }
    if (text == "weak") { *out = Strength::kWeak; return true; }
    if (text == "highz" || text == "high-z") { *out = Strength::kHighZ; return true; }
    return false;
}

/* One driver's contribution to one net: the value it drives and how hard.  A
   Z driver contributes nothing at all - that is what makes a tristate bus a
   bus instead of a short. */
struct Drive {
    Logic logic = Logic::kZ;
    Strength strength = Strength::kHighZ;
};

/* Resolves one net bit.  The rule, strongest first:

     - drivers that drive Z are not drivers (they are "off");
     - otherwise look at the maximum strength any driver uses on this bit;
       when only one value is driven at that strength it wins (0, 1 or X);
       when two or three values tie at that strength the result is X;
     - with nothing left to drive the bit, the result is Z.

   That reproduces the table the plan asks for (same value same strength ->
   value, same value different strength -> the stronger one, opposite values
   same strength -> X, nothing driving -> Z) and it matches iverilog's own
   resolution for the cases the cross-check covers. */
inline Drive resolveBit(const Drive* drivers, size_t count) {
    int best_by_logic[4] = {-1, -1, -1, -1};
    for (size_t index = 0; index < count; ++index) {
        const Drive& driver = drivers[index];
        if (driver.logic == Logic::kZ) continue;
        const int strength = static_cast<int>(driver.strength);
        int& best = best_by_logic[static_cast<size_t>(driver.logic)];
        if (strength > best) best = strength;
    }
    int strongest = -1;
    for (int logic = 0; logic < 4; ++logic) {
        if (best_by_logic[logic] > strongest) strongest = best_by_logic[logic];
    }
    if (strongest < 0) return Drive{Logic::kZ, Strength::kHighZ};
    int winner = 0;
    for (int logic = 0; logic < 4; ++logic) {
        if (best_by_logic[logic] == strongest) ++winner;
    }
    if (winner != 1) return Drive{Logic::kX, Strength::kStrong};
    for (int logic = 0; logic < 4; ++logic) {
        if (best_by_logic[logic] == strongest) {
            Drive out;
            out.logic = static_cast<Logic>(logic);
            out.strength = static_cast<Strength>(strongest);
            return out;
        }
    }
    return Drive{Logic::kX, Strength::kStrong};
}

/* Bit vector, most significant bit first, sized in bits.  Pins wider than one
   bit (the game's Splitter/Maker/adder pins) use this; single-bit nets use a
   one-bit vector, so there is only one code path. */
class BitVector {
public:
    BitVector() = default;
    explicit BitVector(size_t bits, Logic fill = Logic::kZ) : bits_(bits, fill) {}

    /* Reads `text` (MSB first, `x`/`z` allowed) into a vector of `bits` bits.
       Shorter text is extended on the left with the leftmost character. */
    static BitVector fromText(const std::string& text, size_t bits) {
        BitVector out(bits, Logic::kZ);
        const size_t count = bits < text.size() ? bits : text.size();
        for (size_t index = 0; index < count; ++index) {
            const char character = text[text.size() - 1 - index];
            Logic parsed = Logic::kX;
            if (parseLogic(character, &parsed)) out.bits_[bits - 1 - index] = parsed;
        }
        if (text.size() < bits) {
            Logic fill = Logic::kZ;
            parseLogic(text.empty() ? 'z' : text[0], &fill);
            for (size_t index = text.size(); index < bits; ++index) out.bits_[bits - 1 - index] = fill;
        }
        return out;
    }

    size_t size() const { return bits_.size(); }
    bool empty() const { return bits_.empty(); }
    const std::vector<Logic>& bits() const { return bits_; }
    Logic operator[](size_t index) const { return index < bits_.size() ? bits_[index] : Logic::kX; }
    Logic& at(size_t index) { return bits_[index]; }

    void resize(size_t bits, Logic fill = Logic::kZ) { bits_.resize(bits, fill); }
    void fill(Logic value) {
        for (Logic& bit : bits_) bit = value;
    }
    bool isKnown() const {
        for (Logic bit : bits_) {
            if (bit == Logic::kX || bit == Logic::kZ) return false;
        }
        return true;
    }
    bool holdsValue(Logic value) const {
        for (Logic bit : bits_) {
            if (bit != value) return false;
        }
        return true;
    }
    /* MSB first, so it can be printed next to VCD output without reordering. */
    std::string toString() const {
        std::string text;
        text.reserve(bits_.size());
        for (Logic bit : bits_) text.push_back(logicChar(bit));
        return text;
    }
    /* 0 for any bit that is not 0/1, so a caller that needs the number must
       check isKnown() first - the same contract as the game's own state slots. */
    uint64_t toUint64() const {
        uint64_t value = 0;
        for (Logic bit : bits_) {
            value <<= 1;
            if (bit == Logic::kOne) value |= 1u;
        }
        return value;
    }

    bool operator==(const BitVector& other) const { return bits_ == other.bits_; }
    bool operator!=(const BitVector& other) const { return !(*this == other); }

private:
    std::vector<Logic> bits_;
};

/* Bitwise primitives for one bit, with the standard primitive truth tables:
   a Z input is an unknown to a gate (NOT z = x), 0 dominates an AND and 1
   dominates an OR whatever the other input is. */
inline Logic logicNot(Logic value) {
    if (value == Logic::kZero) return Logic::kOne;
    if (value == Logic::kOne) return Logic::kZero;
    return Logic::kX;
}
inline Logic logicAnd(Logic left, Logic right) {
    if (left == Logic::kZero || right == Logic::kZero) return Logic::kZero;
    if (left == Logic::kOne && right == Logic::kOne) return Logic::kOne;
    return Logic::kX;
}
inline Logic logicOr(Logic left, Logic right) {
    if (left == Logic::kOne || right == Logic::kOne) return Logic::kOne;
    if (left == Logic::kZero && right == Logic::kZero) return Logic::kZero;
    return Logic::kX;
}
inline Logic logicXor(Logic left, Logic right) {
    if (left == Logic::kX || left == Logic::kZ || right == Logic::kX || right == Logic::kZ) return Logic::kX;
    return left == right ? Logic::kZero : Logic::kOne;
}
inline Logic logicAndAll(const Logic* values, size_t count) {
    Logic out = Logic::kOne;
    for (size_t index = 0; index < count; ++index) out = logicAnd(out, values[index]);
    return out;
}
inline Logic logicOrAll(const Logic* values, size_t count) {
    Logic out = Logic::kZero;
    for (size_t index = 0; index < count; ++index) out = logicOr(out, values[index]);
    return out;
}
inline Logic logicXorAll(const Logic* values, size_t count) {
    Logic out = Logic::kZero;
    for (size_t index = 0; index < count; ++index) out = logicXor(out, values[index]);
    return out;
}
/* The Verilog conditional operator, bit by bit: an unknown select bit yields
   X unless both branches agree on a value (1 ? x : 1 is 1). */
inline Logic logicMux(Logic select, Logic when_one, Logic when_zero) {
    if (select == Logic::kOne) return when_one;
    if (select == Logic::kZero) return when_zero;
    if (when_one == Logic::kZ) when_one = Logic::kX;
    if (when_zero == Logic::kZ) when_zero = Logic::kX;
    if (when_one == when_zero) return when_one;
    return Logic::kX;
}

}  // namespace tcsim
