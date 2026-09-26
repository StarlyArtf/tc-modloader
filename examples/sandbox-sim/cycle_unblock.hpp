#pragma once

/* Lets the stock game finish building display/state metadata for a cyclic
   sandbox board. It temporarily moves one endpoint of each DFS back edge in
   the table the game is inspecting. The caller restores every endpoint as soon
   as the cycle-check or preorder pass returns. No emitted source is changed. */

#include "../../simcore/include/tcsim/board.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace sandbox_cycle_unblock {

constexpr uint64_t kComponentStride = 0x238;
constexpr uint64_t kWireStride = 0x68;
constexpr uint64_t kHeader = 8;

struct Table {
    uint64_t count = 0;
    unsigned char* payload = nullptr;
};

inline bool table(void* address, uint64_t limit, Table& out) {
    out = Table{};
    if (!address) return false;
    std::memcpy(&out.count, address, sizeof(out.count));
    std::memcpy(&out.payload, static_cast<unsigned char*>(address) + 8, sizeof(out.payload));
    return out.count > 0 && out.count <= limit && out.payload;
}

inline int16_t i16(const unsigned char* bytes, size_t offset) {
    int16_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

inline uint64_t u64(const unsigned char* bytes, size_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

struct Cut {
    unsigned char* wire = nullptr;
    int16_t x2 = 0;
    int16_t y2 = 0;
    uint64_t index = 0;
};

inline void restore(std::vector<Cut>& cuts) {
    for (Cut& cut : cuts) {
        if (!cut.wire) continue;
        std::memcpy(cut.wire + 0x1c, &cut.x2, sizeof(cut.x2));
        std::memcpy(cut.wire + 0x1e, &cut.y2, sizeof(cut.y2));
    }
    cuts.clear();
}

struct Pin {
    size_t component = 0;
    bool input = false;
};

struct Edge {
    size_t producer = 0;
    size_t consumer = 0;
    uint64_t wire = 0;
};

using Geometry = std::function<bool(const tcsim::BoardComponent&, tcsim::KindPins&)>;

inline std::vector<Cut> cutTables(void* component_table, void* wire_table,
                                  const Geometry& geometry) {
    Table components;
    Table wires;
    if (!table(component_table, 4096, components) || !table(wire_table, 65536, wires)) return {};

    std::vector<tcsim::BoardComponent> decoded;
    decoded.reserve(static_cast<size_t>(components.count));
    std::map<std::pair<int, int>, std::vector<Pin>> pins;
    for (uint64_t index = 0; index < components.count; ++index) {
        const unsigned char* record = components.payload + kHeader + index * kComponentStride;
        tcsim::BoardComponent component;
        component.kind = record[0];
        component.x = i16(record, 2);
        component.y = i16(record, 4);
        component.rotation = record[6];
        component.id = u64(record, 8);
        component.input_count = static_cast<uint8_t>(u64(record, 0x30));
        component.output_count = static_cast<uint8_t>(u64(record, 0x40));
        component.custom_id = u64(record, 0x188);
        decoded.push_back(component);
        if (component.kind == 0) continue;
        tcsim::KindPins shape;
        if (!geometry(component, shape)) continue;
        for (uint8_t pin = 0; pin < shape.input_count; ++pin) {
            const auto offset = tcsim::rotateOffset(shape.in_x[pin], shape.in_y[pin], component.rotation);
            pins[{component.x + offset.first, component.y + offset.second}].push_back(
                Pin{static_cast<size_t>(index), true});
        }
        for (uint8_t pin = 0; pin < shape.output_count; ++pin) {
            const auto offset = tcsim::rotateOffset(shape.out_x[pin], shape.out_y[pin], component.rotation);
            pins[{component.x + offset.first, component.y + offset.second}].push_back(
                Pin{static_cast<size_t>(index), false});
        }
    }

    const auto unique = [&pins](int x, int y, bool input, Pin& out) {
        const auto found = pins.find({x, y});
        if (found == pins.end()) return false;
        size_t matches = 0;
        for (const Pin& pin : found->second) {
            if (pin.input != input) continue;
            out = pin;
            ++matches;
        }
        return matches == 1;
    };

    std::vector<Edge> edges;
    for (uint64_t index = 0; index < wires.count; ++index) {
        const unsigned char* wire = wires.payload + kHeader + index * kWireStride;
        Pin a_out, a_in, b_out, b_in;
        const int x1 = i16(wire, 0x18), y1 = i16(wire, 0x1a);
        const int x2 = i16(wire, 0x1c), y2 = i16(wire, 0x1e);
        if (unique(x1, y1, false, a_out) && unique(x2, y2, true, b_in) &&
            a_out.component != b_in.component) {
            edges.push_back(Edge{a_out.component, b_in.component, index});
        } else if (unique(x2, y2, false, b_out) && unique(x1, y1, true, a_in) &&
                   b_out.component != a_in.component) {
            edges.push_back(Edge{b_out.component, a_in.component, index});
        }
    }

    std::vector<std::vector<size_t>> outgoing(decoded.size());
    for (size_t index = 0; index < edges.size(); ++index) {
        if (edges[index].producer < outgoing.size()) outgoing[edges[index].producer].push_back(index);
    }
    std::vector<uint8_t> state(decoded.size(), 0);
    std::set<uint64_t> cut_wires;
    std::function<void(size_t)> visit = [&](size_t component) {
        state[component] = 1;
        for (size_t edge_index : outgoing[component]) {
            const Edge& edge = edges[edge_index];
            if (edge.consumer >= state.size()) continue;
            if (state[edge.consumer] == 1) {
                cut_wires.insert(edge.wire);
                continue;
            }
            if (state[edge.consumer] == 0) visit(edge.consumer);
        }
        state[component] = 2;
    };
    for (size_t component = 0; component < state.size(); ++component) {
        if (state[component] == 0) visit(component);
    }

    std::vector<Cut> cuts;
    for (uint64_t index : cut_wires) {
        unsigned char* wire = wires.payload + kHeader + index * kWireStride;
        Cut cut;
        cut.wire = wire;
        cut.x2 = i16(wire, 0x1c);
        cut.y2 = i16(wire, 0x1e);
        cut.index = index;
        const int16_t moved = static_cast<int16_t>(cut.x2 - 64);
        std::memcpy(wire + 0x1c, &moved, sizeof(moved));
        cuts.push_back(cut);
    }
    return cuts;
}

inline std::vector<Cut> cutBoard(void* board, const Geometry& geometry) {
    if (!board) return {};
    auto* bytes = static_cast<unsigned char*>(board);
    return cutTables(bytes + 0x78, bytes + 0x98, geometry);
}

}  // namespace sandbox_cycle_unblock
