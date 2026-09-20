// Development-only real-game probe for the component menu/placement path.
// It loads the sandbox level, calls the same add_component helper used by the
// component-menu builders, and verifies that the registered custom prototype
// lands in the board component array with its 64-bit identity.

#include "../sdk/tc_mod.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct V2 {
    float x, y;
};

constexpr uint64_t kAndComponentId = 0x414E44325F303031ULL;
constexpr uint8_t kCustomKind = 0x4E;
const TCHost* host;
tc::TCMod mod;
bool done;
bool imported;
bool loaded_level;
bool started;
void* model;
double start_time;
double elapsed;
double load_time;
bool (*invisible_original)(const char*, V2, int);
int test_frame = -1;
int test_button_index;
using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
UpdateWire update_original;
using AddComponent = bool (*)(void*, void*);
AddComponent add_component;
using LoadLevel = void (*)(void*, const tc::TCNimString*);
LoadLevel load_level;

void log(const std::string& message) {
    host->log(host->context, message.c_str());
}

int32_t packPoint(int16_t x, int16_t y) {
    return static_cast<int32_t>((static_cast<uint32_t>(y) << 16) |
                                static_cast<uint16_t>(x));
}

bool hookedUpdate(void* m, void* context, void* input, uint32_t point,
                  uint8_t fifth) {
    model = m;
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
        if (elapsed > 4.0 && test_button_index == 2) {
            return true;
        }
    }
    return result;
}

std::vector<unsigned char> placementFor(int16_t x, int16_t y) {
    std::vector<unsigned char> placement(0x238, 0);
    placement[0] = kCustomKind;
    const int32_t point = packPoint(x, y);
    std::memcpy(placement.data() + 2, &point, sizeof(point));
    uint64_t custom_id = kAndComponentId;
    std::memcpy(placement.data() + 0x188, &custom_id, sizeof(custom_id));

    // Defaults used by the component-menu placement template.
    const uint64_t one = 1;
    const uint64_t capacity = 0x100;
    std::memcpy(placement.data() + 0x58, &one, sizeof(one));
    std::memcpy(placement.data() + 0x60, &capacity, sizeof(capacity));
    placement[0x68] = 1;
    std::memcpy(placement.data() + 0x70, &one, sizeof(one));
    std::memcpy(placement.data() + 0x78, &capacity, sizeof(capacity));
    placement[0x80] = 1;
    return placement;
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    elapsed = value->time_seconds - start_time;
    if (done || !imported || !model || elapsed < 5.0) return;
    if (!loaded_level) {
        if (mod.game.raw_new_string) {
            const char* text = "sandbox";
            const size_t length = std::strlen(text);
            tc::TCNimString name{};
            mod.game.raw_new_string(&name, static_cast<int64_t>(length));
            name.length = length;
            std::memcpy(static_cast<unsigned char*>(name.data) + 8, text,
                        length);
            static_cast<unsigned char*>(name.data)[8 + length] = 0;
            if (load_level) load_level(model, &name);
        }
        loaded_level = true;
        load_time = elapsed;
        return;
    }
    if (elapsed < load_time + 3.0) return;
    done = true;

    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    auto finish = [&](const std::string& text) {
        log(text);
        std::ofstream(folder / "result.txt") << text;
    };

    auto* context = static_cast<unsigned char*>(model);
    uint64_t before = 0;
    std::memcpy(&before, context + 0x78, sizeof(before));
    if (before > 10000 || !add_component) {
        finish("FAIL board or placement helper unavailable");
        return;
    }

    auto placement = placementFor(30, 0);
    const bool placed = add_component(context, placement.data());
    uint64_t after = 0;
    std::memcpy(&after, context + 0x78, sizeof(after));
    log("component placement add helper=" + std::to_string(placed) +
        " before=" + std::to_string(before) +
        " after=" + std::to_string(after));
    if (!placed || after != before + 1) {
        finish("FAIL component placement helper did not commit");
        return;
    }

    void* components = nullptr;
    std::memcpy(&components, context + 0x80, sizeof(components));
    bool found = false;
    if (components && after <= 10000) {
        for (uint64_t i = 0; i < after; ++i) {
            auto* component =
                static_cast<unsigned char*>(components) + 8 + i * 0x238;
            uint16_t kind = 0;
            int16_t x = 0;
            int16_t y = 0;
            uint64_t id = 0;
            std::memcpy(&kind, component, sizeof(kind));
            std::memcpy(&x, component + 2, sizeof(x));
            std::memcpy(&y, component + 4, sizeof(y));
            std::memcpy(&id, component + 0x188, sizeof(id));
            if (kind == kCustomKind && id == kAndComponentId) {
                found = true;
                log("component placement found custom AND at " +
                    std::to_string(x) + "," + std::to_string(y) +
                    " id=" + std::to_string(id));
                break;
            }
        }
    }
    finish(found ? "PASS custom AND component placed through menu helper"
                 : "FAIL custom AND component missing from board");
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
    auto imported_result = mod.components.importCircuit(
        "AND2 Test", bytes.data(), bytes.size(), directory.c_str());
    if (!imported_result.ok() || imported_result.custom_id != kAndComponentId)
        return 4;
    imported = true;

    add_component = reinterpret_cast<AddComponent>(h->resolve_symbol(
        h->context,
        "add_component__presenterZutilitiesZhelper95functions_u5918"));
    load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
        h->context, "load_level__modelZutilities_u7740"));
    auto* update_target = h->resolve_symbol(
        h->context,
        "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
    if (!add_component || !load_level || !update_target || !invisible_target)
        return 5;
    if (h->create_hook(h->context, update_target,
                       reinterpret_cast<void*>(hookedUpdate),
                       reinterpret_cast<void**>(&update_original)) != 0)
        return 6;
    if (h->create_hook(h->context, invisible_target,
                       reinterpret_cast<void*>(hookedInvisible),
                       reinterpret_cast<void**>(&invisible_original)) != 0)
        return 7;
    plugin->on_frame = frame;
    return 0;
}
