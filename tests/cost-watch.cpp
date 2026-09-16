// Diagnostic observer mod: logs what the pinned game actually computes for
// gate/delay scores while the player uses the real UI.  It never mutates game
// state; it only trampolines the score functions and reports aggregated
// numbers through the loader log.
//
// Install: copy dist\dev.cost-watch.mod into <game>\mods, enable it in the
// loader's Mod manager, restart, then play normally (build a circuit, press
// Run).  Every observation is prefixed with "cost-watch:".

#include "../sdk/tc_mod.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

namespace {

const TCHost* host;
tc::TCMod mod;

using GetCostFn = void* (*)(void* result, const void* component);
using GetDelayCostFn = int64_t (*)(const void* component, int64_t fallback);
using GetGateCostFn = uint64_t (*)(const void* components, uint8_t recursive);
using BuildScoresFn = void (*)(void* score, void* a1, float a2, double scale);
using PreorderFn = void (*)(void* model, void* level, void* solution);

GetCostFn get_cost_original;
GetDelayCostFn get_delay_cost_original;
GetGateCostFn get_gate_cost_original;
BuildScoresFn build_scores_original;
PreorderFn preorder_original;

struct KindStat {
    uint64_t calls = 0;
    int64_t gates = 0;
    int64_t delay = 0;
    uint64_t id = 0;
};

std::map<int, KindStat> cost_stats;
std::map<int, std::pair<uint64_t, int64_t>> delay_stats;
uint64_t gate_cost_calls = 0;
int64_t gate_cost_last = -1;
uint64_t score_calls = 0;
int64_t score_gates = -1;
int64_t score_delay = -1;
int score_flag = -1;
std::atomic<uint64_t> preorder_calls{0};
bool dirty = true;
double last_report = -1000.0;
double last_prototype_check = -1000.0;
uint64_t reported_calls = 0;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

void* hookedGetCost(void* result, const void* component) {
    void* out = get_cost_original(result, component);
    const auto kind = *static_cast<const uint8_t*>(component);
    KindStat& stat = cost_stats[kind];
    ++stat.calls;
    int64_t gates = 0;
    int64_t delay = 0;
    std::memcpy(&gates, static_cast<const unsigned char*>(result), 8);
    std::memcpy(&delay, static_cast<const unsigned char*>(result) + 8, 8);
    stat.gates = gates;
    stat.delay = delay;
    if (kind == 0x4e) {
        uint64_t id = 0;
        std::memcpy(&id, static_cast<const unsigned char*>(component) + 0x188,
                    sizeof(id));
        stat.id = id;
    }
    dirty = true;
    return out;
}

int64_t hookedGetDelayCost(const void* component, int64_t fallback) {
    const int64_t value = get_delay_cost_original(component, fallback);
    const auto kind = *static_cast<const uint8_t*>(component);
    auto& entry = delay_stats[kind];
    ++entry.first;
    entry.second = value;
    dirty = true;
    return value;
}

uint64_t hookedGetGateCost(const void* components, uint8_t recursive) {
    const uint64_t value = get_gate_cost_original(components, recursive);
    ++gate_cost_calls;
    gate_cost_last = static_cast<int64_t>(value);
    dirty = true;
    return value;
}

void hookedBuildScores(void* score, void* a1, float a2, double scale) {
    build_scores_original(score, a1, a2, scale);
    if (!score) return;
    std::memcpy(&score_gates, static_cast<unsigned char*>(score), 8);
    std::memcpy(&score_delay, static_cast<unsigned char*>(score) + 8, 8);
    score_flag = static_cast<unsigned char*>(score)[0x20];
    ++score_calls;
    dirty = true;
}

void hookedPreorder(void* model, void* level, void* solution) {
    preorder_calls.fetch_add(1, std::memory_order_relaxed);
    dirty = true;
    preorder_original(model, level, solution);
}

void report(double time) {
    if (time - last_report < 1.5) return;
    // Log whenever something changed, plus a slow heartbeat so the log shows
    // that the observation mod is alive during a manual session.
    const bool heartbeat = time - last_report >= 15.0;
    if (!dirty && !heartbeat) return;
    std::map<int, KindStat>::const_iterator custom = cost_stats.end();
    for (auto it = cost_stats.begin(); it != cost_stats.end(); ++it) {
        if (it->first == 0x4e) custom = it;
    }
    std::ostringstream line;
    line << "cost-watch t=" << static_cast<int>(time)
         << " displayed gates=" << score_gates << " delay=" << score_delay
         << " delay_flag=" << score_flag << " score_calls=" << score_calls
         << " total_gates=" << gate_cost_last << " (calls=" << gate_cost_calls
         << ") preorder_calls=" << preorder_calls;
    for (auto it = cost_stats.begin(); it != cost_stats.end(); ++it) {
        line << " | cost[kind=0x" << std::hex << it->first << std::dec
             << "] calls=" << it->second.calls << " gates=" << it->second.gates
             << " delay=" << it->second.delay;
        if (it->first == 0x4e) line << " id=" << it->second.id;
    }
    for (auto it = delay_stats.begin(); it != delay_stats.end(); ++it) {
        line << " | delay_cost[kind=0x" << std::hex << it->first << std::dec
             << "] calls=" << it->second.first << " last=" << it->second.second;
    }
    // Re-read the registered prototype occasionally: this is the value the
    // game's own cost function reads for custom instances.
    if (cost_stats.count(0x4e) && time - last_prototype_check >= 15.0 &&
        mod.components.valid() &&
        mod.components.readiness() == tc::TCComponentStatus::Ok) {
        tc::TCPrototype prototype{};
        if (mod.game.getCustomPrototype(cost_stats[0x4e].id, prototype)) {
            line << " | prototype[id=" << cost_stats[0x4e].id
                 << "] gates=" << tc::prototypeGateCost(prototype)
                 << " delay=" << tc::prototypeDelay(prototype);
            mod.components.releasePrototype(prototype);
        }
        last_prototype_check = time;
    }
    (void)custom;
    log(line.str());
    last_report = time;
    reported_calls = score_calls;
    dirty = false;
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    report(value->time_seconds);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid()) return 2;

