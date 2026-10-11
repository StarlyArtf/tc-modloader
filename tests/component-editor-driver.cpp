/* Test-only driver for tc.component.editor (docs/HANDOFF-component-editor.md §5).

   It does what a player does and a script cannot: enter a level, put one
   editor-demo component on the board through the command bus, click its body so
   the game opens its own component drawer, read the row rectangles the loader
   logs while it draws them (`TC_EDITOR_TRACE=1`), click one row with real mouse
   messages, and - when asked - type into it with real keyboard messages.

   The rows are the loader's own, so the only way to name them is the loader's
   log; positions are never guessed.  Everything the run did is written to
   `result.txt` in this Mod's data directory and to the loader log, and the
   playtest asserts on both.

   Environment:
     TC_EDITOR_LOG     the loader.log to read the row rectangles from
     TC_EDITOR_ROW     which field to click (default "enabled")
     TC_EDITOR_KEYS    text to type after the click (default: none)
     TC_EDITOR_LEVEL   the level to enter (default "sandbox")
     TC_EDITOR_SKIP_LEVEL=1  keep whatever board the harness already opened
                             instead of loading a level first

   The board is entered the way every board probe here enters it: a hook on the
   game's own board-input function captures the model pointer, then the driver
   calls the game's own `load_level` with "sandbox" and waits for the scene to
   settle before placing.  The first placement also goes through the game's own
   add_component helper as a cross-check, on a cell the command bus will not ask
   for; the placement under test itself goes through the public command bus, and
   a refusal is retried on the next cell (PLACE_COMPONENT answers -6 for a cheap
   reason: the cell is taken). */
#include "../sdk/tc_mod.h"
#include "../sdk/tc_handle_api.h"
#include "../sdk/tc_command_api.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct V2 {
    float x, y;
};

/* EDIT_001, the type examples/editor-demo registers. */
constexpr uint64_t kTargetId = UINT64_C(0x454449545f303031);
/* EDIT_002: the same Mod's twenty-row type, which is what the scrolling case
   places (`TC_EDITOR_TYPE=long`). */
constexpr uint64_t kLongTargetId = UINT64_C(0x454449545f303032);
/* EDIT_003: the staged type whose table is declared with
   TC_COMPONENT_EDITOR_TABLE_CONFIRM (`TC_EDITOR_TYPE=staged`). */
constexpr uint64_t kStagedTargetId = UINT64_C(0x454449545f303033);
constexpr uint64_t kComponentStride = 0x238, kRecordHeader = 8;
constexpr uint32_t kCustomKind = 0x4e;
/* The cells the placement is retried on, in order.  (30,0) is the one
   tests/component-placement-probe.cpp proves empty on the sandbox board; the
   rest keep a board that already has content from turning the run into a single
   failure. */
const int32_t kCells[][2] = {{30, 0}, {34, 0}, {26, 2}, {38, 2}, {22, 0}, {30, 4}};
constexpr int kCellCount = static_cast<int>(sizeof(kCells) / sizeof(kCells[0]));

using SelectComponent = void (*)(int64_t, void*);
using ClearSelections = void (*)();
using WorldToScreen = V2 (*)(V2);
using GetIo = void* (*)();
using LoadLevel = void (*)(void*, const tc::TCNimString*);
using SetSimTest = void (*)(void*, int64_t, uint8_t);
using CompileRequest = void (*)(void*, tc::TCNimString*, int64_t);
/* The same helper the game's own component menu uses; the loader calls it from
   the command bus, and this driver calls it directly as a cross-check (see
   tests/component-placement-probe.cpp, which places this way). */
using AddComponent = bool (*)(void*, unsigned char*);
const TCHost* host = nullptr;
tc::TCMod mod;
HWND window = nullptr;
void* model = nullptr;
bool (*update_original)(void*, void*, void*, uint32_t, uint8_t) = nullptr;

SelectComponent selectComponent = nullptr;
ClearSelections clearSelections = nullptr;
WorldToScreen worldToScreen = nullptr;
GetIo getIo = nullptr;
LoadLevel load_level = nullptr;
SetSimTest set_sim_test = nullptr;
CompileRequest compile_request = nullptr;
AddComponent add_component = nullptr;

struct RowBox {
    std::string field;
    float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;
    /* What the row showed at that moment, so a fold/unfold can be checked
       against the state the player had typed (the trace carries it). */
    std::string value, buffer;
    bool value_valid = false, buffer_valid = false;
    bool valid = false;
};

struct PanelBox {
    float toggle_min_x = 0.f, toggle_min_y = 0.f, toggle_max_x = 0.f, toggle_max_y = 0.f;
    bool valid = false;
};

struct WindowBox {
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    bool valid = false;
};

struct PopupBox {
    float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;
    bool valid = false;
};

std::string log_path, row_wanted = "enabled", keys, level = "sandbox", expect;
/* The staged-table sequence (V4's CONFIRM): a second row to stage, and which of
   the two buttons ends the batch. */
std::string row_wanted2, dialog_action;
bool dialog_sequence = false;
/* Rest the pointer on a V4 validation marker once the first gesture is done, so
   the message that row is carrying can be read from its tooltip. */
bool hover_after = false;
uint64_t target_id = kTargetId;
std::vector<RowBox> rows;
uint64_t target_instance = 0, request = 0;
/* The cell the command-bus placement landed on, so the driver edits the instance
   the *public* path created rather than the one its own cross-check placed. */
int32_t target_x = 0, target_y = 0;
bool target_cell_known = false;
int start_frame = 0, stage = 0, stage_frame = 0, clicks = 0, body_attempts = 0;
int scroll_ticks = 0;
int placement_attempts = 0, placement_cell = 0;
bool level_requested = false, compile_requested = false;
bool trace_once = false;
/* Set when the harness has already opened the board (dev.enter-board), in which
   case the driver keeps it instead of loading a level of its own. */
bool skip_level = false;
bool finished = false, placement_sent = false, placement_seen = false;
/* The fold case: type into a row without committing, fold the game's drawer,
   unfold it, and require the row to come back in the same place carrying what
   was typed. */
bool fold_test = false;
RowBox pre_fold_row;
PanelBox panel;
int fold_row_lines_at_press = 0, fold_rows_drawn = 0;
std::string fold_summary;
/* The row geometry has to stand still before it is clicked: the loader clamps
   its row window onto the screen, so the first frames of a freshly drawn editor
   still carry the unclamped (and possibly off-screen) rectangle. */
float stable_x0 = 0.f, stable_y0 = 0.f, stable_x1 = 0.f, stable_y1 = 0.f;
int stable_reads = 0;
bool have_stable = false;
float scale_x = 1.f, scale_y = 1.f;
int client_width = 0, client_height = 0;

void say(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

std::string env(const char* name) {
    char buffer[512] = {};
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return length && length < sizeof(buffer) ? std::string(buffer, length) : std::string();
}

std::string dataDirectory() {
    return host && host->data_directory_utf8 ? host->data_directory_utf8 : "";
}

void finish(const std::string& message) {
    if (finished) return;
    finished = true;
    say(message);
    const std::string directory = dataDirectory();
    if (!directory.empty())
        std::ofstream(directory + "\\result.txt", std::ios::binary) << message;
}

std::string hex64(uint64_t value) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

/* ---- the board ---------------------------------------------------------- */

struct BoardArrays {
    uint64_t components = 0;
    const unsigned char* data = nullptr;
};

bool boardArrays(BoardArrays* out) {
    TCGameHandle handle{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &handle) != TC_HANDLE_OK) return false;
    const void* raw = nullptr;
    if (tc::resolveGameHandle(host, &handle, &raw) != TC_HANDLE_OK || !raw) return false;
    const auto* bytes = static_cast<const unsigned char*>(raw);
    std::memcpy(&out->components, bytes + 0x78, sizeof(out->components));
    std::memcpy(&out->data, bytes + 0x80, sizeof(out->data));
    return out->data && out->components > 0 && out->components <= 100000;
}

uint64_t findTargetInstance() {
    BoardArrays arrays{};
    if (!boardArrays(&arrays)) return 0;
    uint64_t fallback = 0;
    for (uint64_t index = 0; index < arrays.components; ++index) {
        const unsigned char* record = arrays.data + kRecordHeader + index * kComponentStride;
        if (record[0] != kCustomKind) continue;
        uint64_t id = 0, custom = 0;
        std::memcpy(&id, record + 8, sizeof(id));
        std::memcpy(&custom, record + 0x188, sizeof(custom));
        if (custom != target_id) continue;
        if (!target_cell_known) return id;
        int16_t x = 0, y = 0;
        std::memcpy(&x, record + 2, sizeof(x));
        std::memcpy(&y, record + 4, sizeof(y));
        if (x == target_x && y == target_y) return id;
        if (!fallback) fallback = id;
    }
    return fallback;
}

