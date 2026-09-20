#pragma once
#include "../sdk/tc_logic_api.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

namespace tc::component_definition {
inline bool valid(const TCNativeComponentDefinition* d) {
    if (!d || d->size != sizeof(*d) || !d->custom_id || !d->callback ||
        !d->name || !*d->name || std::strlen(d->name) > 65535 ||
        !d->inputs || !d->outputs || d->input_count < 1 || d->input_count > 8 ||
        d->output_count < 1 || d->output_count > 8 ||
        d->gate_cost > INT64_MAX || d->delay > INT64_MAX) return false;
    unsigned total = 0;
    for (unsigned dir = 0; dir < 2; ++dir) {
        auto pins = dir ? d->outputs : d->inputs;
        auto count = dir ? d->output_count : d->input_count;
        for (unsigned i = 0; i < count; ++i) {
            if (!pins[i].name || std::strlen(pins[i].name) > 65535 ||
                pins[i].bits < 1 || pins[i].bits > 64) return false;
            if (!dir) total += pins[i].bits;
        }
    }
    return total <= 128;
}
struct Writer {
    std::vector<uint8_t> bytes;
    void number(uint64_t n, unsigned size) {
        for (unsigned i = 0; i < size; ++i) bytes.push_back(uint8_t(n >> (i * 8)));
    }
    void string(const char* s) {
        const size_t n = std::strlen(s); number(n, 2);
        bytes.insert(bytes.end(), s, s + n);
    }
    void component(unsigned kind, int x, int y, uint64_t id, const char* name,
                   unsigned bits, int ordinal = 0) {
        number(kind, 2); number(x, 2); number(y, 2); number(0, 1); number(id, 8);
        string(name); number(ordinal ? 1 : 0, 2);
        if (ordinal) number(kind == 0x4f ? 2 : 0, 8);
        number(0, 8); number(-2 * ordinal, 2); number(bits, 8);
        number(0, 1); number(UINT64_MAX, 8); number(0, 8); number(0, 1);
        number(0, 1); number(0, 2); number(0, 2);
    }
};
struct Point { int x, y; };
// Version 3 scaffold contract: N unary input collectors, N-1 dependency gates,
// M unary output drivers, in that serialized order. Only output drivers are
// replaced. The dependency chain schedules every input before the callback.
inline std::vector<uint8_t> encode(const TCNativeComponentDefinition& d) {
    if (!valid(&d)) return {};
    const unsigned n = d.input_count, m = d.output_count;
    Writer w;
    w.number(d.custom_id, 8); w.number(0, 4); w.number(d.gate_cost, 8); w.number(d.delay, 8);
    w.number(1, 1); w.number(10000, 8); w.number(0, 2); w.string("");
    w.number(0, 1); w.number(0, 2); w.number(0, 2); w.string("");
    for (int i = 0; i < 512; ++i) w.number(0, 1);
    w.number(n + m + n + n - 1 + m, 8);
    auto inputY = [n](unsigned i) { return int(i) * 8 - int(n - 1) * 4; };
    for (unsigned i = 0; i < n; ++i)
        w.component(0x4f, -18, inputY(i), 0x1000 + i, d.inputs[i].name, d.inputs[i].bits, i + 1);
    for (unsigned i = 0; i < n; ++i)
        w.component(0x12, -6, inputY(i), 0x2000 + i, "", d.inputs[i].bits);
    for (unsigned i = 1; i < n; ++i)
        w.component(0x17, 40 + int(i) * 12, 100 + int(i) * 8, 0x3000 + i, "", 64);
    for (unsigned i = 0; i < m; ++i)
        w.component(0x12, 4, int(i) * 8, 0x4000 + i, "", d.outputs[i].bits);
    for (unsigned i = 0; i < m; ++i)
        w.component(0x51, 13, int(i) * 8, 0x5000 + i, d.outputs[i].name, d.outputs[i].bits, i + 1);
    Writer wires; unsigned count = 0;
    auto wire = [&](std::initializer_list<Point> points) {
        ++count; wires.number(0, 1); wires.string("");
        auto p = points.begin(); Point last = *p++;
        wires.number(last.x, 2); wires.number(last.y, 2);
        for (; p != points.end(); ++p) {
            if (p->x != last.x) wires.number((p->x > last.x ? 0 : 0x8000) | std::abs(p->x - last.x), 2);
            if (p->y != last.y) wires.number((p->y > last.y ? 0x4000 : 0xc000) | std::abs(p->y - last.y), 2);
            last = *p;
        }
        wires.number(0, 2);
    };
    for (unsigned i = 0; i < n; ++i) wire({{-15, inputY(i)}, {-7, inputY(i)}});
    Point previous{-4, inputY(0)};
    for (unsigned i = 1; i < n; ++i) {
        const int x = 40 + int(i) * 12, y = 100 + int(i) * 8;
        wire({previous, {x - 3, previous.y}, {x - 3, y - 1}, {x - 1, y - 1}});
        wire({{-4, inputY(i)}, {x - 5, inputY(i)}, {x - 5, y + 1}, {x - 1, y + 1}});
        previous = {x + 2, y};
    }
    wire({previous, {previous.x + 4, previous.y}, {previous.x + 4, -80}, {3, -80}, {3, 0}});
    for (unsigned i = 0; i < m; ++i) {
        const int y = int(i) * 8;
        wire({{6, y}, {10, y}});
        if (i + 1 < m) wire({{6, y}, {8, y}, {8, y + 4}, {3, y + 4}, {3, y + 8}});
    }
    w.number(count, 8); w.bytes.insert(w.bytes.end(), wires.bytes.begin(), wires.bytes.end());
    std::vector<uint8_t> result{14};
    size_t size = w.bytes.size();
    do { uint8_t b = size & 127; size >>= 7; result.push_back(b | (size ? 128 : 0)); } while (size);
    for (size_t pos = 0; pos < w.bytes.size();) {
        const auto length = std::min<size_t>(60, w.bytes.size() - pos);
        result.push_back(uint8_t((length - 1) << 2));
        result.insert(result.end(), w.bytes.begin() + pos, w.bytes.begin() + pos + length);
        pos += length;
    }
    return result;
}
}
