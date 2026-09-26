#pragma once
/* S2's first half (docs/PLAN-sandbox-simulator.md section 4.6): the game's board
   records turned into a simcore netlist.

   The records are the authority, not a guess:

     component  kind +0x00, x +0x02, y +0x04, rotation +0x06, id +0x08,
                custom prototype id +0x188, input pin count +0x30,
                output pin count +0x40, and a pointer at +0x38
                (src/board_objects.hpp plus a real session measured on
                2026-09-25 with tests/sim-state-probe.cpp);
     wire       both endpoints +0x18..+0x1e, width +0x30, and the state byte
                offset the game's own reader uses at +0x38.

   Pin geometry comes from the game's prototype table (the same table
   tools/generate-gate-pins.py dumps).  A cell two component pins share is
   reported, never picked at random, and a kind the device library does not
   implement is reported as unsupported - red line 6. */

#include "devices.hpp"
#include "engine.hpp"
#include "netlist.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tcsim {

/* One prototype's pins, relative to the component's own cell.  `bits` is the
   pin's word size: 1 for a gate, up to 64 for a bus pin. */
struct KindPins {
    uint8_t kind = 0;
    uint8_t input_count = 0;
    uint8_t output_count = 0;
    int8_t in_x[8] = {};
    int8_t in_y[8] = {};
    uint16_t in_bits[8] = {};
    int8_t out_x[4] = {};
    int8_t out_y[4] = {};
    uint16_t out_bits[4] = {};
    const char* name = "";
};

struct BoardComponent {
    uint8_t kind = 0;
    int16_t x = 0;
    int16_t y = 0;
    uint8_t rotation = 0;
    uint64_t id = 0;
    uint64_t custom_id = 0;
    uint64_t setting_value = 0;
    bool has_setting_value = false;
    uint8_t input_count = 0;
    uint8_t output_count = 0;
    uint64_t private_pointer = 0;
};

struct BoardWire {
    int16_t x1 = 0;
    int16_t y1 = 0;
    int16_t x2 = 0;
    int16_t y2 = 0;
    uint16_t width = 0;
    uint64_t slot = 0;
};

struct BoardView {
    std::vector<BoardComponent> components;
    std::vector<BoardWire> wires;
};

/* Quarter turns clockwise, the convention the editor's rotation byte uses. */
inline std::pair<int, int> rotateOffset(int x, int y, uint8_t rotation) {
    switch (rotation & 3u) {
        case 1: return {-y, x};
        case 2: return {-x, -y};
        case 3: return {y, -x};
        default: break;
    }
    return {x, y};
}

/* The kinds the sandbox's gate levels are built from.  Everything else is
   reported as unsupported until S3 covers it: a wrong device is worse than a
   missing one. */
inline std::string deviceNameForKind(uint8_t kind) {
    switch (kind) {
        case 0x03: case 0x12: return "not";
        case 0x04: case 0x14: return "and2";
        case 0x05: return "and3";
        case 0x06: case 0x15: return "nand2";
        case 0x07: case 0x13: return "or2";
        case 0x08: return "or3";
        case 0x09: case 0x16: return "nor2";
        case 0x0a: case 0x17: return "xor2";
        case 0x0b: case 0x18: return "xnor2";
        default: break;
    }
    return {};
}

/* Kinds that feed a net from outside the board: the level's inputs and the
   player's switches.  Their value lives in the game's state array, so the
   netlist only marks the net as externally driven. */
inline bool kindIsSource(uint8_t kind) {
    switch (kind) {
        case 0x34: /* Push Button */
        case 0x3c: case 0x3d: case 0x3f: case 0x40: case 0x41: /* level inputs */
        case 0x5f: /* Verilog Input */
            return true;
        default: break;
    }
    return false;
}

/* Kinds that only observe: an Output, a probe. */
inline bool kindIsSink(uint8_t kind) {
    switch (kind) {
        case 0x28: case 0x3a: case 0x44: case 0x45: case 0x49: case 0x4a: case 0x4b: /* Outputs */
        case 0x52: case 0x54: case 0x55: /* probes */
        case 0x60: /* Verilog Output */
        case 0x46: /* Switched Output: value + switch, no outputs */
            return true;
        default: break;
    }
    return false;
}

inline bool kindIsConstant(uint8_t kind) { return kind == 0x01 || kind == 0x02 || kind == 0x2e; }

/* Host components registered by examples/clock.  The clock is simulator-owned;
   the switch and button are external sources whose live value is published by
   their owning Mod into the same wire slots that ordinary inputs use. */
