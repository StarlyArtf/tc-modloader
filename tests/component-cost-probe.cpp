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
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct V2 {
    float x, y;
};

// The probe accepts any definition file; the id comes from the file itself.
uint64_t customComponentId = 0;
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
bool declared = false;
uint64_t declareGates = 0;
uint64_t declareDelay = 0;
std::string level_name = "and_gate";
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
using BuildScoresFn = void (*)(void* score, void* a1, float a2, double scale);
using AddCostFn = void (*)(uint64_t kind, const uint64_t* pair);

GetCostFn get_cost_original;
std::recursive_mutex stats_mutex;
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

/* ---- optional per-cycle state sampling (gate-delay M2 evidence) -----------

   With TC_GATE_DELAY_SAMPLE=1 the probe stops asking for the whole 8-cycle run
   at once, steps the simulation one cycle at a time through
   TC_SERVICE_SIMULATION and records the low state slots after each step.  The
   report then names the slots that moved - which is how "the ring really
   oscillates under the delay model" is told from "the board is a constant
   driven acyclic circuit and never moves".  Off by default: the cost case's own
   output must not change. */
bool sampling = false;
bool have_sim = false;
tc::simulation::Api sim_api{};
int64_t sample_cycles = 8;
uint32_t sample_slots = 512;
int64_t target_cycle = 0;
std::vector<std::vector<unsigned char>> sample_rows;
/* Which cycle each sampled row landed on.  A row that skipped a cycle is what
   makes a two-unit oscillation look constant at cycle boundaries, so the report
   has to show it. */
std::vector<int64_t> sample_row_cycles;

bool sampleRow() {
    if (!have_sim) return false;
    std::vector<TCSimChannelV1> channels(sample_slots);
    std::vector<uint64_t> values(sample_slots, 0);
    for (uint32_t at = 0; at < sample_slots; ++at) {
        channels[at].size = sizeof(TCSimChannelV1);
        channels[at].version = TCSIM_CHANNEL_VERSION_1;
        channels[at].byte_offset = at;
        channels[at].bits = 8;
    }
    int64_t cycle = -1;
    uint32_t stable = 0;
    if (tc::simulation::sample(sim_api, channels.data(), sample_slots, values.data(), &cycle,
                               &stable) != TC_SIMULATION_OK)
        return false;
    std::vector<unsigned char> row(sample_slots, 0);
    for (uint32_t at = 0; at < sample_slots; ++at)
        row[at] = static_cast<unsigned char>(values[at] & 0xff);
    sample_rows.push_back(std::move(row));
    sample_row_cycles.push_back(cycle);
    return true;
}

void appendSamplingReport(std::ostringstream& report) {
    if (!sampling) return;
    report << "sampled cycles=" << sample_rows.size() << " slots=" << sample_slots << "\n";
    report << "  at cycles=";
    for (size_t row = 0; row < sample_row_cycles.size(); ++row)
        report << sample_row_cycles[row] << (row + 1 == sample_row_cycles.size() ? "" : ",");
    report << "\n";
    if (sample_rows.empty()) {
        report << "changed slots=0\n";
        return;
    }
    std::vector<uint32_t> changed;
    std::vector<uint32_t> moves;
    for (uint32_t at = 0; at < sample_slots; ++at) {
        uint32_t moved = 0;
        for (size_t row = 1; row < sample_rows.size(); ++row)
            if (sample_rows[row][at] != sample_rows[row - 1][at]) ++moved;
        if (moved) {
            changed.push_back(at);
            moves.push_back(moved);
        }
    }
    report << "changed slots=" << changed.size() << "\n";
    for (size_t i = 0; i < changed.size() && i < 12; ++i) {
        const uint32_t at = changed[i];
        report << "  slot " << at << " moves=" << moves[i] << " seq=";
        for (size_t row = 0; row < sample_rows.size(); ++row)
            report << static_cast<int>(sample_rows[row][at])
                   << (row + 1 == sample_rows.size() ? "" : ",");
        report << "\n";
    }
    if (changed.size() > 12) report << "  (" << (changed.size() - 12) << " more)\n";
    /* The board's own slots sit at the bottom of the buffer; print them whatever
       they did, so "the ring did not move" can be told from "the ring moved but
       in a pattern the change counter missed" and from "the sample read nothing
       at all". */
    report << "first slots:\n";
    for (uint32_t at = 256; at < 272 && at < sample_slots; ++at) {
        report << "  slot " << at << " seq=";
        for (size_t row = 0; row < sample_rows.size(); ++row)
            report << static_cast<int>(sample_rows[row][at])
                   << (row + 1 == sample_rows.size() ? "" : ",");
        report << "\n";
    }
}
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
    std::lock_guard<std::recursive_mutex> lock(stats_mutex);
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
    std::lock_guard<std::recursive_mutex> lock(stats_mutex);
    const int64_t value = get_delay_cost_original(component, fallback);
    const auto kind = *static_cast<const uint8_t*>(component);
    ++delay_calls[kind];
    delay_result[kind] = value;
    return value;
}

