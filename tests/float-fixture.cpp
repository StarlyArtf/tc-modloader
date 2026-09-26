/* Build v13 circuit.data fixtures for the Float Ops M0 probes.

   Default board (21 instances):

       source -> pass x17 -> result+flags -> sink
                                       \-> flags sink

   The Flags[5] output is routed to its own five-bit sink, so the second wide
   output of one instance is not merely allocated but actually read back with
   the pattern the callback published.  Every 32-bit connection is a real wire;
   the five-bit one leaves the dual probe's out1 pin at (x+2, y+1) and arrives
   at the sink's 5-bit input two rows below.

   `--boundary` builds the same shape with 38 pass probes (42 instances, one
   binding token each).  That board is the diagnostic for the loader's known
   wide-output limit: the first 32 tokens publish into their own state slot, so
   the run records where the chain stops carrying 0xDEADBEEF instead of
   pretending the current loader has no boundary.

   The game accepts raw Snappy streams made only from literal chunks, so this
   generator is dependency-free and deterministic. */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kSourceId = UINT64_C(0x4633325352433031);
constexpr uint64_t kPassId = UINT64_C(0x4633325041535331);
constexpr uint64_t kDualId = UINT64_C(0x4633324455414c31);
constexpr uint64_t kFlagsSinkId = UINT64_C(0x463332464c475331);
constexpr uint64_t kSinkId = UINT64_C(0x46333253494e4b31);
constexpr uint64_t kConstantId = UINT64_C(0x463332434f4e5331);
constexpr uint64_t kAddId = UINT64_C(0x4633324144445f31);
constexpr uint64_t kDisplayId = UINT64_C(0x4633324449535031);
/* The M3/M4 catalogue ids, in the same order as examples/float-ops/classes.hpp
   declares them (the strings spell "F32…" in little-endian ASCII). */
constexpr uint64_t kSubtractId = UINT64_C(0x4633325355425f31);
constexpr uint64_t kMultiplyId = UINT64_C(0x4633324d554c5f31);
constexpr uint64_t kDivideId = UINT64_C(0x4633324449565f31);
constexpr uint64_t kSquareRootId = UINT64_C(0x4633325351545f31);
constexpr uint64_t kNegateId = UINT64_C(0x4633324e45475f31);
constexpr uint64_t kAbsoluteId = UINT64_C(0x4633324142535f31);
constexpr uint64_t kCompareId = UINT64_C(0x463332434d505f31);
constexpr uint64_t kClassifyId = UINT64_C(0x463332434c535f31);
constexpr uint64_t kFusedMultiplyAddId = UINT64_C(0x463332464d415f31);
constexpr uint64_t kRemainderId = UINT64_C(0x46333252454d5f31);
constexpr uint64_t kRoundToIntegralId = UINT64_C(0x463332524e445f31);
constexpr uint64_t kMinimumId = UINT64_C(0x4633324d494e5f31);
constexpr uint64_t kMaximumId = UINT64_C(0x4633324d41585f31);
constexpr uint64_t kI32ToFp32Id = UINT64_C(0x4633324932465f31);
constexpr uint64_t kU32ToFp32Id = UINT64_C(0x4633325532465f31);
constexpr uint64_t kFp32ToI32Id = UINT64_C(0x4633324632495f31);
constexpr uint64_t kFp32ToU32Id = UINT64_C(0x4633324632555f31);
constexpr uint64_t kSplitBitsId = UINT64_C(0x46333253504c5f31);
constexpr uint64_t kMakeBitsId = UINT64_C(0x4633324d4b425f31);

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
    void emptyI64Sequence() { u16(0); }
    void emptyByteSequence() { u16(0); }
};

void addCustomInstance(Writer& writer, int16_t x, int16_t y, uint64_t identity,
                       uint64_t customId) {
    writer.u16(0x4e);
    writer.i16(x);
    writer.i16(y);
    writer.u8(0);
    writer.i64(static_cast<int64_t>(identity));
    writer.string("");
    writer.u16(0);  // values
    writer.i64(0);  // data
    writer.i16(0);
    writer.i64(1);  // legacy bit field; custom definition owns real pin widths
    writer.u8(0);   // bool_a
    writer.u8(0);   // bool_b
    writer.u8(0);   // init_data
    writer.u16(0);  // subcomponents
    writer.u16(0);  // settings
    writer.i64(static_cast<int64_t>(customId));
    writer.u16(0);  // custom pin data
}

/* A wire path is a run of 16-bit segments, `(direction << 13) | length`, ended
   by a zero-length segment.  Directions are 0=east, 2=south, 4=west, 6=north. */
