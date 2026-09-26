// Development-only real-game probe for the missing-Mod case.
//
// It registers *nothing*: the level it loads contains an instance of a custom id
// that no Mod owns in this run.  The probe reports what the game actually kept -
// the component record's geometry and identity, the wires, and the raw contents
// of the component's own key/value table - and, when TC_PLACEHOLDER_SAVE=1, saves
// with the game's own saver.  It is a reporter, not a judge: the wrapper decides
// what each scenario is supposed to see, because "the component became a
// tombstone" is the answer in one scenario and the thing to prevent in another.

#include "../sdk/tc_mod.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

struct V2 {
    float x, y;
};

/* The identity the fixture's instance carries; nobody registers it here. */
constexpr uint64_t kMissingId = 0x4E4F54315F303031ULL;
constexpr int64_t kInstanceId = 0x2222222222222222LL;
/* A key only this probe uses, in the component's own keyspace but outside every
   field the host writes (src/component_tail.hpp).  Inserting it through the
   game's own table setter is what makes the save have something to write, so
   "the save kept the old record" is a measured claim: the next launch has to
   report the sentinel *and* the five original entries. */
constexpr uint64_t kSentinelKey = 0x54434D3300003000ULL;
constexpr int64_t kSentinelValue = 0x506C616365686F6CLL;

const TCHost* host;
tc::TCMod mod;
void* model;
bool loaded_level;
bool done;
bool started;
bool early_dumped;
double start_time;
double load_time;
double current_time;
bool (*invisible_original)(const char*, V2, int);
int test_frame = -1;
int test_button_index;
using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
UpdateWire update_original;
using LoadLevel = void (*)(void*, const tc::TCNimString*);
LoadLevel load_level;
tc::TCNimString* loaded_level_global;
using GetCommandSetting = uint64_t (*)(uint32_t);
GetCommandSetting get_command_setting;
using SaveThisSchematic = void (*)(const tc::TCNimString*, void*, void*, void*, uint64_t);
SaveThisSchematic save_this_schematic;

constexpr uint64_t kComponentStride = 0x238, kWireStride = 0x68, kHeader = 8;
constexpr uint64_t kCustomKind = 0x4e;
constexpr uint64_t kTailTableOffset = 0x190;
constexpr uint64_t kTailElementStride = 0x18, kTailElementHeader = 8;
using TableSet = void (*)(void*, int64_t, int64_t);
TableSet table_set;
bool sentinelInserted;
/* TC_PLACEHOLDER_TRACE=1 logs every prototype lookup the game makes while the
   level loads, with the caller's address, so the missing-Mod protection can be
   attached to the call that actually drops the component. */
bool traceLookups;
using CustomPrototypeGet = bool (*)(uint64_t, void*);
CustomPrototypeGet prototype_get_original;
uint32_t lookupCount;
using TailSetHook = void (*)(void*, int64_t, int64_t);
TailSetHook tail_set_original;
uint32_t tailSetCount;

void log(const std::string& message) {
    host->log(host->context, message.c_str());
}

uint64_t readU64(const void* base, size_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
    return value;
}
int16_t readI16(const void* base, size_t offset) {
    int16_t value = 0;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
    return value;
}
const unsigned char* readPointer(const void* base, size_t offset) {
    const unsigned char* value = nullptr;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
    return value;
}
bool readable(const void* address, size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto* begin = static_cast<const unsigned char*>(region.BaseAddress);
    const auto offset = static_cast<size_t>(static_cast<const unsigned char*>(address) - begin);
    return offset + bytes <= region.RegionSize;
}

/* Every byte the game kept for this component, in one deterministic string: the
   geometry, the identity and the table's live entries (key=value, sorted, so a
   hash order change cannot show up as a difference). */
