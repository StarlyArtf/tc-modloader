#pragma once
/* Flattening a custom component (kind 0x4e) into a board the netlist builder
   can read.

   A custom component is a *definition*: the game parsed a circuit and keeps it
   in its custom-prototype table, and the prototype carries that circuit as two
   counted arrays - `+0xe0` components / `+0xe8` data, `+0x100` wires / `+0x108`
   data - whose element layouts are the board's own (0x238-byte components, and
   0x40-byte definition wires).  docs/research/custom-components.md is the run
   that measured all of that.

   Two callers need this algorithm, and they must not drift apart:

     * the live sandbox-sim Mod, which reads the definitions out of the running
       game's prototype table;
     * simcore-view, which reads them out of the board file the Mod wrote.  A
       viewer that has no definitions can only refuse a board with a custom
       component on it, which is exactly what it used to do.

   So the algorithm lives here, parameterised by two lookups, and both callers
   plug in where their definition data comes from.

   Two properties the callers depend on:

     * **The instance keeps its slot.**  Its record is left in the component
       array and rewritten as kind 0 (the board's own kind, which the builder
       skips); the definition's internals are appended after every real
       component.  That is what lets a caller keep indexing by the *board's* own
       component index - the viewer draws the board it was handed while it
       simulates the flattened one.
     * **Pins join by index.**  The k-th Input Pin of the definition is bridged
       to the k-th input pin of the instance, and likewise for the outputs.  By
       index, because the instance's pin geometry is the normalised one the
       palette shows while the definition's pins sit wherever their author drew
       them (measured: `Symphony/DEC`'s instance pins are a vertical column at
       x=1 while its definition's seven output pins run along y=-4). */

#include "board.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace tcsim {

struct CustomDefinition {
    std::vector<BoardComponent> components;
    std::vector<BoardWire> wires;
};

/* Where a definition comes from: the game's prototype table live, the board
   file's own `defcustom` sections offline. */
using CustomDefinitionLookup = std::function<bool(uint64_t custom_id, CustomDefinition& out)>;
/* The instance's own pin geometry, which is what the bridges are drawn from.
   A live caller reads it from the prototype table, an offline one from the
   pins the record carried. */
using CustomGeometryLookup = std::function<bool(const BoardComponent& component, KindPins& out)>;

/* Definitions never share a coordinate region: each instance's copy is moved a
   further 200 cells along x. */
inline constexpr int kCustomFlattenShift = 100000;
inline constexpr int kCustomFlattenStep = 200;
inline constexpr uint32_t kCustomFlattenDepth = 8;

/* Appends one instance's definition, translated, and bridges its pins.  Returns
   false when the definition could not be read, in which case the caller keeps
   the record as it found it and the builder reports it as an unknown custom
   component (red line 6: say so, never guess). */
