#pragma once
/* The simulator itself: netlist + scheduler + delay model, driven event by
   event (docs/PLAN-sandbox-simulator.md sections 3 and 4).

   The order inside one time step is the Verilog one:

     1. active   - combinational devices evaluate and schedule their outputs;
     2. NBA      - the updates those evaluations scheduled land, and the nets
                   they touch re-resolve;
     3. monitor  - observers and the hold-time checks that look *after* an edge.

   Every event created during a round is delivered in a later round, never
   inside the round that created it.  That is the delta ring: a zero-delay loop
   converges round by round, and `max_delta_rounds` turns a zero-delay
   oscillator into the violation a real simulator reports instead of a hang.
   A register's output lands in the NBA layer, so nothing in the same time step
   can read the value it is about to drive - the semantics of
   `always @(posedge clk) q <= d`. */

#include "devices.hpp"
#include "netlist.hpp"
#include "scheduler.hpp"
#include "time.hpp"
#include "value.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tcsim {

/* An external driver on a net.  The game is the source in tcsim; in the unit
   tests and the cross-check it is the Verilog testbench's `initial` block. */
struct StimulusEntry {
    Tick when = 0;
    NetId net = kNoNet;
    BitVector value;
    Strength strength = Strength::kStrong;
};

enum class ViolationKind : uint8_t {
    kZeroDelayOscillation = 0,
    kMultiDriverConflict = 1,
    kUndrivenNet = 2,
    kSetupViolation = 3,
    kHoldViolation = 4,
    kPastSchedule = 5,
    kIncompleteDevice = 6,
};

inline const char* violationName(ViolationKind kind) {
    switch (kind) {
        case ViolationKind::kZeroDelayOscillation: return "zero-delay-oscillation";
        case ViolationKind::kMultiDriverConflict: return "multi-driver-conflict";
        case ViolationKind::kUndrivenNet: return "undriven-net";
        case ViolationKind::kSetupViolation: return "setup-violation";
        case ViolationKind::kHoldViolation: return "hold-violation";
        case ViolationKind::kPastSchedule: return "past-schedule";
        case ViolationKind::kIncompleteDevice: return "incomplete-device";
    }
    return "violation";
}

struct Violation {
    ViolationKind kind = ViolationKind::kUndrivenNet;
    Tick when = 0;
    NetId net = kNoNet;
    DeviceId device = kNoDevice;
    std::string detail;
};

struct TraceRecord {
    Tick when = 0;
    NetId net = kNoNet;
    BitVector value;
    Logic drive = Logic::kZ;
    Strength strength = Strength::kStrong;
    DeviceId device = kNoDevice;
    uint16_t pin = 0;
    bool initial = false;
};

struct EngineOptions {
    uint32_t max_delta_rounds = 1000;
    bool trace = false;
    size_t trace_capacity = 1u << 20;
    bool report_multi_driver_conflicts = true;
    bool report_undriven_nets = true;
    bool report_incomplete_devices = true;
};

namespace engine_detail {
enum class EventKind : uint32_t { kEvaluate = 0, kPinUpdate = 1, kStimulus = 2, kHoldCheck = 3 };

/* Copies `value` onto a pin of `bits` bits: the low bits line up, the missing
   high bits take the source's own leftmost bit, the way a wider net feeding a
   narrower pin behaves in the game's pin geometry. */
inline BitVector fitVector(const BitVector& value, size_t bits) {
    if (value.size() == bits) return value;
    BitVector out(bits, Logic::kX);
    const Logic fill = value.size() ? value[0] : Logic::kX;
    for (size_t bit = 0; bit < bits; ++bit) {
        if (bit < value.size()) {
            out.at(bits - 1 - bit) = value[value.size() - 1 - bit];
        } else {
            out.at(bits - 1 - bit) = fill;
        }
    }
    return out;
}
}  // namespace engine_detail

class Engine {
public:
    explicit Engine(NetList netlist, const EngineOptions& options = EngineOptions())
        : netlist_(std::move(netlist)), options_(options) {
        const Tick longest = netlist_.longestDelay();
        Tick span = 16;
        while (span < longest + 2) span <<= 1;
        scheduler_ = Scheduler(span);
        unconnected_.assign(netlist_.devices().size(), false);
    }

