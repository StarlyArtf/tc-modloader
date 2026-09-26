/* The sandbox simulator (docs/PLAN-sandbox-simulator.md, S2): the sandbox's
   circuit is evaluated by simcore, not by the program the game compiled.

   What this Mod does, once per game cycle:

     1. reads the board's records (components, wires, and the state byte each
        wire owns) - no emitted source, no game compiler;
     2. builds a netlist and feeds the engine the nets the game drives from its
        own state array (an Input, a Switch);
     3. advances the engine one cycle (cycle_units * ticks_per_unit ticks);
     4. publishes every net's bytes back into the state array, which is where
        the board, the probes and the scope read.

   The switch is a file, not a checkbox yet: `<game>/tc-modloader-data/
   sandbox-sim.txt` with `enabled=1`.  Off (or missing) is the default and the
   Mod then does nothing at all - red line 1 in the plan.  `invert=1` publishes
   the opposite value, which is how the real-machine test proves the bytes show
   what we wrote.  `trace=1` adds the per-wire diagnostics described below, and
   `unblock=0` leaves the game's compile passes untouched (see the plan, 14.6).
   `steps_per_cycle=N` publishes N times inside every cycle instead of once at
   its end, which is the only way an edge detector's one-gate-delay pulse can
   reach the board and the scope (the plan's section 14.7); with `gate_delay`
   set to a whole unit (1024 ticks) that pulse is one published step wide.

   Values reach the screen through one hard contract: the board uploads the whole
   `simulation_state` array as a texture buffer every frame, and each wire
   segment carries a single index the shader samples.  That index is
   `get_state_index(wire record)`, so a wire the game never allocated - its
   descriptor stays {1, 256, 1} and resolves to byte 1, shared with every other
   such wire - would be drawn from a byte that is not its value.  This Mod
   therefore owns those indices: it copies the descriptor shape the game itself
   allocated, patches only the offset, and confirms the crafted descriptor with
   the game's own resolver before writing it back (see
   `ensureWireStateIndices`).  The plan's section 14.6 carries the measurements.

   Everything about the circuit lives in simcore; this file is only the glue. */

#include "../../sdk/tc_mod.h"
#include "../../sdk/tc_hook.h"
#include "../../sdk/tc_component_instances.h"
#include "../../sdk/tc_component_storage.h"
#include "../../simcore/include/tcsim/board.hpp"
#include "../../simcore/include/tcsim/engine.hpp"
#include "../../simcore/include/tcsim/runtime.hpp"
#include "cycle_unblock.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

const TCHost* host = nullptr;
tc::TCMod mod;
bool enabled = false;
bool invert_publish = false;
bool trace_events = false;
bool startup_seed = false;
bool sandbox_active = false;
/* Diagnostic switch (2026-09-25): 0 leaves the game's own cycle/compile passes
   untouched, which is how the "does our cut cause the wire table to be rewritten"
   question is answered with one variable changed. */
bool unblock_enabled = true;
/* Advance control (S2).  The game will not advance a board it cannot compile
   (measured: the closed three-NAND ring reads `timed out waiting for cycle 1`),
   so the simulator needs a clock of its own.  `free_run` is that clock: advance
   one engine cycle every N render frames, publish, and leave the game's cycle
   counter - and with it the level's own inputs and judging - untouched.  It is
   what red line 4 asks for: the sandbox's circuit moves, the level does not. */
int free_run_frames = 0;
uint64_t free_run_counter = 0;
/* Sub-cycle publishing (S2, 2026-09-25).  A cycle is `cycle_units` units and
   one unit is `ticks_per_unit` ticks, but an edge detector's pulse is only one
   gate delay wide - measured: the AND output is high for 8 ticks out of 8192.
   Publishing once per cycle therefore never shows it.  `steps_per_cycle` splits
   each cycle into that many engine steps and publishes after every one, so the
   board, the probes and the scope see the inside of a cycle as well. */
int steps_per_cycle = 1;
std::vector<std::string> history;
/* Ownership of the cycle counter (S2 advance control, owner-approved).  The
   counter is not a field of the board the sim.do chain hands over (a scan of the
   model's first page found nothing that tracked it), so the honest place to own
   it is its reader: hook `sim.cycle` and answer with the number our simulation
   has actually reached.  Everything that reads the cycle - the scope capture,
   the per-cycle samplers, the cycle on screen - then follows *our* simulation,
   while the level's own inputs and judging stay the game's (red line 4). */
bool own_cycle = false;
int64_t owned_cycle = 0;
using GetCycleFn = int64_t (*)();
GetCycleFn cycle_original = nullptr;
void* level_board_model = nullptr;

using WireCopyFn = void* (*)(void*, const void*);
using PreorderFn = int64_t (*)(void*, void*, void*);
using CycleSearchFn = int64_t (*)(void*, void*, void*, void*);
using SetCircularFn = void (*)(void*);
/* The game's own "which state index does this wire read" resolver.  The board's
   wire renderer calls it for every segment it pushes
   (push_tbo_wire_segment -> get_state_index) and then uploads the whole state
   array as a texture buffer each frame, so this number *is* the address the
   shader samples for a wire.  Our write-back uses the raw +0x38 field; logging
   both answers "does the colour come from the bytes we publish". */
using StateIndexFn = uint64_t (*)(const void*, uint64_t);
StateIndexFn game_state_index = nullptr;
/* Owning the index (S2 rendering fix, 2026-09-25).  A wire the game never
   allocated keeps the descriptor {1, 256, 1} - it is the "no state" shape, and
   every such wire resolves to byte 1, so several wires share one byte and the
   board paints them all with whatever that byte happens to hold.  Measured on
   the closed three-NAND ring: the wire the compiler pass skips (the one we
   temporarily cut) keeps this shape while its neighbours get 256/258/260/262/264.
   The fix is to hand such a wire a private offset, using a descriptor the game
   itself allocated as the template, and to confirm the game's own resolver
   agrees before writing it back. */
