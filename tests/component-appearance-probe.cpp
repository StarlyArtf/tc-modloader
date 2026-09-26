/* Development probe: where does a custom component's board appearance come
   from?

   The text-box Mod ran into three things it cannot influence today (see
   docs/research/component-appearance.md): the board footprint / hit area of a
   custom component, the game's own rendering of it (design map + the component
   name watermark), and the fact that a pin-less decorative component is never
   bound.  This probe answers the first part of the measurement: what changes in
   the Prototype when the *internal circuit* changes shape.

   Method: import four hand-built circuit definitions that differ only in their
   internal layout, read each Prototype back, dump its whole 0x5a8 bytes and
   report every offset where a variant differs from the compact baseline.  The
   circuit bytes are built here (not through the loader's encoder) so the layout
   is fully under the probe's control; the byte layout is the same v14 format
   src/component_definition.hpp writes.

   Everything happens during tc_mod_load, so the probe needs no level: a sandbox
   run that only starts the game is enough.  Output goes to the Mod's own data
   directory. */
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_game_model.h"
#include "../sdk/tc_component_model.h"
#include "../sdk/tc_command_api.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

const TCHost* host = nullptr;
tc::TCGameModel game;
tc::TCComponentModel components;
std::string directory;

/* ---- the on-board part: place one instance of every variant so a screenshot
   shows what each modification does to the rendered component ---- */
TCBoardApiV6 boardApi{};
TCCommandApiV2 commandApi{};
struct Placement {
    const char* name;
    uint64_t id;
    int x;
};
Placement placements[] = {
    {"appearance-baseline", 0x4150505F31303031ULL, -60},
    {"appearance-nofields", 0x4150505F31303032ULL, -20},
    {"appearance-flag02", 0x4150505F31303033ULL, 20},
    {"appearance-painted", 0x4150505F31303034ULL, 60},
};
/* The hit scan uses the game's own placement rule as the oracle: placing a
   second component on top of an existing one is refused, so the offsets that
   get refused ARE the existing component's footprint.  Two targets - the
   compact scaffold and the wide one (internal output pin at x=120) - say
   whether the footprint follows the internal circuit at all. */
constexpr uint64_t kLadderId = 0x4150505F31303036ULL;
constexpr uint64_t kWideId = 0x4150505F31303035ULL;
constexpr int kTargetX[2] = {-40, 0};
const int scanOffsets[][2] = {
    {1, 0},  {2, 0},  {3, 0},  {4, 0},  {5, 0},  {6, 0},  {8, 0},  {10, 0},
    {12, 0}, {14, 0}, {16, 0}, {20, 0}, {0, 1},  {0, 2},  {0, 3},  {0, 4},
    {0, 5},  {0, 6},  {0, 8},  {0, 10},
};
constexpr int kScanCount = static_cast<int>(sizeof(scanOffsets) / sizeof(scanOffsets[0]));
bool hitscan = false;
bool geometrySurgery = false;
int hitStage = 0;
int hitTarget = 0;
int hitOffset = 0;
unsigned long long hitStepAt = 0;
bool hitPending = false;
uint64_t hitRequest = 0;
int hitPendingX = 0, hitPendingY = 0;
bool placed = false;
unsigned long long boardSeenAt = 0;
/* Prototype surgery is opt-in: the first run places four *unmodified* copies to
   prove the placement/screenshot path, and only then does the surgery run get
   enabled (raw writes into a prototype are not part of any contract yet). */
bool surgery = false;

void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

struct Writer {
    std::vector<uint8_t> bytes;
    void number(uint64_t value, unsigned size) {
        for (unsigned i = 0; i < size; ++i) bytes.push_back(uint8_t(value >> (i * 8)));
    }
    void string(const char* text) {
        const size_t length = std::strlen(text);
        number(length, 2);
        bytes.insert(bytes.end(), text, text + length);
    }
    void component(unsigned kind, int x, int y, uint64_t id, const char* name, unsigned bits,
                   int ordinal = 0) {
        number(kind, 2); number(x, 2); number(y, 2); number(0, 1); number(id, 8);
        string(name); number(ordinal ? 1 : 0, 2);
        if (ordinal) number(kind == 0x4f ? 2 : 0, 8);
        number(0, 8); number(-2 * ordinal, 2); number(bits, 8);
        number(0, 1); number(UINT64_MAX, 8); number(0, 8); number(0, 1);
        number(0, 1); number(0, 2); number(0, 2);
    }
};