    NetList& netlist() { return netlist_; }
    const NetList& netlist() const { return netlist_; }
    const std::vector<StimulusEntry>& stimulus() const { return stimulus_; }
    const std::vector<TraceRecord>& trace() const { return trace_; }
    const std::vector<Violation>& violations() const { return violations_; }
    Scheduler& scheduler() { return scheduler_; }
    Tick now() const { return now_; }
    bool aborted() const { return aborted_; }
    uint64_t evaluations() const { return evaluations_; }
    uint64_t deltaRounds() const { return delta_rounds_; }
    uint64_t staleUpdates() const { return stale_updates_; }
    uint64_t droppedTraces() const { return dropped_traces_; }
    void setTrace(bool on) {
        options_.trace = on;
        if (!on) trace_.clear();
    }

    /* An external driver.  Several entries on one net are applied in time
       order; the value the engine reports is the resolved bus value. */
    bool addStimulus(Tick when, NetId net, const BitVector& value, Strength strength = Strength::kStrong) {
        if (net >= netlist_.nets().size()) return false;
        if (when < now_) return false;
        stimulus_.push_back(StimulusEntry{when, net, value, strength});
        /* After the first run the initial pass has already happened, so a new
           driver has to be queued here or it would only be recorded in the
           bookkeeping and never take effect (this is how a runtime feeds the
           game's array into the engine every cycle). */
        if (initialized_) {
            Event event;
            event.when = when;
            event.layer = Layer::kActive;
            event.kind = static_cast<uint32_t>(engine_detail::EventKind::kStimulus);
            event.device = net;
            event.value = value;
            event.strength = strength;
            scheduler_.push(event);
        }
        return true;
    }
    bool addStimulus(Tick when, const std::string& net, const std::string& value,
                     Strength strength = Strength::kStrong) {
        const NetId id = netlist_.netId(net);
        if (id == kNoNet) return false;
        return addStimulus(when, id, BitVector::fromText(value, netlist_.net(id).bits), strength);
    }

    /* Runs every event up to and including `target`.  Between events the state
       is settled, so reading a net afterwards returns what the circuit holds at
       `target`.  Returns false when a guard tripped. */
    bool runUntil(Tick target) {
        if (!initialized_) initialize();
        while (!aborted_ && !scheduler_.empty() && scheduler_.nextWhen() <= target) {
            const Tick when = scheduler_.nextWhen();
            processTime(when);
            if (when == 0 && options_.trace && !initial_trace_done_) recordInitialTrace();
        }
        if (options_.trace && !initial_trace_done_) recordInitialTrace();
        if (target > now_) now_ = target;
        reportSettledNets();
        checkScheduler();
        return !aborted_;
    }

    /* Give an already-initialised, unresolved feedback network a deterministic
       starting point.  This is deliberately not an external driver: the next
       real device update resolves the net normally and completely replaces the
       seed.  A permanent stimulus here would fight the gate forever and turn a
       useful startup aid into different circuit semantics.

       The caller must first run the t=0 evaluation pass.  That restriction is
       important: initialize() resolves every device-driven net to X, so a seed
       applied before it would be silently erased. */
    bool seedNet(NetId net, const BitVector& value) {
        if (!initialized_ || aborted_ || net >= netlist_.nets().size()) return false;
        Net& target = netlist_.net(net);
        const BitVector fitted = engine_detail::fitVector(value, target.bits);
        if (fitted == target.value) return true;
        target.previous = target.value;
        target.value = fitted;
        ++target.changes;
        target.last_change = now_;
        recordTrace(target, kNoDevice, 0, false);
        enqueueLoads(net);
        return true;
    }

    const BitVector& netValue(NetId net) const { return netlist_.net(net).value; }

    /* Nets that carry a load but have nothing driving them: a hole in the
       board, reported rather than quietly read as Z. */
    std::vector<NetId> undrivenNets() const {
        std::vector<NetId> found;
        for (size_t index = 0; index < netlist_.nets().size(); ++index) {
            const Net& net = netlist_.nets()[index];
            if (net.loads.empty()) continue;
            if (!net.drivers.empty() || net.stimulus_active) continue;
            found.push_back(static_cast<NetId>(index));
        }
        return found;
    }

    const std::string& netName(NetId net) const { return netlist_.net(net).name; }

