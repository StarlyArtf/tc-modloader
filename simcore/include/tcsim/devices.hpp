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

/* S selects B when it is 1; both data inputs carry the net's width. */
inline const DeviceSpec& deviceMux2(ArcDelay delay = ArcDelay{}, DelayKind kind = DelayKind::kInertial) {
    static std::map<std::string, std::unique_ptr<DeviceSpec>> cache;
    const std::string key = "mux2-" + detail::delayKey(delay, kind);
    const auto found = cache.find(key);
    if (found != cache.end()) return *found->second;
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = "mux2";
    spec->kind = DeviceKind::kCombinational;
    spec->delay_kind = kind;
    spec->inputs.push_back(PinSpec{"S", 1});
    spec->inputs.push_back(PinSpec{"A", 1});
    spec->inputs.push_back(PinSpec{"B", 1});
    spec->outputs.push_back(PinSpec{"Y", 1});
    spec->default_delay = delay;
    spec->arcs.push_back(Arc{0, 0, delay});
    spec->arcs.push_back(Arc{1, 0, delay});
    spec->arcs.push_back(Arc{2, 0, delay});
    spec->behavior = [](const EvalContext&, const std::vector<BitVector>& inputs, std::vector<BitVector>& outputs) {
        const size_t bits = inputs.size() < 3 ? 1 : (inputs[1].size() < inputs[2].size() ? inputs[1].size() : inputs[2].size());
        BitVector out(bits, Logic::kX);
        for (size_t bit = 0; bit < bits; ++bit) {
            out.at(bit) = logicMux(inputs[0][0], inputs[2][bit], inputs[1][bit]);
        }
        outputs.assign(1, out);
    };
    spec->verilog = "assign %strength%#(%rise%,%fall%) %Y% = %S% ? %B% : %A%;";
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

/* The game's Splitter and Maker, one bit per leaf pin. */
inline const DeviceSpec& deviceSplitter(uint16_t bits, DelayKind kind = DelayKind::kInertial) {
    static std::map<uint16_t, std::unique_ptr<DeviceSpec>> cache;
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

/* Every name the library publishes, for a caller that wants to know what is
   supported before it builds a netlist. */
inline std::vector<std::string> builtinDeviceNames() {
    return {"buf",  "not",  "and2", "and3", "nand2", "nand3", "or2",  "or3",
            "nor2", "nor3", "xor2", "xor3", "xnor2", "xnor3", "mux2", "dff"};
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
