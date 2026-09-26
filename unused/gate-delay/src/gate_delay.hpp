#pragma once
/* Gate-level propagation delay for the sandbox (plan: docs/PLAN-gate-delay.md).

   Time runs in **units**; a cycle is K units (K comes from the environment
   until the settings row lands, M3).  Every emitted cycle body is wrapped in a
   unit loop: each unit the host commits what has come due, snapshots the
   simulation state, and every component block re-evaluates against that
   snapshot.  A block's results are *published* with the kind's own delay, so a
   value produced in unit t becomes visible to other components in unit
   t + delay.  That one rule is the whole mode:

     * two cross-coupled NANDs settle instead of being rejected as a circular
       dependency, because no unit ever depends on itself;
     * an odd inverter ring oscillates, one stage per unit;
     * a 32-bit adder is slower than an AND gate (the delay table);
     * a combinational path deeper than K units is cut by the cycle boundary -
       the "clock too fast" failure, visible in the state array.

   What the emitter actually emits (evidence: the native-logic-source-*.txt dumps
   under build/, surveyed by tools/gate-delay-survey.py) is two cycle bodies with
   *different* shapes, and the rewrite has to speak both:

   `mode_refresh` - a state-slot body.  Every block reads its inputs with
   `load(<U8>, #SIMULATION_STATE + N)` and writes its outputs with
   `store(#SIMULATION_STATE + N, U8 (expr))`; cross-component communication goes
   through those slots and the block order is the topological order.

   `mode_run` - the body that actually simulates, and it passes values through
   **local variables**: `var vidN = U8 (U8 vidM)` for every component, i.e. node
   M publishes as `vidM` and its consumers read it directly in the same unit.
   Only timing components touch state there: a register's `LATE` block is
   `if (U1 vid290) == 1 { store(#SIMULATION_STATE + 1369, U32 ((U32 vid1127))) }`,
   and every node's variable number *is* its primary state slot number (node 93
   publishes slots 308/312 through `vid308`, and the refresh body writes exactly
   those offsets).  So the same rule applies once `vidM` reads become snapshot
   reads and each `var vidN` is published to slot N.

   Boundary blocks - the emitter's `LATE` commits, and anything that calls the
   host through the native-logic bridge - must run once per cycle rather than
   once per unit, so they are guarded in place with `if tc_unit == 0 { ... }`.
   Their own state accesses stay direct: a register commit belongs in the real
   state array, and the bridge's mailbox slots (0x9a0000 upwards) are host-owned
   memory that the callback writes during the call.  Everything else runs every
   unit; the reporting statements (short-circuit detection, `input_replay`,
   level outputs) are idempotent, and re-running them is what keeps their local
   variables in scope. */

#include "../sdk/tc_mod_api.h"
#include "gate_pins.generated.hpp"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tc::gate_delay {

inline constexpr uint32_t kDefaultUnitsPerCycle = 8;
inline constexpr size_t kMaxPending = 1u << 18;
/* Native-logic bridge mailbox (`kBridgeStateBase` in native_logic.hpp).  Slots
   from here up belong to a mod's callback, which writes them during the call, so
   they are read and written directly instead of through the snapshot. */
inline constexpr uint64_t kHostStateBase = 0x9a0000;

struct Pending {
    uint64_t due = 0;
    uint64_t offset = 0;
    uint64_t bits = 0;
    uint64_t value = 0;
};

struct Runtime {
    unsigned char** state = nullptr;      /* the game's simulation_state pointer */
    std::vector<unsigned char> scratch;   /* this unit's committed values */
    std::vector<Pending> pending;         /* writes that are not visible yet */
    /* Event scheduling (M1b): a component is evaluated when one of its inputs
       moved.  `dirty` collects what the commits of the current unit changed;
       `dirtyNow` is this unit's snapshot of it, so every consumer of the same
       slot sees the same answer and a consumer's own read does not clear it. */
    std::vector<unsigned char> dirty;
    std::vector<unsigned char> dirtyNow;
    bool eventSchedule = false;
    uint64_t evaluated = 0, skipped = 0;
    size_t stateSize = 0;
    uint64_t time = 0;                    /* monotone unit counter */
    uint64_t unitsPerCycle = kDefaultUnitsPerCycle;
    /* Units per cycle body: 0 is mode_refresh (state slots), 1 is the burst body
       inside mode_run (local variables).  Some front-end paths call both.  Only
       the first site that actually runs may advance the unit clock and publish
       delayed writes; otherwise render refreshes become extra simulation ticks
       and a stable output visibly flashes.  The other site may still perform a
       cycle-boundary pass (mode_run owns LATE blocks and cycle += 1). */
    int activeSite = -1;
    int currentSite = -1;
    bool currentSiteActive = false;
    bool currentCycleStart = false;
    uint64_t lastBoundarySync = UINT64_MAX;
    uint64_t perSite[2] = {0, 0};
    uint64_t suppressedPerSite[2] = {0, 0};
    uint64_t begins = 0, reads = 0, writes = 0, commits = 0, dropped = 0;
    std::function<void(const std::string&)> log;

    unsigned char* base() { return state ? *state : nullptr; }
    void reset() {
        pending.clear();
        time = 0;
        activeSite = currentSite = -1;
        currentSiteActive = currentCycleStart = false;
        lastBoundarySync = UINT64_MAX;
        perSite[0] = perSite[1] = 0;
        suppressedPerSite[0] = suppressedPerSite[1] = 0;
        begins = reads = writes = commits = dropped = 0;
        evaluated = skipped = 0;
        if (!scratch.empty()) std::memset(scratch.data(), 0, scratch.size());
        if (!dirty.empty()) std::memset(dirty.data(), 1, dirty.size());       /* first unit computes everything */
        if (!dirtyNow.empty()) std::memset(dirtyNow.data(), 0, dirtyNow.size());
    }
    void markDirty(uint64_t offset, uint64_t bits) {
        if (offset >= dirty.size()) return;
        const size_t bytes = static_cast<size_t>((bits ? bits : 8) + 7) / 8;
        for (size_t i = 0; i < bytes && offset + i < dirty.size(); ++i) dirty[offset + i] = 1;
    }
};

inline Runtime& store() {
    static Runtime runtime;
    return runtime;
}

/* Delay units per kind.  The game's own cost table says a gate is 1, a delay
   line and RAM are 4, a static indexer is free.  Compiler-made helpers
   (`com_cc_input_buffer`) sit at one unit for now, which costs a unit per
   custom-component layer - the plan's section 4.2 note. */
/* Delay per component kind, in units.

   The game's own cost table calls every gate "1", which is right for the
   primitives a real circuit is built from but wrong for the composite ones: a
   CMOS AND is a NAND followed by an inverter, an OR is a NOR followed by one,
   and an XOR takes about three levels.  This table follows that staging, so the
   difference between a NOT (1) and an AND (2) shows up as a real extra unit of
   propagation - which is what makes gate-chain timing look like hardware.

   These are engineering numbers, not silicon nanoseconds: the model has no
   sub-unit resolution, no per-instance variation and no analog behaviour, so a
   gate's "1 unit" is a quantum, not a measurement.  `TC_GATE_DELAY_TABLE=flat`
   restores the old "everything is one unit" table for A/B comparison. */
inline uint64_t kindDelay(const std::string& kind, unsigned bits = 1) {
    static const int flat = [] {
        wchar_t value[8] = {};
        GetEnvironmentVariableW(L"TC_GATE_DELAY_TABLE", value, 8);
        return (value[0] == L'f' || value[0] == L'F') ? 1 : 0;
    }();
    if (flat) {
        if (kind == "com_delay_line_bit" || kind == "com_delay_line_word" ||
            kind == "com_delay_line_word_config" || kind == "com_ram")
            return 4;
        return 1;
    }
    /* State-holding and memory parts: four units each (the old table's value,
       and the one the game's own cost table reports). */
    if (kind == "com_delay_line_bit" || kind == "com_delay_line_word" ||
        kind == "com_delay_line_word_config" || kind == "com_ram" ||
        kind == "com_register_bit" || kind == "com_register_word" ||
        kind == "com_register_word_config" || kind == "com_counter")
        return 4;
    /* One level of logic: the primitives, and the parts that are only wires as
       far as timing is concerned. */
    if (kind == "com_not_bit" || kind == "com_nand_bit" || kind == "com_nor_bit" ||
        kind == "com_not_word" || kind == "com_nand_word" || kind == "com_nor_word" ||
        kind == "com_and_3_bit" || kind == "com_or_3_bit" ||
        kind == "com_constant" || kind == "com_static_value" ||
        kind == "com_static_indexer" || kind == "com_switch_bit" || kind == "com_switch_word" ||
        kind == "com_splitter_bit_8" || kind == "com_splitter_word_2" ||
        kind == "com_maker_bit_8" || kind == "com_maker_word_2" ||
        kind == "com_cc_input_buffer" || kind == "com_cc_output" || kind == "com_cc_input" ||
        kind == "com_level_input_word" || kind == "com_level_output_word" ||
        kind == "com_level_input_1_pin" || kind == "com_level_output_1_pin" ||
        kind == "com_probe_wire_bit" || kind == "com_probe_wire_word")
        return 1;
    /* Two levels: an inverter in front of a NAND/NOR, and the two-input
       selection networks. */
    if (kind == "com_and_bit" || kind == "com_or_bit" ||
        kind == "com_and_word" || kind == "com_or_word" ||
        kind == "com_mux" || kind == "com_concatenator_2" ||
        kind == "com_concatenator_4" || kind == "com_concatenator_8" ||
        kind == "com_maker_bit_2" || kind == "com_maker_bit_4" ||
        kind == "com_maker_word_4" || kind == "com_maker_word_8")
        return 2;
    /* Three levels: XOR/XNOR are the expensive ones, and a comparator is an
       XOR tree with a reduction. */
    if (kind == "com_xor_bit" || kind == "com_xnor_bit" ||
        kind == "com_xor_word" || kind == "com_xnor_word" ||
        kind == "com_equal" || kind == "com_less_u" || kind == "com_less_s")
        return 3;
    /* Arithmetic: one level per stage the game's own component declares, plus a
       level per four bits of width for the carry chain (log-ish, but a whole
       number of units).  A one-bit operation stays one level. */
    auto wide = [&](uint64_t base) {
        const uint64_t extra = bits <= 4 ? 0 : (bits <= 8 ? 1 : (bits <= 16 ? 2 : 3));
        return base + extra;
    };
    if (kind == "com_neg" || kind == "com_inc") return wide(2);
    if (kind == "com_add" || kind == "com_lsl" || kind == "com_lsr" ||
        kind == "com_rol" || kind == "com_ror" || kind == "com_asr")
        return wide(3);
    if (kind == "com_mul") return wide(6);
    if (kind == "com_div" || kind == "com_mod") return wide(8);
    if (kind == "com_clz" || kind == "com_ctz") return wide(4);
    if (kind == "com_decoder_1" || kind == "com_decoder_2" || kind == "com_decoder_3")
        return wide(2);
    /* Everything else (custom-component scaffolding, level IO, probes, display
       parts): one unit, the old default. */
    return 1;
}

/* ---- the clock source ----------------------------------------------------

   The game has no unit-accurate clock: its sources are a manual button, a
   switch and `Time`, all of which only move at cycle boundaries - which is
   exactly what this mode is about testing.  So the delay model supplies one.

   A registered "Clock" component (examples/clock, a pure source) publishes its
   per-cycle value like any other component.  When the mode is on, *reads* of
   its value slot answer with a pulse instead: high for the first
   `clockWidthUnits()` units of every cycle.  Readers therefore see a real edge
   inside the cycle, the pulse travels through the delay model like any other
   signal (writes carry the usual one-unit delay), and nothing about the
   generated code has to change: the emitter's own block keeps running.

   The slot is learned at compile time from the block the emitter produces for a
   clock instance (`com_custom <bits> <id>`), and the instance ids come from the
   board scan in scanClockInstances() below. */
inline constexpr uint64_t kClockPrototypeId = 0x434C4F4B5F303031ULL;   /* "CLOK_001" */

inline std::map<uint64_t, unsigned>& clockSlots() {
    static std::map<uint64_t, unsigned> value;
    return value;
}

inline std::set<uint64_t>& clockInstances() {
    static std::set<uint64_t> value;
    return value;
}

/* Marked by the native-logic emission (src/native_logic.hpp): the line a clock
   instance produced was replaced by its bridge call, so the slot is learned
   there - the variable name on that line carries it. */
inline void markClockSlot(uint64_t slot, unsigned width) {
    clockSlots()[slot] = width ? width : 1;
}

/* Bridge tokens whose definition is the clock (filled by the native-logic
   binding).  The rewrite finds the clock's value line by token: by then the
   bridge has already replaced that line, and the instance's own `com_custom`
   block is empty. */
inline std::set<uint64_t>& clockTokens() {
    static std::set<uint64_t> value;
    return value;
}

/* How many units the clock is high at the start of every cycle.

   Half the cycle by default, i.e. an ordinary square wave: with one unit per
   gate, a one-unit pulse is *narrower than the path delay of two gates*, so a
   two-NOT chain after the clock shows a one-unit blip that neither the per-cycle
   probe nor the board display can show - the second gate looks dead even though
   it answered (measured: `slot260` was a one-unit high pulse).  Set
   `TC_GATE_DELAY_CLOCK_WIDTH=1` to go back to the narrow pulse, which is the
   right tool when the point *is* pulse-width margin. */
/* Width of the clock's high band, in units, when a setting file pins it.
   Zero means "no opinion": the environment variable, then half a cycle. */
inline unsigned& clockWidthOverride() {
    static unsigned value = 0;
    return value;
}

/* The other shape a clock source can have: instead of a pulse inside every
   cycle, a square wave that flips **once per cycle** (high for a whole cycle,
   low for the next).

   This exists because of what a stepped board can show.  A frame samples one
   unit, and a per-cycle step always samples the same unit, so a signal whose
   period is one cycle reads as a constant - which is exactly the "the clock
   source stopped moving" report.  A two-cycle square wave changes once per
   step, and it is still the *same* value the gates read, because it comes out
   of this one function (see the §32 rule: drawing, wires and logic share the
   signal).  The half-cycle pulse stays the default: it is what the delay model
   is for, and unit stepping is what makes it visible. */
inline bool& clockFlipsPerCycle() {
    static bool value = false;
    return value;
}

inline unsigned clockWidthUnits() {
    static const unsigned width = [] {
        wchar_t value[8] = {};
        const DWORD length = GetEnvironmentVariableW(L"TC_GATE_DELAY_CLOCK_WIDTH", value, 8);
        if (!length) return 0u;
        return static_cast<unsigned>(wcstoul(value, nullptr, 10));
    }();
    if (width) return width;
    /* A setting file can pin the width without an environment variable (the
       Options page cannot show a number field): units=K means "high for one
       whole cycle, low for the next", which is the one shape a per-cycle step
       can show as movement - a pulse that fits inside a cycle is invisible to a
       frame that always samples the same unit. */
    auto& rt = store();
    if (const unsigned pinned = clockWidthOverride()) {
        /* A width of K would mean "always high"; the whole-cycle shape is
           clockFlipsPerCycle() instead, so keep the pulse inside the cycle. */
        const unsigned limit = static_cast<unsigned>(rt.unitsPerCycle ? rt.unitsPerCycle - 1 : 7);
        return pinned < limit ? pinned : limit;
    }
    return static_cast<unsigned>(rt.unitsPerCycle ? rt.unitsPerCycle / 2 : 4);
}

