#pragma once
/* L3's behavior half (docs/PLAN-sandbox-simulator.md section 4.5): the device
   library.  Behavior and timing are separate on purpose - a device knows what
   it computes, its arcs know how long it takes - so a timing library can be
   swapped underneath without touching a truth table.

   The names here are the ones the cross-check and the unit tests use.  The
   game's 125 prototypes map onto this set in S1/S3; anything not covered must
   be reported as unsupported rather than approximated (red line 6). */

#include "netlist.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tcsim {

namespace detail {

inline std::string delayKey(ArcDelay delay, DelayKind kind) {
    return std::to_string(delay.tplh) + "-" + std::to_string(delay.tphl) + "-" + std::to_string(static_cast<int>(kind));
}

/* Builds the Verilog statement for an N-input reduce gate: "%Y% = %A% & %B%". */
inline std::string reduceTemplate(size_t input_count, char op, bool invert) {
    std::string expression;
    for (size_t index = 0; index < input_count; ++index) {
        expression += "%";
        expression.push_back(static_cast<char>('A' + static_cast<int>(index)));
        expression += "%";
        if (index + 1 < input_count) {
            expression.push_back(' ');
            expression.push_back(op == '^' ? '^' : op);
            expression += " ";
        }
    }
    std::string statement = "assign %strength%#(%rise%,%fall%) %Y% = ";
    if (invert) statement += "~(";
    statement += expression;
    if (invert) statement += ")";
    statement += ";";
    return statement;
}

inline std::vector<BitVector> gateOutputs(const std::vector<BitVector>& inputs, Logic (*reduce)(const Logic*, size_t),
                                          bool invert, bool is_not) {
    size_t bits = 0;
    for (const BitVector& input : inputs) {
        if (input.size() == 0) continue;
        bits = bits == 0 ? input.size() : (input.size() < bits ? input.size() : bits);
    }
    std::vector<BitVector> outputs(1, BitVector(bits, Logic::kX));
    if (bits == 0) return outputs;
    std::vector<Logic> slice(inputs.size(), Logic::kX);
    for (size_t bit = 0; bit < bits; ++bit) {
        for (size_t index = 0; index < inputs.size(); ++index) slice[index] = inputs[index][bit];
        Logic value = is_not ? logicNot(slice[0]) : reduce(slice.data(), slice.size());
        if (invert && !is_not) value = logicNot(value);
        outputs[0].at(bit) = value;
    }
    return outputs;
}

/* One bit per bit of a mux: `select` is a single bit, `when_one` is the branch
   it picks when it is 1 and `when_zero` the other one.  The Verilog
   conditional operator, which is also what the game's own `byte_mux` test
   demands (`select == 1` picks B) - and an unknown select still resolves the
   bits the two branches agree on. */
inline BitVector muxBits(Logic select, const BitVector& when_one, const BitVector& when_zero) {
    const size_t bits = when_one.size() < when_zero.size() ? when_one.size() : when_zero.size();
    BitVector out(bits, Logic::kX);
    for (size_t bit = 0; bit < bits; ++bit) out.at(bit) = logicMux(select, when_one[bit], when_zero[bit]);
    return out;
}

}  // namespace detail

/* An N-input AND/OR/XOR family gate with a uniform arc delay from every input
   to the output.  Covers and/or/xor/nand/nor/xnor for two and three inputs
   plus `buf`. */
inline const DeviceSpec& logicGate(const std::string& name, size_t input_count, char op, bool invert,
                                   ArcDelay delay = ArcDelay{}, DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const std::string key = name + "-" + detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = name;
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->default_delay = delay;
    for (size_t index = 0; index < input_count; ++index) {
        spec->inputs.push_back(PinSpec{std::string(1, static_cast<char>('A' + static_cast<int>(index))), 1});
    }
    spec->outputs.push_back(PinSpec{"Y", 1});
    for (size_t index = 0; index < input_count; ++index) {
        spec->arcs.push_back(Arc{static_cast<uint16_t>(index), 0, delay});
    }
    if (op == '&') {
        spec->behavior = [](const EvalContext&, const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
            outputs = detail::gateOutputs(inputs, logicAndAll, false, false);
        };
    } else if (op == '|') {
        spec->behavior = [](const EvalContext&, const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
            outputs = detail::gateOutputs(inputs, logicOrAll, false, false);
        };
    } else {
        spec->behavior = [](const EvalContext&, const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
            outputs = detail::gateOutputs(inputs, logicXorAll, false, false);
        };
    }
    if (invert) {
        Behavior plain = spec->behavior;
        spec->behavior = [plain](const EvalContext& context, const std::vector<BitVector>& inputs,
                                 std::vector<BitVector>& outputs) {
            plain(context, inputs, outputs);
            for (BitVector& output : outputs) {
                for (size_t bit = 0; bit < output.size(); ++bit) output.at(bit) = logicNot(output[bit]);
            }
        };
    }
    spec->verilog = detail::reduceTemplate(input_count, op, invert);
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

inline const DeviceSpec& deviceNot(ArcDelay delay = ArcDelay{}, DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const std::string key = "not-" + detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "not";
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"A", 1});
    spec->outputs.push_back(PinSpec{"Y", 1});
    spec->default_delay = delay;
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->behavior = [](const EvalContext&, const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
        outputs = detail::gateOutputs(inputs, logicAndAll, false, true);
    };
    spec->verilog = "assign %strength%#(%rise%,%fall%) %Y% = ~%A%;";
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

inline const DeviceSpec& deviceBuffer(ArcDelay delay = ArcDelay{}, DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const std::string key = "buf-" + detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "buf";
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"A", 1});
    spec->outputs.push_back(PinSpec{"Y", 1});
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->default_delay = delay;
    spec->behavior = [](const EvalContext&, const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
        outputs.assign(1, inputs.empty() ? BitVector(1, Logic::kX) : inputs[0]);
    };
    spec->verilog = "assign %strength%#(%rise%,%fall%) %Y% = %A%;";
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* The game's word Mux (kind 0x2a): S selects B when it is 1 and A otherwise.
   Only the select is one bit wide - the game's own prototype table says so,
   `0x2a name="Mux" inputs=3 outputs=1 in0=(-1,-1,w1) in1=(-1,0,wauto)
   in2=(-1,1,wauto) out0=(2,0,wauto)` (build/kinds.txt, re-dumped 2026-09-29),
   and a live board record agrees: the wire that lands on in0 is the one-bit
   one.  The data pins are auto width, so one spec exists per resolved width,
   the way the tri-state switch and the splitter do it; the board builder reads
   the width the game resolved into the record (`+0xe0`) or, failing that, the
   widest wire touching a data pin. */
inline const DeviceSpec& deviceMux(uint16_t bits, ArcDelay delay = ArcDelay{},
                                   DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    const std::string key = "mux-" + std::to_string(width) + "-" + detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "mux" + std::to_string(width);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"S", 1});
    spec->inputs.push_back(PinSpec{"A", width});
    spec->inputs.push_back(PinSpec{"B", width});
    spec->outputs.push_back(PinSpec{"Y", width});
    spec->default_delay = delay;
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->arcs.push_back(Arc{1, 0, delay});
    spec->arcs.push_back(Arc{2, 0, delay});
    spec->behavior = [width](const EvalContext&, const std::vector<BitVector>& inputs,
                             std::vector<BitVector>& outputs) {
        const Logic select = inputs.empty() || inputs[0].size() == 0 ? Logic::kX : inputs[0][0];
        const BitVector when_one = inputs.size() > 2 ? inputs[2] : BitVector(width, Logic::kX);
        const BitVector when_zero = inputs.size() > 1 ? inputs[1] : BitVector(width, Logic::kX);
        outputs.assign(1, detail::muxBits(select, when_one, when_zero));
    };
    spec->verilog = "assign %strength%#(%rise%,%fall%) %Y% = %S% ? %B% : %A%;";
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* The one-bit member of the same family, which is what the built-in name list
   publishes as "mux2". */
