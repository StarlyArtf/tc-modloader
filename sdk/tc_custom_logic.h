#ifndef TC_CUSTOM_LOGIC_H
#define TC_CUSTOM_LOGIC_H

// Experimental native logic runtime for simple boards.
//
// Scope of this first version:
//   * level input components (kind 0x3f)
//   * level output components (kind 0x44)
//   * one or more registered custom component instances (kind 0x4e)
//   * point-to-point wires with a runtime state index at wire + 0x38
//
// Built-in logic gates are not interpreted yet.  The purpose of this API is
// to let a native Mod define the truth table/state of its own component while
// the loader supplies cycle/input/output plumbing for the simple level IO
// topology used by the first native-logic tests.

#include "tc_mod.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace tc {

struct TCCustomLogicIO {
    int64_t cycle = 0;
    uint32_t input_count = 0;
    uint32_t output_count = 0;
    uint64_t inputs[8] = {};
    uint64_t outputs[8] = {};
    void* user = nullptr;
};

using TCCustomLogicFn = void (*)(TCCustomLogicIO* io);
using TCLevelInputFn = uint64_t (*)(int64_t cycle, uint32_t pin_index,
                                    void* user);
using TCLevelOutputFn = void (*)(int64_t cycle, uint32_t pin_index,
                                 uint64_t value, void* user);
using TCCustomLogicTestFn = int64_t (*)(int64_t cycle, const uint64_t* outputs,
                                        uint32_t output_count, void* user);

struct TCCustomLogicComponent {
    uint64_t custom_id = 0;
    uint32_t input_count = 0;
    uint32_t output_count = 0;
    TCCustomLogicFn logic = nullptr;
    TCLevelInputFn level_input = nullptr;
    TCLevelOutputFn level_input_write = nullptr;
    TCLevelOutputFn level_output_write = nullptr;
    TCCustomLogicTestFn test = nullptr;
    void* user = nullptr;
};

class TCCustomLogicRuntime {
 public:
    bool load(const TCHost* host) {
        *this = TCCustomLogicRuntime{};
        if (!host || !host->resolve_symbol) return false;
        auto resolve = [&](const char* name) {
            return host->resolve_symbol(host->context, name);
        };
        state_ = static_cast<unsigned char**>(resolve(
            "simulation_state__modelZsimulator95types_u81"));
        input_replay_ = static_cast<unsigned char**>(resolve(
            "simulation_input_replay__modelZsimulator95types_u84"));
        output_history_ = static_cast<unsigned char**>(resolve(
            "simulation_output_history_pins__modelZsimulator95types_u85"));
        get_setting_ = reinterpret_cast<int64_t (*)(uint8_t)>(resolve(
            "get_simulation_setting__modelZsimulator95types_u92"));
        set_setting_ = reinterpret_cast<void (*)(uint8_t, int64_t)>(resolve(
            "set_simulation_setting__modelZsimulator95types_u118"));
        return state_ && input_replay_ && output_history_ && get_setting_ &&
               set_setting_;
    }

    bool add(const TCCustomLogicComponent& component) {
        if (!component.custom_id || !component.logic ||
            component.input_count > 8 || component.output_count > 8) {
            return false;
        }
        components_.push_back(component);
        return true;
    }

    bool run(const TCMod& mod, void* model, int64_t target) {
        if (!state_ || !*state_ || !model) return false;
        Board board;
        if (!parseBoard(mod, model, board)) return false;
        net_values_.clear();

        int64_t current = get_setting_(0);
        if (current < -1) current = -1;
        if (current < 0) failed_ = false;
        for (int64_t cycle = current + 1; cycle <= target; ++cycle) {
            for (const auto& input : board.level_inputs) {
                for (uint32_t pin = 0; pin < input.nets.size(); ++pin) {
                    uint64_t value = 0;
                    if (input.definition && input.definition->level_input) {
                        value = input.definition->level_input(
                            cycle, pin, input.definition->user);
                    } else {
                        value = (cycle < 0 ? 0 : (uint64_t(cycle) >> pin)) & 1;
                    }
                    writeNet(board, input.nets[pin], value);
                    if (input.definition && input.definition->level_input_write) {
                        input.definition->level_input_write(
                            cycle, pin, value, input.definition->user);
                    }
                }
            }

            for (const auto& instance : board.custom_instances) {
                TCCustomLogicIO io;
                io.cycle = cycle;
                io.input_count = static_cast<uint32_t>(instance.input_nets.size());
                io.output_count = static_cast<uint32_t>(instance.output_nets.size());
                io.user = instance.definition->user;
                for (uint32_t i = 0; i < io.input_count && i < 8; ++i) {
                    io.inputs[i] = readNet(board, instance.input_nets[i]);
                }
                instance.definition->logic(&io);
                for (uint32_t i = 0; i < io.output_count && i < 8; ++i) {
                    writeNet(board, instance.output_nets[i], io.outputs[i]);
                }
            }

            uint64_t outputs[8] = {};
            uint32_t output_count = 0;
            for (const auto& output : board.level_outputs) {
                for (uint32_t pin = 0; pin < output.nets.size() && output_count < 8;
                     ++pin) {
                    const uint64_t value = readNet(board, output.nets[pin]);
                    outputs[output_count++] = value;
                    if (output.definition && output.definition->level_output_write) {
                        output.definition->level_output_write(
                            cycle, pin, value, output.definition->user);
                    }
                }
            }

            int64_t test_state = cycle >= target ? 1 : 0;
            for (const auto& instance : board.custom_instances) {
                if (!instance.definition->test) continue;
                const int64_t value = instance.definition->test(
                    cycle, outputs, output_count, instance.definition->user);
                if (value == 2) failed_ = true;
                if (value == 1 && !failed_) test_state = 1;
            }
            if (failed_) test_state = 2;
            set_setting_(0, cycle);
            set_setting_(2, test_state);
        }
        return true;
    }