uint64_t index_patch_count = 0;
uint64_t index_patch_base = 0;
/* The interactive switch/button (local.clock's SWIT_001 / BUTN_001) keeps its
   level in the host-owned instance configuration.  In the sandbox the game's
   generated program - the thing that used to copy that level into a wire slot -
   is suppressed, so reading the state array alone left the switch frozen at its
   power-on value: clicking it changed nothing in the simulation.  These two
   services read the instance's own configuration instead. */
tc::component_instances::Api instance_api{};
tc::component_storage::Api storage_api{};
std::map<uint64_t, uint8_t> interactive_levels;
std::map<uint64_t, uint8_t> interactive_logged;
/* Diagnostic (config `interactive_level=`): store this level into every
   interactive instance and refresh its binding, i.e. do exactly what a click
   does - which is how the switch's path is tested without a synthetic mouse. */
int interactive_override = -1;
/* The descriptor shape the game itself uses for an allocated wire (measured:
   {index, 1, 0}).  Remembered across boards, so a board whose wires are *all*
   inside a cycle - where the game never allocates anything - can still be
   served a private index. */
unsigned char index_template[0x18] = {0};
bool have_index_template = false;
const void* index_checked_board = nullptr;
size_t index_checked_wires = 0;
uint64_t index_check_tick = 0;
/* The unblock cut may only live inside the compile pass it was made for.  If
   that pass aborts (which is exactly what the circular-dependency check does)
   nobody would ever restore it, and the board would be left with a wire whose
   endpoint is 64 cells away - measured 2026-09-25: the game then rewrites that
   wire into per-segment records that carry no state index at all. */
uint64_t pending_cut_tick = 0;
/* The compile passes that need the cut run back to back on the thread that is
   also doing the loading, so no frame (and therefore no board read of ours) can
   land inside the window; 250 ms is far longer than those passes take and far
   shorter than the load itself. */
constexpr uint64_t kCutWatchdogMs = 250;
WireCopyFn wire_copy_original = nullptr;
PreorderFn preorder_original = nullptr;
CycleSearchFn cycle_search_original = nullptr;
SetCircularFn set_circular_original = nullptr;
std::vector<sandbox_cycle_unblock::Cut> pending_compile_cuts;

int64_t hookedCycle() {
    return enabled && sandbox_active && own_cycle ? owned_cycle : (cycle_original ? cycle_original() : 0);
}
uint64_t ticks_per_unit = tcsim::kTicksPerUnit;
uint64_t cycle_units = tcsim::kDefaultCycleUnits;
uint64_t gate_delay = 8;

TCBoardApiV1 board_api{};
bool have_board = false;
unsigned char** state_global = nullptr;
uint64_t state_size = 0x9c4000;

tcsim::SandboxRuntime runtime;
bool runtime_bound = false;
const void* bound_board = nullptr;
size_t bound_components = 0;
/* The state offsets arrive with the game's compile, so a board that has not been
   compiled yet reads as placeholder slots (measured: 1, 2, ...).  Binding again
   when the signature changes is what turns "the board exists" into "the board has
   addresses we can write to". */
uint64_t bound_slot_signature = 0;
const void* failed_board = nullptr;
size_t failed_components = 0;
uint64_t failed_slot_signature = 0;

uint64_t slotSignature(const tcsim::BoardView& view) {
    uint64_t signature = view.components.size() * 1000003u ^ (view.wires.size() * 65537u);
    for (const tcsim::BoardWire& wire : view.wires) signature = signature * 31u + wire.slot;
    return signature;
}
/* The board the *simulation* hands out, captured from the loader's sim.do chain.
   That is the object the probe's dumps were taken from (its wire records carry
   the state offsets the board reads), so it is the one to build from; the board
   service's pointer is a fallback for a session that has not run yet. */
const void* simulation_model = nullptr;
int64_t last_cycle = -1;
uint64_t published = 0;
uint64_t cycles_run = 0;
uint64_t seeded_nets = 0;
std::string last_error;
double report_time = -1.0;

void log(const std::string& text) {
    if (host && host->log) host->log(host->context, text.c_str());
}

std::string dataPath(const char* name) {
    if (!host || !host->data_directory_utf8) return {};
    return (std::filesystem::u8path(host->data_directory_utf8) / name).u8string();
}

/* The board's own prototype table answers the pin geometry, so this Mod and the
   offline test run the same builder over the same numbers. */
bool prototypeGeometry(const tcsim::BoardComponent& component, tcsim::KindPins& out) {
    tc::TCPrototype prototype{};
    if (!mod.game.getPrototype(component.kind, component.custom_id, prototype)) return false;
    const uint64_t inputs = tc::prototypeInputCount(prototype);
    const uint64_t outputs = tc::prototypeOutputCount(prototype);
    if (inputs > 8 || outputs > 4) return false;
    out = tcsim::KindPins{};
    out.kind = component.kind;
    out.name = tc::prototypeNameCStr(prototype);
    out.input_count = static_cast<uint8_t>(inputs);
    out.output_count = static_cast<uint8_t>(outputs);
    for (uint64_t index = 0; index < inputs; ++index) {
        const tc::TCPinPoint point = tc::prototypeInputPinPoint(prototype, index);
        out.in_x[index] = static_cast<int8_t>(point.x);
        out.in_y[index] = static_cast<int8_t>(point.y);
        const uint64_t raw = tc::prototypeInputPinWordSize(prototype, index);
        out.in_bits[index] = tc::pinWordSizeIsAuto(raw) || raw == 0 ? 1 : static_cast<uint16_t>(raw < 64 ? raw : 64);
    }
    for (uint64_t index = 0; index < outputs; ++index) {
        const tc::TCPinPoint point = tc::prototypeOutputPinPoint(prototype, index);
        out.out_x[index] = static_cast<int8_t>(point.x);
        out.out_y[index] = static_cast<int8_t>(point.y);
        const uint64_t raw = tc::prototypeOutputPinWordSize(prototype, index);
        out.out_bits[index] = tc::pinWordSizeIsAuto(raw) || raw == 0 ? 1 : static_cast<uint16_t>(raw < 64 ? raw : 64);
    }
    return true;
}

uint64_t unblock_events = 0;

