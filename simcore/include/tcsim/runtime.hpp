#pragma once
/* S2's runtime core (docs/PLAN-sandbox-simulator.md sections 4.6 and 5): the
   piece that turns "a netlist exists" into "the board follows our simulator".

   It owns exactly three things and nothing about the game:

     * the cycle clock - one cycle is `tick_per_unit * cycle_units` ticks, the
       plan's "周期只是换算点";
     * the source side - the nets the game's state array drives (an Input, a
       Switch) are read every cycle and fed to the engine as stimulus;
     * the write-back side - every net's bytes (the wire's own state offset plus
       its neighbour, measured on a real board) are published after the engine
       has settled.

   Ordering is the part the real machine taught us: the game's own program
   writes those same bytes during its step, so the caller must publish *after*
   that step - `stepCycle()` is meant to be called once per game cycle, once the
   game has finished the cycle.  `publish()` is separated from `stepCycle()` so
   a caller can observe the program's value first, which is also how the
   ownership test proves the bytes show our value and not the game's. */

#include "board.hpp"
#include "engine.hpp"
#include "netlist.hpp"
#include "time.hpp"
#include "value.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tcsim {

struct RuntimeConfig {
    /* One game unit in ticks, and how many units a cycle is.  The engine has no
       notion of a cycle; this is where it comes from. */
    Tick ticks_per_unit = kTicksPerUnit;
    Tick cycle_units = kDefaultCycleUnits;
    ArcDelay gate_delay{8, 8};
    bool rotate_pins = true;
    bool include_neighbour_slot = true;
    bool trace = false;
    /* A register's clock-to-q and a gate's delay come from the timing library in
       S5; until a library is attached every device gets `gate_delay`. */
    Tick cycleTicks() const { return ticks_per_unit * (cycle_units ? cycle_units : 1); }
};

/* What the caller has to provide for one cycle: the value on every source net,
   which is a byte in the game's state array. */
using SourceReader = std::function<uint64_t(NetId, const std::vector<uint64_t>& slots)>;

class SandboxRuntime {
public:
    SandboxRuntime() = default;

    /* Builds the netlist and an engine over it.  Returns false, with the reason
       in `notes`, when the board cannot be run honestly (an unsupported device,
       a pin count that disagrees with the prototype). */
    bool bind(const BoardView& board,
              const std::function<bool(const BoardComponent&, KindPins&)>& geometry,
              const RuntimeConfig& config = RuntimeConfig()) {
        config_ = config;
        BuildOptions options;
        options.rotate_pins = config.rotate_pins;
        options.include_neighbour_slot = config.include_neighbour_slot;
        options.gate_delay = config.gate_delay;
        build_ = buildNetList(board, geometry, options);
        for (const BuildNote& note : build_.notes) {
            if (note.fatal) notes_.push_back(note.text);
        }
        if (build_.fatal()) return false;
        EngineOptions engine_options;
        engine_options.trace = config.trace;
        engine_.emplace(build_.netlist, engine_options);
        bound_ = true;
        cycles_ = 0;
        seeded_ = false;
        return true;
    }

    bool bound() const { return bound_; }
    BuildResult& build() { return build_; }
    NetList& netlist() { return build_.netlist; }
    Engine& engine() { return *engine_; }
    const Engine& engine() const { return *engine_; }
    const std::vector<std::string>& notes() const { return notes_; }
    uint64_t cycles() const { return cycles_; }
    Tick now() const { return engine_->now(); }

    const std::vector<uint64_t>& slotsOf(NetId net) const { return build_.slots_of_net[net]; }
    const std::vector<NetId>& sourceNets() const { return build_.source_nets; }
    const std::vector<NetId>& observedNets() const { return build_.observed_nets; }
    const std::vector<NetId>& clockNets() const { return build_.clock_nets; }

    /* Value the board should show for a net: the engine's bit, or the hold value
       when the engine has it unknown.  `unknownIsZero` is the game-facing
       mapping the plan asks for in section 4.1 (X reads as 0 on the board until
       S4 gives it a visible mark). */
    uint64_t boardByte(NetId net, bool unknown_is_zero = true) const {
        const BitVector& value = engine_->netValue(net);
        if (!value.size()) return unknown_is_zero ? 0 : 1;
        if (value[0] == Logic::kOne) return 1;
        if (value[0] == Logic::kZero) return 0;
        return unknown_is_zero ? 0 : 1;
    }

