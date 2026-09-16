#ifndef TC_EXAMPLE_AND_FIXTURE_HPP
#define TC_EXAMPLE_AND_FIXTURE_HPP

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace tc_example {

constexpr uint64_t kAndComponentId = 0x414E44325F303031ULL;

// Cached design statistics stored in the definition header.  The registered
// prototype keeps this delay verbatim, so it must be the real critical path.
constexpr int64_t kDesignGates = 1;
constexpr int64_t kDesignDelay = 1;

struct Writer {
    std::vector<uint8_t> bytes;

    void add(const void* data, size_t size) {
        const auto* first = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), first, first + size);
    }
    void u8(uint8_t value) { bytes.push_back(value); }
    void u16(uint16_t value) { add(&value, sizeof(value)); }
    void i16(int16_t value) { add(&value, sizeof(value)); }
    void u32(uint32_t value) { add(&value, sizeof(value)); }
    void i64(int64_t value) { add(&value, sizeof(value)); }
    void string(const std::string& value) {
        u16(static_cast<uint16_t>(value.size()));
        add(value.data(), value.size());
    }
};

inline void writeLiteralSnappy(const std::vector<uint8_t>& raw,
                               std::vector<uint8_t>& output) {
    uint64_t remaining = raw.size();
    do {
        uint8_t byte = static_cast<uint8_t>(remaining & 0x7F);
        remaining >>= 7;
        if (remaining) byte |= 0x80;
        output.push_back(byte);
    } while (remaining);
    size_t offset = 0;
    while (offset < raw.size()) {
        const size_t chunk = std::min<size_t>(60, raw.size() - offset);
        output.push_back(static_cast<uint8_t>((chunk - 1) << 2));
        output.insert(output.end(), raw.begin() + offset,
                      raw.begin() + offset + chunk);
        offset += chunk;
    }
}

inline void addPin(Writer& writer, uint16_t kind, int16_t x, int16_t y,
                   uint64_t identity, const char* name, int16_t pin_index,
                   int64_t word_size) {
    writer.u16(kind);
    writer.i16(x);
    writer.i16(y);
    writer.u8(0);
    writer.i64(static_cast<int64_t>(identity));
    writer.string(name);
    writer.u16(0);
    writer.i64(0);
    writer.i16(pin_index);
    writer.i64(word_size);
    writer.u8(1);
    writer.u8(0);
    writer.i64(-1);
    writer.i64(0);
    writer.u8(0);
    writer.u16(0);
    writer.u16(0);
}

inline void addAndGate(Writer& writer) {
    writer.u16(0x04);
    writer.i16(4);
    writer.i16(-10);
    writer.u8(0);
    writer.i64(0x2000000000000002LL);
    writer.string("");
    writer.u16(0);
    writer.i64(0);
    writer.i16(0);
    writer.i64(1);
    writer.u8(0);
    writer.u8(0);
    writer.i64(-1);
    writer.i64(0);
    writer.u8(0);
    writer.u16(0);
    writer.u16(0);
}

inline void addWire(Writer& writer, int16_t x, int16_t y,
                    std::initializer_list<uint16_t> segments) {
    writer.u8(0);
    writer.string("");
    writer.i16(x);
    writer.i16(y);
    for (uint16_t segment : segments) writer.u16(segment);
}

inline std::vector<uint8_t> buildAndComponentPayload() {
    Writer writer;
    writer.i64(static_cast<int64_t>(kAndComponentId));
    writer.u32(0);
    // The definition header stores the design's cached (gate count, delay).
    // The game recomputes the gate count on load but trusts the stored delay,
    // so a definition must carry its real critical path.  One AND gate is
    // (1, 1); see the shipped hub designs for reference values.
    writer.i64(kDesignGates);
    writer.i64(kDesignDelay);
    writer.u8(1);
    writer.i64(10000);
    writer.u16(0);
    writer.string("");
    writer.u8(0);
    writer.u16(0);
    writer.u16(0);
    writer.string("");
    for (int i = 0; i < 512; ++i) writer.u8(0);

    writer.i64(4);
    addPin(writer, 0x4F, -11, -13, 0x1000000000000000ULL, "A", -2, 1);
    addPin(writer, 0x4F, -11, -7, 0x1000000000000001ULL, "B", -4, 1);
    addAndGate(writer);
    addPin(writer, 0x51, 14, -10, 0x1000000000000003ULL, "Out", -2, 1);

    writer.i64(3);
    addWire(writer, -8, -13, {0x000B, 0x4002, 0x0000});
    addWire(writer, -8, -7, {0x000B, 0xC002, 0x0000});
    addWire(writer, 6, -10, {0x0005, 0x0000});
    return writer.bytes;
}

inline std::vector<uint8_t> buildAndCircuitFile() {
    const auto raw = buildAndComponentPayload();
    std::vector<uint8_t> encoded;
    encoded.push_back(14);
    writeLiteralSnappy(raw, encoded);
    return encoded;
}

}  // namespace tc_example

#endif  // TC_EXAMPLE_AND_FIXTURE_HPP
