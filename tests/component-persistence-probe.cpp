// Development-only real-game probe for saved component reload.
// It imports the AND definition, loads and_gate from the profile's saved
// schematic, verifies the custom instance and wires, invokes the game's save
// helpers, and reports PASS. The PowerShell wrapper runs it twice in one
// isolated profile.
//
// TC_PERSISTENCE_MODE=insert switches the same fixture to the route the host
// is meant to adopt: every table change goes through the game's own Table
// setter (save_monger/versions/v7 `[]=`), starting from a table the fixture
// left empty, so the run proves allocation, growth, save and read-back
// without the probe touching a bucket again.

#include "../sdk/tc_mod.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>

namespace {

struct V2 {
    float x, y;
};

/* Which route the run exercises.  Inplace is the historical bucket-scan probe;
   insert is the game-setter route described by docs/HANDOFF-component-m3.md. */
enum class Mode {
    Inplace,
    Insert,
};

constexpr uint64_t kAndComponentId = 0x414E44325F303031ULL;
const TCHost* host;
tc::TCMod mod;
Mode mode = Mode::Inplace;
void* model;
void* board_ui_context;
bool imported;
bool loaded_level;
bool done;
bool started;
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
using SaveLevelData = void (*)();
SaveLevelData save_level_data;
using SaveAllDesignChanges = void (*)();
SaveAllDesignChanges save_all_design_changes;
using SaveLevelDesign = void (*)(void*, void*);
SaveLevelDesign save_level_design;
tc::TCNimString* loaded_level_global;
using SetSetting = void (*)(void*, int64_t, int64_t, int64_t);
SetSetting set_setting;
using UpgradeContext = void (*)(void*, uint8_t);
UpgradeContext upgrade_context;
using GetCommandSetting = uint64_t (*)(uint32_t);
GetCommandSetting get_command_setting;
using SaveThisSchematic = void (*)(const tc::TCNimString*, void*, void*, void*, uint64_t);
SaveThisSchematic save_this_schematic;
/* save_monger/versions/v7 Table[int64, int64] `[]=`: (table*, key, value).  The
   game's own custom-tail deserializer calls exactly this shape while it rebuilds
   a 0x4e record from a saved schematic. */
using TableSet = void (*)(void*, int64_t, int64_t);
TableSet table_set;
constexpr int64_t kInitialSetting = 0x13579BDF2468ACE;
constexpr int64_t kSavedSetting = 0x2468ACE13579BDF;
constexpr uint64_t kStorageKey = 0x54434D3343464701ULL;

/* The insert route fills the table past the first growth threshold.  The game's
   setter starts an empty table at 64 slots and doubles once the live count
   passes two thirds of that capacity, so 48 extra keys force one enlarge
   without turning the fixture into a stress test. */
constexpr int kFillEntries = 48;
constexpr uint64_t kFillKeyBase = 0x54434D3343464800ULL;

/* Table layout, read out of the game's own setter and deserializer:
     table + 0x00  int64  data sequence length (slot count, a power of two)
     table + 0x08  ptr    data sequence payload
     table + 0x10  int64  live entry count
   Element i lives at payload + 8 + i * 24 - the sequence carries an eight-byte
   header - with the key hash at +0, the key at +8 and the value at +0x10.  A
   zero hash marks a free slot. */
constexpr uint64_t kTableCountOffset = 0x10;
constexpr uint64_t kElementStride = 24;
constexpr uint64_t kElementHeader = 8;

uint64_t readU64(const void* base, size_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset,
                sizeof(value));
    return value;
}

const unsigned char* readPointer(const void* base, size_t offset) {
    const unsigned char* value = nullptr;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset,
                sizeof(value));
    return value;
}

/* A pointer is only followed when the page holding it is committed and
   readable, so a stale table can never turn into an access violation. */
bool readable(const void* address, size_t bytes) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const auto* begin = static_cast<const unsigned char*>(region.BaseAddress);
    return static_cast<size_t>(static_cast<const unsigned char*>(address) - begin) + bytes <=
           region.RegionSize;
}

/* Reads one key through the layout above.  Returns false when the table cannot
   be walked at all, which is different from "the key is absent". */
bool tailLookup(const void* table, uint64_t key, uint64_t* value) {
    if (!readable(table, kTableCountOffset + sizeof(uint64_t))) return false;
    const uint64_t slots = readU64(table, 0);
    const unsigned char* elements = readPointer(table, 8);
    if (!slots || slots > 0x100000 || !elements) return true;  // empty or unusable
    if (!readable(elements, kElementHeader + slots * kElementStride)) return false;
    for (uint64_t i = 0; i < slots; ++i) {
        const unsigned char* element = elements + kElementHeader + i * kElementStride;
        if (readU64(element, 0) == 0) continue;
        if (readU64(element, 8) != key) continue;
        if (value) *value = readU64(element, 0x10);
        return true;
    }
    return true;
}

