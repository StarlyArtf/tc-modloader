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
#include <mutex>
#include <sstream>
#include <string>

namespace {

const TCHost* host;
std::recursive_mutex stats_mutex;
tc::TCMod mod;

using GetCostFn = void* (*)(void* result, const void* component);
using GetDelayCostFn = int64_t (*)(const void* component, int64_t fallback);
using GetGateCostFn = uint64_t (*)(const void* components, uint8_t skip_custom);
using BuildScoresFn = void (*)(void* score, void* a1, float a2, double scale);
// Eight machine-level arguments, including the result buffer in argument 8.
// A three-argument trampoline loses r9 and the four stack arguments.
using PreorderFn = void (*)(void*, void*, void*, void*, void*, void*,
                           uint64_t, void*);
using UpdateWireFn = bool (*)(void* context, void* a1, void* a2, uint32_t point,
                              uint8_t fifth);

GetCostFn get_cost_original;
GetDelayCostFn get_delay_cost_original;
GetGateCostFn get_gate_cost_original;
BuildScoresFn build_scores_original;
PreorderFn preorder_original;
UpdateWireFn update_wire_original;

void* board_context = nullptr;
std::string last_board;

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
    std::lock_guard<std::recursive_mutex> lock(stats_mutex);
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
    std::lock_guard<std::recursive_mutex> lock(stats_mutex);
    const int64_t value = get_delay_cost_original(component, fallback);
    const auto kind = *static_cast<const uint8_t*>(component);
    auto& entry = delay_stats[kind];
    ++entry.first;
    entry.second = value;
    dirty = true;
    return value;
}

uint64_t hookedGetGateCost(const void* components, uint8_t skip_custom) {
    std::lock_guard<std::recursive_mutex> lock(stats_mutex);
    const uint64_t value = get_gate_cost_original(components, skip_custom);
    ++gate_cost_calls;
    gate_cost_last = static_cast<int64_t>(value);
    dirty = true;
    return value;
}

void hookedBuildScores(void* score, void* a1, float a2, double scale) {
    std::lock_guard<std::recursive_mutex> lock(stats_mutex);
    build_scores_original(score, a1, a2, scale);
    if (!score) return;
    int64_t gates = 0;
    int64_t delay = 0;
    std::memcpy(&gates, static_cast<unsigned char*>(score), 8);
    std::memcpy(&delay, static_cast<unsigned char*>(score) + 8, 8);
    const int flag = static_cast<unsigned char*>(score)[0x20];
    ++score_calls;
    // build_scores runs every frame; only a real value change is interesting.
    if (gates != score_gates || delay != score_delay || flag != score_flag) {
        score_gates = gates;
        score_delay = delay;
        score_flag = flag;
        dirty = true;
    }
}

void hookedPreorder(void* settings, void* components, void* wires, void* memory,
                   void* a5, void* a6, uint64_t a7, void* result) {
    preorder_calls.fetch_add(1, std::memory_order_relaxed);
    { std::lock_guard<std::recursive_mutex> lock(stats_mutex); dirty = true; }
    preorder_original(settings, components, wires, memory, a5, a6, a7, result);
}

/* Kept for the record, not installed: see the target list in tc_mod_load. */
[[maybe_unused]] bool hookedUpdateWire(void* context, void* a1, void* a2, uint32_t point,
                                       uint8_t fifth) {
    board_context = context;
    return update_wire_original(context, a1, a2, point, fifth);
}