std::vector<uint8_t> literals(const Writer& body) {
    std::vector<uint8_t> out{14};   /* component definition save format */
    uint64_t remaining = body.bytes.size();
    do {
        uint8_t byte = uint8_t(remaining & 0x7f);
        remaining >>= 7;
        out.push_back(uint8_t(byte | (remaining ? 0x80 : 0)));
    } while (remaining);
    for (size_t at = 0; at < body.bytes.size();) {
        const size_t chunk = body.bytes.size() - at < 60 ? body.bytes.size() - at : 60;
        out.push_back(uint8_t((chunk - 1) << 2));
        out.insert(out.end(), body.bytes.begin() + at, body.bytes.begin() + at + chunk);
        at += chunk;
    }
    return out;
}

/* 0 inputs, 1 output.  `driverX`/`pinX` move the two internal nodes, which is
   the only difference between the variants; `empty` writes a definition with no
   node at all. */
std::vector<uint8_t> definition(uint64_t id, int driverX, int pinX, bool empty) {
    Writer writer;
    writer.number(id, 8); writer.number(0, 4); writer.number(0, 8); writer.number(0, 8);
    writer.number(1, 1); writer.number(10000, 8); writer.number(0, 2); writer.string("");
    writer.number(0, 1); writer.number(0, 2); writer.number(0, 2); writer.string("");
    for (int i = 0; i < 512; ++i) writer.number(0, 1);
    if (empty) {
        writer.number(0, 8);      /* no nodes */
        writer.number(0, 8);      /* no wires */
        return literals(writer);
    }
    writer.number(2, 8);          /* one driver gate and one output pin */
    writer.component(0x12, driverX, 0, 0x4000, "", 1);
    writer.component(0x51, pinX, 0, 0x5000, "", 1, 1);
    Writer wires;
    wires.number(0, 1); wires.string("");
    wires.number(driverX + 2, 2); wires.number(0, 2);
    wires.number((pinX - 3) - (driverX + 2), 2);
    wires.number(0, 2);
    writer.number(1, 8);
    writer.bytes.insert(writer.bytes.end(), wires.bytes.begin(), wires.bytes.end());
    return literals(writer);
}

struct Variant {
    const char* name;
    uint64_t id;
    int driverX;
    int pinX;
    bool empty;
};

std::string hexLine(const unsigned char* data, size_t offset, size_t count) {
    char line[8 + 32 * 3 + 4];
    int at = std::snprintf(line, sizeof(line), "%04zx  ", offset);
    for (size_t i = 0; i < count; ++i)
        at += std::snprintf(line + at, sizeof(line) - at, "%02x ", data[offset + i]);
    return std::string(line);
}

/* Places the four board variants once a board is up; the screenshot the
   playtest takes afterwards is the measurement. */