    /* Looks a net name up and returns its current value; Z when the name is
       unknown, which the caller must not mistake for a driven low. */
    BitVector value(const std::string& name) const {
        const NetId id = netlist_.netId(name);
        return id == kNoNet ? BitVector(1, Logic::kZ) : netlist_.net(id).value;
    }

private:
    void initialize() {
        initialized_ = true;
        /* A net an external driver will write starts unknown, the way a reg
           starts x, rather than floating until its first assignment. */
        for (const StimulusEntry& entry : stimulus_) {
            if (entry.net >= netlist_.nets().size()) continue;
            Net& net = netlist_.net(entry.net);
            if (net.stimulus_active) continue;
            net.stimulus_active = true;
            net.stimulus_logic = Logic::kX;
            net.stimulus_strength = Strength::kStrong;
        }
        /* One resolution pass before anything runs, so a net driven by a device
           that has not evaluated yet reads X - exactly what iverilog reports
           for a wire whose gate has not driven it, and what $dumpvars prints. */
        for (size_t index = 0; index < netlist_.nets().size(); ++index) {
            resolveNet(static_cast<NetId>(index), kNoDevice, 0);
        }
        for (const StimulusEntry& entry : stimulus_) {
            Event event;
            event.when = entry.when;
            event.layer = Layer::kActive;
            event.kind = static_cast<uint32_t>(engine_detail::EventKind::kStimulus);
            event.device = entry.net; /* stimulus events carry the net in `device` */
            event.value = entry.value;
            event.strength = entry.strength;
            scheduler_.push(event);
        }
        /* Every device gets one pass at t = 0, exactly like a gate primitive
           that drives its output once before anything else happens. */
        for (size_t index = 0; index < netlist_.devices().size(); ++index) {
            Event event;
            event.when = 0;
            event.layer = Layer::kActive;
            event.kind = static_cast<uint32_t>(engine_detail::EventKind::kEvaluate);
            event.device = static_cast<uint32_t>(index);
            event.pin = 0xFFFFu;
            scheduler_.push(event);
        }
        now_ = 0;
    }

    void processTime(Tick when) {
        now_ = when;
        pull(when);
        uint32_t rounds = 0;
        while (!layers_[0].empty() || !layers_[1].empty() || !layers_[2].empty()) {
            if (++rounds > options_.max_delta_rounds) {
                Violation violation;
                violation.kind = ViolationKind::kZeroDelayOscillation;
                violation.when = when;
                violation.detail = "more than " + std::to_string(options_.max_delta_rounds) +
                                   " delta rounds at one time step";
                violations_.push_back(violation);
                aborted_ = true;
                return;
            }
            ++delta_rounds_;
            runLayer(0);
            if (aborted_) return;
            runLayer(1);
            if (aborted_) return;
            runLayer(2);
            if (aborted_) return;
            pull(when);
        }
    }

    void pull(Tick when) {
        while (!scheduler_.empty() && scheduler_.nextWhen() == when) {
            Event event = scheduler_.pop();
            layers_[static_cast<size_t>(event.layer)].push_back(std::move(event));
        }
    }

    void runLayer(size_t layer) {
        if (layers_[layer].empty()) return;
        std::vector<Event> batch;
        batch.swap(layers_[layer]);
        for (Event& event : batch) {
            switch (static_cast<engine_detail::EventKind>(event.kind)) {
                case engine_detail::EventKind::kEvaluate: evaluateDevice(event); break;
                case engine_detail::EventKind::kPinUpdate: applyPinUpdate(event); break;
                case engine_detail::EventKind::kStimulus: applyStimulus(event); break;
                case engine_detail::EventKind::kHoldCheck: checkHold(event); break;
            }
            if (aborted_) return;
        }
    }

    static Logic firstBit(const BitVector& value) { return value.size() ? value[0] : Logic::kZ; }

    DeviceId deviceIdOf(const DeviceInstance& device) const {
        return static_cast<DeviceId>(&device - netlist_.devices().data());
    }

