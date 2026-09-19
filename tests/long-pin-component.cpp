/* Writes a *board* (level save) in the version-3 format tests/and-component-fixture.cpp
   uses, holding input IO components along the top edge and output IO components
   along the bottom edge, each with a deliberately long name - the case a player
   reports: in the component workshop's preview those labels are drawn
   horizontally side by side and overlap.

   The board format (leading 13, the magic header and the 512-byte block) is the
   one the game accepts as a level's circuit.data; a *component* file (leading 14,
   custom id first) is rejected here and the level comes up empty, which is what
   an earlier version of this generator did.

   Usage: long-pin-component.exe <output-path> */
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>

namespace {
struct Writer {
    std::vector<uint8_t> bytes;
    void number(uint64_t n, unsigned size) {
        for (unsigned i = 0; i < size; ++i) bytes.push_back(uint8_t(n >> (i * 8)));
    }
    void string(const char* s) {
        const size_t n = std::strlen(s);
        number(n, 2);
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

const char* const kInputs[] = {"aaaaaaaaaa", "bbbbbbbbbb", "cccccccccc", "dddddddddd",
                               "eeeeeeeeee", "ffffffffff"};
const char* const kOutputs[] = {"gggggggggg", "hhhhhhhhhh"};
constexpr int kInputCount = 6, kOutputCount = 2;

std::vector<uint8_t> build() {
    Writer w;
    /* Component scaffold header, copied from src/component_definition.hpp: a
       *component* file (leading 14, custom id first) is what the workshop level's
       variants are, while a level's own save is the board format (leading 13). */
    w.number(0x4c4f4e47504e3032ULL, 8);   /* "LONGPN02" */
    w.number(0, 4); w.number(1, 8); w.number(1, 8);
    w.number(1, 1); w.number(10000, 8); w.number(0, 2); w.string("");
    w.number(0, 1); w.number(0, 2); w.number(0, 2); w.string("");
    for (int i = 0; i < 512; ++i) w.number(0, 1);
    w.number(kInputCount + kOutputCount, 8);
    auto spread = [](int index, int count) { return index * 8 - (count - 1) * 4; };
    /* Inputs along the top row, outputs along the bottom row: that is the layout
       whose labels collide in the preview. */
    for (int i = 0; i < kInputCount; ++i)
        w.component(0x4f, spread(i, kInputCount), -18, 0x1000 + i, kInputs[i], 1, i + 1);
    for (int i = 0; i < kOutputCount; ++i)
        w.component(0x51, spread(i, kOutputCount), 18, 0x5000 + i, kOutputs[i], 1, i + 1);
    Writer wires;
    unsigned count = 0;
    auto wire = [&](std::initializer_list<Point> points) {
        ++count;
        wires.number(0, 1); wires.string("");
        auto p = points.begin();
        Point last = *p++;
        wires.number(last.x, 2); wires.number(last.y, 2);
        for (; p != points.end(); ++p) {
            if (p->x != last.x)
                wires.number((p->x > last.x ? 0 : 0x8000) | std::abs(p->x - last.x), 2);
            if (p->y != last.y)
                wires.number((p->y > last.y ? 0x4000 : 0xc000) | std::abs(p->y - last.y), 2);
            last = *p;
        }
        wires.number(0, 2);
    };
    w.number(count, 8);
    w.bytes.insert(w.bytes.end(), wires.bytes.begin(), wires.bytes.end());
    return w.bytes;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: long-pin-component <output-path>\n");
        return 1;
    }
    const std::vector<uint8_t> bytes = build();
    std::ofstream file(argv[1], std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file ? 0 : 2;
}