bool sendPlacement() {
    TCGameHandle board{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) != TC_HANDLE_OK) return false;
    TCCommandApiV2 api{};
    if (host->query_service(host->context, TC_SERVICE_COMMANDS, TC_COMMAND_API_VERSION_2,
                            &api, sizeof(api)) != TC_SERVICE_OK)
        return false;
    TCCommandV2 place{};
    place.size = sizeof(place);
    place.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
    place.subject = board;
    place.custom_prototype_id = target_id;
    place.kind = kCustomKind;
    place.x = kCells[placement_cell % kCellCount][0];
    place.y = kCells[placement_cell % kCellCount][1];
    if (api.submit(api.context, &place, &request) != TC_COMMAND_OK) return false;
    say("DRIVER: asked for one editor-demo component at " + std::to_string(place.x) + "," +
        std::to_string(place.y) + " (request " + std::to_string(request) + ")");
    return true;
}

/* The command bus runs the placement after this frame's callbacks, so a refusal
   (a stale Board handle while the level is still settling, for instance) shows
   up in the status a frame or two later. */
/* Exactly the record board_edits::buildPlacement builds in the loader. */
void buildPlacementRecord(unsigned char* record, uint64_t custom_id, int32_t x, int32_t y) {
    std::memset(record, 0, kComponentStride);
    record[0] = static_cast<unsigned char>(kCustomKind);
    const int32_t point = static_cast<int32_t>((static_cast<uint32_t>(y) << 16) |
                                               static_cast<uint16_t>(x));
    std::memcpy(record + 2, &point, sizeof(point));
    const uint64_t one = 1, capacity = 0x100;
    std::memcpy(record + 0x58, &one, sizeof(one));
    std::memcpy(record + 0x60, &capacity, sizeof(capacity));
    record[0x68] = 1;
    std::memcpy(record + 0x70, &one, sizeof(one));
    std::memcpy(record + 0x78, &capacity, sizeof(capacity));
    record[0x80] = 1;
    std::memcpy(record + 0x188, &custom_id, sizeof(custom_id));
}

bool commandOutcome(int* result) {
    *result = TC_COMMAND_OK;
    TCCommandApiV2 api{};
    if (!host->query_service || host->query_service(host->context, TC_SERVICE_COMMANDS,
                                                    TC_COMMAND_API_VERSION_2, &api,
                                                    sizeof(api)) != TC_SERVICE_OK)
        return true;
    TCCommandStatusV1 status{};
    status.size = sizeof(status);
    status.version = 1u;
    if (api.get_status(api.context, request, &status, sizeof(status)) != TC_COMMAND_OK)
        return true;
    if (status.state == TC_COMMAND_STATE_SUCCEEDED || status.state == TC_COMMAND_STATE_FAILED ||
        status.state == TC_COMMAND_STATE_CANCELLED) {
        *result = status.result;
        return true;
    }
    return false;
}

/* Where the component's own cell is on screen.  The game's transform returns
   normalised coordinates, so it is multiplied by the ImGui display size - the
   same recipe examples/board-grid uses - and the caller scales that into client
   pixels. */
bool targetScreenPoint(float* out_x, float* out_y) {
    BoardArrays arrays{};
    if (!boardArrays(&arrays) || !worldToScreen || !getIo || !target_instance) return false;
    for (uint64_t index = 0; index < arrays.components; ++index) {
        const unsigned char* record = arrays.data + kRecordHeader + index * kComponentStride;
        uint64_t id = 0;
        std::memcpy(&id, record + 8, sizeof(id));
        if (id != target_instance) continue;
        int16_t x = 0, y = 0;
        std::memcpy(&x, record + 2, sizeof(x));
        std::memcpy(&y, record + 4, sizeof(y));
        const V2 normalised = worldToScreen(V2{static_cast<float>(x), static_cast<float>(y)});
        float display[2] = {0.f, 0.f};
        const auto* io = static_cast<const unsigned char*>(getIo());
        if (!io) return false;
        std::memcpy(display, io + 8, sizeof(display));
        if (display[0] <= 1.f || display[1] <= 1.f) return false;
        if (trace_once) {
            trace_once = false;
            say("DRIVER: the target sits on cell " + std::to_string(x) + "," + std::to_string(y) +
                " of " + std::to_string(arrays.components) + " board records; display=" +
                std::to_string((int)display[0]) + "x" + std::to_string((int)display[1]));
        }
        *out_x = normalised.x * display[0];
        *out_y = normalised.y * display[1];
        return true;
    }
    return false;
}

bool selectTarget(uint64_t instance) {
    BoardArrays arrays{};
    if (!boardArrays(&arrays) || !selectComponent || !clearSelections || !instance) return false;
    for (uint64_t index = 0; index < arrays.components; ++index) {
        unsigned char* record =
            const_cast<unsigned char*>(arrays.data) + kRecordHeader + index * kComponentStride;
        uint64_t id = 0;
        std::memcpy(&id, record + 8, sizeof(id));
        if (id != instance) continue;
        clearSelections();
        /* The game's own setter takes the component sequence index on this
           build (the same numbering its delete command uses). */
        selectComponent(static_cast<int64_t>(index), record);
        return true;
    }
    return false;
}

/* ---- the rows the loader drew ------------------------------------------- */

/* One line per row per second:
     Component editor: row instance=0x… field="label" rect=x1,y1,x2,y2
   The last reading of each field wins, so a stale rectangle from an earlier
   frame cannot be clicked by accident. */
bool readRows() {
    if (log_path.empty()) return false;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return false;
    std::vector<RowBox> parsed;
    char line[512] = {};
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "Component editor: row ", 22) != 0) continue;
        char field[64] = {};
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        if (std::sscanf(line, "Component editor: row instance=%*s field=\"%63[^\"]\" rect=%f,%f,%f,%f",
                        field, &x0, &y0, &x1, &y1) != 5)
            continue;
        RowBox box;
        box.field = field;
        box.min_x = x0;
        box.min_y = y0;
        box.max_x = x1;
        box.max_y = y1;
        box.valid = x1 > x0 && y1 > y0;
        /* value="..." and buffer="..." follow the rectangle; they are what the
           fold case compares (the trace is the only place a not-yet-committed
           edit exists). */
        if (const char* at = std::strstr(line, " value=\"")) {
            const char* begin = at + 8;
            const char* end = std::strchr(begin, '"');
            if (end) { box.value.assign(begin, end - begin); box.value_valid = true; }
        }
        if (const char* at = std::strstr(line, " buffer=\"")) {
            const char* begin = at + 9;
            const char* end = std::strchr(begin, '"');
            if (end) { box.buffer.assign(begin, end - begin); box.buffer_valid = true; }
        }
        bool replaced = false;
        for (RowBox& earlier : parsed)
            if (earlier.field == box.field) {
                earlier = box;
                replaced = true;
                break;
            }
        if (!replaced) parsed.push_back(box);
    }
    std::fclose(file);
    if (parsed.empty()) return false;
    rows = parsed;
    return true;
}

const RowBox* rowOf(const char* field) {
    for (const RowBox& box : rows)
        if (box.field == field && box.valid) return &box;
    return nullptr;
}

/* How many lines in the loader log start with `prefix`.  The fold case uses it
   to prove that *no* row was traced while the drawer was folded. */
int countLogLines(const char* prefix) {
    if (log_path.empty()) return 0;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return 0;
    int count = 0;
    char line[2048] = {};
    const size_t length = std::strlen(prefix);
    while (std::fgets(line, sizeof(line), file))
        if (std::strncmp(line, prefix, length) == 0) ++count;
    std::fclose(file);
    return count;
}

/* The game's own fold switch.  The loader publishes it in every drawer state
   (the box can move with the fold), so any line that carries it counts and the
   last one wins. */
bool readPanel(PanelBox* out) {
    if (log_path.empty()) return false;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return false;
    char line[2048] = {};
    bool found = false;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "Component ", 10) != 0) continue;
        const char* at = std::strstr(line, "toggle=");
        if (!at) continue;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        if (std::sscanf(at, "toggle=%f,%f,%f,%f", &x0, &y0, &x1, &y1) == 4 &&
            x1 > x0 && y1 > y0) {
            out->toggle_min_x = x0;
            out->toggle_min_y = y0;
            out->toggle_max_x = x1;
            out->toggle_max_y = y1;
            out->valid = true;
            found = true;
        }
    }
    std::fclose(file);
    return found;
}

/* The row window's own rectangle: the row trace gives rows wherever they are in
   the table, and this says which part of that table is actually on screen. */
bool readWindowBox(WindowBox* out) {
    if (log_path.empty()) return false;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return false;
    char line[2048] = {};
    bool found = false;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "Component editor: window ", 25) != 0) continue;
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        if (std::sscanf(line, "Component editor: window instance=%*s pos=%f,%f size=%f,%f",
                        &x, &y, &w, &h) == 4 && w > 1.f && h > 1.f) {
            out->x = x; out->y = y; out->w = w; out->h = h;
            out->valid = true;
            found = true;
        }
    }
    std::fclose(file);
    return found;
}

/* A combo popup's rectangle, published while it is open. */
bool readComboPopup(PopupBox* out) {
    if (log_path.empty()) return false;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return false;
    char line[2048] = {};
    bool found = false;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "Component editor: popup ", 24) != 0) continue;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        if (std::sscanf(line, "Component editor: popup instance=%*s field=%*s rect=%f,%f,%f,%f",
                        &x0, &y0, &x1, &y1) == 4 && x1 > x0 && y1 > y0) {
            out->min_x = x0; out->min_y = y0; out->max_x = x1; out->max_y = y1;
            out->valid = true;
            found = true;
        }
    }
    std::fclose(file);
    return found;
}