    void evaluateDevice(const Event& event) {
        if (event.device >= netlist_.devices().size()) return;
        DeviceInstance& device = netlist_.device(event.device);
        const DeviceSpec& spec = *device.spec;
        if (device.inputs.size() != spec.inputs.size() || device.outputs.size() != spec.outputs.size()) return;
        bool connected = true;
        for (NetId net : device.inputs) {
            if (net == kNoNet) connected = false;
        }
        for (NetId net : device.outputs) {
            if (net == kNoNet) connected = false;
        }
        if (!connected) {
            if (options_.report_incomplete_devices && !unconnected_[event.device]) {
                unconnected_[event.device] = true;
                Violation violation;
                violation.kind = ViolationKind::kIncompleteDevice;
                violation.when = now_;
                violation.device = event.device;
                violation.detail = "device " + device.name + " has an unconnected pin";
                violations_.push_back(violation);
            }
            return;
        }

        EvalContext context;
        context.now = now_;
        ++device.evaluations;
        context.evaluations = device.evaluations;
        ++evaluations_;

        const int cause = event.pin < spec.inputs.size() ? static_cast<int>(event.pin) : -1;
        context.cause_pin = cause;
        if (cause >= 0) {
            const BitVector& previous = device.input_values[static_cast<size_t>(cause)];
            context.cause_from = previous.size() ? previous[0] : Logic::kX;
            context.cause_at = device.input_change[static_cast<size_t>(cause)];
        }

        std::vector<BitVector> inputs(spec.inputs.size());
        for (size_t pin = 0; pin < device.inputs.size(); ++pin) {
            inputs[pin] = engine_detail::fitVector(netlist_.net(device.inputs[pin]).value, spec.inputs[pin].bits);
        }
        if (cause >= 0) {
            const BitVector& current = inputs[static_cast<size_t>(cause)];
            context.cause_to = current.size() ? current[0] : Logic::kX;
        }
        device.input_values = inputs;

        std::vector<BitVector> outputs;
        if (spec.behavior) spec.behavior(context, inputs, outputs);
        if (outputs.size() != spec.outputs.size()) outputs.resize(spec.outputs.size(), BitVector(1, Logic::kX));

        if (spec.kind == DeviceKind::kSequential) {
            if (cause != spec.clock_pin) return; /* a data change waits for the clock */
            const Logic clock = inputs[static_cast<size_t>(spec.clock_pin)][0];
            if (spec.clock_edge == EdgeKind::kPosEdge && clock != Logic::kOne) return;
            if (spec.clock_edge == EdgeKind::kNegEdge && clock != Logic::kZero) return;
            device.last_clock_edge = now_;
            const bool violated = checkSetup(device, spec);
            if (violated && spec.x_on_violation) {
                for (BitVector& output : outputs) output.fill(Logic::kX);
            }
        }

        for (size_t pin = 0; pin < spec.outputs.size(); ++pin) {
            const BitVector value = engine_detail::fitVector(outputs[pin], spec.outputs[pin].bits);
            const ArcDelay delay =
                spec.kind == DeviceKind::kSequential
                    ? spec.clock_to_q
                    : spec.delayFor(static_cast<uint16_t>(cause < 0 ? 0 : cause), static_cast<uint16_t>(pin));
            const Tick when = now_ + delay.forTransition(firstBit(value));
            const Layer layer = spec.kind == DeviceKind::kSequential ? Layer::kNBA : Layer::kActive;
            scheduleOutput(event.device, static_cast<uint16_t>(pin), value, when, layer);
        }
    }

    bool checkSetup(DeviceInstance& device, const DeviceSpec& spec) {
        bool violated = false;
        for (size_t pin = 0; pin < spec.inputs.size(); ++pin) {
            if (static_cast<int>(pin) == spec.clock_pin) continue;
            const Tick changed = device.input_change[pin];
            if (changed < 0 || changed >= now_) continue;
            const Logic from = device.input_previous[pin].size() ? device.input_previous[pin][0] : Logic::kX;
            const Tick limit = from == Logic::kZero ? spec.setup.tphl : spec.setup.tplh;
            if (limit > 0 && now_ - changed < limit) {
                ++device.setup_violations;
                Violation violation;
                violation.kind = ViolationKind::kSetupViolation;
                violation.when = now_;
                violation.device = deviceIdOf(device);
                violation.detail = "data changed " + std::to_string(now_ - changed) +
                                   " ticks before the edge, limit " + std::to_string(limit);
                violations_.push_back(violation);
                violated = true;
            }
            if (spec.hold.tplh > 0 || spec.hold.tphl > 0) {
                Event event;
                event.when = now_ + (spec.hold.tplh > spec.hold.tphl ? spec.hold.tplh : spec.hold.tphl);
                event.layer = Layer::kMonitor;
                event.kind = static_cast<uint32_t>(engine_detail::EventKind::kHoldCheck);
                event.device = deviceIdOf(device);
                event.pin = static_cast<uint16_t>(pin);
                event.token = static_cast<uint64_t>(now_);
                scheduler_.push(event);
            }
        }
        return violated;
    }

