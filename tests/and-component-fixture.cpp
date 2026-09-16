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
                         uint64_t identity=0x2222222222222222ULL) {
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
    writer.i64(static_cast<int64_t>(kAndComponentId));
    writer.u16(0);
}

std::string topology="single";
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
    // meaning is under investigation (see research/COMPONENT-PIPELINE.md);
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