inline const DeviceSpec& deviceMux2(ArcDelay delay = ArcDelay{}, DelayKind kind = DelayKind::kInertial) {
    return deviceMux(1, delay, kind);
}

/* The game's Bit/Word Switch is a tri-state buffer: S=1 drives A, while S=0
   disconnects the output (Z).  Word switches carry the width stored in the
   component record, so the spec is cached per width as well as per delay. */
inline const DeviceSpec& deviceTriState(uint16_t bits, ArcDelay delay = ArcDelay{},
                                        DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    const std::string key = "tristate-" + std::to_string(width) + "-" + detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "tristate" + std::to_string(width);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"S", 1});
    spec->inputs.push_back(PinSpec{"A", width});
    spec->outputs.push_back(PinSpec{"Y", width});
    spec->default_delay = delay;
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->arcs.push_back(Arc{1, 0, delay});
    spec->behavior = [width](const EvalContext&, const std::vector<BitVector>& inputs,
                             std::vector<BitVector>& outputs) {
        BitVector out(width, Logic::kX);
        const Logic enable = inputs.empty() || inputs[0].size() == 0 ? Logic::kX : inputs[0][0];
        if (enable == Logic::kZero) {
            out = BitVector(width, Logic::kZ);
        } else if (enable == Logic::kOne && inputs.size() > 1) {
            out = inputs[1];
            out.resize(width, Logic::kX);
        }
        outputs.assign(1, out);
    };
    spec->verilog = "assign %strength%#(%rise%,%fall%) %Y% = %S% ? %A% : " +
                    std::to_string(width) + "'bz;";
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* A constant driver, for reset and tie-off: no inputs, one output. */
inline const DeviceSpec& deviceConstant(Logic value, Tick delay = 0, DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const std::string key = std::string("const-") + logicChar(value) + "-" + std::to_string(delay) + "-" +
                            std::to_string(static_cast<int>(kind));
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = std::string("const") + logicChar(value);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->outputs.push_back(PinSpec{"Y", 1});
    spec->default_delay = ArcDelay{delay, delay};
    spec->behavior = [value](const EvalContext&, const std::vector<BitVector>&, std::vector<BitVector>& outputs) {
        BitVector out(1, value);
        outputs.assign(1, out);
    };
    spec->verilog = std::string("assign %strength%#(%rise%,%fall%) %Y% = 1'b") + logicChar(value) + ";";
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* The game's Splitter and Maker, one bit per leaf pin.

   **Which leaf gets which bit** is the one thing about these two the prototype
   table does not state: it lists the output offsets (`(1,-1)`, `(1,0)`, `(1,1)`,
   `(1,2)`) and the original widths, but not whether the top leaf is the most or
   the least significant bit.  The convention taken here is **top leaf = most
   significant**, i.e. leaf 0 is the bit the game draws first and the Verilog
   export writes `%Y0% = %A%[w-1]`.  Three reasons:

     1. it makes Splitter and Maker exact inverses, which is what the verified
        `bus_split` circuit shows (`simcore` and Icarus Verilog agree there);
     2. it is the same "first declared pin is the high end" reading that the
        2-in Concatenator's own declared widths force (`0x72`: `in0` 2 bits, the
        `out0` 4 bits, and the campaign builds words with it);
     3. it is a **naming** choice between two readings that both round-trip, not
        an invented behaviour: no bit is moved anywhere the game does not move
        it, and a board that only splits and re-joins is identical either way.

   It is still listed as unmeasured in
   docs/research/simcore-kind-coverage.md, because a board that splits a word and
   then feeds one leaf somewhere else *would* notice.  Measuring it needs one
   real board; until then this is the documented default, not a claim. */
inline const DeviceSpec& deviceSplitter(uint16_t bits, DelayKind kind = DelayKind::kInertial) {    static std::map<uint16_t, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    (void)kind; /* the splitter's delay comes from its arcs, which the caller fills in */
    const auto found = cache.find(width);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "splitter" + std::to_string(width);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = DelayKind::kInertial;
    spec->inputs.push_back(PinSpec{"A", width});
    for (uint16_t bit = 0; bit < width; ++bit) {
        spec->outputs.push_back(PinSpec{"Y" + std::to_string(bit), 1});
        spec->arcs.push_back(Arc{0, bit, spec->default_delay});
    }
    spec->behavior = [width](const EvalContext&, const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
        outputs.assign(width, BitVector(1, Logic::kX));
        if (inputs.empty()) return;
        for (uint16_t bit = 0; bit < width; ++bit) outputs[bit].at(0) = inputs[0][bit];
    };
    for (uint16_t bit = 0; bit < width; ++bit) {
        spec->verilog += "assign %strength%#(%rise%,%fall%) %Y" + std::to_string(bit) + "% = %A%[" +
                         std::to_string(width - 1 - bit) + "];\n";
    }
    const auto inserted = cache.emplace(width, std::move(spec));
    return *inserted.first->second;
}

/* The one Splitter whose halves are words: "Splits a (2n)-bit value into two
   n-bit values" (the game's own description of kind 0x2f).  The prototype
   declares its input width as a sentinel and its two outputs `wauto`, so the
   halved width is resolved from the wires; the top output is the high half,
   the same "top leaf is the most significant" convention the bit-leaf
   Splitter/Maker pair uses. */
inline const DeviceSpec& deviceWordSplit(uint16_t half_bits, ArcDelay delay = ArcDelay{},
                                         DelayKind kind = DelayKind::kInertial) {
    static std::map<uint16_t, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t half = half_bits ? half_bits : 1;
    (void)kind;
    const auto found = cache.find(half);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "wordsplit" + std::to_string(half);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = DelayKind::kInertial;
    spec->inputs.push_back(PinSpec{"A", static_cast<uint16_t>(half * 2)});
    spec->outputs.push_back(PinSpec{"Y0", half});
    spec->outputs.push_back(PinSpec{"Y1", half});
    spec->default_delay = delay;
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->arcs.push_back(Arc{0, 1, delay});
    spec->behavior = [half](const EvalContext&, const std::vector<BitVector>& inputs,
                            std::vector<BitVector>& outputs) {
        outputs.assign(2, BitVector(half, Logic::kX));
        if (inputs.empty() || inputs[0].size() < half * 2) return;
        for (uint16_t bit = 0; bit < half; ++bit) {
            outputs[0].at(bit) = inputs[0][bit];              /* the top half */
            outputs[1].at(bit) = inputs[0][half + bit];       /* the bottom half */
        }
    };
    spec->verilog = "assign %strength%#(%rise%,%fall%) %Y0% = %A%[" +
                    std::to_string(half * 2 - 1) + ":" + std::to_string(half) + "];\n" +
                    "assign %strength%#(%rise%,%fall%) %Y1% = %A%[" +
                    std::to_string(half - 1) + ":0];";
    const auto inserted = cache.emplace(half, std::move(spec));
    return *inserted.first->second;
}

/* A Splitter whose leaves are words rather than bits: "Splits a (4n)-bit value
   into four n-bit values" (0x63; 0x2f is the two-way one, 0x64 the eight-way).
   Those widths are sentinels in the prototype table, so the leaf width comes off
   the board - measured on the player's own board: `kind 0x2F at 138,-2 … in0 64
   bits -> out0 32, out1 32`, and the record's own `+0xe0` says 32, which is the
   leaf width.  The topmost output is the most significant slice, the same
   convention the bit-leaf Splitter and the two-way word Splitter use. */