/* One entry of a popup (a dropdown or a custom row's context menu), published
   while it is open.  These are the only reliable target: the entries are taller
   than the rows, and ImGui moves the popup (first frame at the pointer, then
   flipped above the field when the drawer leaves no room below it). */
bool readPopupEntry(const char* prefix, size_t prefix_length, int index, PopupBox* out) {
    if (log_path.empty()) return false;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return false;
    char line[2048] = {};
    bool found = false;
    const std::string needle = "index=" + std::to_string(index) + " ";
    /* Only blocks published after this gesture's own log marker count: the same
       popup is opened again on every attempt and an older placement must not be
       mistaken for the one that is on screen now. */
    long marker = -1;
    while (std::fgets(line, sizeof(line), file)) {
        const long position = std::ftell(file);
        if (std::strstr(line, "DRIVER: gesture attempt")) marker = position;
    }
    std::rewind(file);
    if (marker >= 0) std::fseek(file, marker, SEEK_SET);
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, prefix, prefix_length) != 0) continue;
        const char* at = std::strstr(line, needle.c_str());
        if (!at) continue;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        if (std::sscanf(at, "index=%*d rect=%f,%f,%f,%f", &x0, &y0, &x1, &y1) == 4 &&
            x1 > x0 && y1 > y0) {
            out->min_x = x0; out->min_y = y0; out->max_x = x1; out->max_y = y1;
            out->valid = true;
            found = true;
        }
    }
    std::fclose(file);
    return found;
}

bool readComboItem(int index, PopupBox* out) {
    return readPopupEntry("Component editor: popup item ", 29, index, out);
}

/* One swatch of a V4 palette row.  The strip's geometry follows the row width,
   which follows the window, so the host publishes each swatch's own rectangle
   and a reader clicks that rather than a fraction of the row. */
bool readSwatch(const char* field, int index, PopupBox* out) {
    if (log_path.empty()) return false;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return false;
    char line[2048] = {};
    const std::string needle = std::string("field=\"") + field + "\" index=" +
                               std::to_string(index) + " ";
    bool found = false;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "Component editor: swatch ", 25) != 0) continue;
        if (!std::strstr(line, needle.c_str())) continue;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        if (std::sscanf(line,
                        "Component editor: swatch instance=%*s field=%*s index=%*d "
                        "rect=%f,%f,%f,%f", &x0, &y0, &x1, &y1) == 4 && x1 > x0 && y1 > y0) {
            out->min_x = x0; out->min_y = y0; out->max_x = x1; out->max_y = y1;
            out->valid = true;
            found = true;
        }
    }
    std::fclose(file);
    return found;
}

/* A V4 validation marker: the row's message is a small square at the end of the
   control, and this is where the host says it put it. */
bool readValidationIcon(const char* field, PopupBox* out) {
    if (log_path.empty()) return false;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return false;
    char line[2048] = {};
    const std::string needle = std::string("field=\"") + field + "\"";
    bool found = false;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "Component editor: validation icon ", 34) != 0) continue;
        if (!std::strstr(line, needle.c_str())) continue;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        if (std::sscanf(line,
                        "Component editor: validation icon instance=%*s field=%*s status=%*d "
                        "rect=%f,%f,%f,%f", &x0, &y0, &x1, &y1) == 4 && x1 > x0 && y1 > y0) {
            out->min_x = x0; out->min_y = y0; out->max_x = x1; out->max_y = y1;
            out->valid = true;
            found = true;
        }
    }
    std::fclose(file);
    return found;
}

/* A staged table's two buttons.  They are published together with the number of
   fields waiting, which is what the sequence asserts before pressing one. */
struct DialogBox {
    PopupBox apply_box, cancel_box;
    int pending = -1;
};

bool readDialog(DialogBox* out) {
    if (log_path.empty()) return false;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return false;
    char line[2048] = {};
    bool found = false;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "Component editor: dialog ", 25) != 0) continue;
        float ax0 = 0.f, ay0 = 0.f, ax1 = 0.f, ay1 = 0.f;
        float cx0 = 0.f, cy0 = 0.f, cx1 = 0.f, cy1 = 0.f;
        int pending = -1;
        if (std::sscanf(line,
                        "Component editor: dialog instance=%*s apply=%f,%f,%f,%f "
                        "cancel=%f,%f,%f,%f pending=%d",
                        &ax0, &ay0, &ax1, &ay1, &cx0, &cy0, &cx1, &cy1, &pending) == 9 &&
            ax1 > ax0 && ay1 > ay0 && cx1 > cx0 && cy1 > cy0) {
            out->apply_box = PopupBox{ax0, ay0, ax1, ay1, true};
            out->cancel_box = PopupBox{cx0, cy0, cx1, cy1, true};
            out->pending = pending;
            found = true;
        }
    }
    std::fclose(file);
    return found;
}

bool readMenuItem(int index, PopupBox* out) {
    return readPopupEntry("Component editor: menu item ", 28, index, out);
}

/* The resize grip's rectangle, published by the host (the row window's clip can
   cut the bottom corner of the last row, so the grip is not where the row's
   corner is). */
bool readGrip(const char* field, PopupBox* out) {
    if (log_path.empty()) return false;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return false;
    char line[2048] = {};
    /* The grip is drawn from the first frame the rows exist, and it is published
       only when it moves - so if nothing was written after this gesture's marker,
       the last line of the whole log is still the grip on screen. */
    PopupBox afterMarker{};
    bool foundAfterMarker = false, foundAny = false;
    long marker = -1;
    while (std::fgets(line, sizeof(line), file)) {
        const long position = std::ftell(file);
        if (std::strstr(line, "DRIVER: gesture attempt")) marker = position;
    }
    const std::string needle = std::string("field=\"") + field + "\"";
    auto parse = [&](long from, PopupBox* sink, bool* flag) {
        if (from > 0) std::fseek(file, from, SEEK_SET);
        else std::rewind(file);
        while (std::fgets(line, sizeof(line), file)) {
            if (std::strncmp(line, "Component editor: grip ", 23) != 0) continue;
            if (!std::strstr(line, needle.c_str())) continue;
            float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
            if (std::sscanf(line, "Component editor: grip instance=%*s field=%*s rect=%f,%f,%f,%f",
                            &x0, &y0, &x1, &y1) == 4 && x1 > x0 && y1 > y0) {
                sink->min_x = x0; sink->min_y = y0; sink->max_x = x1; sink->max_y = y1;
                sink->valid = true;
                *flag = true;
            }
        }
    };
    parse(marker, &afterMarker, &foundAfterMarker);
    std::rewind(file);
    parse(0, out, &foundAny);
    std::fclose(file);
    if (foundAfterMarker) {
        *out = afterMarker;
        return true;
    }
    return foundAny;
}

/* ---- real input --------------------------------------------------------- */

void moveTo(float game_x, float game_y) {
    if (!window) return;
    const int client_x = static_cast<int>(game_x * scale_x);
    const int client_y = static_cast<int>(game_y * scale_y);
    POINT screen{client_x, client_y};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(client_x, client_y));
}

void clickAt(float game_x, float game_y) {
    if (!window) return;
    const int client_x = static_cast<int>(game_x * scale_x);
    const int client_y = static_cast<int>(game_y * scale_y);
    POINT screen{client_x, client_y};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    Sleep(30);
    const LPARAM point = MAKELPARAM(client_x, client_y);
    PostMessageW(window, WM_MOUSEMOVE, 0, point);
    Sleep(40);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
    Sleep(60);
    PostMessageW(window, WM_LBUTTONUP, 0, point);
    ++clicks;
}

LPARAM clientPoint(float game_x, float game_y) {
    const int client_x = static_cast<int>(game_x * scale_x);
    const int client_y = static_cast<int>(game_y * scale_y);
    POINT screen{client_x, client_y};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    return MAKELPARAM(client_x, client_y);
}

/* A drag cannot be one payload: the host only reports a resize while ImGui sees
   the grip *active*, and a down/move/up burst delivered between two frames
   leaves the button up by the time the row is drawn.  So the press, the moves
   and the release are posted on separate frames. */
void pressAt(float game_x, float game_y) {
    if (!window) return;
    const LPARAM point = clientPoint(game_x, game_y);
    PostMessageW(window, WM_MOUSEMOVE, 0, point);
    Sleep(30);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
}

void dragTo(float game_x, float game_y) {
    if (!window) return;
    const LPARAM point = clientPoint(game_x, game_y);
    PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, point);
}

void releaseAt(float game_x, float game_y) {
    if (!window) return;
    PostMessageW(window, WM_LBUTTONUP, 0, clientPoint(game_x, game_y));
    ++clicks;
}

void rightClickAt(float game_x, float game_y) {
    if (!window) return;
    const LPARAM point = clientPoint(game_x, game_y);
    Sleep(30);
    PostMessageW(window, WM_MOUSEMOVE, 0, point);
    Sleep(40);
    PostMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, point);
    Sleep(60);
    PostMessageW(window, WM_RBUTTONUP, 0, point);
    ++clicks;
}

