#pragma once
/* Static pin geometry for the built-in prototypes the sandbox uses.

   The live plugin asks the game's own prototype table for this
   (simcore/include/tcsim/board.hpp takes a `geometry` callback for exactly
   that reason).  A viewer that runs with no game attached cannot, so this file
   freezes the same data - and a hand-written copy of a table that the game
   keeps changing is exactly the thing that drifts.  It did: the viewer refused
   a board with a 3 Bit Decoder, a Register or a Splitter while the live Mod
   simulated the very same board.  The rows now come from
   `kind_pins_generated.hpp`, which tools/kind-pins.py writes from
   build/kinds.txt, and a live board record that carries its own `inpins=` /
   `outpins=` (the Mod writes them for every component) outranks both.

   Offsets are relative to the component's own cell.  The y axis grows down,
   which is the direction the prototype table's own offsets use (`in_y = -1`
   is the pin above the body).

   A kind that is not listed here is NOT guessed: the viewer still draws the
   component and reports it, which is the same rule the netlist builder
   follows (docs/PLAN-sandbox-simulator.md red line 6, "器件不支持的电路要
   明说，不静默算错"). */

#include "../include/tcsim/board.hpp"

#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace simcore_view {

struct KindGeometry {
    uint8_t kind = 0;
    uint8_t input_count = 0;
    uint8_t output_count = 0;
    /* The game's own tables are what set the width of these arrays: a built-in
       stops at eight leaves (Splitter / Maker / Concatenator / Output), and a
       custom component's interface is whatever its author drew - the player's
       own `Symphony/DEC` has seven outputs, a 5-to-32 decoder thirty-two.  The
       record format carries the pins of a live board anyway (see
       board_file.hpp), so this table is the fallback for a fixture that has
       none. */
    int8_t in_x[tcsim::KindPins::kMaxPins] = {};
    int8_t in_y[tcsim::KindPins::kMaxPins] = {};
    int8_t out_x[tcsim::KindPins::kMaxPins] = {};
    int8_t out_y[tcsim::KindPins::kMaxPins] = {};
    const char* name = "";
    /* Per-pin widths from the prototype table.  Regenerated from
       build/kinds.txt by tools/kind-pins.py - the hand-written copy of this
       table is exactly what drifted out of date and made the viewer refuse a
       board the live Mod simulated. */
    uint16_t in_bits[tcsim::KindPins::kMaxPins] = {};
    uint16_t out_bits[tcsim::KindPins::kMaxPins] = {};
};

#include "kind_pins_generated.hpp"

inline const KindGeometry* geometryForKind(uint8_t kind) {
    for (const KindGeometry& row : kKindGeometry) {
        if (row.kind == kind) return &row;
    }
    return nullptr;
}

/* The sandbox's own Mod types (examples/clock, `local.clock`): a custom prototype
   is known by its id, not by a kind byte - all three are one output, no inputs.
   The offset is measured from the running game rather than assumed:
   `sandbox-sim: custom 3 id=0x535749545f303031 at (-15,-18) … out0=(-13,-18)` and
   the same shape for the button, i.e. (+2, 0) relative to the instance.  The
   clock has the same declaration. */
struct CustomGeometry {
    uint64_t custom_id = 0;
    const char* name = "";
};

inline const CustomGeometry kCustomGeometry[] = {
    {0x434C4F4B5F303031ULL, "Clock"},   /* CLOK_001 */
    {0x535749545F303031ULL, "Switch"},  /* SWIT_001 */
    {0x4255544E5F303031ULL, "Button"},  /* BUTN_001 */
};

inline const CustomGeometry* geometryForCustomId(uint64_t custom_id) {
    if (!custom_id) return nullptr;
    for (const CustomGeometry& row : kCustomGeometry) {
        if (row.custom_id == custom_id) return &row;
    }
    return nullptr;
}

/* Pins the board record carried for a component, keyed by its instance id.  The
   frozen table cannot know a Mod type's pins - only the game did - so the viewer
   registers them here before building anything, and every lookup (the drawing's
   and the netlist's alike) finds them. */
inline std::map<uint64_t, KindGeometry>& recordGeometry() {
    static std::map<uint64_t, KindGeometry> table;
    return table;
}

/* Fills a tcsim::KindPins for one component.  The order of authority is

     1. the pins the board record carried for this instance - the live Mod
        writes them for *every* component, so a board that came off the running
        game is always described by the game's own prototype table;
     2. the frozen table for the kind (`kind_pins_generated.hpp`);
     3. the sandbox's own Mod types, which are known by id, not by kind.

   Returns false when none of the three covers the component, so the caller
   reports it instead of inventing pins. */
inline bool pinsForKind(const tcsim::BoardComponent& component, tcsim::KindPins& out) {
    const KindGeometry* row = nullptr;
    if (component.id) {
        const auto found = recordGeometry().find(component.id);
        if (found != recordGeometry().end()) row = &found->second;
    }
    if (!row) row = geometryForKind(component.kind);
    if (!row) {
        /* Not a built-in: the sandbox's clock/switch/button are one output at
           (2, 0) with nothing on the input side. */
        const CustomGeometry* custom = geometryForCustomId(component.custom_id);
        if (!custom) return false;
        out = tcsim::KindPins{};
        out.kind = component.kind;
        out.name = custom->name;
        out.input_count = 0;
        out.output_count = 1;
        out.out_x[0] = 2;
        out.out_y[0] = 0;
        out.out_bits[0] = 1;
        return true;
    }
    const uint8_t inputs = std::min<uint8_t>(row->input_count, tcsim::KindPins::kMaxPins);
    const uint8_t outputs = std::min<uint8_t>(row->output_count, tcsim::KindPins::kMaxPins);
    out = tcsim::KindPins{};
    out.kind = row->kind;
    out.name = row->name;
    out.input_count = inputs;
    out.output_count = outputs;
    for (uint8_t index = 0; index < inputs; ++index) {
        out.in_x[index] = row->in_x[index];
        out.in_y[index] = row->in_y[index];
        out.in_bits[index] = row->in_bits[index] ? row->in_bits[index] : 1;
    }
    for (uint8_t index = 0; index < outputs; ++index) {
        out.out_x[index] = row->out_x[index];
        out.out_y[index] = row->out_y[index];
        out.out_bits[index] = row->out_bits[index] ? row->out_bits[index] : 1;
    }
    return true;
}

}  // namespace simcore_view
