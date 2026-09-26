/* Test-only driver for the Float Ops editor rows in the game's own component
   drawer (examples/float-ops/components_ui.cpp).

   It does the three things a player does and a script cannot: it selects one of
   the Mod's components through the board's own selection entry point, moves the
   real cursor onto one of the drawer's rows, and clicks it with real mouse
   messages; then it types into whatever the row opened with real keyboard
   messages.  The Mod reports what its rows saw in return
   (`float-ops panel trace: … windowHovered …`, `float-ops editor trace: …`), so
   the playtest can tell "the panel delivered the click to the plugin's widget"
   from "it did not".

   Positions are never guessed: the Mod writes the row rectangles it painted into
   its own plugin-data directory, and this driver reads that file.

   Environment:
     TC_FLOAT_SELECT   constant | add | display | id:0x<custom id>
                       (default constant)
     TC_FLOAT_KEYS     text to type into the opened editor (default "xy")
     TC_FLOAT_ROW      label | rounding | value | info - which of the drawer's
                       rows to click (default label).  A rounding row is clicked
                       on its last radio (RUP), which is the choice no other row
                       can make; the "info" row has no widget to click.
     TC_FLOAT_ROWS     override for the panel-rows.txt path
     TC_FLOAT_BODIES   override for the bodies.txt path (the Mod writes every
                       instance's body rectangle when TC_FLOATOPS_BODIES is set)
     TC_FLOAT_PALETTE  when set, the run drives the Mod's own component palette
                       instead of the drawer: the value is the row index to
                       click, read from palette-rows.txt (the Mod rewrites that
                       file while its palette is on screen).
*/
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_handle_api.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kConstantId = UINT64_C(0x463332434f4e5331); /* F32CONS1 */
constexpr uint64_t kAddId = UINT64_C(0x4633324144445f31);      /* F32ADD_1 */
constexpr uint64_t kDisplayId = UINT64_C(0x4633324449535031);  /* F32DISP1 */

constexpr uint64_t kComponentStride = 0x238, kRecordHeader = 8;
constexpr uint32_t kCustomComponentKind = 0x4e;

/* The board object tables, read exactly like src/board_objects.hpp does (the
   loader's own fingerprint and every placement probe use the same offsets). */
struct BoardArrays {
    uint64_t components = 0;
    const unsigned char* data = nullptr;
};

const TCHost* host = nullptr;
HWND window = nullptr;
using SelectFn = void (*)(int64_t index, void* record);
using ClearSelectionsFn = void (*)();
SelectFn selectComponent = nullptr;
ClearSelectionsFn clearSelections = nullptr;
using GetIoFn = void* (*)();
GetIoFn getIo = nullptr;

struct RowBox {
    std::string kind;
    float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;
    bool valid = false;
    /* The row's field alone (input box or radio group); empty when the Mod
       reported only the row. */
    float field_min_x = 0.f, field_min_y = 0.f, field_max_x = 0.f, field_max_y = 0.f;
    bool field_valid = false;
    float choice_min_x = 0.f, choice_min_y = 0.f, choice_max_x = 0.f, choice_max_y = 0.f;
    bool choice_valid = false;
};

bool started = false, finished = false, selected = false;
bool hover_logged = false;
int body_click_attempts = 0;
bool body_click = true;
int palette_row = -1;
int start_frame = 0, stage = 0, stage_frame = 0, clicks_sent = 0;
std::string target = "constant";
std::string keys = "xy";
std::string row_kind = "label";
uint64_t target_custom_id = 0;
std::string report_path;
std::string bodies_path;
uint64_t target_instance = 0;
std::vector<RowBox> rows;
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

uint64_t targetId() {
    if (target_custom_id) return target_custom_id;
    if (target == "add") return kAddId;
    if (target == "display") return kDisplayId;
    return kConstantId;
}

