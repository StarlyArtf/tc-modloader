// Development probe: how close can two components be placed?
//
// The player's report: "这些元件在垂直方向上 footprint 好像有点大了，比外观要大
// 一点，导致现在这些元件相互之间不能贴在一起，而是中间有间隔."  The declared
// footprint is a rectangle the game reserves, while the face the Mod draws is
// 4.92 x 2.93 cells, so this probe measures the *effective* placement pitch
// instead of trusting either number:
//
//   * the board's own placement entry (TC_COMMAND_BOARD_PLACE_COMPONENT through
//     the loader's command bus, the same call the game's menu uses) is asked to
//     put a reference part at (col, 0) and a second one at (col, dy);
//   * every trial gets its own column, so an accepted placement cannot decide
//     the next trial's answer;
//   * a stock part is measured the same way when one can be found by name in
//     the game's own custom prototype list, which is what "compare with the
//     game's own parts" needs.
//
// Read-only in the sense that it only places through the game's own command
// path (the same thing a player's drag does); the report goes to the plugin data
// directory as `pitch.txt` and to the log.

#include "../sdk/tc_mod.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const TCHost* host;
tc::TCGameModel game;
TCCommandApiV2 commands{};

bool started = false, done = false;
double first_frame = -1.0;

/* One measurement: a reference part at (x0, y0) and then the smallest offset at
   which a second part of the same type is accepted.  A refused attempt leaves
   the board untouched, so the search walks the offsets upwards and stops at the
   first answer - that keeps the board clear enough for every trial to have a
   valid reference. */
struct Measure {
    uint64_t id = 0;        /* custom prototype id, 0 for a built-in kind */
    uint32_t kind = 0;      /* built-in kind byte when id == 0 */
    std::string label;
    int axis = 0;           /* 0 = try y + offset, 1 = try x + offset */
    int x0 = 0, y0 = 0;
    int offset = 1;
    bool placed = false;
    bool waiting = false;
    int answer = 0;
    uint64_t reference = 0, target = 0;
};

std::vector<Measure> measures;
size_t measureIndex = 0;
int phase = 0;
std::string report;
int submitStatus = 0;

void log(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

std::string hex(uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(value));
    return buffer;
}

void listCustomPrototypes() {
    const uint64_t count = game.customPrototypeCount();
    report += "# custom prototypes the game knows (" + std::to_string(count) + ")\n";
    for (uint64_t index = 0; index < count && index < 256; ++index) {
        const uint64_t id = game.customPrototypeIdAt(index);
        const char* name = game.customPrototypeName(id);
        report += "prototype " + hex(id) + " \"" + (name ? name : "") + "\"\n";
    }
}

/* A custom instance is placed with the game's custom kind byte (0x4e), exactly
   like the Mod's own palette did; a built-in passes its own kind and a zero id. */
bool submit(uint64_t id, uint32_t kind, int x, int y, uint64_t* request, int* status) {
    TCGameHandle board{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) != TC_HANDLE_OK)
    {
        if (status) *status = TC_COMMAND_ERR_STALE;
        return false;
    }
    TCCommandV2 command{};
    command.size = sizeof(command);
    command.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
    command.subject = board;
    command.custom_prototype_id = id;
    command.kind = id ? 0x4eu : kind;
    command.rotation = 0;
    command.x = x;
    command.y = y;
    const int result = commands.submit(commands.context, &command, request);
    if (status) *status = result;
    return result == TC_COMMAND_OK;
}

/* Reads the request's state; returns true once it is terminal. */
bool status(uint64_t request, int* result) {
    TCCommandStatusV1 state{};
    state.size = sizeof(state);
    if (commands.get_status(commands.context, request, &state, sizeof(state)) !=
        TC_COMMAND_OK)
        return true;
    if (state.state == TC_COMMAND_STATE_QUEUED || state.state == TC_COMMAND_STATE_RUNNING)
        return false;
    *result = state.result;
    return true;
}

void buildMeasures() {
    /* Two of this Mod's own types: a two-pin side (Subtract) and the tallest
       catalogue face (Compare, four outputs), plus the M2 constant that the
       drawer cases use. */
    const struct {
        uint64_t id;
        const char* label;
    } own[] = {
        {UINT64_C(0x4633325355425f31), "float-ops subtract"},
        {UINT64_C(0x463332434f4e5331), "float-ops constant"},
        {UINT64_C(0x463332464d415f31), "float-ops fma"},
        {UINT64_C(0x463332434d505f31), "float-ops compare"},
        {UINT64_C(0x46333253504c5f31), "float-ops split bits"},
        {UINT64_C(0x4633324d4b425f31), "float-ops make bits"},
    };
    /* Columns start clear of the fixture board the sandbox opens (its parts sit
       around x = -12..8), so a trial's own reference placement is not the one
       that gets refused. */
    /* Every measurement gets its own band of rows: a refused attempt leaves no
       part behind, but the accepted one does, and the next measurement must not
       find it in the way.  Twenty rows of separation is more than the widest
       answer (ten cells) plus the bodies. */
    int band = 0;
    for (const auto& type : own) {
        if (!game.hasCustomPrototype(type.id)) continue;
        for (int axis = 0; axis < 2; ++axis) {
            Measure measure{};
            measure.id = type.id;
            measure.label = type.label;
            measure.axis = axis;
            measure.x0 = 40;
            measure.y0 = 26 + band * 20;
            measures.push_back(measure);
            ++band;
        }
    }
}