    /* Advances `ticks` of simulated time.  The source nets are fed from the
       game's array, and the clock nets take the value of the cycle that starts
       there, at every cycle boundary the step crosses.  A step of a whole cycle
       is therefore exactly the old behaviour; a smaller step is what makes the
       inside of a cycle observable (an edge-detector pulse is one gate delay
       wide, far shorter than the 8192-tick cycle, and publishing only at cycle
       boundaries hid it completely). */
    bool advance(Tick ticks, const SourceReader& read_source) {
        if (!bound_ || ticks <= 0) return false;
        Tick remaining = ticks;
        bool settled = true;
        while (remaining > 0) {
            const Tick cycle = config_.cycleTicks();
            const Tick now = engine_->now();
            const Tick phase = cycle > 0 ? ((now % cycle) + cycle) % cycle : 0;
            const Tick to_boundary = phase == 0 ? (cycle > 0 ? cycle : remaining) : (cycle - phase);
            const Tick step = remaining < to_boundary ? remaining : to_boundary;
            if (phase == 0) {
                for (NetId net : build_.source_nets) {
                    const uint64_t value = read_source(net, build_.slots_of_net[net]);
                    engine_->addStimulus(now, net,
                                         BitVector(1, (value & 1u) ? Logic::kOne : Logic::kZero));
                }
                for (NetId net : build_.clock_nets) {
                    const Logic value = (cycles_ & 1u) == 0 ? Logic::kOne : Logic::kZero;
                    engine_->addStimulus(now, net, BitVector(1, value));
                }
            }
            if (!engine_->runUntil(now + step)) settled = false;
            remaining -= step;
            if (step == to_boundary) ++cycles_;
        }
        return settled;
    }

    /* One game cycle in a single step (the default cadence). */
    bool stepCycle(const SourceReader& read_source) {
        return advance(config_.cycleTicks(), read_source);
    }
    /* How many ticks one call of `advance` should cover to publish `steps` times
       per cycle.  A steps value of 1 (or less) means one publish per cycle. */
    Tick stepTicksFor(int steps) const {
        const Tick cycle = config_.cycleTicks();
        if (steps <= 1 || cycle <= 1) return cycle;
        const Tick step = cycle / static_cast<Tick>(steps);
        return step > 0 ? step : 1;
    }

    /* Break the all-X fixed point of an un-driven feedback network after its
       first evaluation pass.  Only device-driven, non-source nets qualify and
       the default limit of one introduces the smallest possible asymmetry.
       This operation is explicit because a four-state simulator is correct to
       leave an uninitialised ring at X unless the user asks for a power-on
       seed. */
    size_t seedUnknownDrivenNets(Logic value = Logic::kZero, size_t limit = 1) {
        if (!bound_ || seeded_ || cycles_ == 0 || limit == 0) return 0;
        seeded_ = true;
        size_t count = 0;
        for (size_t index = 0; index < build_.netlist.netCount() && count < limit; ++index) {
            const NetId net = static_cast<NetId>(index);
            const Net& candidate = engine_->netlist().net(net);
            if (candidate.drivers.empty()) continue;
            if (std::find(build_.source_nets.begin(), build_.source_nets.end(), net) !=
                build_.source_nets.end())
                continue;
            bool unknown = false;
            for (size_t bit = 0; bit < candidate.value.size(); ++bit) {
                if (candidate.value[bit] == Logic::kX || candidate.value[bit] == Logic::kZ) {
                    unknown = true;
                    break;
                }
            }
            if (!unknown) continue;
            if (engine_->seedNet(net, BitVector(candidate.bits, value))) ++count;
        }
        return count;
    }

    /* Publishes every net's bytes into the game's state array.  `write` takes
       (offset, byte); the caller owns the memory.  Returns the number of bytes
       written, and `invert_test_only` exists so a real-machine test can write
       the opposite value and prove the board shows ours. */
    size_t publish(const std::function<void(uint64_t, unsigned char)>& write,
                   bool invert_test_only = false) const {
        if (!bound_) return 0;
        size_t written = 0;
        for (size_t index = 0; index < build_.netlist.netCount(); ++index) {
            const NetId net = static_cast<NetId>(index);
            uint64_t value = boardByte(net);
            const bool is_source =
                std::find(build_.source_nets.begin(), build_.source_nets.end(), net) != build_.source_nets.end();
            if (invert_test_only && !is_source) value ^= 1u;
            for (uint64_t slot : build_.slots_of_net[net]) {
                write(slot, static_cast<unsigned char>(value & 1u));
                ++written;
            }
        }
        return written;
    }

    /* A one-line summary for a log or a test report. */
    std::string describe() const {
        std::string text = "nets=" + std::to_string(build_.netlist.netCount()) +
                           " devices=" + std::to_string(build_.netlist.deviceCount()) +
                           " sources=" + std::to_string(build_.source_nets.size()) +
                           " observed=" + std::to_string(build_.observed_nets.size()) +
                           " cycles=" + std::to_string(cycles_);
        if (engine_->aborted()) text += " aborted=1";
        if (!notes_.empty()) text += " notes=" + std::to_string(notes_.size());
        return text;
    }

private:
    BuildResult build_;
    std::optional<Engine> engine_;
    RuntimeConfig config_;
    std::vector<std::string> notes_;
    uint64_t cycles_ = 0;
    bool bound_ = false;
    bool seeded_ = false;
};

}  // namespace tcsim