void hookedBuildScores(void* score, void* a1, float a2, double scale) {
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
    int16_t x = 0;
    int16_t y = 0;
    uint8_t rotation = 0;
    uint64_t id = 0;
    int64_t gates = 0;
    int64_t delay = 0;
};

struct BoardWire {
    int16_t x1 = 0;
    int16_t y1 = 0;
    int16_t x2 = 0;
    int16_t y2 = 0;
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
        std::memcpy(&entry.x, component + 2, sizeof(entry.x));
        std::memcpy(&entry.y, component + 4, sizeof(entry.y));
        std::memcpy(&entry.rotation, component + 6, sizeof(entry.rotation));
        std::memcpy(&entry.id, component + 0x188, sizeof(entry.id));
        out.push_back(entry);
    }
    return out;
}

std::vector<BoardWire> readWires(void* context) {
    std::vector<BoardWire> out;
    auto* base = static_cast<unsigned char*>(context);
    uint64_t wires = 0;
    void* wire_data = nullptr;
    std::memcpy(&wires, base + 0x98, sizeof(wires));
    std::memcpy(&wire_data, base + 0xa0, sizeof(wire_data));
    if (!wire_data || wires > 10000) return out;
    for (uint64_t i = 0; i < wires; ++i) {
        auto* wire = static_cast<unsigned char*>(wire_data) + 8 + i * 0x68;
        BoardWire entry;
        std::memcpy(&entry.x1, wire + 0x18, sizeof(entry.x1));
        std::memcpy(&entry.y1, wire + 0x1a, sizeof(entry.y1));
        std::memcpy(&entry.x2, wire + 0x1c, sizeof(entry.x2));
        std::memcpy(&entry.y2, wire + 0x1e, sizeof(entry.y2));
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
        const char* text = level_name.c_str();
        const size_t length = std::strlen(text);
        tc::TCNimString name{};
        mod.game.raw_new_string(&name, static_cast<int64_t>(length));
        name.length = length;
        std::memcpy(static_cast<unsigned char*>(name.data) + 8, text, length);
        static_cast<unsigned char*>(name.data)[8 + length] = 0;
        load_level(model, &name);
        // Request the same asynchronous compilation consumed by the board UI.
        // load_level alone never refreshes its cached statistics.
        tc::TCNimString progress{};
        auto request=reinterpret_cast<void(*)(void*,const tc::TCNimString*,uint8_t)>(
            host->resolve_symbol(host->context,
                "preorder__modelZsimulationZcompile95thread_u3523"));
        request(model,&progress,0);
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
        if (sampling && have_sim) {
            /* One cycle per request, then sample: the per-cycle view the
               gate-delay evidence needs. */
            tc::simulation::setSlice(sim_api, 1);
            target_cycle = cycle_before + 1;
            mod.simulation.run(model, target_cycle);
            stage_time = current_time;
            stage = 3;
            return;
        }
        mod.simulation.run(model, 8);
        stage_time = current_time;
        stage = 2;
        return;
    }
    if (stage == 3) {
        if (mod.simulation.cycle() < target_cycle) return;
        sampleRow();
        if (static_cast<int64_t>(sample_rows.size()) >= sample_cycles ||
            current_time > stage_time + 30.0) {
            cycle_after = mod.simulation.cycle();
            stage_time = current_time;      /* report after the usual settle */
            stage = 2;
            return;
        }
        target_cycle = mod.simulation.cycle() + 1;
        mod.simulation.run(model, target_cycle);
        return;
    }
    if (current_time < stage_time + 3.0) return;
    done = true;
    cycle_after = mod.simulation.cycle();

    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    std::lock_guard<std::recursive_mutex> lock(stats_mutex);
    std::ostringstream report;
    auto finish = [&](const std::string& text) {
        log(text);
        std::ofstream(folder / "result.txt") << text;
    };

    tc::TCPrototype prototype{};
    const bool have_prototype =
        customComponentId && mod.game.getCustomPrototype(customComponentId, prototype);
    const int64_t prototype_gates =
        have_prototype ? static_cast<int64_t>(tc::prototypeGateCost(prototype)) : -1;
    const int64_t prototype_delay =
        have_prototype ? static_cast<int64_t>(tc::prototypeDelay(prototype)) : -1;
    if (have_prototype) mod.components.releasePrototype(prototype);

    const auto board = readBoard(model);
    const auto wires = readWires(model);
    // Compile the actual board through the game's synchronous wrapper. The
    // direct load_level path does not refresh the UI score, so that stale
    // display must never be used as the assertion for this test.
    alignas(16) unsigned char compiled[0xc0]{};
    auto preorder = reinterpret_cast<void(*)(const void*,const void*,void*)>(
        host->resolve_symbol(host->context,
            "preorder__modelZsimulationZpreorder_u31266"));
    preorder(static_cast<char*>(model)+0x78,static_cast<char*>(model)+0x98,compiled);
    int64_t compiled_delay=0;
    std::memcpy(&compiled_delay,compiled+0x20,8);
    report << "compiled delay=" << compiled_delay << "\n";
    auto gateCost=reinterpret_cast<uint64_t(*)(const void*,uint8_t)>(
        host->resolve_symbol(host->context,"get_gate_cost__modelZscores_u2560"));
    report << "compiled gates=" << gateCost(static_cast<char*>(model)+0x78,0) << "\n";
    report << "mode=" << mode << "\n";
    report << "level=" << level_name << "\n";
    report << "prototype gates=" << prototype_gates
           << " delay=" << prototype_delay << "\n";
    report << "board components=" << board.size() << "\n";
    for (const auto& entry : board) {
        report << "  kind=0x" << std::hex << entry.kind << std::dec
               << " at=" << entry.x << "," << entry.y
               << " rotation=" << static_cast<unsigned>(entry.rotation);
        if (entry.kind == 0x4e) report << " id=" << entry.id;
        report << "\n";
    }
    report << "board wires=" << wires.size() << "\n";
    for (size_t i = 0; i < wires.size(); ++i)
        report << "  wire[" << i << "]=" << wires[i].x1 << "," << wires[i].y1
               << " -> " << wires[i].x2 << "," << wires[i].y2 << "\n";
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
    appendSamplingReport(report);

    finish(report.str());
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;

    const auto folder = std::filesystem::u8path(h->data_directory_utf8);
    {std::ifstream f(folder/"level.txt");std::string s;if(f>>s) level_name=s;}
    {
        std::ifstream f(folder/"fixtures"/"inner_component.data",std::ios::binary);
        if(f) {
            std::string bytes((std::istreambuf_iterator<char>(f)),{});
            auto result=mod.components.importCircuit("Inner AND",bytes.data(),bytes.size(),"D:/timing/");
            if(!result.ok()) return 12;
        }
    }
    {
        std::ifstream mode_file(folder / "mode.txt");
        std::string text;
        if (mode_file) std::getline(mode_file, text);
        if (!text.empty()) mode = text;
    }
    {
        const char* value = std::getenv("TC_GATE_DELAY_SAMPLE");
        sampling = value && value[0] && value[0] != '0';
        have_sim = tc::simulation::table(h, &sim_api);
        std::ifstream cycles_file(folder / "sample_cycles.txt");
        int64_t cycles = 0;
        if (cycles_file >> cycles && cycles > 0) sample_cycles = cycles;
        std::ifstream slots_file(folder / "sample_slots.txt");
        int64_t slots = 0;
        if (slots_file >> slots && slots > 0) sample_slots = static_cast<uint32_t>(slots);
    }
    // Optional declared design cost: same order as a real Mod, which imports
    // the definition and then rewrites the cached statistics before the final
    // registration.  Without the file the probe registers what the importer
    // produced, which is the previous behaviour.
    {
        std::ifstream declare_file(folder / "declare.txt");
        if (declare_file >> declareGates >> declareDelay) declared = true;
    }

    // "builtin" runs the same level geometry with the game's own AND gate and
    // no imported component, as a reference for what the score should read.
    // "observe" also imports nothing: the board's custom component comes from
    // another Mod that is enabled in the same session.
    if (mode != "builtin" && mode != "observe") {
        std::ifstream file(folder / "fixtures" / "and2_component.data",
                           std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)), {});
        if (bytes.empty()) return 3;
        const auto directory = (folder / "fixtures").generic_u8string() + "/";
        auto result = mod.components.importCircuit(
            "AND2 Test", bytes.data(), bytes.size(), directory.c_str());
        if (!result.ok() || !result.custom_id) return 4;
        customComponentId = result.custom_id;
        log("cost-probe: imported custom id=0x" + std::to_string(customComponentId));
        imported = true;

        tc::TCPrototype prototype{};
        if (!mod.game.getCustomPrototype(customComponentId, prototype)) return 5;
        if (declared) {
            mod.game.setPrototypeGateCost(prototype, declareGates);
            mod.game.setPrototypeDelay(prototype, declareDelay);
        }
        const uint64_t gate_cost = tc::prototypeGateCost(prototype);
        const uint64_t delay_cost = tc::prototypeDelay(prototype);
        // Register explicitly through the same SDK path used by native mods.
        mod.game.setCustomPrototype(customComponentId, prototype);
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