constexpr uint64_t kSwitchCustomId = 0x535749545F303031ULL; /* SWIT_001 */
constexpr uint64_t kButtonCustomId = 0x4255544E5F303031ULL; /* BUTN_001 */
uint32_t interactive_override_applied = 0;

/* Reads every interactive instance's configured level.  The click handler in
   local.clock stores the level there before it refreshes the instance, so this
   is the click's own value, not a guess. */
void refreshInteractiveLevels() {
    if (!instance_api.enumerate || !storage_api.read_config) return;
    const uint64_t ids[2] = {kSwitchCustomId, kButtonCustomId};
    std::vector<TCComponentInstanceHandle> handles(32);
    for (uint64_t id : ids) {
        uint32_t written = 0;
        uint32_t total = 0;
        int status = tc::component_instances::enumerate(
            instance_api, id, handles.data(), static_cast<uint32_t>(handles.size()), &written, &total);
        if (status == TC_COMPONENT_INSTANCES_ERR_RANGE && total > handles.size()) {
            handles.resize(total);
            status = tc::component_instances::enumerate(
                instance_api, id, handles.data(), static_cast<uint32_t>(handles.size()), &written, &total);
        }
        if (status != TC_COMPONENT_INSTANCES_OK) {
            if (trace_events)
                log("sandbox-sim: interactive instances unavailable (status " + std::to_string(status) + ")");
            continue;
        }
        for (uint32_t index = 0; index < written; ++index) {
            uint8_t config[8] = {0};
            uint32_t count = 0;
            if (storage_api.read_config(storage_api.context, &handles[index], config, sizeof(config), &count) !=
                    TC_COMPONENT_STORAGE_OK ||
                count < 1)
                continue;
            interactive_levels[handles[index].instance_id] = static_cast<uint8_t>(config[0] & 1u);
        }
    }
    if (!trace_events) return;
    for (const auto& entry : interactive_levels) {
        const auto seen = interactive_logged.find(entry.first);
        if (seen != interactive_logged.end() && seen->second == entry.second) continue;
        interactive_logged[entry.first] = entry.second;
        std::ostringstream line;
        line << "sandbox-sim: interactive 0x" << std::hex << entry.first << std::dec
             << " level=" << static_cast<int>(entry.second) << " (from its own configuration)";
        log(line.str());
    }
}

/* Diagnostic: store `interactive_level` into every interactive instance and
   refresh its binding - exactly what a click does - so the switch's path can be
   exercised without a synthetic mouse. */
void applyInteractiveOverride() {
    if (interactive_override < 0 || !instance_api.enumerate || !storage_api.write_config) return;
    const uint64_t ids[2] = {kSwitchCustomId, kButtonCustomId};
    std::vector<TCComponentInstanceHandle> handles(32);
    uint32_t applied = 0;
    for (uint64_t id : ids) {
        uint32_t written = 0;
        uint32_t total = 0;
        int status = tc::component_instances::enumerate(
            instance_api, id, handles.data(), static_cast<uint32_t>(handles.size()), &written, &total);
        if (status == TC_COMPONENT_INSTANCES_ERR_RANGE && total > handles.size()) {
            handles.resize(total);
            status = tc::component_instances::enumerate(
                instance_api, id, handles.data(), static_cast<uint32_t>(handles.size()), &written, &total);
        }
        if (status != TC_COMPONENT_INSTANCES_OK) continue;
        for (uint32_t index = 0; index < written; ++index) {
            uint8_t config[8] = {0};
            uint32_t count = 0;
            if (storage_api.read_config &&
                storage_api.read_config(storage_api.context, &handles[index], config, sizeof(config), &count) ==
                    TC_COMPONENT_STORAGE_OK &&
                count >= 1) {
                config[0] = static_cast<uint8_t>(interactive_override & 1);
            } else {
                config[0] = static_cast<uint8_t>(interactive_override & 1);
                config[1] = 0;
                count = 2;
            }
            const uint32_t bytes = count >= 2 ? 2u : 1u;
            if (tc::component_storage::writeConfig(storage_api, handles[index], 1u, config, bytes) !=
                TC_COMPONENT_STORAGE_OK)
                continue;
            if (instance_api.reset) (void)tc::component_instances::reset(instance_api, handles[index]);
            ++applied;
        }
    }
    interactive_override_applied = applied;
    log("sandbox-sim: interactive level forced to " + std::to_string(interactive_override & 1) + " on " +
        std::to_string(applied) + " instance(s) (click-equivalent write + reset)");
}

/* A source net's value: a host component answers from its own configuration,
   everything else from the byte the game's state array holds. */
uint64_t readSourceNet(tcsim::NetId net, const std::vector<uint64_t>& slots, unsigned char* buffer) {
    const tcsim::BuildResult& build = runtime.build();
    const std::vector<tcsim::NetId>& sources = runtime.sourceNets();
    if (build.source_origins.size() == sources.size()) {
        const auto found = std::find(sources.begin(), sources.end(), net);
        if (found != sources.end()) {
            const uint64_t instance =
                build.source_origins[static_cast<size_t>(found - sources.begin())].instance_id;
            if (instance) {
                const auto level = interactive_levels.find(instance);
                if (level != interactive_levels.end()) return level->second;
            }
        }
    }
    if (slots.empty() || slots[0] >= state_size) return 0;
    return buffer[slots[0]] & 1u;
}

void noteCuts(const char* phase, const std::vector<sandbox_cycle_unblock::Cut>& cuts) {
    if (cuts.empty()) return;
    ++unblock_events;
    if (unblock_events <= 12) {
        std::string which;
        for (const sandbox_cycle_unblock::Cut& cut : cuts) {
            if (!which.empty()) which += ",";
            which += std::to_string(cut.index);
        }
        log(std::string("sandbox-sim: cycle-unblock ") + phase + " temporarily cut " +
            std::to_string(cuts.size()) + " feedback wire(s) [" + which + "]");
    }
}

/* Dumps what every wire record says about its own state index, next to what the
   game's own resolver answers for the same record.  Read-only. */