/* Units since the current cycle began.  `time` is the monotone unit counter. */
inline uint64_t unitInCycle() {
    auto& rt = store();
    return rt.unitsPerCycle ? rt.time % rt.unitsPerCycle : 0;
}

inline uint64_t clockPulse() {
    if (clockFlipsPerCycle()) {
        auto& rt = store();
        const uint64_t k = rt.unitsPerCycle ? rt.unitsPerCycle : kDefaultUnitsPerCycle;
        return (rt.time / k) % 2 ? 0 : 1;
    }
    return unitInCycle() < clockWidthUnits() ? 1 : 0;
}

/* ---- symmetric ties inside a loop ---------------------------------------

   Two identical gates cross-coupled (the NAND latch) are a perfectly symmetric
   system: every unit is computed from one snapshot and committed together, so
   with both inputs high the pair flips in lockstep for ever
   (1,1 -> 0,0 -> 1,1 ...) and the player reads a latch that never picks a
   state.  Real hardware breaks that through physical asymmetry; a deterministic
   model has to choose a rule.

   The rule here is the cheapest one that never touches a normal circuit: when
   *every* member of a feedback loop publishes the same new value in the same
   unit, the board-order-first member keeps its old value for that unit.  Only
   the unit a group moves into lockstep is affected - a settled latch, an
   asymmetric loop and an odd-length ring never fire it. */
struct TieGroup {
    std::vector<uint64_t> slots;    /* board order; [0] is the one that holds */
};

inline std::vector<TieGroup>& tieGroups() {
    static std::vector<TieGroup> value;
    return value;
}

inline std::vector<std::vector<int64_t>>& tieGroupComponents() {
    static std::vector<std::vector<int64_t>> value;
    return value;
}

/* How often the rule fired: one per symmetric transition, so a latch that
   settles should show a couple and then stop. */
inline uint64_t& tieBreaks() {
    static uint64_t value = 0;
    return value;
}

/* ---- unit stepping ------------------------------------------------------

   The board is drawn once per program pass, so with K units per pass a player
   only ever sees the end of the cycle - which is what made every "the second NOT
   does not respond" report happen.  With stepping on, one pass advances exactly
   one unit: the board updates once per unit, so a cycle takes K passes and you
   watch the propagation instead of guessing at it.  `time` stays monotone
   underneath, so the unit index inside the cycle (and with it the clock's
   pulse) keeps its meaning.

   Turned on with `on=1` in tc-modloader-data/gate-delay-step.txt, or the
   TC_GATE_DELAY_STEP=1 environment variable (playtests). */
inline uint64_t& unitsPerPassOverride() {
    static uint64_t value = 0;
    return value;
}

inline void setUnitsPerPass(uint64_t units) { unitsPerPassOverride() = units; }

/* Unit stepping pace, in milliseconds per unit.

   Stepping a unit per *frame* is what made "single-step the board" report as
   "the clock flashes" (measured 15.6 ms between units, so a cycle's four-unit
   high band lasts 63 ms - far too fast to read as a waveform, and a frame that
   lands in the low band looks like a flicker).  The pace holds the unit clock
   back: a pass that comes due early advances *nothing*, which leaves the board
   showing the previous unit, so a stepped cycle plays as slow motion instead.
   `ms=0` restores the frame-driven rate; the loader reads both from
   tc-modloader-data/gate-delay-step.txt and playtests set
   TC_GATE_DELAY_STEP_MS. */
inline uint64_t& unitPaceMs() {
    static uint64_t value = 0;
    return value;
}

inline void setUnitPaceMs(uint64_t ms) { unitPaceMs() = ms; }

inline uint64_t& lastUnitTick() {
    static uint64_t value = 0;
    return value;
}

inline bool ownsUnitClock(uint64_t site) {
    auto& rt = store();
    if (site >= 2) return false;
    if (rt.activeSite < 0) rt.activeSite = static_cast<int>(site);
    return rt.activeSite == static_cast<int>(site);
}

inline uint64_t unitsThisPass(uint64_t site) {
    /* A non-owner must not evaluate its combinational locals.  Even though
       write() suppresses their publication, the game can use those locals to
       paint the board, alternating the display between the committed state and
       a second, immediate evaluation.  mode_run receives one read-only pass
       only after the owner has reached a new cycle boundary, so its LATE blocks
       and cycle counter can still advance exactly once. */
    if (!ownsUnitClock(site)) {
        auto& rt = store();
        return site == 1 && rt.activeSite == 0 && rt.time != 0 &&
                       unitInCycle() == 0 && rt.lastBoundarySync != rt.time
                   ? 1u
                   : 0u;
    }
    if (unitsPerPassOverride()) {
        const uint64_t pace = unitPaceMs();
        if (pace) {
            const uint64_t now = GetTickCount64();
            if (now - lastUnitTick() < pace) return 0;      /* too soon: idle pass */
            lastUnitTick() = now;
        }
        return 1;
    }
    static const uint64_t step = [] {
        wchar_t value[8] = {};
        return (GetEnvironmentVariableW(L"TC_GATE_DELAY_STEP", value, 8) && value[0] == L'1')
                   ? 1ull : 0ull;
    }();
    if (step) return 1;
    auto& rt = store();
    return rt.unitsPerCycle ? rt.unitsPerCycle : kDefaultUnitsPerCycle;
}

/* True on the first unit of a cycle.  The emitter's once-per-cycle blocks (its
   LATE commit, the native-logic bridge calls that drive custom components, the
   scope tick) are guarded with this instead of "the last unit of the pass": with
   unit stepping a pass is one unit, so a pass-relative guard runs them once per
   *unit* - which turned the clock's callback into a per-unit flip and made every
   board flash. */
inline uint64_t cycleStart() {
    return store().currentCycleStart ? 1 : 0;
}

/* ---- host functions the rewritten program calls ------------------------- */

inline void writeBits(unsigned char* destination, uint64_t offset, uint64_t bits, uint64_t value) {
    if (!destination || !bits) return;
    const size_t bytes = static_cast<size_t>((bits + 7) / 8);
    for (size_t i = 0; i < bytes; ++i) destination[offset + i] = static_cast<unsigned char>(value >> (8 * i));
}

/* The clock pulse used by delayed logic must also be the value the board draws.
   Originally only tc_delay_read() synthesized the pulse, while the simulation
   buffer (and therefore the wire shader) kept the clock callback's per-cycle
   value.  A player could then see a clock wire holding steady while its reader
   reacted to a different, hidden signal.  Publish the pulse into every state
   slot emitted for the clock output before taking this unit's snapshot. */
inline void publishClockPulse() {
    auto& rt = store();
    unsigned char* state = rt.base();
    if (!state || clockSlots().empty()) return;
    const uint64_t pulse = clockPulse();
    for (const auto& clock : clockSlots()) {
        const uint64_t offset = clock.first;
        const uint64_t bits = clock.second ? clock.second : 1;
        const size_t bytes = static_cast<size_t>((bits + 7) / 8);
        bool changed = false;
        for (size_t i = 0; i < bytes && offset + i < rt.stateSize; ++i) {
            if (state[offset + i] != static_cast<unsigned char>(pulse >> (8 * i))) {
                changed = true;
                break;
            }
        }
        /* Bounded by the same check the change test uses: a slot whose width
           runs past the buffer must not make this write step outside it. */
        for (size_t i = 0; i < bytes && offset + i < rt.stateSize; ++i)
            state[offset + i] = static_cast<unsigned char>(pulse >> (8 * i));
        if (changed) rt.markDirty(offset, bits);
    }
}

/* ---- wire copies: a wire is not a device ---------------------------------

   The emitter turns a wire into its own node (`com_cc_input_buffer`, and the
   same shape wherever a wire has to be mirrored for a consumer), and the delay
   table gave every node one unit.  A wire has no logic in it, so that unit was
   wrong in a way the player can see: the board draws the *copy* slot, the gate
   pin next to it is the *source*, and the component panel reads the pin - so a
   wire could sit on the previous unit's value while the pin it comes from had
   already moved, and stayed behind for as long as the circuit kept changing.

   A copy node is therefore a mirror: at every unit boundary the host copies its
   source slot's committed bytes into the copy slot, so drawing, the gates that
   read it and the panel that shows the pin all carry the same value in the same
   unit.  Only a block whose whole body is one slot read is treated this way -
   anything that computes (a gate, a splitter's bit field, a constant) keeps the
   delay its kind asks for. */
struct Mirror {
    uint64_t destination = 0;
    uint64_t source = 0;
    unsigned bits = 1;
};

inline std::vector<Mirror>& mirrors() {
    static std::vector<Mirror> value;
    return value;
}

/* Called by the rewritten program, once per pass.  Idempotent: the list is
   rebuilt at every compile, and a program may name the same copy twice. */
inline void registerMirror(uint64_t destination, uint64_t source, uint64_t bits) {
    if (!bits || destination == source) return;
    for (const Mirror& mirror : mirrors())
        if (mirror.destination == destination) return;
    mirrors().push_back(Mirror{destination, source, static_cast<unsigned>(bits)});
}

inline void applyMirrors() {
    auto& rt = store();
    unsigned char* state = rt.base();
    if (!state || mirrors().empty()) return;
    /* Two passes: a wire may copy another wire, and the emitter lists them in
       board order, so one pass could still show the older end of a chain. */
    for (int pass = 0; pass < 2; ++pass) {
        for (const Mirror& mirror : mirrors()) {
            const size_t bytes = static_cast<size_t>((mirror.bits ? mirror.bits : 1) + 7) / 8;
            if (mirror.source + bytes > rt.stateSize) continue;
            if (mirror.destination + bytes > rt.stateSize) continue;
            bool changed = false;
            for (size_t i = 0; i < bytes; ++i) {
                const unsigned char byte = state[mirror.source + i];
                if (state[mirror.destination + i] != byte) changed = true;
                state[mirror.destination + i] = byte;
            }
            if (changed) rt.markDirty(mirror.destination, mirror.bits);
        }
    }
}

inline void commitPending() {
    auto& rt = store();
    unsigned char* state = rt.base();
    if (!state) return;
    /* Tie-break pass (see "symmetric ties inside a loop" above): look at the
       entries coming due now and drop the board-first member's commit when the
       whole group is moving into lockstep. */
    std::vector<char> hold(rt.pending.size(), 0);
    if (!tieGroups().empty()) {
        for (const TieGroup& group : tieGroups()) {
            if (group.slots.size() < 2) continue;
            size_t found[64];
            uint64_t firstValue = 0;
            bool allChanged = true, allEqual = true, decided = false;
            if (group.slots.size() > 64) continue;
            for (size_t k = 0; k < group.slots.size() && allChanged && allEqual; ++k) {
                found[k] = rt.pending.size();
                for (size_t i = 0; i < rt.pending.size(); ++i)
                    if (rt.pending[i].offset == group.slots[k] && rt.pending[i].due <= rt.time) {
                        found[k] = i;
                        break;
                    }
                if (found[k] == rt.pending.size()) { allChanged = false; break; }
                const Pending& entry = rt.pending[found[k]];
                const size_t bytes = static_cast<size_t>((entry.bits ? entry.bits : 8) + 7) / 8;
                uint64_t value = 0;
                bool changed = false;
                for (size_t i = 0; i < bytes && i < 8 && entry.offset + i < rt.scratch.size(); ++i) {
                    const unsigned char byte = static_cast<unsigned char>(entry.value >> (8 * i));
                    value |= static_cast<uint64_t>(byte) << (8 * i);
                    if (state[entry.offset + i] != byte) changed = true;
                }
                if (!changed) allChanged = false;
                if (!decided) { firstValue = value; decided = true; }
                else if (value != firstValue) allEqual = false;
            }
            if (!allChanged || !allEqual) continue;
            hold[found[0]] = 1;
            ++tieBreaks();
        }
    }
    size_t kept = 0;
    for (size_t i = 0; i < rt.pending.size(); ++i) {
        const Pending& entry = rt.pending[i];
        if (entry.due <= rt.time) {
            /* The group's board-first member keeps its old value for this unit;
               its block runs again next unit and the tie is gone by then. */
            if (hold[i]) continue;
            /* Mark the slot dirty only when the value really moved: otherwise a
               settled board would re-evaluate itself for ever. */
            const size_t bytes = static_cast<size_t>((entry.bits ? entry.bits : 8) + 7) / 8;
            bool changed = false;
            for (size_t i = 0; i < bytes && entry.offset + i < rt.scratch.size(); ++i)
                if (state[entry.offset + i] != static_cast<unsigned char>(entry.value >> (8 * i))) {
                    changed = true;
                    break;
                }
            writeBits(state, entry.offset, entry.bits, entry.value);
            if (changed) rt.markDirty(entry.offset, entry.bits);
            ++rt.commits;
        } else {
            rt.pending[kept++] = entry;
        }
    }
    rt.pending.resize(kept);
}

/* The unit boundary: commit everything that has come due, then snapshot the
   state, so every read of this unit sees the same previous values. */
inline void traceUnit(uint64_t unit);
inline void traceEveryUnit(uint64_t unit);

inline void begin(uint64_t site) {
    auto& rt = store();
    rt.currentSite = site < 2 ? static_cast<int>(site) : -1;
    rt.currentSiteActive = ownsUnitClock(site);
    rt.currentCycleStart = false;
    if (!rt.currentSiteActive) {
        if (site < 2) ++rt.suppressedPerSite[site];
        /* When refresh owns propagation, mode_run is the boundary synchronizer:
           let exactly one invocation at this unit see cycleStart=true.  Every
           guarded statement in that invocation sees the same answer. */
        if (site == 1 && rt.activeSite == 0 && rt.time != 0 && unitInCycle() == 0 &&
            rt.lastBoundarySync != rt.time) {
            rt.currentCycleStart = true;
            rt.lastBoundarySync = rt.time;
        }
        return;
    }
    ++rt.begins;
    if (site < 2) ++rt.perSite[site];
    /* A line at each of the first three cycle boundaries, then sparsely: enough
       to prove the mode is live on a real run (units advance, values are
       published, committed and read) without writing a line per cycle. */
    if (rt.log && (rt.begins <= 3 * rt.unitsPerCycle
                       ? rt.begins % rt.unitsPerCycle == 0
                       : rt.begins % 4096 == 0))
    {
        /* The board's own slots, as committed and as this unit's snapshot sees
           them: what tells "the publish landed" from "the publish landed but the
           read looked elsewhere".  256 upwards is where the emitter puts a small
           board's nodes. */
        std::string committed, snapshot;
        unsigned char* state = rt.base();
        /* The window has to cover every low node the emitter hand out - a board
           with custom components pushes its nodes up past 287 (a `com_custom`
           instance's own value, the group's "any field" slot), and a window that
           stops at 287 hides exactly the node a "the wire never changes" report
           is about. */
        for (uint64_t at = 256; at < 304; ++at) {
            committed += std::to_string(state ? static_cast<int>(state[at]) : -1) + (at == 303 ? "" : ",");
            snapshot += std::to_string(at < rt.scratch.size() ? static_cast<int>(rt.scratch[at]) : -2) +
                        (at == 303 ? "" : ",");
        }
        rt.log("Gate delay: units=" + std::to_string(rt.begins) + " writes=" + std::to_string(rt.writes) +
               " commits=" + std::to_string(rt.commits) + " reads=" + std::to_string(rt.reads) +
               " pending=" + std::to_string(rt.pending.size()) + " (refresh=" +
               std::to_string(rt.perSite[0]) + " burst=" + std::to_string(rt.perSite[1]) + ")" +
               " ignored=(refresh=" + std::to_string(rt.suppressedPerSite[0]) +
               " burst=" + std::to_string(rt.suppressedPerSite[1]) + ")" +
               " state[256..287]=" + committed + " snapshot=" + snapshot);
    }
    ++rt.time;
    commitPending();
    applyMirrors();
    publishClockPulse();
    unsigned char* state = rt.base();
    if (state && !rt.scratch.empty()) std::memcpy(rt.scratch.data(), state, rt.scratch.size());
    /* This unit's view of "what moved": the commits above, taken once so every
       consumer of a slot agrees and a read does not consume the flag. */
    if (!rt.dirtyNow.empty() && rt.dirtyNow.size() == rt.dirty.size())
        std::memcpy(rt.dirtyNow.data(), rt.dirty.data(), rt.dirty.size());
    if (!rt.dirty.empty()) std::memset(rt.dirty.data(), 0, rt.dirty.size());
    rt.currentCycleStart = unitInCycle() == 0;
    traceUnit(rt.begins);
    traceEveryUnit(rt.begins);
}