// Reads the live board through the same layout the placement/persistence
// probes use and reports what the game's cost function says per component.
std::string describeBoard() {
    if (!board_context) return "board=<no context yet>";
    auto* base = static_cast<unsigned char*>(board_context);
    uint64_t components = 0;
    uint64_t wires = 0;
    void* payload = nullptr;
    std::memcpy(&components, base + 0x78, sizeof(components));
    std::memcpy(&wires, base + 0x98, sizeof(wires));
    std::memcpy(&payload, base + 0x80, sizeof(payload));
    if (!payload || components > 4096) return "board=<unreadable>";
    std::ostringstream text;
    text << "board components=" << components << " wires=" << wires << " [";
    for (uint64_t i = 0; i < components; ++i) {
        auto* component =
            static_cast<unsigned char*>(payload) + 8 + i * 0x238;
        const auto kind = *reinterpret_cast<const uint8_t*>(component);
        text << (i ? ", " : "") << "0x" << std::hex << static_cast<int>(kind)
             << std::dec;
        if (kind == 0x4e) {
            uint64_t id = 0;
            std::memcpy(&id, component + 0x188, sizeof(id));
            text << ":id=" << id;
        }
        if (get_cost_original) {
            uint64_t pair[2] = {0, 0};
            get_cost_original(pair, component);
            text << "(g=" << pair[0] << ",d=" << pair[1] << ")";
        }
    }
    text << "]";
    return text.str();
}

void report(double time) {
    std::lock_guard<std::recursive_mutex> lock(stats_mutex);
    if (time - last_report < 1.5) return;
    const std::string board = describeBoard();
    const bool board_changed = board != last_board;
    if (board_changed) {
        last_board = board;
        dirty = true;
    }
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
    if (board_changed) line << " | " << board;
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
    auto* update_target = h->resolve_symbol(
        h->context,
        "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    if (!get_cost_target || !get_delay_target || !get_gate_target ||
        !build_scores_target || !preorder_target || !update_target) {
        log("cost-watch: symbol lookup failed");
        return 3;
    }
    // An observer must tolerate targets the core loader already owns: it hooks
    // preorder, custom_prototypes_set and the gate-cost entry points itself.
    // Install what is free, name what is not, and keep the rest working.
    //
    // `handle_update_wire` is deliberately *not* in this list even though it is
    // what fills the report's board line: the hook is exclusive, and a player
    // mod that needs the same target (local.wire-palette hooks the wire update)
    // would fail to load whenever this observer got there first.  A diagnostic
    // probe does not get to break a real mod, so the board line now stays
    // "no context yet" - the gate/delay numbers, which are the point, do not
    // depend on it.
    struct Target { const char* name; void* target; void* detour; void** original; };
    const Target targets[] = {
        {"get_cost", get_cost_target,
         reinterpret_cast<void*>(&hookedGetCost),
         reinterpret_cast<void**>(&get_cost_original)},
        {"get_delay_cost", get_delay_target,
         reinterpret_cast<void*>(&hookedGetDelayCost),
         reinterpret_cast<void**>(&get_delay_cost_original)},
        {"get_gate_cost", get_gate_target,
         reinterpret_cast<void*>(&hookedGetGateCost),
         reinterpret_cast<void**>(&get_gate_cost_original)},
        {"build_scores", build_scores_target,
         reinterpret_cast<void*>(&hookedBuildScores),
         reinterpret_cast<void**>(&build_scores_original)},
        {"preorder", preorder_target,
         reinterpret_cast<void*>(&hookedPreorder),
         reinterpret_cast<void**>(&preorder_original)},
    };
    int installed = 0;
    for (const auto& entry : targets) {
        if (h->create_hook(h->context, entry.target, entry.detour,
                           entry.original) == 0) {
            ++installed;
        } else {
            log(std::string("cost-watch: ") + entry.name +
                " hook unavailable (owned by the loader or rejected)");
        }
    }
    if (!installed) {
        log("cost-watch: no observation hook could be installed");
        return 4;
    }
    plugin->on_frame = frame;
    log("cost-watch: observing " + std::to_string(installed) + "/" +
        std::to_string(sizeof(targets) / sizeof(targets[0])) +
        " targets (get_cost, get_delay_cost, get_gate_cost, build_scores, "
        "preorder)");
    return 0;
}