void finish() {
    done = true;
    std::ofstream(std::string(host->data_directory_utf8) + "/pitch.txt") << report;
    std::istringstream lines(report);
    std::string line;
    int emitted = 0;
    while (std::getline(lines, line) && emitted < 120) {
        log("pitch-probe: " + line);
        ++emitted;
    }
    log("pitch-probe: wrote pitch.txt");
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (done) return;
    if (!commands.submit) return;
    TCGameHandle board{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) != TC_HANDLE_OK)
        return;
    if (first_frame < 0.0) first_frame = value->time_seconds;
    if (value->time_seconds - first_frame < 3.0) return;
    if (!started) {
        started = true;
        listCustomPrototypes();
        buildMeasures();
        report += "# smallest accepted offset from one part to the next\n";
        log("pitch-probe: " + std::to_string(measures.size()) + " measurement(s)");
        if (measures.empty()) { finish(); return; }
    }
    Measure& measure = measures[measureIndex];
    if (phase == 0) {
        if (!measure.placed) {
            measure.reference = 0;
            if (!submit(measure.id, measure.kind, measure.x0, measure.y0, &measure.reference,
                        &submitStatus)) {
                report += "measure " + measure.label + " " +
                          (measure.axis ? "dx" : "dy") +
                          ": the reference could not be submitted (submit=" +
                          std::to_string(submitStatus) + ")\n";
                ++measureIndex;
                if (measureIndex >= measures.size()) finish();
                return;
            }
            measure.placed = true;
            measure.waiting = true;
            return;
        }
        if (measure.waiting) {
            int result = 0;
            if (!status(measure.reference, &result)) return;
            measure.waiting = false;
            if (result != TC_COMMAND_OK) {
                report += "measure " + measure.label + " " +
                          (measure.axis ? "dx" : "dy") + ": no reference at (" +
                          std::to_string(measure.x0) + "," + std::to_string(measure.y0) +
                          "), result=" + std::to_string(result) + "\n";
                ++measureIndex;
                if (measureIndex >= measures.size()) finish();
                return;
            }
        }
        const int x = measure.x0 + (measure.axis ? measure.offset : 0);
        const int y = measure.y0 + (measure.axis ? 0 : measure.offset);
        measure.target = 0;
        if (!submit(measure.id, measure.kind, x, y, &measure.target, &submitStatus)) {
            report += "measure " + measure.label + " " + (measure.axis ? "dx" : "dy") +
                      " offset=" + std::to_string(measure.offset) +
                      ": the attempt could not be submitted (submit=" +
                      std::to_string(submitStatus) + ")\n";
            measure.answer = -1;
            ++measureIndex;
            if (measureIndex >= measures.size()) finish();
            return;
        }
        measure.waiting = true;
        phase = 1;
        return;
    }
    int result = 0;
    if (!status(measure.target, &result)) return;
    if (result == TC_COMMAND_OK) {
        measure.answer = measure.offset;
        report += "measure " + measure.label + " " + (measure.axis ? "horizontal" : "vertical") +
                  " pitch=" + std::to_string(measure.answer) + " cell(s)  ADJACENT-OK\n";
        phase = 0;
        ++measureIndex;
        if (measureIndex >= measures.size()) finish();
        return;
    }
    ++measure.offset;
    if (measure.offset > 10) {
        measure.answer = -1;
        report += "measure " + measure.label + " " + (measure.axis ? "horizontal" : "vertical") +
                  " pitch>10 cells (no offset up to ten was accepted)\n";
        phase = 0;
        ++measureIndex;
        if (measureIndex >= measures.size()) finish();
        return;
    }
    phase = 0;
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!game.load(h) || !game.valid()) return 2;
    if (tc::commandService(h, &commands) != TC_SERVICE_OK || !commands.submit) {
        log("pitch-probe: this loader has no tc.commands service");
        return 3;
    }
    plugin->on_frame = frame;
    log("pitch-probe: measuring the placement pitch");
    return 0;
}