/* Event scheduling: a component asks whether one of its inputs moved since the
   last unit it looked.  With `TC_GATE_DELAY_SCHED=event` the generated guards use
   this instead of running every block every unit, which is also what turns a
   component's several input arrivals into several evaluations (a real glitch). */
inline uint64_t isDirty(uint64_t offset) {
    auto& rt = store();
    if (!rt.eventSchedule) return 1;
    if (offset >= rt.dirtyNow.size()) return 1;
    return rt.dirtyNow[offset] ? 1u : 0u;
}

/* Reads a committed value.  Offsets past the snapshot (the bridge mailbox) fall
   back to the real state, which is where a callback writes its results. */
inline uint64_t read(uint64_t offset, uint64_t bits) {
    auto& rt = store();
    ++rt.reads;
    /* A clock slot answers with this unit's pulse rather than with the per-cycle
       value the component published (see the clock block above).  Marking it
       dirty keeps the event schedule re-evaluating its readers every unit: the
       pulse does change per unit, even though the published value does not. */
    if (!clockSlots().empty()) {
        const auto clock = clockSlots().find(offset);
        if (clock != clockSlots().end()) {
            rt.markDirty(offset, bits);
            return clockPulse();
        }
    }
    const size_t bytes = static_cast<size_t>((bits ? bits : 8) + 7) / 8;
    const unsigned char* source = nullptr;
    if (offset + bytes <= rt.scratch.size()) source = rt.scratch.data() + offset;
    else if (unsigned char* state = rt.base()) source = state + offset;
    if (!source) return 0;
    uint64_t value = 0;
    for (size_t i = 0; i < bytes && i < 8; ++i) value |= static_cast<uint64_t>(source[i]) << (8 * i);
    return value;
}

inline void write(uint64_t offset, uint64_t bits, uint64_t value, uint64_t delay) {
    auto& rt = store();
    /* The non-owner body is allowed to build its locals for a LATE boundary
       commit, but it must never publish a second, differently phased copy of
       the combinational wave. */
    if (rt.currentSite >= 0 && !rt.currentSiteActive) return;
    ++rt.writes;
    Pending entry;
    entry.offset = offset;
    entry.bits = bits ? bits : 8;
    entry.value = value;
    /* The snapshot is taken before a block runs, so due = time + 1 is the
       earliest moment another component can see this value. */
    entry.due = rt.time + (delay ? delay : 1);
    if (rt.pending.size() >= kMaxPending) {          /* never grow without bound */
        commitPending();
        ++rt.dropped;
        entry.due = rt.time;
    }
    rt.pending.push_back(entry);
}

inline void reset() { store().reset(); }

/* ---- the source transform ------------------------------------------------ */

struct Block {
    size_t header = 0;             /* index of the `// <node> <kind> ...` line */
    size_t end = 0;                /* index one past the block's lines */
    uint64_t node = 0;
    unsigned bits = 0;
    std::string kind;
    bool late = false;             /* the emitter's cycle-boundary commit */
    bool host = false;             /* calls into the native-logic bridge */
};

inline std::string trimmed(const std::string& line) {
    size_t begin = line.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return std::string();
    size_t end = line.find_last_not_of(" \t\r");
    return line.substr(begin, end - begin + 1);
}

inline std::string indentOf(const std::string& line) {
    size_t end = line.find_first_not_of(" \t");
    if (end == std::string::npos) return std::string();
    return line.substr(0, end);
}

inline bool readDigits(uint64_t& out, const std::string& text, size_t& pos) {
    const size_t start = pos;
    uint64_t value = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        value = value * 10 + static_cast<uint64_t>(text[pos] - '0');
        ++pos;
    }
    if (pos == start) return false;
    out = value;
    return true;
}

inline bool isIdentifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/* True when a line or block calls into the native-logic bridge or another host
   entry point that has to happen once per cycle.  The mode's own `tc_delay_*`
   calls are not that: they run every unit by design, and a re-closed read puts
   one inside a component block. */
inline bool hasHostCall(const std::string& text) {
    static const std::string marker = "game_engine.'";
    static const std::string delay = "tc_delay_";
    size_t pos = 0;
    while ((pos = text.find(marker, pos)) != std::string::npos) {
        const size_t name = pos + marker.size();
        if (text.compare(name, delay.size(), delay) != 0) return true;
        const size_t close = text.find('\'', name);
        pos = close == std::string::npos ? name + 1 : close + 1;
    }
    return false;
}

/* `// <node> com_<kind> <bits> [LATE] [id] [label]`: every field the transform
   needs, including the component's own 64-bit id (the number after `LATE` when
   that marker is present). */
inline bool parseHeaderFull(const std::string& line, Block& block, bool* hasId, int64_t* id) {
    const std::string text = trimmed(line);
    if (text.rfind("//", 0) != 0) return false;
    size_t pos = 2;
    while (pos < text.size() && text[pos] == ' ') ++pos;
    uint64_t node = 0;
    if (!readDigits(node, text, pos)) return false;
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (text.compare(pos, 4, "com_") != 0) return false;
    size_t kindEnd = pos;
    while (kindEnd < text.size() && isIdentifierChar(text[kindEnd])) ++kindEnd;
    const std::string kind = text.substr(pos, kindEnd - pos);
    pos = kindEnd;
    while (pos < text.size() && text[pos] == ' ') ++pos;
    uint64_t bits = 0;
    if (!readDigits(bits, text, pos)) return false;
    block.node = node;
    block.bits = static_cast<unsigned>(bits);
    block.kind = kind;
    block.late = false;
    if (hasId) *hasId = false;
    if (id) *id = 0;
    while (pos < text.size()) {
        while (pos < text.size() && text[pos] == ' ') ++pos;
        if (pos >= text.size()) break;
        if (text.compare(pos, 4, "LATE") == 0) {
            block.late = true;
            pos += 4;
            continue;
        }
        const size_t start = pos;
        const bool negative = text[pos] == '-';
        if (negative) ++pos;
        uint64_t value = 0;
        if (readDigits(value, text, pos)) {
            if (hasId) *hasId = true;
            if (id) *id = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
            continue;
        }
        pos = start;                     /* a label: nothing more to read */
        break;
    }
    return true;
}

inline bool parseHeader(const std::string& line, Block& block) {
    return parseHeaderFull(line, block, nullptr, nullptr);
}

inline std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) { lines.push_back(text.substr(start)); break; }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

/* Slot widths for the whole program.  The refresh body always writes typed
   stores, which is what tells us how wide each node's slots are; the run body's
   untyped output caches lean on that. */
struct Slots {
    std::map<uint64_t, unsigned> bits;
    uint64_t maxOffset = 0;
    void note(uint64_t offset, unsigned width) {
        if (!width) return;
        auto found = bits.find(offset);
        if (found == bits.end() || found->second < width) bits[offset] = width;
        maxOffset = std::max(maxOffset, offset + width / 8 + 1);
    }
    unsigned widthOf(uint64_t offset, unsigned fallback) const {
        auto found = bits.find(offset);
        return found == bits.end() ? fallback : found->second;
    }
};

/* `load(<U<bits>, #SIMULATION_STATE + offset)` starting at `pos`. */
inline bool matchLoad(const std::string& text, size_t pos, unsigned& bits, uint64_t& offset, size_t& end) {
    static const std::string head = "load(<U";
    if (text.compare(pos, head.size(), head) != 0) return false;
    size_t cursor = pos + head.size();
    uint64_t width = 0;
    if (!readDigits(width, text, cursor)) return false;
    static const std::string middle = ">, #SIMULATION_STATE + ";
    if (text.compare(cursor, middle.size(), middle) != 0) return false;
    cursor += middle.size();
    uint64_t slot = 0;
    if (!readDigits(slot, text, cursor)) return false;
    if (cursor >= text.size() || text[cursor] != ')') return false;
    bits = static_cast<unsigned>(width);
    offset = slot;
    end = cursor + 1;
    return true;
}

/* `store(#SIMULATION_STATE + offset, <value>)` starting at `pos`, with the
   value's extent and whatever follows the closing parenthesis. */
inline bool matchStore(const std::string& text, size_t pos, uint64_t& offset, size_t& valueStart,
                       size_t& valueEnd, std::string& suffix) {
    static const std::string head = "store(#SIMULATION_STATE + ";
    if (text.compare(pos, head.size(), head) != 0) return false;
    size_t cursor = pos + head.size();
    uint64_t slot = 0;
    if (!readDigits(slot, text, cursor)) return false;
    while (cursor < text.size() && text[cursor] == ' ') ++cursor;
    if (cursor >= text.size() || text[cursor] != ',') return false;
    ++cursor;
    while (cursor < text.size() && text[cursor] == ' ') ++cursor;
    valueStart = cursor;
    int depth = 0;
    size_t scan = pos + 5;                          /* at the store's own parenthesis */
    while (scan < text.size()) {
        if (text[scan] == '(') ++depth;
        else if (text[scan] == ')') { --depth; if (depth == 0) break; }
        ++scan;
    }
    if (depth != 0 || scan >= text.size()) return false;
    valueEnd = scan;
    suffix = text.substr(scan + 1);
    offset = slot;
    return true;
}

/* The declared type of a `var x = U8 (...)` line, as a bit width. */
inline unsigned declaredWidth(const std::string& line) {
    const std::string text = trimmed(line);
    if (text.rfind("var ", 0) != 0 && text.rfind("let ", 0) != 0) return 0;
    const size_t eq = text.find('=');
    if (eq == std::string::npos) return 0;
    size_t pos = eq + 1;
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size() || text[pos] != 'U') return 0;
    size_t cursor = pos + 1;
    uint64_t width = 0;
    if (!readDigits(width, text, cursor)) return 0;
    return static_cast<unsigned>(width);
}

/* `let value = U32 (...)` / `var x = U1 ...`: the local's name and its width.
   An untyped `store(ptr + N, value)` needs this to know how many bytes the
   emitter meant to write. */
inline bool localDecl(const std::string& line, std::string& name, unsigned& width) {
    const std::string text = trimmed(line);
    size_t start = 0;
    if (text.rfind("let ", 0) == 0) start = 4;
    else if (text.rfind("var ", 0) == 0) start = 4;
    else return false;
    const size_t eq = text.find('=');
    if (eq == std::string::npos || eq <= start) return false;
    size_t end = start;
    while (end < eq && isIdentifierChar(text[end])) ++end;
    if (end == start) return false;
    name = text.substr(start, end - start);
    width = declaredWidth(text);
    return width != 0;
}

/* `var vidN = ...`: the node's primary state slot is the number in the name.
   Only `var vid` names are node values; a `let value_idN` is the emitter's own
   local and its state slot is written by the block's `store` instead. */
inline bool nodeVarName(const std::string& line, std::string& name, unsigned& bits, uint64_t& slot) {
    const std::string text = trimmed(line);
    static const std::string keyword = "var ";
    if (text.rfind(keyword, 0) != 0) return false;
    const size_t after = keyword.size();
    const size_t eq = text.find('=');
    if (eq == std::string::npos || eq <= after) return false;
    size_t end = after;
    while (end < eq && isIdentifierChar(text[end])) ++end;
    if (end == after) return false;
    name = text.substr(after, end - after);
    if (name.rfind("vid", 0) != 0) return false;
    const size_t cut = name.find_first_of("0123456789");
    if (cut == std::string::npos) return false;
    slot = std::strtoull(name.c_str() + cut, nullptr, 10);
    bits = declaredWidth(text);
    return true;
}

/* Turns `load(<Ub>, #SIMULATION_STATE + N)` into a snapshot read and
   `store(#SIMULATION_STATE + N, V)` into a delayed publish. */
inline std::string rewriteState(const std::string& line, const Slots& slots,
                                const std::map<std::string, unsigned>& locals, uint64_t delay,
                                Runtime& stats, std::vector<std::string>& problems) {
    std::string text = line;
    size_t pos = 0;
    while ((pos = text.find("load(<U", pos)) != std::string::npos) {
        unsigned bits = 0;
        uint64_t offset = 0;
        size_t end = 0;
        if (!matchLoad(text, pos, bits, offset, end)) { ++pos; continue; }
        if (offset >= kHostStateBase) { pos = end; continue; }
        ++stats.reads;
        const std::string replacement = "((U" + std::to_string(bits) + " game_engine.'tc_delay_read'(U64 " +
                                        std::to_string(offset) + ", U64 " + std::to_string(bits) + ")))";
        text.replace(pos, end - pos, replacement);
        pos += replacement.size();
    }
    pos = 0;
    while ((pos = text.find("store(#SIMULATION_STATE + ", pos)) != std::string::npos) {
        uint64_t offset = 0;
        size_t valueStart = 0, valueEnd = 0;
        std::string suffix;
        if (!matchStore(text, pos, offset, valueStart, valueEnd, suffix)) { ++pos; continue; }
        if (offset >= kHostStateBase) { pos = valueEnd + 1; continue; }
        const std::string value = text.substr(valueStart, valueEnd - valueStart);
        unsigned bits = 0;
        const std::string valueText = trimmed(value);
        if (valueText.rfind("U", 0) == 0) {          /* keep the emitter's own type tag */
            size_t cursor = 1;
            uint64_t width = 0;
            bool tagged = readDigits(width, valueText, cursor);
            /* The emitter writes `U8 (expr)` and `U1 0` - both are typed. */
            if (tagged && (cursor >= valueText.size() || !isIdentifierChar(valueText[cursor])))
                bits = static_cast<unsigned>(width);
        }
        if (!bits) bits = slots.widthOf(offset, 0);
        if (!bits) {                                 /* `store(ptr + N, value)` */
            auto found = locals.find(trimmed(value));
            if (found != locals.end()) bits = found->second;
        }
        if (!bits) {
            problems.push_back("no width for slot " + std::to_string(offset) + ": " + trimmed(line));
            pos = valueEnd + 1;
            continue;
        }
        ++stats.writes;
        const std::string replacement = "game_engine.'tc_delay_write'(U64 " + std::to_string(offset) +
                                        ", U64 " + std::to_string(bits) + ", U64 (" + value + "), U64 " +
                                        std::to_string(delay) + ")";
        text.replace(pos, valueEnd + 1 - pos, replacement);
        pos += replacement.size();
    }
    return text;
}

