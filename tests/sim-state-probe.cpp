// Route 1 reconnaissance: map circuit pins to simulation state slots.
//
// Runs the stock and_gate level one cycle at a time and snapshots the whole
// simulation state after each cycle.  The level's own test defines the
// expected output sequence (0,0,0,1) for inputs (0,0),(0,1),(1,0),(1,1), so
// byte slots whose value sequence matches a known pattern are the pin storage
// we are looking for.  Read-only: it never writes game state.

#include "../sdk/tc_mod.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const TCHost* host;
tc::TCMod mod;
void* model = nullptr;
using LoadLevel = void (*)(void*, const tc::TCNimString*);
LoadLevel load_level;
using SetSimTest = void (*)(void*, int64_t, uint8_t);
SetSimTest set_sim_test;

bool started = false;
double start_time = 0;
double current_time = 0;
int stage = 0;
double stage_time = 0;
bool done = false;
int64_t target = 1;

unsigned char* state = nullptr;   // simulation_state payload
uint64_t state_len = 0;
void* state_global = nullptr;     // simulation_state sequence global
std::vector<std::vector<unsigned char>> snapshots;
std::vector<int64_t> snapshot_cycles;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

void readState() {
    if (!state) return;
    snapshots.emplace_back(state, state + state_len);
    snapshot_cycles.push_back(mod.simulation.cycle());
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    current_time = value->time_seconds - start_time;
    if (done || !model || current_time < 3.0) return;
    if (stage == 0) {
        const char* text = "and_gate";
        const size_t length = std::strlen(text);
        tc::TCNimString name{};
        mod.game.raw_new_string(&name, static_cast<int64_t>(length));
        name.length = length;
        std::memcpy(static_cast<unsigned char*>(name.data) + 8, text, length);
        static_cast<unsigned char*>(name.data)[8 + length] = 0;
        load_level(model, &name);
        stage = 1;
        stage_time = current_time;
        return;
    }
    if (stage == 1) {
        if (current_time < stage_time + 2.0) return;
        // The state sequence is allocated when the simulator initialises, i.e.
        // only once a board exists, so read it here rather than at load time.
        if (!state) {
            std::memcpy(&state_len, state_global, sizeof(state_len));
            std::memcpy(&state, static_cast<unsigned char*>(state_global) + 8,
                        sizeof(state));
        }
        if (!state || !state_len || state_len > (64u << 20)) return;
        if (set_sim_test) set_sim_test(model, 0, 1);
        mod.simulation.run(model, target);
        stage = 2;
        stage_time = current_time;
        return;
    }
    if (stage == 2) {
        const int64_t cycle = mod.simulation.cycle();
        if (cycle < target) {
            if (current_time > stage_time + 5.0) { stage = 3; }  // give up waiting
            return;
        }
        readState();
        ++target;
        stage_time = current_time;
        if (target > 5) { stage = 3; return; }
        mod.simulation.run(model, target);
        return;
    }
    if (stage != 3) return;
    done = true;

    std::ostringstream report;
    report << "state_len=" << state_len << " snapshots=" << snapshots.size();
    for (int64_t cycle : snapshot_cycles) report << " " << cycle;
    report << "\n";

    if (snapshots.size() >= 4) {
        const auto& first = snapshots.front();
        size_t changed = 0;
        for (uint64_t index = 0; index < state_len; ++index) {
            bool varies = false;
            for (const auto& snap : snapshots) {
                if (snap[index] != first[index]) { varies = true; break; }
            }
            if (!varies) continue;
            ++changed;
            report << "slot " << index << " seq=";
            for (const auto& snap : snapshots) report << static_cast<int>(snap[index]) << ",";
            report << "\n";
        }
        report << "changed_slots=" << changed << "\n";
    }
    const auto folder = std::string(host->data_directory_utf8);
    std::ofstream(folder + "/state-map.txt") << report.str();
    log("sim-state: " + std::to_string(state_len) + " bytes, " +
        std::to_string(snapshots.size()) + " snapshots saved");
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;
    load_level = reinterpret_cast<LoadLevel>(
        h->resolve_symbol(h->context, "load_level__modelZutilities_u7740"));
    set_sim_test = reinterpret_cast<SetSimTest>(h->resolve_symbol(
        h->context, "set_sim_test__modelZutilities_u6840"));
    state_global = h->resolve_symbol(
        h->context, "simulation_state__modelZsimulator95types_u81");
    if (!load_level || !state_global) return 3;
    auto* update = h->resolve_symbol(
        h->context,
        "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    if (!update) return 5;
    using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
    static UpdateWire original = nullptr;
    struct Holder { static bool hook(void* m, void* a, void* b, uint32_t p, uint8_t f) {
        model = m;
        return original ? original(m, a, b, p, f) : false;
    } };
    if (h->create_hook(h->context, update,
                       reinterpret_cast<void*>(&Holder::hook),
                       reinterpret_cast<void**>(&original)) != 0) {
        return 6;
    }
    plugin->on_frame = frame;
    log("sim-state: state bytes=" + std::to_string(state_len));
    return 0;
}