inline const DeviceSpec& deviceWordSplitter(uint16_t leaves, uint16_t half_bits, ArcDelay delay = ArcDelay{},
                                            DelayKind kind = DelayKind::kInertial) {
    static std::map<std::pair<uint16_t, uint16_t>, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t count = leaves ? leaves : 2;
    const uint16_t half = half_bits ? half_bits : 1;
    (void)kind;
    const auto key = std::make_pair(count, half);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "wordsplit" + std::to_string(count) + "x" + std::to_string(half);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = DelayKind::kInertial;
    spec->inputs.push_back(PinSpec{"A", static_cast<uint16_t>(half * count)});
    for (uint16_t leaf = 0; leaf < count; ++leaf) {
        spec->outputs.push_back(PinSpec{"Y" + std::to_string(leaf), half});
        spec->arcs.push_back(Arc{0, leaf, delay});
    }
    spec->default_delay = delay;
    spec->behavior = [count, half](const EvalContext&, const std::vector<BitVector>& inputs,
                                   std::vector<BitVector>& outputs) {
        outputs.assign(count, BitVector(half, Logic::kX));
        if (inputs.empty() || inputs[0].size() < static_cast<size_t>(half) * count) return;
        for (uint16_t leaf = 0; leaf < count; ++leaf) {
            const uint16_t base = static_cast<uint16_t>(leaf * half);
            for (uint16_t bit = 0; bit < half; ++bit) outputs[leaf].at(bit) = inputs[0][base + bit];
        }
    };
    for (uint16_t leaf = 0; leaf < count; ++leaf) {
        const uint16_t high = static_cast<uint16_t>(half * (count - leaf) - 1);
        const uint16_t low = static_cast<uint16_t>(half * (count - 1 - leaf));
        spec->verilog += "assign %strength%#(%rise%,%fall%) %Y" + std::to_string(leaf) + "% = %A%[" +
                         std::to_string(high) + ":" + std::to_string(low) + "];\n";
    }
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* A Word Maker / wide Concatenator: N inputs, each with its own width, and one
   output that is their concatenation with the topmost input most significant.
   "Merges 2 (n/2)-bit values into one n-bit value" is the game's own description
   of 0x30, and a real board agrees: `kind 0x30 … in0 8 bits, in1 8 bits -> out0
   16 bits`, with the record's `+0xe0` saying 16 - the output width.  The bit
   Maker (0x10 / 0x6f / 0x70) is this device with one-bit leaves, so the two
   share an implementation. */
inline const DeviceSpec& deviceWordMaker(const std::vector<uint16_t>& widths, ArcDelay delay = ArcDelay{},
                                         DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    (void)kind;
    std::vector<uint16_t> leaves = widths;
    if (leaves.empty()) leaves.push_back(1);
    for (uint16_t& width : leaves) {
        if (!width) width = 1;
    }
    uint16_t total = 0;
    std::string key;
    for (uint16_t width : leaves) {
        total = static_cast<uint16_t>(total + width);
        key += std::to_string(width) + "-";
    }
    if (!total) total = 1;
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "wordmaker" + std::to_string(total);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = DelayKind::kInertial;
    for (size_t index = 0; index < leaves.size(); ++index) {
        spec->inputs.push_back(PinSpec{"A" + std::to_string(index), leaves[index]});
        spec->arcs.push_back(Arc{static_cast<uint16_t>(index), 0, delay});
    }
    spec->outputs.push_back(PinSpec{"Y", total});
    spec->default_delay = delay;
    spec->behavior = [leaves, total](const EvalContext&, const std::vector<BitVector>& inputs,
                                     std::vector<BitVector>& outputs) {
        BitVector out(total, Logic::kX);
        uint16_t offset = 0;
        for (size_t index = 0; index < leaves.size(); ++index) {
            const uint16_t width = leaves[index];
            if (index < inputs.size()) {
                for (uint16_t bit = 0; bit < width && bit < inputs[index].size(); ++bit) {
                    out.at(offset + bit) = inputs[index][bit];
                }
            }
            offset = static_cast<uint16_t>(offset + width);
        }
        outputs.assign(1, out);
    };
    std::string concatenation;
    for (size_t index = 0; index < leaves.size(); ++index) {
        if (index) concatenation += ", ";
        concatenation += "%A" + std::to_string(index) + "%";
    }
    spec->verilog = "assign %strength%#(%rise%,%fall%) %Y% = {" + concatenation + "};\n";
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

inline const DeviceSpec& deviceMaker(uint16_t bits, DelayKind kind = DelayKind::kInertial) {
    static std::map<uint16_t, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    (void)kind;
    const auto found = cache.find(width);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "maker" + std::to_string(width);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = DelayKind::kInertial;
    for (uint16_t bit = 0; bit < width; ++bit) {
        spec->inputs.push_back(PinSpec{"A" + std::to_string(bit), 1});
        spec->arcs.push_back(Arc{bit, 0, spec->default_delay});
    }
    spec->outputs.push_back(PinSpec{"Y", width});
    spec->behavior = [width](const EvalContext&, const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
        BitVector out(width, Logic::kX);
        for (uint16_t bit = 0; bit < width && bit < inputs.size(); ++bit) out.at(bit) = inputs[bit][0];
        outputs.assign(1, out);
    };
    std::string concatenation;
    for (uint16_t bit = 0; bit < width; ++bit) {
        if (bit) concatenation += ", ";
        concatenation += "%A" + std::to_string(bit) + "%";
    }
    spec->verilog = "assign %strength%#(%rise%,%fall%) %Y% = {" + concatenation + "};\n";
    const auto inserted = cache.emplace(width, std::move(spec));
    return *inserted.first->second;
}

/* ---------------------------------------------------------------------------
   The game's word operations (docs/research/simcore-kind-coverage.md).

   Every built-in word component is one operation on fixed-width bit vectors:
   Add (with carry in and out), Equal, the two "less than" comparisons, Negate,
   Multiply, Divide, Modulo, the five shifts/rotates, Increment, the word-wide
   bitwise gates, and the Zero/Leading/Trailing count family.

   **Four-state rule.**  The game's generated program is plain C on 64-bit words,
   and its own reference simulator resolves every value to a concrete number, so
   an operand bit that has not settled (X or Z) poisons the **whole result** of a
   word operation rather than just its own column.  That is the rule implemented
   here: one unknown bit anywhere in a word operation's operands makes every one
   of its outputs X.  It is deliberately the same answer the game gives, because
   a bit-precise answer the game does not agree with would make the cross-check
   corpus lie.  (The single-bit gates next door are different: four-state
   AND/OR/XOR is what a gate physically does, and `deviceNot`/`logicGate` keep
   per-bit truth.  A word-wide gate, however, is compiled as one C operation on
   the word, so it follows the word rule.)

   **Width and layout.**  A pin's width comes from the caller (the board builder
   resolves the game's automatic width from its own record, exactly as it does
   for the Word Switch).  Parameter pins - the shift amount, the comparison's
   second operand - are NOT assumed to be full width: their width is a
   parameter, and the operation reads as many low bits as their width holds.
   That is why the shift amount is modelled as its own pin rather than as a
   second data word.
   --------------------------------------------------------------------------- */

enum class WordOp : uint8_t {
    kAdd = 0,
    kEqual,
    kLessUnsigned,
    kLessSigned,
    kNegate,
    kMultiply,
    kDivide,
    kModulo,
    kShiftLeft,
    kShiftRight,
    kRotateLeft,
    kRotateRight,
    kArithmeticShiftRight,
    kIncrement,
    kNot,
    kAnd,
    kOr,
    kXor,
    kNand,
    kNor,
    kXnor,
    kCountLeadingZero,
    kCountTrailingZero,
    /* Concatenation: the game's 2-in Concatenator (0x72).  The other Maker and
       Concatenator shapes wait for the leaf-order measurement recorded in
       docs/research/simcore-kind-coverage.md. */
    kConcat,
    /* The two leaf operations, named apart from the gates so that "Splitter"
       and "NOT" can never share a name. */
    kSplit,
    kMake,
};

namespace detail {

inline uint64_t vectorToWord(const BitVector& value) {
    uint64_t word = 0;
    for (Logic bit : value.bits()) {
        word = (word << 1) | (bit == Logic::kOne ? 1u : 0u);
    }
    return word;
}

inline BitVector wordToVector(uint64_t value, size_t bits) {
    BitVector out(bits, Logic::kZero);
    for (size_t bit = 0; bit < bits; ++bit) {
        out.at(bits - 1 - bit) = ((value >> bit) & 1u) ? Logic::kOne : Logic::kZero;
    }
    return out;
}

inline uint64_t widthMask(size_t bits) {
    if (bits == 0) return 0;
    if (bits >= 64) return ~0ull;
    return (1ull << bits) - 1ull;
}

inline bool isUnknown(Logic value) { return value == Logic::kX || value == Logic::kZ; }

/* The operand as the 64-bit value the game's generated C would use.  A vector
   wider than 64 bits keeps its low 64, which is what a C `U64` parameter does;
   the sign of a signed comparison comes from the declared width. */
inline uint64_t operandWord(const BitVector& operand) { return vectorToWord(operand); }

inline uint64_t arithmeticShiftRight(uint64_t value, uint64_t amount, size_t bits) {
    const uint64_t mask = widthMask(bits);
    if (bits == 0) return 0;
    if (amount >= bits) return 0;
    const uint64_t sign = (value >> (bits - 1)) & 1ull;
    uint64_t shifted = (value & mask) >> amount;
    if (sign) {
        const uint64_t fill = (widthMask(amount)) << (bits - amount);
        shifted |= fill & mask;
    }
    return shifted & mask;
}

inline uint64_t rotateLeft(uint64_t value, uint64_t amount, size_t bits) {
    if (bits == 0) return 0;
    const uint64_t mask = widthMask(bits);
    const uint64_t shift = amount % bits;
    const uint64_t word = value & mask;
    if (shift == 0) return word;
    return ((word << shift) | (word >> (bits - shift))) & mask;
}

inline uint64_t rotateRight(uint64_t value, uint64_t amount, size_t bits) {
    if (bits == 0) return 0;
    const uint64_t mask = widthMask(bits);
    const uint64_t shift = amount % bits;
    const uint64_t word = value & mask;
    if (shift == 0) return word;
    return ((word >> shift) | (word << (bits - shift))) & mask;
}

/* The bitwise gates, per bit, in four-state logic. */
inline Logic bitwise(WordOp op, Logic left, Logic right) {
    switch (op) {
        case WordOp::kAnd: return logicAnd(left, right);
        case WordOp::kOr: return logicOr(left, right);
        case WordOp::kXor: return logicXor(left, right);
        case WordOp::kNand: return logicNot(logicAnd(left, right));
        case WordOp::kNor: return logicNot(logicOr(left, right));
        case WordOp::kXnor: return logicNot(logicXor(left, right));
        default: return Logic::kX;
    }
}

/* One word operation's result, for operands that are fully known.  `carry_out`
   is only meaningful for Add, which is the one op the game gives a carry. */
struct WordResult {
    uint64_t value = 0;
    uint64_t carry = 0;
};

/* Every op that is exactly one 64-bit integer plus, where the game has one, a
   carry.  Returns nothing when the operand still carries unknown bits, which is
   the caller's signal to fall back to enumeration. */
inline std::optional<WordResult> wordResult(WordOp op, const BitVector& left, const BitVector& right,
                                            uint64_t carry_in, size_t bits) {
    const bool unary = op == WordOp::kNegate || op == WordOp::kIncrement ||
                       op == WordOp::kCountLeadingZero || op == WordOp::kCountTrailingZero ||
                       op == WordOp::kNot;
    if (!left.isKnown() || (!unary && !right.isKnown())) return std::nullopt;
    WordResult out;
    const uint64_t a = operandWord(left);
    const uint64_t b = operandWord(right);
    const uint64_t mask = widthMask(bits);
    switch (op) {
        /* The word-wide bitwise gates: one C operation per bit of the word,
           which is why they belong to the same table as the arithmetic. */
        case WordOp::kAnd:
        case WordOp::kOr:
        case WordOp::kXor:
        case WordOp::kNand:
        case WordOp::kNor:
        case WordOp::kXnor: {
            /* `BitVector` is most-significant-bit first, and so is the C value
               the game writes: bit 0 of the value is the vector's last entry. */
            uint64_t result = 0;
            for (size_t index = 0; index < bits; ++index) {
                result = (result << 1) | (bitwise(op, left[index], right[index]) == Logic::kOne ? 1ull : 0ull);
            }
            out.value = result & mask;
            return out;
        }
        case WordOp::kNot: {
            out.value = (~a) & mask;
            return out;
        }
        case WordOp::kAdd: {
            out.value = (a + b + carry_in) & mask;
            out.carry = ((a & mask) + (b & mask) + carry_in) > mask ? 1ull : 0ull;
            return out;
        }
        case WordOp::kIncrement: {
            out.value = (a + 1ull) & mask;
            return out;
        }
        case WordOp::kNegate: {
            out.value = ((~a) + 1ull) & mask;
            return out;
        }
        case WordOp::kMultiply: {
            out.value = bits >= 64 ? (a * b) : (((a & mask) * (b & mask)) & mask);
            return out;
        }
        case WordOp::kDivide: {
            /* Division by zero: the game's own test for `byte_divide` is the
               authority, and a probe has to pin the answer before this stays. */
            out.value = (b & mask) == 0 ? mask : (((a & mask) / (b & mask)) & mask);
            return out;
        }
        case WordOp::kModulo: {
            out.value = (b & mask) == 0 ? (a & mask) : (((a & mask) % (b & mask)) & mask);
            return out;
        }
        case WordOp::kShiftLeft: {
            out.value = b >= bits ? 0 : (((a & mask) << b) & mask);
            return out;
        }
        case WordOp::kShiftRight: {
            out.value = b >= bits ? 0 : ((a & mask) >> b);
            return out;
        }
        case WordOp::kArithmeticShiftRight: {
            out.value = arithmeticShiftRight(a & mask, b, bits);
            return out;
        }
        case WordOp::kRotateLeft: {
            out.value = rotateLeft(a & mask, b, bits);
            return out;
        }
        case WordOp::kRotateRight: {
            out.value = rotateRight(a & mask, b, bits);
            return out;
        }
        case WordOp::kCountLeadingZero: {
            const uint64_t word = a & mask;
            uint64_t count = 0;
            for (size_t bit = 0; bit < bits; ++bit) {
                if ((word >> (bits - 1 - bit)) & 1ull) break;
                ++count;
            }
            out.value = count & mask;
            return out;
        }
        case WordOp::kCountTrailingZero: {
            const uint64_t word = a & mask;
            uint64_t count = 0;
            for (size_t bit = 0; bit < bits; ++bit) {
                if ((word >> bit) & 1ull) break;
                ++count;
            }
            out.value = count & mask;
            return out;
        }
        default: return std::nullopt;
    }
}

/* One comparison, as the game's generated program writes it: plain C on the
   operands it can read.  `bits` supplies the signed comparison's width. */
inline bool compareResult(WordOp op, uint64_t a, uint64_t b, size_t bits) {
    const uint64_t mask = widthMask(bits);
    switch (op) {
        case WordOp::kEqual: return (a & mask) == (b & mask);
        case WordOp::kLessUnsigned: return (a & mask) < (b & mask);
        case WordOp::kLessSigned: {
            const bool negative_a = bits > 0 && ((a >> (bits - 1)) & 1ull) != 0;
            const bool negative_b = bits > 0 && ((b >> (bits - 1)) & 1ull) != 0;
            if (negative_a != negative_b) return negative_a;
            return (a & mask) < (b & mask);
        }
        default: return false;
    }
}

}  // namespace detail

/* A word unary operation: one data pin, one output of the same width. */
inline const DeviceSpec& deviceWordUnary(WordOp op, uint16_t bits, ArcDelay delay = ArcDelay{},
                                         DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    const std::string key = "wordunary-" + std::to_string(static_cast<int>(op)) + "-" + std::to_string(width) +
                            "-" + detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "wordunary" + std::to_string(static_cast<int>(op)) + "_" + std::to_string(width);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"A", width});
    spec->outputs.push_back(PinSpec{"Y", width});
    spec->default_delay = delay;
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->behavior = [op, width](const EvalContext&, const std::vector<BitVector>& inputs,
                                 std::vector<BitVector>& outputs) {
        const BitVector a = inputs.empty() ? BitVector(width, Logic::kX) : inputs[0];
        /* The game's own rule for a word operation: one unsettled operand bit
           settles nothing (see the family note above). */
        if (!a.isKnown()) {
            outputs.assign(1, BitVector(width, Logic::kX));
            return;
        }
        const BitVector none(width, Logic::kX);
        if (const std::optional<detail::WordResult> direct = detail::wordResult(op, a, none, 0, width)) {
            outputs.assign(1, detail::wordToVector(direct->value, width));
            return;
        }
        outputs.assign(1, BitVector(width, Logic::kX));
    };
    spec->verilog.clear(); /* filled in by name for the operations iverilog can run */
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* A word binary operation: two data pins and, for Add, a carry in and out. */
inline const DeviceSpec& deviceWordBinary(WordOp op, uint16_t bits, uint16_t right_bits,
                                          ArcDelay delay = ArcDelay{}, DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    const uint16_t parameter_width = right_bits ? right_bits : width;
    const bool carries = op == WordOp::kAdd;
    const std::string key = "wordbinary-" + std::to_string(static_cast<int>(op)) + "-" + std::to_string(width) +
                            "-" + std::to_string(parameter_width) + "-" + detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "wordbinary" + std::to_string(static_cast<int>(op)) + "_" + std::to_string(width);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"A", width});
    spec->inputs.push_back(PinSpec{"B", parameter_width});
    if (carries) spec->inputs.push_back(PinSpec{"Ci", 1});
    spec->outputs.push_back(PinSpec{"Y", width});
    if (carries) spec->outputs.push_back(PinSpec{"Co", 1});
    spec->default_delay = delay;
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->arcs.push_back(Arc{1, 0, delay});
    if (carries) spec->arcs.push_back(Arc{2, 0, delay});
    spec->behavior = [op, width, carries](const EvalContext&, const std::vector<BitVector>& inputs,
                                         std::vector<BitVector>& outputs) {
        const BitVector a = inputs.size() > 0 ? inputs[0] : BitVector(width, Logic::kX);
        const BitVector b = inputs.size() > 1 ? inputs[1] : BitVector(width, Logic::kX);
        const Logic carry_bit = carries && inputs.size() > 2 ? inputs[2][0] : Logic::kZero;
        const auto all_unknown = [&]() {
            outputs.assign(1, BitVector(width, Logic::kX));
            if (carries) outputs.push_back(BitVector(1, Logic::kX));
        };
        /* A comparison answers with one bit, so its unsettled operands produce
           one X - not a poisoned word. */
        if (op == WordOp::kEqual || op == WordOp::kLessUnsigned || op == WordOp::kLessSigned) {
            if (!a.isKnown() || !b.isKnown()) {
                outputs.assign(1, BitVector(1, Logic::kX));
                return;
            }
            const bool answer = detail::compareResult(op, detail::operandWord(a), detail::operandWord(b), width);
            outputs.assign(1, BitVector(1, answer ? Logic::kOne : Logic::kZero));
            return;
        }
        /* The game's own rule for a word operation: one unsettled operand bit
           settles nothing. */
        if (!a.isKnown() || !b.isKnown() || detail::isUnknown(carry_bit)) {
            all_unknown();
            return;
        }
        const uint64_t carry_in = carry_bit == Logic::kOne ? 1ull : 0ull;
        const std::optional<detail::WordResult> direct = detail::wordResult(op, a, b, carry_in, width);
        if (!direct) {
            all_unknown();
            return;
        }
        outputs.assign(1, detail::wordToVector(direct->value, width));
        if (carries) outputs.push_back(detail::wordToVector(direct->carry & 1ull, 1));
    };
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* The game's 2-in Concatenator (kind 0x72): two vectors joined into one, the
   first pin the high half.  Its prototype declares `in0=(-1,-1) in1=(-1,0)
   out0=(1,0)`, and the same left-to-right reading is what builds a word from
   bytes in the campaign's own circuits, so the pin order fixes the bit order:
   in0 occupies the high bits, in1 the low ones, and the result is as wide as
   the two pins together.

   The other Maker/Concatenator shapes (2/4/8 leaves) and the whole Splitter
   family join the library the same way once their leaf-to-bit mapping has been
   measured - docs/research/simcore-kind-coverage.md lists them as waiting. */
