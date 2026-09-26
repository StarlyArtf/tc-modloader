#pragma once
/* L4 of the sandbox simulator (docs/PLAN-sandbox-simulator.md section 4.6):
   nets, pins and arcs, with runtime state next to the structure.

   The netlist is what tcsim will build from a board and what simcore runs on,
   so it holds exactly the things the game's data can supply: net width, the
   pins a net connects, the strength of each driver, and the resolved value per
   net.  Loops are ordinary here - there is no compiler to refuse them.

   `problems` collects everything the builder refused to guess (an unknown pin
   name, a drive on a pin that is an input, a width mismatch).  The plan's red
   line 6 says an unsupported circuit must be named rather than quietly
   mis-simulated, so a caller checks `problems` before running. */

#include "delay.hpp"
#include "time.hpp"
#include "value.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tcsim {

using DeviceId = uint32_t;
using NetId = uint32_t;
inline constexpr uint32_t kNoDevice = 0xFFFFFFFFu;
inline constexpr uint32_t kNoNet = 0xFFFFFFFFu;

struct PinSpec {
    std::string name;
    uint16_t bits = 1;
};

enum class DeviceKind : uint8_t { kCombinational = 0, kSequential = 1 };
enum class EdgeKind : uint8_t { kNone = 0, kPosEdge = 1, kNegEdge = 2 };

/* What a device is told about why it is being evaluated.  A combinational
   device only needs the input values; a sequential one needs the edge, and the
   observer needs to know which pin caused a value so a trace can name it. */
struct EvalContext {
    Tick now = 0;
    int cause_pin = -1;
    Logic cause_from = Logic::kX;
    Logic cause_to = Logic::kX;
    Tick cause_at = 0;
    uint64_t evaluations = 0;
};

using Behavior = std::function<void(const EvalContext&, const std::vector<BitVector>&,
                                    std::vector<BitVector>&)>;

struct DeviceSpec {
    std::string name;
    DeviceKind kind = DeviceKind::kCombinational;
    std::vector<PinSpec> inputs;
    std::vector<PinSpec> outputs;
    Behavior behavior;
    ArcDelay default_delay{};
    std::vector<Arc> arcs;
    DelayKind delay_kind = DelayKind::kInertial;

    /* Sequential devices: which input is the clock, which edge latches, and
       the three windows the plan asks for (clock-to-q plus setup/hold). */
    int clock_pin = -1;
    EdgeKind clock_edge = EdgeKind::kNone;
    ArcDelay clock_to_q{};
    ArcDelay setup{};
    ArcDelay hold{};
    bool x_on_violation = false;

    /* A Verilog statement list for tools/simcore-iverilog.ps1: %pinname% is
       replaced by the connected net and %rise%/%fall% by the arc delay.  Empty
       when the device has no counterpart a Verilog simulator can run. */
    std::string verilog;

    int inputIndex(const std::string& pin) const {
        for (size_t index = 0; index < inputs.size(); ++index) {
            if (inputs[index].name == pin) return static_cast<int>(index);
        }
        return -1;
    }
    int outputIndex(const std::string& pin) const {
        for (size_t index = 0; index < outputs.size(); ++index) {
            if (outputs[index].name == pin) return static_cast<int>(index);
        }
        return -1;
    }
    ArcDelay delayFor(uint16_t input, uint16_t output) const {
        for (const Arc& arc : arcs) {
            if (arc.input == input && arc.output == output) return arc.delay;
        }
        return default_delay;
    }
    /* true when every arc shares one rise/fall pair, which is what a single
       Verilog gate delay can express. */
    bool uniformArcs() const {
        for (const Arc& arc : arcs) {
            if (arc.delay.tplh != default_delay.tplh || arc.delay.tphl != default_delay.tphl) return false;
        }
        return true;
    }
};

struct DriverSlot {
    DeviceId device = kNoDevice;
    uint16_t pin = 0;
};
struct LoadSlot {
    DeviceId device = kNoDevice;
    uint16_t pin = 0;
};