inline bool customIsClock(uint64_t custom_id) {
    return custom_id == 0x434C4F4B5F303031ULL; /* CLOK_001 */
}
inline bool customIsInteractiveSource(uint64_t custom_id) {
    return custom_id == 0x535749545F303031ULL || /* SWIT_001 */
           custom_id == 0x4255544E5F303031ULL;   /* BUTN_001 */
}

inline Logic constantValue(const BoardComponent& component) {
    if (component.kind == 0x01) return Logic::kZero; /* OFF */
    if (component.kind == 0x02) return Logic::kOne;  /* ON */
    return component.has_setting_value && (component.setting_value & 1u) ? Logic::kOne : Logic::kZero;
}

struct BuildNote {
    std::string text;
    bool fatal = false; /* a fatal note means the netlist must not be run */
};

struct BuildResult {
    /* Where a source net's value comes from.  A host component (the interactive
       switch/button) keeps its value in its own instance configuration, not in a
       wire slot: the game's generated program is what copies it into the state
       array, and the sandbox suppresses that program.  `custom_id == 0` means
       "read the state array" (level inputs and the builtin sources). */
    struct SourceOrigin {
        uint64_t custom_id = 0;
        uint64_t instance_id = 0;
    };
    NetList netlist;
    std::vector<BuildNote> notes;
    std::vector<NetId> net_of_wire;
    std::vector<std::vector<uint64_t>> slots_of_net; /* state bytes to write per net */
    std::vector<NetId> source_nets;                  /* driven by the game's array */
    std::vector<SourceOrigin> source_origins;        /* parallel to source_nets */
    std::vector<NetId> observed_nets;                /* read back by the game */
    std::vector<NetId> clock_nets;                   /* host clock components */
    std::vector<DeviceId> devices_by_component;
    size_t unconnected_pins = 0;
    bool fatal() const {
        for (const BuildNote& note : notes) {
            if (note.fatal) return true;
        }
        return false;
    }
};

struct BuildOptions {
    /* Include the wire's neighbouring byte in the write-back set: a real board
       writes the same value into the wire's slot and into the byte after it
       (measured 2026-09-25: slots 256/257, 258/259, 260/261 move together). */
    bool include_neighbour_slot = true;
    bool rotate_pins = true;
    /* Delay a gate gets when no timing library covers it ("每器件一个常数" in
       the plan's section 4.4).  Zero means "settle through delta rounds". */
    ArcDelay gate_delay{};
};