struct VarRef {
    uint64_t slot = 0;
    unsigned bits = 0;
};

/* Which slots a block's own lines read: the `load`s of the state array, plus the
   node variables earlier blocks published.  In the event-scheduled pass this is
   the block's guard - "did any of my inputs move?". */
inline void collectInputSlots(const std::string& line, const std::map<std::string, VarRef>& known,
                              std::vector<uint64_t>& out) {
    size_t at = 0;
    while ((at = line.find("load(<U", at)) != std::string::npos) {
        unsigned bits = 0;
        uint64_t offset = 0;
        size_t end = 0;
        if (!matchLoad(line, at, bits, offset, end)) { ++at; continue; }
        if (offset < kHostStateBase &&
            std::find(out.begin(), out.end(), offset) == out.end())
            out.push_back(offset);
        at = end;
    }
    size_t pos = 0;
    while (pos < line.size()) {
        if (isIdentifierChar(line[pos])) {
            size_t end = pos;
            while (end < line.size() && isIdentifierChar(line[end])) ++end;
            auto found = known.find(line.substr(pos, end - pos));
            if (found != known.end() &&
                std::find(out.begin(), out.end(), found->second.slot) == out.end())
                out.push_back(found->second.slot);
            pos = end;
            continue;
        }
        ++pos;
    }
}

/* Replaces references to *earlier* blocks' node variables with a snapshot read.
   That is what makes the wave front advance one component delay at a time
   instead of collapsing into a single unit. */
inline std::string rewriteRefs(const std::string& line, const std::map<std::string, VarRef>& known,
                               const std::vector<std::string>& keepLocal = {}) {
    std::string out;
    out.reserve(line.size() + 32);
    size_t pos = 0;
    while (pos < line.size()) {
        if (std::isalpha(static_cast<unsigned char>(line[pos])) || line[pos] == '_') {
            size_t end = pos;
            while (end < line.size() && isIdentifierChar(line[end])) ++end;
            const std::string name = line.substr(pos, end - pos);
            auto found = known.find(name);
            if (found == known.end() ||
                std::find(keepLocal.begin(), keepLocal.end(), name) != keepLocal.end()) out += name;
            else out += "((U" + std::to_string(found->second.bits) + " game_engine.'tc_delay_read'(U64 " +
                        std::to_string(found->second.slot) + ", U64 " + std::to_string(found->second.bits) +
                        ")))";
            pos = end;
            continue;
        }
        out += line[pos++];
    }
    return out;
}

/* Multi-driver blocks use an earlier component's `vidN` as a local accumulator
   (`vidN |= value`) while publishing each driver's value through a store.  That
   accumulator is an lvalue, not a cross-component read: replacing it with a
   tc_delay_read call produces the invalid `tc_delay_read(...) |= value` shape. */
inline bool assignsLocal(const std::string& line, const std::string& name) {
    const std::string text = trimmed(line);
    if (text.compare(0, name.size(), name) != 0 ||
        (text.size() > name.size() && isIdentifierChar(text[name.size()]))) return false;
    size_t at = name.size();
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    static const char* operators[] = {"|=", "&=", "^=", "+=", "-=", "*=", "/=", "<<=", ">>=", "="};
    for (const char* op : operators) {
        const size_t length = std::strlen(op);
        if (text.compare(at, length, op) != 0) continue;
        if (length == 1 && at + 1 < text.size() && text[at + 1] == '=') return false;
        return true;
    }
    return false;
}

struct BodyStats {
    size_t wave = 0, boundary = 0, publishes = 0;
    size_t guarded = 0, unconditional = 0;    /* event scheduling only */
};

inline bool eventSchedule();

/* The publish a block appends for one of its node variables. */
inline std::string writeCall(const std::string& indent, uint64_t slot, unsigned width, const std::string& name,
                             uint64_t delay) {
    return indent + "game_engine.'tc_delay_write'(U64 " + std::to_string(slot) + ", U64 " +
           std::to_string(width) + ", U64 (" + name + "), U64 " + std::to_string(delay) + ")";
}

/* The slot a wire-copy block merely passes on, if that is all the block does.

   A wire becomes a node of its own in the emitted program (`com_cc_input_buffer`
   and the same shape wherever a wire has to be mirrored for a consumer), and a
   node costs a unit in the delay model.  For a wire that unit is wrong: the copy
   sits one unit behind the pin it comes from, and it stays there for as long as
   the circuit keeps changing - which is what made a wire look stuck while the
   gate's own panel (which reads the pin) showed the new value.  Such a block is
   recognised here and mirrored by the host instead of published with a delay
   (see mirrors()): one slot read, and no operator that could compute. */
inline bool pureCopySource(const std::vector<std::string>& lines, const Block& block, uint64_t& source,
                           unsigned& bits) {
    std::string value;
    for (size_t j = block.header + 1; j < block.end; ++j) {
        const std::string text = trimmed(lines[j]);
        if (text.empty()) continue;
        if (text.rfind("store(", 0) == 0) continue;          /* the block's own writes */
        if (text.rfind("let ", 0) != 0 && text.rfind("var ", 0) != 0) return false;
        if (!value.empty()) return false;                    /* one statement only */
        value = text;
    }
    if (value.empty()) return false;
    const size_t at = value.find("load(<U");
    if (at == std::string::npos) return false;
    if (value.find("load(<U", at + 1) != std::string::npos) return false;
    size_t end = 0;
    unsigned readBits = 0;
    uint64_t offset = 0;
    if (!matchLoad(value, at, readBits, offset, end)) return false;
    /* Everything that is not the read has to be punctuation and casts.  An
       operator anywhere else means the block computes (a gate, a splitter's bit
       field, an inverter) and keeps the delay its kind asks for. */
    for (char c : value.substr(0, at) + value.substr(end))
        if (std::strchr("~&|^*%!?+-<>", c)) return false;
    source = offset;
    bits = readBits;
    return true;
}

/* `game_engine.'tc_delay_write'(U64 <dst>, U64 <bits>, ...)` for a wire copy
   becomes `game_engine.'tc_delay_mirror'(U64 <dst>, U64 <src>, U64 <bits>)`; a
   line that is not a write call is returned unchanged. */
inline std::string wireMirrorCall(const std::string& line, uint64_t source) {
    const std::string marker = "tc_delay_write'(U64 ";
    const size_t at = line.find(marker);
    if (at == std::string::npos) return line;
    size_t pos = at + marker.size();
    const size_t destinationAt = pos;
    while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos]))) ++pos;
    if (pos == destinationAt) return line;
    const uint64_t destination = std::strtoull(line.c_str() + destinationAt, nullptr, 10);
    const std::string middle = ", U64 ";
    if (line.compare(pos, middle.size(), middle) != 0) return line;
    pos += middle.size();
    const size_t bitsAt = pos;
    while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos]))) ++pos;
    if (pos == bitsAt) return line;
    const uint64_t bits = std::strtoull(line.c_str() + bitsAt, nullptr, 10);
    const size_t callAt = line.rfind("game_engine.'", at);
    const std::string prefix = callAt == std::string::npos ? std::string() : line.substr(0, callAt);
    return prefix + "game_engine.'tc_delay_mirror'(U64 " + std::to_string(destination) + ", U64 " +
           std::to_string(source) + ", U64 " + std::to_string(bits) + ")";
}

/* ---- re-closing a connection that was cut for the compile -----------------

   A board with a cycle never reaches the emitter (plan section 13.3), so the
   board that is loaded keeps that one connection cut: the consumer's input is
   driven by a placeholder constant instead of by the producer, which is a legal,
   acyclic graph.  The generated program then carries every component, and the
   transform puts the connection back by pointing the consumer's read of the
   placeholder's slot at the producer's slot - the producer publishes there every
   unit, so the loop closes *through the delay model* and no longer needs an
   evaluation order.

   One record per cut connection: the consumer's component id, the placeholder's
   id (the constant that stands in for the cut wire) and the producer's id. */
struct Reclose {
    int64_t consumer = 0;
    int64_t placeholder = 0;
    int64_t producer = 0;
    unsigned bits = 0;
};

/* A connection the loader cut for the compile: there is no placeholder, the
   consumer's input simply lost its driver, so the emitter folded it to a
   constant.  Older proximity records did not know direction and leave
   `directed` false; exact built-in pin matching records consumer/producer and
   the consumer input index so a two-NAND latch with another open input does not
   have to guess from its folded literals. */
struct CutEdge {
    int64_t first = 0;
    int64_t second = 0;
    unsigned bits = 0;
    uint64_t slotFirst = 0;      /* filled in from the emitted blocks */
    uint64_t slotSecond = 0;
    bool directed = false;
    unsigned input = 0;
};

/* The records the loader produced for this compile.  The environment variable
   stays for the playtests that name the connection by hand; a reader that never
   touches this is a reader the loader did not cut anything for. */
inline std::vector<CutEdge>& pendingCuts() {
    static std::vector<CutEdge> cuts;
    return cuts;
}

inline void addCut(int64_t first, int64_t second, unsigned bits) {
    pendingCuts().push_back(CutEdge{first, second, bits, 0, 0, false, 0});
}

inline void addDirectedCut(int64_t consumer, int64_t producer, unsigned bits, unsigned input) {
    pendingCuts().push_back(CutEdge{consumer, producer, bits, 0, 0, true, input});
}

inline void clearCuts() { pendingCuts().clear(); }

/* ---- cutting a feedback connection before the graph is built -------------

   The compile walks an ordered component list that comes out empty for a board
   with a cycle (plan section 13.3), so the board has to be acyclic at the moment
   the game builds its graph.  The loader therefore detaches one wire end of one
   cycle right before the graph-building call and puts it back right after; the
   transform then re-closes that connection through the delay model, so the
   player's loop exists again - this time legally.

   The reliable input is the normalized wire copy produced immediately before
   preorder (hooked in native.hpp), together with the Board's component sequence.
   Both are a length plus payload pointer, with elements at
   payload + 8 + i * stride.  The Board offsets remain a fallback for the earlier
   cycle check, where the normalized copy does not exist yet. */
inline constexpr uint64_t kComponentStride = 0x238, kWireStride = 0x68, kTableHeader = 8;
inline constexpr uint64_t kMaxBoardComponents = 200000, kMaxBoardWires = 400000;
/* The bound below is about how much work one compile may spend looking for a
   feedback edge, not a safety boundary any more.  Direction comes from exact
   NAND/constant pins (see pinAtPoint), so a board that carries other kinds no
   longer has to be skipped as a whole: only the parts whose pins we do not know
   are ignored, and an edge can only appear between two pins we do know.  Keep
   the pin lookup indexed - scanning every component per wire end is quadratic
   and this runs on every compile. */
inline constexpr uint64_t kMaxAutomaticCutComponents = 4096, kMaxAutomaticCutWires = 16384;

struct CutState {
    bool active = false;
    unsigned char* wire = nullptr;
    int16_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int64_t first = 0, second = 0;
};

inline CutState& cutState() {
    static CutState state;
    return state;
}

inline bool readable(const void* address, size_t bytes) {
    if (!address || !bytes) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto* base = static_cast<const unsigned char*>(region.BaseAddress);
    const auto offset = static_cast<size_t>(static_cast<const unsigned char*>(address) - base);
    return offset + bytes <= region.RegionSize;
}

struct Table {
    uint64_t count = 0;
    const unsigned char* payload = nullptr;
    const unsigned char* element(uint64_t index, uint64_t stride) const {
        return payload ? payload + kTableHeader + index * stride : nullptr;
    }
};

inline bool readTable(const void* address, uint64_t stride, uint64_t limit, Table& out) {
    out = Table{};
    if (!readable(address, 16)) return false;
    std::memcpy(&out.count, address, 8);
    std::memcpy(&out.payload, static_cast<const unsigned char*>(address) + 8, 8);
    if (!out.count || out.count > limit || !out.payload) return false;
    /* The whole sequence must be readable: that is what makes "is this really
       the table the graph builder got" answerable without guessing. */
    return readable(out.payload, kTableHeader + out.count * stride);
}

inline int16_t readI16(const unsigned char* p, size_t offset) {
    int16_t value = 0;
    std::memcpy(&value, p + offset, sizeof(value));
    return value;
}

inline std::string hexBytes(const unsigned char* bytes, size_t count) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        text.push_back(digits[bytes[i] >> 4]);
        text.push_back(digits[bytes[i] & 0x0f]);
    }
    return text;
}

enum class PinRole { none, input, output };

struct PinOwner {
    bool found = false;
    uint64_t component = 0;
    PinRole role = PinRole::none;
    unsigned input = 0;
    /* Set on an output pin of a prototype that has more than one output.  Such
       a component emits one node per output under the same component id, and the
       re-close record can only name the component - so an edge *out of* it must
       not be the one we cut. */
    bool multiOutput = false;
};

inline void rotatePin(int32_t x, int32_t y, unsigned rotation, int32_t& outX, int32_t& outY) {
    switch (rotation & 3u) {
    case 1: outX = -y; outY = x; break;
    case 2: outX = -x; outY = -y; break;
    case 3: outX = y; outY = -x; break;
    default: outX = x; outY = y; break;
    }
}

/* Exact pin ownership, taken from the game's own prototype table (see
   src/gate_pins.generated.hpp).  Unlike the old nearest-component rule this
   knows direction, ignores text boxes/custom components, and cannot turn an
   ordinary reconvergent path into an undirected cycle.

   The generated table only carries **combinational** prototypes: a cycle
   through a state-holding part (Delay Line, Register, RAM, Switch, Counter,
   custom component) is something the game compiles happily, so cutting it would
   break a working circuit instead of legalising a refused one.

   The pins are collected once into an index rather than re-scanned per wire end
   (that scan is components x wires, and this whole pass runs on every compile).
   Two parts whose exact pins land on the same grid point are marked as a
   conflict and own nothing: on that board a wire end cannot be attributed, and
   guessing there is what makes a half-drawn editor board stop compiling. */
using PinIndex = std::map<std::pair<int32_t, int32_t>, PinOwner>;

inline void insertPin(PinIndex& pins, int32_t originX, int32_t originY, unsigned rotation,
                      int32_t localX, int32_t localY, const PinOwner& owner) {
    int32_t dx = 0, dy = 0;
    rotatePin(localX, localY, rotation, dx, dy);
    const auto key = std::make_pair(originX + dx, originY + dy);
    auto found = pins.find(key);
    if (found == pins.end()) {
        pins.emplace(key, owner);
    } else {
        found->second.found = false;           /* overlapping exact pins: do not guess */
    }
}