inline const DeviceSpec& deviceConcat(uint16_t left_bits, uint16_t right_bits, ArcDelay delay = ArcDelay{},
                                      DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t high = left_bits ? left_bits : 1;
    const uint16_t low = right_bits ? right_bits : 1;
    const uint16_t width = static_cast<uint16_t>(high + low);
    const std::string key = "concat-" + std::to_string(high) + "-" + std::to_string(low) + "-" +
                            detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "concat" + std::to_string(width);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"A", high});
    spec->inputs.push_back(PinSpec{"B", low});
    spec->outputs.push_back(PinSpec{"Y", width});
    spec->default_delay = delay;
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->arcs.push_back(Arc{1, 0, delay});
    spec->behavior = [high, low, width](const EvalContext&, const std::vector<BitVector>& inputs,
                                        std::vector<BitVector>& outputs) {
        BitVector out(width, Logic::kX);
        const BitVector a = inputs.size() > 0 ? inputs[0] : BitVector(high, Logic::kX);
        const BitVector b = inputs.size() > 1 ? inputs[1] : BitVector(low, Logic::kX);
        for (uint16_t bit = 0; bit < high; ++bit) out.at(bit) = a[bit];
        for (uint16_t bit = 0; bit < low; ++bit) out.at(high + bit) = b[bit];
        outputs.assign(1, out);
    };
    spec->verilog = "assign %strength%#(%rise%,%fall%) %Y% = {%A%, %B%};";
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* The game's Static Indexer (kind 0x65): read `width` bits of the input starting
   at bit `index`, i.e. `(A >> index) & ((1 << width) - 1)` for a non-negative
   index.  Its rule is **measured from the game's own files**, not guessed:

     * `campaign/symphony_1_nand/test.si` decodes an instruction exactly this
       way - `let mode = (instruction >> 29) & 0b11`, `let opcode =
       (instruction >> 24) & 0b1111`, `dest`, `a`, `b`, the immediate flag at 28
       and the 16-bit immediate value - and the level's own
       `hint_custom/Instruction Decoder` solution wires seven Output Pins to
       seven Static Indexers configured `(index, width)` = `(29,2) (24,4) (20,4)
       (16,4) (8,4) (28,1) (0,16)`: the same numbers, in the same order, as the
       ISA;
     * `campaign/multiply/hint_solution.data` builds a 256-entry table with six
       Static Indexers of **negative** index (`-1` through `-7`, width 8), which
       is a left shift: `n << k` has to be `(n >> -k)`'s mirror, and the level's
       own test is `(a * b) & 0xff`.

   So a negative index shifts the other way.  The width is the output mask, and
   that part of the negative case (whether the result is `(A << -index) & mask`)
   is the one reading here that a level only implies rather than states; it is
   listed as such in docs/research/simcore-kind-coverage.md. */
