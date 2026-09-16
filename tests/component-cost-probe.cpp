// Development-only real-game probe for the gate/delay score of a board that
// contains an imported circuit component.  It hooks the game's own cost and
// score functions, loads the and_gate level from the profile's saved
// schematic, and reports what the game computed.
//
// Modes (read from <plugin-data>/mode.txt):
//   plain  - register the circuit component, do not touch the score tables
//   insert - additionally call scores.add_cost(0x4e, prototype gate/delay)

#include "../sdk/tc_mod.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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
int stage = 0;
double start_time;
double stage_time;
double current_time;
std::string mode = "plain";
bool (*invisible_original)(const char*, V2, int);
int test_frame = -1;
int test_button_index;
using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
UpdateWire update_original;
using LoadLevel = void (*)(void*, const tc::TCNimString*);
LoadLevel load_level;

// --- hooks -----------------------------------------------------------------

using GetCostFn = void* (*)(void* result, const void* component);
using GetDelayCostFn = int64_t (*)(const void* component, int64_t fallback);
using BuildScoresFn = void (*)(void* score, void* a1, void* a2, double scale);
using AddCostFn = void (*)(uint64_t kind, const uint64_t* pair);

GetCostFn get_cost_original;
GetDelayCostFn get_delay_cost_original;
BuildScoresFn build_scores_original;
AddCostFn add_cost;

// per-kind observations
std::map<int, int> cost_calls;
std::map<int, std::pair<int64_t, int64_t>> cost_result;
std::map<int, int> delay_calls;
std::map<int, int64_t> delay_result;
bool have_scores = false;
int64_t score_gates = 0;
int64_t score_delay = 0;
uint8_t score_flag = 0;
int score_calls = 0;
bool have_before = false;
int64_t before_gates = 0;
int64_t before_delay = 0;
int64_t cycle_before = -1;
int64_t cycle_after = -1;

void log(const std::string& message) {
    host->log(host->context, message.c_str());
}

void* hookedGetCost(void* result, const void* component) {
    void* out = get_cost_original(result, component);
    const auto kind = *static_cast<const uint8_t*>(component);
    int64_t gates = 0;
    int64_t delay = 0;
    std::memcpy(&gates, static_cast<const unsigned char*>(result), 8);
    std::memcpy(&delay, static_cast<const unsigned char*>(result) + 8, 8);
    ++cost_calls[kind];
    cost_result[kind] = {gates, delay};
    return out;
}

int64_t hookedGetDelayCost(const void* component, int64_t fallback) {
    const int64_t value = get_delay_cost_original(component, fallback);
    const auto kind = *static_cast<const uint8_t*>(component);
    ++delay_calls[kind];
    delay_result[kind] = value;
    return value;
}