inline void collectPins(const Table& components, PinIndex& pins) {
    pins.clear();
    for (uint64_t i = 0; i < components.count; ++i) {
        const unsigned char* record = components.element(i, kComponentStride);
        const KindPins* geometry = builtinKindPins(record[0]);
        if (!geometry) continue;
        const int32_t originX = readI16(record, 2), originY = readI16(record, 4);
        const unsigned rotation = record[6];
        for (uint8_t pin = 0; pin < geometry->inCount; ++pin) {
            PinOwner owner;
            owner.found = true;
            owner.component = i;
            owner.role = PinRole::input;
            owner.input = pin;
            insertPin(pins, originX, originY, rotation,
                      geometry->inX[pin], geometry->inY[pin], owner);
        }
        for (uint8_t pin = 0; pin < geometry->outCount; ++pin) {
            PinOwner owner;
            owner.found = true;
            owner.component = i;
            owner.role = PinRole::output;
            owner.input = pin;
            owner.multiOutput = geometry->outCount > 1;
            insertPin(pins, originX, originY, rotation,
                      geometry->outX[pin], geometry->outY[pin], owner);
        }
    }
}

inline bool pinAtPoint(const PinIndex& pins, int32_t x, int32_t y, PinOwner& out) {
    out = PinOwner{};
    const auto found = pins.find(std::make_pair(x, y));
    if (found == pins.end()) return false;
    out = found->second;
    return out.found;
}

/* One directed edge per wire that joins a known output to a known input.
   `input` is the consumer's pin ordinal; `cuttable` says the producer has a
   single output, which is what the re-close record needs to name. */
struct Net {
    uint64_t wire = 0;
    uint64_t producer = 0, consumer = 0;
    unsigned input = 0;
    bool cuttable = true;
};

inline uint64_t componentId(const Table& components, uint64_t index) {
    uint64_t id = 0;
    std::memcpy(&id, components.element(index, kComponentStride) + 8, sizeof(id));
    return id;
}

/* Every board the loader is about to compile is scanned for instances of the
   clock prototype: kind 0x4e (a custom instance) whose prototype id sits at
   +0x188 (the offset the native-logic binding reads).  The rewrite then marks
   the value slots of those instances, which is what turns a read of the clock
   into a unit pulse. */
inline void scanClockInstances(void* componentTable, const std::function<void(const std::string&)>& log) {
    Table components;
    if (!readTable(componentTable, kComponentStride, kMaxBoardComponents, components)) return;
    /* Add only: a table that is not the component table must not be able to
       clear what the real one said. */
    size_t before = clockInstances().size();
    for (uint64_t i = 0; i < components.count; ++i) {
        const unsigned char* record = components.element(i, kComponentStride);
        if (record[0] != 0x4e) continue;
        uint64_t prototype = 0;
        std::memcpy(&prototype, record + 0x188, sizeof(prototype));
        if (prototype != kClockPrototypeId) continue;
        clockInstances().insert(componentId(components, i));
    }
    if (log && clockInstances().size() != before)
        log("Gate delay: board carries " + std::to_string(clockInstances().size()) +
            " clock source(s); their signal is a " + std::to_string(clockWidthUnits()) +
            "-unit pulse at the start of every cycle");
}

/* Finds a cycle in the component graph and detaches one of its wires.  Returns
   false (and changes nothing) when the board has no cycle we can see, when the
   tables do not look like the ones the caller gets, or when a cut is already
   pending. */
inline bool detachCycleEdge(void* componentTable, void* wireTable,
                            const std::function<void(const std::string&)>& log) {
    Table components, wires;
    if (!readTable(componentTable, kComponentStride, kMaxBoardComponents, components)) {
        if (log) log("Gate delay: the first table is not the component table; not cutting here");
        return false;
    }
    if (!readTable(wireTable, kWireStride, kMaxBoardWires, wires)) {
        if (log) log("Gate delay: the second table is not the wire table; not cutting here");
        return false;
    }
    if (components.count > kMaxAutomaticCutComponents || wires.count > kMaxAutomaticCutWires) {
        if (log)
            log("Gate delay: automatic cut skipped for a board too large to scan here (" +
                std::to_string(components.count) + " components, " +
                std::to_string(wires.count) + " wires)");
        return false;
    }
    if (cutState().active) return false;

    PinIndex pins;
    collectPins(components, pins);
    std::vector<Net> nets;
    nets.reserve(wires.count);
    const bool traceWires = GetEnvironmentVariableW(L"TC_GATE_DELAY_WIRE_TRACE", nullptr, 0) > 0;
    if (traceWires && log) {
        for (uint64_t i = 0; i < components.count; ++i) {
            const unsigned char* component = components.element(i, kComponentStride);
            log("Gate delay: component[" + std::to_string(i) + "] id=" +
                std::to_string(componentId(components, i)) + " at " +
                std::to_string(readI16(component, 2)) + "," +
                std::to_string(readI16(component, 4)));
        }
    }
    for (uint64_t i = 0; i < wires.count; ++i) {
        const unsigned char* wire = wires.element(i, kWireStride);
        PinOwner a, b;
        const bool haveA = pinAtPoint(pins, readI16(wire, 0x18), readI16(wire, 0x1a), a);
        const bool haveB = pinAtPoint(pins, readI16(wire, 0x1c), readI16(wire, 0x1e), b);
        if (traceWires && log)
            log("Gate delay: wire[" + std::to_string(i) + "] " +
                std::to_string(readI16(wire, 0x18)) + "," + std::to_string(readI16(wire, 0x1a)) +
                " -> " + std::to_string(readI16(wire, 0x1c)) + "," +
                std::to_string(readI16(wire, 0x1e)) + " owners=" +
                (haveA ? std::to_string(a.component) : std::string("-")) + "," +
                (haveB ? std::to_string(b.component) : std::string("-")) + " raw=" +
                hexBytes(wire, kWireStride));
        if (!haveA || !haveB || a.component == b.component) continue;
        if (a.role == PinRole::output && b.role == PinRole::input)
            nets.push_back(Net{i, a.component, b.component, b.input, !a.multiOutput});
        else if (b.role == PinRole::output && a.role == PinRole::input)
            nets.push_back(Net{i, b.component, a.component, a.input, !b.multiOutput});
    }
    if (log)
        log("Gate delay: board has " + std::to_string(components.count) + " components, " +
            std::to_string(wires.count) + " wires, " + std::to_string(nets.size()) +
            " directed combinational connections");
    if (nets.empty()) return false;

    /* Iterative depth-first search over directed dependencies: an edge to a
       component already on the current path is real feedback.  Which edge of a
       cycle comes out as the back edge depends on where the walk starts, so a
       pass that had to skip a back edge (multi-output producer) is repeated with
       the roots in reverse before giving up. */
    std::vector<char> state(components.count, 0);          /* 0 new, 1 on path, 2 done */
    std::vector<std::vector<uint64_t>> outgoing(components.count);
    for (size_t i = 0; i < nets.size(); ++i) outgoing[nets[i].producer].push_back(i);
    bool skippedBackEdge = false;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass && !skippedBackEdge) break;
        std::fill(state.begin(), state.end(), 0);
        std::vector<size_t> cursor(components.count, 0);
        std::vector<uint64_t> stack;
        skippedBackEdge = false;
        for (uint64_t step = 0; step < components.count; ++step) {
            const uint64_t root = pass ? components.count - 1 - step : step;
            if (state[root]) continue;
            stack.clear();
            stack.push_back(root);
            while (!stack.empty()) {
                const uint64_t node = stack.back();
                state[node] = 1;
                bool descended = false;
                while (cursor[node] < outgoing[node].size()) {
                    const uint64_t net = outgoing[node][cursor[node]++];
                    const uint64_t next = nets[net].consumer;
                    if (state[next] == 1) {
                        /* Back edge: this wire is on a cycle.  A producer with
                           more than one output cannot be named by the re-close
                           record, so skip that edge and keep looking - the
                           same cycle usually has another edge we can cut. */
                        if (!nets[net].cuttable) {
                            skippedBackEdge = true;
                            continue;
                        }
                        const unsigned char* wire = wires.element(nets[net].wire, kWireStride);
                        CutState& cut = cutState();
                        cut.wire = const_cast<unsigned char*>(wire);
                        cut.x1 = readI16(wire, 0x18);
                        cut.y1 = readI16(wire, 0x1a);
                        cut.x2 = readI16(wire, 0x1c);
                        cut.y2 = readI16(wire, 0x1e);
                        cut.first = static_cast<int64_t>(componentId(components, nets[net].consumer));
                        cut.second = static_cast<int64_t>(componentId(components, nets[net].producer));
                        /* Detach one end: far enough that no pin can reach it. */
                        const int16_t moved = static_cast<int16_t>(cut.x2 - 64);
                        std::memcpy(cut.wire + 0x1c, &moved, sizeof(moved));
                        cut.active = true;
                        clearCuts();
                        unsigned bits = 0;
                        memcpy(&bits, wire + 0x30, 4);
                        addDirectedCut(cut.first, cut.second, bits >= 1 && bits <= 64 ? bits : 1,
                                       nets[net].input);
                        if (log)
                            log("Gate delay: cut wire " + std::to_string(nets[net].wire) +
                                " between " + std::to_string(cut.first) + " and " +
                                std::to_string(cut.second) +
                                " so the compile sees an acyclic board");
                        return true;
                    }
                    if (state[next] == 0) {
                        stack.push_back(next);
                        descended = true;
                        break;
                    }
                }
                if (descended) continue;
                state[node] = 2;
                stack.pop_back();
            }
        }
    }
    if (log)
        log("Gate delay: no cycle among the " + std::to_string(nets.size()) + " connections; nothing to cut");
    return false;
}

/* Which components sit on a feedback loop: the graph the cut pass builds, run
   through Tarjan to get its strongly connected components.  Each component of
   size >= 2 is a "tie group" for the rule above; the runtime then needs their
   state slots, which the rewrite finds from the emitted blocks. */
inline void noteTieGroups(void* componentTable, void* wireTable,
                          const std::function<void(const std::string&)>& log) {
    tieGroupComponents().clear();
    Table components, wires;
    if (!readTable(componentTable, kComponentStride, kMaxBoardComponents, components)) return;
    if (!readTable(wireTable, kWireStride, kMaxBoardWires, wires)) return;
    if (components.count > kMaxAutomaticCutComponents || wires.count > kMaxAutomaticCutWires) return;
    PinIndex pins;
    collectPins(components, pins);
    std::vector<std::vector<uint64_t>> outgoing(components.count);
    for (uint64_t i = 0; i < wires.count; ++i) {
        const unsigned char* wire = wires.element(i, kWireStride);
        PinOwner a, b;
        const bool haveA = pinAtPoint(pins, readI16(wire, 0x18), readI16(wire, 0x1a), a);
        const bool haveB = pinAtPoint(pins, readI16(wire, 0x1c), readI16(wire, 0x1e), b);
        if (!haveA || !haveB || a.component == b.component) continue;
        if (a.role == PinRole::output && b.role == PinRole::input)
            outgoing[a.component].push_back(b.component);
        else if (b.role == PinRole::output && a.role == PinRole::input)
            outgoing[b.component].push_back(a.component);
    }
    const uint64_t count = components.count;
    std::vector<int64_t> index(count, -1), low(count, 0);
    std::vector<char> onStack(count, 0);
    std::vector<uint64_t> stack, work, cursor(count, 0);
    int64_t nextIndex = 0;
    for (uint64_t root = 0; root < count; ++root) {
        if (index[root] >= 0) continue;
        work.clear();
        work.push_back(root);
        while (!work.empty()) {
            const uint64_t node = work.back();
            if (index[node] < 0) {
                index[node] = low[node] = nextIndex++;
                stack.push_back(node);
                onStack[node] = 1;
            }
            bool descended = false;
            while (cursor[node] < outgoing[node].size()) {
                const uint64_t next = outgoing[node][cursor[node]++];
                if (index[next] < 0) {
                    work.push_back(next);
                    descended = true;
                    break;
                }
                if (onStack[next]) low[node] = std::min(low[node], index[next]);
            }
            if (descended) continue;
            if (low[node] == index[node]) {
                std::vector<uint64_t> members;
                while (!stack.empty()) {
                    const uint64_t member = stack.back();
                    stack.pop_back();
                    onStack[member] = 0;
                    members.push_back(member);
                    if (member == node) break;
                }
                if (members.size() >= 2) {
                    std::sort(members.begin(), members.end());      /* board order */
                    std::vector<int64_t> group;
                    for (uint64_t member : members)
                        group.push_back(static_cast<int64_t>(componentId(components, member)));
                    tieGroupComponents().push_back(group);
                }
            }
            work.pop_back();
        }
    }
    if (log && !tieGroupComponents().empty())
        log("Gate delay: " + std::to_string(tieGroupComponents().size()) +
            " feedback group(s) carry a deterministic tie-break (board order wins)");
}

inline void restoreCut() {
    CutState& cut = cutState();
    if (!cut.active || !cut.wire) return;
    std::memcpy(cut.wire + 0x18, &cut.x1, sizeof(cut.x1));
    std::memcpy(cut.wire + 0x1a, &cut.y1, sizeof(cut.y1));
    std::memcpy(cut.wire + 0x1c, &cut.x2, sizeof(cut.x2));
    std::memcpy(cut.wire + 0x1e, &cut.y2, sizeof(cut.y2));
    cut.active = false;
    cut.wire = nullptr;
}

/* Called by the loader's hook on the game's preorder entry point (see
   src/native.hpp): with the mode on and the sandbox loaded, make the board the
   graph builder is about to read acyclic.  The loader restores the wire as soon
   as the call returns, so the player's board never changes. */
inline bool enabled();

inline bool cutForCompile(void* componentTable, void* wireTable,
                          const std::function<void(const std::string&)>& log) {
    if (!enabled()) return false;
    /* Every compile scans the board for clock sources, cut or not: the clock's
       unit pulse is a property of the mode, not of the cycle we happen to cut. */
    clockInstances().clear();
    scanClockInstances(componentTable, log);
    scanClockInstances(wireTable, log);          /* the arguments are swapped sometimes */
    noteTieGroups(componentTable, wireTable, log);
    if (tieGroupComponents().empty()) noteTieGroups(wireTable, componentTable, log);
    if (detachCycleEdge(componentTable, wireTable, log)) return true;
    return detachCycleEdge(wireTable, componentTable, log);
}

inline void afterCompileCut() { restoreCut(); }

/* A cut record belongs to exactly one generated compiler program.  Restoring
   the temporary wire does not consume that record because rewrite still needs
   it to re-close the connection.  Once rewrite succeeds or explicitly rejects
   it, both halves of the state must be retired together. */
inline void finishCompileCut() {
    restoreCut();
    clearCuts();
}

/* The board model the loader is showing (it gets it from the level.load event).
   The cycle check runs before the graph is built, so this is where a cut has to
   be made from - see the loader's hook on the game's cycle search. */
inline void*& boardModel() {
    static void* model = nullptr;
    return model;
}

inline void setBoardModel(void* model) { boardModel() = model; }

/* Cuts the board the loader knows about: the component and wire tables hang off
   the model at the offsets the probes already use (board +0x78, board +0x98). */
