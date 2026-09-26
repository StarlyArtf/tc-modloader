/* Board grid: the board is laid out on an integer coordinate grid - pins sit
   one unit apart, wires turn on integers, components are placed on them - but
   the game never draws it.  This Mod draws it, and puts the switch where a
   player looks for a setting: the game's own Options page, General tab.

   Drawing in board coordinates rather than screen coordinates is what makes
   the grid follow pan and zoom for free: the game's own
   world_pos_to_screen_pos (see the symbol profile, "board.world_to_screen")
   returns *normalised* screen coordinates, and its own draw_simple_rect
   multiplies that by ImGuiIO.DisplaySize to get pixels.  Two probes - the
   origin and one unit along each axis - give the affine map, its inverse gives
   the visible board rectangle, and the lines are placed on every integer
   inside it.  They go into the main viewport's *background* draw list, which
   is the layer the game paints the board's own selection rectangles into: over
   the board, under every panel. */
#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_ui_draw.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

struct V2 {
    float x, y;
};

const TCHost* host = nullptr;

V2 (*worldToScreen)(V2) = nullptr;
void* (*getIO)() = nullptr;
void* (*getMainViewport)() = nullptr;
void* (*getBackgroundDrawList)(void*) = nullptr;
bool (*igCheckbox)(const char*, bool*) = nullptr;
void (*igSeparator)() = nullptr;
/* The game's own "which level is loaded" Nim string (a data object, not a
   callable): non-empty means a board is on screen.  Read the header only -
   the characters belong to the game and are not needed here. */
const void* loadedLevelName = nullptr;

void (*optionsGeneralOriginal)(void*) = nullptr;

bool enabled = true;
bool hookReady = false;
std::string configPath;

void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

void note(const std::string& message, int level) {
    if (host && host->report_status) host->report_status(host->context, level, message.c_str());
}

void saveConfig() {
    if (configPath.empty()) return;
    std::ofstream file(configPath, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << (enabled ? "grid=1\n" : "grid=0\n");
}

void loadConfig() {
    if (configPath.empty()) return;
    std::ifstream file(configPath, std::ios::binary);
    if (!file) return;
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line == "grid=0") enabled = false;
        if (line == "grid=1") enabled = true;
    }
}

/* The game's Options page, General tab: the original draws the settings, then
   this appends one row of its own - the switch a player expects to find
   there. */
void hookOptionsGeneral(void* presenter) {
    if (optionsGeneralOriginal) optionsGeneralOriginal(presenter);
    if (!igCheckbox || !igSeparator) return;
    igSeparator();
    bool value = enabled;
    if (igCheckbox("显示网格 (board grid)", &value)) {
        enabled = value;
        saveConfig();
        report(std::string("board grid: ") + (enabled ? "on" : "off"));
    }
}

/* True while a board is on screen: the level object the game considers loaded
   is null everywhere else, and the grid has no meaning while it is. */
bool boardUp() {
    if (!loadedLevelName) return true;
    struct NimStringHeader {
        int64_t length;
        const void* data;
    };
    const auto* name = static_cast<const NimStringHeader*>(loadedLevelName);
    return name->length > 0 && name->length <= 64 && name->data != nullptr;
}