uint16_t east(uint16_t length) { return static_cast<uint16_t>((0u << 13) | length); }
uint16_t south(uint16_t length) { return static_cast<uint16_t>((2u << 13) | length); }
uint16_t west(uint16_t length) { return static_cast<uint16_t>((4u << 13) | length); }
uint16_t north(uint16_t length) { return static_cast<uint16_t>((6u << 13) | length); }

void addWire(Writer& writer, int16_t x, int16_t y,
             std::initializer_list<uint16_t> segments) {
    writer.u8(0);
    writer.string("");
    writer.i16(x);
    writer.i16(y);
    for (uint16_t segment : segments) writer.u16(segment);
    writer.u16(0);
}

struct Board {
    int passCount;
    int catalogueCount = 0;
    std::vector<uint8_t> bytes;
};

Board buildCircuit(int passCount) {
    Writer writer;
    writer.i64(INT64_C(0x6f13c29bb2e19440));
    writer.u32(0);
    writer.i64(0);  // cached gates
    writer.i64(0);  // cached delay
    writer.u8(1);
    writer.i64(10000);
    writer.emptyI64Sequence();
    writer.string("");
    writer.u8(0);
    writer.u16(0);
    writer.emptyByteSequence();
    writer.string("");
    for (int i = 0; i < 512; ++i) writer.u8(0);

    /* source, the pass chain, the dual probe, the word sink, the flags sink */
    const int componentCount = passCount + 4;
    writer.i64(componentCount);

    uint64_t identity = UINT64_C(0x1000000000000000);
    int16_t center = -60;
    const int16_t firstX = center;
    addCustomInstance(writer, center, 0, identity++, kSourceId);
    for (int i = 0; i < passCount; ++i) {
        center = static_cast<int16_t>(center + 6);
        addCustomInstance(writer, center, 0, identity++, kPassId);
    }
    center = static_cast<int16_t>(center + 6);
    const int16_t dualX = center;
    addCustomInstance(writer, center, 0, identity++, kDualId);
    center = static_cast<int16_t>(center + 6);
    addCustomInstance(writer, center, 0, identity++, kSinkId);
    /* The five-bit sink sits two rows below the dual probe, so its input
       (-2,0) is reachable from out1 (+2,+1) without crossing any other pin. */
    const int16_t flagsX = dualX;
    const int16_t flagsY = 8;
    addCustomInstance(writer, flagsX, flagsY, identity++, kFlagsSinkId);

    const int chainWires = componentCount - 2;  /* every chained neighbour pair */
    writer.i64(chainWires + 1);
    int16_t outputX = static_cast<int16_t>(firstX + 2);
    for (int i = 0; i < chainWires; ++i) {
        addWire(writer, outputX, 0, {east(2)});
        outputX = static_cast<int16_t>(outputX + 6);
    }
    addWire(writer, static_cast<int16_t>(dualX + 2), 1,
            {east(2), south(7), west(6)});

    Board board{};
    board.passCount = passCount;
    board.bytes = std::move(writer.bytes);
    return board;
}

/* The M2 board: two constants feed the adder, the adder's R[32] feeds the
   display.  Every instance keeps the configuration it was registered with, so
   the true-game case can compare "defaults" against "what the storage service
   wrote" - the same write the editors use. */
Board buildM2Circuit() {
    Writer writer;
    writer.i64(INT64_C(0x6f13c29bb2e19440));
    writer.u32(0);
    writer.i64(0);
    writer.i64(0);
    writer.u8(1);
    writer.i64(10000);
    writer.emptyI64Sequence();
    writer.string("");
    writer.u8(0);
    writer.u16(0);
    writer.emptyByteSequence();
    writer.string("");
    for (int i = 0; i < 512; ++i) writer.u8(0);

    writer.i64(4);
    uint64_t identity = UINT64_C(0x1000000000000000);
    /* The loader's own pin layout for a two-pin side is y = 0 and y = +1
       (measured: "in0=(-3,0,w32) in1=(-3,1,w32)" at the 3.0 lane these M2 types
       declare).  The second constant sits three cells below the first rather
       than one: a stock-sized body is 2.93 cells tall, so one cell apart would
       bury most of it - and the layout case measures each body in the picture,
       which needs to see them. */
    addCustomInstance(writer, -12, 0, identity++, kConstantId); /* A */
    addCustomInstance(writer, -12, 4, identity++, kConstantId); /* B */
    addCustomInstance(writer, 0, 0, identity++, kAddId);
    addCustomInstance(writer, 8, 0, identity++, kDisplayId);

    writer.i64(3);
    /* The M2 components sit on the pin lane the Mod declares (3.0 cells, like
       the stock Constant/Static Value), so their pins - and therefore every
       wire that has to reach one - are one cell further out than they were
       when the lane was the loader's default 2.0: constant A/B drive from
       (-9,0)/(-9,1) into the adder's inputs at (-3,0)/(-3,1), and the adder's R
       leaves at (3,0) for the display's input at (5,0). */
    addWire(writer, -9, 0, {east(6)});          /* constant A -> adder input A (-3,0) */
    addWire(writer, -9, 4, {east(6), north(3)}); /* constant B -> adder input B (-3,1) */
    addWire(writer, 3, 0, {east(2)});           /* adder R (3,0) -> display input (5,0) */

    Board board{};
    board.passCount = 0;
    board.bytes = std::move(writer.bytes);
    return board;
}