/* Every custom component and the pin geometry the board's own prototype table
   answers for it.  This is what tells "the switch is unknown" apart from "the
   switch's prototype carries a scaffolding pin that happens to sit on a wire". */
void logCustomComponents(const tcsim::BoardView& view) {
    if (!trace_events) return;
    for (size_t index = 0; index < view.components.size() && index < 64; ++index) {
        const tcsim::BoardComponent& component = view.components[index];
        if (component.kind != 0x4e) continue;
        tcsim::KindPins shape;
        const bool known = prototypeGeometry(component, shape);
        std::ostringstream line;
        line << "sandbox-sim: custom " << index << " id=0x" << std::hex << component.custom_id << std::dec
             << " at (" << component.x << "," << component.y << ") rot=" << static_cast<int>(component.rotation)
             << (known ? (" pins=" + std::to_string(shape.input_count) + "in/" +
                          std::to_string(shape.output_count) + "out")
                       : std::string(" pins=unresolved"));
        if (known) {
            for (uint8_t pin = 0; pin < shape.input_count; ++pin) {
                const auto offset = tcsim::rotateOffset(shape.in_x[pin], shape.in_y[pin], component.rotation);
                line << " in" << static_cast<int>(pin) << "=(" << component.x + offset.first << ","
                     << component.y + offset.second << ")";
            }
            for (uint8_t pin = 0; pin < shape.output_count; ++pin) {
                const auto offset = tcsim::rotateOffset(shape.out_x[pin], shape.out_y[pin], component.rotation);
                line << " out" << static_cast<int>(pin) << "=(" << component.x + offset.first << ","
                     << component.y + offset.second << ")";
            }
        }
        log(line.str());
    }
}

void logWireBindings(const char* label, const void* board, const tcsim::BoardView& view,
                     const tcsim::BuildResult& build) {
    if (!trace_events || !board) return;
    const auto* bytes = static_cast<const unsigned char*>(board);
    uint64_t count = 0;
    const unsigned char* data = nullptr;
    std::memcpy(&count, bytes + 0x98, sizeof(count));
    std::memcpy(&data, bytes + 0xa0, sizeof(data));
    if (!data || count != view.wires.size()) {
        log(std::string("sandbox-sim: wire dump (") + label + ") skipped (table shape changed)");
        return;
    }
    for (size_t index = 0; index < view.wires.size() && index < 64; ++index) {
        const unsigned char* record = data + 8 + index * 0x68;
        unsigned char descriptor[0x18] = {0};
        std::memcpy(descriptor, record + 0x38, sizeof(descriptor));
        const uint64_t resolved = game_state_index ? game_state_index(descriptor, 0) : 0;
        const tcsim::BoardWire& wire = view.wires[index];
        std::ostringstream line;
        line << "sandbox-sim: wire[" << label << "] " << index
             << " net=" << (index < build.net_of_wire.size() ? static_cast<int>(build.net_of_wire[index]) : -1)
             << " raw=" << wire.slot << " index=" << (game_state_index ? std::to_string(resolved) : "n/a")
             << " width=" << wire.width
             << " (" << wire.x1 << "," << wire.y1 << ")->(" << wire.x2 << "," << wire.y2 << ")"
             << " desc=";
        for (unsigned char byte : descriptor) {
            char text[4] = {0};
            std::snprintf(text, sizeof(text), "%02x", byte);
            line << text;
        }
        log(line.str());
    }
}

void* hookedWireCopy(void* out, const void* components) {
    /* A cut never crosses into a new copy pass: whatever the previous compile
       left behind is restored before the game builds the next table. */
    sandbox_cycle_unblock::restore(pending_compile_cuts);
    void* result = wire_copy_original ? wire_copy_original(out, components) : out;
    if (enabled && sandbox_active && pending_compile_cuts.empty()) {
        pending_compile_cuts = sandbox_cycle_unblock::cutTables(
            const_cast<void*>(components), result, &prototypeGeometry);
        pending_cut_tick = GetTickCount64();
        noteCuts("wire-copy", pending_compile_cuts);
    }
    return result;
}

/* The cut exists so the compiler's own passes see an acyclic graph; it must not
   outlive them.  The preorder hook restores it on the normal path, this is the
   belt for the paths that abort (the circular-dependency check being one). */
void enforceCutDeadline() {
    if (pending_compile_cuts.empty()) return;
    if (GetTickCount64() - pending_cut_tick < kCutWatchdogMs) return;
    log("sandbox-sim: cycle-unblock cut outlived its compile pass; restored");
    sandbox_cycle_unblock::restore(pending_compile_cuts);
}

/* Every wire must own a state byte, otherwise its colour is not its value: the
   board's shader samples `simulation_state[get_state_index(wire)]`, and a wire
   the game never allocated resolves to the shared byte 1 (the descriptor
   {1, 256, 1}).  This hands such a wire a private offset, built from a
   descriptor the game allocated itself (every allocated one is {index, 1, 0},
   so only the leading index changes), and only after the game's own resolver
   confirms the crafted descriptor - the check is what makes this safe. */