 private:
    struct Point {
        int16_t x = 0;
        int16_t y = 0;
    };
    struct NetInfo {
        uint64_t state_index = 0;
        bool has_state = false;
    };
    struct IOInstance {
        const TCCustomLogicComponent* definition = nullptr;
        std::vector<uint32_t> nets;
    };
    struct CustomInstance {
        const TCCustomLogicComponent* definition = nullptr;
        std::vector<uint32_t> input_nets;
        std::vector<uint32_t> output_nets;
    };
    struct Board {
        std::vector<NetInfo> nets;
        std::vector<IOInstance> level_inputs;
        std::vector<IOInstance> level_outputs;
        std::vector<CustomInstance> custom_instances;
    };

    static uint32_t pointKey(Point point) {
        return static_cast<uint32_t>(static_cast<uint16_t>(point.x)) |
               (static_cast<uint32_t>(static_cast<uint16_t>(point.y)) << 16);
    }

    static Point readPoint(const unsigned char* data) {
        Point point;
        std::memcpy(&point.x, data, sizeof(point.x));
        std::memcpy(&point.y, data + 2, sizeof(point.y));
        return point;
    }

    static void addOffset(Point base, int16_t dx, int16_t dy, Point& out) {
        out.x = static_cast<int16_t>(base.x + dx);
        out.y = static_cast<int16_t>(base.y + dy);
    }

    static bool prototypePinOffsets(const TCMod& mod, uint64_t custom_id,
                                    std::vector<Point>& inputs,
                                    std::vector<Point>& outputs) {
        TCPrototype prototype{};
        if (!mod.game.getCustomPrototype(custom_id, prototype)) return false;
        inputs.clear();
        outputs.clear();
        for (uint64_t i = 0; i < prototypeInputCount(prototype); ++i) {
            TCPin* pin = prototypeInputPin(prototype, i);
            if (!pin) break;
            const auto* data =
                reinterpret_cast<const unsigned char*>(pin) + 8;
            inputs.push_back(readPoint(data + 2));
        }
        for (uint64_t i = 0; i < prototypeOutputCount(prototype); ++i) {
            TCPin* pin = prototypeOutputPin(prototype, i);
            if (!pin) break;
            const auto* data =
                reinterpret_cast<const unsigned char*>(pin) + 8;
            outputs.push_back(readPoint(data + 2));
        }
        if (mod.components.valid()) mod.components.releasePrototype(prototype);
        return !inputs.empty() || !outputs.empty();
    }

    const TCCustomLogicComponent* findDefinition(uint64_t custom_id) const {
        for (const auto& component : components_) {
            if (component.custom_id == custom_id) return &component;
        }
        return nullptr;
    }

    uint32_t findNet(std::map<uint32_t, uint32_t>& parent, uint32_t point) {
        auto it = parent.find(point);
        if (it == parent.end()) {
            parent[point] = point;
            return point;
        }
        uint32_t root = it->second;
        if (root != point) root = findNet(parent, root);
        parent[point] = root;
        return root;
    }

    void unite(std::map<uint32_t, uint32_t>& parent, uint32_t a, uint32_t b) {
        const uint32_t root_a = findNet(parent, a);
        const uint32_t root_b = findNet(parent, b);
        if (root_a != root_b) parent[root_b] = root_a;
    }

