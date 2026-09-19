// Build the pinned game's v14 circuit.data fixture for a two-input AND
// component.  This is intentionally a small, dependency-free generator:
// the game accepts a raw Snappy stream consisting only of literal chunks.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kAndComponentId = 0x414E44325F303031ULL;
constexpr uint64_t kInnerComponentId = 0x414E44325F303032ULL;
constexpr uint16_t kInputPin = 0x4F;
constexpr uint16_t kOutputPin = 0x51;
constexpr uint16_t kAndGate = 0x04;
constexpr uint16_t kDelayLine = 0x0D;  // 1-bit Delay Line (stateful)

// The definition header carries the design's cached (gate count, delay) pair.
// The game recomputes the gate count when the definition is parsed, but it
// trusts the stored delay verbatim, so a definition must carry its real
// critical path.  Reference values from the shipped hub designs:
//   foundry/4or      -> (3, 2)     foundry/8or   -> (7, 3)
//   foundry/1and8    -> (8, 1)     foundry/half-add -> (4, 2)
// This AND is a single gate, so the correct pair is (1, 1); the command line
// can still override both fields for controlled experiments.
int64_t kMetaGates = 1;
int64_t kMetaDelay = 1;

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

void writeLiteralSnappy(const std::vector<uint8_t>& raw,
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

uint64_t readVarint(const std::vector<uint8_t>& data, size_t& offset) {
    uint64_t value = 0;
    unsigned shift = 0;
    while (offset < data.size() && shift < 64) {
        const uint8_t byte = data[offset++];
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return value;
        shift += 7;
    }
    return value;
}

bool decodeLiteralSnappy(const std::vector<uint8_t>& encoded,
                         std::vector<uint8_t>& raw) {
    size_t offset = 0;
    const uint64_t expected = readVarint(encoded, offset);
    while (offset < encoded.size() && raw.size() < expected) {
        const uint8_t tag = encoded[offset++];
        if ((tag & 0x03) != 0) return false;
        uint64_t length = (tag >> 2) + 1;
        if (length > 60) {
            const size_t extra = static_cast<size_t>(length - 60);
            if (offset + extra > encoded.size()) return false;
            length = 0;
            for (size_t i = 0; i < extra; ++i)
                length |= static_cast<uint64_t>(encoded[offset++]) << (8 * i);
            length += 1;
        }
        if (offset + length > encoded.size() ||
            raw.size() + length > expected) {
            return false;
        }
        raw.insert(raw.end(), encoded.begin() + offset,
                   encoded.begin() + offset + length);
        offset += length;
    }
    return raw.size() == expected && offset == encoded.size();
}

void addPin(Writer& writer, uint16_t kind, int16_t x, int16_t y,
            uint64_t identity, const char* name, int16_t pin_index,
            int64_t word_size) {
    writer.u16(kind);
    writer.i16(x);
    writer.i16(y);
    writer.u8(0);  // flags
    writer.i64(static_cast<int64_t>(identity));
    writer.string(name);
    writer.u16(0);  // values
    writer.i64(0);  // data
    writer.i16(pin_index);
    writer.i64(word_size);
    writer.u8(1);  // custom port
    writer.u8(0);
    writer.i64(-1);
    writer.i64(0);
    writer.u8(0);  // init_data
    writer.u16(0);  // subcomponents
    writer.u16(0);  // settings
}

void addAndGate(Writer& writer, bool nested=false) {
    writer.u16(nested?0x4e:kAndGate);
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
    if(nested) { writer.i64(static_cast<int64_t>(kInnerComponentId));writer.u16(0); }
}

// 1-bit Delay Line.  Its ports are not declared in the prototype's input list,
// so the geometry comes from the shipped campaign solution
// (campaign/double_buffer/hint_solution.data): input at (-3,0), output at
// (+3,0) relative to the component position.
void addDelayLine(Writer& writer, int16_t x, int16_t y) {
    writer.u16(kDelayLine);
    writer.i16(x);
    writer.i16(y);
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

void addWire(Writer& writer, int16_t x, int16_t y,
             std::initializer_list<uint16_t> segments) {
    writer.u8(0);
    writer.string("");
    writer.i16(x);
    writer.i16(y);
    for (uint16_t segment : segments) writer.u16(segment);
}

// v14 component tail: bool_a, value_i64_a, value_i64_b, bool_b, init_data,
// subcomponents, settings.  The game's own definitions carry bool_a=false and
// value_i64_a=-1; a definition with three input pins is rejected when these
// fields are written in the wrong order, which the older fixture writer did.
void writeComponentTail(Writer& writer) {
    writer.u8(0);   // bool_a
    writer.i64(-1); // value_i64_a
    writer.i64(0);  // value_i64_b
    writer.u8(0);   // bool_b
    writer.u8(0);   // init_data
    writer.u16(0);  // subcomponents
    writer.u16(0);  // settings
}

// Definition pins in the game's own files carry a one-element value sequence:
// the pin ordinal for inputs (2, 4, 6, ...) and 0 for outputs.  The older AND
// fixture writes an empty sequence, which the game only tolerates for one and
// two input pins.
void addLogicPin(Writer& writer, uint16_t kind, int16_t x, int16_t y,
                 uint64_t identity, const char* name, int16_t pin_index,
                 int64_t word_size, int64_t pin_value) {
    writer.u16(kind);
    writer.i16(x);
    writer.i16(y);
    writer.u8(0);
    writer.i64(static_cast<int64_t>(identity));
    writer.string(name);
    writer.sequence_i64({pin_value});
    writer.i64(0);
    writer.i16(pin_index);
    writer.i64(word_size);
    writeComponentTail(writer);
}

// Same field layout as addAndGate, with the gate kind and position chosen by
// the caller.  Used by the interface-shape definitions below.
void addGateAt(Writer& writer, uint16_t kind, int16_t x, int16_t y,
               int64_t bits = 1) {
    writer.u16(kind);
    writer.i16(x);
    writer.i16(y);
    writer.u8(0);
    writer.i64(0x2000000000000002LL);
    writer.string("");
    writer.u16(0);
    writer.i64(0);
    writer.i16(0);
    writer.i64(bits);
    writeComponentTail(writer);
}

// Component definitions used by the interface-shape scenarios.  Their internal
// circuits are placeholders: the loader replaces the gate lines, so only the pin
// count, the pin order and the operand tuple matter.
//   not1   1 input,  1 output : A -> NOT -> Out
//   and3   3 inputs, 1 output : three-input AND node
//   adder  3 inputs, 2 outputs: two gates fed from the same pin set
std::vector<uint8_t> buildNativeLogicDefinition(const std::string& kind) {
    const bool not1=kind=="not1",and3=kind=="and3",double8=kind=="double8";
    const bool xor8=kind=="xor8",mux8=kind=="mux8",asr8=kind=="asr8",adder8=kind=="adder8";
    Writer writer;
    writer.i64(static_cast<int64_t>(not1?0x4E4F54315F303031ULL:
                                    and3?0x414E44335F303031ULL:
                                    double8?0x44424C385F303031ULL:
                                    xor8?0x584F52385F303031ULL:
                                    mux8?0x4D5558385F303031ULL:
                                    asr8?0x415352385F303031ULL:
                                    adder8?0x414444385F303031ULL:
                                            0x414444525F303031ULL));
    writer.u32(0);
    writer.i64(1);   // cached gate count
    writer.i64(1);   // cached critical-path delay
    writer.u8(1);
    writer.i64(10000);
    writer.sequence_i64({});
    writer.string("");
    writer.u8(0);
    writer.u16(0);
    writer.sequence_u8({});
    writer.string("");
    for (int i = 0; i < 512; ++i) writer.u8(0);

    if(double8) {
        // One 8-bit input pin and one 8-bit output pin: the loader passes the
        // whole byte to the callback instead of a single bit.
        writer.i64(3);
        addLogicPin(writer, kInputPin, -18, 0, 0x1000000000000000ULL, "In", -2, 8, 2);
        addGateAt(writer, 0x12, 4, 0, 8);  // NOT (auto width), placeholder
        addLogicPin(writer, kOutputPin, 13, 0, 0x1000000000000003ULL, "Out", -2, 8, 0);
        writer.i64(2);
        addWire(writer, -15, 0, {0x0012, 0x0000});
        addWire(writer, 6, 0, {0x0004, 0x0000});
    } else if(xor8) {
        // Two 8-bit inputs -> one 8-bit output (placeholders only).
        writer.i64(4);
        addLogicPin(writer, kInputPin, -18, -4, 0x1000000000000000ULL, "A", -2, 8, 2);
        addLogicPin(writer, kInputPin, -18, 4, 0x1000000000000001ULL, "B", -4, 8, 2);
        addGateAt(writer, 0x17, 4, 0, 8);  // XOR (auto width)
        addLogicPin(writer, kOutputPin, 13, 0, 0x1000000000000003ULL, "Out", -2, 8, 0);
        writer.i64(3);
        addWire(writer, -15, -4, {0x0012, 0x4003, 0x0000});
        addWire(writer, -15, 4, {0x0012, 0xC003, 0x0000});
        addWire(writer, 6, 0, {0x0004, 0x0000});
    } else if(mux8) {
        // Three 8-bit inputs -> one 8-bit output, using the game's own Mux as
        // the three-operand placeholder.
        writer.i64(5);
        addLogicPin(writer, kInputPin, -18, -8, 0x1000000000000000ULL, "Select", -2, 8, 2);
        addLogicPin(writer, kInputPin, -18, 0, 0x1000000000000001ULL, "A", -4, 8, 2);
        addLogicPin(writer, kInputPin, -18, 8, 0x1000000000000002ULL, "B", -6, 8, 2);
        addGateAt(writer, 0x2a, 4, 0, 8);  // Mux (auto width), 3 inputs
        addLogicPin(writer, kOutputPin, 13, 0, 0x1000000000000003ULL, "Out", -2, 8, 0);
        writer.i64(4);
        addWire(writer, -15, -8, {0x0012, 0x4007, 0x0000});
        addWire(writer, -15, 0, {0x0012, 0x0000});
        addWire(writer, -15, 8, {0x0012, 0xC007, 0x0000});
        addWire(writer, 6, 0, {0x0004, 0x0000});
    } else if(asr8) {
        // 8-bit input plus 3-bit shift input -> 8-bit output.
        writer.i64(4);
        addLogicPin(writer, kInputPin, -18, -4, 0x1000000000000000ULL, "In", -2, 8, 2);
        addLogicPin(writer, kInputPin, -18, 4, 0x1000000000000001ULL, "Shift", -4, 3, 2);
        addGateAt(writer, 0x17, 4, 0, 8);
        addLogicPin(writer, kOutputPin, 13, 0, 0x1000000000000003ULL, "Out", -2, 8, 0);
        writer.i64(3);
        addWire(writer, -15, -4, {0x0012, 0x4003, 0x0000});
        addWire(writer, -15, 4, {0x0012, 0xC003, 0x0000});
        addWire(writer, 6, 0, {0x0004, 0x0000});
    } else if(adder8) {
        // 1-bit carry in + two 8-bit operands -> 8-bit sum + 1-bit carry out.
        // The first node carries the operand tuple; the second only drives the
        // second output pin.
        writer.i64(7);
        addLogicPin(writer, kInputPin, -18, -8, 0x1000000000000000ULL, "Carry in", -2, 1, 2);
        addLogicPin(writer, kInputPin, -18, 0, 0x1000000000000001ULL, "A", -4, 8, 2);
        addLogicPin(writer, kInputPin, -18, 8, 0x1000000000000002ULL, "B", -6, 8, 2);
        addGateAt(writer, 0x2a, 2, 0, 8);  // Mux placeholder, 3 inputs
        addGateAt(writer, 0x12, 4, 4, 1);  // NOT placeholder for the carry pin
        addLogicPin(writer, kOutputPin, 13, 0, 0x1000000000000003ULL, "Sum", -2, 8, 0);
        addLogicPin(writer, kOutputPin, 13, 8, 0x1000000000000004ULL, "Carry out", -4, 1, 0);
        writer.i64(6);
        addWire(writer, -15, -8, {0x0010, 0x4007, 0x0000});
        addWire(writer, -15, 0, {0x0010, 0x0000});
        addWire(writer, -15, 8, {0x0010, 0xC007, 0x0000});
        addWire(writer, 4, 0, {0x4004, 0x8001, 0x0000});  // Mux out -> NOT in
        addWire(writer, 4, 0, {0x0006, 0x0000});           // Mux out -> Sum
        addWire(writer, 6, 4, {0x0004, 0x4004, 0x0000});   // NOT out -> Carry out
    } else if(not1) {
        writer.i64(3);
        addLogicPin(writer, kInputPin, -11, -10, 0x1000000000000000ULL, "A", -2, 1, 2);
        addGateAt(writer, 0x03, 4, -10);
        addLogicPin(writer, kOutputPin, 14, -10, 0x1000000000000003ULL, "Out", -2, 1, 0);
        writer.i64(2);
        addWire(writer, -8, -10, {0x000B, 0x0000});
        addWire(writer, 6, -10, {0x0005, 0x0000});
    } else if(and3) {
        // Multi-pin definitions follow the geometry the game writes itself:
        // inputs on a left column eight units apart, one gate per output pin.
        // A three-input definition using the two-unit spacing of the older
        // two-pin fixture is rejected by the game's importer.
        writer.i64(5);
        addLogicPin(writer, kInputPin, -18, -10, 0x1000000000000000ULL, "A", -2, 1, 2);
        addLogicPin(writer, kInputPin, -18, -2, 0x1000000000000001ULL, "B", -4, 1, 2);
        addLogicPin(writer, kInputPin, -18, 6, 0x1000000000000002ULL, "C", -6, 1, 2);
        addGateAt(writer, 0x05, 4, -5);
        addLogicPin(writer, kOutputPin, 13, -5, 0x1000000000000003ULL, "Out", -2, 1, 0);
        writer.i64(4);
        addWire(writer, -15, -10, {0x0012, 0x4004, 0x0000});         // A -> in0
        addWire(writer, -15, -2, {0x0011, 0xC003, 0x0001, 0x0000});  // B -> in1
        addWire(writer, -15, 6, {0x0013, 0xC00A, 0x8001, 0x0000});   // C -> in2
        addWire(writer, 6, -5, {0x0004, 0x0000});                    // gate -> Out
    } else {
        // Three inputs, two outputs.  The first gate carries the operand tuple
        // that becomes the callback inputs; the second output pin only needs
        // its own driver, so it is fed from the first gate.
        writer.i64(7);
        addLogicPin(writer, kInputPin, -18, -10, 0x1000000000000000ULL, "A", -2, 1, 2);
        addLogicPin(writer, kInputPin, -18, -2, 0x1000000000000001ULL, "B", -4, 1, 2);
        addLogicPin(writer, kInputPin, -18, 6, 0x1000000000000002ULL, "C", -6, 1, 2);
        addGateAt(writer, 0x05, 4, -5);
        addGateAt(writer, 0x04, 6, -1);
        addLogicPin(writer, kOutputPin, 13, -5, 0x1000000000000003ULL, "Sum", -2, 1, 0);
        // Output pins also sit eight units apart, like the game's own
        // multi-output definitions.
        addLogicPin(writer, kOutputPin, 13, 3, 0x1000000000000004ULL, "Carry", -4, 1, 0);
        writer.i64(7);
        addWire(writer, -15, -10, {0x0012, 0x4004, 0x0000});         // A -> gate 1 in0
        addWire(writer, -15, -2, {0x0011, 0xC003, 0x0001, 0x0000});  // B -> gate 1 in1
        addWire(writer, -15, 6, {0x0013, 0xC00A, 0x8001, 0x0000});   // C -> gate 1 in2
        addWire(writer, 6, -5, {0x4003, 0x8001, 0x0000});            // gate 1 -> gate 2 in0
        addWire(writer, 6, -5, {0x4005, 0x8001, 0x0000});            // gate 1 -> gate 2 in1
        addWire(writer, 6, -5, {0x0004, 0x0000});                    // gate 1 -> Sum
        addWire(writer, 8, -1, {0x4004, 0x0002, 0x0000});            // gate 2 -> Carry
    }
    return writer.bytes;
}

void addV13Pin(Writer& writer, uint16_t kind, int16_t x, int16_t y,
               uint64_t identity, const char* name, int64_t bits) {
    writer.u16(kind);
    writer.i16(x);
    writer.i16(y);
    writer.u8(0);
    writer.i64(static_cast<int64_t>(identity));
    writer.string(name);
    writer.u16(0);
    writer.i64(0);
    writer.i16(0);
    writer.i64(bits);
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    writer.u16(0);
    writer.u16(0);
}

void addV13CustomInstance(Writer& writer, int16_t x=-5, int16_t y=0,
                         uint64_t identity=0x2222222222222222ULL,
                         uint64_t custom_id=kAndComponentId) {
    writer.u16(0x4E);
    writer.i16(x);
    writer.i16(y);
    writer.u8(0);
    writer.i64(static_cast<int64_t>(identity));
    writer.string("");
    writer.u16(0);
    writer.i64(0);
    writer.i16(0);
    writer.i64(1);
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    writer.u16(0);
    writer.u16(0);
    writer.i64(static_cast<int64_t>(custom_id));
    writer.u16(0);
}

std::string topology="single";
// Boards for tests/custom-or-playtest.ps1.  They contain only gates: the level
// itself already supplies its input and output components, and duplicating
// them in the schematic would put two output components on one net (the level
// test reads the last one while the UI shows the other).  Pin geometry:
//   level input 0x3f at (x,y)      -> (x,y-1), (x,y+1)
//   level output 0x44 at (x,y)     -> input (x-1,y)
//   built-in AND/OR/XOR at (x,y)   -> in (x-1,y-1), (x-1,y+1), out (x+2,y)
//   built-in NOT at (x,y)          -> in  (x-1,y),      out (x+2,y)
//   Mod component at (x,y)         -> in (x-1,y-1), (x-1,y), out (x+2,y-1)
std::vector<uint8_t> buildNativeLogicBoard(const std::string& kind) {
    if(kind=="wordnot") {
        // Diagnostic board for the double_number level that uses the game's own
        // word NOT instead of a Mod component, to tell a broken Mod bridge
        // apart from broken level plumbing.
        Writer writer;
        writer.i64(0x6f13c29bb2e19440LL);
        writer.u32(0);
        writer.i64(0);
        writer.i64(0);
        writer.u8(1);
        writer.i64(10000);
        writer.sequence_i64({});
        writer.string("");
        writer.u8(0);
        writer.u16(0);
        writer.sequence_u8({});
        writer.string("");
        for (int i = 0; i < 512; ++i) writer.u8(0);
        writer.i64(1);
        addGateAt(writer, 0x12, -6, 0, 8);
        writer.i64(2);
        addWire(writer, -12, 0, {0x0005, 0x0000});
        addWire(writer, -4, 0, {0x0010, 0x0000});
        return writer.bytes;
    }
    Writer writer;
    writer.i64(0x6f13c29bb2e19440LL);  // original and_gate seed
    writer.u32(0);
    writer.i64(0);
    writer.i64(0);
    writer.u8(1);
    writer.i64(10000);
    writer.sequence_i64({});
    writer.string("");
    writer.u8(0);
    writer.u16(0);
    writer.sequence_u8({});
    writer.string("");
    for (int i = 0; i < 512; ++i) writer.u8(0);

    if(kind=="multi") {
        // input -> two Mod instances of the same definition -> native AND ->
        // output.  Wires are laid out so that no two nets share a point.
        writer.i64(3);
        addV13CustomInstance(writer, -5, 0, 0x2222222222222222ULL);
        addV13CustomInstance(writer, -5, 6, 0x5555555555555555ULL);
        addV13Pin(writer, kAndGate, 2, 2, 0x4444444444444444ULL, "", 1);
        writer.i64(7);
        addWire(writer, -13, -1, {0x0007, 0x0000});                        // a -> Mod 1 in0
        addWire(writer, -13, 1, {0x0007, 0xC001, 0x0000});                 // b -> Mod 1 in1
        addWire(writer, -6, -1, {0x8009, 0x4006, 0x0009, 0x0000});         // a -> Mod 2 in0
        addWire(writer, -13, 1, {0x8006, 0x4007, 0x000D, 0xC002, 0x0000}); // b -> Mod 2 in1
        addWire(writer, -3, -1, {0x0004, 0x4002, 0x0000});                 // Mod 1 out -> AND in0
        addWire(writer, -3, 5, {0x0004, 0xC002, 0x0000});                  // Mod 2 out -> AND in1
        addWire(writer, 4, 2, {0x0008, 0xC002, 0x0000});                   // AND out -> Output
    } else if(kind=="mixed") {
        // input -> Mod instance -> native NOT -> output.  The level used for
        // this board (nor_gate) expects the negated callback result, so the
        // test passes only when both the callback and the native gate run.
        writer.i64(2);
        addV13CustomInstance(writer, -5, 0, 0x2222222222222222ULL);
        addV13Pin(writer, 0x03, 1, -1, 0x4444444444444444ULL, "", 1);
        writer.i64(4);
        addWire(writer, -13, -1, {0x0007, 0x0000});              // a -> Mod in0
        addWire(writer, -13, 1, {0x0007, 0xC001, 0x0000});       // b -> Mod in1
        addWire(writer, -3, -1, {0x0003, 0x0000});               // Mod out -> NOT
        addWire(writer, 3, -1, {0x0009, 0x4001, 0x0000});        // NOT -> Output
    } else {
        // input -> single Mod instance -> output
        const bool notBoard=kind=="not1board";
        const bool and3Board=kind=="and3board";
        const bool adderBoard=kind=="adderboard";
        const bool double8Board=kind=="double8board";
        const bool xor8Board=kind=="xor8board";
        const bool mux8Board=kind=="mux8board";
        const bool asr8Board=kind=="asr8board";
        const bool adder8Board=kind=="adder8board";
        writer.i64(1);
        addV13CustomInstance(writer, -5, adderBoard?1:0, 0x2222222222222222ULL,
                             notBoard?0x4E4F54315F303031ULL:
                             and3Board?0x414E44335F303031ULL:
                             adderBoard?0x414444525F303031ULL:
                             double8Board?0x44424C385F303031ULL:
                             xor8Board?0x584F52385F303031ULL:
                             mux8Board?0x4D5558385F303031ULL:
                             asr8Board?0x415352385F303031ULL:
                             adder8Board?0x414444385F303031ULL:kAndComponentId);
        if(notBoard) {
            // not_gate level: one input pin at (-12,0), output pin at (12,0).
            writer.i64(2);
            addWire(writer, -12, 0, {0xC001, 0x0006, 0x0000});
            addWire(writer, -3, -1, {0x000F, 0x4001, 0x0000});
        } else if(and3Board) {
            // and_gate_3 level: three input pins at (-13,-1),(-13,0),(-13,1)
            // and the output pin at (12,0).  The component's three inputs sit
            // at (-7,-1),(-7,0),(-7,1) for an instance at (-5,0).
            writer.i64(4);
            addWire(writer, -13, -1, {0x0006, 0x0000});
            addWire(writer, -13, 0, {0x0006, 0x0000});
            addWire(writer, -13, 1, {0x0006, 0x0000});
            addWire(writer, -3, 0, {0x000F, 0x0000});
        } else if(adderBoard) {
            // full_adder level: three single-pin inputs on the left and two
            // outputs (Sum at (14,-3), Carry at (14,2)).  The instance sits at
            // (-5,1), so its pins are (-7,-1)..(-7,2) and (-3,1),(-3,2).
            writer.i64(5);
            addWire(writer, -16, -4, {0x0004, 0x4003, 0x0005, 0x4001, 0x0000});
            addWire(writer, -16, 0, {0x0006, 0x4001, 0x0003, 0x0000});
            addWire(writer, -16, 4, {0x0008, 0xC002, 0x0001, 0x0000});
            addWire(writer, -3, 1, {0xC004, 0x0011, 0x0000});
            addWire(writer, -3, 2, {0x0011, 0x0000});
        } else if(double8Board) {
            // double_number level: one 8-bit input at (-12,0) and one 8-bit
            // output at (12,0).  An instance at (-5,0) has its word pins at
            // (-7,0) and (-3,0).
            writer.i64(2);
            addWire(writer, -12, 0, {0x0005, 0x0000});
            addWire(writer, -3, 0, {0x000F, 0x0000});
        } else if(xor8Board) {
            // byte_xor level: A at (-13,-4), B at (-13,6), Result at (15,1).
            writer.i64(3);
            addWire(writer, -13, -4, {0x0006, 0x4004, 0x0000});
            addWire(writer, -13, 6, {0x0006, 0xC005, 0x0000});
            addWire(writer, -3, 0, {0x0012, 0x4001, 0x0000});
        } else if(mux8Board) {
            // byte_mux level: Select (-14,-3), A (-12,0), B (-12,3), Output (12,0).
            writer.i64(4);
            addWire(writer, -14, -3, {0x0007, 0x4002, 0x0000});
            addWire(writer, -12, 0, {0x0005, 0x0000});
            addWire(writer, -12, 3, {0x0005, 0xC002, 0x0000});
            addWire(writer, -3, 0, {0x000F, 0x0000});
        } else if(asr8Board) {
            // byte_asr level: Input (-17,-10), Shift (-17,9), Result (17,0).
            writer.i64(3);
            addWire(writer, -17, -10, {0x000A, 0x400A, 0x0000});
            addWire(writer, -17, 9, {0x000A, 0xC008, 0x0000});
            addWire(writer, -3, 0, {0x0014, 0x0000});
        } else if(adder8Board) {
            // byte_adder level: Carry in (-17,-12), A (-17,-4), B (-17,4),
            // Sum (18,-4), Carry out (18,4).
            writer.i64(5);
            addWire(writer, -17, -12, {0x000A, 0x400B, 0x0000});
            addWire(writer, -17, -4, {0x0009, 0x4004, 0x0001, 0x0000});
            addWire(writer, -17, 4, {0x0009, 0xC003, 0x0001, 0x0000});
            addWire(writer, -3, 0, {0x0015, 0xC004, 0x0000});
            addWire(writer, -3, 1, {0x0015, 0x4003, 0x0000});
        } else {
            writer.i64(3);
            addWire(writer, -13, -1, {0x0007, 0x0000});          // a -> Mod in0
            addWire(writer, -13, 1, {0x0007, 0xC001, 0x0000});   // b -> Mod in1
            addWire(writer, -3, -1, {0x000F, 0x4001, 0x0000});   // Mod out -> Output
        }
    }
    return writer.bytes;
}

std::vector<uint8_t> buildLevelPayload(bool builtin_and) {
    Writer writer;
    writer.i64(0x6f13c29bb2e19440LL);  // original and_gate seed
    writer.u32(0);
    writer.i64(0);
    writer.i64(0);
    writer.u8(1);
    writer.i64(10000);
    writer.sequence_i64({});
    writer.string("");
    writer.u8(0);
    writer.u16(0);
    writer.sequence_u8({});
    writer.string("");
    for (int i = 0; i < 512; ++i) writer.u8(0);

    const bool series=!builtin_and && topology=="series";
    const bool parallel=!builtin_and && topology=="parallel";
    const bool multidriver=!builtin_and && topology=="multidriver";
    writer.i64(parallel?5:(series||multidriver)?4:3);
    addV13Pin(writer, 0x3F, -13, 0, 0x1111111111111111ULL, "Input", 8);
    if (builtin_and)
        addV13Pin(writer, kAndGate, -5, 0, 0x2222222222222222ULL, "", 1);
    else
        addV13CustomInstance(writer);
    addV13Pin(writer, 0x44, 13, 0, 0x3333333333333333ULL, "Output", 8);
    if(series) addV13CustomInstance(writer,3,0,0x5555555555555555ULL);
    if(parallel) {
        addV13CustomInstance(writer,-5,6,0x5555555555555555ULL);
        addV13Pin(writer,0x44,13,6,0x6666666666666666ULL,"Output 2",8);
    }
    // Two component outputs driving the same net: the verified rules do not
    // cover it, so the modded timing must fall back to the native statistics.
    if(multidriver) addV13CustomInstance(writer,-5,6,0x5555555555555555ULL);

    writer.i64(parallel?6:series?5:multidriver?4:3);
    addWire(writer, -13, -1, {0x0007, 0x0000});
    if(builtin_and) addWire(writer,-13,1,{0x0007,0x0000});
    else addWire(writer, -13, 1, {0x0007, 0xC001, 0x0000});
    if(series) {
        addWire(writer,-3,-1,{0x0005,0x0000});
        addWire(writer,-3,-1,{0x4001,0x0005,0x0000});
        addWire(writer,5,-1,{0x0007,0x4001,0x0000});
    } else if(builtin_and) addWire(writer,-3,0,{0x000F,0x0000});
    else addWire(writer, -3, -1, {0x000F, 0x4001, 0x0000});
    if(parallel) {
        addWire(writer,-13,-1,{0x4006,0x0007,0x0000});
        addWire(writer,-13,1,{0x4005,0x0007,0x0000});
        addWire(writer,-3,5,{0x000F,0x4001,0x0000});
    }
    // Custom instance pin offsets are (2,-1) for the output, so the second
    // driver starts at (-3,5) and lands on the same net end as the first one.
    if(multidriver) addWire(writer,-3,5,{0x000F,0xC005,0x0000});
    return writer.bytes;
}

std::vector<uint8_t> buildPayload(bool inner=false) {
    Writer writer;
    const bool nested=topology=="nested" && !inner;
    const bool stateful=topology=="delay";
    writer.i64(static_cast<int64_t>(inner?kInnerComponentId:kAndComponentId));
    writer.u32(0);
    // These two fields are carried in the serialized definition.  Their exact
    // meaning is documented in docs/sdk/custom-logic.md ("写元件定义");
    // the generator lets callers vary them for controlled experiments.
    writer.i64(kMetaGates);  // copied from the verified Not ZR component
    writer.i64(inner?1:kMetaDelay);
    writer.u8(1);        // simulation settings are present
    writer.i64(10000);   // maximum cycle count
    writer.sequence_i64({});
    writer.string("");
    writer.u8(0);
    writer.u16(0);
    writer.sequence_u8({});
    writer.string("");
    for (int i = 0; i < 512; ++i) writer.u8(0);  // default wire palette

    writer.i64(4);  // components
    addPin(writer, kInputPin, -11, -13, 0x1000000000000000ULL, "A", -2, 1);
    addPin(writer, kInputPin, -11, -7, 0x1000000000000001ULL, "B", -4, 1);
    if (stateful) addDelayLine(writer, 0, 0);
    else addAndGate(writer,nested);
    addPin(writer, kOutputPin, 14, -10, 0x1000000000000003ULL, "Out", -2, 1);

    writer.i64(stateful?2:3);  // wires
    if(stateful) {
        // A -> delay input (-3,0); delay output (3,0) -> Out pin input (11,-10).
        addWire(writer, -8, -13, {0x0005, 0x400D, 0x0000});
        addWire(writer, 3, 0, {0x0008, 0xC00A, 0x0000});
        return writer.bytes;
    }
    addWire(writer, -8, -13, {0x000B, 0x4002, 0x0000});
    if(nested) addWire(writer,-8,-7,{0x000B,0xC003,0x0000});
    else addWire(writer, -8, -7, {0x000B, 0xC002, 0x0000});
    if(nested) addWire(writer,6,-11,{0x0005,0x4001,0x0000});
    else addWire(writer, 6, -10, {0x0005, 0x0000});
    return writer.bytes;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path output =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("build/and2_component.data");
    if (argc > 2) kMetaGates = std::stoll(argv[2]);
    if (argc > 3) kMetaDelay = std::stoll(argv[3]);
    if (argc > 4) topology=argv[4];
    if (topology.rfind("nl-",0)==0) {
        const std::string kind=topology.substr(3);
        if (kind.rfind("def-",0)==0) {
            const auto raw=buildNativeLogicDefinition(kind.substr(4));
            std::vector<uint8_t> encoded_def;
            encoded_def.push_back(14);  // component definition save format
            writeLiteralSnappy(raw,encoded_def);
            const auto path=output.parent_path()/("nl_def_"+kind.substr(4)+".data");
            std::ofstream def_file(path,std::ios::binary|std::ios::trunc);
            def_file.write(reinterpret_cast<const char*>(encoded_def.data()),
                           static_cast<std::streamsize>(encoded_def.size()));
            if(!def_file) {std::cerr << "FAIL write " << path.u8string() << "\n";return 1;}
            std::cout << "PASS native logic definition: kind=" << kind.substr(4)
                      << " raw=" << raw.size() << " path=" << path.u8string() << "\n";
            return 0;
        }
        const auto board_raw=buildNativeLogicBoard(kind);
        std::vector<uint8_t> board;
        board.push_back(13);  // level solution save-format version
        writeLiteralSnappy(board_raw,board);
        const auto path=output.parent_path()/("nl_"+kind+".data");
        std::filesystem::create_directories(path.parent_path());
        std::ofstream board_file(path,std::ios::binary|std::ios::trunc);
        board_file.write(reinterpret_cast<const char*>(board.data()),
                         static_cast<std::streamsize>(board.size()));
        if(!board_file) {std::cerr << "FAIL write " << path.u8string() << "\n";return 1;}
        std::cout << "PASS native logic board: kind=" << kind
                  << " raw=" << board_raw.size() << " path=" << path.u8string() << "\n";
        return 0;
    }
    const auto raw = buildPayload();
    std::vector<uint8_t> encoded;
    encoded.push_back(14);  // save-format version
    writeLiteralSnappy(raw, encoded);
    std::vector<uint8_t> decoded;
    if (!decodeLiteralSnappy(
            std::vector<uint8_t>(encoded.begin() + 1, encoded.end()),
            decoded) ||
        decoded != raw) {
        std::cerr << "FAIL fixture Snappy round-trip\n";
        return 1;
    }
    std::filesystem::create_directories(output.parent_path());
    if(topology=="nested") {
        std::vector<uint8_t> inner{14};writeLiteralSnappy(buildPayload(true),inner);
        std::ofstream dependency(output.parent_path()/"inner_component.data",std::ios::binary);
        dependency.write(reinterpret_cast<const char*>(inner.data()),inner.size());
        if(!dependency) return 1;
    }
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(encoded.data()),
               static_cast<std::streamsize>(encoded.size()));
    if (!file) {
        std::cerr << "FAIL write " << output.u8string() << "\n";
        return 1;
    }

    std::filesystem::path level_output;
    for (bool builtin_and : {false, true}) {
        const auto level_raw = buildLevelPayload(builtin_and);
        std::vector<uint8_t> level_encoded;
        level_encoded.push_back(13);  // level solution save-format version
        writeLiteralSnappy(level_raw, level_encoded);
        level_output = output.parent_path() /
                       (builtin_and ? "and2_solution_builtin.data"
                                    : "and2_solution.data");
        std::ofstream level_file(level_output,
                                 std::ios::binary | std::ios::trunc);
        level_file.write(reinterpret_cast<const char*>(level_encoded.data()),
                         static_cast<std::streamsize>(level_encoded.size()));
        if (!level_file) {
            std::cerr << "FAIL write " << level_output.u8string() << "\n";
            return 1;
        }
    }
    std::cout << "PASS AND fixture: id=" << kAndComponentId
              << " raw=" << raw.size() << " encoded=" << encoded.size()
              << " path=" << output.u8string()
              << " level=" << level_output.u8string() << "\n";
    return 0;
}