inline const DeviceSpec& deviceIndexer(int32_t index, uint16_t width, ArcDelay delay = ArcDelay{},
                                       DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t bits = width ? width : 1;
    const std::string key = "indexer-" + std::to_string(index) + "-" + std::to_string(bits) + "-" +
                            detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "indexer" + std::to_string(index) + "_" + std::to_string(bits);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"A", bits});
    spec->outputs.push_back(PinSpec{"Y", bits});
    spec->default_delay = delay;
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->behavior = [index, bits](const EvalContext&, const std::vector<BitVector>& inputs,
                                  std::vector<BitVector>& outputs) {
        const BitVector value = inputs.empty() ? BitVector(bits, Logic::kX) : inputs[0];
        /* One unsettled bit settles nothing, the same rule the word operations
           follow (the generated program is plain C on a word). */
        if (!value.isKnown()) {
            outputs.assign(1, BitVector(bits, Logic::kX));
            return;
        }
        BitVector out(bits, Logic::kZero);
        if (index >= 0) {
            /* Read the field: bit `bits - 1` of the result is input bit
               `index + bits - 1`, and anything past the input reads as 0. */
            for (uint16_t bit = 0; bit < bits; ++bit) {
                const int32_t source = index + bit;
                if (source >= 0 && static_cast<size_t>(source) < value.size()) {
                    out.at(bits - 1 - bit) = value[value.size() - 1 - static_cast<size_t>(source)];
                }
            }
        } else {
            /* A negative index shifts the other way; the vacated low bits are 0
               (`campaign/multiply` builds its table this way). */
            const uint16_t shift = static_cast<uint16_t>(-index);
            for (uint16_t bit = shift; bit < bits; ++bit) {
                const uint16_t source = static_cast<uint16_t>(bit - shift);
                if (source < value.size()) out.at(bits - 1 - bit) = value[value.size() - 1 - source];
            }
        }
        outputs.assign(1, out);
    };
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* The custom component's two interface pins, kinds 0x4f (Input Pin) and 0x51
   (Output Pin).  Both are **pass-throughs**, and that is measured rather than
   assumed: over the 217 readable circuits under `campaign/`, every one of the
   90 Output Pins has its `in0` wired and its `out0` unwired, and every one of
   the 74 Input Pins has its `in0` unwired and its `out0` wired
   (`python tools/netlist-view.py --scan --kinds 0x4f 0x51`).  Their pin offsets
   are the prototype table's own: the Input Pin's output sits at `(3,0)` and its
   input at `(0,0)` - the component's own cell, i.e. the boundary where the
   outside value arrives; the Output Pin's input is at `(-3,0)` and its output at
   `(0,0)`, the boundary it writes back through.

   The width is the signal's own, so the device is cached per width.  A pin whose
   input nothing drives (`out0` reaches the boundary but no wire feeds `in0`)
   passes an unsettled value on, which is exactly what a pin with no source
   should do in a shadow simulation. */
