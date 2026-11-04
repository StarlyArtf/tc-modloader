/* Board grid: the board is laid out on an integer coordinate grid - pins sit
   one unit apart, wires turn on integers, components are placed on them - but
   the game never draws it.  This Mod draws it, and puts the switch where a
   player looks for it: the loader's unified Mod settings page.

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
#include "../../sdk/tc_mod_storage.h"
#include "../../sdk/tc_mod_settings.h"

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
/* The game's own "which level is loaded" Nim string (a data object, not a
   callable): non-empty means a board is on screen.  Read the header only -
   the characters belong to the game and are not needed here. */
const void* loadedLevelName = nullptr;

bool enabled = true;
std::string configPath;
tc::mod_storage::ApiV2 storage{};
bool storageReady = false;
tc::mod_settings::Api settings{};

void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

void note(const std::string& message, int level) {
    if (host && host->report_status) host->report_status(host->context, level, message.c_str());
}

int migrateSettings(void*, uint32_t from, uint32_t to, const void* oldData,
                    uint32_t oldSize, void* newData, uint32_t capacity,
                    uint32_t* newSize) {
    if (!newData || !newSize || from != 0 || to != 1 || capacity < 1)
        return TC_MOD_STORAGE_MIGRATE_REJECT;
    std::string old;
    if (oldData && oldSize) old.assign(static_cast<const char*>(oldData), oldSize);
    const char value = old.find("grid=0") != std::string::npos || old == "0" ? '0' : '1';
    *static_cast<char*>(newData) = value;
    *newSize = 1;
    return TC_MOD_STORAGE_MIGRATE_OK;
}

int settingChanged(void*, const char* id, const TCModSettingValueV1* value) {
    if (!id || std::strcmp(id, "grid") != 0 || !value ||
        value->type != TC_MOD_SETTING_BOOL) return TC_MOD_SETTINGS_ERR_ARGUMENT;
    enabled = value->integer != 0;
    report(std::string("board grid: ") + (enabled ? "on" : "off"));
    return TC_MOD_SETTINGS_OK;
}

bool registerSetting() {
    storageReady = tc::mod_storage::tableV2(host, &storage);
    if (!storageReady || !tc::mod_settings::table(host, &settings)) return false;

    /* Import the pre-service text file only when no loader-owned value exists. */
    if (tc::mod_storage::has(storage, TC_MOD_STORAGE_SCOPE_GLOBAL, "settings") == 0 &&
        !configPath.empty()) {
        std::ifstream file(configPath, std::ios::binary);
        if (file) {
            const std::string legacy((std::istreambuf_iterator<char>(file)), {});
            tc::mod_storage::writeText(storage, TC_MOD_STORAGE_SCOPE_GLOBAL,
                                       "settings", legacy);
        }
    }
    uint32_t version = 0;
    const char defaultValue = '1';
    const int migration = tc::mod_storage::migrate(
        storage, TC_MOD_STORAGE_SCOPE_GLOBAL, "settings", 1,
        &defaultValue, 1, &migrateSettings, nullptr, &version);
    if (migration != TC_MOD_STORAGE_OK) {
        report(std::string("board grid: settings migration failed: ") +
               tc::mod_storage::errorText(migration));
        return false;
    }

    TCModSettingFieldV1 field{};
    field.size = sizeof(field);
    field.type = TC_MOD_SETTING_BOOL;
    field.scope = TC_MOD_STORAGE_SCOPE_GLOBAL;
    field.id = "grid";
    field.label = "Show board grid";
    field.description = "Draw integer board coordinates behind components and panels.";
    field.storage_key = "settings";
    field.default_integer = 1;
    if (tc::mod_settings::setFields(settings, &field, 1, &settingChanged, nullptr) !=
        TC_MOD_SETTINGS_OK) return false;
    TCModSettingValueV1 value{};
    if (tc::mod_settings::get(settings, "grid", &value) != TC_MOD_SETTINGS_OK)
        return false;
    enabled = value.integer != 0;
    return true;
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
    loadedLevelName = host->resolve_alias(host->context, "level.loaded");
    if (!worldToScreen || !getIO || !getMainViewport || !getBackgroundDrawList)
        report("board grid: board transform unavailable; the grid stays off");

    if (h->data_directory_utf8 && *h->data_directory_utf8) {
        configPath = std::string(h->data_directory_utf8) + "\\board-grid.txt";
    }
    if (!registerSetting()) {
        report("board grid: unified Mod settings service is unavailable");
        return 4;
    }
    report("board grid: unified Mod settings page carries the switch");
    note(std::string("网格：") + (enabled ? "开" : "关") +
             "（主菜单 → Mod settings 里可以切换）", 0);
    out->on_frame = &frame;
    return 0;
}
