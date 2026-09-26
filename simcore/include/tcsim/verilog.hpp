#pragma once
/* The external judge (docs/PLAN-sandbox-simulator.md section 8.2): a netlist
   and its stimulus turned into Verilog a real simulator can run.

   The module is emitted flat, one statement per device, with the net names
   kept as they are, so the VCD iverilog writes can be compared net by net with
   the engine's own trace.  `problems` names every device that has no Verilog
   counterpart instead of leaving it out silently - a check that compares fewer
   devices than the circuit has is worse than no check. */

#include "engine.hpp"
#include "netlist.hpp"
#include "value.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace tcsim {

struct VerilogExport {
    std::string text;
    std::vector<std::string> problems;
};

namespace verilog_detail {

inline std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

inline std::string literal(const BitVector& value) {
    return std::to_string(value.size()) + "'b" + value.toString();
}

inline std::string declaration(const char* kind, const std::string& name, uint16_t bits) {
    if (bits <= 1) return std::string("  ") + kind + " " + name + ";\n";
    return std::string("  ") + kind + " [" + std::to_string(bits - 1) + ":0] " + name + ";\n";
}

/* A driver's strength is part of the netlist (`connectOutput(..., strength)`),
   and Verilog spells it in front of the delay.  strong is the default and
   needs no text at all. */
inline std::string strengthText(Strength strength) {
    switch (strength) {
        case Strength::kWeak: return "(weak1, weak0) ";
        case Strength::kHighZ: return "(highz1, highz0) ";
        case Strength::kStrong: break;
    }
    return "";
}

}  // namespace verilog_detail

inline VerilogExport exportVerilog(const NetList& list, const std::vector<StimulusEntry>& stimulus,
                                   Tick end_tick, const std::string& module_name = "tb") {
    VerilogExport out;
    const size_t net_count = list.nets().size();
    std::vector<bool> sequential_driven(net_count, false);
    std::vector<bool> any_driver(net_count, false);
    std::vector<bool> stimulated(net_count, false);
    for (const StimulusEntry& entry : stimulus) {
        if (entry.net < net_count) stimulated[entry.net] = true;
    }
    for (const DeviceInstance& device : list.devices()) {
        const bool sequential = device.spec->kind == DeviceKind::kSequential;
        for (NetId net : device.outputs) {
            if (net >= net_count) continue;
            any_driver[net] = true;
            if (sequential) sequential_driven[net] = true;
        }
    }
    for (size_t index = 0; index < net_count; ++index) {
        if (!sequential_driven[index]) continue;
        if (stimulated[index] || list.nets()[index].drivers.size() > 1) {
            out.problems.push_back("net " + list.nets()[index].name +
                                   " mixes a sequential driver with another driver; not emittable");
        }
    }

    std::string text;
    text += "`timescale 1ps/1ps\n";
    text += "module " + module_name + ";\n";
    for (size_t index = 0; index < net_count; ++index) {
        const Net& net = list.nets()[index];
        if (sequential_driven[index]) {
            text += verilog_detail::declaration("reg", net.name, net.bits);
        } else if (stimulated[index] && any_driver[index]) {
            text += verilog_detail::declaration("wire", net.name, net.bits);
            text += verilog_detail::declaration("reg", net.name + "_stim", net.bits);
            text += "  assign " + net.name + " = " + net.name + "_stim;\n";
        } else if (stimulated[index]) {
            text += verilog_detail::declaration("reg", net.name, net.bits);
        } else {
            text += verilog_detail::declaration("wire", net.name, net.bits);
        }
    }

    for (size_t device_index = 0; device_index < list.devices().size(); ++device_index) {
        const DeviceInstance& device = list.devices()[device_index];
        const DeviceSpec& spec = *device.spec;
        if (spec.verilog.empty()) {
            out.problems.push_back("device " + device.name + " (" + spec.name + ") has no Verilog form");
            continue;
        }
        if (!spec.uniformArcs()) {
            out.problems.push_back("device " + device.name + " (" + spec.name + ") has per-arc delays a gate delay cannot express");
            continue;
        }
        const ArcDelay delay = spec.kind == DeviceKind::kSequential ? spec.clock_to_q : spec.default_delay;
        std::string statement = spec.verilog;
        statement = verilog_detail::replaceAll(statement, "%rise%", std::to_string(delay.tplh));
        statement = verilog_detail::replaceAll(statement, "%fall%", std::to_string(delay.tphl));
        std::string strength_text;
        bool strength_known = false;
        bool strength_conflict = false;
        for (size_t pin = 0; pin < spec.outputs.size(); ++pin) {
            const NetId net = device.outputs[pin];
            if (net >= net_count) continue;
            const Net& target = list.net(net);
            for (size_t index = 0; index < target.drivers.size(); ++index) {
                if (target.drivers[index].device != device_index || target.drivers[index].pin != pin) continue;
                const std::string text = verilog_detail::strengthText(target.driver_strength[index]);
                if (!strength_known) {
                    strength_text = text;
                    strength_known = true;
                } else if (strength_text != text) {
                    strength_conflict = true;
                }
            }
        }
        if (strength_conflict) {
            out.problems.push_back("device " + device.name + " drives nets of different strengths; not emittable");
        }
        statement = verilog_detail::replaceAll(statement, "%strength%", strength_text);
        for (size_t pin = 0; pin < spec.inputs.size(); ++pin) {
            const NetId net = device.inputs[pin];
            const std::string name = net < net_count ? list.net(net).name : std::string("1'bx");
            statement = verilog_detail::replaceAll(statement, "%" + spec.inputs[pin].name + "%", name);
        }
        for (size_t pin = 0; pin < spec.outputs.size(); ++pin) {
            const NetId net = device.outputs[pin];
            const std::string name = net < net_count ? list.net(net).name : std::string("unconnected");
            statement = verilog_detail::replaceAll(statement, "%" + spec.outputs[pin].name + "%", name);
        }
        text += "  /* " + device.name + " */\n  " + statement + "\n";
    }

    std::vector<StimulusEntry> ordered = stimulus;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const StimulusEntry& left, const StimulusEntry& right) { return left.when < right.when; });
    text += "  initial begin\n";
    text += "    $dumpfile(\"" + module_name + ".vcd\");\n";
    text += "    $dumpvars(0, " + module_name + ");\n";
    Tick previous = 0;
    bool first = true;
    for (const StimulusEntry& entry : ordered) {
        if (entry.net >= net_count) continue;
        const std::string name = stimulated[entry.net] && any_driver[entry.net]
                                     ? list.net(entry.net).name + "_stim"
                                     : list.net(entry.net).name;
        if (entry.when > previous || first) {
            if (entry.when > previous) text += "    #" + std::to_string(entry.when - previous) + " " + name + " = " +
                                              verilog_detail::literal(entry.value) + ";\n";
            else text += "    " + name + " = " + verilog_detail::literal(entry.value) + ";\n";
            previous = entry.when;
            first = false;
            continue;
        }
        text += "    " + name + " = " + verilog_detail::literal(entry.value) + ";\n";
    }
    text += "    #" + std::to_string(end_tick > previous ? end_tick - previous : 0) + " $finish;\n";
    text += "  end\n";
    text += "endmodule\n";
    out.text = std::move(text);
    return out;
}

}  // namespace tcsim