std::string describeBoard(const void* board, bool includeSentinel) {
    std::string text;
    const uint64_t components = readU64(board, 0x78);
    const uint64_t wires = readU64(board, 0x98);
    const unsigned char* componentData = readPointer(board, 0x80);
    const unsigned char* wireData = readPointer(board, 0xa0);
    char line[256];
    uint64_t customCount = 0;
    uint64_t tombstoneCount = 0;
    for (uint64_t i = 0; i < components && componentData; ++i) {
        const unsigned char* record = componentData + kHeader + i * kComponentStride;
        /* Every slot is reported, including the kind-0 tombstones the game leaves
           behind.  "The component is gone" and "the slot is a tombstone" are
           different answers, and only a full dump tells them apart. */
        if (record[0] == 0) {
            ++tombstoneCount;
            std::snprintf(line, sizeof(line),
                          "slot=%llu kind=00 x=%d y=%d rot=%u\n",
                          static_cast<unsigned long long>(i), readI16(record, 2),
                          readI16(record, 4), static_cast<unsigned>(record[6]));
            text += line;
            continue;
        }
        if (record[0] != kCustomKind) {
            std::snprintf(line, sizeof(line), "slot=%llu kind=%02x x=%d y=%d rot=%u\n",
                          static_cast<unsigned long long>(i), record[0], readI16(record, 2),
                          readI16(record, 4), static_cast<unsigned>(record[6]));
            text += line;
            continue;
        }
        ++customCount;
        std::snprintf(line, sizeof(line),
                      "component=%llu kind=%02x x=%d y=%d rot=%u id=%016llx custom=%016llx slots=%llu live=%llu\n",
                      static_cast<unsigned long long>(i), record[0], readI16(record, 2),
                      readI16(record, 4), static_cast<unsigned>(record[6]),
                      static_cast<unsigned long long>(readU64(record, 8)),
                      static_cast<unsigned long long>(readU64(record, 0x188)),
                      static_cast<unsigned long long>(readU64(record, kTailTableOffset)),
                      static_cast<unsigned long long>(readU64(record, kTailTableOffset + 0x10)));
        text += line;
        const unsigned char* table = record + kTailTableOffset;
        const uint64_t slots = readU64(table, 0);
        const unsigned char* elements = readPointer(table, 8);
        std::vector<std::pair<uint64_t, uint64_t>> entries;
        if (slots && slots <= 0x100000 && readable(elements, kTailElementHeader + slots * kTailElementStride)) {
            for (uint64_t slot = 0; slot < slots; ++slot) {
                const unsigned char* element =
                    elements + kTailElementHeader + slot * kTailElementStride;
                if (readU64(element, 0) == 0) continue;
                if (!includeSentinel && readU64(element, 8) == kSentinelKey) continue;
                entries.emplace_back(readU64(element, 8), readU64(element, 0x10));
            }
        }
        std::sort(entries.begin(), entries.end());
        text += "  entries=" + std::to_string(entries.size());
        for (const auto& entry : entries) {
            std::snprintf(line, sizeof(line), " %016llx=%016llx",
                          static_cast<unsigned long long>(entry.first),
                          static_cast<unsigned long long>(entry.second));
            text += line;
        }
        text += "\n";
    }
    for (uint64_t i = 0; i < wires && wireData; ++i) {
        const unsigned char* wire = wireData + kHeader + i * kWireStride;
        std::snprintf(line, sizeof(line), "wire=%llu a=(%d,%d) b=(%d,%d) width=%u\n",
                      static_cast<unsigned long long>(i), readI16(wire, 0x18), readI16(wire, 0x1a),
                      readI16(wire, 0x1c), readI16(wire, 0x1e),
                      static_cast<unsigned>(readU64(wire, 0x30)));
        text += line;
    }
    text += "components=" + std::to_string(components) + " wires=" + std::to_string(wires) +
            " custom=" + std::to_string(customCount) + " tombstones=" +
            std::to_string(tombstoneCount) + "\n";
    return text;
}

bool hookedUpdate(void* m, void* context, void* input, uint32_t point, uint8_t fifth) {
    model = m;
    return update_original ? update_original(m, context, input, point, fifth) : false;
}

/* Every tail-table write the game performs while a level loads.  The
   deserializer builds each custom component's table through this same function
   (get_component__modelZsave95mongerZversionsZv13_u3+0x7ae), so these calls are
   what "the record was read" looks like from the outside. */
void hookedTailSet(void* table, int64_t key, int64_t value) {
    if (traceLookups && tailSetCount < 64) {
        ++tailSetCount;
        /* The deserializer builds the component record on its own stack and the
           table it fills lives at record+0x190 (get_component__..._v13_u3 reads
           the kind from [rsp+0xe0] and passes [rsp+0x270] here).  So the record
           under construction - kind, position, rotation, instance id, custom id -
           is readable from the table pointer, which is the foothold a rescue
           needs for a component the game is about to drop. */
        char line[256];
        const unsigned char* record = static_cast<const unsigned char*>(table) - kTailTableOffset;
        std::snprintf(line, sizeof(line),
                      "placeholder tail-set table=%p key=%016llx value=%016llx record=kind=%02x x=%d y=%d rot=%u id=%016llx custom=%016llx",
                      table, static_cast<unsigned long long>(key),
                      static_cast<unsigned long long>(value), record[0], readI16(record, 2),
                      readI16(record, 4), static_cast<unsigned>(record[6]),
                      static_cast<unsigned long long>(readU64(record, 8)),
                      static_cast<unsigned long long>(readU64(record, 0x188)));
        log(line);
    }
    if (tail_set_original) tail_set_original(table, key, value);
}

