/* Test-only driver for the Float Ops "leave the board and come back" path.

   The player report this case exists for (2026-09-26): "进入沙盒后退出再重进，
   会发现选中元件之后面板的相关配置不会渲染" and "同 2 重进的时候，常量值输出
   变为默认".  A level that is left and entered again hands the new board the
   *same* component ids, so the two things that can go wrong are silent:

     * the drawer's editor kept the rows it filled on the board that is gone, and
       nothing made it re-read the record;
     * the saved configuration was restored only when something read it, and the
       paused board was never re-evaluated after the restore, so a Constant went
       on publishing its default.

   The case needs a real level lifetime, which only the game can produce: it
   starts in the level's sandbox (the playtest writes setting_current_level), edits
   one FP32 Constant's value through the game's own drawer with real mouse and
   keyboard input, leaves the level with the player's own exit (Escape), presses
   the campaign screen's own level entry again, and then - *without touching a
   field* - selects the Constant so the drawer opens on it.

   Everything it reports is a line either the Mod or this driver writes, so the
   playtest can assert the sequence instead of trusting a screenshot.

   Environment:
     TC_FLOAT_REENTRY_SELECT   constant | add | display | id:0x<custom id>
     TC_FLOAT_REENTRY_KEYS     text typed into the row's field (default "2.5")
     TC_FLOAT_REENTRY_ROW      label | value (default value)
     TC_FLOAT_REENTRY_LEVEL    level name for the load_level fallback
     TC_FLOAT_REENTRY_ENTRY    "0" skips the campaign entry press (fallback only)
     TC_FLOAT_REENTRY_BUTTON   which invisible button of the current screen is the
                               level entry (default 2, the "capture" block the
                               enter-board driver presses)
     TC_FLOAT_REENTRY_SETTLE   seconds to wait after a board is up (default 4)
     TC_FLOAT_ROWS             override for the panel-rows.txt path
     TC_FLOAT_BODIES           override for the bodies.txt path
*/
#include "../sdk/tc_event.h"
#include "../sdk/tc_game_model.h"
#include "../sdk/tc_handle_api.h"
#include "../sdk/tc_mod_api.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kConstantId = UINT64_C(0x463332434f4e5331); /* F32CONS1 */
constexpr uint64_t kAddId = UINT64_C(0x4633324144445f31);      /* F32ADD_1 */
constexpr uint64_t kDisplayId = UINT64_C(0x4633324449535031);  /* F32DISP1 */

constexpr uint64_t kComponentStride = 0x238, kRecordHeader = 8;
constexpr uint32_t kCustomComponentKind = 0x4e;
/* The screens' own level blocks are drawn from the same call site the enter-board
   driver measures; tests/game-handle-probe pressed rva=0x44a308 of that window
   for both of its visits. */

struct V2 {
    float x, y;
};

struct BoardArrays {
    uint64_t components = 0;
    const unsigned char* data = nullptr;
};

const TCHost* host = nullptr;
HWND window = nullptr;
using SelectFn = void (*)(int64_t index, void* record);
using ClearSelectionsFn = void (*)();
using GetIoFn = void* (*)();
using ChangeSceneFn = void (*)(void*, int);
using LoadLevelFn = void (*)(void*, const tc::TCNimString*);
using InvisibleFn = bool (*)(const char*, V2, int);
SelectFn selectComponent = nullptr;
ClearSelectionsFn clearSelections = nullptr;
GetIoFn getIo = nullptr;
ChangeSceneFn changeScene = nullptr;
LoadLevelFn loadLevel = nullptr;
InvisibleFn invisibleOriginal = nullptr;

struct RowBox {
    std::string kind;
    float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;
    bool valid = false;
    float field_min_x = 0.f, field_min_y = 0.f, field_max_x = 0.f, field_max_y = 0.f;
    bool field_valid = false;
};

bool started = false, finished = false;
int start_frame = 0, stage = 0, stage_frame = 0, clicks_sent = 0;
int level_loads = 0, scene_changes = 0;
int loads_at_exit = -1, scenes_before_step = -1;
double start_time = 0.0, elapsed = 0.0, stage_time = 0.0;
bool fallback_entry = false;
bool press_entry = true;
double settle = 4.0;
int entry_button = 2;
/* Armed while a level entry has to be pressed; the hook presses candidate
   `entry_button` of whatever screen is drawing buttons and disarms itself. */