void ensureWireStateIndices(const void* board, tcsim::BoardView& view) {
    if (!game_state_index || !board || view.wires.empty()) return;
    const auto* bytes = static_cast<const unsigned char*>(board);
    uint64_t count = 0;
    const unsigned char* data = nullptr;
    std::memcpy(&count, bytes + 0x98, sizeof(count));
    std::memcpy(&data, bytes + 0xa0, sizeof(data));
    if (!data || count != view.wires.size()) return;
    auto* records = const_cast<unsigned char*>(data);

    const size_t wires = view.wires.size();
    std::vector<unsigned char> descriptors(wires * 0x18, 0);
    std::vector<uint64_t> resolved(wires, 0);
    std::map<uint64_t, size_t> users;
    for (size_t index = 0; index < wires; ++index) {
        std::memcpy(descriptors.data() + index * 0x18, records + 8 + index * 0x68 + 0x38, 0x18);
        resolved[index] = game_state_index(descriptors.data() + index * 0x18, 0);
        users[resolved[index]] += 1;
    }

    /* A descriptor we may copy the shape from: allocated by the game, and not
       shared with another wire.  Without one there is nothing to imitate, and
       guessing the layout would be worse than leaving the wire alone. */
    size_t template_index = wires;
    for (size_t index = 0; index < wires; ++index) {
        if (resolved[index] >= 0x100 && users[resolved[index]] == 1) {
            template_index = index;
            std::memcpy(index_template, descriptors.data() + index * 0x18, sizeof(index_template));
            have_index_template = true;
            break;
        }
    }
    if (template_index == wires && !have_index_template) {
        log("sandbox-sim: no allocated wire to copy a state index from; leaving indices alone");
        return;
    }

    /* The game allocates wire state in two-byte steps (measured: 256/257,
       258/259, 260/261 ...).  Our private block sits far above anything a
       compiled design uses but well inside the state array, and drops back to
       just above the board's own high-water mark if a design ever gets there. */
    uint64_t highest = 0;
    for (uint64_t value : resolved) {
        if (value >= 0x100 && value < 0x9c4000 && value > highest) highest = value;
    }
    uint64_t next = highest >= 0x400000 ? ((highest + 0x1000) & ~1ull) : 0x400000ull;

    for (size_t index = 0; index < wires; ++index) {
        const bool placeholder = resolved[index] < 0x100;
        const bool shared = users[resolved[index]] > 1;
        if (!placeholder && !shared) continue;
        unsigned char candidate[0x18];
        if (template_index < wires) {
            std::memcpy(candidate, descriptors.data() + template_index * 0x18, sizeof(candidate));
        } else {
            std::memcpy(candidate, index_template, sizeof(candidate));
        }
        std::memcpy(candidate, &next, sizeof(next));
        const uint64_t check = game_state_index(candidate, 0);
        if (check != next) {
            log("sandbox-sim: the game refused the crafted index for wire " + std::to_string(index) +
                " (answered " + std::to_string(check) + "); leaving it alone");
            continue;
        }
        std::memcpy(records + 8 + index * 0x68 + 0x38, candidate, sizeof(candidate));
        view.wires[index].slot = next;
        index_patch_base = next;
        if (index_patch_count == 0) {
            log("sandbox-sim: wire " + std::to_string(index) + " had no state index (resolved " +
                std::to_string(resolved[index]) + "); gave it " + std::to_string(next));
        }
        ++index_patch_count;
        next += 2;
    }
}

int64_t hookedPreorder(void* components, void* wires, void* out) {
    if (enabled && sandbox_active && pending_compile_cuts.empty()) {
        pending_compile_cuts =
            sandbox_cycle_unblock::cutTables(components, wires, &prototypeGeometry);
        noteCuts("preorder", pending_compile_cuts);
    }
    const int64_t answer = preorder_original ? preorder_original(components, wires, out) : 0;
    sandbox_cycle_unblock::restore(pending_compile_cuts);
    return answer;
}

int64_t hookedCycleSearch(void* a, void* b, void* c, void* d) {
    std::vector<sandbox_cycle_unblock::Cut> cuts;
    if (enabled && sandbox_active && level_board_model) {
        cuts = sandbox_cycle_unblock::cutBoard(level_board_model, &prototypeGeometry);
        noteCuts("cycle-check", cuts);
    }
    const int64_t answer = cycle_search_original ? cycle_search_original(a, b, c, d) : 0;
    sandbox_cycle_unblock::restore(cuts);
    return answer;
}

void hookedSetCircular(void* context) {
    /* This routine only records and displays the game's circular-dependency
       annotation.  The normalized wire copy above has already supplied an
       acyclic shadow graph to the original compiler.  Suppressing the marker
       in sandbox mode therefore removes the blocking toast without touching
       the real board that simcore evaluates. */
    /* The game calls this the moment it decides the graph it is looking at is
       cyclic - which is also the moment a compile pass ends without ever
       reaching preorder.  Restoring here is what keeps the moved endpoint from
       outliving the pass that needed it (measured: without it the board kept a
       dangling wire for seconds, and the game rewrote that wire into
       per-segment records with no state index). */
    sandbox_cycle_unblock::restore(pending_compile_cuts);
    if (enabled && sandbox_active) return;
    if (set_circular_original) set_circular_original(context);
}

bool createRawHook(const char* symbol, void* detour, void** original) {
    if (!host || !host->resolve_symbol || !host->create_hook) return false;
    void* target = host->resolve_symbol(host->context, symbol);
    return target && host->create_hook(host->context, target, detour, original) == 0;
}

void armCycleUnblock() {
    if (!unblock_enabled) {
        log("sandbox-sim: cycle-unblock disabled by config; the game's passes run untouched");
        return;
    }
    const bool wire = createRawHook(
        "wires__modelZsave95mongerZcommon_u4073", reinterpret_cast<void*>(&hookedWireCopy),
        reinterpret_cast<void**>(&wire_copy_original));
    const bool preorder = createRawHook(
        "preorder__modelZsimulationZpreorder_u31266", reinterpret_cast<void*>(&hookedPreorder),
        reinterpret_cast<void**>(&preorder_original));
    const bool cycle1 = createRawHook(
        "find_circular_path__modelZsimulationZpreorder_u27493",
        reinterpret_cast<void*>(&hookedCycleSearch),
        reinterpret_cast<void**>(&cycle_search_original));
    const bool cycle2 = createRawHook(
        "set_circular_dependency__modelZsimulationZpreorder_u27466",
        reinterpret_cast<void*>(&hookedSetCircular),
        reinterpret_cast<void**>(&set_circular_original));
    log(std::string("sandbox-sim: cycle-unblock hooks wire=") + (wire ? "1" : "0") +
        " preorder=" + (preorder ? "1" : "0") + " cycle=" +
        std::to_string((cycle1 ? 1 : 0) + (cycle2 ? 1 : 0)));
}

/* Binds the runtime to the board the game is showing.  Called again whenever
   the board object changes (a new level, or an edit that reloads it). */