    auto* get_cost_target =
        h->resolve_symbol(h->context, "get_cost__modelZscores_u2321");
    auto* get_delay_target =
        h->resolve_symbol(h->context, "get_delay_cost__modelZscores_u2316");
    auto* get_gate_target =
        h->resolve_symbol(h->context, "get_gate_cost__modelZscores_u2560");
    auto* build_scores_target = h->resolve_symbol(
        h->context, "build_scores__presenterZboard95uiZmenu95bar_u886");
    auto* preorder_target = h->resolve_symbol(
        h->context, "preorder__modelZsimulationZpreorder_u8749");
    if (!get_cost_target || !get_delay_target || !get_gate_target ||
        !build_scores_target || !preorder_target) {
        log("cost-watch: symbol lookup failed");
        return 3;
    }
    if (h->create_hook(h->context, get_cost_target,
                       reinterpret_cast<void*>(hookedGetCost),
                       reinterpret_cast<void**>(&get_cost_original)) != 0 ||
        h->create_hook(h->context, get_delay_target,
                       reinterpret_cast<void*>(hookedGetDelayCost),
                       reinterpret_cast<void**>(&get_delay_cost_original)) != 0 ||
        h->create_hook(h->context, get_gate_target,
                       reinterpret_cast<void*>(hookedGetGateCost),
                       reinterpret_cast<void**>(&get_gate_cost_original)) != 0 ||
        h->create_hook(h->context, build_scores_target,
                       reinterpret_cast<void*>(hookedBuildScores),
                       reinterpret_cast<void**>(&build_scores_original)) != 0 ||
        h->create_hook(h->context, preorder_target,
                       reinterpret_cast<void*>(hookedPreorder),
                       reinterpret_cast<void**>(&preorder_original)) != 0) {
        log("cost-watch: hook install failed");
        return 4;
    }
    plugin->on_frame = frame;
    log("cost-watch: observing get_cost, get_delay_cost, get_gate_cost, "
        "build_scores, preorder");
    return 0;
}