/* One wheel notch, sent where a player's pointer would be.  WM_MOUSEWHEEL
   carries *screen* coordinates, unlike the other mouse messages. */
void postWheel(float game_x, float game_y, int notches) {
    if (!window || !notches) return;
    const int client_x = static_cast<int>(game_x * scale_x);
    const int client_y = static_cast<int>(game_y * scale_y);
    POINT screen{client_x, client_y};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(client_x, client_y));
    const WORD delta = static_cast<WORD>(notches > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
    for (int index = 0; index < std::abs(notches); ++index)
        PostMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, delta),
                     MAKELPARAM(screen.x, screen.y));
}

/* Does the loader log already carry this line?  The driver retries a gesture on
   a slightly different point until it does, the way a player tries again, and
   the playtest only has to assert the end state. */
bool logHas(const std::string& needle) {
    if (log_path.empty() || needle.empty()) return false;
    std::FILE* file = std::fopen(log_path.c_str(), "rb");
    if (!file) return false;
    bool found = false;
    char line[2048] = {};
    while (std::fgets(line, sizeof(line), file))
        if (std::strstr(line, needle.c_str())) {
            found = true;
            break;
        }
    std::fclose(file);
    return found;
}

/* ---- the row gestures --------------------------------------------------- */

/* How the driver drives a row.  `click` is what the V1/V2 cases always did; the
   rest are the V3 shapes: a combo that opens and then takes a choice, a context
   menu, a double click, and the two drags (the resize grip and the colour
   widget's sliders).

   The candidates move a little between attempts because the popup offset and
   the field's own width are ImGui's business, not the driver's: each attempt
   presses somewhere slightly different, and the run stops as soon as the
   expected line appears in the loader log. */
/* `Swatch` clicks one preset of a V4 palette row and `Hover` rests the pointer
   on a V4 validation marker; both read the rectangle the host published instead
   of estimating one.  `SelectText` drags across a text box before anything is
   typed, which is how a player replaces a word: the editing keys never reach
   ImGui on this build, but the mouse does select. */
enum class Gesture { Click, Double, Dropdown, Menu, Grip, Wiggle, Swatch, Hover, SelectText };

Gesture gesture = Gesture::Click;
int choice = 0, gesture_attempt = 0, gesture_phase = 0;
/* How many entries the popup a gesture opens has.  The driver needs it to aim
   at an entry when ImGui flips the popup above the row (the drawer sits at the
   bottom of the screen, so that flip is the common case): above the row the
   *last* entry is the one nearest the row. */
int items = 5;
/* How many frames a gesture has been waiting for the host to publish the
   rectangle it needs; a gesture that never gets one has to end so the caller's
   retry loop can try a different point. */
int gesture_wait = 0;
float gesture_x0 = 0.f, gesture_y0 = 0.f, gesture_x1 = 0.f, gesture_y1 = 0.f;
/* The popup entry read last frame, used to click only once it stops moving. */
PopupBox lastItem;
bool haveLastItem = false;
constexpr int kMaxGestureAttempts = 8;

float rowHeight(const RowBox& box) {
    return box.max_y - box.min_y > 4.f ? box.max_y - box.min_y : 20.f;
}

void gestureBegin(const RowBox& box) {
    const float height = rowHeight(box);
    const float mid_x = (box.min_x + box.max_x) * 0.5f;
    const float mid_y = (box.min_y + box.max_y) * 0.5f;
    const float width = box.max_x - box.min_x;
    const float jitter = 5.f * ((gesture_attempt % 3) - 1);
    gesture_phase = 0;
    haveLastItem = false;
    gesture_wait = 0;
    gesture_x0 = mid_x;
    gesture_y0 = mid_y;
    gesture_x1 = mid_x;
    gesture_y1 = mid_y;
    switch (gesture) {
        case Gesture::Grip:
            gesture_x0 = box.max_x - 6.f + jitter;
            gesture_y0 = box.max_y - 6.f;
            gesture_x1 = gesture_x0 + 34.f;
            gesture_y1 = gesture_y0 + 16.f;
            break;
        case Gesture::Wiggle:
            gesture_x0 = box.min_x + width * (0.32f + 0.06f * (gesture_attempt % 8));
            gesture_y0 = box.min_y + height * (0.25f + 0.25f * (gesture_attempt % 3));
            gesture_x1 = gesture_x0 + 26.f;
            gesture_y1 = gesture_y0;
            break;
        default:
            break;
    }
    say("DRIVER: gesture attempt " + std::to_string(gesture_attempt) + " on the " +
        row_wanted + " row at " + std::to_string((int)gesture_x0) + "," +
        std::to_string((int)gesture_y0));
}

/* One frame of the gesture.  Nothing may be posted and clicked in the same
   payload: the popups of a combo or a context menu only exist on the frame
   *after* the click that opened them is processed, and a drag has to be spread
   over frames (see pressAt).  Returns true when the gesture is done. */