bool bindRuntime(const void* board, const tcsim::BoardView& view) {
    tcsim::RuntimeConfig config;
    config.ticks_per_unit = static_cast<tcsim::Tick>(ticks_per_unit);
    config.cycle_units = static_cast<tcsim::Tick>(cycle_units);
    config.gate_delay = tcsim::ArcDelay{static_cast<tcsim::Tick>(gate_delay), static_cast<tcsim::Tick>(gate_delay)};
    config.trace = trace_events || startup_seed;
    runtime = tcsim::SandboxRuntime();
    if (!runtime.bind(view, &prototypeGeometry, config)) {
        last_error = runtime.notes().empty() ? "bind failed" : runtime.notes().front();
        log("sandbox-sim: cannot run this board: " + last_error);
        runtime_bound = false;
        return false;
    }
    runtime_bound = true;
    bound_board = board;
    bound_components = view.components.size();
    bound_slot_signature = slotSignature(view);
    failed_board = nullptr;
    failed_components = 0;
    failed_slot_signature = 0;
    last_cycle = -1;
    log("sandbox-sim: bound board -> " + runtime.describe());
    for (const tcsim::BuildNote& note : runtime.build().notes) {
        log("sandbox-sim: note: " + note.text);
    }
    logWireBindings("model", board, view, runtime.build());
    logCustomComponents(view);
    return true;
}

/* The game's state array; the same pointer the game's own reader uses. */
unsigned char* stateArray() {
    if (!state_global) return nullptr;
    unsigned char* buffer = *state_global;
    if (!buffer || reinterpret_cast<uintptr_t>(buffer) < 0x10000) return nullptr;
    return buffer;
}

void writeReport() {
    const std::string path = dataPath("sandbox-sim-report.txt");
    if (path.empty()) return;
    std::ostringstream report;
    report << "enabled=" << (enabled ? 1 : 0) << " bound=" << (runtime_bound ? 1 : 0)
           << " sandbox=" << (sandbox_active ? 1 : 0) << " invert=" << (invert_publish ? 1 : 0)
           << " startup_seed=" << (startup_seed ? 1 : 0) << " seeded_nets=" << seeded_nets
           << " steps_per_cycle=" << (steps_per_cycle > 0 ? steps_per_cycle : 1)
           << " patched_wires=" << index_patch_count;
    if (index_patch_base) report << " patch_base=" << index_patch_base;
    report << "\n";
    if (!runtime_bound) {
        report << "error=" << last_error << "\n";
    } else {
        report << runtime.describe() << " published=" << published << " cycles_run=" << cycles_run << "\n";
        for (const tcsim::BuildNote& note : runtime.build().notes) report << "note: " << note.text << "\n";
        unsigned char* buffer = stateArray();
        for (size_t index = 0; index < runtime.netlist().netCount() && index < 16; ++index) {
            const tcsim::NetId net = static_cast<tcsim::NetId>(index);
            const bool source = std::find(runtime.sourceNets().begin(), runtime.sourceNets().end(), net) !=
                                runtime.sourceNets().end();
            report << "net" << index << " value=" << runtime.boardByte(net) << " source="
                   << (source ? 1 : 0) << " slots=";
            for (uint64_t slot : runtime.slotsOf(net)) {
                report << slot << ":";
                if (buffer && slot < state_size) report << static_cast<int>(buffer[slot] & 1u);
                else report << "?";
                report << " ";
            }
            report << "\n";
        }
        report << "trace:\n" << runtime.engine().violations().size() << " violation(s)\n";
        const std::vector<tcsim::TraceRecord>& trace = runtime.engine().trace();
        const size_t first = trace.size() > 96 ? trace.size() - 96 : 0;
        for (size_t index = first; index < trace.size(); ++index) {
            const tcsim::TraceRecord& event = trace[index];
            report << "event t=" << event.when << " net=" << event.net << " value="
                   << event.value.toString() << " initial=" << (event.initial ? 1 : 0) << "\n";
        }
    }
    for (const std::string& line : history) report << line << "\n";
    std::ofstream(path, std::ios::trunc) << report.str();
}

/* Advances the engine in `steps` pieces of `step_ticks` and publishes after
   each one.  One piece per cycle is the old cadence; more pieces are what lets
   a cycle's interior - an 8-tick edge pulse, a gate chain settling - reach the
   board and the scope. */
uint64_t advanceAndPublish(int steps, tcsim::Tick step_ticks, unsigned char* buffer) {
    uint64_t bytes = 0;
    for (int step = 0; step < steps; ++step) {
        runtime.advance(step_ticks, [buffer](tcsim::NetId net, const std::vector<uint64_t>& slots) -> uint64_t {
            return readSourceNet(net, slots, buffer);
        });
        bytes += runtime.publish([buffer](uint64_t slot, unsigned char byte) {
            if (slot < state_size) buffer[slot] = byte;
        }, invert_publish);
        cycles_run = runtime.cycles();
        if (own_cycle) owned_cycle = static_cast<int64_t>(cycles_run);
        std::ostringstream row;
        row << "history cycle=" << cycles_run << " t=" << runtime.now();
        for (size_t index = 0; index < runtime.netlist().netCount() && index < 8; ++index) {
            row << " n" << index << "=" << runtime.boardByte(static_cast<tcsim::NetId>(index));
        }
        history.push_back(row.str());
        /* Keep at least four cycles worth of rows: with sub-cycle publishing a
           cycle is `steps_per_cycle` rows, and a 24-row window silently dropped
           the rows that carried an edge-detector pulse (measured: the pulse sits
           in steps 5..9 of a 32-step cycle, i.e. always outside a 24-row tail). */
        const size_t limit =
            std::max<size_t>(24, static_cast<size_t>(steps > 0 ? steps : 1) * 4 + 8);
        while (history.size() > limit) history.erase(history.begin());
    }
    if (startup_seed) seeded_nets += runtime.seedUnknownDrivenNets();
    return bytes;
}

/* The body returns early on several paths (no board yet, not bound, nothing to
   do); the report has to come out regardless, so it is written by the wrapper
   below rather than by each path. */