/* The catalogue board: one instance of every M3/M4 type, laid out in a grid so
   the true-game case can measure each body in the picture and open each type's
   drawer rows.  Nothing is wired: this board is about registration, the stock
   look and the panel, while the arithmetic of the same types is covered by the
   kernel vectors and by the offline definition test.  Spacing is 12 cells
   across and 8 down, which leaves the widest face (4.92 cells) plus its pins
   (3.0 cells either side) clear of its neighbours. */
Board buildCatalogueCircuit() {
    struct Type {
        uint64_t id;
        int x;
        int y;
    };
    const Type types[] = {
        {kSubtractId, -32, 12},         {kMultiplyId, -16, 12},       {kDivideId, 0, 12},
        {kSquareRootId, 16, 12},        {kNegateId, 32, 12},
        {kAbsoluteId, -32, 24},         {kCompareId, -16, 24},        {kClassifyId, 0, 24},
        {kFusedMultiplyAddId, 16, 24},  {kRemainderId, 32, 24},
        {kRoundToIntegralId, -32, 36},  {kMinimumId, -16, 36},        {kMaximumId, 0, 36},
        {kI32ToFp32Id, 16, 36},         {kU32ToFp32Id, 32, 36},
        {kFp32ToI32Id, -32, 48},        {kFp32ToU32Id, -16, 48},      {kSplitBitsId, 0, 48},
        {kMakeBitsId, 16, 48},
    };

    Writer writer;
    writer.i64(INT64_C(0x6f13c29bb2e19440));
    writer.u32(0);
    writer.i64(0);
    writer.i64(0);
    writer.u8(1);
    writer.i64(10000);
    writer.emptyI64Sequence();
    writer.string("");
    writer.u8(0);
    writer.u16(0);
    writer.emptyByteSequence();
    writer.string("");
    for (int i = 0; i < 512; ++i) writer.u8(0);

    /* The M2 circuit comes first, unchanged: the true-game case that already
       passes proves the game opens its component drawer on this board, and the
       catalogue grid is added below it.  (Measured while building this case: a
       board that holds *only* catalogue parts never gets the drawer at all -
       the game draws its bottom panel for a board it can compile, and the
       display sink plus these wires are what make this one compile.) */
    const size_t count = sizeof(types) / sizeof(types[0]);
    writer.i64(static_cast<int64_t>(count) + 4);
    uint64_t identity = UINT64_C(0x2000000000000000);
    addCustomInstance(writer, -12, 0, identity++, kConstantId);
    addCustomInstance(writer, -12, 4, identity++, kConstantId);
    addCustomInstance(writer, 0, 0, identity++, kAddId);
    addCustomInstance(writer, 8, 0, identity++, kDisplayId);
    for (const Type& type : types)
        addCustomInstance(writer, static_cast<int16_t>(type.x),
                          static_cast<int16_t>(type.y), identity++, type.id);
    writer.i64(3);
    addWire(writer, -9, 0, {east(6)});            /* constant A -> adder input A */
    addWire(writer, -9, 4, {east(6), north(3)});  /* constant B -> adder input B */
    addWire(writer, 3, 0, {east(2)});             /* adder R -> display input */

    Board board{};
    board.passCount = 0;
    board.bytes = std::move(writer.bytes);
    board.catalogueCount = static_cast<int>(count);
    return board;
}

/* A board with more native components than a Mod's first handle buffer is
   likely to hold, with the M2 chain that the drawer cases edit *behind* them.
   The player's report "所有能写配置的元件都写入不了" (2026-09-26) was exactly
   this shape: their board carried 22 float instances, the Mod asked the host
   for eight instance handles, and the host answers "all of them or nothing" -
   so every write looked like "the component is not on the board".  This fixture
   is what makes a true-game case catch that
   (tests/float-busy-board-playtest.ps1); the crowd is unwired, like the
   catalogue grid. */
