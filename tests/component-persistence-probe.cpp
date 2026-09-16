// Development-only real-game probe for saved component reload.
// It imports the AND definition, loads and_gate from the profile's saved
// schematic, verifies the custom instance and wires, invokes the game's save
// helpers, and reports PASS. The PowerShell wrapper runs it twice in one
// isolated profile.

#include "../sdk/tc_mod.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

struct V2 {
    float x, y;
};

constexpr uint64_t kAndComponentId = 0x414E44325F303031ULL;
const TCHost* host;
tc::TCMod mod;
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

bool boardHasCustomComponent(void* context, uint64_t* wire_count) {
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
        if (kind == 0x4e && id == kAndComponentId) return true;
    }
    return false;
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

    uint64_t wires = 0;
    const bool found = boardHasCustomComponent(model, &wires);
    log("persistence board custom_found=" + std::to_string(found) +
        " wires=" + std::to_string(wires));
    if (!found || wires != 3) {
        finish("FAIL saved custom component or wires missing");
        return;
    }

    if (save_level_data) save_level_data();
    if (save_all_design_changes) save_all_design_changes();
    if (save_level_design) {
        save_level_design(model, board_ui_context ? board_ui_context : model);
    }
    finish("PASS saved custom component and three wires reloaded");
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;

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
    auto* update_target = h->resolve_symbol(
        h->context,
        "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
    if (!load_level || !save_level_data || !save_all_design_changes ||
        !save_level_design || !update_target || !invisible_target) {
        return 5;
    }
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