/* True when the pair really sits in the table's own storage.  This is the
   check that would catch a wrong stride: the value has to follow the key. */
bool tailHoldsPair(const void* table, uint64_t key, uint64_t value) {
    const uint64_t slots = readU64(table, 0);
    const unsigned char* elements = readPointer(table, 8);
    if (!slots || slots > 0x100000 || !elements) return false;
    if (!readable(elements, kElementHeader + slots * kElementStride)) return false;
    for (uint64_t i = 0; i < slots; ++i) {
        const unsigned char* element = elements + kElementHeader + i * kElementStride;
        if (readU64(element, 8) == key && readU64(element, 0x10) == value) return true;
    }
    return false;
}

void log(const std::string& message) {
    host->log(host->context, message.c_str());
}

bool hookedUpdate(void* m, void* context, void* input, uint32_t point,
                  uint8_t fifth) {
    model = m;
    board_ui_context = context;
    return update_original ? update_original(m, context, input, point, fifth)
                           : false;
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

bool boardHasCustomComponent(void* context, uint64_t* wire_count,
                             uint64_t* component_index, unsigned char** found_record) {
    auto* base = static_cast<unsigned char*>(context);
    uint64_t components = 0;
    uint64_t wires = 0;
    void* component_data = nullptr;
    std::memcpy(&components, base + 0x78, sizeof(components));
    std::memcpy(&wires, base + 0x98, sizeof(wires));
    std::memcpy(&component_data, base + 0x80, sizeof(component_data));
    if (wire_count) *wire_count = wires;
    if (!component_data || components > 10000) return false;
    for (uint64_t i = 0; i < components; ++i) {
        auto* component =
            static_cast<unsigned char*>(component_data) + 8 + i * 0x238;
        uint16_t kind = 0;
        uint64_t id = 0;
        std::memcpy(&kind, component, sizeof(kind));
        std::memcpy(&id, component + 0x188, sizeof(id));
        if (kind == 0x4e && id == kAndComponentId) {
            if (component_index) *component_index = i;
            if (found_record) *found_record = component;
            return true;
        }
    }
    return false;
}

/* The save the wrapper asserts on.  `loaded_level` has to name the level the
   fixture was installed as, because save_this_schematic reads it. */
void saveSchematic() {
    if (!loaded_level_global) return;
    const char* level_text = "and_gate";
    tc::TCNimString level{};
    mod.game.raw_new_string(&level, 8);
    level.length = 8;
    std::memcpy(static_cast<unsigned char*>(level.data) + 8, level_text, 8);
    static_cast<unsigned char*>(level.data)[16] = 0;
    *loaded_level_global = level;
    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !save_this_schematic || !get_command_setting || !model) return;
    const auto path = (std::filesystem::u8path(appdata) /
        "Turing Complete Mods/profiles/default/schematics/and_gate/Default/circuit.data").generic_u8string();
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

/* The route the host is meant to adopt: the probe never writes a bucket.  It
   starts from the empty table the fixture shipped, inserts through the game's
   own setter, grows the table once, and saves; the next launch asserts that the
   value the game deserialized is the one the setter stored. */
void runInsert(unsigned char* record,
               const std::function<void(const std::string&)>& finish) {
    unsigned char* table = record + 0x190;
    const uint64_t slotsBefore = readU64(table, 0);
    const uint64_t countBefore = readU64(table, kTableCountOffset);
    char before[96];
    std::snprintf(before, sizeof(before), "persistence insert table before: slots=%llu count=%llu",
                  static_cast<unsigned long long>(slotsBefore),
                  static_cast<unsigned long long>(countBefore));
    log(before);
    uint64_t current = 0;
    if (!tailLookup(table, kStorageKey, &current)) {
        finish("FAIL insert route saw an unreadable custom tail table");
        return;
    }
    bool wrote = false;
    uint64_t expected = 0;
    uint64_t slotsAfter = slotsBefore;
    std::string step;
    if (countBefore == 0) {
        table_set(table, static_cast<int64_t>(kStorageKey), kInitialSetting);
        wrote = true;
        expected = kInitialSetting;
        step = "inserted";
        const uint64_t count = readU64(table, kTableCountOffset);
        const uint64_t slots = readU64(table, 0);
        if (count != 1) {
            finish("FAIL insert did not add exactly one entry, count=" + std::to_string(count));
            return;
        }
        if (slots < 64 || slots > 0x100000) {
            finish("FAIL insert did not allocate the table, slots=" + std::to_string(slots));
            return;
        }
        for (int i = 0; i < kFillEntries; ++i)
            table_set(table, static_cast<int64_t>(kFillKeyBase + i),
                      static_cast<int64_t>(i + 1));
        slotsAfter = readU64(table, 0);
        const uint64_t filled = readU64(table, kTableCountOffset);
        if (filled != 1 + kFillEntries) {
            finish("FAIL insert fill count=" + std::to_string(filled) +
                   " expected=" + std::to_string(1 + kFillEntries));
            return;
        }
        if (slotsAfter <= slots) {
            finish("FAIL insert did not grow, slots=" + std::to_string(slots) + "->" +
                   std::to_string(slotsAfter));
            return;
        }
    } else if (current == static_cast<uint64_t>(kInitialSetting)) {
        /* The game rebuilt this table from the saved schematic: the value below
           is what the deserializer stored. */
        step = "readback " + std::to_string(kInitialSetting);
        table_set(table, static_cast<int64_t>(kStorageKey), kSavedSetting);
        wrote = true;
        expected = kSavedSetting;
        step += " then updated";
        slotsAfter = readU64(table, 0);
    } else if (current == static_cast<uint64_t>(kSavedSetting)) {
        step = "readback " + std::to_string(kSavedSetting) + " stable";
        expected = kSavedSetting;
        slotsAfter = readU64(table, 0);
    } else {
        finish("FAIL insert route found an unexpected persisted value " +
               std::to_string(current));
        return;
    }
    uint64_t observed = 0;
    if (!tailLookup(table, kStorageKey, &observed) || observed != expected) {
        finish("FAIL insert route value=" + std::to_string(observed) +
               " expected=" + std::to_string(expected));
        return;
    }
    if (!tailHoldsPair(table, kStorageKey, expected)) {
        finish("FAIL insert route table does not hold the key/value pair");
        return;
    }
    const uint64_t count = readU64(table, kTableCountOffset);
    if (count != 1 + kFillEntries) {
        finish("FAIL insert route entry count=" + std::to_string(count));
        return;
    }
    if (wrote) saveSchematic();
    finish("PASS insert " + step + " value=" + std::to_string(expected) +
           " count=" + std::to_string(count) + " slots=" + std::to_string(slotsAfter));
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    current_time = value->time_seconds - start_time;
    if (done || !imported || !model || current_time < 5.0) return;
    if (!loaded_level) {
        const char* text = "and_gate";
        const size_t length = std::strlen(text);
        tc::TCNimString name{};
        mod.game.raw_new_string(&name, static_cast<int64_t>(length));
        name.length = length;
        std::memcpy(static_cast<unsigned char*>(name.data) + 8, text, length);
        static_cast<unsigned char*>(name.data)[8 + length] = 0;
        load_level(model, &name);
        loaded_level = true;
        load_time = current_time;
        return;
    }
    if (current_time < load_time + 3.0) return;
    done = true;

    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    auto finish = [&](const std::string& text) {
        log(text);
        std::ofstream(folder / "result.txt") << text;
    };

    uint64_t wires = 0, component_index = 0;
    unsigned char* record = nullptr;
    const bool found = boardHasCustomComponent(model, &wires, &component_index, &record);
    log("persistence board custom_found=" + std::to_string(found) +
        " wires=" + std::to_string(wires));
    if (!found || wires != 3 || !record) {
        finish("FAIL saved custom component or wires missing");
        return;
    }
    if (mode == Mode::Insert) {
        runInsert(record, finish);
        return;
    }
    std::string tail = "persistence custom tail";
    for (size_t offset = 0x190; offset < 0x238; offset += 8) {
        uint64_t value = 0;
        std::memcpy(&value, record + offset, sizeof(value));
        char field[48];
        std::snprintf(field, sizeof(field), " +%03zx=%016llx", offset,
                      static_cast<unsigned long long>(value));
        tail += field;
    }
    log(tail);
    const unsigned char* custom_table = nullptr;
    std::memcpy(&custom_table, record + 0x198, sizeof(custom_table));
    MEMORY_BASIC_INFORMATION region{};
    uint64_t* saved_value = nullptr;
    if (custom_table && VirtualQuery(custom_table, &region, sizeof(region)) == sizeof(region) &&
        region.State == MEM_COMMIT && !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        const size_t available = std::min<size_t>(
            0x4000, static_cast<const unsigned char*>(region.BaseAddress) + region.RegionSize - custom_table);
        std::string hits = "persistence custom table";
        for (size_t offset = 0; offset + sizeof(uint64_t) <= available; offset += 8) {
            uint64_t value = 0;
            std::memcpy(&value, custom_table + offset, sizeof(value));
            if (value == kStorageKey || value == static_cast<uint64_t>(kInitialSetting) ||
                value == static_cast<uint64_t>(kSavedSetting)) {
                char field[64];
                std::snprintf(field, sizeof(field), " +%zx=%016llx", offset,
                              static_cast<unsigned long long>(value));
                hits += field;
                if (value == static_cast<uint64_t>(kInitialSetting) && offset >= 8) {
                    uint64_t key = 0;
                    std::memcpy(&key, custom_table + offset - 8, sizeof(key));
                    if (key == kStorageKey)
                        saved_value = reinterpret_cast<uint64_t*>(const_cast<unsigned char*>(custom_table) + offset);
                }
            }
        }
        log(hits);
    }
    uint64_t current = 0;
    if (saved_value) current = *saved_value;
    else {
        const size_t available = custom_table ? 0x4000 : 0;
        for (size_t offset = 8; offset + sizeof(uint64_t) <= available; offset += 8) {
            uint64_t key = 0, value = 0;
            std::memcpy(&key, custom_table + offset - 8, sizeof(key));
            std::memcpy(&value, custom_table + offset, sizeof(value));
            if (key == kStorageKey) {
                current = value;
                if (value == static_cast<uint64_t>(kInitialSetting))
                    saved_value = reinterpret_cast<uint64_t*>(const_cast<unsigned char*>(custom_table) + offset);
                break;
            }
        }
    }
    if (current != static_cast<uint64_t>(kSavedSetting) && !saved_value) {
        finish("FAIL custom storage pair missing");
        return;
    }
    if (saved_value) {
        *saved_value = static_cast<uint64_t>(kSavedSetting);
        log("persistence custom table value changed in place before save");
        saveSchematic();
    }
    finish(std::string("PASS custom component storage value=") +
           std::to_string(saved_value ? kSavedSetting : static_cast<int64_t>(current)));
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;
    if (const char* requested = std::getenv("TC_PERSISTENCE_MODE"))
        if (std::strcmp(requested, "insert") == 0) mode = Mode::Insert;

    const auto folder = std::filesystem::u8path(h->data_directory_utf8);
    std::ifstream file(folder / "fixtures" / "and2_component.data",
                       std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.empty()) return 3;
    const auto directory = (folder / "fixtures").generic_u8string() + "/";
    auto result = mod.components.importCircuit(
        "AND2 Test", bytes.data(), bytes.size(), directory.c_str());
    if (!result.ok() || result.custom_id != kAndComponentId) return 4;
    imported = true;

    load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
        h->context, "load_level__modelZutilities_u7740"));
    save_level_data = reinterpret_cast<SaveLevelData>(h->resolve_symbol(
        h->context, "save_level_data__modelZutilities_u5683"));
    save_all_design_changes =
        reinterpret_cast<SaveAllDesignChanges>(h->resolve_symbol(
            h->context,
            "save_all_design_changes__presenterZutilitiesZhelper95functions_u9517"));
    save_level_design = reinterpret_cast<SaveLevelDesign>(h->resolve_symbol(
        h->context, "save_level_design__presenterZutilities_u16021"));
    loaded_level_global = static_cast<tc::TCNimString*>(h->resolve_symbol(
        h->context, "loaded_level__modelZmodel95types_u840"));
    set_setting = reinterpret_cast<SetSetting>(h->resolve_symbol(
        h->context, "set_setting__presenterZutilitiesZhelper95functions_u2763"));
    upgrade_context = reinterpret_cast<UpgradeContext>(h->resolve_symbol(
        h->context, "upgrade__presenterZcontext_u2766"));
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
    if (!load_level || !save_level_data || !save_all_design_changes ||
        !save_level_design || !loaded_level_global || !set_setting || !upgrade_context ||
        !get_command_setting || !save_this_schematic ||
        !update_target || !invisible_target) {
        return 5;
    }
    if (mode == Mode::Insert && !table_set) return 8;
    if (h->create_hook(h->context, update_target,
                       reinterpret_cast<void*>(hookedUpdate),
                       reinterpret_cast<void**>(&update_original)) != 0) {
        return 6;
    }
    if (h->create_hook(h->context, invisible_target,
                       reinterpret_cast<void*>(hookedInvisible),
                       reinterpret_cast<void**>(&invisible_original)) != 0) {
        return 7;
    }
    plugin->on_frame = frame;
    return 0;
}