inline const DeviceSpec& devicePin(uint16_t bits, ArcDelay delay = ArcDelay{},
                                    DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    const std::string key = "pin-" + std::to_string(width) + "-" + detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "pin" + std::to_string(width);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"A", width});
    spec->outputs.push_back(PinSpec{"Y", width});
    spec->default_delay = delay;
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->behavior = [width](const EvalContext&, const std::vector<BitVector>& inputs,
                             std::vector<BitVector>& outputs) {
        /* An unwired input is not "some unknown value", it is nothing at all:
           the pin's output floats (Z), the same answer a net with no driver
           gets. */
        outputs.assign(1, inputs.empty() ? BitVector(width, Logic::kZ) : inputs[0]);
    };
    spec->verilog = "assign %strength%#(%rise%,%fall%) %Y% = %A%;";
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* The game's Decoder family: kind 0x2b (1 Bit Decoder, one input bit and two
   outputs), 0x2c (2 Bit Decoder, a 2-bit input and four outputs) and 0x2d
   (3 Bit Decoder, a 3-bit input, a second one-bit pin, and eight outputs).  The
   prototype table declares every one of those widths, so the shape is not in
   doubt: `in0=(-1,-2,w3) in1=(0,-4,w1) out0..7=(1,-3..4,w1)`.

   The behaviour is **one-hot**: output `k` is 1 exactly when the value on the
   address pin is `k`, everything else 0.  Evidence: the game's own
   `overture_2_alu` / `overture_3_immediates` solutions wire a 3 Bit Decoder's
   `out0..out2` into the **high three leaves of a 4-input Maker** and a
   Splitter's `out2` into the Maker's low leaf, i.e. the decoder supplies the
   selection word of a 4-way mux while the splitter supplies the low data bit.

   `in1` is modelled as **active-high enable**, and that is the one part here that
   is inferred rather than measured: it is the only pin whose declared offset is
   not on the data path (`(0,-4)`, a row above the input), it is wired in only
   1 of the 11 appearances in the game's circuits - and in that one it carries a
   1-bit signal - and with it left unwired the gate stays enabled, which is what
   a player sees.  It is listed with the unmeasured items in
   docs/research/simcore-kind-coverage.md. */
inline const DeviceSpec& deviceDecoder(uint8_t address_bits, bool has_enable, ArcDelay delay = ArcDelay{},
                                       DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint8_t width = address_bits ? address_bits : 1;
    const uint16_t outputs = static_cast<uint16_t>(1u << width);
    const std::string key = "decoder-" + std::to_string(width) + (has_enable ? "-en" : "") + "-" +
                            detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "decoder" + std::to_string(width);
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"A", width});
    if (has_enable) spec->inputs.push_back(PinSpec{"EN", 1});
    for (uint16_t index = 0; index < outputs; ++index) {
        spec->outputs.push_back(PinSpec{"Y" + std::to_string(index), 1});
        spec->arcs.push_back(Arc{0, index, delay});
    }
    spec->default_delay = delay;
    spec->behavior = [width, outputs, has_enable](const EvalContext&, const std::vector<BitVector>& inputs,
                                                 std::vector<BitVector>& outputs_out) {
        const BitVector address = inputs.empty() ? BitVector(width, Logic::kX) : inputs[0];
        const Logic enable = has_enable && inputs.size() > 1 ? inputs[1][0] : Logic::kOne;
        outputs_out.assign(outputs, BitVector(1, Logic::kZero));
        /* An unsettled address or enable settles nothing, the rule the rest of
           the library follows. */
        if (!address.isKnown() || detail::isUnknown(enable)) {
            outputs_out.assign(outputs, BitVector(1, Logic::kX));
            return;
        }
        if (enable != Logic::kOne) return; /* disabled: every output is 0 */
        const size_t selected = static_cast<size_t>(detail::vectorToWord(address) & (outputs - 1));
        if (selected < outputs_out.size()) outputs_out[selected].at(0) = Logic::kOne;
    };
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* A rising-edge D flip-flop.  S3 owns the full sequential set (counters, delay
   lines, RAM, ports); this one exists from S1 because the NBA layer and the
   setup/hold reporting need something real to be tested against. */
inline const DeviceSpec& deviceDff(ArcDelay clock_to_q = ArcDelay{0, 0}, ArcDelay setup = ArcDelay{0, 0},
                                   ArcDelay hold = ArcDelay{0, 0}, bool x_on_violation = false,
                                   DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const std::string key = "dff-" + std::to_string(clock_to_q.tplh) + "-" + std::to_string(clock_to_q.tphl) + "-" +
                            std::to_string(setup.tplh) + "-" + std::to_string(setup.tphl) + "-" + std::to_string(hold.tplh) +
                            "-" + std::to_string(hold.tphl) + "-" + std::to_string(x_on_violation ? 1 : 0) + "-" +
                            std::to_string(static_cast<int>(kind));
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "dff";
    spec->kind = DeviceKind::kSequential;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"D", 1});
    spec->inputs.push_back(PinSpec{"CLK", 1});
    spec->outputs.push_back(PinSpec{"Q", 1});
    spec->clock_pin = 1;
    spec->clock_edge = EdgeKind::kPosEdge;
    spec->clock_to_q = clock_to_q;
    spec->setup = setup;
    spec->hold = hold;
    spec->x_on_violation = x_on_violation;
    spec->behavior = [](const EvalContext&, const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
        BitVector out(1, Logic::kX);
        if (!inputs.empty()) out.at(0) = inputs[0][0];
        outputs.assign(1, out);
    };
    spec->verilog = "always @(posedge %CLK%) %Q% <= #(%rise%) %D%;";
    const auto inserted = cache.emplace(key, std::move(spec));
    return *inserted.first->second;
}