/* <game>/tc-modloader-data/plugin-data/<this mod> -> <game>/tc-modloader-data */
std::string dataRoot() {
    std::string dir = host && host->data_directory_utf8 ? host->data_directory_utf8 : "";
    for (int level = 0; level < 2; ++level) {
        const size_t cut = dir.find_last_of("\\/");
        if (cut == std::string::npos) break;
        dir.resize(cut);
    }
    return dir;
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

bool readRows() {
    if (report_path.empty()) return false;
    std::FILE* file = std::fopen(report_path.c_str(), "rb");
    if (!file) return false;
    std::vector<RowBox> parsed;
    char line[256] = {};
    while (std::fgets(line, sizeof(line), file)) {
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        char kind[64] = {};
        if (std::sscanf(line, "field %63s %f %f %f %f", kind, &x0, &y0, &x1, &y1) == 5) {
            for (RowBox& box : parsed) {
                if (box.kind != kind) continue;
                box.field_min_x = x0;
                box.field_min_y = y0;
                box.field_max_x = x1;
                box.field_max_y = y1;
                box.field_valid = x1 > x0 && y1 > y0;
                break;
            }
            continue;
        }
        if (std::sscanf(line, "choice %63s %f %f %f %f", kind, &x0, &y0, &x1, &y1) == 5) {
            for (RowBox& box : parsed) {
                if (box.kind != kind) continue;
                box.choice_min_x = x0;
                box.choice_min_y = y0;
                box.choice_max_x = x1;
                box.choice_max_y = y1;
                box.choice_valid = x1 > x0 && y1 > y0;
                break;
            }
            continue;
        }
        if (std::sscanf(line, "row %63s %f %f %f %f", kind, &x0, &y0, &x1, &y1) == 5) {
            RowBox box;
            box.kind = kind;
            box.min_x = x0;
            box.min_y = y0;
            box.max_x = x1;
            box.max_y = y1;
            box.valid = x1 > x0 && y1 > y0;
            parsed.push_back(box);
        }
    }
    std::fclose(file);
    if (parsed.empty()) return false;
    rows = parsed;
    return true;
}

const RowBox* row(const char* kind) {
    for (const RowBox& box : rows)
        if (box.kind == kind && box.valid) return &box;
    return nullptr;
}

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
    ++clicks_sent;
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

/* The board's own selection setter.  On this build the game opens its component
   drawer when the selection changes, and a synthetic click on the body does not
   always reach the board's input (measured: the same click works while the
   board is settling and is dropped on other frames), so the driver uses both:
   the setter first, the click as the retry. */
bool selectTarget(uint64_t instance) {
    BoardArrays arrays{};
    if (!boardArrays(&arrays) || !selectComponent || !clearSelections || !instance)
        return false;
    for (uint64_t index = 0; index < arrays.components; ++index) {
        unsigned char* record =
            const_cast<unsigned char*>(arrays.data) + kRecordHeader + index * kComponentStride;
        uint64_t id = 0;
        std::memcpy(&id, record + 8, sizeof(id));
        if (id != instance) continue;
        clearSelections();
        selectComponent(static_cast<int64_t>(id), record);
        return true;
    }
    return false;
}

/* The instance the board scan found, so the driver can click it the way the
   player does: a real click on the body, which is what opens the game's own
   component drawer (selection through the board's setter does not draw the
   panel on its own - measured). */
uint64_t findTargetInstance() {
    BoardArrays arrays{};
    if (!boardArrays(&arrays)) return 0;
    const uint64_t wanted = targetId();
    for (uint64_t index = 0; index < arrays.components; ++index) {
        const unsigned char* record =
            arrays.data + kRecordHeader + index * kComponentStride;
        if (record[0] != kCustomComponentKind) continue;
        uint64_t id = 0, custom = 0;
        std::memcpy(&id, record + 8, sizeof(id));
        std::memcpy(&custom, record + 0x188, sizeof(custom));
        if (custom == wanted) return id;
    }
    return 0;
}

/* The body rectangle the Mod painted for one instance, in game coordinates. */
bool readBodyBox(uint64_t instance, RowBox* out) {
    if (bodies_path.empty()) return false;
    std::FILE* file = std::fopen(bodies_path.c_str(), "rb");
    if (!file) return false;
    char line[256] = {};
    bool found = false;
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long id = 0;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        if (std::sscanf(line, "body 0x%llx %f %f %f %f", &id, &x0, &y0, &x1, &y1) != 5)
            continue;
        if (id != instance) continue;
        out->kind = "body";
        out->min_x = x0;
        out->min_y = y0;
        out->max_x = x1;
        out->max_y = y1;
        out->valid = x1 > x0 && y1 > y0;
        found = out->valid;
        break;
    }
    std::fclose(file);
    return found;
}