bool gestureTick(const RowBox& box) {
    const float height = rowHeight(box);
    const float mid_x = (box.min_x + box.max_x) * 0.5f;
    const float mid_y = (box.min_y + box.max_y) * 0.5f;
    switch (gesture) {
        case Gesture::Click:
            if (gesture_phase == 0) {
                moveTo(gesture_x0, gesture_y0);
                clickAt(gesture_x0, gesture_y0);
                gesture_phase = 1;
                return false;
            }
            return true;
        case Gesture::Swatch:
            if (gesture_phase == 0) {
                /* The host publishes each swatch's own rectangle, so the click
                   lands on a preset instead of a fraction of the row.  Waiting
                   for it is bounded: a row without a published swatch ends the
                   gesture and the caller's retry loop takes over. */
                PopupBox swatch{};
                if (!readSwatch(row_wanted.c_str(), choice, &swatch)) {
                    if (++gesture_wait < 40) return false;
                    return true;
                }
                const float x = (swatch.min_x + swatch.max_x) * 0.5f;
                const float y = (swatch.min_y + swatch.max_y) * 0.5f;
                say("DRIVER: aiming at swatch " + std::to_string(choice) + " rect=" +
                    std::to_string((int)swatch.min_x) + "," +
                    std::to_string((int)swatch.min_y) + "," +
                    std::to_string((int)swatch.max_x) + "," +
                    std::to_string((int)swatch.max_y) + " click=" + std::to_string((int)x) +
                    "," + std::to_string((int)y));
                moveTo(x, y);
                clickAt(x, y);
                gesture_phase = 1;
                return false;
            }
            return true;
        case Gesture::Hover:
            if (gesture_phase == 0) {
                PopupBox icon{};
                if (!readValidationIcon(row_wanted.c_str(), &icon)) {
                    if (++gesture_wait < 40) return false;
                    return true;
                }
                const float x = (icon.min_x + icon.max_x) * 0.5f;
                const float y = (icon.min_y + icon.max_y) * 0.5f;
                say("DRIVER: hovering the validation marker rect=" +
                    std::to_string((int)icon.min_x) + "," +
                    std::to_string((int)icon.min_y) + "," +
                    std::to_string((int)icon.max_x) + "," +
                    std::to_string((int)icon.max_y) + " at " + std::to_string((int)x) + "," +
                    std::to_string((int)y));
                moveTo(x, y);
                gesture_phase = 1;
                return false;
            }
            return true;
        case Gesture::Double:
            if (gesture_phase == 0) {
                clickAt(gesture_x0, gesture_y0);
                gesture_phase = 1;
                return false;
            }
            if (gesture_phase == 1) {
                clickAt(gesture_x0, gesture_y0);
                gesture_phase = 2;
                return false;
            }
            return true;
        case Gesture::SelectText:
            if (gesture_phase == 0) {
                pressAt(box.min_x + 6.f, mid_y);
                gesture_phase = 1;
                return false;
            }
            if (gesture_phase == 1 || gesture_phase == 2) {
                dragTo(box.max_x - 6.f, mid_y);
                ++gesture_phase;
                return false;
            }
            releaseAt(box.max_x - 6.f, mid_y);
            return true;
        case Gesture::Dropdown:
            if (gesture_phase == 0) {
                moveTo(gesture_x0, gesture_y0);
                clickAt(gesture_x0, gesture_y0);
                gesture_phase = 1;
                return false;
            }
            if (gesture_phase == 1) {
                gesture_phase = 2;   /* let the popup be drawn once */
                return false;
            }
            if (gesture_phase >= 2 && gesture_phase < 8) {
                /* The popup is first placed under the pointer and then moved to
                   fit the drawer, so the entry is clicked only once its
                   rectangle has stood still for a frame. */
                PopupBox item{};
                if (readComboItem(static_cast<int>(choice), &item)) {
                    const bool settled = haveLastItem &&
                                         item.min_x == lastItem.min_x && item.min_y == lastItem.min_y &&
                                         item.max_x == lastItem.max_x && item.max_y == lastItem.max_y;
                    lastItem = item;
                    haveLastItem = true;
                    if (settled) {
                        const float x = (item.min_x + item.max_x) * 0.5f;
                        const float y = (item.min_y + item.max_y) * 0.5f;
                        say("DRIVER: aiming at combo entry " + std::to_string(choice) +
                            " rect=" + std::to_string((int)item.min_x) + "," +
                            std::to_string((int)item.min_y) + "," +
                            std::to_string((int)item.max_x) + "," +
                            std::to_string((int)item.max_y) + " click=" +
                            std::to_string((int)x) + "," + std::to_string((int)y));
                        moveTo(x, y);
                        clickAt(x, y);
                        gesture_phase = 9;
                        return false;
                    }
                    ++gesture_phase;
                    return false;
                }
                gesture_phase = 8;
                return false;
            }
            if (gesture_phase == 8) {
                /* No published entry (an older loader): estimate inside the
                   popup the way the layout usually lands. */
                /* The popup is only as wide as its widest entry, and it opens at
                   the combo's own left edge: aiming at the row's centre lands
                   outside it and just closes it (measured on the first attempt
                   at this case).  So the entry is clicked near the popup's left
                   edge, and the horizontal jitter walks into the label. */
                const float pad = 4.f + 4.f * (gesture_attempt % 3);
                const bool above = (gesture_attempt % 2) != 0;
                float y = above ? box.min_y - pad - (items - choice - 0.5f) * height
                                : box.max_y + pad + (choice + 0.5f) * height;
                float x = box.min_x + 10.f + 8.f * (gesture_attempt % 4);
                PopupBox popup{};
                if (readComboPopup(&popup) && popup.max_y > popup.min_y && items > 0) {
                    /* The entries fill the popup minus its padding: the row height
                       is *not* the entry height (measured: a 41 px row has ~53 px
                       entries inside a 276 px five-entry popup), so the entry
                       height is derived from the popup itself. */
                    const float popupHeight = popup.max_y - popup.min_y;
                    const float padding = std::min(8.f, popupHeight * 0.03f);
                    const float itemHeight = std::max(1.f, (popupHeight - 2.f * padding) / items);
                    y = popup.min_y + padding + (choice + 0.5f) * itemHeight;
                    x = popup.min_x + 8.f + 6.f * (gesture_attempt % 3);
                }
                moveTo(x, y);
                clickAt(x, y);
                gesture_phase = 9;
                return false;
            }
            return true;
        case Gesture::Menu:
            if (gesture_phase == 0) {
                rightClickAt(mid_x, mid_y);
                gesture_phase = 1;
                return false;
            }
            if (gesture_phase == 1) {
                gesture_phase = 2;   /* let the popup be drawn once */
                return false;
            }
            if (gesture_phase >= 2 && gesture_phase < 8) {
                PopupBox item{};
                if (readMenuItem(static_cast<int>(choice), &item)) {
                    const bool settled = haveLastItem &&
                                         item.min_x == lastItem.min_x && item.min_y == lastItem.min_y &&
                                         item.max_x == lastItem.max_x && item.max_y == lastItem.max_y;
                    lastItem = item;
                    haveLastItem = true;
                    if (settled) {
                        const float x = (item.min_x + item.max_x) * 0.5f;
                        const float y = (item.min_y + item.max_y) * 0.5f;
                        say("DRIVER: aiming at menu entry " + std::to_string(choice) +
                            " rect=" + std::to_string((int)item.min_x) + "," +
                            std::to_string((int)item.min_y) + "," +
                            std::to_string((int)item.max_x) + "," +
                            std::to_string((int)item.max_y) + " click=" +
                            std::to_string((int)x) + "," + std::to_string((int)y));
                        moveTo(x, y);
                        clickAt(x, y);
                        gesture_phase = 9;
                        return false;
                    }
                    ++gesture_phase;
                    return false;
                }
                gesture_phase = 8;
                return false;
            }
            if (gesture_phase == 8) {
                /* The menu opens at the pointer; if it does not fit below the
                   row (the drawer sits at the bottom of the screen) ImGui flips
                   it above, so alternate between the two. */
                const float pad = 4.f + 4.f * (gesture_attempt % 2);
                const bool above = (gesture_attempt % 2) != 0;
                const float y = above ? mid_y - pad - (items - choice - 0.5f) * height
                                      : mid_y + pad + (choice + 0.5f) * height;
                moveTo(mid_x + 10.f, y);
                clickAt(mid_x + 10.f, y);
                gesture_phase = 9;
                return false;
            }
            return true;
        case Gesture::Grip:
            if (gesture_phase < 6) {
                /* Wait for the grip's published rectangle to stand still, then
                   press its centre.  Falls back to the row's own corner if the
                   loader never publishes one. */
                PopupBox grip{};
                float x = gesture_x0;
                float y = gesture_y0;
                if (readGrip(row_wanted.c_str(), &grip)) {
                    const bool settled = haveLastItem &&
                                         grip.min_x == lastItem.min_x && grip.min_y == lastItem.min_y &&
                                         grip.max_x == lastItem.max_x && grip.max_y == lastItem.max_y;
                    lastItem = grip;
                    haveLastItem = true;
                    if (!settled) {
                        ++gesture_phase;
                        return false;
                    }
                    x = (grip.min_x + grip.max_x) * 0.5f;
                    y = (grip.min_y + grip.max_y) * 0.5f;
                    say("DRIVER: pressing the " + row_wanted + " row's resize grip at " +
                        std::to_string((int)x) + "," + std::to_string((int)y));
                } else if (gesture_phase >= 5) {
                    say("DRIVER: no published grip; pressing the row's bottom-right corner");
                } else {
                    ++gesture_phase;
                    return false;
                }
                gesture_x0 = x;
                gesture_y0 = y;
                gesture_x1 = x + 34.f;
                gesture_y1 = y + 16.f;
                pressAt(gesture_x0, gesture_y0);
                gesture_phase = 6;
                return false;
            }
            if (gesture_phase == 6) {
                dragTo(gesture_x0 + (gesture_x1 - gesture_x0) * 0.5f,
                       gesture_y0 + (gesture_y1 - gesture_y0) * 0.5f);
                gesture_phase = 7;
                return false;
            }
            if (gesture_phase == 7) {
                dragTo(gesture_x1, gesture_y1);
                gesture_phase = 8;
                return false;
            }
            if (gesture_phase == 8) {
                releaseAt(gesture_x1, gesture_y1);
                gesture_phase = 9;
                return false;
            }
            return true;
        case Gesture::Wiggle:
            if (gesture_phase == 0) {
                pressAt(gesture_x0, gesture_y0);
                gesture_phase = 1;
                return false;
            }
            if (gesture_phase == 1) {
                dragTo(gesture_x0 + (gesture_x1 - gesture_x0) * 0.5f,
                       gesture_y0 + (gesture_y1 - gesture_y0) * 0.5f);
                gesture_phase = 2;
                return false;
            }
            if (gesture_phase == 2) {
                dragTo(gesture_x1, gesture_y1);
                gesture_phase = 3;
                return false;
            }
            if (gesture_phase == 3) {
                releaseAt(gesture_x1, gesture_y1);
                gesture_phase = 4;
                return false;
            }
            return true;
    }
    return true;
}

void sendChar(char value) {
    if (!window) return;
    PostMessageW(window, WM_CHAR, static_cast<WPARAM>(static_cast<unsigned char>(value)), 1);
}

void sendKey(WORD virtual_key, bool down) {
    if (!window) return;
    const LPARAM data = static_cast<LPARAM>(MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC) << 16);
    PostMessageW(window, down ? WM_KEYDOWN : WM_KEYUP, virtual_key, data);
}

BOOL CALLBACK findWindowCallback(HWND candidate, LPARAM data) {
    DWORD owner = 0;
    GetWindowThreadProcessId(candidate, &owner);
    if (owner != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(candidate)) return TRUE;
    RECT rect{};
    if (!GetClientRect(candidate, &rect)) return TRUE;
    if (rect.right < 320 || rect.bottom < 240) return TRUE;
    *reinterpret_cast<HWND*>(data) = candidate;
    return FALSE;
}

HWND findWindow() {
    HWND found = nullptr;
    EnumWindows(findWindowCallback, reinterpret_cast<LPARAM>(&found));
    return found;
}

/* ---- the run ------------------------------------------------------------ */

bool hookedUpdate(void* m, void* a, void* b, uint32_t c, uint8_t d) {
    model = m;
    return update_original ? update_original(m, a, b, c, d) : false;
}