struct Net {
    std::string name;
    uint16_t bits = 1;
    std::vector<DriverSlot> drivers;
    std::vector<Strength> driver_strength;
    std::vector<LoadSlot> loads;
    BitVector value;
    BitVector previous;
    bool stimulus_active = false;
    Logic stimulus_logic = Logic::kZ;
    Strength stimulus_strength = Strength::kStrong;
    uint64_t changes = 0;
    Tick last_change = -1;
    bool ever_driven = false;
    bool conflict_reported = false;
};

/* One output pin's timing state: the value the net currently sees from this
   driver, plus the update that is on its way.  `token` is what makes an
   inertial delay cancellable without touching the queue. */
struct PinState {
    BitVector value;
    uint64_t token = 0;
    bool has_pending = false;
    BitVector pending_value;
    Tick pending_when = 0;
};

struct DeviceInstance {
    const DeviceSpec* spec = nullptr;
    std::string name;
    std::vector<NetId> inputs;
    std::vector<NetId> outputs;
    std::vector<PinState> output_state;
    std::vector<BitVector> input_values;
    std::vector<BitVector> input_previous;
    std::vector<Tick> input_change;
    std::vector<BitVector> state;
    uint64_t evaluations = 0;
    Tick last_clock_edge = -1;
    uint64_t setup_violations = 0;
    uint64_t hold_violations = 0;
};

class NetList {
public:
    std::vector<std::string> problems;

    NetId addNet(const std::string& name, uint16_t bits = 1) {
        const auto found = net_names_.find(name);
        if (found != net_names_.end()) {
            problems.push_back("duplicate net name: " + name);
            return found->second;
        }
        const NetId id = static_cast<NetId>(nets_.size());
        Net net;
        net.name = name;
        net.bits = bits ? bits : 1;
        net.value = BitVector(net.bits, Logic::kZ);
        net.previous = net.value;
        nets_.push_back(std::move(net));
        net_names_[name] = id;
        return id;
    }

    DeviceId addDevice(const DeviceSpec* spec, const std::string& name) {
        if (!spec) {
            problems.push_back("addDevice with no spec: " + name);
            return kNoDevice;
        }
        const DeviceId id = static_cast<DeviceId>(devices_.size());
        DeviceInstance instance;
        instance.spec = spec;
        instance.name = name.empty() ? (spec->name + std::to_string(id)) : name;
        instance.inputs.assign(spec->inputs.size(), kNoNet);
        instance.outputs.assign(spec->outputs.size(), kNoNet);
        instance.output_state.resize(spec->outputs.size());
        instance.input_values.resize(spec->inputs.size());
        instance.input_previous.resize(spec->inputs.size());
        instance.input_change.assign(spec->inputs.size(), -1);
        for (size_t pin = 0; pin < spec->inputs.size(); ++pin) {
            instance.input_values[pin] = BitVector(spec->inputs[pin].bits, Logic::kX);
            instance.input_previous[pin] = instance.input_values[pin];
        }
        /* A driver nobody has evaluated yet reads as X, exactly like a wire
           driven by a gate that has not run: iverilog reports x there too. */
        for (size_t pin = 0; pin < spec->outputs.size(); ++pin) {
            instance.output_state[pin].value = BitVector(spec->outputs[pin].bits, Logic::kX);
        }
        devices_.push_back(std::move(instance));
        device_names_[name] = id;
        return id;
    }

    NetId netId(const std::string& name) const {
        const auto found = net_names_.find(name);
        return found == net_names_.end() ? kNoNet : found->second;
    }
    DeviceId deviceId(const std::string& name) const {
        const auto found = device_names_.find(name);
        return found == device_names_.end() ? kNoDevice : found->second;
    }