bool entry_armed = false;
double entry_armed_at = 0.0;
int entry_count = 0, entry_frame = -1, entry_presses = 0, entry_screens = 0;
uintptr_t entry_site = 0;
void* board_context = nullptr;
std::string target = "constant", keys = "2.5", row_kind = "value", level_name;
uint64_t target_custom_id = 0, target_instance = 0;
std::string report_path, bodies_path;
std::vector<RowBox> rows;
float scale_x = 1.f, scale_y = 1.f;
int client_width = 0, client_height = 0;
tc::TCGameModel game;
bool game_loaded = false;

void say(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

std::string hex(uintptr_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
    return text;
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

/* The panel rows the Mod painted, read from the file it rewrites (the same recipe
   tests/float-panel-driver.cpp uses).  The header carries the instance and the
   frame the rows were written for, which is what tells a re-entered board's rows
   apart from the ones the board that is gone had left there. */
struct PanelFile {
    uint64_t instance = 0;
    uint64_t type = 0;
    int frame = -1;
    int row_count = -1;
    std::vector<RowBox> rows;
};

bool readPanelFile(PanelFile* out) {
    if (report_path.empty()) return false;
    std::FILE* file = std::fopen(report_path.c_str(), "rb");
    if (!file) return false;
    PanelFile parsed;
    char line[256] = {};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long value = 0;
        int number = 0;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        char kind[64] = {};
        if (std::sscanf(line, "instance 0x%llx", &value) == 1) {
            parsed.instance = value;
            continue;
        }
        if (std::sscanf(line, "type 0x%llx", &value) == 1) {
            parsed.type = value;
            continue;
        }
        if (std::sscanf(line, "frame %d", &number) == 1) {
            parsed.frame = number;
            continue;
        }
        if (std::sscanf(line, "rows %d", &number) == 1) {
            parsed.row_count = number;
            continue;
        }
        if (std::sscanf(line, "field %63s %f %f %f %f", kind, &x0, &y0, &x1, &y1) == 5) {
            for (RowBox& box : parsed.rows) {
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
        if (std::sscanf(line, "row %63s %f %f %f %f", kind, &x0, &y0, &x1, &y1) == 5) {
            RowBox box;
            box.kind = kind;
            box.min_x = x0;
            box.min_y = y0;
            box.max_x = x1;
            box.max_y = y1;
            box.valid = x1 > x0 && y1 > y0;
            parsed.rows.push_back(box);
        }
    }
    std::fclose(file);
    if (parsed.frame < 0 || parsed.instance == 0) return false;
    *out = parsed;
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

/* GLFW derives the key from the scancode, not from the virtual key, so the
   message has to carry one (tests/ui-keyboard-driver.hpp measures the same). */
void pressKey(int virtual_key) {
    if (!window) return;
    SetForegroundWindow(window);
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(virtual_key), MAPVK_VK_TO_VSC);
    const LPARAM down = 1 | (static_cast<LPARAM>(scan & 0xff) << 16);
    const LPARAM up = down | (1LL << 30) | (1LL << 31);
    PostMessageW(window, WM_KEYDOWN, virtual_key, down);
    Sleep(30);
    PostMessageW(window, WM_KEYUP, virtual_key, up);
    Sleep(30);
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

uint64_t findTargetInstance() {
    BoardArrays arrays{};
    if (!boardArrays(&arrays)) return 0;
    const uint64_t wanted = targetId();
    for (uint64_t index = 0; index < arrays.components; ++index) {
        const unsigned char* record = arrays.data + kRecordHeader + index * kComponentStride;
        if (record[0] != kCustomComponentKind) continue;
        uint64_t id = 0, custom = 0;
        std::memcpy(&id, record + 8, sizeof(id));
        std::memcpy(&custom, record + 0x188, sizeof(custom));
        if (custom == wanted) return id;
    }
    return 0;
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
        selectComponent(static_cast<int64_t>(id), record);
        return true;
    }
    return false;
}

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

/* Clicks the component the way the player does: a real click on the body, which
   is what makes the game draw its own drawer for it. */
bool clickBody(uint64_t instance, const char* what) {
    RowBox body;
    if (!readBodyBox(instance, &body)) return false;
    const float x = (body.min_x + body.max_x) * 0.5f;
    const float y = (body.min_y + body.max_y) * 0.5f;
    clickAt(x, y);
    moveTo(x, y);
    say(std::string("DRIVER: clicked the component body (") + what + ") at " +
        std::to_string(static_cast<int>(x)) + "," + std::to_string(static_cast<int>(y)) +
        " (attempt " + std::to_string(clicks_sent) + ")");
    return true;
}

/* The game's own exit from a level.  The loader's scene.change detour runs in
   front of the game's function, so the board handle is invalidated with it. */
void beginSelfExit() {
    scenes_before_step = scene_changes;
    say("DRIVER: trying the game's own way out (Escape)");
    pressKey(VK_ESCAPE);
}

bool leftSinceStep() {
    return scene_changes > scenes_before_step;
}

/* Arms the level entry press.  The game draws one invisible button per level
   block on its own screens, and the enter-board driver's recipe - press the
   Nth button of a freshly drawn screen a couple of seconds later - is what
   reaches the level from either the main menu or the campaign screen.  The
   hook below logs what it saw, so a miss names the buttons that were there. */
void pressEntry(const char* why) {
    entry_armed = true;
    entry_armed_at = elapsed;
    entry_frame = -1;
    entry_count = 0;
    say(std::string("DRIVER: pressing the level entry (button #") +
        std::to_string(entry_button) + ") " + why);
}

void loadLevelByName() {
    if (!loadLevel || !game_loaded || !board_context) {
        say("DRIVER: the game's own load_level is unavailable");
        return;
    }
    const std::string name = level_name.empty() ? std::string("sandbox") : level_name;
    tc::TCNimString level{};
    const size_t length = name.size();
    game.raw_new_string(&level, static_cast<int64_t>(length));
    if (!level.data) {
        say("DRIVER: cannot allocate the level name");
        return;
    }
    std::memcpy(static_cast<unsigned char*>(level.data) + 8, name.data(), length);
    level.length = length;
    static_cast<unsigned char*>(level.data)[8 + length] = 0;
    say("DRIVER: asking the game to load the level '" + name + "' by name");
    loadLevel(board_context, &level);
}

bool hookInvisible(const char* id, V2 size, int flags) {
    const bool result = invisibleOriginal ? invisibleOriginal(id, size, flags) : false;
    if (!entry_armed) return result;
    const uintptr_t rva = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                          reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    /* The same window the enter-board driver measures the level blocks in: the
       screens' own buttons.  A button drawn from anywhere else (the board's own
       toolbar, a Mod's row) is never the level entry and is left alone. */
    if (rva < 0x449df0u || rva >= 0x44b610u) return result;
    /* Fresh count per frame: the button order is the game's draw order, and only
       that order is stable across the two screens this case walks through. */
    const int frame = static_cast<int>(elapsed * 60.0) + 1;
    if (frame != entry_frame) {
        /* One line per arming: what the screen of that moment looked like is the
           first thing to read when a press does not reach a level. */
        if (entry_count && entry_screens < 2) {
            ++entry_screens;
            say("DRIVER: the screen drew " + std::to_string(entry_count) +
                " button(s) while the entry was armed (last rva=0x" + hex(rva) + " id=\"" +
                (id ? id : "") + "\")");
        }
        entry_frame = frame;
        entry_count = 0;
    }
    ++entry_count;
    if (entry_count != entry_button) return result;
    if (elapsed - entry_armed_at < 2.5) return result;
    ++entry_presses;
    entry_armed = false;
    entry_site = rva;
    say("DRIVER: pressed the level entry (button #" + std::to_string(entry_button) +
        ", rva=0x" + hex(rva) + ", id=\"" + (id ? id : "") + "\")");
    return true;
}

void onEvent(TCEvent* event) {
    if (tc::events::is(event, TC_EVENT_LEVEL_LOAD)) {
        ++level_loads;
        /* The level's own name, copied here: it is what the load_level fallback
           has to ask for to come back to the board that was just left. */
        if (const char* name = tc::events::levelName(event))
            if (*name) level_name = name;
        return;
    }
    if (tc::events::is(event, TC_EVENT_SCENE_CHANGE)) {
        ++scene_changes;
        /* The context the game itself passes to change_scene.  It arrives with
           the entry into the board scene, which is before any fallback needs it,
           and the same value comes back with the exit. */
        if (void* context = tc::events::sceneContext(event)) board_context = context;
    }
}

/* The instance the drawer was showing when the rows were written, together with
   the frame: a stale file left by the board that is gone names the same instance
   id (the game reuses it) but not the same frame. */
void reportPanelFile(const char* what) {
    PanelFile file;
    if (!readPanelFile(&file)) {
        say(std::string("DRIVER: no panel rows were reported ") + what);
        return;
    }
    rows = file.rows;
    std::string list;
    for (const RowBox& box : file.rows)
        list += (list.empty() ? "" : ", ") + box.kind;
    say(std::string("DRIVER: panel rows ") + what + ": instance 0x" +
        hex(static_cast<uintptr_t>(file.instance)) + " type 0x" +
        hex(static_cast<uintptr_t>(file.type)) + " frame=" + std::to_string(file.frame) +
        " rows=" + std::to_string(file.row_count) + " [" + list + "]");
}

void tick(void*, const TCFrame* frame) {
    if (!frame || finished) return;
    if (!started) {
        started = true;
        start_frame = frame->frame_number;
        start_time = frame->time_seconds;
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
        say("DRIVER: started frame=" + std::to_string(frame->frame_number) +
            " client=" + std::to_string(client_width) + "x" + std::to_string(client_height) +
            " target=" + target + " keys=\"" + keys + "\" row=" + row_kind);
    }
    elapsed = frame->time_seconds - start_time;
    if ((frame->frame_number - start_frame) % 600 == 0 && frame->frame_number != start_frame)
        say("DRIVER: alive stage=" + std::to_string(stage) + " loads=" +
            std::to_string(level_loads) + " scenes=" + std::to_string(scene_changes) +
            " clicks=" + std::to_string(clicks_sent));
    if (elapsed > 190.0) {
        finished = true;
        say("DRIVER: giving up after 190s at stage " + std::to_string(stage) +
            " (loads=" + std::to_string(level_loads) + " scenes=" +
            std::to_string(scene_changes) + ")");
        return;
    }
    if (frame->frame_number - stage_frame < 0) return;

    switch (stage) {
        case 0:
            /* The game is on its own start screen; the case enters the level the
               way the player does (the screen's own level block). */
            if (elapsed < 4.0) return;
            pressEntry("to reach the level");
            stage = 12;
            stage_frame = frame->frame_number;
            return;
        case 12:
            /* Board A: the fixture's own component is the proof that the level
               came up. */
            target_instance = findTargetInstance();
            if (!target_instance) {
                /* The press may have been swallowed by a screen the game rebuilt
                   between the arming and the button: try it again while nothing
                   has been pressed yet. */
                if (!entry_armed && entry_presses == 0 &&
                    frame->frame_number - stage_frame > 720) {
                    say("DRIVER: no level came up after the entry press; pressing it again");
                    pressEntry("to reach the level (retry)");
                    stage_frame = frame->frame_number;
                    return;
                }
                if (elapsed > 90.0) {
                    finished = true;
                    say("DRIVER: the board never showed the requested component");
                }
                return;
            }
            say("DRIVER: board A is up; the target instance is 0x" +
                hex(static_cast<uintptr_t>(target_instance)));
            stage = 1;
            stage_frame = frame->frame_number;
            return;
        case 1:
            /* The player's way into the drawer: a click on the body. */
            if (frame->frame_number - stage_frame < 30) return;
            (void)selectTarget(target_instance);
            if (!clickBody(target_instance, "before leaving")) {
                if (frame->frame_number - stage_frame > 900)
                    say("DRIVER: the Mod never reported the target's body");
                return;
            }
            stage = 2;
            stage_frame = frame->frame_number;
            return;
        case 2: {
            if (frame->frame_number - stage_frame < 45) return;
            PanelFile file;
            if (!readPanelFile(&file)) {
                /* The sandbox drops the first click or two while it settles. */
                if (frame->frame_number - stage_frame > 150) {
                    (void)selectTarget(target_instance);
                    clickBody(target_instance, "retry");
                    stage_frame = frame->frame_number;
                    return;
                }
                return;
            }
            rows = file.rows;
            reportPanelFile("before leaving");
            if (!row(row_kind.c_str())) {
                if (frame->frame_number - stage_frame > 900)
                    say("DRIVER: the drawer never reported the " + row_kind + " row");
                return;
            }
            stage = 3;
            stage_frame = frame->frame_number;
            return;
        }
        case 3: {
            const RowBox* entry = row(row_kind.c_str());
            if (!entry) return;
            const float x = entry->field_valid ? (entry->field_min_x + entry->field_max_x) * 0.5f
                                               : (entry->min_x + entry->max_x) * 0.5f;
            const float y = entry->field_valid ? (entry->field_min_y + entry->field_max_y) * 0.5f
                                               : (entry->min_y + entry->max_y) * 0.5f;
            if (frame->frame_number - stage_frame < 40) {
                moveTo(x, y);
                return;
            }
            clickAt(x, y);
            moveTo(x, y);
            say("DRIVER: clicked the " + row_kind + " field");
            stage = 4;
            stage_frame = frame->frame_number;
            return;
        }
        case 4:
            if (frame->frame_number - stage_frame < 60) return;
            for (char value : keys) {
                sendChar(value);
                Sleep(60);
            }
            pressKey(VK_RETURN);
            say("DRIVER: typed \"" + keys + "\" and pressed Enter");
            stage = 5;
            stage_frame = frame->frame_number;
            stage_time = elapsed;
            return;
        case 5:
            /* Let the write, the paused-board refresh and the circuit save land
               before the level is left: the saved record is what the second visit
               has to come back with. */
            if (elapsed < stage_time + settle + 2.0) return;
            beginSelfExit();
            stage = 6;
            stage_frame = frame->frame_number;
            stage_time = elapsed;
            return;
        case 6:
            if (leftSinceStep()) {
                say("DRIVER: left the level by itself; the old board handle is gone");
                loads_at_exit = level_loads;
                stage = 7;
                stage_frame = frame->frame_number;
                stage_time = elapsed;
                return;
            }
            if (elapsed > stage_time + 8.0) {
                say("DRIVER: self-exit=not-found; falling back to change_scene");
                if (changeScene && board_context) changeScene(board_context, 0);
                stage = 7;
                stage_frame = frame->frame_number;
                stage_time = elapsed;
            }
            return;
        case 7:
            if (elapsed < stage_time + 2.5) return;
            if (press_entry) pressEntry("to reach the level again");
            else loadLevelByName();
            stage = 8;
            stage_frame = frame->frame_number;
            stage_time = elapsed;
            return;
        case 8:
            if (level_loads > loads_at_exit) {
                say("DRIVER: board B is up (level loads=" + std::to_string(level_loads) + ")");
                stage = 9;
                stage_frame = frame->frame_number;
                stage_time = elapsed;
                return;
            }
            if (press_entry && !fallback_entry && elapsed > stage_time + 14.0) {
                fallback_entry = true;
                stage_time = elapsed;
                say("DRIVER: entry=fallback; the level entry did not load a level");
                loadLevelByName();
                return;
            }
            if (elapsed > stage_time + 30.0) {
                finished = true;
                say("DRIVER: the level never loaded a second time");
            }
            return;
        case 9:
            /* Nothing is touched here on purpose: the restored configuration and
               the paused board's refresh have to happen by themselves. */
            if (elapsed < stage_time + settle) return;
            target_instance = findTargetInstance();
            if (!target_instance) return;
            say("DRIVER: board B settled; selecting 0x" +
                hex(static_cast<uintptr_t>(target_instance)) + " without touching a field");
            (void)selectTarget(target_instance);
            clickBody(target_instance, "after re-entry");
            stage = 10;
            stage_frame = frame->frame_number;
            return;
        case 10:
            if (frame->frame_number - stage_frame < 60) return;
            reportPanelFile("after re-entry");
            stage = 11;
            stage_frame = frame->frame_number;
            return;
        case 11:
            if (frame->frame_number - stage_frame < 60) return;
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
    if (!h->create_hook || !h->resolve_symbol || !h->engine_proc) return 2;
    selectComponent = reinterpret_cast<SelectFn>(
        h->resolve_symbol(h->context, "select_component__modelZboardZboard_u9202"));
    clearSelections = reinterpret_cast<ClearSelectionsFn>(
        h->resolve_symbol(h->context, "clear_selections__modelZboardZboard_u8323"));
    changeScene = reinterpret_cast<ChangeSceneFn>(
        h->resolve_symbol(h->context, "change_scene__presenterZcontext_u2958"));
    loadLevel = reinterpret_cast<LoadLevelFn>(
        h->resolve_symbol(h->context, "load_level__modelZutilities_u7740"));
    getIo = reinterpret_cast<GetIoFn>(h->engine_proc(h->context, "igGetIO"));
    if (!selectComponent || !clearSelections) return 3;
    game_loaded = game.load(host) && game.raw_new_string != nullptr;

    const std::string requested = env("TC_FLOAT_REENTRY_SELECT");
    if (!requested.empty()) {
        if (requested.rfind("id:", 0) == 0) {
            target_custom_id = std::strtoull(requested.c_str() + 3, nullptr, 16);
            target = "id:0x" + std::to_string(target_custom_id);
            if (!target_custom_id) return 4;
        } else {
            target = requested;
        }
    }
    const std::string typed = env("TC_FLOAT_REENTRY_KEYS");
    if (!typed.empty()) keys = typed;
    const std::string row_requested = env("TC_FLOAT_REENTRY_ROW");
    if (!row_requested.empty()) row_kind = row_requested;
    level_name = env("TC_FLOAT_REENTRY_LEVEL");
    const std::string entry = env("TC_FLOAT_REENTRY_ENTRY");
    if (!entry.empty() && entry[0] == '0') press_entry = false;
    const std::string button = env("TC_FLOAT_REENTRY_BUTTON");
    if (!button.empty()) {
        const int value = std::atoi(button.c_str());
        if (value > 0 && value < 16) entry_button = value;
    }
    const std::string settle_requested = env("TC_FLOAT_REENTRY_SETTLE");
    if (!settle_requested.empty()) {
        const double value = std::atof(settle_requested.c_str());
        if (value > 0.0 && value < 60.0) settle = value;
    }
    report_path = env("TC_FLOAT_ROWS");
    if (report_path.empty())
        report_path = dataRoot() + "\\plugin-data\\local.float-ops\\panel-rows.txt";
    bodies_path = env("TC_FLOAT_BODIES");
    if (bodies_path.empty())
        bodies_path = dataRoot() + "\\plugin-data\\local.float-ops\\bodies.txt";

    void* invisibleButton = h->resolve_symbol(h->context, "igInvisibleButton");
    if (!invisibleButton ||
        h->create_hook(h->context, invisibleButton, reinterpret_cast<void*>(hookInvisible),
                       reinterpret_cast<void**>(&invisibleOriginal)) != 0) {
        say("DRIVER: cannot hook igInvisibleButton; only the load_level fallback is available");
        invisibleOriginal = nullptr;
        press_entry = false;
    }
    if (tc::events::subscribe(host, TC_EVENT_LEVEL_LOAD | TC_EVENT_SCENE_CHANGE, &onEvent,
                              nullptr) != TC_EVENT_OK) {
        say("DRIVER: cannot subscribe to the event bus");
        return 5;
    }
    out->on_frame = tick;
    say("DRIVER: armed (entry=" + std::string(press_entry ? "1" : "0") + ", level=\"" +
        level_name + "\")");
    return 0;
}