void frameBody(const TCFrame*) {
    if (!enabled || !sandbox_active || !have_board || !state_global) return;
    enforceCutDeadline();
    TCGameHandle handle{};
    const void* displayed_board = nullptr;
    if (board_api.get_current(board_api.context, &handle) != TC_SERVICE_OK) return;
    if (board_api.resolve(board_api.context, &handle, &displayed_board) != TC_SERVICE_OK || !displayed_board) return;
    const tcsim::BoardView displayed_view =
        tcsim::readBoard(static_cast<const unsigned char*>(displayed_board));
    const void* board = displayed_board;
    tcsim::BoardView view = displayed_view;
    bool preserve_failed_compile_binding = false;
    if (simulation_model) {
        tcsim::BoardView model_view = tcsim::readBoard(static_cast<const unsigned char*>(simulation_model));
        /* A failed cyclic compile clears the simulation model even though the
           editor is still showing the original board.  Keep the last honest
           binding (and its measured slots) in takeover mode until a real edit
           or level load presents a different non-empty board. */
        if (model_view.components.empty() && runtime_bound && free_run_frames > 0 && own_cycle &&
            bound_components > 0 && displayed_view.components.size() == bound_components) {
            preserve_failed_compile_binding = true;
        } else {
            board = simulation_model;
            view = std::move(model_view);
        }
    }
    /* Own the state index of every wire before the netlist is built from it: a
       wire the game left unallocated would otherwise be published to (and drawn
       from) the shared placeholder byte. */
    const uint64_t check_tick = GetTickCount64();
    if (board != index_checked_board || view.wires.size() != index_checked_wires ||
        check_tick - index_check_tick > 1000) {
        index_checked_board = board;
        index_checked_wires = view.wires.size();
        index_check_tick = check_tick;
        ensureWireStateIndices(board, view);
    }
    /* A different board object, a board whose contents changed (an edit, or the
       game reusing one object across levels), or a cycle that went backwards: all
       three mean the netlist on hand no longer describes what is on screen. */
    const uint64_t signature = slotSignature(view);
    const bool same_failed_board = !runtime_bound && failed_board == board &&
                                   failed_components == view.components.size() &&
                                   failed_slot_signature == signature;
    if (same_failed_board) return;
    if (!preserve_failed_compile_binding &&
        (!runtime_bound || bound_board != board || bound_components != view.components.size() ||
        bound_slot_signature != signature)) {
        if (!bindRuntime(board, view)) {
            failed_board = board;
            failed_components = view.components.size();
            failed_slot_signature = signature;
            writeReport();
            return;
        }
    }
    /* The renderer reads the board that is on screen, not necessarily the model
       the simulation handed us; dump both once so "the colour is wrong" can be
       told apart from "we publish to a different record than the shader reads". */
    static const void* dumped_display_board = nullptr;
    if (trace_events && runtime_bound && displayed_board && displayed_board != dumped_display_board) {
        dumped_display_board = displayed_board;
        log(std::string("sandbox-sim: the rendered board is ") +
            (displayed_board == board ? "the bound board" : "a different object from the bound board"));
        logWireBindings("display", displayed_board, displayed_view, runtime.build());
    }
    unsigned char* buffer = stateArray();
    if (!buffer) return;
    /* The interactive pair's value lives in the instance configuration; refresh
       it every frame so a click reaches the engine on the next cycle. */
    refreshInteractiveLevels();
    if (interactive_override >= 0 && interactive_override_applied == 0) applyInteractiveOverride();

    /* Our own clock first: a board the game refuses to advance has cycle 0 (or
       -1) forever, and that must not stop *us* from running the circuit. */
    if (free_run_frames > 0) {
        if (++free_run_counter >= static_cast<uint64_t>(free_run_frames)) {
            free_run_counter = 0;
            const int steps = steps_per_cycle > 1 ? steps_per_cycle : 1;
            published += advanceAndPublish(steps, runtime.stepTicksFor(steps), buffer);
        }
        return;
    }

    const int64_t cycle = mod.simulation.cycle();
    if (cycle < 0) return;
    if (last_cycle < 0) last_cycle = cycle;
    if (own_cycle && cycle > owned_cycle) owned_cycle = cycle; /* the game got there first */
    /* One game cycle per engine cycle; a frame that skipped several still gets
       one step per cycle so a fast run cannot drop the simulation. */
    while (last_cycle < cycle) {
        const int64_t elapsed = cycle - last_cycle;
        const int64_t cycles_now = elapsed > 64 ? 64 : elapsed;
        const int steps = steps_per_cycle > 1 ? steps_per_cycle : 1;
        const tcsim::Tick step_ticks = runtime.stepTicksFor(steps);
        for (int64_t index = 0; index < cycles_now; ++index) {
            published += advanceAndPublish(steps, step_ticks, buffer);
        }
        last_cycle = cycle;
        if (trace_events) {
            log("sandbox-sim: cycle " + std::to_string(cycle) + " " + runtime.describe());
        }
    }
}

/* The report is what a test (and a player debugging a board) reads, so it is
   refreshed about once a second.  Writing it here rather than inside the body
   keeps a report coming even from the paths that leave early. */
void frame(void*, const TCFrame* value) {
    frameBody(value);
    if (!value) return;
    if (value->time_seconds < report_time + 1.0) return;
    report_time = value->time_seconds;
    writeReport();
}

bool readConfig() {
    const std::string path = dataPath("sandbox-sim.txt");
    if (path.empty()) return false;
    std::ifstream file(path);
    if (!file) return false;
    std::string line;
    while (std::getline(file, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "enabled") enabled = value == "1" || value == "true" || value == "on";
        else if (key == "invert") invert_publish = value == "1" || value == "true";
        else if (key == "trace") trace_events = value == "1" || value == "true";
        else if (key == "ticks_per_unit") ticks_per_unit = std::stoull(value);
        else if (key == "cycle_units") cycle_units = std::stoull(value);
        else if (key == "gate_delay") gate_delay = std::stoull(value);
        else if (key == "free_run") free_run_frames = std::stoi(value);
        else if (key == "steps_per_cycle") steps_per_cycle = std::stoi(value);
        else if (key == "interactive_level") interactive_override = std::stoi(value);
        else if (key == "own_cycle") own_cycle = value == "1" || value == "true";
        else if (key == "startup_seed") startup_seed = value == "1" || value == "true";
        else if (key == "unblock") unblock_enabled = !(value == "0" || value == "false" || value == "off");
    }
    return true;
}