namespace board_detail {

inline uint64_t readU64(const unsigned char* bytes, size_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

using Coordinate = std::pair<int, int>;

struct PinSite {
    size_t component = 0;
    bool input = false;
    uint8_t pin = 0;
};

}  // namespace board_detail

/* Builds the netlist.  `geometry` answers with a prototype's pins for a kind;
   the plugin wires it to the game's own table and the offline test wires it to
   the generated table, so both run the same builder. */
inline BuildResult buildNetList(const BoardView& board,
                                const std::function<bool(const BoardComponent&, KindPins&)>& geometry,
                                const BuildOptions& options = BuildOptions()) {
    BuildResult result;
    result.net_of_wire.assign(board.wires.size(), kNoNet);
    result.devices_by_component.assign(board.components.size(), kNoDevice);

    /* Every pin the board has, placed where the wires are drawn. */
    std::map<board_detail::Coordinate, std::vector<board_detail::PinSite>> sites;
    std::vector<KindPins> pins_of(board.components.size());
    for (size_t index = 0; index < board.components.size(); ++index) {
        const BoardComponent& component = board.components[index];
        if (component.kind == 0x00) continue; /* the board's own record */
        KindPins pins;
        if (!geometry(component, pins)) {
            result.notes.push_back(BuildNote{"component " + std::to_string(index) + " has kind 0x" +
                                                 std::to_string(component.kind) + " with no pin geometry",
                                             true});
            continue;
        }
        pins_of[index] = pins;
        if (pins.input_count != component.input_count || pins.output_count != component.output_count) {
            result.notes.push_back(BuildNote{"component " + std::to_string(index) + " (" + pins.name +
                                                 ") has " + std::to_string(component.input_count) + "/" +
                                                 std::to_string(component.output_count) +
                                                 " pins in its record but the prototype has " +
                                                 std::to_string(pins.input_count) + "/" +
                                                 std::to_string(pins.output_count),
                                             true});
            continue;
        }
        if (pins.input_count > 8 || pins.output_count > 4) continue; /* the geometry table is bounded */
        const uint8_t rotation = options.rotate_pins ? component.rotation : 0;
        for (uint8_t pin = 0; pin < pins.input_count; ++pin) {
            const auto offset = rotateOffset(pins.in_x[pin], pins.in_y[pin], rotation);
            sites[{component.x + offset.first, component.y + offset.second}].push_back(
                board_detail::PinSite{index, true, pin});
        }
        for (uint8_t pin = 0; pin < pins.output_count; ++pin) {
            const auto offset = rotateOffset(pins.out_x[pin], pins.out_y[pin], rotation);
            sites[{component.x + offset.first, component.y + offset.second}].push_back(
                board_detail::PinSite{index, false, pin});
        }
    }

    /* Wires join the cells they are drawn between; a cell lands on exactly one
       net, so a net is a connected group of wires. */
    std::vector<int> parent;
    std::map<board_detail::Coordinate, int> coordinate_id;
    const auto find = [&parent](int value) {
        while (parent[static_cast<size_t>(value)] != value) {
            parent[static_cast<size_t>(value)] = parent[static_cast<size_t>(parent[static_cast<size_t>(value)])];
            value = parent[static_cast<size_t>(value)];
        }
        return value;
    };
    const auto id_of = [&coordinate_id, &parent](const board_detail::Coordinate& coordinate) {
        const auto found = coordinate_id.find(coordinate);
        if (found != coordinate_id.end()) return found->second;
        const int id = static_cast<int>(parent.size());
        parent.push_back(id);
        coordinate_id[coordinate] = id;
        return id;
    };
    for (const BoardWire& wire : board.wires) {
        const int first = id_of({wire.x1, wire.y1});
        const int second = id_of({wire.x2, wire.y2});
        const int a = find(first);
        const int b = find(second);
        if (a != b) parent[static_cast<size_t>(a)] = b;
    }

    std::map<int, NetId> net_of_group;
    for (size_t index = 0; index < board.wires.size(); ++index) {
        const BoardWire& wire = board.wires[index];
        const int group = find(id_of({wire.x1, wire.y1}));
        auto found = net_of_group.find(group);
        if (found == net_of_group.end()) {
            const NetId net =
                result.netlist.addNet("net" + std::to_string(group), static_cast<uint16_t>(wire.width ? wire.width : 1));
            result.slots_of_net.resize(result.netlist.netCount());
            found = net_of_group.emplace(group, net).first;
        }
        result.net_of_wire[index] = found->second;
        /* The wire's own state byte is where the game reads this net's value. */
        result.slots_of_net[found->second].push_back(wire.slot);
        if (options.include_neighbour_slot) result.slots_of_net[found->second].push_back(wire.slot + 1);
    }

    /* Pins attach to the net that already owns their cell. */
    for (const auto& entry : sites) {
        const std::vector<board_detail::PinSite>& pins = entry.second;
        const auto found = coordinate_id.find(entry.first);
        if (found == coordinate_id.end()) continue; /* a pin no wire reaches */
        const NetId net = net_of_group[find(found->second)];
        if (pins.size() > 1) {
            result.notes.push_back(BuildNote{"component pins " + std::to_string(pins.size()) +
                                                 " share the cell " + std::to_string(entry.first.first) + "," +
                                                 std::to_string(entry.first.second) + "; not guessed",
                                             false});
        }
        for (const board_detail::PinSite& site : pins) {
            const BoardComponent& component = board.components[site.component];
            if (component.kind == 0x4e) {
                /* A host component (the clock, the interactive switch/button) is
                   recognized by its id, not by which side of the pin list the
                   cell came from.  Registered 0-input components can still carry
                   a scaffolding input pin in the prototype table; if a wire
                   happens to touch that cell, a pin-site-only test would call
                   the switch unknown and refuse the whole board (measured on a
                   player board: `custom component 6005349254844919857 has no
                   sandbox simulator behavior` while the switch itself was fine). */
                const bool is_clock = customIsClock(component.custom_id);
                const bool is_interactive = customIsInteractiveSource(component.custom_id);
                if (is_clock || is_interactive) {
                    if (site.input) continue; /* scaffolding pin: nothing is driven from it */
                    std::vector<NetId>& driven = is_clock ? result.clock_nets : result.source_nets;
                    if (std::find(driven.begin(), driven.end(), net) == driven.end()) {
                        driven.push_back(net);
                        if (!is_clock) {
                            result.source_origins.push_back(
                                BuildResult::SourceOrigin{component.custom_id, component.id});
                        }
                    }
                    continue;
                }
                /* An unknown custom component only matters where it drives a
                   net; its input pins are somebody else's output. */
                if (site.input) continue;
                result.notes.push_back(BuildNote{"custom component " + std::to_string(component.custom_id) +
                                                     " has no sandbox simulator behavior",
                                                 true});
                continue;
            }
            if (kindIsConstant(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const DeviceSpec& spec = deviceConstant(constantValue(component), options.gate_delay.tplh);
                    result.devices_by_component[site.component] =
                        result.netlist.addDevice(&spec, "u" + std::to_string(site.component) + "_constant");
                }
                if (!site.input) {
                    result.netlist.connectOutput(result.devices_by_component[site.component], site.pin, net);
                }
                continue;
            }
            if (kindIsSource(component.kind)) {
                if (std::find(result.source_nets.begin(), result.source_nets.end(), net) == result.source_nets.end()) {
                    result.source_nets.push_back(net);
                    /* A builtin source (a level input, a Verilog input) is read
                       from the game's state array. */
                    result.source_origins.push_back(BuildResult::SourceOrigin{});
                }
                continue;
            }
            if (kindIsSink(component.kind)) {
                if (std::find(result.observed_nets.begin(), result.observed_nets.end(), net) ==
                    result.observed_nets.end()) {
                    result.observed_nets.push_back(net);
                }
                continue;
            }
            const std::string name = deviceNameForKind(component.kind);
            if (name.empty()) {
                result.notes.push_back(BuildNote{"component " + std::to_string(site.component) + " (kind 0x" +
                                                     std::to_string(component.kind) +
                                                     ") has no device in the library",
                                                 true});
                continue;
            }
            if (result.devices_by_component[site.component] == kNoDevice) {
                const DeviceSpec* spec = builtinDevice(name, options.gate_delay);
                if (!spec) {
                    result.notes.push_back(BuildNote{"device " + name + " is missing from the builtin library", false});
                    continue;
                }
                result.devices_by_component[site.component] =
                    result.netlist.addDevice(spec, "u" + std::to_string(site.component) + "_" + name);
            }
            const DeviceId device = result.devices_by_component[site.component];
            if (site.input) {
                result.netlist.connectInput(device, site.pin, net);
            } else {
                result.netlist.connectOutput(device, site.pin, net);
            }
        }
    }

    for (const auto& entry : sites) {
        if (coordinate_id.find(entry.first) == coordinate_id.end()) ++result.unconnected_pins;
    }
    for (const std::string& problem : result.netlist.problems) {
        result.notes.push_back(BuildNote{problem, true});
    }
    return result;
}

/* The record decoding, kept next to the builder so the plugin and the offline
   test read the bytes the same way. */
inline BoardView readBoard(const unsigned char* board, uint64_t component_stride = 0x238,
                           uint64_t wire_stride = 0x68) {
    BoardView view;
    if (!board) return view;
    uint64_t components = 0;
    const unsigned char* component_data = nullptr;
    uint64_t wires = 0;
    const unsigned char* wire_data = nullptr;
    components = board_detail::readU64(board, 0x78);
    std::memcpy(&component_data, board + 0x80, sizeof(component_data));
    wires = board_detail::readU64(board, 0x98);
    std::memcpy(&wire_data, board + 0xa0, sizeof(wire_data));
    if (component_data && components <= 4096) {
        for (uint64_t index = 0; index < components; ++index) {
            const unsigned char* record = component_data + 8 + index * component_stride;
            BoardComponent component;
            component.kind = record[0];
            std::memcpy(&component.x, record + 2, 2);
            std::memcpy(&component.y, record + 4, 2);
            component.rotation = record[6];
            component.id = board_detail::readU64(record, 8);
            component.input_count = static_cast<uint8_t>(board_detail::readU64(record, 0x30));
            component.output_count = static_cast<uint8_t>(board_detail::readU64(record, 0x40));
            component.private_pointer = board_detail::readU64(record, 0x38);
            component.custom_id = board_detail::readU64(record, 0x188);
            const uint64_t setting_count = board_detail::readU64(record, 0xa8);
            const unsigned char* settings = nullptr;
            std::memcpy(&settings, record + 0xb0, sizeof(settings));
            if (setting_count > 0 && setting_count < 1024 &&
                reinterpret_cast<uintptr_t>(settings) >= 0x10000) {
                component.setting_value = board_detail::readU64(settings, 8);
                component.has_setting_value = true;
            }
            view.components.push_back(component);
        }
    }
    if (wire_data && wires <= 65536) {
        for (uint64_t index = 0; index < wires; ++index) {
            const unsigned char* record = wire_data + 8 + index * wire_stride;
            BoardWire wire;
            std::memcpy(&wire.x1, record + 0x18, 2);
            std::memcpy(&wire.y1, record + 0x1a, 2);
            std::memcpy(&wire.x2, record + 0x1c, 2);
            std::memcpy(&wire.y2, record + 0x1e, 2);
            uint32_t width = 0;
            std::memcpy(&width, record + 0x30, 4);
            wire.width = static_cast<uint16_t>(width);
            wire.slot = board_detail::readU64(record, 0x38);
            view.wires.push_back(wire);
        }
    }
    return view;
}

}  // namespace tcsim