    bool parseBoard(const TCMod& mod, void* model, Board& board) {
        auto* base = static_cast<unsigned char*>(model);
        uint64_t components = 0;
        uint64_t wires = 0;
        void* component_data = nullptr;
        void* wire_data = nullptr;
        std::memcpy(&components, base + 0x78, sizeof(components));
        std::memcpy(&component_data, base + 0x80, sizeof(component_data));
        std::memcpy(&wires, base + 0x98, sizeof(wires));
        std::memcpy(&wire_data, base + 0xa0, sizeof(wire_data));
        if (!component_data || !wire_data || components > 4096 || wires > 4096) {
            return false;
        }

        std::map<uint32_t, uint32_t> parent;
        std::map<uint32_t, NetInfo> net_info;
        std::map<uint32_t, uint32_t> root_to_net;
        auto netFor = [&](uint32_t point) {
            const uint32_t root = findNet(parent, point);
            auto existing = root_to_net.find(root);
            if (existing != root_to_net.end()) return existing->second;
            const uint32_t net = static_cast<uint32_t>(board.nets.size());
            auto info = net_info.find(root);
            board.nets.push_back(info == net_info.end() ? NetInfo{} : info->second);
            root_to_net[root] = net;
            return net;
        };
        for (uint64_t i = 0; i < wires; ++i) {
            const auto* wire =
                static_cast<const unsigned char*>(wire_data) + 8 + i * 0x68;
            const Point a = readPoint(wire + 0x18);
            const Point b = readPoint(wire + 0x1c);
            const uint32_t key_a = pointKey(a);
            const uint32_t key_b = pointKey(b);
            unite(parent, key_a, key_b);
            NetInfo info;
            std::memcpy(&info.state_index, wire + 0x38, sizeof(info.state_index));
            info.has_state = info.state_index != 0;
            const uint32_t root = findNet(parent, key_a);
            if (info.has_state && net_info.find(root) == net_info.end()) {
                net_info[root] = info;
            }
        }

        for (uint64_t i = 0; i < components; ++i) {
            const auto* component =
                static_cast<const unsigned char*>(component_data) + 8 + i * 0x238;
            uint16_t kind = 0;
            Point position;
            uint64_t custom_id = 0;
            std::memcpy(&kind, component, sizeof(kind));
            std::memcpy(&position.x, component + 2, sizeof(position.x));
            std::memcpy(&position.y, component + 4, sizeof(position.y));
            std::memcpy(&custom_id, component + 0x188, sizeof(custom_id));

            std::vector<Point> input_offsets;
            std::vector<Point> output_offsets;
            if (kind == 0x3f) {
                output_offsets = {{0, -1}, {0, 1}};
            } else if (kind == 0x44) {
                input_offsets = {{-1, 0}};
            } else if (kind == 0x4e) {
                if (!prototypePinOffsets(mod, custom_id, input_offsets,
                                         output_offsets)) {
                    continue;
                }
            } else {
                continue;
            }

            auto netsFor = [&](const std::vector<Point>& offsets) {
                std::vector<uint32_t> nets;
                nets.reserve(offsets.size());
                for (const Point& offset : offsets) {
                    Point absolute;
                    addOffset(position, offset.x, offset.y, absolute);
                    nets.push_back(netFor(pointKey(absolute)));
                }
                return nets;
            };

            if (kind == 0x3f) {
                IOInstance instance;
                instance.nets = netsFor(output_offsets);
                if (!components_.empty()) instance.definition = &components_.front();
                board.level_inputs.push_back(std::move(instance));
            } else if (kind == 0x44) {
                IOInstance instance;
                instance.nets = netsFor(input_offsets);
                if (!components_.empty()) instance.definition = &components_.front();
                board.level_outputs.push_back(std::move(instance));
            } else if (kind == 0x4e) {
                const TCCustomLogicComponent* definition =
                    findDefinition(custom_id);
                if (!definition) continue;
                CustomInstance instance;
                instance.definition = definition;
                instance.input_nets = netsFor(input_offsets);
                instance.output_nets = netsFor(output_offsets);
                board.custom_instances.push_back(std::move(instance));
            }
        }
        return true;
    }

    uint64_t readNet(const Board& board, uint32_t net) const {
        auto it = net_values_.find(net);
        if (it != net_values_.end()) return it->second;
        if (net >= board.nets.size() || !board.nets[net].has_state) return 0;
        return (*state_)[board.nets[net].state_index] & 1;
    }

    void writeNet(Board& board, uint32_t net, uint64_t value) {
        if (net >= board.nets.size()) return;
        net_values_[net] = value;
        if (board.nets[net].has_state) {
            (*state_)[board.nets[net].state_index] =
                static_cast<unsigned char>(value & 0xff);
        }
    }

    const TCHost* host_ = nullptr;
    unsigned char** state_ = nullptr;
    unsigned char** input_replay_ = nullptr;
    unsigned char** output_history_ = nullptr;
    int64_t (*get_setting_)(uint8_t) = nullptr;
    void (*set_setting_)(uint8_t, int64_t) = nullptr;
    std::vector<TCCustomLogicComponent> components_;
    std::map<uint32_t, uint64_t> net_values_;
    bool failed_ = false;
};

}  // namespace tc

#endif  // TC_CUSTOM_LOGIC_H