/* One link in the loader's sim.do chain: the first argument is the board the
   simulation is running, which is where the wire records and their state
   offsets live. */
int32_t onSimDo(TCHookCall* call) {
    auto* args = tc::hook::simDoArgs(call);
    enforceCutDeadline();
    if (sandbox_active && args && args->model) simulation_model = args->model;
    if (enabled && sandbox_active && free_run_frames > 0 && own_cycle && args &&
        args->command == 0) {
        /* The cycle-unblock layer lets the stock compiler build an acyclic
           shadow program so it can allocate the state slots used by drawing.
           Do not let that shadow program run: it would continuously overwrite
           simcore's values between our frame callback and the next wire draw,
           making a live ring look frozen.  Stop/refresh/reset still reach the
           game; only run is owned by simcore in takeover mode. */
        call->skip_original = 1;
        static bool reported = false;
        if (!reported) {
            reported = true;
            log("sandbox-sim: stock run suppressed; simcore owns sandbox state updates");
        }
    }
    return 0;
}

int32_t onLevelLoad(TCHookCall* call) {
    auto* args = tc::hook::levelLoadArgs(call);
    const auto* name = args ? static_cast<const tc::TCNimString*>(args->name) : nullptr;
    const char* text = name && name->data ? static_cast<const char*>(name->data) + 8 : nullptr;
    sandbox_active = text && std::string(text).find("sandbox") != std::string::npos;
    sandbox_cycle_unblock::restore(pending_compile_cuts);
    level_board_model = args ? args->board_model : nullptr;
    simulation_model = nullptr;
    runtime = tcsim::SandboxRuntime();
    runtime_bound = false;
    bound_board = nullptr;
    bound_components = 0;
    bound_slot_signature = 0;
    failed_board = nullptr;
    failed_components = 0;
    failed_slot_signature = 0;
    last_cycle = -1;
    owned_cycle = 0;
    free_run_counter = 0;
    published = 0;
    cycles_run = 0;
    seeded_nets = 0;
    index_patch_count = 0;
    index_patch_base = 0;
    index_checked_board = nullptr;
    index_checked_wires = 0;
    index_check_tick = 0;
    pending_cut_tick = 0;
    interactive_levels.clear();
    interactive_logged.clear();
    interactive_override_applied = 0;
    history.clear();
    log(std::string("sandbox-sim: level ") + (text ? text : "(unnamed)") +
        (sandbox_active ? " accepted" : " ignored (not sandbox)"));
    return 0;
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid()) return 2;
    if (!readConfig()) {
        log("sandbox-sim: no sandbox-sim.txt; the simulator stays off");
        return 0; /* installed but doing nothing is a valid state */
    }
    if (!enabled) {
        log("sandbox-sim: disabled; no hooks or callbacks installed");
        return 0;
    }
    if (startup_seed && (free_run_frames <= 0 || !own_cycle)) {
        startup_seed = false;
        log("sandbox-sim: startup_seed requires free_run and own_cycle; seed disabled");
    }
    state_global = static_cast<unsigned char**>(
        h->resolve_symbol(h->context, "simulation_state__modelZsimulator95types_u81"));
    if (!state_global) {
        log("sandbox-sim: no simulation_state global; staying off");
        enabled = false;
    }
    if (h->resolve_symbol) {
        game_state_index = reinterpret_cast<StateIndexFn>(
            h->resolve_symbol(h->context, "get_state_index__modelZsave95mongerZcommon_u5502"));
    }
    /* The interactive switch/button: its level is read from the instance's own
       configuration (see refreshInteractiveLevels). */
    if (!tc::component_instances::table(h, &instance_api)) {
        log("sandbox-sim: no tc.component.instances service; the interactive pair cannot be read");
    }
    if (!tc::component_storage::table(h, &storage_api)) {
        log("sandbox-sim: no tc.component.storage service; the interactive pair cannot be read");
    }
    TCBoardApiV1 queried{};
    if (h->query_service &&
        h->query_service(h->context, TC_SERVICE_BOARD, TC_BOARD_API_VERSION_1, &queried, sizeof(queried)) ==
            TC_SERVICE_OK &&
        queried.size >= sizeof(queried) && queried.get_current && queried.resolve) {
        board_api = queried;
        have_board = true;
    }
    armCycleUnblock();
    log(std::string("sandbox-sim: loaded enabled=") + (enabled ? "1" : "0") +
        " board_service=" + (have_board ? "1" : "0") + " state=" + (state_global ? "1" : "0"));
    if (own_cycle) {
        void* target = h->resolve_alias ? h->resolve_alias(h->context, "sim.cycle") : nullptr;
        if (!target && h->resolve_symbol) {
            target = h->resolve_symbol(h->context, "sim_get_cycle__modelZsimulationZcompile95thread_u3041");
        }
        if (target &&
            h->create_hook(h->context, target, reinterpret_cast<void*>(&hookedCycle),
                           reinterpret_cast<void**>(&cycle_original)) == 0) {
            log("sandbox-sim: the cycle counter is ours (sim.cycle hooked)");
        } else {
            own_cycle = false;
            log("sandbox-sim: could not take the cycle counter; laying off it");
        }
    }
    if (tc::hook::addSimDo(h, 0, &onSimDo, nullptr) != TC_HOOK_OK) {
        log("sandbox-sim: no sim.do link; the board service pointer is the fallback");
    }
    if (tc::hook::addLevelLoad(h, 0, &onLevelLoad, nullptr) != TC_HOOK_OK) {
        enabled = false; /* fail closed: without the level name sandbox-only cannot be guaranteed */
        log("sandbox-sim: no level.load link; disabled to preserve normal levels");
    }
    plugin->on_frame = frame;
    return 0;
}
