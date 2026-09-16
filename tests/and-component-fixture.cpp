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
constexpr uint16_t kInputPin = 0x4F;
constexpr uint16_t kOutputPin = 0x51;
constexpr uint16_t kAndGate = 0x04;

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

void addAndGate(Writer& writer) {
    writer.u16(kAndGate);
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

void addWire(Writer& writer, int16_t x, int16_t y,
             std::initializer_list<uint16_t> segments) {
    writer.u8(0);
    writer.string("");
    writer.i16(x);
    writer.i16(y);
    for (uint16_t segment : segments) writer.u16(segment);
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

void addV13CustomInstance(Writer& writer) {
    writer.u16(0x4E);
    writer.i16(-5);
    writer.i16(0);
    writer.u8(0);
    writer.i64(0x2222222222222222LL);
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
    writer.i64(static_cast<int64_t>(kAndComponentId));
    writer.u16(0);
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

    writer.i64(3);
    addV13Pin(writer, 0x3F, -13, 0, 0x1111111111111111ULL, "Input", 8);
    if (builtin_and)
        addV13Pin(writer, kAndGate, -5, 0, 0x2222222222222222ULL, "", 1);
    else
        addV13CustomInstance(writer);
    addV13Pin(writer, 0x44, 13, 0, 0x3333333333333333ULL, "Output", 8);

    writer.i64(3);
    addWire(writer, -13, -1, {0x0007, 0x0000});
    addWire(writer, -13, 1, {0x0007, 0xC001, 0x0000});
    addWire(writer, -3, -1, {0x000F, 0x4001, 0x0000});
    return writer.bytes;
}

std::vector<uint8_t> buildPayload() {
    Writer writer;
    writer.i64(static_cast<int64_t>(kAndComponentId));
    writer.u32(0);
    writer.i64(3);       // copied from the verified Not ZR component
    writer.i64(2);       // copied from the verified Not ZR component
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
    addAndGate(writer);
    addPin(writer, kOutputPin, 14, -10, 0x1000000000000003ULL, "Out", -2, 1);

    writer.i64(3);  // wires
    addWire(writer, -8, -13, {0x000B, 0x4002, 0x0000});
    addWire(writer, -8, -7, {0x000B, 0xC002, 0x0000});
    addWire(writer, 6, -10, {0x0005, 0x0000});
    return writer.bytes;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path output =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("build/and2_component.data");
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