/* ---- the storage family: Register, Counter, Delay Line --------------------

   Their contract is the program the game itself compiles for a board that
   carries them (tests/kind-source-dump.ps1 keeps those programs, one directory
   per board, under build/):

     // 3 com_register_word 13 1694631439058008882 Reg 0
     let value_3 = load(<U13>, #SIMULATION_STATE + 332)
     store(#SIMULATION_STATE + 333, value_3)          // the output pin
     // 9 com_register_word 13 LATE
     if (U1 vid272) == 1 { store(#SIMULATION_STATE + 332, U13 (vid317)) }

     // com_delay_line_word LATE
     store(#SIMULATION_STATE + N, U8 (vid265))

     // com_counter LATE
     if (U1 0x0) == 1 { store(state + N, 0) }
     else { store(state + N, (load(state + N) & <width mask> + 1)) }

   Three things follow, and they are the whole model:

     * a register latches its data when its enable is 1, and keeps its value
       otherwise ("Small memory component");
     * a delay line latches its input every cycle ("Stores the input for 1
       cycle");
     * a counter counts up unless its reset input is 1 ("Increments each cycle,
       unless overwritten").

   All three latch in the game's LATE phase - the step right before `cycle += 1`
   - so the sandbox clocks them with one net per board that the runtime toggles
   at every cycle boundary, exactly like the RAM's port ordering chain.  The
   pins are the prototype table's *hidden* list (`+0x70`; kind-list-probe now
   prints it as `hidden0=…`): the register's enable sits at (-3,-1) and its data
   at (-3,0), the delay line's data at (-3,0), and the counter's reset at
   (-3,-1).  See docs/research/simcore-kind-coverage.md section 7. */
inline BitVector maskedIncrement(const BitVector& value) {
    BitVector out(value.size(), Logic::kX);
    bool unknown = false;
    for (size_t bit = 0; bit < value.size(); ++bit) {
        if (value[bit] != Logic::kZero && value[bit] != Logic::kOne) unknown = true;
    }
    if (unknown) return out;
    bool carry = true;
    /* BitVector is most-significant-bit first, so the carry runs from the last
       element towards the first. */
    for (size_t index = value.size(); index-- > 0;) {
        const Logic current = value[index];
        out.at(index) = carry ? (current == Logic::kOne ? Logic::kZero : Logic::kOne) : current;
        carry = carry && current == Logic::kOne;
    }
    return out;
}

inline const DeviceSpec& deviceRegister(uint16_t bits, ArcDelay clock_to_q = ArcDelay{},
                                        DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    const std::string key = "register" + std::to_string(width) + "-" +
                            std::to_string(clock_to_q.tplh) + "-" + std::to_string(clock_to_q.tphl) + "-" +
                            std::to_string(static_cast<int>(kind));
    auto found = cache.find(key);
    if (found == cache.end()) {
        auto spec = std::make_unique<DeviceSpec>();
        spec->name = "register" + std::to_string(width);
        spec->kind = DeviceKind::kSequential;
        spec->delay_kind = kind;
        spec->inputs.push_back(PinSpec{"EN", 1});
        spec->inputs.push_back(PinSpec{"D", width});
        spec->inputs.push_back(PinSpec{"CLK", 1});
        spec->outputs.push_back(PinSpec{"Q", width});
        spec->clock_pin = 2;
        spec->clock_edge = EdgeKind::kPosEdge;
        spec->clock_to_q = clock_to_q;
        spec->state_size = 1;
        spec->output_is_state = true;
        spec->state_init = {BitVector(width, Logic::kZero)};
        spec->state_behavior = [](const EvalContext&, std::vector<BitVector>& state,
                                  const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
            const size_t width = inputs.size() > 1 ? inputs[1].size() : 1;
            if (state.size() != 1 || state[0].size() != width) state.assign(1, BitVector(width, Logic::kZero));
            const bool enable = !inputs.empty() && inputs[0].size() && inputs[0][0] == Logic::kOne;
            if (enable && inputs.size() > 1) state[0] = inputs[1];
            outputs.assign(1, state[0]);
        };
        spec->verilog = "always @(posedge %CLK%) if (%EN%) %Q% <= #(%rise%) %D%;";
        found = cache.emplace(key, std::move(spec)).first;
    }
    return *found->second;
}

inline const DeviceSpec& deviceDelayLine(uint16_t bits, ArcDelay clock_to_q = ArcDelay{},
                                         DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    const std::string key = "delayline" + std::to_string(width) + "-" +
                            std::to_string(clock_to_q.tplh) + "-" + std::to_string(clock_to_q.tphl) + "-" +
                            std::to_string(static_cast<int>(kind));
    auto found = cache.find(key);
    if (found == cache.end()) {
        auto spec = std::make_unique<DeviceSpec>();
        spec->name = "delayline" + std::to_string(width);
        spec->kind = DeviceKind::kSequential;
        spec->delay_kind = kind;
        spec->inputs.push_back(PinSpec{"D", width});
        spec->inputs.push_back(PinSpec{"CLK", 1});
        spec->outputs.push_back(PinSpec{"Q", width});
        spec->clock_pin = 1;
        spec->clock_edge = EdgeKind::kPosEdge;
        spec->clock_to_q = clock_to_q;
        spec->state_size = 1;
        spec->output_is_state = true;
        spec->state_init = {BitVector(width, Logic::kZero)};
        spec->state_behavior = [](const EvalContext&, std::vector<BitVector>& state,
                                  const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
            const size_t width = inputs.empty() ? 1 : inputs[0].size();
            if (state.size() != 1 || state[0].size() != width) state.assign(1, BitVector(width, Logic::kZero));
            if (!inputs.empty()) state[0] = inputs[0];
            outputs.assign(1, state[0]);
        };
        spec->verilog = "always @(posedge %CLK%) %Q% <= #(%rise%) %D%;";
        found = cache.emplace(key, std::move(spec)).first;
    }
    return *found->second;
}

inline const DeviceSpec& deviceCounter(uint16_t bits, ArcDelay clock_to_q = ArcDelay{},
                                       DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    const std::string key = "counter" + std::to_string(width) + "-" +
                            std::to_string(clock_to_q.tplh) + "-" + std::to_string(clock_to_q.tphl) + "-" +
                            std::to_string(static_cast<int>(kind));
    auto found = cache.find(key);
    if (found == cache.end()) {
        auto spec = std::make_unique<DeviceSpec>();
        spec->name = "counter" + std::to_string(width);
        spec->kind = DeviceKind::kSequential;
        spec->delay_kind = kind;
        /* in0 = reset (1 bit), in1 = the auto-size data pin the prototype also
           declares.  The game's own program reads only the reset: the data pin
           is the width source, not an operand, which is why the emitted counter
           has a single condition. */
        spec->inputs.push_back(PinSpec{"RST", 1});
        spec->inputs.push_back(PinSpec{"D", width});
        spec->inputs.push_back(PinSpec{"CLK", 1});
        spec->outputs.push_back(PinSpec{"Q", width});
        spec->clock_pin = 2;
        spec->clock_edge = EdgeKind::kPosEdge;
        spec->clock_to_q = clock_to_q;
        spec->state_size = 1;
        spec->output_is_state = true;
        spec->state_init = {BitVector(width, Logic::kZero)};
        spec->state_behavior = [](const EvalContext&, std::vector<BitVector>& state,
                                  const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
            const size_t width = inputs.size() > 1 ? inputs[1].size() : 1;
            if (state.size() != 1 || state[0].size() != width) state.assign(1, BitVector(width, Logic::kZero));
            const bool reset = !inputs.empty() && inputs[0].size() && inputs[0][0] == Logic::kOne;
            state[0] = reset ? BitVector(width, Logic::kZero) : maskedIncrement(state[0]);
            outputs.assign(1, state[0]);
        };
        spec->verilog = "always @(posedge %CLK%) %Q% <= #(%rise%) (%RST% ? 0 : (%Q% + 1));";
        found = cache.emplace(key, std::move(spec)).first;
    }
    return *found->second;
}