void tick(void*, const TCFrame* frame) {
    if (!frame || finished) return;
    if (!start_frame) {
        start_frame = frame->frame_number;
        stage_frame = start_frame;
        window = findWindow();
        if (window) {
            SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetForegroundWindow(window);
            RECT client{};
            GetClientRect(window, &client);
            client_width = client.right;
            client_height = client.bottom;
        }
        if (getIo) {
            const auto* io = static_cast<const unsigned char*>(getIo());
            float display[2] = {0.f, 0.f};
            if (io) std::memcpy(display, io + 8, sizeof(display));
            if (display[0] > 1.f && display[1] > 1.f && client_width > 1 && client_height > 1) {
                scale_x = static_cast<float>(client_width) / display[0];
                scale_y = static_cast<float>(client_height) / display[1];
            }
        }
        say("DRIVER: armed window client=" + std::to_string(client_width) + "x" +
            std::to_string(client_height) + " scale=" + std::to_string(scale_x) + "x" +
            std::to_string(scale_y) + " row=" + row_wanted + " log=" + log_path);
    }
    if (frame->frame_number - start_frame > 0 && (frame->frame_number - start_frame) % 900 == 0)
        say("DRIVER: alive stage=" + std::to_string(stage) + " clicks=" +
            std::to_string(clicks));

    switch (stage) {
        case 0:
            /* The model pointer comes from the game's own board-input function,
               the way every board probe here captures it.  It requires a board
               *screen*, which is why the harness also installs
               dist/dev.enter-board.mod (it presses the game's own level entry).
               Once the pointer is here the driver loads the level it wants and
               waits for the scene to settle - a placement on a scene that is
               still settling is refused with TC_COMMAND_ERR_STATE. */
            if (!model) {
                if (frame->frame_number - start_frame > 1800)
                    finish("FAIL the game never reported a board model to the driver");
                return;
            }
            if (!level_requested) {
                level_requested = true;
                if (!skip_level && load_level && mod.game.raw_new_string) {
                    tc::TCNimString name{};
                    mod.game.raw_new_string(&name, static_cast<int64_t>(level.size()));
                    name.length = level.size();
                    std::memcpy(static_cast<unsigned char*>(name.data) + 8, level.c_str(),
                                level.size());
                    static_cast<unsigned char*>(name.data)[8 + level.size()] = 0;
                    load_level(model, &name);
                    say("DRIVER: entered level " + level);
                } else {
                    say("DRIVER: using the board the harness opened");
                }
                stage_frame = frame->frame_number;
                return;
            }
            /* ~3 s at the game's frame rate: tests/component-placement-probe.cpp
               waits the same before its first placement. */
            if (frame->frame_number - stage_frame < 180) return;
            say("DRIVER: the scene has settled; placing the component");
            /* Cross-check before the command bus: is the Board handle the
               loader resolves the same pointer the game hands the driver, and
               does the game's own placement helper accept this record?  A "no"
               to either is the answer to why the command bus refuses. */
            {
                TCGameHandle board{};
                const void* resolved = nullptr;
                const bool haveHandle =
                    tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) == TC_HANDLE_OK;
                const bool resolvedOk =
                    haveHandle &&
                    tc::resolveGameHandle(host, &board, &resolved) == TC_HANDLE_OK && resolved;
                say("DRIVER: model=" + hex64(reinterpret_cast<uintptr_t>(model)) +
                    " board handle -> " + hex64(reinterpret_cast<uintptr_t>(resolved)) +
                    " same=" + std::string(resolvedOk && resolved == model ? "yes" : "no"));
                if (add_component) {
                    /* Deliberately a cell the command-bus list does not use: the
                       cross-check must not take the cell the real placement is
                       about to ask for (a taken cell is refused with -6). */
                    unsigned char record[kComponentStride];
                    buildPlacementRecord(record, target_id, 46, 6);
                    const bool placed = add_component(model, record);
                    say("DRIVER: the game's own placement helper returned " +
                        std::string(placed ? "true" : "false") + " at 46,6");
                }
            }
            stage = 2;
            stage_frame = frame->frame_number;
            return;
        case 1:
            return;
        case 2:
            if (frame->frame_number - stage_frame < 30) return;
            if (!placement_sent) {
                if (!sendPlacement()) {
                    if (frame->frame_number - stage_frame > 900)
                        finish("FAIL the placement command could not be submitted");
                    return;
                }
                placement_sent = true;
                stage = 8;
                stage_frame = frame->frame_number;
                return;
            }
            stage = 3;
            stage_frame = frame->frame_number;
            return;
        case 8: {
            /* Waiting for the placement to report its outcome; a refusal is
               retried, because a sandbox that is still opening its level refuses
               the first handle. */
            if (frame->frame_number - stage_frame < 30) return;
            int result = TC_COMMAND_OK;
            if (!commandOutcome(&result)) return;
            if (result == TC_COMMAND_OK) {
                target_x = kCells[placement_cell % kCellCount][0];
                target_y = kCells[placement_cell % kCellCount][1];
                target_cell_known = true;
                say("DRIVER: the placement command succeeded");
                stage = 3;
                stage_frame = frame->frame_number;
                return;
            }
            if (++placement_attempts >= 6) {
                finish("FAIL the placement command kept failing with " + std::to_string(result));
                return;
            }
            ++placement_cell;   /* the next empty cell on the list */
            say("DRIVER: the placement command reported " + std::to_string(result) +
                "; trying again");
            if (sendPlacement()) {
                stage_frame = frame->frame_number;
                return;
            }
            finish("FAIL the placement command could not be resubmitted");
            return;
        }
        case 3: {
            /* The command runs after this frame's callbacks; the instance shows
               up in the board array once it landed. */
            if (frame->frame_number - stage_frame < 60) return;
            target_instance = findTargetInstance();
            if (!target_instance) {
                /* A board that has not recompiled since the placement has no
                   bound instance - and the editor rows only exist for a bound
                   one, so the driver asks for the compile the game's own edit
                   would have caused anyway (component-storage-probe does the
                   same after its level load). */
                if (!compile_requested && frame->frame_number - stage_frame > 240) {
                    compile_requested = true;
                    if (set_sim_test) set_sim_test(model, 0, 1);
                    if (compile_request) {
                        tc::TCNimString progress{};
                        compile_request(model, &progress, 0);
                        say("DRIVER: asked for a compile so the placed component binds");
                    }
                }
                if (frame->frame_number - stage_frame > 900)
                    finish("FAIL the placed editor-demo component never appeared on the board");
                return;
            }
            placement_seen = true;
            say("DRIVER: the component is on the board as instance " + hex64(target_instance));
            trace_once = true;
            const bool selected = selectTarget(target_instance);
            float x = 0.f, y = 0.f;
            if (targetScreenPoint(&x, &y)) {
                moveTo(x, y);
                clickAt(x, y);
                ++body_attempts;
                say("DRIVER: clicked the component body at " + std::to_string((int)x) + "," +
                    std::to_string((int)y) + " (attempt " + std::to_string(body_attempts) +
                    ", board selection " + std::string(selected ? "set" : "not set") + ")");
            } else {
                say("DRIVER: the component's screen position could not be computed");
            }
            stage = 4;
            stage_frame = frame->frame_number;
            return;
        }
        case 4: {
            /* Wait for the loader to report the rows it drew for the selected
               instance.  A sandbox that is still settling drops the first click
               or two, so the body click is retried a few times. */
            if (frame->frame_number - stage_frame < 60) return;
            if (!readRows()) {
                    if (frame->frame_number - stage_frame > 240 && body_attempts < 40) {
                        (void)selectTarget(target_instance);
                        float x = 0.f, y = 0.f;
                        if (targetScreenPoint(&x, &y)) {
                            /* Every few attempts, click a little off the cell
                               centre: a component's hit box is its own rectangle,
                               and a player who misses simply clicks again.  The
                               ring also covers a body that sits off-centre. */
                            static const float kRing[][2] = {
                                {0.f, 0.f},   {-20.f, 0.f}, {20.f, 0.f},  {0.f, -20.f},
                                {0.f, 20.f},  {-40.f, -20.f}, {40.f, 20.f}, {-40.f, 20.f}};
                            const int index = body_attempts % 8;
                            moveTo(x + kRing[index][0], y + kRing[index][1]);
                            clickAt(x + kRing[index][0], y + kRing[index][1]);
                            ++body_attempts;
                        }
                        stage_frame = frame->frame_number;
                        return;
                    }
                if (frame->frame_number - stage_frame > 1200) {
                    std::string list;
                    for (const RowBox& box : rows)
                        list += (list.empty() ? "" : ",") + box.field;
                    finish("FAIL the loader never reported the editor rows (body clicks=" +
                           std::to_string(body_attempts) + " rows=" + list + ")");
                }
                return;
            }
            {
                std::string list;
                for (const RowBox& box : rows)
                    list += (list.empty() ? "" : ", ") + box.field + "=" +
                            std::to_string((int)box.min_x) + "," + std::to_string((int)box.min_y);
                say("DRIVER: the loader's editor rows: " + list);
            }
            if (!rowOf(row_wanted.c_str())) {
                finish("FAIL the editor rows carry no \"" + row_wanted + "\" field");
                return;
            }
            {
                const RowBox* entry = rowOf(row_wanted.c_str());
                const bool same = have_stable && entry->min_x == stable_x0 &&
                                  entry->min_y == stable_y0 && entry->max_x == stable_x1 &&
                                  entry->max_y == stable_y1;
                if (same) {
                    ++stable_reads;
                } else {
                    have_stable = true;
                    stable_reads = 0;
                    stable_x0 = entry->min_x;
                    stable_y0 = entry->min_y;
                    stable_x1 = entry->max_x;
                    stable_y1 = entry->max_y;
                }
            }
            if (stable_reads < 3) {
                stage_frame = frame->frame_number;
                return;
            }
            /* The table can be taller than the space the panel leaves, so the row
               under test may sit outside the window's visible part: bring it into
               view first, the way a player would. */
            stage = 13;
            stage_frame = frame->frame_number;
            return;
        }
        case 13: {
            (void)readRows();
            const RowBox* entry = rowOf(row_wanted.c_str());
            WindowBox win{};
            if (!entry || !readWindowBox(&win)) {
                stage = 5;
                stage_frame = frame->frame_number;
                return;
            }
            const float centre_y = (entry->min_y + entry->max_y) * 0.5f;
            /* The *whole* row has to be inside the window, not just its centre:
               a custom row's resize grip sits in the bottom-right corner, and a
               clipped corner cannot be pressed. */
            /* The row's *centre* has to be on screen; a couple of pixels of
               slack, because the window's own frame can leave the last row's
               bottom a pixel or two past its rectangle.  A custom row's grip is
               not measured against the row (the host publishes and clamps the
               grip itself), so the corner does not have to be inside. */
            if (centre_y > win.y - 3.f && centre_y < win.y + win.h + 3.f) {
                stage = 5;
                stage_frame = frame->frame_number;
                return;
            }
            if (scroll_ticks >= 8) {
                finish("FAIL the " + row_wanted + " row never scrolled into view inside the panel");
                return;
            }
            /* The trace is throttled, so give the host about a second to publish
               the new rectangles before measuring again. */
            if (frame->frame_number - stage_frame < 70) return;
            const bool below = centre_y >= win.y + win.h;
            postWheel(win.x + win.w * 0.5f, win.y + win.h * 0.5f, below ? -1 : 1);
            ++scroll_ticks;
            stage_frame = frame->frame_number;
            return;
        }
        case 5: {
            /* Read once more, so the click uses the rectangle of this frame and
               not the one from when the rows first appeared. */
            (void)readRows();
            const RowBox* entry = rowOf(row_wanted.c_str());
            if (!entry) return;
            gestureBegin(*entry);
            stage = 9;
            stage_frame = frame->frame_number;
            return;
        }
        case 9: {
            (void)readRows();
            const RowBox* entry = rowOf(row_wanted.c_str());
            if (!entry) return;
            if (!gestureTick(*entry)) return;
            if (gesture == Gesture::Click) {
                const float x = (entry->min_x + entry->max_x) * 0.5f;
                const float y = (entry->min_y + entry->max_y) * 0.5f;
                moveTo(x, y);
                say("DRIVER: clicked the " + row_wanted + " row at " +
                    std::to_string((int)x) + "," + std::to_string((int)y));
            } else {
                say("DRIVER: ran the " + row_wanted + " row's gesture");
            }
            if (dialog_sequence) {
                /* The staged batch: the first row is staged, the second row is
                   picked, and the two buttons decide the batch's fate. */
                gesture_attempt = 0;
                gesture_phase = 0;
                gesture_wait = 0;
                haveLastItem = false;
                stage = 20;
                stage_frame = frame->frame_number;
                return;
            }
            if (hover_after) {
                stage = 24;
                stage_frame = frame->frame_number;
                return;
            }
            stage = 6;
            stage_frame = frame->frame_number;
            return;
        }
        case 6: {
            /* A gesture is retried on a slightly different point until the line
               it is supposed to produce shows up; the popup offsets are ImGui's
               business and a single point is not a contract. */
            if (!expect.empty() && !logHas(expect)) {
                if (gesture_attempt + 1 < kMaxGestureAttempts &&
                    frame->frame_number - stage_frame > 20) {
                    ++gesture_attempt;
                    stage = 5;
                    stage_frame = frame->frame_number;
                    return;
                }
                if (frame->frame_number - stage_frame < 240) return;
                finish("FAIL the gesture never produced \"" + expect + "\"");
                return;
            }
            if (keys.empty()) {
                if (frame->frame_number - stage_frame < 120) return;
                finish("PASS the driver clicked the " + row_wanted +
                       " row and the loader's editor committed " + std::to_string(clicks) +
                       " click(s)");
                return;
            }
            if (frame->frame_number - stage_frame < 60) return;
            /* The typed characters are inserted at the caret the click left
               behind: on this build the game's message loop delivers characters
               to the widget but not the editing keys (Home/Delete/Backspace and
               Ctrl+A never arrive), so a driver cannot clear the box first.  The
               playtest therefore asserts that the committed value *carries* what
               was typed rather than that it equals it. */
             for (char value : keys) {
                 sendChar(value);
                 Sleep(60);
             }
             if (fold_test) {
                 say("DRIVER: typed \"" + keys + "\" into the " + row_wanted +
                     " row; the Enter comes after the fold test");
                 stage = 10;
                 stage_frame = frame->frame_number;
                 return;
             }
             sendKey(VK_RETURN, true);
            Sleep(40);
            sendKey(VK_RETURN, false);
            say("DRIVER: typed \"" + keys + "\" into the " + row_wanted + " row and pressed Enter");
             stage = 7;
             stage_frame = frame->frame_number;
             return;
        }
        case 10: {
            /* Wait until the trace really carries the typed text (the trace is
               throttled, so the row's last line is a second old), then click the
               game's own fold switch. */
            (void)readRows();
            const RowBox* entry = rowOf(row_wanted.c_str());
            if (!entry || !entry->buffer_valid || entry->buffer.find(keys) == std::string::npos) {
                if (frame->frame_number - stage_frame > 600) {
                    finish("FAIL the trace never showed the typed text before the fold");
                    return;
                }
                return;
            }
            pre_fold_row = *entry;
            if (!readPanel(&panel)) {
                finish("FAIL the loader never published the drawer's fold button");
                return;
            }
            fold_row_lines_at_press = countLogLines("Component editor: row ");
            const float x = (panel.toggle_min_x + panel.toggle_max_x) * 0.5f;
            const float y = (panel.toggle_min_y + panel.toggle_max_y) * 0.5f;
            clickAt(x, y);
            say("DRIVER: clicked the drawer's fold button at " + std::to_string((int)x) + "," +
                std::to_string((int)y) + " with \"" + pre_fold_row.buffer +
                "\" still in the " + row_wanted + " row");
            stage = 11;
            stage_frame = frame->frame_number;
            return;
        }
        case 11: {
            /* ~2.5 s folded: the trace fires about once a second, so an editor
               that kept drawing would leave row lines in that window. */
            if (frame->frame_number - stage_frame < 150) return;
            fold_rows_drawn = countLogLines("Component editor: row ") - fold_row_lines_at_press;
            /* Read the switch again: the folded drawer lays its title row out
               differently, and clicking the remembered rectangle misses. */
            (void)readPanel(&panel);
            clickAt((panel.toggle_min_x + panel.toggle_max_x) * 0.5f,
                    (panel.toggle_min_y + panel.toggle_max_y) * 0.5f);
            say("DRIVER: clicked the fold button again; rows traced while folded=" +
                std::to_string(fold_rows_drawn));
            stage = 12;
            stage_frame = frame->frame_number;
            return;
        }
        case 12: {
            if (frame->frame_number - stage_frame < 120) return;
            const int lines_now = countLogLines("Component editor: row ");
            (void)readRows();
            const RowBox* entry = rowOf(row_wanted.c_str());
            if (!entry) {
                finish("FAIL the rows did not come back after the drawer was unfolded");
                return;
            }
            if (fold_rows_drawn != 0) {
                finish("FAIL the editor kept drawing while the drawer was folded (" +
                       std::to_string(fold_rows_drawn) + " row lines)");
                return;
            }
            if (lines_now <= fold_row_lines_at_press) {
                finish("FAIL the rows were never redrawn after the drawer was unfolded");
                return;
            }
            const bool same_place = entry->min_x == pre_fold_row.min_x &&
                                    entry->min_y == pre_fold_row.min_y &&
                                    entry->max_x == pre_fold_row.max_x &&
                                    entry->max_y == pre_fold_row.max_y;
            const bool same_text = entry->buffer_valid && entry->buffer == pre_fold_row.buffer;
            if (!same_place) {
                finish("FAIL the rows came back in a different place (" +
                       std::to_string((int)entry->min_x) + "," + std::to_string((int)entry->min_y) +
                       " vs " + std::to_string((int)pre_fold_row.min_x) + "," +
                       std::to_string((int)pre_fold_row.min_y) + ")");
                return;
            }
            if (!same_text) {
                finish("FAIL the typed value was lost across the fold (\"" + entry->buffer +
                       "\" vs \"" + pre_fold_row.buffer + "\")");
                return;
            }
            fold_summary = "PASS the drawer folded with no rows drawn, and unfolding brought the " +
                           row_wanted + " row back at the same rectangle with \"" +
                           entry->buffer + "\" still in it";
            sendKey(VK_RETURN, true);
            Sleep(40);
            sendKey(VK_RETURN, false);
            say("DRIVER: the rows came back unchanged; committing the typed text");
            stage = 7;
            stage_frame = frame->frame_number;
            return;
        }
        case 7:
            if (frame->frame_number - stage_frame < 150) return;
            finish(fold_summary.empty()
                       ? "PASS the driver clicked and typed into the " + row_wanted + " row"
                       : fold_summary);
            return;
        case 20: {
            /* The staged table's second row: an enum picked through its own
               dropdown, the shape a staged choice is edited with. */
            (void)readRows();
            const RowBox* entry = rowOf(row_wanted2.c_str());
            if (!entry) {
                if (frame->frame_number - stage_frame > 600) {
                    finish("FAIL the editor rows carry no \"" + row_wanted2 + "\" field");
                    return;
                }
                return;
            }
            gesture = Gesture::Dropdown;
            gestureBegin(*entry);
            stage = 21;
            stage_frame = frame->frame_number;
            return;
        }
        case 21: {
            (void)readRows();
            const RowBox* entry = rowOf(row_wanted2.c_str());
            if (!entry) return;
            if (!gestureTick(*entry)) return;
            say("DRIVER: picked entry " + std::to_string(choice) + " in the " + row_wanted2 +
                " row");
            stage = 22;
            stage_frame = frame->frame_number;
            return;
        }
        case 22: {
            /* Both rows have to say they staged before a button is pressed: that
               is the whole difference between this table and a V3 one, which
               writes each edit as it happens. */
            if (!logHas("staged field \"" + row_wanted + "\"") ||
                !logHas("staged field \"" + row_wanted2 + "\"")) {
                if (frame->frame_number - stage_frame > 300) {
                    finish("FAIL the staged rows never reported a staged edit");
                    return;
                }
                return;
            }
            DialogBox dialog{};
            if (!readDialog(&dialog)) {
                if (frame->frame_number - stage_frame > 300) {
                    finish("FAIL the loader never published the staged table's buttons");
                    return;
                }
                return;
            }
            const PopupBox& box = dialog_action == "cancel" ? dialog.cancel_box : dialog.apply_box;
            const float x = (box.min_x + box.max_x) * 0.5f;
            const float y = (box.min_y + box.max_y) * 0.5f;
            moveTo(x, y);
            clickAt(x, y);
            say("DRIVER: clicked " + std::string(dialog_action == "cancel" ? "Cancel" : "Apply") +
                " at " + std::to_string((int)x) + "," + std::to_string((int)y) +
                " with pending=" + std::to_string(dialog.pending));
            stage = 23;
            stage_frame = frame->frame_number;
            return;
        }
        case 23: {
            if (frame->frame_number - stage_frame < 150) return;
            if (!expect.empty() && !logHas(expect)) {
                /* The buttons move when the rows re-lay out, so a press that
                   missed is retried from the freshly published rectangle. */
                if (gesture_attempt + 1 < kMaxGestureAttempts &&
                    frame->frame_number - stage_frame > 30) {
                    ++gesture_attempt;
                    stage = 22;
                    stage_frame = frame->frame_number;
                    return;
                }
                finish("FAIL the staged table never produced \"" + expect + "\"");
                return;
            }
            finish("PASS the staged table staged " + row_wanted + " and " + row_wanted2 +
                   " and " + (dialog_action == "cancel" ? "cancelled" : "applied") +
                   " them with one click");
            return;
        }
        case 24: {
            /* A V4 row that is carrying a message publishes its marker's own
               rectangle; resting the pointer on it is what turns the message
               into a tooltip. */
            PopupBox icon{};
            if (!readValidationIcon(row_wanted.c_str(), &icon)) {
                if (frame->frame_number - stage_frame > 300) {
                    finish("FAIL the loader never published a validation marker for the " +
                           row_wanted + " row");
                    return;
                }
                return;
            }
            const float x = (icon.min_x + icon.max_x) * 0.5f;
            const float y = (icon.min_y + icon.max_y) * 0.5f;
            moveTo(x, y);
            say("DRIVER: resting the pointer on the " + row_wanted +
                " row's validation marker at " + std::to_string((int)x) + "," +
                std::to_string((int)y));
            stage = 25;
            stage_frame = frame->frame_number;
            return;
        }
        case 25: {
            if (frame->frame_number - stage_frame < 90) return;
            if (!logHas("Component editor: validation tip") ||
                !logHas("field=\"" + row_wanted + "\"")) {
                finish("FAIL the validation marker's tooltip never reported the message");
                return;
            }
            finish("PASS the " + row_wanted +
                   " row's validation message was read from its marker's tooltip");
            return;
        }
        default:
            return;
    }
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!h->resolve_symbol || !h->engine_proc || !h->query_service) return 2;
    selectComponent = reinterpret_cast<SelectComponent>(
        h->resolve_symbol(h->context, "select_component__modelZboardZboard_u9202"));
    clearSelections = reinterpret_cast<ClearSelections>(
        h->resolve_symbol(h->context, "clear_selections__modelZboardZboard_u8323"));
    /* Through the profile's stable alias, like examples/board-grid: the mangled
       name belongs to the loader's build profile, not to this driver. */
    if (h->resolve_alias) {
        void* found = h->resolve_alias(h->context, "board.world_to_screen");
        std::memcpy(&worldToScreen, &found, sizeof(found));
    }
    getIo = reinterpret_cast<GetIo>(h->engine_proc(h->context, "igGetIO"));
    load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
        h->context, "load_level__modelZutilities_u7740"));
    set_sim_test = reinterpret_cast<SetSimTest>(h->resolve_symbol(
        h->context, "set_sim_test__modelZutilities_u6840"));
    compile_request = reinterpret_cast<CompileRequest>(h->resolve_symbol(
        h->context, "preorder__modelZsimulationZcompile95thread_u3523"));
    add_component = reinterpret_cast<AddComponent>(h->resolve_symbol(
        h->context, "add_component__presenterZutilitiesZhelper95functions_u5918"));
    if (!selectComponent || !clearSelections || !getIo || !load_level) return 4;
    /* The model pointer arrives through the game's own board-input function,
       the way tests/component-placement-probe.cpp captures it. */
    if (!h->create_hook || !mod.game.load(h)) return 5;
    auto* update_target = h->resolve_symbol(
        h->context, "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    if (!update_target) return 6;
    if (h->create_hook(h->context, update_target, reinterpret_cast<void*>(&hookedUpdate),
                       reinterpret_cast<void**>(&update_original)) != 0)
        return 7;
    const std::string requested_row = env("TC_EDITOR_ROW");
    if (!requested_row.empty()) row_wanted = requested_row;
    keys = env("TC_EDITOR_KEYS");
    {
        /* Which V3 shape the row is driven with.  Anything else is the plain
           click the checkbox/text/hexadecimal cases have always used. */
        const std::string requested_gesture = env("TC_EDITOR_GESTURE");
        if (requested_gesture == "double") gesture = Gesture::Double;
        else if (requested_gesture == "dropdown") gesture = Gesture::Dropdown;
        else if (requested_gesture == "menu") gesture = Gesture::Menu;
        else if (requested_gesture == "grip") gesture = Gesture::Grip;
        else if (requested_gesture == "wiggle") gesture = Gesture::Wiggle;
        else if (requested_gesture == "swatch") gesture = Gesture::Swatch;
        else if (requested_gesture == "hover") gesture = Gesture::Hover;
        else if (requested_gesture == "select") gesture = Gesture::SelectText;
        else gesture = Gesture::Click;
        const std::string requested_choice = env("TC_EDITOR_CHOICE");
        if (!requested_choice.empty()) choice = std::atoi(requested_choice.c_str());
        const std::string requested_items = env("TC_EDITOR_ITEMS");
        if (!requested_items.empty()) items = std::atoi(requested_items.c_str());
        expect = env("TC_EDITOR_EXPECT");
    }
    const std::string requested_level = env("TC_EDITOR_LEVEL");
    if (!requested_level.empty()) level = requested_level;
    {
        /* Which of the Mod's three types to place: the settings type is the
           default, "long" is the twenty-row one the scroll case uses and
           "staged" is the table that waits for Apply. */
        const std::string requested_type = env("TC_EDITOR_TYPE");
        if (requested_type == "long") {
            target_id = kLongTargetId;
            if (row_wanted == "enabled") row_wanted = "f0";
        } else if (requested_type == "staged") {
            target_id = kStagedTargetId;
            if (row_wanted == "enabled") row_wanted = "active";
        }
    }
    /* The staged sequence: a second row to stage and the button that ends the
       batch.  Both are absent for every other case. */
    row_wanted2 = env("TC_EDITOR_ROW2");
    dialog_action = env("TC_EDITOR_DIALOG");
    dialog_sequence = !row_wanted2.empty();
    if (dialog_action.empty()) dialog_action = "apply";
    {
        const std::string hover = env("TC_EDITOR_HOVER_AFTER");
        hover_after = !hover.empty() && hover[0] != '0';
    }
    {
        const std::string skip = env("TC_EDITOR_SKIP_LEVEL");
        skip_level = !skip.empty() && skip[0] != '0';
    }
    {
        const std::string fold = env("TC_EDITOR_FOLD");
        fold_test = !fold.empty() && fold[0] != '0';
    }
    log_path = env("TC_EDITOR_LOG");
    if (log_path.empty()) return 8;
    out->on_frame = tick;
    h->log(h->context, "DRIVER: armed");
    return 0;
}
