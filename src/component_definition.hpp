#pragma once
#include "../sdk/tc_logic_api.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

namespace tc::component_definition {
inline constexpr unsigned kMaxPinBits = 64;
inline constexpr unsigned kMaxInputBits = 2048;
/* `maxPins` is the caller's shape limit: the V1 callback struct carries eight
   pins per direction, while a V2/V3 definition (tc.component.types) may declare
   up to thirty-two.  Everything else - either direction may be empty, each pin
   1..64 bits, total input width within the staged-input budget - is the same for
   both.

   A shape with *no* pin at all is allowed on purpose: it is a decorative board
   object (the text-note component), which owns a place on the board and a
   per-instance configuration but never observes or produces a value.  Its
   scaffold is the single unconnected driver the encoder already writes for a
   sink, so the bridge has exactly one node to bind and the callback is a no-op.
   Registers only because the board is the place the note lives in; see
   docs/research/text-component.md. */
inline bool validShape(const TCNativeComponentDefinition* d, unsigned maxPins = 8) {
    if (!d || d->size != sizeof(*d) || !d->custom_id ||
        !d->name || !*d->name || std::strlen(d->name) > 65535 ||
        d->input_count > maxPins || d->output_count > maxPins ||
        (d->input_count != 0 && !d->inputs) || (d->output_count != 0 && !d->outputs) ||
        d->gate_cost > INT64_MAX || d->delay > INT64_MAX) return false;
    unsigned total = 0;
    for (unsigned dir = 0; dir < 2; ++dir) {
        auto pins = dir ? d->outputs : d->inputs;
        auto count = dir ? d->output_count : d->input_count;
        for (unsigned i = 0; i < count; ++i) {
            if (!pins[i].name || std::strlen(pins[i].name) > 65535 ||
                pins[i].bits < 1 || pins[i].bits > kMaxPinBits) return false;
            if (!dir) total += pins[i].bits;
        }
    }
    return total <= kMaxInputBits;
}
/* A registration needs a callback; the encoder only needs a shape, which is
   what the V2 path hands it (its callback takes the V2 IO struct instead). */
inline bool valid(const TCNativeComponentDefinition* d, unsigned maxPins = 8) {
    return validShape(d, maxPins) && d->callback != nullptr;
}
struct Writer {
    std::vector<uint8_t> bytes;
    void number(uint64_t n, unsigned size) {
        for (unsigned i = 0; i < size; ++i) bytes.push_back(uint8_t(n >> (i * 8)));
    }
    void string(const char* s) {
        const size_t n = std::strlen(s); number(n, 2);
        bytes.insert(bytes.end(), s, s + n);
    }
    void component(unsigned kind, int x, int y, uint64_t id, const char* name,
                   unsigned bits, int ordinal = 0) {
        number(kind, 2); number(x, 2); number(y, 2); number(0, 1); number(id, 8);
        string(name); number(ordinal ? 1 : 0, 2);
        if (ordinal) number(kind == 0x4f ? 2 : 0, 8);
        number(0, 8); number(-2 * ordinal, 2); number(bits, 8);
        number(0, 1); number(UINT64_MAX, 8); number(0, 8); number(0, 1);
        number(0, 1); number(0, 2); number(0, 2);
    }
};
struct Point { int x, y; };

/* Where the generated pins sit, and how the caller asks for somewhere else.

   The game turns a pin's circuit coordinate into a board offset with
   `round(circuit / 8)` - measured, not assumed: circuit x = -18 lands on -2,
   +13 on +2, y = -4 on 0 and +4 on +1, and the three-input definitions that
   import cleanly are exactly the ones whose pins are eight units apart (one
   board cell).  So the scaffold's pin x is `8 * lane` circuit units for the
   lane the caller asked for, and the loader's default keeps the historical
   asymmetric pair so that every type registered before this cut produces a
   byte-identical definition (and therefore an identical design thumbnail).

   Rounding outward from the requested lane is exact: `round(8 * lane / 8)` is
   `lane` whenever `8 * lane` is a whole number of eighths of a cell, which is
   the resolution the callback API promises. */
inline constexpr float kDefaultPinLane = 2.0f;
inline constexpr float kMaxPinLane = 16.0f;
inline constexpr int kPinLaneUnitsPerCell = 8;
inline constexpr int kDefaultInputPinX = -18;   /* round(-18/8) = -2 */
inline constexpr int kDefaultOutputPinX = 13;   /* round(13/8) = +2  */

/* Where the scaffold's own nodes sit.  Everything inside the rectangle the pins
   span, because the game renders a generated component's **inner circuit** for
   its menu, its foundry panel and its preview picture: a node placed outside
   that rectangle shows up as a stray block next to the part.  The dependency
   gates used to sit at (40 + 12i, 100 + 8i) - a player saw that as an extra
   square six cells to the right and thirteen below an FP32 Add, and described
   it exactly that way.  The contract only fixes the node *order*, so the
   positions are free; these three columns keep the wiring short and inside the
   part. */
inline constexpr int kInputCollectorX = -6;
inline constexpr int kDependencyGateX = 0;
inline constexpr int kOutputDriverX = 6;

inline bool validPinLane(float lane) {
    return std::isfinite(lane) && lane >= 1.0f && lane <= kMaxPinLane;
}
// Generated scaffold contract: input pins/collectors, dependency gates, output
// drivers, then output pins, in that serialized order.  Without a dependency
// table there is one conservative all-input chain.  With a table there is one
// private cone per output and, for a clocked definition, one unconnected
// all-input tick cone. V1/V2 use unary drivers; V3 uses the game's bit/word
// Switch so the bridge can leave an output genuinely disconnected.
inline std::vector<uint8_t> encode(const TCNativeComponentDefinition& d, unsigned maxPins = 8,
                                   float pinLane = kDefaultPinLane,
                                   bool triStateOutputs = false,
                                   const uint32_t* outputDependencies = nullptr,
                                   bool clockedTick = false) {
    if (!validShape(&d, maxPins)) return {};
    if (!validPinLane(pinLane)) return {};
    const bool defaultLane = pinLane == kDefaultPinLane;
    const int laneUnits = static_cast<int>(std::lround(pinLane * kPinLaneUnitsPerCell));
    const int inputPinX = defaultLane ? kDefaultInputPinX : -laneUnits;
    const int outputPinX = defaultLane ? kDefaultOutputPinX : laneUnits;
    // Keep the output pin beyond the driver output even at the minimum lane.
    const int outputDriverX = std::min(kOutputDriverX, outputPinX - 6);
    const unsigned n = d.input_count, m = d.output_count;
    const bool precise = outputDependencies != nullptr && m != 0;
    const auto popcount32=[](uint32_t value) {
        unsigned count=0;
        while(value){value&=value-1;++count;}
        return count;
    };
    const uint32_t* masks = outputDependencies;
    size_t dependencyGateCount = 0;
    if (precise) {
        const uint32_t inputMask = n >= 32 ? ~uint32_t(0) : ((uint32_t(1) << n) - 1);
        for (unsigned output = 0; output < m; ++output) {
            const unsigned dependencies = popcount32(masks[output] & inputMask);
            if (dependencies > 1) dependencyGateCount += dependencies - 1;
        }
        /* A sequential callback is driven by a private all-input cone.  Its
           driver is deliberately not wired to a public output: it orders and
           performs the state transition without adding an input -> output edge
           to the graph the game uses for cycle detection. */
        if (clockedTick && n > 1) dependencyGateCount += n - 1;
    }
    Writer w;
    w.number(d.custom_id, 8); w.number(0, 4); w.number(d.gate_cost, 8); w.number(d.delay, 8);
    w.number(1, 1); w.number(10000, 8); w.number(0, 2); w.string("");
    w.number(0, 1); w.number(0, 2); w.number(0, 2); w.string("");
    for (int i = 0; i < 512; ++i) w.number(0, 1);
    /* Node count: n input pins, n input collectors, the dependency gates,
       m output drivers and m output pins (plus the optional hidden tick). With no inputs there are no pins,
       no collectors and no dependency chain to seed - the drivers stand alone
       and their input is unconnected, which is exactly the case the real
       machine is asked about (see docs/research/custom-component-pins.md). */
    /* A sink (no outputs) still needs one node to run on: the game only emits
       code for logic it can reach, and with nothing driving it a callback would
       never be called.  That node's output is deliberately left unconnected -
       whether the compiler keeps it is the measurement, see
       docs/research/custom-component-pins.md. */
    w.number(precise ? (2 * n + dependencyGateCount + 2 * m + (clockedTick ? 1 : 0))
                     : ((n ? 3 * n - 1 : 0) + (m ? 2 * m : 1)), 8);
    auto inputY = [n](unsigned i) { return int(i) * 8 - int(n - 1) * 4; };
    for (unsigned i = 0; i < n; ++i)
        w.component(0x4f, inputPinX, inputY(i), 0x1000 + i, d.inputs[i].name,
                    d.inputs[i].bits, i + 1);
    for (unsigned i = 0; i < n; ++i)
        w.component(0x12, kInputCollectorX, inputY(i), 0x2000 + i, "", d.inputs[i].bits);
    /* Where the precise layout puts every cone's gates.

       Two rules, both learned the hard way:

       * a gate inside a cone sits four rows below the one before it.  An XOR
         reads its inputs at row-1 and row+1, so gates packed one row apart put
         gate k's "+" pin on gate k+2's "-" pin - the *same lattice point*.  The
         tap wire ending there for one input and the chain wire ending there for
         the next gate met, the game merged them into one net, the shorted inputs
         gave the net two drivers, and the board came back as a combinational
         loop (the foundry drawing showed the two wires running across a gate).
         Three rows would already separate the pins; the fourth is the free row
         the chain is routed through.
       * each cone owns its own band of rows, so two cones never share pins even
         when their masks are large.  The band starts where the previous one
         ended; the driver keeps its own row next to the output pin, and the
         chain reaches up to it in the free column beside the drivers. */
    std::vector<int> coneFirstGate;
    if (precise) {
        const uint32_t inputMask = n >= 32 ? ~uint32_t(0) : ((uint32_t(1) << n) - 1);
        // Keep gate pins and their collector-row detours below every direct
        // output route and collector hop; an endpoint touching a direct route
        // would otherwise short two inputs with different dependency masks.
        int cursor = std::max(2, std::max(8 * int(m), 5 * int(n)));
        for (unsigned cone = 0; cone < m + (clockedTick ? 1u : 0u); ++cone) {
            const uint32_t mask = cone < m ? masks[cone] & inputMask : inputMask;
            const unsigned dependencies = popcount32(mask);
            if (dependencies < 2) { coneFirstGate.push_back(-1); continue; }
            coneFirstGate.push_back(cursor);
            cursor += 4 * int(dependencies - 1) + 3;
        }
    }
    if (precise) {
        unsigned gate = 0;
        const uint32_t inputMask = n >= 32 ? ~uint32_t(0) : ((uint32_t(1) << n) - 1);
        for (unsigned cone = 0; cone < m + (clockedTick ? 1u : 0u); ++cone) {
            const uint32_t mask = cone < m ? masks[cone] & inputMask : inputMask;
            const unsigned dependencies = popcount32(mask);
            const int first = coneFirstGate[cone];
            for (unsigned part = 1; part < dependencies; ++part, ++gate)
                w.component(0x17, kDependencyGateX, first + 4 * int(part - 1),
                            0x3000 + gate, "", 64);
        }
    } else {
        for (unsigned i = 1; i < n; ++i)
            w.component(0x17, kDependencyGateX, inputY(i), 0x3000 + i, "", 64);
    }
    for (unsigned i = 0; i < m; ++i) {
        const unsigned driverKind=triStateOutputs?(d.outputs[i].bits==1?0x0c:0x19):0x12;
        w.component(driverKind, outputDriverX, int(i) * 8, 0x4000 + i, "", d.outputs[i].bits);
    }
    if (precise && clockedTick)
        w.component(0x12, outputDriverX, -8, 0x4800, "", 1);
    if (m == 0) w.component(0x12, outputDriverX, 0, 0x4000, "", 1);
    for (unsigned i = 0; i < m; ++i)
        w.component(0x51, outputPinX, int(i) * 8, 0x5000 + i, d.outputs[i].name,
                    d.outputs[i].bits, i + 1);
    Writer wires; unsigned count = 0;
    auto wire = [&](std::initializer_list<Point> points) {
        ++count; wires.number(0, 1); wires.string("");
        auto p = points.begin(); Point last = *p++;
        wires.number(last.x, 2); wires.number(last.y, 2);
        for (; p != points.end(); ++p) {
            if (p->x != last.x) wires.number((p->x > last.x ? 0 : 0x8000) | std::abs(p->x - last.x), 2);
            if (p->y != last.y) wires.number((p->y > last.y ? 0x4000 : 0xc000) | std::abs(p->y - last.y), 2);
            last = *p;
        }
        wires.number(0, 2);
    };
    /* A pin's own connection point is three circuit units inside its edge: the
       generated input pin (kind 0x4f) drives from +3 and the output pin (kind
       0x51) reads at -3.  The wires follow the pins wherever the lane put
       them; a wire left behind at the old coordinate is exactly the failure
       that made an earlier attempt at a wider lane produce a dead board. */
    for (unsigned i = 0; i < n; ++i)
        wire({{inputPinX + 3, inputY(i)}, {kInputCollectorX - 1, inputY(i)}});
    /* The dependency chain: every gate (kind 0x17, two inputs at (-1,-1) and
       (-1,+1)) folds the chain so far together with one more input, two rows
       apart.  What the bridge needs is the *order* - the chain makes each
       input's collector run before the driver - so the value itself is beside
       the point.  The chain runs one column left of the gates and the inputs
       are tapped from the collector's output, so nothing has to travel outside
       the part. */
    if (precise) {
        const uint32_t inputMask = n >= 32 ? ~uint32_t(0) : ((uint32_t(1) << n) - 1);
        /* One private vertical column per *input* (not per cone).

           The game merges wires whose coordinates touch, so two cones sharing a
           column are merged: a tap left on x = -1 (which is also the gate pin
           column) both runs through every other gate's input pin and, because
           every cone used that one column, shorted the address cones to the
           read-data cones - the false loop came back through the board.  Each
           input now descends in its own column and hops out of the collector
           inside its own row gap, so the only wires that can ever overlap carry
           the same collector net. */
        // Trunks must be left of the public input wires, including wide shapes.
        // Each collector only travels one row in the shared escape column;
        // longer hops overlap once the input count exceeds eight.
        auto tapColumn = [&](unsigned input) {
            return std::min(kInputCollectorX - 2, inputPinX + 2) - int(input);
        };
        auto tapHopRow = [&](unsigned input) { return inputY(input) + 1; };
        // A fractional pin lane can place the driver corridor on the gate
        // output column. Move only that corridor to keep private cones apart.
        const int dependencyRouteX = outputDriverX - 2 == kDependencyGateX + 2
            ? kDependencyGateX : outputDriverX - 2;
        auto tapInto = [&](unsigned input, int targetRow, int targetX) {
            const int column = tapColumn(input);
            const int hop = tapHopRow(input);
            /* A horizontal at the pin's own row would run through the collector
               pins when that row is an input row (8-unit bands: always for an
               odd input count, at +4 for an even one); hop one row and come in
               through the gate's left corridor instead. */
            bool throughCollectors = false;
            for (unsigned k = 0; k < n; ++k)
                if (inputY(k) == targetRow) throughCollectors = true;
            if (!throughCollectors) {
                wire({{kInputCollectorX + 2, inputY(input)},
                      {kDependencyGateX - 3, inputY(input)},
                      {kDependencyGateX - 3, hop},
                      {column, hop},
                      {column, targetRow},
                      {targetX, targetRow}});
                return;
            }
            const int approach = targetRow + 1;
            wire({{kInputCollectorX + 2, inputY(input)},
                  {kDependencyGateX - 3, inputY(input)},
                  {kDependencyGateX - 3, hop},
                  {column, hop},
                  {column, approach}});
            wire({{column, approach},
                  {kDependencyGateX - 3, approach},
                  {kDependencyGateX - 3, targetRow},
                  {targetX, targetRow}});
        };
        for (unsigned cone = 0; cone < m + (clockedTick ? 1u : 0u); ++cone) {
            const uint32_t mask = cone < m ? masks[cone] & inputMask : inputMask;
            std::vector<unsigned> dependencies;
            for (unsigned input = 0; input < n; ++input)
                if (mask & (uint32_t(1) << input)) dependencies.push_back(input);
            if (dependencies.empty()) continue;
            const int driverY = cone < m ? int(cone) * 8 : -8;
            const int gates = int(dependencies.size()) - 1;
            const int first = coneFirstGate[cone];
            for (int part = 0; part < gates; ++part) {
                const int gateY = first + 4 * part;   /* the gate's centre row */
                if (part == 0) {
                    tapInto(dependencies[0], gateY - 1, kDependencyGateX - 1);
                } else {
                    /* The chain reaches this gate's "-" pin through the free row
                       between the two gates: out of the previous gate, right into
                       the free column beside the drivers, along the gap row, then
                       down into the pin from the left.  The straight hop the first
                       cut used ran across the gate body and landed on a pin the
                       tap of another input already owned. */
                    const int previousY = gateY - 4;
                    wire({{kDependencyGateX + 2, previousY},
                          {dependencyRouteX, previousY},
                          {dependencyRouteX, gateY - 2},
                          {kDependencyGateX - 2, gateY - 2},
                          {kDependencyGateX - 2, gateY - 1},
                          {kDependencyGateX - 1, gateY - 1}});
                }
                tapInto(dependencies[size_t(part) + 1], gateY + 1, kDependencyGateX - 1);
            }
            if (gates == 0) {
                /* One dependency only: come in one row below the driver and hop
                   down, so the approach never runs along a collector row. */
                tapInto(dependencies[0], driverY + 1, outputDriverX - 3);
                wire({{outputDriverX - 3, driverY + 1}, {outputDriverX - 3, driverY},
                      {outputDriverX - 1, driverY}});
            } else {
                const int lastGateY = first + 4 * (gates - 1);
                wire({{kDependencyGateX + 2, lastGateY},
                      {dependencyRouteX, lastGateY},
                      {dependencyRouteX, driverY},
                      {outputDriverX - 1, driverY}});
            }
        }
        for (unsigned i = 0; i < m; ++i) {
            const int y = int(i) * 8;
            wire({{outputDriverX + 2, y}, {outputPinX - 3, y}});
        }
    } else {
        Point previous{kInputCollectorX + 2, inputY(0)};
        for (unsigned i = 1; i < n; ++i) {
            const int y = inputY(i);
            wire({previous, {kDependencyGateX - 3, previous.y},
                  {kDependencyGateX - 3, y - 1}, {kDependencyGateX - 1, y - 1}});
            wire({{kInputCollectorX + 2, y}, {kDependencyGateX - 1, y},
                  {kDependencyGateX - 1, y + 1}});
            previous = {kDependencyGateX + 2, y};
        }
        /* The chain ends on the first driver's input, which is the node the bridge
           replaces with the callback; the rest of the drivers are chained off its
           output below. */
        if (n)
            wire({previous, {outputDriverX - 3, previous.y}, {outputDriverX - 3, 0},
                  {outputDriverX - 1, 0}});
        for (unsigned i = 0; i < m; ++i) {
            const int y = int(i) * 8;
            wire({{outputDriverX + 2, y}, {outputPinX - 3, y}});
            if (i + 1 < m)
                wire({{outputDriverX + 2, y}, {outputDriverX + 4, y},
                      {outputDriverX + 4, y + 4}, {outputDriverX - 1, y + 4},
                      {outputDriverX - 1, y + 8}});
        }
    }
    w.number(count, 8); w.bytes.insert(w.bytes.end(), wires.bytes.begin(), wires.bytes.end());
    std::vector<uint8_t> result{14};
    size_t size = w.bytes.size();
    do { uint8_t b = size & 127; size >>= 7; result.push_back(b | (size ? 128 : 0)); } while (size);
    for (size_t pos = 0; pos < w.bytes.size();) {
        const auto length = std::min<size_t>(60, w.bytes.size() - pos);
        result.push_back(uint8_t((length - 1) << 2));
        result.insert(result.end(), w.bytes.begin() + pos, w.bytes.begin() + pos + length);
        pos += length;
    }
    return result;
}
}