    void checkHold(const Event& event) {
        if (event.device >= netlist_.devices().size()) return;
        DeviceInstance& device = netlist_.device(event.device);
        if (event.pin >= device.input_change.size()) return;
        const Tick changed = device.input_change[event.pin];
        if (changed <= static_cast<Tick>(event.token)) return; /* nothing moved after the edge */
        const ArcDelay& hold = device.spec->hold;
        const Tick limit = hold.tplh > hold.tphl ? hold.tplh : hold.tphl;
        if (limit <= 0) return;
        ++device.hold_violations;
        Violation violation;
        violation.kind = ViolationKind::kHoldViolation;
        violation.when = now_;
        violation.device = event.device;
        violation.detail = "data changed " + std::to_string(changed - static_cast<Tick>(event.token)) +
                           " ticks after the edge, hold limit " + std::to_string(limit);
        violations_.push_back(violation);
    }

    void scheduleOutput(DeviceId id, uint16_t pin, const BitVector& value, Tick when, Layer layer) {
        DeviceInstance& device = netlist_.device(id);
        PinState& state = device.output_state[pin];
        if (state.value.size() != value.size()) state.value = engine_detail::fitVector(state.value, value.size());
        if (device.spec->delay_kind == DelayKind::kInertial) {
            if (state.value == value) {
                /* Back to the value the net already sees: an inertial delay
                   swallows the pulse that never made it out (`assign #d`). */
                if (state.has_pending) {
                    state.has_pending = false;
                    state.token = ++token_source_;
                }
                return;
            }
            if (state.has_pending && state.pending_value == value) return; /* keep the timer */
            state.has_pending = true;
            state.pending_value = value;
            state.pending_when = when;
            state.token = ++token_source_;
            Event event;
            event.when = when;
            event.layer = layer;
            event.kind = static_cast<uint32_t>(engine_detail::EventKind::kPinUpdate);
            event.device = id;
            event.pin = pin;
            event.token = state.token;
            event.value = value;
            scheduler_.push(event);
            return;
        }
        /* Transport: every transition travels on its own, so a narrow pulse
           arrives at the far end, late. */
        Event event;
        event.when = when;
        event.layer = layer;
        event.kind = static_cast<uint32_t>(engine_detail::EventKind::kPinUpdate);
        event.device = id;
        event.pin = pin;
        event.token = ++token_source_;
        event.value = value;
        scheduler_.push(event);
    }

    void applyPinUpdate(const Event& event) {
        if (event.device >= netlist_.devices().size()) return;
        DeviceInstance& device = netlist_.device(event.device);
        if (event.pin >= device.output_state.size()) return;
        PinState& state = device.output_state[event.pin];
        if (device.spec->delay_kind == DelayKind::kInertial) {
            if (event.token != state.token) {
                ++stale_updates_; /* cancelled by a later evaluation: inertial */
                return;
            }
            state.has_pending = false;
        }
        state.value = event.value;
        const NetId net = device.outputs[event.pin];
        if (net == kNoNet) return;
        resolveNet(net, event.device, event.pin);
    }

    void applyStimulus(const Event& event) {
        const NetId net = event.device; /* stimulus events carry the net here */
        if (net >= netlist_.nets().size()) return;
        Net& target = netlist_.net(net);
        target.stimulus_active = true;
        target.stimulus_logic = event.value.size() ? event.value[0] : Logic::kZ;
        target.stimulus_strength = event.strength;
        resolveNet(net, kNoDevice, 0);
    }

    void resolveNet(NetId id, DeviceId source, uint16_t pin) {
        Net& net = netlist_.net(id);
        const size_t bits = net.bits;
        BitVector resolved(bits, Logic::kZ);
        std::vector<Drive> drives;
        bool conflict = false;
        for (size_t bit = 0; bit < bits; ++bit) {
            drives.clear();
            if (net.stimulus_active) drives.push_back(Drive{net.stimulus_logic, net.stimulus_strength});
            int zeros = 0;
            int ones = 0;
            for (size_t index = 0; index < net.drivers.size(); ++index) {
                const DriverSlot& driver = net.drivers[index];
                const PinState& state = netlist_.device(driver.device).output_state[driver.pin];
                const BitVector fitted = engine_detail::fitVector(state.value, bits);
                const Logic value = fitted[bit];
                drives.push_back(Drive{value, net.driver_strength[index]});
                if (value == Logic::kZero) ++zeros;
                if (value == Logic::kOne) ++ones;
            }
            const Drive winner = resolveBit(drives.data(), drives.size());
            resolved.at(bit) = winner.logic;
            if (zeros > 0 && ones > 0) conflict = true;
        }
        if (resolved == net.value) return;
        net.previous = net.value;
        net.value = resolved;
        ++net.changes;
        net.last_change = now_;
        recordTrace(net, source, pin, false);
        if (conflict && options_.report_multi_driver_conflicts && !net.conflict_reported) {
            net.conflict_reported = true;
            Violation violation;
            violation.kind = ViolationKind::kMultiDriverConflict;
            violation.when = now_;
            violation.net = id;
            violation.detail = "net " + net.name + " resolved to " + resolved.toString() + " with fighting drivers";
            violations_.push_back(violation);
        }
        enqueueLoads(id);
    }