inline bool flattenCustomInstance(const BoardComponent& instance, int shift, uint32_t depth,
                                  const CustomDefinitionLookup& definitions,
                                  const CustomGeometryLookup& geometry, BoardView& out, int& next_shift) {
    if (depth > kCustomFlattenDepth) return false;
    CustomDefinition definition;
    if (!definitions(instance.custom_id, definition)) return false;
    KindPins pins;
    if (!geometry(instance, pins)) return false;
    /* The definition's own boundary pins, in array order: that order is the pin
       order the palette shows (measured on `Symphony/ADDRDECODER-3`: its three
       Input Pins run top to bottom exactly like in0/in1/in2). */
    std::vector<BoardComponent> inputs;
    std::vector<BoardComponent> outputs;
    for (const BoardComponent& component : definition.components) {
        if (component.kind == 0x4f) inputs.push_back(component);
        else if (component.kind == 0x51) outputs.push_back(component);
    }
    if (inputs.size() != instance.input_count || outputs.size() != instance.output_count) return false;
    const int dx = shift;
    const int dy = 0;
    for (const BoardComponent& component : definition.components) {
        BoardComponent copy = component;
        copy.x = static_cast<int16_t>(component.x + dx);
        copy.y = static_cast<int16_t>(component.y + dy);
        if (copy.kind == 0x4e) {
            /* A nested custom component gets a region of its own.  Its
               definition's internals are routinely far larger than the cells the
               instance occupies, so laying them over the parent's own
               components shorts unrelated nets - measured on the player's
               board, where one definition's 4-bit Splitter output landed on
               another definition's Negate output (`component pins 2 share the
               cell … out3 … out0`).  The bridges below still join the nested
               instance's pins (in the parent's coordinates) to its own copy of
               the circuit. */
            const int nested_shift = next_shift;
            next_shift += kCustomFlattenStep;
            BoardComponent nested = copy;
            if (!flattenCustomInstance(nested, nested_shift, depth + 1, definitions, geometry, out, next_shift)) {
                out.components.push_back(copy);
            }
            continue;
        }
        out.components.push_back(copy);
    }
    for (const BoardWire& wire : definition.wires) {
        BoardWire copy = wire;
        copy.x1 = static_cast<int16_t>(wire.x1 + dx);
        copy.y1 = static_cast<int16_t>(wire.y1 + dy);
        copy.x2 = static_cast<int16_t>(wire.x2 + dx);
        copy.y2 = static_cast<int16_t>(wire.y2 + dy);
        copy.slot = kNoStateSlot;
        out.wires.push_back(copy);
    }
    /* The bridges.  The instance's own pins sit where the board's wires already
       end, so a wire from there to the definition's boundary pin joins them into
       one net without touching the outer wiring at all. */
    for (size_t index = 0; index < inputs.size(); ++index) {
        BoardWire bridge;
        bridge.x1 = static_cast<int16_t>(instance.x + pins.in_x[index]);
        bridge.y1 = static_cast<int16_t>(instance.y + pins.in_y[index]);
        bridge.x2 = static_cast<int16_t>(inputs[index].x + dx);
        bridge.y2 = static_cast<int16_t>(inputs[index].y + dy);
        bridge.width = static_cast<uint16_t>(pins.in_bits[index] ? pins.in_bits[index] : 1);
        bridge.slot = kNoStateSlot;
        out.wires.push_back(bridge);
    }
    for (size_t index = 0; index < outputs.size(); ++index) {
        BoardWire bridge;
        bridge.x1 = static_cast<int16_t>(outputs[index].x + dx);
        bridge.y1 = static_cast<int16_t>(outputs[index].y + dy);
        bridge.x2 = static_cast<int16_t>(instance.x + pins.out_x[index]);
        bridge.y2 = static_cast<int16_t>(instance.y + pins.out_y[index]);
        bridge.width = static_cast<uint16_t>(pins.out_bits[index] ? pins.out_bits[index] : 1);
        bridge.slot = kNoStateSlot;
        out.wires.push_back(bridge);
    }
    return true;
}

/* The whole board, flattened.  The board's own wires keep their indices and
   their slots, so anything keyed by wire index (`net_of_wire`, the viewer's
   value lookup) still lines up. */
inline BoardView flattenCustomComponents(const BoardView& view, const CustomDefinitionLookup& definitions,
                                         const CustomGeometryLookup& geometry) {
    BoardView out;
    out.wires = view.wires;
    int next_shift = kCustomFlattenShift;
    for (const BoardComponent& component : view.components) {
        if (component.kind != 0x4e || customIsClock(component.custom_id) ||
            customIsInteractiveSource(component.custom_id)) {
            out.components.push_back(component);
            continue;
        }
        const size_t slot = out.components.size();
        /* Every flattened definition takes one 200-cell region, and those
           coordinates live in an int16: past the end of that range the regions
           would start to overlap each other.  Refuse rather than short two
           circuits together - the builder then reports the component, which is
           the honest answer. */
        if (next_shift - kCustomFlattenShift + kCustomFlattenStep > 30000) {
            out.components.push_back(component);
            continue;
        }
        out.components.push_back(component);
        const int shift = next_shift;
        next_shift += kCustomFlattenStep;
        if (!flattenCustomInstance(component, shift, 0, definitions, geometry, out, next_shift)) {
            /* Unknown definition: keep the instance exactly as it came and let
               the builder report it, rather than dropping a component the board
               is really wired to. */
            out.components.resize(slot + 1);
            continue;
        }
        out.components[slot].kind = 0x00;
    }
    return out;
}

}  // namespace tcsim