inline bool cutBoardModel(const std::function<void(const std::string&)>& log) {
    if (!enabled() || !boardModel()) return false;
    auto* base = static_cast<unsigned char*>(boardModel());
    if (!readable(base, 0xa8)) return false;
    if (GetEnvironmentVariableW(L"TC_GATE_DELAY_WIRE_TRACE", nullptr, 0) > 0 && log) {
        uint64_t componentCount = 0;
        std::memcpy(&componentCount, base + 0x78, 8);
        static uint64_t tracedCount = UINT64_MAX;
        if (tracedCount != componentCount) {
            tracedCount = componentCount;
            if (readable(base, 0x180))
                log("Gate delay: board raw=" + hexBytes(base, 0x180));
            for (size_t offset = 0; offset + 16 <= 0x300; offset += 8) {
                if (!readable(base + offset, 16)) break;
                uint64_t count = 0;
                const unsigned char* payload = nullptr;
                std::memcpy(&count, base + offset, 8);
                std::memcpy(&payload, base + offset + 8, 8);
                if (!count || count > 64 || !readable(payload, 64)) continue;
                log("Gate delay: board sequence +" + std::to_string(offset) +
                    " count=" + std::to_string(count) + " head=" + hexBytes(payload, 64));
            }
        }
    }
    if (detachCycleEdge(base + 0x78, base + 0x98, log)) return true;
    return detachCycleEdge(base + 0x98, base + 0x78, log);
}

struct Body {
    size_t from = 0;
    size_t to = 0;
    bool slotBody = false;      /* mode_refresh: state slots, not local variables */
};

inline std::vector<Body> findBodies(const std::vector<std::string>& lines) {
    const std::string refresh = "def mode_refresh() None {";
    std::vector<Body> bodies;
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        const std::string text = trimmed(lines[i]);
        Body body;
        if (text == refresh) {
            body.slotBody = true;
            int depth = 0;
            for (size_t j = i; j < lines.size(); ++j) {
                depth += static_cast<int>(std::count(lines[j].begin(), lines[j].end(), '{'));
                depth -= static_cast<int>(std::count(lines[j].begin(), lines[j].end(), '}'));
                if (depth == 0 && j > i) { body.from = i + 1; body.to = j; break; }
            }
        } else if (text.rfind("while cycle < burst_target_cycle {", 0) == 0) {
            body.from = i + 1;
            for (size_t j = body.from; j < lines.size(); ++j) {
                if (trimmed(lines[j]).rfind("cycle += 1", 0) == 0) { body.to = j; break; }
            }
        }
        if (body.to > body.from) bodies.push_back(body);
    }
    return bodies;
}

/* The emitter's blocks inside one body: header line and the line past its body. */
struct BlockRange {
    size_t header = 0;
    size_t end = 0;
    Block info;
    bool hasId = false;
    int64_t id = 0;
};

inline std::vector<BlockRange> blocksIn(const std::vector<std::string>& lines, const Body& body) {
    std::vector<BlockRange> blocks;
    for (size_t i = body.from; i < body.to; ++i) {
        BlockRange range;
        if (!parseHeaderFull(lines[i], range.info, &range.hasId, &range.id)) continue;
        range.header = i;
        size_t end = body.to;
        for (size_t j = i + 1; j < body.to; ++j) {
            Block next;
            if (parseHeader(lines[j], next)) { end = j; break; }
        }
        range.end = end;
        blocks.push_back(range);
    }
    return blocks;
}

/* The state slot a block publishes: the number in its node variable.  Both
   bodies use the same slot numbers for the same component (node 93 publishes
   slot 308 through `vid308` in mode_run and stores exactly 308 in
   mode_refresh), which is what lets one id lookup serve both. */
inline bool blockSlot(const std::vector<std::string>& lines, size_t from, size_t to, uint64_t& slot) {
    for (size_t j = from; j < to; ++j) {
        const std::string text = trimmed(lines[j]);
        const size_t at = text.find("vid");
        const size_t valueAt = text.find("value_id");
        size_t start = std::string::npos;
        if (valueAt != std::string::npos && (at == std::string::npos || valueAt < at)) start = valueAt + 8;
        else if (at != std::string::npos) start = at + 3;
        if (start == std::string::npos) continue;
        /* The digit run starts right after the name, so only what follows it can
           disqualify the match (`vid256` must not match `vid2560`). */
        size_t cursor = start;
        uint64_t number = 0;
        if (!readDigits(number, text, cursor)) continue;
        if (cursor < text.size() && isIdentifierChar(text[cursor])) continue;
        slot = number;
        return true;
    }
    return false;
}

/* `vidN` / `value_idN` for one slot, rewritten into a snapshot read of another.
   Used on the run body, where blocks pass values through local variables and a
   consumer may sit *before* the producer (the compile saw an acyclic graph). */
inline std::string redirectVarRefs(const std::string& line, uint64_t fromSlot, uint64_t toSlot,
                                   unsigned bits) {
    std::string out;
    out.reserve(line.size() + 48);
    size_t pos = 0;
    while (pos < line.size()) {
        if (isIdentifierChar(line[pos]) && !std::isdigit(static_cast<unsigned char>(line[pos]))) {
            size_t end = pos;
            while (end < line.size() && isIdentifierChar(line[end])) ++end;
            const std::string name = line.substr(pos, end - pos);
            const size_t cut = name.find_first_of("0123456789");
            const bool nodeName = name.rfind("vid", 0) == 0 ||
                                  (name.rfind("value_id", 0) == 0);
            if (nodeName && cut != std::string::npos &&
                std::strtoull(name.c_str() + cut, nullptr, 10) == fromSlot) {
                out += "((U" + std::to_string(bits) + " game_engine.'tc_delay_read'(U64 " +
                       std::to_string(toSlot) + ", U64 " + std::to_string(bits) + ")))";
            } else {
                out += name;
            }
            pos = end;
            continue;
        }
        out += line[pos++];
    }
    return out;
}

/* The refresh body reads its inputs from state slots, so the same idea is a slot
   number swap there. */
inline std::string redirectSlotReads(const std::string& line, uint64_t fromSlot, uint64_t toSlot) {
    std::string text = line;
    size_t pos = 0;
    while ((pos = text.find("load(<U", pos)) != std::string::npos) {
        unsigned bits = 0;
        uint64_t offset = 0;
        size_t end = 0;
        if (!matchLoad(text, pos, bits, offset, end)) { ++pos; continue; }
        if (offset != fromSlot) { pos = end; continue; }
        const std::string replacement = "load(<U" + std::to_string(bits) + ">, #SIMULATION_STATE + " +
                                        std::to_string(toSlot) + ")";
        text.replace(pos, end - pos, replacement);
        pos += replacement.size();
    }
    return text;
}

/* Applies every re-close record.  Runs before the phasing pass, so a re-closed
   read is phased exactly like any other cross-component read. */
inline size_t applyReclosures(std::vector<std::string>& lines, const std::vector<Body>& bodies,
                              const std::vector<Reclose>& records, std::vector<std::string>& problems,
                              const std::function<void(const std::string&)>& log) {
    size_t applied = 0;
    for (const Reclose& record : records) {
        uint64_t placeholderSlot = 0, producerSlot = 0;
        unsigned bits = record.bits;
        bool foundPlaceholder = false, foundProducer = false;
        for (const Body& body : bodies) {
            for (const BlockRange& block : blocksIn(lines, body)) {
                if (!block.hasId) continue;
                if (block.id == record.placeholder && !foundPlaceholder) {
                    foundPlaceholder = blockSlot(lines, block.header + 1, block.end, placeholderSlot);
                    if (!bits) bits = block.info.bits;
                }
                if (block.id == record.producer && !foundProducer)
                    foundProducer = blockSlot(lines, block.header + 1, block.end, producerSlot);
            }
        }
        const std::string name = std::to_string(record.consumer) + "<-" + std::to_string(record.producer);
        if (!foundPlaceholder || !foundProducer) {
            problems.push_back("re-close " + name + ": the " +
                               (foundPlaceholder ? "producer" : "placeholder") + " has no node slot");
            continue;
        }
        if (!bits) bits = 1;
        bool touched = false;
        for (const Body& body : bodies) {
            for (const BlockRange& block : blocksIn(lines, body)) {
                if (!block.hasId || block.id != record.consumer) continue;
                for (size_t j = block.header + 1; j < block.end; ++j) {
                    const std::string before = lines[j];
                    lines[j] = body.slotBody
                                   ? redirectSlotReads(lines[j], placeholderSlot, producerSlot)
                                   : redirectVarRefs(lines[j], placeholderSlot, producerSlot, bits);
                    if (lines[j] != before) touched = true;
                }
            }
        }
        if (!touched) {
            problems.push_back("re-close " + name + ": the consumer never read slot " +
                               std::to_string(placeholderSlot));
            continue;
        }
        ++applied;
        if (log)
            log("Gate delay: re-closed " + name + " - slot " + std::to_string(placeholderSlot) + " reads slot " +
                std::to_string(producerSlot) + ", " + std::to_string(bits) + " bit");
    }
    return applied;
}

/* `consumer:placeholder:producer[:bits][;...]`, ids in decimal - the emitter
   prints a component's own signed 64-bit id in its block comment. */
inline std::vector<Reclose> parseReclosures(const std::string& text) {
    std::vector<Reclose> records;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t stop = text.find(';', at);
        const std::string item =
            text.substr(at, stop == std::string::npos ? std::string::npos : stop - at);
        at = stop == std::string::npos ? text.size() + 1 : stop + 1;
        std::vector<int64_t> fields;
        size_t cursor = 0;
        while (cursor <= item.size()) {
            const size_t end = item.find(':', cursor);
            const std::string field =
                trimmed(item.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor));
            cursor = end == std::string::npos ? item.size() + 1 : end + 1;
            if (!field.empty()) fields.push_back(std::strtoll(field.c_str(), nullptr, 10));
        }
        if (fields.size() < 3) continue;
        Reclose record;
        record.consumer = fields[0];
        record.placeholder = fields[1];
        record.producer = fields[2];
        record.bits = fields.size() > 3 && fields[3] > 0 ? static_cast<unsigned>(fields[3]) : 0;
        records.push_back(record);
    }
    return records;
}

inline std::vector<Reclose> environmentReclosures() {
    const char* value = std::getenv("TC_GATE_DELAY_CLOSE");
    return value && value[0] ? parseReclosures(value) : std::vector<Reclose>{};
}

/* ---- putting back a connection the loader cut ---------------------------

   When there is no placeholder (the loader detached a wire end in the model, so
   the consumer's input simply has no driver), the emitter folds that operand to
   a constant - `(U1 0x0)` in every dump collected so far.  Putting the
   connection back is replacing that operand with a read of the producer's slot,
   written the way the emitter writes its own reads so that the phasing pass and
   the event pass treat it like any other input. */
inline bool foldedLiteralAt(const std::string& text, size_t pos, unsigned& width, size_t& end) {
    if (text.compare(pos, 2, "(U") != 0) return false;
    size_t cursor = pos + 2;
    uint64_t bits = 0;
    if (!readDigits(bits, text, cursor)) return false;
    while (cursor < text.size() && text[cursor] == ' ') ++cursor;
    if (cursor >= text.size() || text[cursor] != '0') return false;      /* only a zero is a folded input */
    ++cursor;
    if (cursor < text.size() && text[cursor] == 'x') {                   /* 0x0 */
        ++cursor;
        if (cursor >= text.size() || text[cursor] != '0') return false;
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ')') return false;
    width = static_cast<unsigned>(bits);
    end = cursor + 1;
    return true;
}

/* Counts the folded operands a block carries, and remembers where the first one
   is.  More than one means this is not a single cut connection: refusing beats
   guessing which input was the cut one. */
inline size_t foldedOperands(const std::vector<std::string>& lines, size_t from, size_t to,
                             size_t& lineIndex, size_t& position, unsigned& width) {
    size_t count = 0;
    for (size_t j = from; j < to; ++j) {
        size_t at = 0;
        while ((at = lines[j].find("(U", at)) != std::string::npos) {
            unsigned bits = 0;
            size_t end = 0;
            if (!foldedLiteralAt(lines[j], at, bits, end)) { ++at; continue; }
            if (count == 0) {
                lineIndex = j;
                position = at;
                width = bits;
            }
            ++count;
            at = end;
        }
    }
    return count;
}

inline bool foldedOperandAt(const std::vector<std::string>& lines, size_t from, size_t to,
                            size_t ordinal, size_t& lineIndex, size_t& position,
                            unsigned& width, size_t& end) {
    size_t seen = 0;
    for (size_t j = from; j < to; ++j) {
        size_t at = 0;
        while ((at = lines[j].find("(U", at)) != std::string::npos) {
            unsigned bits = 0;
            size_t literalEnd = 0;
            if (!foldedLiteralAt(lines[j], at, bits, literalEnd)) { ++at; continue; }
            if (seen++ == ordinal) {
                lineIndex = j;
                position = at;
                width = bits;
                end = literalEnd;
                return true;
            }
            at = literalEnd;
        }
    }
    return false;
}

/* `applyCuts` runs before the phasing pass: it looks at the two components of a
   cut wire, decides which one lost an input (the one carrying a folded operand)
   and points that input at the other's slot. */
struct CutBlock {
    bool present = false;
    size_t from = 0;
    size_t to = 0;
    uint64_t slot = 0;
};

inline CutBlock cutBlockFor(const std::vector<std::string>& lines, const Body& body, int64_t id) {
    CutBlock found;
    for (const BlockRange& block : blocksIn(lines, body)) {
        if (!block.hasId || block.id != id) continue;
        found.present = true;
        found.from = block.header + 1;
        found.to = block.end;
        blockSlot(lines, found.from, found.to, found.slot);
        return found;
    }
    return found;
}

inline size_t applyCuts(std::vector<std::string>& lines, const std::vector<Body>& bodies,
                        std::vector<std::string>& problems,
                        const std::function<void(const std::string&)>& log) {
    size_t applied = 0;
    for (CutEdge& cut : pendingCuts()) {
        const std::string name = std::to_string(cut.first) + "/" + std::to_string(cut.second);
        /* Exact pin records already name consumer and producer.  Legacy/manual
           records retain the conservative folded-operand inference. */
        bool decided = false;
        int64_t consumerId = 0, producerId = 0;
        for (const Body& body : bodies) {
            const CutBlock first = cutBlockFor(lines, body, cut.first);
            const CutBlock second = cutBlockFor(lines, body, cut.second);
            if (!first.present || !second.present) continue;
            if (cut.directed) {
                consumerId = cut.first;
                producerId = cut.second;
                decided = true;
                break;
            }
            size_t line = 0, position = 0;
            unsigned width = 0;
            const size_t firstFolded = foldedOperands(lines, first.from, first.to, line, position, width);
            const size_t secondFolded = foldedOperands(lines, second.from, second.to, line, position, width);
            if (firstFolded == 1 && secondFolded == 0) {
                consumerId = cut.first; producerId = cut.second; decided = true;
            } else if (secondFolded == 1 && firstFolded == 0) {
                consumerId = cut.second; producerId = cut.first; decided = true;
            } else {
                problems.push_back("cut " + name + ": the folded input is ambiguous (" +
                                   std::to_string(firstFolded) + " vs " + std::to_string(secondFolded) + ")");
            }
            break;
        }
        if (!decided) {
            if (problems.empty() || problems.back().rfind("cut " + name, 0) != 0)
                problems.push_back("cut " + name + ": the two components are not both in the program");
            continue;
        }
        /* Replace the folded operand in every body, so both cycle bodies carry
           the connection. */
        uint64_t producerSlot = 0;
        bool touched = false;
        for (const Body& body : bodies) {
            const CutBlock consumer = cutBlockFor(lines, body, consumerId);
            const CutBlock producer = cutBlockFor(lines, body, producerId);
            if (!consumer.present || !producer.present || !producer.slot) continue;
            size_t line = 0, position = 0, end = 0;
            unsigned width = cut.bits;
            const size_t folded = foldedOperands(lines, consumer.from, consumer.to,
                                                  line, position, width);
            if (!folded) continue;
            const size_t ordinal = folded == 1 ? 0 : cut.input;
            if (!foldedOperandAt(lines, consumer.from, consumer.to, ordinal,
                                 line, position, width, end)) continue;
            if (!width) width = 1;
            lines[line].replace(position, end - position,
                                "(U" + std::to_string(width) + " load(<U" + std::to_string(width) +
                                    ">, #SIMULATION_STATE + " + std::to_string(producer.slot) + "))");
            producerSlot = producer.slot;
            touched = true;
        }
        if (!touched) {
            problems.push_back("cut " + name + ": the folded operand could not be replaced");
            continue;
        }
        ++applied;
        if (log)
            log("Gate delay: cut wire re-closed " + std::to_string(consumerId) + " <- " +
                std::to_string(producerId) + " (consumer input now reads slot " +
                std::to_string(producerSlot) + ")");
    }
    return applied;
}