/* One row of the Mod's own palette (the board side panel it registers because
   the game's right-hand column cannot take a Mod category). */
bool readPaletteRow(int index, RowBox* out) {
    if (bodies_path.empty()) return false;
    std::string path = bodies_path;
    const size_t cut = path.find_last_of("\\/");
    if (cut == std::string::npos) return false;
    path.resize(cut + 1);
    path += "palette-rows.txt";
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    char line[256] = {};
    bool found = false;
    while (std::fgets(line, sizeof(line), file)) {
        int row = -1;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        if (std::sscanf(line, "row %d %f %f %f %f", &row, &x0, &y0, &x1, &y1) != 5)
            continue;
        if (row != index) continue;
        out->kind = "palette";
        out->min_x = x0;
        out->min_y = y0;
        out->max_x = x1;
        out->max_y = y1;
        out->valid = x1 > x0 && y1 > y0;
        found = out->valid;
        break;
    }
    std::fclose(file);
    return found;
}

void tick(void*, const TCFrame* frame) {
    if (!frame || finished) return;
    if (!started) {
        started = true;
        start_frame = frame->frame_number;
        window = findWindow();
        if (window) {
            /* The game reads its mouse position from the real cursor, so the
               window sits at the desktop origin for the duration of the run. */
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
        say("DRIVER: started frame=" + std::to_string(frame->frame_number) +
            " window=" + std::to_string(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(window))) +
            " client=" + std::to_string(client_width) + "x" + std::to_string(client_height) +
            " scale=" + std::to_string(scale_x) + "x" + std::to_string(scale_y) +
            " target=" + target + " rows=" + report_path);
    }
    if (frame->frame_number - start_frame > 0 && (frame->frame_number - start_frame) % 600 == 0)
        say("DRIVER: alive stage=" + std::to_string(stage) + " selected=" +
            std::to_string(selected ? 1 : 0) + " clicks=" + std::to_string(clicks_sent));
    if (frame->frame_number - stage_frame < 0) return;

    switch (stage) {
        case 0:
            /* The palette run needs no selection: the panel is on the board as
               soon as the board is, and its rows are what the driver clicks. */
            if (palette_row >= 0) {
                if (frame->frame_number - start_frame < 240) return;
                {
                    RowBox row;
                    if (!readPaletteRow(palette_row, &row)) {
                        if (frame->frame_number - start_frame > 900)
                            say("DRIVER: the Mod never reported its palette rows");
                        return;
                    }
                    const float x = (row.min_x + row.max_x) * 0.5f;
                    const float y = (row.min_y + row.max_y) * 0.5f;
                    clickAt(x, y);
                    moveTo(x, y);
                    say("DRIVER: clicked palette row " + std::to_string(palette_row) +
                        " at " + std::to_string(static_cast<int>(x)) + "," +
                        std::to_string(static_cast<int>(y)));
                }
                stage = 5;
                stage_frame = frame->frame_number;
                return;
            }
            /* The board has to be up before its component array can be read;
               the level's own driver brings it there.  The target is only
               *found* here - selecting it is the body click below, because the
               game draws its component drawer for a component the player
               clicked, not for one the Mod's own selection call picked. */
            if (frame->frame_number - start_frame < 240) return;
            target_instance = findTargetInstance();
            if (!target_instance) return;
            selected = true;
            {
                const bool chosen = selectTarget(target_instance);
                say("DRIVER: the target instance is 0x" +
                    std::to_string(static_cast<unsigned long long>(target_instance)) +
                    (chosen ? " (selected through the board)" : " (selection call failed)"));
            }
            stage = 1;
            stage_frame = frame->frame_number;
            return;
        case 1:
            /* The player's own way in: click the body.  The game's selection
               setter alone does not make it draw its bottom panel (measured on
               this build), while a real click on the body selects the component
               and opens the drawer exactly as it does in play. */
            if (!body_click) {
                stage = 2;
                stage_frame = frame->frame_number;
                return;
            }
            if (!target_instance) {
                say("DRIVER: the board has no instance of the requested type");
                finished = true;
                return;
            }
            {
                RowBox body;
                if (!readBodyBox(target_instance, &body)) {
                    if (frame->frame_number - stage_frame > 900)
                        say("DRIVER: the Mod never reported the target's body");
                    return;
                }
                const float x = (body.min_x + body.max_x) * 0.5f;
                const float y = (body.min_y + body.max_y) * 0.5f;
                if (frame->frame_number - stage_frame < 30) {
                    moveTo(x, y);
                    return;
                }
                clickAt(x, y);
                moveTo(x, y);
                ++body_click_attempts;
                say("DRIVER: clicked the component body at " +
                    std::to_string(static_cast<int>(x)) + "," +
                    std::to_string(static_cast<int>(y)) + " (attempt " +
                    std::to_string(body_click_attempts) + ")");
            }
            stage = 2;
            stage_frame = frame->frame_number;
            return;
        case 2:
            if (frame->frame_number - stage_frame < 30) return;
            if (!readRows()) {
                /* The game only opens its drawer while it is ready for board
                   input, and a sandbox that is still settling drops the first
                   click or two - measured.  Click again instead of giving up. */
                if (frame->frame_number - stage_frame > 150 && body_click_attempts < 8) {
                    (void)selectTarget(target_instance);
                    stage = 1;
                    stage_frame = frame->frame_number;
                    return;
                }
                if (frame->frame_number - stage_frame > 900)
                    say("DRIVER: the Mod never reported its panel rows");
                return;
            }
            {
                std::string list;
                for (const RowBox& box : rows)
                    list += (list.empty() ? "" : ", ") + box.kind + "=" +
                            std::to_string(static_cast<int>(box.min_x)) + "," +
                            std::to_string(static_cast<int>(box.min_y));
                say("DRIVER: the Mod's panel rows: " + list);
            }
            if (!row(row_kind.c_str())) {
                if (frame->frame_number - stage_frame > 900)
                    say("DRIVER: the drawer never reported the " + row_kind + " row");
                return;
            }
            stage = 3;
            stage_frame = frame->frame_number;
            return;
        case 3: {
            const RowBox* entry = row(row_kind.c_str());
            if (!entry) return;
            /* The field, not the row: the label text next to it is not a widget,
               and what the player has to be able to reach is the input box. */
            const float min_x = entry->field_valid ? entry->field_min_x : entry->min_x;
            const float max_x = entry->field_valid ? entry->field_max_x : entry->max_x;
            const float min_y = entry->field_valid ? entry->field_min_y : entry->min_y;
            const float max_y = entry->field_valid ? entry->field_max_y : entry->max_y;
            /* An input field is clicked in its middle; a rounding row is clicked
               on its last radio (RUP), which is the choice that proves the
               write-back reached the instance, so the Mod reports that radio's
               own box. */
            float x = (min_x + max_x) * 0.5f;
            float y = (min_y + max_y) * 0.5f;
            if (entry->choice_valid) {
                x = (entry->choice_min_x + entry->choice_max_x) * 0.5f;
                y = (entry->choice_min_y + entry->choice_max_y) * 0.5f;
            }
            if (frame->frame_number - stage_frame < 40) {
                moveTo(x, y);
                if (!hover_logged) {
                    hover_logged = true;
                    say("DRIVER: hovering the " + row_kind + " field at " +
                        std::to_string(static_cast<int>(x)) + "," +
                        std::to_string(static_cast<int>(y)));
                }
                return;
            }
            clickAt(x, y);
            say("DRIVER: clicked the " + row_kind + " field");
            /* The pointer is the game's mouse: a person moving the real mouse
               during the run would otherwise take the hover (never the focus)
               away mid-typing. */
            moveTo(x, y);
            if (row_kind != "label" && row_kind != "value") {
                /* A radio commits on the click itself; there is nothing to
                   type, so the run is over after the write-back has had a few
                   frames to reach the log. */
                stage = 5;
                stage_frame = frame->frame_number;
                return;
            }
            stage = 4;
            stage_frame = frame->frame_number;
            return;
        }
        case 4:
            /* The row's field took the click, so the player's next action is
               typing into it.  The pointer is held on the field so a person
               moving the real mouse cannot take the hover off it mid-word. */
            if (frame->frame_number - stage_frame < 60) return;
            for (char value : keys) {
                sendChar(value);
                Sleep(60);
            }
            sendKey(VK_RETURN, true);
            Sleep(40);
            sendKey(VK_RETURN, false);
            say("DRIVER: typed \"" + keys + "\" and pressed Enter");
            stage = 5;
            stage_frame = frame->frame_number;
            return;
        case 5:
            if (frame->frame_number - stage_frame < 90) return;
            say("DRIVER: done");
            finished = true;
            return;
        default:
            return;
    }
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!h->resolve_symbol || !h->engine_proc) return 2;
    selectComponent = reinterpret_cast<SelectFn>(
        h->resolve_symbol(h->context, "select_component__modelZboardZboard_u9202"));
    clearSelections = reinterpret_cast<ClearSelectionsFn>(
        h->resolve_symbol(h->context, "clear_selections__modelZboardZboard_u8323"));
    getIo = reinterpret_cast<GetIoFn>(h->engine_proc(h->context, "igGetIO"));
    if (!selectComponent || !clearSelections) return 3;
    const std::string requested = env("TC_FLOAT_SELECT");
    if (!requested.empty()) {
        if (requested.rfind("id:", 0) == 0) {
            target_custom_id = std::strtoull(requested.c_str() + 3, nullptr, 16);
            target = "id:0x" + std::to_string(target_custom_id);
            if (!target_custom_id) return 4;
        } else {
            target = requested;
        }
    }
    const std::string typed = env("TC_FLOAT_KEYS");
    if (!typed.empty()) keys = typed;
    const std::string rowRequested = env("TC_FLOAT_ROW");
    if (!rowRequested.empty()) row_kind = rowRequested;
    {
        const std::string click = env("TC_FLOAT_BODY_CLICK");
        if (!click.empty() && click[0] == '0') body_click = false;
    }
    {
        const std::string palette = env("TC_FLOAT_PALETTE");
        if (!palette.empty()) palette_row = std::atoi(palette.c_str());
    }
    report_path = env("TC_FLOAT_ROWS");
    if (report_path.empty())
        report_path = dataRoot() + "\\plugin-data\\local.float-ops\\panel-rows.txt";
    bodies_path = env("TC_FLOAT_BODIES");
    if (bodies_path.empty())
        bodies_path = dataRoot() + "\\plugin-data\\local.float-ops\\bodies.txt";
    out->on_frame = tick;
    h->log(h->context, "DRIVER: armed");
    return 0;
}