/* A component that drives a fixed value and takes nothing: the game's Static
   Value / Static Evaluator ("Calculates a value before simulation (it becomes a
   constant during simulation)"), and the 2-bit configuration word the
   Auto Register / Auto Delay Line publish for their sibling. */
inline const DeviceSpec& deviceWordConstant(uint16_t bits, uint64_t value,
                                            DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const uint16_t width = bits ? bits : 1;
    const std::string key = "wordconst" + std::to_string(width) + ":" + std::to_string(value) + "-" +
                            std::to_string(static_cast<int>(kind));
    auto found = cache.find(key);
    if (found == cache.end()) {
        auto spec = std::make_unique<DeviceSpec>();
        spec->name = "wordconst" + std::to_string(width) + ":" + std::to_string(value);
        spec->kind = DeviceKind::kCombinational;
        spec->delay_kind = kind;
        spec->outputs.push_back(PinSpec{"Y", width});
        BitVector pattern(width, Logic::kZero);
        for (uint16_t bit = 0; bit < width && bit < 64; ++bit) {
            /* Most-significant bit first: value bit 0 is the last element. */
            pattern.at(width - 1u - bit) = ((value >> bit) & 1u) ? Logic::kOne : Logic::kZero;
        }
        spec->behavior = [pattern](const EvalContext&, const std::vector<BitVector>&,
                                   std::vector<BitVector>& outputs) { outputs.assign(1, pattern); };
        spec->verilog = "assign %Y% = " + std::to_string(value) + ";";
        found = cache.emplace(key, std::move(spec)).first;
    }
    return *found->second;
}

/* Every fixed-name device the library publishes, for a caller that wants to
   know what is supported before it builds a netlist.  The width-parameterized
   ones are not listed because a name without a width does not identify them:
   `mux<w>` (and its one-bit alias "mux2"), `splitter<w>`, `maker<w>`, and the
   tri-state switch `tristate<w>`. */
inline std::vector<std::string> builtinDeviceNames() {
    return {"buf",  "not",  "and2", "and3", "nand2", "nand3", "or2",  "or3",
            "nor2", "nor3", "xor2", "xor3", "xnor2", "xnor3", "mux2", "dff"};
}

/* The word operations, looked up by name because their width only exists at
   build time.  `left_bits` is the width the caller resolved for the data path
   and `right_bits` the width of the second pin (a comparison's other operand, a
   shift amount).  Returns null for a name this library does not implement: the
   board builder reports that instead of guessing (red line 6). */
inline const DeviceSpec* builtinWordDevice(const std::string& name, uint16_t left_bits, uint16_t right_bits,
                                           ArcDelay delay = ArcDelay{}) {
    const uint16_t width = left_bits ? left_bits : 1;
    const uint16_t other = right_bits ? right_bits : width;
    if (name == "addW") return &deviceWordBinary(WordOp::kAdd, width, other, delay);
    if (name == "equalW") return &deviceWordBinary(WordOp::kEqual, width, other, delay);
    if (name == "lessU") return &deviceWordBinary(WordOp::kLessUnsigned, width, other, delay);
    if (name == "lessS") return &deviceWordBinary(WordOp::kLessSigned, width, other, delay);
    if (name == "mulW") return &deviceWordBinary(WordOp::kMultiply, width, other, delay);
    if (name == "divW") return &deviceWordBinary(WordOp::kDivide, width, other, delay);
    if (name == "modW") return &deviceWordBinary(WordOp::kModulo, width, other, delay);
    if (name == "shlW") return &deviceWordBinary(WordOp::kShiftLeft, width, other, delay);
    if (name == "shrW") return &deviceWordBinary(WordOp::kShiftRight, width, other, delay);
    if (name == "asrW") return &deviceWordBinary(WordOp::kArithmeticShiftRight, width, other, delay);
    if (name == "rolW") return &deviceWordBinary(WordOp::kRotateLeft, width, other, delay);
    if (name == "rorW") return &deviceWordBinary(WordOp::kRotateRight, width, other, delay);
    if (name == "andW") return &deviceWordBinary(WordOp::kAnd, width, other, delay);
    if (name == "orW") return &deviceWordBinary(WordOp::kOr, width, other, delay);
    if (name == "xorW") return &deviceWordBinary(WordOp::kXor, width, other, delay);
    if (name == "nandW") return &deviceWordBinary(WordOp::kNand, width, other, delay);
    if (name == "norW") return &deviceWordBinary(WordOp::kNor, width, other, delay);
    if (name == "xnorW") return &deviceWordBinary(WordOp::kXnor, width, other, delay);
    if (name == "negateW") return &deviceWordUnary(WordOp::kNegate, width, delay);
    if (name == "incW") return &deviceWordUnary(WordOp::kIncrement, width, delay);
    if (name == "notW") return &deviceWordUnary(WordOp::kNot, width, delay);
    if (name == "clzW") return &deviceWordUnary(WordOp::kCountLeadingZero, width, delay);
    if (name == "ctzW") return &deviceWordUnary(WordOp::kCountTrailingZero, width, delay);
    if (name == "concat2") return &deviceConcat(width, other, delay);
    return nullptr;
}

/* The word-operation names, so a caller can enumerate what the library covers
   without building one first. */
inline std::vector<std::string> builtinWordDeviceNames() {
    return {"addW",    "equalW", "lessU",  "lessS", "mulW",  "divW",   "modW",  "shlW",
            "shrW",    "asrW",   "rolW",   "rorW",  "andW",  "orW",    "xorW",  "nandW",
            "norW",    "xnorW",  "negateW", "incW", "notW",  "clzW",   "ctzW",  "concat2"};
}

/* Looks a published name up in the library.  Returns null for anything the
   library does not implement, which is the answer the caller has to report
   instead of approximating (red line 6). */
inline const DeviceSpec* builtinDevice(const std::string& name, ArcDelay delay = ArcDelay{}) {
    if (name == "buf") return &deviceBuffer(delay);
    if (name == "not") return &deviceNot(delay);
    if (name == "mux2") return &deviceMux2(delay);
    if (name == "dff") return &deviceDff(delay);
    if (name == "and2") return &logicGate(name, 2, '&', false, delay);
    if (name == "and3") return &logicGate(name, 3, '&', false, delay);
    if (name == "nand2") return &logicGate(name, 2, '&', true, delay);
    if (name == "nand3") return &logicGate(name, 3, '&', true, delay);
    if (name == "or2") return &logicGate(name, 2, '|', false, delay);
    if (name == "or3") return &logicGate(name, 3, '|', false, delay);
    if (name == "nor2") return &logicGate(name, 2, '|', true, delay);
    if (name == "nor3") return &logicGate(name, 3, '|', true, delay);
    if (name == "xor2") return &logicGate(name, 2, '^', false, delay);
    if (name == "xor3") return &logicGate(name, 3, '^', false, delay);
    if (name == "xnor2") return &logicGate(name, 2, '^', true, delay);
    if (name == "xnor3") return &logicGate(name, 3, '^', true, delay);
    return nullptr;
}

}  // namespace tcsim