/* Splits a block's lines into statements: a statement ends when the braces it
   opened are closed again, so a multi-line `if { ... }` is guarded as one unit. */
inline std::vector<std::pair<size_t, size_t>> statementGroups(const std::vector<std::string>& lines,
                                                              size_t from, size_t to) {
    std::vector<std::pair<size_t, size_t>> groups;
    size_t start = from;
    int depth = 0;
    bool open = false;
    for (size_t i = from; i < to; ++i) {
        const std::string text = trimmed(lines[i]);
        if (text.empty()) continue;
        if (!open) { start = i; open = true; }
        depth += static_cast<int>(std::count(lines[i].begin(), lines[i].end(), '{'));
        depth -= static_cast<int>(std::count(lines[i].begin(), lines[i].end(), '}'));
        if (depth <= 0) { groups.push_back({start, i + 1}); depth = 0; open = false; }
    }
    if (open) groups.push_back({start, to});
    return groups;
}

/* Rewrites one cycle body into a unit loop.  `slotBody` is the refresh body,
   whose cross-component traffic already goes through state slots; the run body
   passes values through local variables and needs the extra publishing step. */
inline std::vector<std::string> rewriteBody(const std::vector<std::string>& lines, size_t from, size_t to,
                                             uint64_t units, const Slots& slots, bool slotBody, Runtime& stats,
                                             BodyStats& bodyStats, std::vector<std::string>& problems) {
    /* `units` is only the fallback for a program whose runtime has not measured
       K yet: how many units a pass advances is answered at run time so stepping
       can be switched on without recompiling (see unitsThisPass). */
    (void)units;
    std::vector<Block> blocks;
    for (size_t i = from; i < to; ++i) {
        Block block;
        if (!parseHeader(lines[i], block)) continue;
        block.header = i;
        size_t end = to;
        for (size_t j = i + 1; j < to; ++j) {
            Block next;
            if (parseHeader(lines[j], next)) { end = j; break; }
        }
        /* The last component is followed by body-level reporting and the
           loader's scope tick.  They have no component header of their own, so
           treating them as part of the last block makes that block look
           host-backed and can pull unrelated declarations into its guard. */
        if (end == to) {
            for (size_t j = i + 1; j < to; ++j) {
                const std::string one = trimmed(lines[j]);
                if (one.find("game_engine.'tc_scope_tick'") != std::string::npos ||
                    one.rfind("cycle += 1", 0) == 0) {
                    end = j;
                    break;
                }
            }
        }
        block.end = end;
        std::string body;
        for (size_t j = i + 1; j < end; ++j) { body += lines[j]; body += '\n'; }
        block.host = hasHostCall(body);
        blocks.push_back(block);
    }
    if (blocks.empty()) return std::vector<std::string>(lines.begin() + from, lines.begin() + to);

    const std::string outer = indentOf(lines[blocks.front().header]);
    const std::string inner = outer + "    ";
    std::vector<std::string> out;
    /* Prelude.  The burst loop (`while cycle < burst_target_cycle`) is the game's
       "run to cycle N" loop: with one unit per pass it has to leave after that
       unit, or a single pass would swallow the whole burst and the display would
       jump again.  `tc_pass_more` is that budget; the cycle counter itself is
       advanced at the cycle boundary further down, so a game cycle still takes K
       passes and the clock keeps its "once per cycle" period. */
    bool stepped = false;
    for (size_t i = from; i < blocks.front().header; ++i) {
        const std::string one = trimmed(lines[i]);
        if (one.rfind("while cycle < ", 0) == 0) {
            stepped = true;
            std::string condition = one;
            const size_t brace = condition.find_last_of('{');
            if (brace != std::string::npos) condition.insert(brace, "and tc_pass_more ");
            out.push_back(outer + "var tc_pass_more = true");
            out.push_back(indentOf(lines[i]) + condition);
            continue;
        }
        out.push_back(lines[i]);
    }
    /* How many units this pass may advance is a runtime answer, so stepping can
       be switched on without recompiling: one unit per pass makes the board show
       every unit.  The explicit type prefix is not decoration - a bare foreign
       call in an initializer trips the game's front end. */
    out.push_back(outer + "var tc_unit = Int (U64 game_engine.'tc_delay_units_this_pass'(U64 " +
                  std::string(slotBody ? "0" : "1") + "))");
    out.push_back(outer + "while tc_unit > 0 {");
    out.push_back(inner + "tc_unit -= 1");
    out.push_back(inner + "game_engine.'tc_delay_begin'(U64 " +
                  std::string(slotBody ? "0" : "1") + ")");

    std::map<std::string, VarRef> known;
    for (const Block& block : blocks) {
        const uint64_t delay = kindDelay(block.kind, block.bits);
        std::vector<std::string> emitted, declared, publish;
        std::vector<std::string> keepLocal;
        if (!slotBody) {
            for (const auto& entry : known) {
                for (size_t j = block.header + 1; j < block.end; ++j) {
                    if (!assignsLocal(lines[j], entry.first)) continue;
                    keepLocal.push_back(entry.first);
                    break;
                }
            }
        }
        /* Locals of this block, so an untyped `store(ptr + N, value)` can learn
           the width the emitter meant.  Reset per block: the emitter reuses
           names like `value` in every block. */
        std::map<std::string, unsigned> locals;
        for (size_t j = block.header + 1; j < block.end; ++j) {
            if (trimmed(lines[j]).empty()) { emitted.push_back(lines[j]); continue; }
            std::string localName;
            unsigned localBits = 0;
            if (localDecl(lines[j], localName, localBits)) locals[localName] = localBits;
            std::string line = block.late ? lines[j]
                                          : rewriteState(lines[j], slots, locals, delay, stats, problems);
            if (!block.late && !slotBody) line = rewriteRefs(line, known, keepLocal);
            std::string name;
            unsigned bits = 0;
            uint64_t slot = 0;
            if (!block.late && !slotBody && nodeVarName(line, name, bits, slot)) {
                const unsigned width = slots.widthOf(slot, bits);
                if (!width) problems.push_back("no slot width for " + name + " (" + block.kind + ")");
                else {
                    declared.push_back(name);
                    publish.push_back(writeCall(inner, slot, width, name, delay));
                }
            } else if (block.late && nodeVarName(line, name, bits, slot)) {
                problems.push_back("a LATE block declares " + name + "; its publishes are skipped");
            }
            emitted.push_back(line);
        }
        /* A wire copy is not a device: mirror its source instead of publishing
           with a delay of its own (see pureCopySource/mirrors). */
        if (!block.late) {
            uint64_t copySource = 0;
            unsigned copyBits = 0;
            if (pureCopySource(lines, block, copySource, copyBits)) {
                for (std::string& line : emitted) line = wireMirrorCall(line, copySource);
                for (std::string& line : publish) line = wireMirrorCall(line, copySource);
            }
        }
        if (block.late) {
            ++bodyStats.boundary;
            /* The emitter's cycle-boundary commit: once per cycle, into the real
               state array, with the block's own lines left exactly as they are. */
            out.push_back(inner + "if (U64 game_engine.'tc_delay_cycle_start'()) != 0 {");
            for (const std::string& line : emitted)
                if (!trimmed(line).empty()) out.push_back(inner + "    " + trimmed(line));
            out.push_back(inner + "}");
        } else if (block.host) {
            ++bodyStats.boundary;
            /* The call, every value it declares, and the stores/publishes that
               consume those values are one lexical unit.  Splitting only the
               call into a guard puts a declaration such as `value_id260`
               inside the `if` while leaving its delay_write outside, which the
               real compiler correctly rejects as out of scope. */
            out.push_back(inner + "if (U64 game_engine.'tc_delay_cycle_start'()) != 0 {");
            for (const std::string& line : emitted) {
                const std::string one = trimmed(line);
                if (!one.empty()) out.push_back(inner + "    " + one);
            }
            for (const std::string& line : publish) {
                out.push_back(inner + "    " + trimmed(line));
                ++bodyStats.publishes;
            }
            publish.clear();
            out.push_back(inner + "}");
        } else {
            ++bodyStats.wave;
            bool published = false;
            if (eventSchedule()) {
                /* M1b: evaluate this block only when one of its inputs moved.
                   A block with no input of its own (a constant, a level input)
                   has nothing that could go dirty, so it keeps running - which
                   is also what publishes a source's value in the first place. */
                std::vector<uint64_t> inputs;
                for (size_t j = block.header + 1; j < block.end; ++j)
                    collectInputSlots(lines[j], known, inputs);
                if (!inputs.empty()) {
                    /* The type prefix is not decoration: a bare foreign call
                       inside a condition makes the game's front end assert
                       (front_end.nim, `lhs.exp.info.value.kind != exp_none`),
                       which is how the first event-pass run died. */
                    std::string guard = inner + "if (";
                    for (size_t k = 0; k < inputs.size(); ++k)
                        guard += (k ? " || " : "") + std::string("(U64 game_engine.'tc_delay_dirty'(U64 ") +
                                 std::to_string(inputs[k]) + ")) != 0";
                    guard += ") {";
                    out.push_back(guard);
                    for (const std::string& line : emitted) out.push_back(inner + "    " + trimmed(line));
                    for (const std::string& line : publish) {
                        out.push_back(inner + "    " + trimmed(line));
                        ++bodyStats.publishes;
                    }
                    out.push_back(inner + "}");
                    publish.clear();               /* already emitted inside the guard */
                    published = true;
                    ++bodyStats.guarded;
                }
            }
            if (!published) {
                for (const std::string& line : emitted) out.push_back(line);
                if (eventSchedule()) ++bodyStats.unconditional;
            }
        }
        /* A block's own local variables only become "the committed value" once
           the block is done with them, and later blocks may read them. */
        for (const std::string& line : publish) { out.push_back(line); ++bodyStats.publishes; }
        for (const std::string& name : declared) {
            const size_t cut = name.find_first_of("0123456789");
            const uint64_t slot = cut == std::string::npos ? 0 : std::strtoull(name.c_str() + cut, nullptr, 10);
            const unsigned width = slots.widthOf(slot, 0);
            if (width) known[name] = VarRef{slot, width};
        }
    }
    /* Tail: what the emitter puts after the last block, including the per-cycle
       scope tick the loader inserts before `cycle += 1`.  A host call there also
       has to stay once per cycle. */
    {
        const size_t tailFrom = blocks.back().end;
        const std::vector<std::pair<size_t, size_t>> groups = statementGroups(lines, tailFrom, to);
        size_t cursor = tailFrom;
        for (const auto& group : groups) {
            for (size_t i = cursor; i < group.first; ++i) out.push_back(lines[i]);
            std::string text;
            for (size_t i = group.first; i < group.second; ++i) { text += trimmed(lines[i]); text += '\n'; }
            if (!hasHostCall(text)) {
                for (size_t i = group.first; i < group.second; ++i) out.push_back(lines[i]);
            } else {
                out.push_back(inner + "if (U64 game_engine.'tc_delay_cycle_start'()) != 0 {");
                for (size_t i = group.first; i < group.second; ++i) {
                    const std::string one = trimmed(lines[i]);
                    if (!one.empty()) out.push_back(inner + "    " + one);
                }
                out.push_back(inner + "}");
            }
            cursor = group.second;
        }
        for (size_t i = cursor; i < to; ++i) out.push_back(lines[i]);
    }
    /* The game's cycle counter advances once per *cycle*, not once per unit:
       with stepping on, a pass is one unit, so an unguarded `cycle += 1` would
       both inflate the cycle number and end the burst (the condition above is
       `cycle < burst_target_cycle`). */
    for (std::string& line : out) {
        const std::string one = trimmed(line);
        if (one.rfind("cycle += 1", 0) != 0) continue;
        line = indentOf(line) +
               "if (U64 game_engine.'tc_delay_cycle_start'()) != 0 { cycle += 1 }";
    }
    if (stepped) out.insert(out.end() - 1, inner + "tc_pass_more = false");
    out.push_back(outer + "}");
    return out;
}

/* The identifier a `let`/`var` line declares, whether or not the line spells the
   type.  The emitter writes `let value_id256 = U1 <bridge call>` for a component's
   value and `let value_id256 = (U1 0x0)` for a constant one; both carry the slot
   number in the name, and the bridge replacement leaves only that line behind. */
inline std::string declaredName(const std::string& line) {
    const std::string text = trimmed(line);
    if (text.rfind("let ", 0) != 0 && text.rfind("var ", 0) != 0) return std::string();
    const size_t eq = text.find('=');
    if (eq == std::string::npos || eq <= 4) return std::string();
    size_t end = 4;
    while (end < eq && isIdentifierChar(text[end])) ++end;
    if (end == 4) return std::string();
    return text.substr(4, end - 4);
}

/* Every state slot one clock value is published into, given the value line's name.

   The emitter mirrors a component's output into the component's own node slot and
   into the slot of each wire hanging off it, and the board's shader draws whichever
   of them it walks first.  Marking only the primary slot left the drawn wire on the
   component's per-cycle value while every gate read the synthesized pulse, so the
   value line's own stores are marked as well - that is what makes drawing, wires
   and logic read one signal (see publishClockPulse). */
inline size_t markClockValueSlots(const std::vector<std::string>& lines,
                                  const std::string& valueName, unsigned valueBits) {
    const unsigned bits = valueBits ? valueBits : 1;
    size_t marked = 0;
    if (valueName.empty()) return 0;
    const size_t digits = valueName.find_first_of("0123456789");
    if (digits != std::string::npos) {
        const uint64_t slot = std::strtoull(valueName.c_str() + digits, nullptr, 10);
        if (clockSlots().emplace(slot, bits).second) ++marked;
    }
    for (const std::string& candidate : lines) {
        const size_t use = candidate.find(valueName);
        if (use == std::string::npos) continue;
        if (use && isIdentifierChar(candidate[use - 1])) continue;
        if (use + valueName.size() < candidate.size() &&
            isIdentifierChar(candidate[use + valueName.size()]))
            continue;
        const size_t storeAt = candidate.find("store(#SIMULATION_STATE + ");
        if (storeAt == std::string::npos) continue;
        uint64_t offset = 0;
        size_t valueStart = 0, valueEnd = 0;
        std::string suffix;
        if (!matchStore(candidate, storeAt, offset, valueStart, valueEnd, suffix)) continue;
        if (clockSlots().emplace(offset, bits).second) ++marked;
    }
    return marked;
}

