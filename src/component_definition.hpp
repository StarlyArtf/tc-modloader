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
/* `maxPins` is the caller's shape limit: the V1 callback struct carries eight
   pins per direction, while a V2 definition (tc.component.types) may declare up
   to sixteen.  Everything else - either direction may be empty, each pin 1..64
   bits, total input width within the payload budget - is the same for both,
   because it is the generated call that sets those limits.

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
                pins[i].bits < 1 || pins[i].bits > 64) return false;
            if (!dir) total += pins[i].bits;
        }
    }
    return total <= 128;
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
// Version 3 scaffold contract: N unary input collectors, N-1 dependency gates,
// M unary output drivers, in that serialized order. Only output drivers are
// replaced. The dependency chain schedules every input before the callback.
inline std::vector<uint8_t> encode(const TCNativeComponentDefinition& d, unsigned maxPins = 8,
                                   float pinLane = kDefaultPinLane) {
    if (!validShape(&d, maxPins)) return {};
    if (!validPinLane(pinLane)) return {};
    const bool defaultLane = pinLane == kDefaultPinLane;
    const int laneUnits = static_cast<int>(std::lround(pinLane * kPinLaneUnitsPerCell));
    const int inputPinX = defaultLane ? kDefaultInputPinX : -laneUnits;
    const int outputPinX = defaultLane ? kDefaultOutputPinX : laneUnits;
    const unsigned n = d.input_count, m = d.output_count;
    Writer w;
    w.number(d.custom_id, 8); w.number(0, 4); w.number(d.gate_cost, 8); w.number(d.delay, 8);
    w.number(1, 1); w.number(10000, 8); w.number(0, 2); w.string("");
    w.number(0, 1); w.number(0, 2); w.number(0, 2); w.string("");
    for (int i = 0; i < 512; ++i) w.number(0, 1);
    /* Node count: n input pins, n input collectors, n-1 dependency gates,
       m output drivers and m output pins.  With no inputs there are no pins,
       no collectors and no dependency chain to seed - the drivers stand alone
       and their input is unconnected, which is exactly the case the real
       machine is asked about (see docs/research/custom-component-pins.md). */
    /* A sink (no outputs) still needs one node to run on: the game only emits
       code for logic it can reach, and with nothing driving it a callback would
       never be called.  That node's output is deliberately left unconnected -
       whether the compiler keeps it is the measurement, see
       docs/research/custom-component-pins.md. */
    w.number((n ? 3 * n - 1 : 0) + (m ? 2 * m : 1), 8);
    auto inputY = [n](unsigned i) { return int(i) * 8 - int(n - 1) * 4; };
    for (unsigned i = 0; i < n; ++i)
        w.component(0x4f, inputPinX, inputY(i), 0x1000 + i, d.inputs[i].name,
                    d.inputs[i].bits, i + 1);
    for (unsigned i = 0; i < n; ++i)
        w.component(0x12, kInputCollectorX, inputY(i), 0x2000 + i, "", d.inputs[i].bits);
    for (unsigned i = 1; i < n; ++i)
        w.component(0x17, kDependencyGateX, inputY(i), 0x3000 + i, "", 64);
    for (unsigned i = 0; i < m; ++i)
        w.component(0x12, kOutputDriverX, int(i) * 8, 0x4000 + i, "", d.outputs[i].bits);
    if (m == 0) w.component(0x12, kOutputDriverX, 0, 0x4000, "", 1);
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
        wire({previous, {kOutputDriverX - 3, previous.y}, {kOutputDriverX - 3, 0},
              {kOutputDriverX - 1, 0}});
    for (unsigned i = 0; i < m; ++i) {
        const int y = int(i) * 8;
        wire({{kOutputDriverX + 2, y}, {outputPinX - 3, y}});
        if (i + 1 < m)
            wire({{kOutputDriverX + 2, y}, {kOutputDriverX + 4, y},
                  {kOutputDriverX + 4, y + 4}, {kOutputDriverX - 1, y + 4},
                  {kOutputDriverX - 1, y + 8}});
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