bool hookedPrototypeGet(uint64_t customId, void* prototype) {
    const bool result = prototype_get_original ? prototype_get_original(customId, prototype) : false;
    if (!traceLookups || lookupCount >= 64) return result;
    ++lookupCount;
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    char line[160];
    std::snprintf(line, sizeof(line), "placeholder lookup id=%016llx found=%d caller=0x%zx",
                  static_cast<unsigned long long>(customId), result ? 1 : 0, static_cast<size_t>(caller));
    log(line);
    return result;
}

bool hookedInvisible(const char* id, V2 size, int flags) {
    const bool result = invisible_original(id, size, flags);
    const auto rva = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                     reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (rva >= 0x449df0 && rva < 0x44b610) {
        const auto get_frame = reinterpret_cast<int (*)()>(
            host->engine_proc(host->context, "igGetFrameCount"));
        const int frame = get_frame ? get_frame() : 0;
        if (frame != test_frame) {
            test_frame = frame;
            test_button_index = 0;
        }
        ++test_button_index;
        if (current_time > 4.0 && test_button_index == 2) return true;
    }
    return result;
}

void saveSchematic() {
    if (!loaded_level_global || !model) return;
    const char* level_text = "not_gate";
    tc::TCNimString level{};
    mod.game.raw_new_string(&level, 8);
    level.length = 8;
    std::memcpy(static_cast<unsigned char*>(level.data) + 8, level_text, 8);
    static_cast<unsigned char*>(level.data)[16] = 0;
    *loaded_level_global = level;
    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !save_this_schematic || !get_command_setting) return;
    const auto path = (std::filesystem::u8path(appdata) /
        "Turing Complete Mods/profiles/default/schematics/not_gate/Default/circuit.data").generic_u8string();
    tc::TCNimString target{};
    mod.game.raw_new_string(&target, static_cast<int64_t>(path.size()));
    target.length = path.size();
    std::memcpy(static_cast<unsigned char*>(target.data) + 8, path.data(), path.size());
    static_cast<unsigned char*>(target.data)[8 + path.size()] = 0;
    void* board = static_cast<unsigned char*>(model) + 0x78;
    void* model_field = nullptr;
    std::memcpy(&model_field, static_cast<unsigned char*>(model) + 0x48, sizeof(model_field));
    save_this_schematic(&target, model, model_field, board, get_command_setting(2));
}

/* The component has no owner in this run, but the record's table is still the
   game's data structure and the game still serializes it - which is exactly what
   a placeholder has to rely on.  Returns false when the component is not on the
   board, true when its table now holds the sentinel; `inserted` says whether this
   call put it there (the next launch must find it already present). */
bool ensureSentinel(const void* board, bool* inserted) {
    if (!table_set) return false;
    const uint64_t components = readU64(board, 0x78);
    const unsigned char* componentData = readPointer(board, 0x80);
    for (uint64_t i = 0; i < components && componentData; ++i) {
        const unsigned char* record = componentData + kHeader + i * kComponentStride;
        if (record[0] != kCustomKind || readU64(record, 0x188) != kMissingId) continue;
        void* table = const_cast<unsigned char*>(record) + kTailTableOffset;
        const uint64_t slots = readU64(table, 0);
        const unsigned char* elements = readPointer(table, 8);
        if (slots && slots <= 0x100000 &&
            readable(elements, kTailElementHeader + slots * kTailElementStride)) {
            for (uint64_t slot = 0; slot < slots; ++slot) {
                const unsigned char* element =
                    elements + kTailElementHeader + slot * kTailElementStride;
                if (readU64(element, 0) != 0 && readU64(element, 8) == kSentinelKey) {
                    if (inserted) *inserted = false;
                    return true;
                }
            }
        }
        table_set(table, static_cast<int64_t>(kSentinelKey), kSentinelValue);
        if (inserted) *inserted = true;
        return true;
    }
    return false;
}

/* One launch's report.  The wrapper compares the baselines of two launches - the
   state with the probe's own sentinel filtered out - and checks that the second
   launch also sees the sentinel the first one stored. */
