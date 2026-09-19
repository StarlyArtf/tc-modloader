#ifndef TC_EXAMPLE_BYTE_ADDER_FIXTURE_HPP
#define TC_EXAMPLE_BYTE_ADDER_FIXTURE_HPP

// Serialized component definition for the byte-adder example.  Shape:
//   Carry in (1 bit) + A (8 bit) + B (8 bit)  ->  Sum (8 bit) + Carry out (1 bit)
// The internal circuit is only a placeholder: the loader replaces its two gates
// with the plugin callback, so the behaviour comes from C++.
//
// The header carries the cached design statistics that the game shows in the
// component panel: 1 gate, 1 delay.  The game recomputes the gate count while
// parsing but keeps the delay verbatim, and the loader uses both for board-level
// compile statistics.

#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>

namespace tc_example {

constexpr uint64_t kByteAdderId = 0x414444385F303031ULL;  // "ADD8_001"
constexpr int64_t kDeclaredGates = 1;
constexpr int64_t kDeclaredDelay = 1;

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
    void sequence_i64(const std::vector<int64_t>& values) {
        u16(static_cast<uint16_t>(values.size()));
        for (int64_t value : values) i64(value);
    }
    void sequence_u8(const std::vector<uint8_t>& values) {
        u16(static_cast<uint16_t>(values.size()));
        add(values.data(), values.size());
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

// v14 component tail.  Order matters: the game rejects a multi-pin definition
// when these fields are written in another order.
inline void writeTail(Writer& writer) {
    writer.u8(0);    // bool_a
    writer.i64(-1);  // value_i64_a
    writer.i64(0);   // value_i64_b
    writer.u8(0);    // bool_b
    writer.u8(0);    // init data
    writer.u16(0);   // subcomponents
    writer.u16(0);   // settings
}

// Definition pins carry a one-element value list: the pin ordinal for inputs
// (2, 4, 6 ...) and 0 for outputs.
inline void addPin(Writer& writer, uint16_t kind, int16_t x, int16_t y,
                   uint64_t identity, const char* name, int16_t pin_index,
                   int64_t bits, int64_t pin_value) {
    writer.u16(kind);
    writer.i16(x);
    writer.i16(y);
    writer.u8(0);
    writer.i64(static_cast<int64_t>(identity));
    writer.string(name);
    writer.sequence_i64({pin_value});
    writer.i64(0);
    writer.i16(pin_index);
    writer.i64(bits);
    writeTail(writer);
}

inline void addGate(Writer& writer, uint16_t kind, int16_t x, int16_t y,
                    uint64_t identity, int64_t bits) {
    writer.u16(kind);
    writer.i16(x);
    writer.i16(y);
    writer.u8(0);
    writer.i64(static_cast<int64_t>(identity));
    writer.string("");
    writer.u16(0);
    writer.i64(0);
    writer.i16(0);
    writer.i64(bits);
    writeTail(writer);
}

inline void addWire(Writer& writer, int16_t x, int16_t y,
                    std::initializer_list<uint16_t> segments) {
    writer.u8(0);
    writer.string("");
    writer.i16(x);
    writer.i16(y);
    for (uint16_t segment : segments) writer.u16(segment);
}

inline std::vector<uint8_t> buildDefinitionPayload() {
    Writer writer;
    writer.i64(static_cast<int64_t>(kByteAdderId));
    writer.u32(0);
    writer.i64(kDeclaredGates);  // cached gate count shown in the panel
    writer.i64(kDeclaredDelay);  // cached delay; the loader also uses it for timing
    writer.u8(1);
    writer.i64(10000);
    writer.sequence_i64({});
    writer.string("");
    writer.u8(0);
    writer.u16(0);
    writer.sequence_u8({});
    writer.string("");
    for (int i = 0; i < 512; ++i) writer.u8(0);  // default wire palette

    writer.i64(7);  // components
    addPin(writer, 0x4F, -18, -8, 0x1000000000000000ULL, "Carry in", -2, 1, 2);
    addPin(writer, 0x4F, -18, 0, 0x1000000000000001ULL, "A", -4, 8, 2);
    addPin(writer, 0x4F, -18, 8, 0x1000000000000002ULL, "B", -6, 8, 2);
    addGate(writer, 0x2A, 2, 0, 0x2000000000000002ULL, 8);  // Mux (placeholder)
    addGate(writer, 0x12, 4, 4, 0x2000000000000003ULL, 1);  // NOT  (placeholder)
    addPin(writer, 0x51, 13, 0, 0x1000000000000003ULL, "Sum", -2, 8, 0);
    addPin(writer, 0x51, 13, 8, 0x1000000000000004ULL, "Carry out", -4, 1, 0);

    writer.i64(6);  // wires
    addWire(writer, -15, -8, {0x0010, 0x4007, 0x0000});  // Carry in -> Mux in2
    addWire(writer, -15, 0, {0x0010, 0x0000});           // A -> Mux in0
    addWire(writer, -15, 8, {0x0010, 0xC007, 0x0000});   // B -> Mux in1
    addWire(writer, 4, 0, {0x4004, 0x8001, 0x0000});     // Mux out -> NOT in
    addWire(writer, 4, 0, {0x0006, 0x0000});             // Mux out -> Sum
    addWire(writer, 6, 4, {0x0004, 0x4004, 0x0000});     // NOT out -> Carry out
    return writer.bytes;
}

// circuit.data is one version byte followed by a raw Snappy block.
inline std::vector<uint8_t> buildDefinitionFile() {
    const auto raw = buildDefinitionPayload();
    std::vector<uint8_t> encoded;
    encoded.push_back(14);
    writeLiteralSnappy(raw, encoded);
    return encoded;
}

}  // namespace tc_example

#endif  // TC_EXAMPLE_BYTE_ADDER_FIXTURE_HPP