Board buildCrowdCircuit() {
    constexpr int kCrowd = 18;
    Writer writer;
    writer.i64(INT64_C(0x6f13c29bb2e19440));
    writer.u32(0);
    writer.i64(0);
    writer.i64(0);
    writer.u8(1);
    writer.i64(10000);
    writer.emptyI64Sequence();
    writer.string("");
    writer.u8(0);
    writer.u16(0);
    writer.emptyByteSequence();
    writer.string("");
    for (int i = 0; i < 512; ++i) writer.u8(0);

    writer.i64(kCrowd + 4);
    uint64_t identity = UINT64_C(0x3000000000000000);
    for (int index = 0; index < kCrowd; ++index) {
        addCustomInstance(writer, static_cast<int16_t>(-60 + (index % 8) * 12),
                          static_cast<int16_t>(20 + (index / 8) * 8), identity++,
                          kDisplayId);
    }
    addCustomInstance(writer, -12, 0, identity++, kConstantId);
    addCustomInstance(writer, -12, 4, identity++, kConstantId);
    addCustomInstance(writer, 0, 0, identity++, kAddId);
    addCustomInstance(writer, 8, 0, identity++, kDisplayId);
    writer.i64(3);
    addWire(writer, -9, 0, {east(6)});
    addWire(writer, -9, 4, {east(6), north(3)});
    addWire(writer, 3, 0, {east(2)});

    Board board{};
    board.passCount = 0;
    board.catalogueCount = kCrowd + 4;
    board.bytes = std::move(writer.bytes);
    return board;
}

void appendLiteralSnappy(const std::vector<uint8_t>& raw, std::vector<uint8_t>* out) {
    uint64_t remaining = raw.size();
    do {
        uint8_t byte = static_cast<uint8_t>(remaining & 0x7f);
        remaining >>= 7;
        out->push_back(static_cast<uint8_t>(byte | (remaining ? 0x80 : 0)));
    } while (remaining);
    for (size_t offset = 0; offset < raw.size();) {
        const size_t count = std::min<size_t>(60, raw.size() - offset);
        out->push_back(static_cast<uint8_t>((count - 1) << 2));
        out->insert(out->end(), raw.begin() + offset, raw.begin() + offset + count);
        offset += count;
    }
}

int writeBoard(const Board& board, const std::filesystem::path& output,
               const char* label) {
    std::vector<uint8_t> encoded{13};
    appendLiteralSnappy(board.bytes, &encoded);
    std::filesystem::create_directories(output.parent_path());
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(encoded.data()),
               static_cast<std::streamsize>(encoded.size()));
    if (!file) {
        std::cerr << "FAIL writing " << output.u8string() << '\n';
        return 1;
    }
    const int components = board.catalogueCount ? board.catalogueCount
                                                : 3 + board.passCount + 1;
    std::cout << "PASS float fixture (" << label << "): components="
              << components << " bytes=" << encoded.size()
              << " path=" << output.u8string() << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    constexpr int kMainPassCount = 17;
    constexpr int kBoundaryPassCount = 38;

    if (argc > 1 && std::strcmp(argv[1], "--m2") == 0) {
        const std::filesystem::path output =
            argc > 2 ? argv[2] : "build/float-m2-board.data";
        return writeBoard(buildM2Circuit(), output, "m2");
    }
    if (argc > 1 && std::strcmp(argv[1], "--catalogue") == 0) {
        const std::filesystem::path output =
            argc > 2 ? argv[2] : "build/float-catalogue-board.data";
        return writeBoard(buildCatalogueCircuit(), output, "catalogue");
    }
    if (argc > 1 && std::strcmp(argv[1], "--crowd") == 0) {
        const std::filesystem::path output =
            argc > 2 ? argv[2] : "build/float-crowd-board.data";
        return writeBoard(buildCrowdCircuit(), output, "crowd");
    }
    if (argc > 1 && std::strcmp(argv[1], "--boundary") == 0) {
        const std::filesystem::path output =
            argc > 2 ? argv[2] : "build/float-boundary-board.data";
        return writeBoard(buildCircuit(kBoundaryPassCount), output, "boundary");
    }
    const std::filesystem::path output =
        argc > 1 ? argv[1] : "build/float-compat-board.data";
    return writeBoard(buildCircuit(kMainPassCount), output, "compat");
}
