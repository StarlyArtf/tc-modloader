/* Development-only probe: does the sandbox board's state keep changing when the
   gate-delay mode is on?

   STATUS (2026-09-24): this probe does not reach a board yet.  It drives the
   simulation through TC_SERVICE_SIMULATION (which needs no board pointer) and
   samples by offset, but in the playtest that was tried the session never left
   the menu: `cycle=-1`, `TC_SERVICE_BOARD.get_current=-1`, and neither the IO
   update hook nor the level.load event ever fired, while the component-cost
   probe does reach the sandbox in the same kind of session.  The sampling that
   the M2 evidence actually uses therefore lives in tests/component-cost-probe.cpp
   behind TC_GATE_DELAY_SAMPLE=1.  Kept because the offset sweep and the
   "step one cycle, then sample" loop are what a working standalone probe wants.

   A cyclic board cannot reach the code generator (plan section 13.3), so the
   board this probe loads is the *cut* one and the loader re-closes the missing
   connection (TC_GATE_DELAY_CLOSE).  With the mode on, the closed loop advances
   one stage per unit, so an odd ring keeps flipping; with the mode off the same
   board is a plain acyclic circuit driven by constants and never moves.  That
   difference is the assertion: the probe sweeps the low part of the state buffer
   cycle by cycle and reports which slots changed.

   Reads go through TC_SERVICE_SIMULATION (a Mod never resolves the game's own
   state reader), one multi-channel sample per cycle. */

#include "../sdk/tc_mod.h"
#include "../sdk/tc_hook.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const TCHost* host = nullptr;
tc::TCMod mod;
void* model = nullptr;
bool started = false;
bool done = false;
double start_time = 0;
double current_time = 0;
double stage_time = 0;
int stage = 0;
std::string level_name = "sandbox";
int64_t cycles_wanted = 12;
uint32_t slots = 512;
int64_t target = 1;
std::vector<std::vector<unsigned char>> rows;
std::vector<int64_t> row_cycles;
tc::simulation::Api sim_api{};
bool have_sim = false;
void (*load_level)(void*, const tc::TCNimString*) = nullptr;
/* The board model arrives through the board-IO update hook: that is where the
   game hands the presenter thread the model on every input event (the cost probe
   reads it the same way). */
bool (*update_original)(void*, void*, void*, uint32_t, uint8_t) = nullptr;
TCBoardApiV1 board_api{};
bool have_board = false;

bool hookedUpdate(void* m, void* context, void* input, uint32_t point, uint8_t fifth) {
    model = m;
    return update_original ? update_original(m, context, input, point, fifth) : false;
}

/* The simulated board is the object the loader's own board services hand out, so
   a probe that never sees an input event can still find it.  `resolve` is what
   turns the handle into the pointer the game's own level loader takes. */
bool findModelFromBoardService() {
    if (!have_board) return false;
    TCGameHandle handle{};
    if (board_api.get_current(board_api.context, &handle) != TC_SERVICE_OK) return false;
    if (!handle.token) return false;
    const void* pointer = nullptr;
    if (board_api.resolve(board_api.context, &handle, &pointer) != TC_SERVICE_OK) return false;
    if (!pointer) return false;
    model = const_cast<void*>(pointer);
    return true;
}

void log(const std::string& text) {
    if (host && host->log) host->log(host->context, text.c_str());
}

/* The board model arrives with the loader's level.load event: the same path the
   M0 probes used to reach a board before any input happened. */
int32_t onLevelLoad(TCHookCall* call) {
    auto* args = tc::hook::levelLoadArgs(call);
    if (args && args->board_model) {
        model = args->board_model;
        log("gate-delay-slots: level.load handed over the board model");
    }
    return 0;
}

/* One sample of every slot in the low window.  A single multi-channel sample
   keeps the whole row on one engine frame. */
bool readRow(std::vector<unsigned char>& out) {
    std::vector<TCSimChannelV1> channels(slots);
    std::vector<uint64_t> values(slots, 0);
    for (uint32_t at = 0; at < slots; ++at) {
        channels[at].size = sizeof(TCSimChannelV1);
        channels[at].version = TCSIM_CHANNEL_VERSION_1;
        channels[at].byte_offset = at;
        channels[at].bits = 8;
    }
    int64_t cycle = -1;
    uint32_t stable = 0;
    if (tc::simulation::sample(sim_api, channels.data(), slots, values.data(), &cycle, &stable) !=
        TC_SIMULATION_OK)
        return false;
    out.assign(slots, 0);
    for (uint32_t at = 0; at < slots; ++at) out[at] = static_cast<unsigned char>(values[at] & 0xff);
    return true;
}

void writeReport(const std::string& text) {
    log(text);
    if (!host || !host->data_directory_utf8) return;
    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    std::ofstream(folder / "result.txt") << text;
}