    void enqueueLoads(NetId id) {
        const Net& net = netlist_.net(id);
        for (const LoadSlot& load : net.loads) {
            if (load.device >= netlist_.devices().size()) continue;
            DeviceInstance& device = netlist_.device(load.device);
            if (load.pin >= device.input_change.size()) continue;
            device.input_previous[load.pin] = device.input_values[load.pin];
            device.input_change[load.pin] = now_;
            const DeviceSpec& spec = *device.spec;
            if (spec.kind == DeviceKind::kCombinational) {
                scheduleEvaluation(load.device, load.pin);
                continue;
            }
            if (static_cast<int>(load.pin) != spec.clock_pin) continue;
            const Logic clock = net.value.size() ? net.value[0] : Logic::kX;
            const bool active = (spec.clock_edge == EdgeKind::kPosEdge && clock == Logic::kOne) ||
                                (spec.clock_edge == EdgeKind::kNegEdge && clock == Logic::kZero);
            if (active) scheduleEvaluation(load.device, load.pin);
        }
    }

    void scheduleEvaluation(DeviceId device, uint16_t pin) {
        Event event;
        event.when = now_;
        event.layer = Layer::kActive;
        event.kind = static_cast<uint32_t>(engine_detail::EventKind::kEvaluate);
        event.device = device;
        event.pin = pin;
        scheduler_.push(event);
    }

    void recordTrace(const Net& net, DeviceId device, uint16_t pin, bool initial) {
        if (!options_.trace) return;
        if (trace_.size() >= options_.trace_capacity) {
            ++dropped_traces_;
            return;
        }
        TraceRecord record;
        record.when = now_;
        record.net = netIdOf(net);
        record.value = net.value;
        record.drive = firstBit(net.value);
        record.strength = Strength::kStrong;
        record.device = device;
        record.pin = pin;
        record.initial = initial;
        trace_.push_back(std::move(record));
    }

    NetId netIdOf(const Net& net) const { return static_cast<NetId>(&net - netlist_.nets().data()); }

    /* Every net gets one record at t = 0, the way $dumpvars does, so a reader
       never has to guess what a net started at. */
    void recordInitialTrace() {
        initial_trace_done_ = true;
        if (!options_.trace) return;
        for (size_t index = 0; index < netlist_.nets().size(); ++index) {
            recordTrace(netlist_.nets()[index], kNoDevice, 0, true);
        }
    }

    void reportSettledNets() {
        if (!options_.report_undriven_nets || settled_reported_) return;
        settled_reported_ = true;
        for (NetId id : undrivenNets()) {
            Violation violation;
            violation.kind = ViolationKind::kUndrivenNet;
            violation.when = now_;
            violation.net = id;
            violation.detail = "net " + netlist_.net(id).name + " has loads but no driver";
            violations_.push_back(violation);
        }
    }

    void checkScheduler() {
        if (scheduler_.clamped() > 0 && !past_reported_) {
            past_reported_ = true;
            Violation violation;
            violation.kind = ViolationKind::kPastSchedule;
            violation.when = now_;
            violation.detail = std::to_string(scheduler_.clamped()) + " events were scheduled in the past";
            violations_.push_back(violation);
        }
    }

    NetList netlist_;
    EngineOptions options_;
    Scheduler scheduler_{1024};
    std::array<std::vector<Event>, 3> layers_;
    std::vector<StimulusEntry> stimulus_;
    std::vector<TraceRecord> trace_;
    std::vector<Violation> violations_;
    std::vector<bool> unconnected_;
    uint64_t token_source_ = 0;
    Tick now_ = 0;
    uint64_t evaluations_ = 0;
    uint64_t delta_rounds_ = 0;
    uint64_t stale_updates_ = 0;
    uint64_t dropped_traces_ = 0;
    bool initialized_ = false;
    bool initial_trace_done_ = false;
    bool settled_reported_ = false;
    bool past_reported_ = false;
    bool aborted_ = false;
};

}  // namespace tcsim