bool componentExists(uint64_t customId, int x, int y) {
    TCGameHandle board{};
    if (boardApi.get_current(boardApi.context, &board) != TC_HANDLE_OK) return false;
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    TCBoardObjectBuffersV1 buffers{};
    buffers.size = sizeof(buffers);
    buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    static std::vector<TCGameHandle> handles;
    int status = boardApi.capture_objects(boardApi.context, &board, &snapshot,
                                          static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    if (status == TC_SNAPSHOT_ERR_CAPACITY) {
        handles.assign(static_cast<size_t>(snapshot.component_count), TCGameHandle{});
        buffers.components = handles.data();
        buffers.component_capacity = handles.size();
        status = boardApi.capture_objects(boardApi.context, &board, &snapshot,
                                          static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    }
    if (status != TC_SNAPSHOT_OK) return false;
    for (uint64_t index = 0; index < snapshot.component_written; ++index) {
        TCComponentInfoV1 info{};
        info.size = sizeof(info);
        if (boardApi.read_component(boardApi.context, &handles[index], &info,
                                    static_cast<uint32_t>(sizeof(info))) != TC_SNAPSHOT_OK)
            continue;
        if (info.kind != 0x4e || !(info.flags & TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE))
            continue;
        if (info.custom_prototype_id == customId && info.x == x && info.y == y) return true;
    }
    return false;
}

bool placeComponent(uint64_t customId, int x, int y) {
    TCGameHandle board{};
    if (boardApi.get_current(boardApi.context, &board) != TC_HANDLE_OK) return false;
    TCCommandV2 command{};
    command.size = sizeof(command);
    command.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
    command.subject = board;
    command.custom_prototype_id = customId;
    command.kind = 0x4e;
    command.x = x;
    command.y = y;
    command.rotation = 0;
    uint64_t request = 0;
    return commandApi.submit(commandApi.context, &command, &request) == TC_COMMAND_OK;
}

/* One step of the hit scan: ask the game to place a component at the offset,
   then look whether a record with that id and position really appeared. */
void hitScan() {
    if (hitStage == 0) {
        report("appearance: hitscan starts (one fresh target per offset)");
        hitStage = 1;
        hitStepAt = GetTickCount64();
        return;
    }
    if (hitStage == 1) {
        if (GetTickCount64() - hitStepAt < 500) return;
        if (hitTarget >= 2) {
            report("appearance: hitscan finished");
            hitStage = 2;
            return;
        }
        if (hitOffset >= kScanCount) {
            ++hitTarget;
            hitOffset = 0;
            return;
        }
        /* A fresh base per offset: the ladder can then only collide with its own
           target, never with a previously placed ladder (which is what made the
           first version's accept/refuse pattern an artefact). */
        const int base = 200 + (hitTarget * kScanCount + hitOffset) * 40;
        if (hitPending == false) {
            placeComponent(hitTarget == 0 ? placements[0].id : kWideId, base, 0);
            hitPending = true;
            hitStepAt = GetTickCount64();
            return;
        }
        if (hitRequest == 0) {
            /* The target is down; now the ladder, one step to the side. */
            hitPendingX = base + scanOffsets[hitOffset][0];
            hitPendingY = scanOffsets[hitOffset][1];
            TCGameHandle board{};
            if (boardApi.get_current(boardApi.context, &board) != TC_HANDLE_OK) return;
            TCCommandV2 command{};
            command.size = sizeof(command);
            command.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
            command.subject = board;
            command.custom_prototype_id = kLadderId;
            command.kind = 0x4e;
            command.x = hitPendingX;
            command.y = hitPendingY;
            command.rotation = 0;
            hitRequest = 1;
            commandApi.submit(commandApi.context, &command, &hitRequest);
            hitStepAt = GetTickCount64();
            return;
        }
        {
            /* The placement command is queued: submit, let the queue run, and
               only then ask whether a component really appeared there. */
            const bool accepted = componentExists(kLadderId, hitPendingX, hitPendingY);
            TCCommandStatusV1 status{};
            status.size = sizeof(status);
            status.version = 1;
            uint32_t state = 0;
            int result = 0;
            if (commandApi.get_status && hitRequest &&
                commandApi.get_status(commandApi.context, hitRequest, &status,
                                      static_cast<uint32_t>(sizeof(status))) == TC_COMMAND_OK) {
                state = status.state;
                result = status.result;
            }
            char line[192];
            std::snprintf(line, sizeof(line),
                          "appearance: hitscan %s offset=(%d,%d) %s state=%u result=%d",
                          hitTarget == 0 ? "compact" : "wide",
                          scanOffsets[hitOffset][0], scanOffsets[hitOffset][1],
                          accepted ? "accepted (outside the footprint)"
                                   : "REFUSED (inside the footprint)",
                          state, result);
            report(line);
            hitPending = false;
            hitRequest = 0;
            ++hitOffset;
            hitStepAt = GetTickCount64();
            return;
        }
    }
}

void frame(void*, const TCFrame*) {
    if (!boardApi.get_current || !commandApi.submit) return;
    if (hitscan) {
        TCGameHandle board{};
        if (boardApi.get_current(boardApi.context, &board) != TC_HANDLE_OK) return;
        if (!boardSeenAt) {
            boardSeenAt = GetTickCount64();
            return;
        }
        if (GetTickCount64() - boardSeenAt < 1500) return;
        hitScan();
        return;
    }
    if (placed) return;
    TCGameHandle board{};
    if (boardApi.get_current(boardApi.context, &board) != TC_HANDLE_OK) return;
    const unsigned long long now = GetTickCount64();
    if (!boardSeenAt) {
        boardSeenAt = now;
        return;
    }
    if (now - boardSeenAt < 1500) return;
    for (const Placement& placement : placements) {
        TCCommandV2 command{};
        command.size = sizeof(command);
        command.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
        command.subject = board;
        command.custom_prototype_id = placement.id;
        command.kind = 0x4e;
        command.x = placement.x;
        command.y = 0;
        command.rotation = 0;
        uint64_t request = 0;
        const int status = commandApi.submit(commandApi.context, &command, &request);
        report(std::string("appearance: placed ") + placement.name + " at (" +
               std::to_string(placement.x) + ",0) status=" + std::to_string(status));
    }
    placed = true;
}

/* The two prototype words that move when only the internal layout moves are
   pointers; whatever they point at is the candidate for "where the board
   footprint lives".  Read a small window out of each, but only after the
   region check - a prototype field is not a promise that the address is still
   mapped. */
bool readable(const void* address, size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const auto* base = static_cast<const unsigned char*>(region.BaseAddress);
    const size_t offset =
        static_cast<size_t>(static_cast<const unsigned char*>(address) - base);
    return offset + bytes <= region.RegionSize;
}

std::string pointerWindow(const unsigned char* prototype, size_t offset) {
    const void* target = nullptr;
    std::memcpy(&target, prototype + offset, sizeof(target));
    char summary[192];
    if (!readable(target, 32)) {
        std::snprintf(summary, sizeof(summary), "prototype+0x%03zx -> %p (not readable)",
                      offset, target);
        return summary;
    }
    unsigned long long words[4]{};
    std::memcpy(words, target, sizeof(words));
    std::snprintf(summary, sizeof(summary),
                  "prototype+0x%03zx -> %p words=%llu,%llu,%llu,%llu", offset, target,
                  words[0], words[1], words[2], words[3]);
    return summary;
}

/* Prototype surgery: the whole modification vocabulary the SDK offers today is
   "read a prototype, keep it, write raw fields, write it back". */
void writeWord(tc::TCPrototype& prototype, size_t offset, uint64_t value) {
    std::memcpy(prototype.bytes + offset, &value, sizeof(value));
}

/* Replace the local collision shape without mutating the shared one-cell
   sequence returned by get_custom_prototype. */
bool replaceShape(tc::TCPrototype& prototype, int16_t x, int16_t y,
                  uint16_t width, uint16_t height, uint16_t radius) {
    using DestroySeq = void (*)(void*);
    using NewSeq = void (*)(void*, int64_t);
    auto destroy = reinterpret_cast<DestroySeq>(host->resolve_symbol(
        host->context, "eqdestroy___modelZsave95mongerZcommon_u4418"));
    auto make = reinterpret_cast<NewSeq>(host->resolve_symbol(
        host->context, "newSeq__modelZboardZboard_u5624"));
    if (!destroy || !make || !width || !height) return false;
    void* sequence = prototype.bytes + 0x48;
    destroy(sequence);
    std::memset(sequence, 0, 16);
    make(sequence, 1);
    unsigned char* storage = nullptr;
    std::memcpy(&storage, prototype.bytes + 0x50, sizeof(storage));
    if (!readable(storage, 16)) return false;
    const uint64_t packed = uint64_t(uint16_t(x)) |
                            (uint64_t(uint16_t(y)) << 16) |
                            (uint64_t(width) << 32) |
                            (uint64_t(height) << 48);
    std::memcpy(storage + 8, &packed, sizeof(packed));
    std::memcpy(prototype.bytes + 0x58, &radius, sizeof(radius));
    return true;
}

/* Paint every one of the 1024 design cells with value=3, color=15.  If the
   board picture follows this buffer, the component's sprite becomes a solid
   block and the footprint claim ("the design map renders it") is settled. */
void paintDesign(tc::TCPrototype& prototype) {
    unsigned char* cells = nullptr;
    std::memcpy(&cells, prototype.bytes + 0x570, sizeof(cells));
    if (!readable(cells, 512)) return;
    for (int word = 0; word < 64; ++word) {
        uint64_t value = 0;
        for (int nibble = 0; nibble < 16; ++nibble) value |= uint64_t(0x3F) << (nibble * 4);
        std::memcpy(cells + word * 8, &value, sizeof(value));
    }
}

/* The design map is the 32x32 cell picture the game renders for a custom
   component (component_custom.frag reads it as value = cell & 0xF,
   color = cell >> 4; a cell is drawn when value >= 3, or value == 0 with a
   non-zero color - the latter is the name watermark).  It is stored as 64
   64-bit words = 1024 nibbles.  Decode it and report the bounding box of the
   cells that carry something, which is the candidate for the component's board
   footprint. */
struct DesignSummary {
    bool present = false;
    int cells = 0;
    int minX = 0, maxX = 0, minY = 0, maxY = 0;
};

DesignSummary decodeDesign(const unsigned char* prototype, std::string* listing) {
    DesignSummary summary;
    const unsigned char* cells = nullptr;
    std::memcpy(&cells, prototype + 0x570, sizeof(cells));
    if (!readable(cells, 512)) return summary;
    summary.present = true;
    bool first = true;
    for (int word = 0; word < 64; ++word) {
        uint64_t value = 0;
        std::memcpy(&value, cells + word * 8, sizeof(value));
        for (int nibble = 0; nibble < 16; ++nibble) {
            const unsigned cell = unsigned((value >> (nibble * 4)) & 0xFull);
            if (!cell) continue;
            const int index = word * 16 + nibble;
            const int x = index / 32;
            const int y = index % 32;
            ++summary.cells;
            if (first) {
                summary.minX = summary.maxX = x;
                summary.minY = summary.maxY = y;
                first = false;
            } else {
                if (x < summary.minX) summary.minX = x;
                if (x > summary.maxX) summary.maxX = x;
                if (y < summary.minY) summary.minY = y;
                if (y > summary.maxY) summary.maxY = y;
            }
            if (listing) {
                char line[64];
                std::snprintf(line, sizeof(line), "cell index=%d cell=(%d,%d) value=%u color=%u\n",
                              index, x, y, cell & 0xFu, cell >> 4);
                *listing += line;
            }
        }
    }
    return summary;
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (h->data_directory_utf8 && *h->data_directory_utf8)
        directory = std::string(h->data_directory_utf8) + "/";
    if (!game.load(h) || !components.load(h)) return 2;
    tc::boardService(h, &boardApi);
    tc::commandService(h, &commandApi);
    {
        char setting[8]{};
        surgery = GetEnvironmentVariableA("TC_APPEARANCE_SURGERY", setting, sizeof(setting)) > 0;
        hitscan = GetEnvironmentVariableA("TC_APPEARANCE_HITSCAN", setting, sizeof(setting)) > 0;
        geometrySurgery =
            GetEnvironmentVariableA("TC_APPEARANCE_GEOMETRY", setting, sizeof(setting)) > 0;
        report(std::string("appearance: prototype surgery ") + (surgery ? "on" : "off") +
               "; geometry surgery " + (geometrySurgery ? "on" : "off") +
               "; hit scan " + (hitscan ? "on" : "off"));
    }

    const Variant variants[] = {
        {"compact", 0x4150505F30303031ULL, 4, 13, false},
        {"wide", 0x4150505F30303032ULL, 4, 120, false},
        {"empty", 0x4150505F30303033ULL, 0, 0, true},
    };

    std::vector<std::vector<unsigned char>> dumps;
    std::vector<size_t> sizes;
    for (const Variant& variant : variants) {
        const auto bytes = definition(variant.id, variant.driverX, variant.pinX, variant.empty);
        const auto imported = components.importCircuit(variant.name, bytes.data(), bytes.size(),
                                                     directory.empty() ? "D:/" : directory.c_str());
        if (!imported.ok() || imported.custom_id != variant.id) {
            report(std::string("appearance: variant ") + variant.name +
                   " was refused by the importer");
            dumps.emplace_back();
            sizes.push_back(0);
            continue;
        }
        tc::TCPrototype prototype{};
        if (!game.getCustomPrototype(variant.id, prototype)) {
            report(std::string("appearance: variant ") + variant.name +
                   " could not be read back");
            dumps.emplace_back();
            sizes.push_back(0);
            continue;
        }
        dumps.emplace_back(prototype.bytes, prototype.bytes + sizeof(prototype.bytes));
        sizes.push_back(sizeof(prototype.bytes));
        report(std::string("appearance: variant ") + variant.name + " imported id=0x" +
               std::to_string(variant.id) + " inputs=" +
               std::to_string(tc::prototypeInputCount(prototype)) + " outputs=" +
               std::to_string(tc::prototypeOutputCount(prototype)) + " driverX=" +
               std::to_string(variant.driverX) + " pinX=" + std::to_string(variant.pinX));
        report(std::string("appearance: ") + variant.name + " " +
               pointerWindow(prototype.bytes, 0x108));
        report(std::string("appearance: ") + variant.name + " " +
               pointerWindow(prototype.bytes, 0x50));
        {
            uint64_t shapeCount = 0, shapeRadius = 0;
            std::memcpy(&shapeCount, prototype.bytes + 0x48, sizeof(shapeCount));
            std::memcpy(&shapeRadius, prototype.bytes + 0x58, sizeof(shapeRadius));
            char shapeLine[192];
            std::snprintf(shapeLine, sizeof(shapeLine),
                          "appearance: %s shape count=%llu radius=0x%llx",
                          variant.name, static_cast<unsigned long long>(shapeCount),
                          static_cast<unsigned long long>(shapeRadius));
            report(shapeLine);
        }
        report(std::string("appearance: ") + variant.name + " " +
               pointerWindow(prototype.bytes, 0x570));
        /* The board footprint is not computed from the internal circuit
           (compute_bounding_boxes never touches the prototype), so the pin
           geometry is the remaining candidate.  Read it for both layouts. */
        {
            const tc::TCPinPoint outPoint = tc::prototypeOutputPinPoint(prototype, 0);
            char pinLine[192];
            std::snprintf(pinLine, sizeof(pinLine),
                          "appearance: %s pin inputs=%llu outputs=%llu out0=(%d,%d)",
                          variant.name,
                          static_cast<unsigned long long>(tc::prototypeInputCount(prototype)),
                          static_cast<unsigned long long>(tc::prototypeOutputCount(prototype)),
                          outPoint.x, outPoint.y);
            report(pinLine);
        }
        std::string listing;
        const DesignSummary design = decodeDesign(prototype.bytes, &listing);
        char designLine[224];
        std::snprintf(designLine, sizeof(designLine),
                      "appearance: %s design present=%d cells=%d bbox=(%d,%d)..(%d,%d)",
                      variant.name, design.present ? 1 : 0, design.cells, design.minX,
                      design.minY, design.maxX, design.maxY);
        report(designLine);
        if (!directory.empty()) {
            std::ofstream file(directory + "\\appearance-" + variant.name + ".txt",
                               std::ios::binary | std::ios::trunc);
            if (file) {
                file << "# prototype bytes for " << variant.name << " (driverX="
                     << variant.driverX << " pinX=" << variant.pinX << ")\n";
                for (size_t offset = 0; offset < sizeof(prototype.bytes); offset += 16) {
                    const size_t count = sizeof(prototype.bytes) - offset < 16
                                             ? sizeof(prototype.bytes) - offset
                                             : 16;
                    file << hexLine(prototype.bytes, offset, count) << '\n';
                }
                file << "# " << pointerWindow(prototype.bytes, 0x108) << '\n';
                file << "# " << pointerWindow(prototype.bytes, 0x570) << '\n';
                file << "# design cells:\n" << listing;
            }
        }
        if (components.releasePrototype(prototype) != tc::TCComponentStatus::Ok)
            report(std::string("appearance: variant ") + variant.name +
                   " snapshot release reported a failure");
    }

    /* Which prototype offsets move when only the internal layout moves? */
    if (dumps.size() == 3 && !dumps[0].empty() && !dumps[1].empty()) {
        size_t base = 0, wide = 0, emptyProbe = 0;
        (void)base; (void)wide; (void)emptyProbe;
        for (size_t offset = 0; offset < dumps[0].size(); offset += 8) {
            uint64_t a = 0, b = 0;
            std::memcpy(&a, dumps[0].data() + offset, sizeof(a));
            std::memcpy(&b, dumps[1].data() + offset, sizeof(b));
            if (a != b) {
                char line[128];
                std::snprintf(line, sizeof(line),
                              "appearance: +0x%03zx compact=%lld wide=%lld", offset,
                              static_cast<long long>(a), static_cast<long long>(b));
                report(line);
            }
        }
    }
    if (dumps.size() == 3 && !dumps[2].empty()) {
        size_t differing = 0;
        for (size_t offset = 0; offset < dumps[0].size(); ++offset)
            if (dumps[0][offset] != dumps[2][offset]) ++differing;
        report(std::string("appearance: the empty definition differs from the compact one at ") +
               std::to_string(differing) + " byte(s)");
    } else {
        report("appearance: the empty definition did not produce a prototype");
    }
    if (geometrySurgery) {
        tc::TCPrototype prototype{};
        if (game.getCustomPrototype(variants[0].id, prototype) &&
            replaceShape(prototype, -6, -3, 12, 6, 9) &&
            game.setCustomPrototype(variants[0].id, prototype))
            report("appearance: compact geometry replaced with (-6,-3,12,6)");
        else
            report("appearance: compact geometry replacement failed");
        components.releasePrototype(prototype);
    }
    /* The four board variants: same scaffold, different prototype surgery. */
    for (const Placement& placement : placements) {
        const auto bytes = definition(placement.id, 4, 13, false);
        const auto imported = components.importCircuit(placement.name, bytes.data(),
                                                     bytes.size(), directory.c_str());
        if (!imported.ok()) {
            report(std::string("appearance: ") + placement.name + " import failed");
            continue;
        }
        tc::TCPrototype prototype{};
        if (!game.getCustomPrototype(placement.id, prototype)) {
            report(std::string("appearance: ") + placement.name + " lookup failed");
            continue;
        }
        if (surgery && std::strcmp(placement.name, "appearance-nofields") == 0) {
            writeWord(prototype, 0x368, 0);
            writeWord(prototype, 0x508, 0);
        } else if (surgery && std::strcmp(placement.name, "appearance-flag02") == 0) {
            writeWord(prototype, 0x368, 0x0200000000000000ULL);
            writeWord(prototype, 0x508, 0);
        } else if (surgery && std::strcmp(placement.name, "appearance-painted") == 0) {
            paintDesign(prototype);
        }
        if (geometrySurgery &&
            std::strcmp(placement.name, "appearance-baseline") == 0)
            replaceShape(prototype, -6, -3, 12, 6, 9);
        game.setCustomPrototype(placement.id, prototype);
        components.releasePrototype(prototype);
        report(std::string("appearance: board variant ") + placement.name + " ready");
    }
    /* The wide target of the hit scan: same shape, output pin far to the right. */
    {
        const auto bytes = definition(kWideId, 4, 120, false);
        const auto imported = components.importCircuit("appearance-wide", bytes.data(),
                                                     bytes.size(), directory.c_str());
        if (geometrySurgery && imported.ok()) {
            tc::TCPrototype prototype{};
            if (game.getCustomPrototype(kWideId, prototype) &&
                replaceShape(prototype, -6, -3, 12, 6, 9))
                game.setCustomPrototype(kWideId, prototype);
            components.releasePrototype(prototype);
        }
        report(std::string("appearance: wide target ") +
               (imported.ok() ? "ready" : "import failed"));
    }
    {
        const auto bytes = definition(kLadderId, 4, 13, false);
        const auto imported = components.importCircuit("appearance-ladder", bytes.data(),
                                                     bytes.size(), directory.c_str());
        report(std::string("appearance: ladder ") +
               (imported.ok() ? "ready" : "import failed"));
    }
    report("appearance: probe finished");
    out->on_frame = &frame;
    return 0;
}