void drawGrid() {
    auto& api = tc::ui::drawing_detail::table();
    if (!api.line) return;
    void* io = getIO ? getIO() : nullptr;
    if (!io) return;
    const V2 display = *reinterpret_cast<V2*>(static_cast<char*>(io) + 8);
    if (!(display.x > 8.f) || !(display.y > 8.f)) return;
    /* The affine board -> screen map, measured with three probes. */
    const V2 origin{worldToScreen({0.f, 0.f}).x * display.x,
                    worldToScreen({0.f, 0.f}).y * display.y};
    const V2 unitX = worldToScreen({1.f, 0.f});
    const V2 unitY = worldToScreen({0.f, 1.f});
    const V2 ex{unitX.x * display.x - origin.x, unitX.y * display.y - origin.y};
    const V2 ey{unitY.x * display.x - origin.x, unitY.y * display.y - origin.y};
    const float determinant = ex.x * ey.y - ex.y * ey.x;
    if (std::fabs(determinant) < 1e-6f) return;
    if (std::fabs(ex.x) < 1e-4f && std::fabs(ex.y) < 1e-4f) return;

    /* The inverse: which board coordinates the four screen corners cover. */
    const V2 corners[4] = {{0.f, 0.f}, {display.x, 0.f}, {0.f, display.y},
                           {display.x, display.y}};
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (const V2& corner : corners) {
        const V2 d{corner.x - origin.x, corner.y - origin.y};
        const V2 world{(d.x * ey.y - d.y * ey.x) / determinant,
                       (d.y * ex.x - d.x * ex.y) / determinant};
        minX = std::min(minX, world.x);
        maxX = std::max(maxX, world.x);
        minY = std::min(minY, world.y);
        maxY = std::max(maxY, world.y);
    }
    if (!(maxX > minX) || !(maxY > minY)) return;
    /* A sane board, not a degenerate transform: the grid must not devolve into
       thousands of lines (or none) because the camera is mid-flight. */
    if (maxX - minX > 4096.f || maxY - minY > 4096.f) return;

    void* list = getBackgroundDrawList ? getBackgroundDrawList(getMainViewport()) : nullptr;
    if (!list) return;
    const auto screen = [&](float x, float y) {
        return tc::ui::Vec2{origin.x + x * ex.x + y * ey.x, origin.y + x * ex.y + y * ey.y};
    };
    const float firstX = std::floor(minX);
    const float lastX = std::ceil(maxX);
    const float firstY = std::floor(minY);
    const float lastY = std::ceil(maxY);
    for (float x = firstX; x <= lastX; x += 1.f) {
        const bool major = std::fabs(std::fmod(x, 4.f)) < 0.001f;
        const tc::ui::Color colour = tc::ui::rgba(255, 255, 255, major ? 46 : 22);
        api.line(list, screen(x, minY), screen(x, maxY), colour, 1.f);
    }
    for (float y = firstY; y <= lastY; y += 1.f) {
        const bool major = std::fabs(std::fmod(y, 4.f)) < 0.001f;
        const tc::ui::Color colour = tc::ui::rgba(255, 255, 255, major ? 46 : 22);
        api.line(list, screen(minX, y), screen(maxX, y), colour, 1.f);
    }
}

void frame(void*, const TCFrame*) {
    if (!enabled || !worldToScreen || !boardUp()) return;
    drawGrid();
}

template <class T>
T resolve(const char* alias) {
    T value = nullptr;
    void* found = host->resolve_alias(host->context, alias);
    static_assert(sizeof(value) == sizeof(found));
    std::memcpy(&value, &found, sizeof(value));
    return value;
}

template <class T>
T engine(const char* name) {
    T value = nullptr;
    void* found = host->engine_proc(host->context, name);
    static_assert(sizeof(value) == sizeof(found));
    std::memcpy(&value, &found, sizeof(value));
    return value;
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    if (!tc::ui::load(h)) {
        report("board grid: tc::ui::load failed: " + tc::ui::missing());
        return 2;
    }
    if (!tc::ui::loadDrawing(h)) {
        report("board grid: drawing unavailable: " + tc::ui::drawingMissing());
        return 3;
    }
    worldToScreen = resolve<V2 (*)(V2)>("board.world_to_screen");
    getIO = engine<void* (*)()>("igGetIO");
    getMainViewport = engine<void* (*)()>("igGetMainViewport");
    getBackgroundDrawList = engine<void* (*)(void*)>("igGetBackgroundDrawList");
    igCheckbox = engine<bool (*)(const char*, bool*)>("igCheckbox");
    igSeparator = engine<void (*)()>("igSeparator");
    loadedLevelName = host->resolve_alias(host->context, "level.loaded");
    if (!worldToScreen || !getIO || !getMainViewport || !getBackgroundDrawList)
        report("board grid: board transform unavailable; the grid stays off");

    if (h->data_directory_utf8 && *h->data_directory_utf8) {
        configPath = std::string(h->data_directory_utf8) + "\\board-grid.txt";
        loadConfig();
    }

    if (h->create_hook) {
        void* target = h->resolve_alias(h->context, "options.general");
        if (target &&
            h->create_hook(h->context, target, reinterpret_cast<void*>(hookOptionsGeneral),
                           reinterpret_cast<void**>(&optionsGeneralOriginal)) == 0) {
            hookReady = true;
            report("board grid: the Options page carries the switch");
        } else {
            report("board grid: Options page hook refused; the setting is unavailable");
        }
    }
    if (!hookReady) return 4;
    note(std::string("网格：") + (enabled ? "开" : "关") +
             "（设置 → 常规 里可以切换）", 0);
    out->on_frame = &frame;
    return 0;
}