/* Finds every cycle body (mode_refresh's body and the `while cycle <
   burst_target_cycle { ... }` region) and rewrites it.  Returns false when the
   source is not one of the game's simulation programs. */
inline bool rewrite(std::string& text, uint64_t unitsPerCycle,
                    const std::vector<Reclose>& reclosures = {}) {
    const std::string refresh = "def mode_refresh() None {";
    if (text.find(refresh) == std::string::npos) return false;
    std::vector<std::string> lines = splitLines(text);
    const std::vector<Body> bodies = findBodies(lines);
    if (bodies.empty()) return false;
    Slots slots;
    Runtime& stats = store();
    std::vector<std::string> problems;
    /* Clock sources: the emitter writes one `com_custom` block per output of the
       instance, and its node number is the value's state slot.  Marking it is
       what makes a read of that slot answer with the unit pulse. */
    clockSlots().clear();
    mirrors().clear();
    size_t marked = 0;
    /* The bridge call the native-logic emission left behind is where a clock's
       value line went; its variable name carries the slot. */
    if (!clockTokens().empty()) {
        for (const std::string& line : lines) {
            if (line.find("tc_logic_invoke") == std::string::npos &&
                line.find("tc_logic_peek") == std::string::npos)
                continue;
            const size_t at = line.find("'(U64 ");
            if (at == std::string::npos) continue;
            const uint64_t token = std::strtoull(line.c_str() + at + 6, nullptr, 10);
            if (!clockTokens().count(token)) continue;
            const std::string valueName = declaredName(line);
            if (valueName.empty()) continue;
            /* localDecl knows the width only when the line spells it; the slot is
               in the name either way, so the width is a bonus, not a filter. */
            unsigned valueBits = 1;
            std::string typed;
            unsigned width = 0;
            if (localDecl(line, typed, width) && typed == valueName) valueBits = width;
            marked += markClockValueSlots(lines, valueName, valueBits);
        }
    }
    /* The instance list has to come from the board being compiled: the compile's
       own preorder pass can run *after* this transform (measured on the clock
       board's first compile), so relying on that scan marked nothing. */
    if (enabled() && boardModel()) {
        clockInstances().clear();
        auto* board = static_cast<unsigned char*>(boardModel());
        scanClockInstances(board + 0x78, stats.log);
        scanClockInstances(board + 0x98, stats.log);
        noteTieGroups(board + 0x78, board + 0x98, stats.log);
    }
    /* Tie groups: the components are known, their state slots come from the
       blocks the emitter wrote for them. */
    tieGroups().clear();
    if (!tieGroupComponents().empty()) {
        for (const std::vector<int64_t>& componentsInGroup : tieGroupComponents()) {
            TieGroup group;
            for (int64_t id : componentsInGroup) {
                for (const Body& body : bodies) {
                    const CutBlock block = cutBlockFor(lines, body, id);
                    if (block.present && block.slot) {
                        group.slots.push_back(block.slot);
                        break;
                    }
                }
            }
            if (group.slots.size() >= 2) tieGroups().push_back(group);
        }
        if (!tieGroups().empty() && stats.log)
            stats.log("Gate delay: " + std::to_string(tieGroups().size()) +
                      " loop(s) hold a deterministic tie-break (board order wins the symmetric unit)");
    }
    if (!clockInstances().empty()) {
        for (const Body& body : bodies) {
            for (const BlockRange& block : blocksIn(lines, body)) {
                if (block.info.kind != "com_custom" || !block.hasId) continue;
                if (!clockInstances().count(static_cast<uint64_t>(block.id))) continue;
                /* An emitted block declares its own value first, and that name is
                   where the slot number lives.  A block whose value line the bridge
                   replacement left without a name falls back to the numbered slot
                   the node variable names. */
                std::string valueName;
                for (size_t j = block.header + 1; j < block.end && valueName.empty(); ++j)
                    valueName = declaredName(lines[j]);
                if (!valueName.empty()) {
                    uint64_t slot = 0;
                    const size_t digits = valueName.find_first_of("0123456789");
                    if (digits != std::string::npos)
                        slot = std::strtoull(valueName.c_str() + digits, nullptr, 10);
                    const unsigned width = slots.widthOf(
                        slot, block.info.bits ? block.info.bits : 1);
                    marked += markClockValueSlots(lines, valueName, width);
                    continue;
                }
                uint64_t slot = 0;
                if (!blockSlot(lines, block.header + 1, block.end, slot)) continue;
                if (clockSlots().emplace(slot, block.info.bits ? block.info.bits : 1).second) ++marked;
            }
        }
    }
    if (marked && stats.log)
        stats.log("Gate delay: " + std::to_string(marked) +
                  " clock slot(s) answer with a " + std::to_string(clockWidthUnits()) +
                  "-unit pulse");
    const size_t reclosed =
        reclosures.empty() ? 0 : applyReclosures(lines, bodies, reclosures, problems, stats.log);
    /* Connections the loader had to cut for the compile: their consumer's input
       carries a folded operand, which this puts back. */
    const size_t recut = applyCuts(lines, bodies, problems, stats.log);
    /* applyCuts/applyReclosures work on the split-line copy, so rejecting here
       leaves `text` byte-for-byte untouched.  Continuing used to compile a
       half-rewritten program; the game immediately retried it and locked the UI
       in a compile storm. */
    if (!problems.empty()) {
        if (stats.log) {
            stats.log("Gate delay: rewrite skipped because a cut connection could not be restored safely");
            for (size_t i = 0; i < problems.size() && i < 8; ++i)
                stats.log("Gate delay: unsupported - " + problems[i]);
        }
        finishCompileCut();
        return false;
    }
    /* Slot widths first: the refresh body's typed stores describe every node. */
    for (const std::string& line : lines) {
        size_t pos = 0;
        while ((pos = line.find("load(<U", pos)) != std::string::npos) {
            unsigned bits = 0;
            uint64_t offset = 0;
            size_t end = 0;
            if (!matchLoad(line, pos, bits, offset, end)) { ++pos; continue; }
            if (offset < kHostStateBase) slots.note(offset, bits);
            pos = end;
        }
        pos = 0;
        while ((pos = line.find("store(#SIMULATION_STATE + ", pos)) != std::string::npos) {
            uint64_t offset = 0;
            size_t valueStart = 0, valueEnd = 0;
            std::string suffix;
            if (!matchStore(line, pos, offset, valueStart, valueEnd, suffix)) { ++pos; continue; }
            if (offset < kHostStateBase) {
                const std::string value = trimmed(line.substr(valueStart, valueEnd - valueStart));
                if (value.rfind("U", 0) == 0) {
                    size_t cursor = 1;
                    uint64_t width = 0;
                    const bool tagged = readDigits(width, value, cursor);
                    if (tagged && (cursor >= value.size() || !isIdentifierChar(value[cursor])))
                        slots.note(offset, static_cast<unsigned>(width));
                }
            }
            pos = valueEnd + 1;
        }
    }
    BodyStats bodyStats{};
    bool touched = false;
    /* Back to front, so rewriting one body leaves the earlier bodies' indices
       valid. */
    for (size_t i = bodies.size(); i-- > 0;) {
        const Body& body = bodies[i];
        const std::vector<std::string> current(lines.begin() + body.from, lines.begin() + body.to);
        std::vector<std::string> replacement =
            rewriteBody(lines, body.from, body.to, unitsPerCycle, slots, body.slotBody, stats, bodyStats,
                        problems);
        if (replacement == current) continue;                 /* nothing to do */
        lines.erase(lines.begin() + body.from, lines.begin() + body.to);
        lines.insert(lines.begin() + body.from, replacement.begin(), replacement.end());
        touched = true;
    }
    if (!touched && !reclosed && !recut) return false;
    stats.stateSize = std::max<size_t>(slots.maxOffset + 8, 64);
    stats.scratch.assign(stats.stateSize, 0);
    /* Event scheduling (M1b) keeps a dirty byte per state slot; a fresh start
       evaluates everything once, which is what the reset below arranges. */
    stats.eventSchedule = eventSchedule();
    if (stats.eventSchedule) {
        stats.dirty.assign(stats.stateSize, 1);
        stats.dirtyNow.assign(stats.stateSize, 0);
    } else {
        stats.dirty.clear();
        stats.dirtyNow.clear();
    }
    stats.unitsPerCycle = unitsPerCycle;
    if (stats.log) {
        stats.log("Gate delay: rewrite done; wave=" + std::to_string(bodyStats.wave) +
                  " boundary=" + std::to_string(bodyStats.boundary) +
                  " publish=" + std::to_string(bodyStats.publishes) +
                  " guarded=" + std::to_string(bodyStats.guarded) +
                  " source=" + std::to_string(bodyStats.unconditional) +
                  " reclose=" + std::to_string(reclosed) +
                  " recut=" + std::to_string(recut) +
                  " slots=" + std::to_string(slots.bits.size()) +
                  " peak=" + std::to_string(slots.maxOffset) +
                  " scratch=" + std::to_string(stats.stateSize) + " bytes, sched=" +
                  (stats.eventSchedule ? "event" : "layered") + ", K=" +
                  std::to_string(unitsPerCycle) + ", unit step=" +
                  (unitsPerPassOverride()
                       ? "1 per pass" + std::string(unitPaceMs() ? " paced at " +
                                                       std::to_string(unitPaceMs()) + " ms/unit"
                                                 : " at the frame rate") +
                             " (one cycle = K passes)"
                       : "off (one pass = one cycle)") +
                  ", clock=" +
                  (clockFlipsPerCycle()
                       ? std::string("one square wave (flips once per cycle)")
                       : std::to_string(clockWidthUnits()) + " of " +
                             std::to_string(unitsPerCycle) + "-unit pulse"));
        for (size_t i = 0; i < problems.size() && i < 8; ++i)
            stats.log("Gate delay: unsupported - " + problems[i]);
    }
    std::string joined;
    for (const std::string& line : lines) { joined += line; joined += '\n'; }
    text = joined;
    return true;
}

/* ---- the switch, and the level it applies to -----------------------------

   The mode is compiled in - the loader rewrites the generated program - so the
   player's setting only has to reach the loader: a checkbox the loader appends
   to the game's own Options page, a one-line config file next to the log, and
   the level name the loader already sees on every load.  The mode is
   sandbox-only by construction (plan section 2), which is why the switch is
   ANDed with the level rather than trusted on its own: a campaign level must
   behave exactly as it would without this Mod, whatever the switch says. */
inline bool& switchState() {
    static bool on = [] {
        const char* value = std::getenv("TC_GATE_DELAY");
        return value && value[0] && value[0] != '0';
    }();
    return on;
}

inline void setSwitch(bool on) { switchState() = on; }

inline std::string& loadedLevel() {
    static std::string name;
    return name;
}

/* The game hands the loader the level's name on every load; the sandbox is
   `campaign/sandbox`, and matching on the name keeps this working when the same
   level is entered from a different menu. */
inline void setLoadedLevel(const char* name) {
    /* A level transition starts a new compile generation.  Never let a record
       from the previous board name component ids in the new one. */
    finishCompileCut();
    loadedLevel() = name ? name : "";
}

inline bool sandboxLevel() { return loadedLevel().find("sandbox") != std::string::npos; }

inline bool enabled() { return switchState() && sandboxLevel(); }

inline uint64_t unitsPerCycle() {
    static const uint64_t units = [] {
        const char* value = std::getenv("TC_GATE_DELAY_K");
        const uint64_t parsed = value ? std::strtoull(value, nullptr, 10) : 0;
        return parsed ? parsed : kDefaultUnitsPerCycle;
    }();
    return units;
}

/* Event scheduling is the second implementation of the same model (the plan's
   T-b): blocks run when an input moved instead of every unit.  Off by default -
   the layered pass is the one M2 is verified with - and switched on with
   TC_GATE_DELAY_SCHED=event. */
inline bool eventSchedule() {
    /* Not cached: the transform runs once per compile, and a test needs to be
       able to switch the pass on and off inside one process. */
    const char* value = std::getenv("TC_GATE_DELAY_SCHED");
    return value && std::string(value).find("event") != std::string::npos;
}

/* Unit-level trace, for the M2 investigation only (TC_GATE_DELAY_TRACE=1): the
   first few units print what the state array holds and what this unit's snapshot
   holds, which is what tells "our publish never landed" from "something else
   wrote the slot back". */
inline bool traceEnabled() {
    static const bool on = [] {
        const char* value = std::getenv("TC_GATE_DELAY_TRACE");
        return value && value[0] && value[0] != '0';
    }();
    return on;
}

inline void traceUnit(uint64_t unit) {
    auto& rt = store();
    if (!traceEnabled() || !rt.log || unit > 16) return;
    std::string committed, snapshot;
    unsigned char* state = rt.base();
    for (uint64_t at = 256; at < 288; ++at) {
        committed += std::to_string(state ? static_cast<int>(state[at]) : -1) + (at == 287 ? "" : ",");
        snapshot += std::to_string(at < rt.scratch.size() ? static_cast<int>(rt.scratch[at]) : -2) +
                    (at == 287 ? "" : ",");
    }
    rt.log("Gate delay trace: unit=" + std::to_string(unit) + " state=" + committed +
           " snapshot=" + snapshot + " pending=" + std::to_string(rt.pending.size()));
}

/* `TC_GATE_DELAY_TRACEALL=1` logs one line per unit, which is what tells "the
   board's own repaint advances the simulation" from "the simulation advances and
   the board shows it": the site that ran, whether it owned the unit clock, the
   unit index inside the cycle, the pulse and the clock slot as it stands after
   this unit's publish.  Diagnostic only - a normal run does not print it. */
inline void traceEveryUnit(uint64_t unit) {
    auto& rt = store();
    if (!rt.log) return;
    static const bool on = [] {
        const char* value = std::getenv("TC_GATE_DELAY_TRACEALL");
        return value && value[0] && value[0] != '0';
    }();
    if (!on || unit > 4000) return;
    unsigned char* state = rt.base();
    std::string window;
    for (uint64_t at = 256; at < 288; ++at)
        window += std::to_string(state ? static_cast<int>(state[at]) : -1) + (at == 287 ? "" : ",");
    rt.log("Gate delay unit: n=" + std::to_string(unit) +
           " ms=" + std::to_string(static_cast<uint64_t>(GetTickCount64())) +
           " site=" + std::to_string(rt.currentSite) +
           " owner=" + (rt.currentSiteActive ? "1" : "0") +
           " t=" + std::to_string(rt.time) +
           " u=" + std::to_string(unitInCycle()) +
           " pulse=" + std::to_string(clockPulse()) +
           " cycleStart=" + (rt.currentCycleStart ? "1" : "0") +
           " slot256=" + std::to_string(state ? static_cast<int>(state[256]) : -1) +
           " slot258=" + std::to_string(state ? static_cast<int>(state[258]) : -1) +
           " state[256..287]=" + window);
}

}  // namespace tc::gate_delay