void report(const std::function<void(const std::string&)>& finish) {
    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    /* The dump is written first: whether or not the component is still there is
       exactly what this probe is measuring, so the evidence has to survive the
       "it is gone" answer too. */
    const bool found = ensureSentinel(model, &sentinelInserted);
    const std::string text = describeBoard(model, true);
    const std::string baseline = describeBoard(model, false);
    log("placeholder board:\n" + text);
    std::ofstream(folder / "report.txt", std::ios::trunc) << text;
    std::ofstream(folder / "baseline.txt", std::ios::trunc) << baseline;
    std::string summary = baseline;
    while (!summary.empty() && (summary.back() == '\n' || summary.back() == '\r')) summary.pop_back();
    summary = summary.substr(summary.find_last_of('\n') + 1);
    const char* save = std::getenv("TC_PLACEHOLDER_SAVE");
    const bool saving = save && save[0] && save[0] != '0';
    if (saving) saveSchematic();
    finish(std::string("PASS placeholder component=") + (found ? "present" : "missing") +
           " sentinel=" +
           (found ? (sentinelInserted ? "inserted" : "present") : "missing") + " saved=" +
           (saving ? "1" : "0") + " " + summary);
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    current_time = value->time_seconds - start_time;
    if (done || !model || current_time < 5.0) return;
    if (!loaded_level) {
        const char* text = "not_gate";
        const size_t length = std::strlen(text);
        tc::TCNimString name{};
        mod.game.raw_new_string(&name, static_cast<int64_t>(length));
        name.length = length;
        std::memcpy(static_cast<unsigned char*>(name.data) + 8, text, length);
        static_cast<unsigned char*>(name.data)[8 + length] = 0;
        load_level(model, &name);
        loaded_level = true;
        load_time = current_time;
        /* The board as it is the moment the load call returns.  Comparing this
           with the later dump says whether the component was dropped by the
           deserializer itself or by a pass that runs after the load. */
        log("placeholder board (immediately after load_level):\n" + describeBoard(model, true));
        early_dumped = true;
        return;
    }
    if (current_time < load_time + 3.0) return;
    done = true;
    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    auto finish = [&](const std::string& text) {
        log(text);
        std::ofstream(folder / "result.txt") << text;
    };
    report(finish);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid()) return 2;
    if (const char* trace = std::getenv("TC_PLACEHOLDER_TRACE"))
        traceLookups = trace[0] && trace[0] != '0';
    load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
        h->context, "load_level__modelZutilities_u7740"));
    loaded_level_global = static_cast<tc::TCNimString*>(h->resolve_symbol(
        h->context, "loaded_level__modelZmodel95types_u840"));
    get_command_setting = reinterpret_cast<GetCommandSetting>(h->resolve_symbol(
        h->context, "get_command_setting__modelZsimulator95types_u124"));
    save_this_schematic = reinterpret_cast<SaveThisSchematic>(h->resolve_symbol(
        h->context, "save_this_schematic__modelZboardZschematics_u132"));
    table_set = reinterpret_cast<TableSet>(h->resolve_symbol(
        h->context, "X5BX5Deq___modelZsave95mongerZversionsZv7_u70"));
    auto* update_target = h->resolve_symbol(
        h->context,
        "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
    if (!load_level || !loaded_level_global || !get_command_setting ||
        !save_this_schematic || !table_set || !update_target || !invisible_target)
        return 3;
    if (h->create_hook(h->context, update_target, reinterpret_cast<void*>(hookedUpdate),
                       reinterpret_cast<void**>(&update_original)) != 0)
        return 4;
    if (h->create_hook(h->context, invisible_target, reinterpret_cast<void*>(hookedInvisible),
                       reinterpret_cast<void**>(&invisible_original)) != 0)
        return 5;
    if (traceLookups) {
        auto* lookup = h->resolve_symbol(
            h->context, "get_custom_prototype__modelZboardZcustom95prototype95list_u451");
        if (lookup && h->create_hook(h->context, lookup, reinterpret_cast<void*>(hookedPrototypeGet),
                                     reinterpret_cast<void**>(&prototype_get_original)) != 0)
            return 6;
        auto* setter = h->resolve_symbol(
            h->context, "X5BX5Deq___modelZsave95mongerZversionsZv7_u70");
        if (setter && h->create_hook(h->context, setter, reinterpret_cast<void*>(hookedTailSet),
                                     reinterpret_cast<void**>(&tail_set_original)) != 0)
            return 7;
        log("placeholder trace: prototype lookups and tail writes armed");
    }
    plugin->on_frame = frame;
    return 0;
}