    /* The general form: the direction comes from the device's own pin list, so
       a caller cannot wire an output to an output by accident. */
    bool connect(DeviceId device, const std::string& pin, NetId net, Strength strength = Strength::kStrong) {
        if (device >= devices_.size() || net >= nets_.size()) {
            problems.push_back("connect with an unknown device or net: " + pin);
            return false;
        }
        DeviceInstance& instance = devices_[device];
        const int input = instance.spec->inputIndex(pin);
        if (input >= 0) return connectInput(device, static_cast<uint16_t>(input), net);
        const int output = instance.spec->outputIndex(pin);
        if (output >= 0) return connectOutput(device, static_cast<uint16_t>(output), net, strength);
        problems.push_back("no such pin on " + instance.spec->name + ": " + pin);
        return false;
    }
    bool connectInput(DeviceId device, uint16_t pin, NetId net) {
        DeviceInstance& instance = devices_[device];
        if (pin >= instance.inputs.size() || net >= nets_.size()) {
            problems.push_back("bad input connection on " + instance.name);
            return false;
        }
        instance.inputs[pin] = net;
        nets_[net].loads.push_back(LoadSlot{device, pin});
        return true;
    }
    bool connectOutput(DeviceId device, uint16_t pin, NetId net, Strength strength = Strength::kStrong) {
        DeviceInstance& instance = devices_[device];
        if (pin >= instance.outputs.size() || net >= nets_.size()) {
            problems.push_back("bad output connection on " + instance.name);
            return false;
        }
        instance.outputs[pin] = net;
        nets_[net].drivers.push_back(DriverSlot{device, pin});
        nets_[net].driver_strength.push_back(strength);
        nets_[net].bits = nets_[net].bits ? nets_[net].bits : 1;
        return true;
    }
    bool setDriverStrength(DeviceId device, const std::string& pin, Strength strength) {
        const DeviceInstance& instance = devices_[device];
        const int output = instance.spec->outputIndex(pin);
        if (output < 0 || instance.outputs[static_cast<size_t>(output)] == kNoNet) return false;
        Net& net = nets_[instance.outputs[static_cast<size_t>(output)]];
        for (size_t index = 0; index < net.drivers.size(); ++index) {
            if (net.drivers[index].device == device && net.drivers[index].pin == static_cast<uint16_t>(output)) {
                net.driver_strength[index] = strength;
                return true;
            }
        }
        return false;
    }

    std::vector<Net>& nets() { return nets_; }
    const std::vector<Net>& nets() const { return nets_; }
    std::vector<DeviceInstance>& devices() { return devices_; }
    const std::vector<DeviceInstance>& devices() const { return devices_; }
    Net& net(NetId id) { return nets_[id]; }
    const Net& net(NetId id) const { return nets_[id]; }
    DeviceInstance& device(DeviceId id) { return devices_[id]; }
    const DeviceInstance& device(DeviceId id) const { return devices_[id]; }

    /* The longest delay any arc can produce, which is how big the scheduler's
       wheel has to be so that no deadline lands in the overflow heap by
       accident.  Sequential devices' clock-to-q counts as well. */
    Tick longestDelay() const {
        Tick longest = 0;
        for (const DeviceInstance& instance : devices_) {
            const DeviceSpec& spec = *instance.spec;
            const ArcDelay* delays[] = {&spec.default_delay, &spec.clock_to_q, &spec.setup, &spec.hold};
            for (const ArcDelay* delay : delays) {
                if (delay->tplh > longest) longest = delay->tplh;
                if (delay->tphl > longest) longest = delay->tphl;
            }
            for (const Arc& arc : spec.arcs) {
                if (arc.delay.tplh > longest) longest = arc.delay.tplh;
                if (arc.delay.tphl > longest) longest = arc.delay.tphl;
            }
        }
        return longest;
    }
    size_t deviceCount() const { return devices_.size(); }
    size_t netCount() const { return nets_.size(); }

private:
    std::vector<Net> nets_;
    std::vector<DeviceInstance> devices_;
    std::unordered_map<std::string, NetId> net_names_;
    std::unordered_map<std::string, DeviceId> device_names_;
};

}  // namespace tcsim