void report(const std::string& note) {
    std::ostringstream out;
    out << "level=" << level_name << "\n";
    out << "cycles=" << rows.size() << " swept_slots=" << slots << "\n";
    if (!note.empty()) out << note << "\n";
    if (rows.empty()) {
        writeReport(out.str());
        done = true;
        return;
    }
    /* Which slots moved, and how often. */
    std::vector<uint32_t> changed;
    std::vector<uint32_t> moves;
    for (uint32_t at = 0; at < slots; ++at) {
        uint32_t moved = 0;
        for (size_t row = 1; row < rows.size(); ++row)
            if (rows[row][at] != rows[row - 1][at]) ++moved;
        if (moved) {
            changed.push_back(at);
            moves.push_back(moved);
        }
    }
    out << "changed slots=" << changed.size() << "\n";
    for (size_t i = 0; i < changed.size() && i < 24; ++i) {
        const uint32_t at = changed[i];
        out << "  slot " << at << " moves=" << moves[i] << " seq=";
        for (size_t row = 0; row < rows.size(); ++row)
            out << static_cast<int>(rows[row][at]) << (row + 1 == rows.size() ? "" : ",");
        out << "\n";
    }
    if (changed.size() > 24) out << "  (" << (changed.size() - 24) << " more)\n";
    writeReport(out.str());
    done = true;
}

void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    current_time = value->time_seconds - start_time;
    if (done) return;
    /* The board model is only a hint now: the probe drives the simulation through
       the service and never calls the game's own level loader. */
    if (!model) {
        static double lastDiagnostic = 0;
        if (current_time > lastDiagnostic + 10.0) {
            lastDiagnostic = current_time;
            int status = -1;
            TCGameHandle handle{};
            if (have_board) status = board_api.get_current(board_api.context, &handle);
            log("gate-delay-slots: no board model (service=" + std::to_string(have_board) +
                " get_current=" + std::to_string(status) + " cycle=" +
                std::to_string(mod.simulation.cycle()) + ")");
        }
        findModelFromBoardService();
    }
    if (current_time < 5.0) return;
    if (stage == 0) {
        if (!have_sim) {
            report("the loader has no TC_SERVICE_SIMULATION");
            return;
        }
        tc::simulation::setSlice(sim_api, 1);          /* one cycle per request */
        stage_time = current_time;
        stage = 1;
        return;
    }
    if (stage == 1) {
        /* The board is the profile's own schematic for the sandbox level, so the
           game has already loaded and compiled it: all this probe has to do is
           step the simulation and look at the state. */
        const int64_t cycle = mod.simulation.cycle();
        if (cycle < 0) {
            if (current_time > stage_time + 2.0) {
                stage_time = current_time;
                log("gate-delay-slots: waiting for the simulation (cycle=" + std::to_string(cycle) +
                    " model=" + std::to_string(model != nullptr) + ")");
            }
            return;
        }
        target = cycle + 1;
        tc::simulation::runTo(sim_api, target);
        stage = 2;
        return;
    }
    if (mod.simulation.cycle() < target) return;
    std::vector<unsigned char> row;
    if (!readRow(row)) {
        report("the state sample failed");
        return;
    }
    rows.push_back(std::move(row));
    row_cycles.push_back(target);
    if (static_cast<int64_t>(rows.size()) >= cycles_wanted) {
        report("");
        return;
    }
    ++target;
    tc::simulation::runTo(sim_api, target);
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid()) return 2;
    load_level = reinterpret_cast<void (*)(void*, const tc::TCNimString*)>(
        h->resolve_symbol(h->context, "load_level__modelZutilities_u7740"));
    if (!load_level) return 3;
    if (void* update_target = h->resolve_symbol(
            h->context, "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5")) {
        if (h->create_hook(h->context, update_target, reinterpret_cast<void*>(&hookedUpdate),
                           reinterpret_cast<void**>(&update_original)) != 0)
            return 4;
    } else {
        return 5;
    }
    if (tc::hook::addLevelLoad(h, 0, &onLevelLoad, nullptr) != TC_HOOK_OK) return 6;
    have_sim = tc::simulation::table(h, &sim_api);
    {
        TCBoardApiV1 queried{};
        if (h->query_service &&
            h->query_service(h->context, TC_SERVICE_BOARD, TC_BOARD_API_VERSION_1, &queried,
                             sizeof(queried)) == TC_SERVICE_OK &&
            queried.size >= sizeof(queried) && queried.get_current && queried.resolve) {
            board_api = queried;
            have_board = true;
        }
    }
    const auto folder = std::filesystem::u8path(h->data_directory_utf8);
    {
        std::ifstream file(folder / "level.txt");
        std::string text;
        if (file >> text) level_name = text;
    }
    {
        std::ifstream file(folder / "cycles.txt");
        int64_t value = 0;
        if (file >> value && value > 0) cycles_wanted = value;
    }
    {
        std::ifstream file(folder / "slots.txt");
        int64_t value = 0;
        if (file >> value && value > 0) slots = static_cast<uint32_t>(value);
    }
    plugin->on_frame = frame;
    return 0;
}