void hookedBuildScores(void* score, void* a1, void* a2, double scale) {
    build_scores_original(score, a1, a2, scale);
    if (!score) return;
    std::memcpy(&score_gates, static_cast<unsigned char*>(score), 8);
    std::memcpy(&score_delay, static_cast<unsigned char*>(score) + 8, 8);
    score_flag = static_cast<unsigned char*>(score)[0x20];
    have_scores = true;
    ++score_calls;
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

// --- board helpers ---------------------------------------------------------

struct BoardComponent {
    uint16_t kind = 0;
    uint64_t id = 0;
    int64_t gates = 0;
    int64_t delay = 0;
};

std::vector<BoardComponent> readBoard(void* context) {
    std::vector<BoardComponent> out;
    auto* base = static_cast<unsigned char*>(context);
    uint64_t components = 0;
    void* component_data = nullptr;
    std::memcpy(&components, base + 0x78, sizeof(components));
    std::memcpy(&component_data, base + 0x80, sizeof(component_data));
    if (!component_data || components > 10000) return out;
    for (uint64_t i = 0; i < components; ++i) {
        auto* component =
            static_cast<unsigned char*>(component_data) + 8 + i * 0x238;
        BoardComponent entry;
        std::memcpy(&entry.kind, component, sizeof(entry.kind));
        std::memcpy(&entry.id, component + 0x188, sizeof(entry.id));
        out.push_back(entry);
    }
    return out;
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    current_time = value->time_seconds - start_time;
    if (done || !model || current_time < 5.0) return;
    if (stage == 0) {
        const char* text = "and_gate";
        const size_t length = std::strlen(text);
        tc::TCNimString name{};
        mod.game.raw_new_string(&name, static_cast<int64_t>(length));
        name.length = length;
        std::memcpy(static_cast<unsigned char*>(name.data) + 8, text, length);
        static_cast<unsigned char*>(name.data)[8 + length] = 0;
        load_level(model, &name);
        loaded_level = true;
        stage_time = current_time;
        stage = 1;
        return;
    }
    if (stage == 1) {
        if (current_time < stage_time + 3.0) return;
        before_gates = score_gates;
        before_delay = score_delay;
        have_before = have_scores;
        cycle_before = mod.simulation.cycle();
        mod.simulation.run(model, 8);
        stage_time = current_time;
        stage = 2;
        return;
    }
    if (current_time < stage_time + 3.0) return;
    done = true;
    cycle_after = mod.simulation.cycle();

    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    std::ostringstream report;
    auto finish = [&](const std::string& text) {
        log(text);
        std::ofstream(folder / "result.txt") << text;
    };

    tc::TCPrototype prototype{};
    const bool have_prototype =
        mod.game.getCustomPrototype(kAndComponentId, prototype);
    const int64_t prototype_gates =
        have_prototype ? static_cast<int64_t>(tc::prototypeGateCost(prototype)) : -1;
    const int64_t prototype_delay =
        have_prototype ? static_cast<int64_t>(tc::prototypeDelay(prototype)) : -1;
    if (have_prototype) mod.components.releasePrototype(prototype);

    const auto board = readBoard(model);
    report << "mode=" << mode << "\n";
    report << "prototype gates=" << prototype_gates
           << " delay=" << prototype_delay << "\n";
    report << "board components=" << board.size() << "\n";
    for (const auto& entry : board) {
        report << "  kind=0x" << std::hex << entry.kind << std::dec;
        if (entry.kind == 0x4e) report << " id=" << entry.id;
        report << "\n";
    }
    report << "get_cost calls by kind:\n";
    for (const auto& entry : cost_calls) {
        const auto result = cost_result[entry.first];
        report << "  kind=0x" << std::hex << entry.first << std::dec
               << " calls=" << entry.second << " gates=" << result.first
               << " delay=" << result.second << "\n";
    }
    report << "get_delay_cost calls by kind:\n";
    for (const auto& entry : delay_calls) {
        report << "  kind=0x" << std::hex << entry.first << std::dec
               << " calls=" << entry.second
               << " last=" << delay_result[entry.first] << "\n";
    }
    report << "build_scores calls=" << score_calls
           << " gates=" << (have_scores ? score_gates : -1)
           << " delay=" << (have_scores ? score_delay : -1)
           << " flag=" << static_cast<int>(score_flag) << "\n";
    report << "before sim: gates=" << before_gates << " delay=" << before_delay
           << " cycle=" << cycle_before << "\n";
    report << "after sim: cycle=" << cycle_after << "\n";

    finish(report.str());
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;

    const auto folder = std::filesystem::u8path(h->data_directory_utf8);
    {
        std::ifstream mode_file(folder / "mode.txt");
        std::string text;
        if (mode_file) std::getline(mode_file, text);
        if (!text.empty()) mode = text;
    }

    // "builtin" runs the same level geometry with the game's own AND gate and
    // no imported component, as a reference for what the score should read.
    if (mode != "builtin") {
        std::ifstream file(folder / "fixtures" / "and2_component.data",
                           std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)), {});
        if (bytes.empty()) return 3;
        const auto directory = (folder / "fixtures").generic_u8string() + "/";
        auto result = mod.components.importCircuit(
            "AND2 Test", bytes.data(), bytes.size(), directory.c_str());
        if (!result.ok() || result.custom_id != kAndComponentId) return 4;
        imported = true;

        tc::TCPrototype prototype{};
        if (!mod.game.getCustomPrototype(kAndComponentId, prototype)) return 5;
        const uint64_t gate_cost = tc::prototypeGateCost(prototype);
        const uint64_t delay_cost = tc::prototypeDelay(prototype);
        mod.components.releasePrototype(prototype);

        if (mode == "insert") {
            add_cost = reinterpret_cast<AddCostFn>(
                h->resolve_symbol(h->context, "add_cost__modelZscores_u2110"));
            if (add_cost) {
                const uint64_t pair[2] = {gate_cost, delay_cost};
                add_cost(0x4e, pair);
            }
        }
    }

    load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
        h->context, "load_level__modelZutilities_u7740"));
    auto* update_target = h->resolve_symbol(
        h->context,
        "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
    auto* get_cost_target =
        h->resolve_symbol(h->context, "get_cost__modelZscores_u2321");
    auto* get_delay_target =
        h->resolve_symbol(h->context, "get_delay_cost__modelZscores_u2316");
    auto* build_scores_target = h->resolve_symbol(
        h->context, "build_scores__presenterZboard95uiZmenu95bar_u886");
    if (!load_level || !update_target || !invisible_target || !get_cost_target ||
        !get_delay_target || !build_scores_target) {
        return 6;
    }
    if (h->create_hook(h->context, update_target,
                       reinterpret_cast<void*>(hookedUpdate),
                       reinterpret_cast<void**>(&update_original)) != 0) {
        return 7;
    }
    if (h->create_hook(h->context, invisible_target,
                       reinterpret_cast<void*>(hookedInvisible),
                       reinterpret_cast<void**>(&invisible_original)) != 0) {
        return 8;
    }
    if (h->create_hook(h->context, get_cost_target,
                       reinterpret_cast<void*>(hookedGetCost),
                       reinterpret_cast<void**>(&get_cost_original)) != 0) {
        return 9;
    }
    if (h->create_hook(h->context, get_delay_target,
                       reinterpret_cast<void*>(hookedGetDelayCost),
                       reinterpret_cast<void**>(&get_delay_cost_original)) != 0) {
        return 10;
    }
    if (h->create_hook(h->context, build_scores_target,
                       reinterpret_cast<void*>(hookedBuildScores),
                       reinterpret_cast<void**>(&build_scores_original)) != 0) {
        return 11;
    }
    plugin->on_frame = frame;
    return 0;
}
