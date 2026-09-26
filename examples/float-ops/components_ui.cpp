/* The components' editor rows, drawn inside the game's own component drawer.

   Where a player edits a component: the game shows a panel along the bottom for
   whatever is selected, and a built-in Constant's label and value fields live
   in it.  A Mod's editor belongs in the same place and the same shape, so this
   file registers one TC_UI_SLOT_BOARD_COMPONENT_PANEL slot
   (sdk/tc_ui.h, registerComponentPanel) and draws rows with ordinary tc::ui
   widgets from the cursor the game left after its own rows.  Nothing pops up
   next to the component any more: selection alone is what opens the editor,
   exactly as it does for the game's own parts.

   The host hands the slot the instance the drawer is showing, so the rows can
   read and write that instance's configuration through the same helpers the
   rest of the Mod uses.  Every commit goes through writeConfig(), which asks
   the host for a refresh of the paused board, so the wire and the Display
   follow immediately (docs/sdk/services.md). */

#include "components_internal.hpp"

#include "fp/decimal.hpp"
#include "fp/fp32.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace floatops {
namespace internals {

namespace {

/* The panel rows are the only interactive thing this Mod draws in the game's
   own window, and whether that window hands a plugin's widget to ImGui's input
   path is a property of the build, not of the code here.  The rows therefore
   report their own geometry (so a playtest can click one instead of guessing)
   and, when TC_FLOATOPS_UI is set, what ImGui answered about them. */
struct RowReport {
    RowKind kind = RowKind::none;
    float min_x = 0.f, min_y = 0.f, max_x = 0.f, max_y = 0.f;
    bool valid = false;
    /* The row's field alone (the input box, or the radio group), which is what
       a playtest clicks to reach the widget itself. */
    float field_min_x = 0.f, field_min_y = 0.f, field_max_x = 0.f, field_max_y = 0.f;
    bool field_valid = false;
    /* The last choice of a control group (the RUP radio of a rounding row), so
       a playtest can click a specific choice instead of guessing where the
       radios sit inside the row. */
    float choice_min_x = 0.f, choice_min_y = 0.f, choice_max_x = 0.f, choice_max_y = 0.f;
    bool choice_valid = false;
    bool live = false;
    /* Whether the row's field held the keyboard focus when it was drawn. */
    bool focused = false;
};

struct PanelReport {
    uint64_t instance = 0;
    uint64_t type = 0;
    float width = 0.f, height = 0.f;
    float mouse_x = 0.f, mouse_y = 0.f;
    /* What the game's own drawer answers about itself (see the comment on the
       row window: on this build it is never the hovered window). */
    bool panel_hovered = false;
    bool window_hovered = false;
    bool window_focused = false;
    bool any_active = false;
    bool rows_drawn = false;
    RowReport rows[4];
    int rowCount = 0;
    int frame = 0;
};

bool uiTrace() {
    static const bool enabled = [] {
        const char* value = std::getenv("TC_FLOATOPS_UI");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

const char* rowName(RowKind kind) {
    switch (kind) {
        case RowKind::label: return "label";
        case RowKind::value: return "value";
        case RowKind::rounding: return "rounding";
        case RowKind::display: return "display";
        case RowKind::info: return "info";
        default: return "none";
    }
}

/* Written whenever the geometry changes: the playtest clicks a row by reading
   this file, so it never has to reimplement the panel's own layout. */
void writePanelReport(const PanelReport& report) {
    if (!host || !host->data_directory_utf8) return;
    char path[512] = {};
    std::snprintf(path, sizeof(path), "%s\\panel-rows.txt", host->data_directory_utf8);
    std::FILE* file = std::fopen(path, "wb");
    if (!file) return;
    std::fprintf(file,
                 "instance 0x%llx\ntype 0x%llx\nwindow %.2f %.2f\nframe %d\nrows %d\n",
                 static_cast<unsigned long long>(report.instance),
                 static_cast<unsigned long long>(report.type), report.width,
                 report.height, report.frame, report.rowCount);
    for (int index = 0; index < report.rowCount; ++index) {
        const RowReport& row = report.rows[index];
        if (!row.valid) continue;
        std::fprintf(file, "row %s %.2f %.2f %.2f %.2f\n", rowName(row.kind), row.min_x,
                     row.min_y, row.max_x, row.max_y);
        if (row.field_valid)
            std::fprintf(file, "field %s %.2f %.2f %.2f %.2f\n", rowName(row.kind),
                         row.field_min_x, row.field_min_y, row.field_max_x, row.field_max_y);
        if (row.choice_valid)
            std::fprintf(file, "choice %s %.2f %.2f %.2f %.2f\n", rowName(row.kind),
                         row.choice_min_x, row.choice_min_y, row.choice_max_x,
                         row.choice_max_y);
    }
    std::fclose(file);
}

bool sameGeometry(const PanelReport& left, const PanelReport& right) {
    if (left.instance != right.instance || left.type != right.type ||
        left.rowCount != right.rowCount)
        return false;
    if (left.width != right.width || left.height != right.height) return false;
    for (int index = 0; index < left.rowCount; ++index) {
        const RowReport& a = left.rows[index];
        const RowReport& b = right.rows[index];
        if (a.valid != b.valid || a.min_x != b.min_x || a.min_y != b.min_y ||
            a.max_x != b.max_x || a.max_y != b.max_y)
            return false;
    }
    return true;
}

/* One bounded line every kTraceFrames while the drawer shows one of this Mod's
   components: the hover answers are the evidence for "the game's window does /
   does not deliver input to a plugin's widget here". */
void tracePanel(const PanelReport& report) {
    if (!uiTrace()) return;
    static int lines = 0;
    static int lastFrame = -1000;
    if (report.frame - lastFrame < 30) return;
    lastFrame = report.frame;
    if (lines++ >= 200) return;
    char detail[512] = {};
    int used = std::snprintf(detail, sizeof(detail),
                             "float-ops panel trace: frame=%d instance=0x%llx type=0x%llx "
                             "window=%.0fx%.0f mouse=%.0f,%.0f panelHovered=%d "
                             "windowHovered=%d windowFocused=%d anyActive=%d rowsDrawn=%d rows=%d",
                             report.frame, static_cast<unsigned long long>(report.instance),
                             static_cast<unsigned long long>(report.type), report.width,
                             report.height, report.mouse_x, report.mouse_y,
                             report.panel_hovered ? 1 : 0,
                             report.window_hovered ? 1 : 0, report.window_focused ? 1 : 0,
                             report.any_active ? 1 : 0, report.rows_drawn ? 1 : 0,
                             report.rowCount);
    for (int index = 0; index < report.rowCount; ++index) {
        const RowReport& row = report.rows[index];
        if (!row.valid) continue;
        used += std::snprintf(detail + used, sizeof(detail) - static_cast<size_t>(used),
                              " r%d=%.0f,%.0f..%.0f,%.0f live=%d focus=%d", index,
                              row.min_x, row.min_y, row.max_x, row.max_y, row.live ? 1 : 0,
                              row.focused ? 1 : 0);
        if (used >= static_cast<int>(sizeof(detail)) - 64) break;
    }
    if (used < static_cast<int>(sizeof(detail)) - 64)
        std::snprintf(detail + used, sizeof(detail) - static_cast<size_t>(used),
                      " label=\"%s\"", editor.label);
    note(detail);
}

PanelReport lastPanelReport;
int lastPanelWriteFrame = -1000;

/* A two-float vector, the shape the engine's getter exports return through a
   hidden pointer (same convention as tc::ui's own Vec2). */
struct V2 { float x, y; };

bool itemRect(float* minX, float* minY, float* maxX, float* maxY);

bool inside(const HitBox& box, const MousePoint& point) {
    return box.valid && point.x >= box.min_x && point.x <= box.max_x &&
           point.y >= box.min_y && point.y <= box.max_y;
}

/* The newest painted boxes under the pointer: the body highlights on hover, the
   way the stock parts do. */
void refreshHover(const MousePoint& point) {
    std::lock_guard<std::mutex> lock(hitMutex);
    ++frameCounter;
    for (auto& entry : boxes) {
        entry.second.body.hovered = inside(entry.second.body, point);
        entry.second.width.hovered = inside(entry.second.width, point);
        entry.second.rounding.hovered = inside(entry.second.rounding, point);
    }
}

/* Which type an instance is.  The painted boxes are the fastest answer and the
   one the drawer's own frame already has; when they say nothing (the frame a
   board was entered, or a component that is off screen) the host's instance list
   answers instead, so the rows do not depend on a render pass having happened. */
uint64_t typeOfInstance(uint64_t instance) {
    {
        std::lock_guard<std::mutex> lock(hitMutex);
        const auto found = boxes.find(instance);
        if (found != boxes.end() && found->second.custom_id) return found->second.custom_id;
    }
    return typeOfLiveInstance(instance);
}

/* Reads the record the rows describe for one type, in the layout that type owns.
   One read serves both the fields and the snapshot they are compared against, so
   "the rows match the record" is a question about exactly the bytes the player
   sees.  Returns false when the type is not one of this Mod's or the host
   refused the read (a record that is not on the board yet). */
bool readConfigSnapshot(uint64_t instance, uint64_t customId, ConfigSnapshot* out) {
    if (!out) return false;
    /* A failed read still answers with the type it was asked about and nothing
       else, so the caller can tell "this record is not readable yet" from "these
       rows belong to another instance". */
    *out = ConfigSnapshot{};
    out->type = customId;
    ConstantConfig constant = constantDefault();
    AddConfig add = addDefault();
    DisplayConfig display = displayDefault();
    OpsConfig ops = opsDefault();
    void* buffer = nullptr;
    uint32_t size = 0;
    if (customId == kConstantId) {
        buffer = &constant;
        size = sizeof(constant);
    } else if (customId == kAddId) {
        buffer = &add;
        size = sizeof(add);
    } else if (customId == kDisplayId) {
        buffer = &display;
        size = sizeof(display);
    } else if (catalogueInfo(customId)) {
        buffer = &ops;
        size = sizeof(ops);
    } else {
        return false;
    }
    if (size > kConfigSnapshotBytes) return false;
    if (!currentConfig(instance, customId, buffer, size)) return false;
    out->size = size;
    out->valid = true;
    std::memcpy(out->bytes, buffer, size);
    return true;
}

/* Whether the rows still show what the record holds.

   A record that cannot be read *yet* answers "yes": the drawer can open before
   the loader has restored a saved configuration, and there is nothing better to
   show than the default until it is there.  A record that becomes readable
   afterwards answers "no" - which is how the fields pick up a value the loader
   restored after the rows were laid out, without the player touching anything. */
bool recordMatchesRows(uint64_t instance, uint64_t customId) {
    if (editor.loaded.type != customId) return false;
    ConfigSnapshot now;
    if (!readConfigSnapshot(instance, customId, &now)) return true;
    if (!editor.loaded.valid) return false;
    return now.size == editor.loaded.size &&
           std::memcmp(now.bytes, editor.loaded.bytes, now.size) == 0;
}

void loadRows(uint64_t instance, uint64_t customId) {
    editor.instance = instance;
    editor.message.clear();
    (void)readConfigSnapshot(instance, customId, &editor.loaded);
    const uint8_t* stored = editor.loaded.valid ? editor.loaded.bytes : nullptr;
    if (customId == kConstantId) {
        ConstantConfig config = constantDefault();
        if (stored) std::memcpy(&config, stored, sizeof(config));
        config.label[sizeof(config.label) - 1] = 0;
        std::snprintf(editor.label, sizeof(editor.label), "%s", config.label);
        const char* special = tcfp::decimal_special_name(config.bits);
        if (special) std::snprintf(editor.value, sizeof(editor.value), "%s", special);
        else tcfp::format_shortest(config.bits, editor.value, sizeof(editor.value));
        return;
    }
    if (customId == kAddId) {
        AddConfig config = addDefault();
        if (stored) std::memcpy(&config, stored, sizeof(config));
        config.label[sizeof(config.label) - 1] = 0;
        std::snprintf(editor.label, sizeof(editor.label), "%s", config.label);
        return;
    }
    if (customId == kDisplayId) {
        DisplayConfig config = displayDefault();
        if (stored) std::memcpy(&config, stored, sizeof(config));
        config.label[sizeof(config.label) - 1] = 0;
        std::snprintf(editor.label, sizeof(editor.label), "%s", config.label);
        return;
    }
    if (catalogueInfo(customId)) {
        OpsConfig config = opsDefault();
        if (stored) std::memcpy(&config, stored, sizeof(config));
        config.label[sizeof(config.label) - 1] = 0;
        std::snprintf(editor.label, sizeof(editor.label), "%s", config.label);
    }
}

/* The label is shared by every type, so it is committed generically: read the
   stored configuration, replace the label, write it back.  The catalogue types
   share one layout, the three M2 types each have their own. */
void commitLabel() {
    if (!editor.instance) return;
    const uint64_t type = typeOfInstance(editor.instance);
    char label[kLabelBytes] = {};
    std::snprintf(label, sizeof(label), "%s", editor.label);
    ConstantConfig constant = constantDefault();
    AddConfig add = addDefault();
    DisplayConfig display = displayDefault();
    bool ok = false;
    switch (type) {
        case kConstantId:
            (void)currentConfig(editor.instance, type, &constant, sizeof(constant));
            std::memcpy(constant.label, label, sizeof(constant.label));
            ok = writeConfig(editor.instance, &constant, sizeof(constant));
            break;
        case kAddId:
            (void)currentConfig(editor.instance, type, &add, sizeof(add));
            std::memcpy(add.label, label, sizeof(add.label));
            ok = writeConfig(editor.instance, &add, sizeof(add));
            break;
        case kDisplayId:
            (void)currentConfig(editor.instance, type, &display, sizeof(display));
            std::memcpy(display.label, label, sizeof(display.label));
            ok = writeConfig(editor.instance, &display, sizeof(display));
            break;
        default: {
            if (!catalogueInfo(type)) return;
            OpsConfig ops = opsDefault();
            (void)currentConfig(editor.instance, type, &ops, sizeof(ops));
            std::memcpy(ops.label, label, sizeof(ops.label));
            ok = writeConfig(editor.instance, &ops, sizeof(ops));
            break;
        }
    }
    char detail[192] = {};
    std::snprintf(detail, sizeof(detail), "float-ops: instance 0x%llx label \"%s\" (%s)",
                  static_cast<unsigned long long>(editor.instance), label,
                  ok ? "written" : "refused");
    note(detail);
    if (!ok) editor.message = "写入元件配置失败（元件可能已经不在棋盘上）";
}

void commitValue() {
    const tcfp::DecimalParse parsed = tcfp::parse_decimal(editor.value);
    if (!parsed.ok) {
        editor.message = "看不懂这个写法，保留原值";
        note(std::string("float-ops: constant editor refused \"") + editor.value + "\"");
        return;
    }
    ConstantConfig config = constantDefault();
    (void)currentConfig(editor.instance, kConstantId, &config, sizeof(config));
    config.format = kFormatBinary32;
    config.bits = parsed.bits;
    if (!writeConfig(editor.instance, &config, sizeof(config))) {
        editor.message = "写入元件配置失败（元件可能已经不在棋盘上）";
        return;
    }
    editor.message.clear();
    char detail[192] = {};
    std::snprintf(detail, sizeof(detail),
                  "float-ops: constant 0x%llx set to 0x%08X (diagnostics 0x%02X)",
                  static_cast<unsigned long long>(editor.instance), parsed.bits,
                  parsed.diagnostics);
    note(detail);
}

void commitAddRounding(uint8_t code) {
    AddConfig config = addDefault();
    (void)currentConfig(editor.instance, kAddId, &config, sizeof(config));
    config.format = kFormatBinary32;
    config.rounding = code;
    if (!writeConfig(editor.instance, &config, sizeof(config))) {
        editor.message = "写入元件配置失败";
        return;
    }
    editor.message.clear();
    char detail[192] = {};
    std::snprintf(detail, sizeof(detail), "float-ops: add 0x%llx rounding=%s",
                  static_cast<unsigned long long>(editor.instance),
                  roundingCode(static_cast<tcfp::FPRounding>(code)));
    note(detail);
}

/* The catalogue types keep their rounding mode in their own layout; the M2
   adder keeps it in its AddConfig.  Both are one field, so the panel's row does
   not have to know which is which beyond this pair of helpers. */
uint8_t roundingOfType(uint64_t type, uint64_t instance) {
    if (type == kAddId) {
        AddConfig config = addDefault();
        (void)currentConfig(instance, kAddId, &config, sizeof(config));
        return config.rounding;
    }
    OpsConfig config = opsDefault();
    (void)currentConfig(instance, type, &config, sizeof(config));
    return config.rounding;
}

void commitRounding(uint64_t type, uint8_t code) {
    if (type == kAddId) {
        commitAddRounding(code);
        return;
    }
    OpsConfig config = opsDefault();
    (void)currentConfig(editor.instance, type, &config, sizeof(config));
    config.format = kFormatBinary32;
    config.rounding = code;
    if (!writeConfig(editor.instance, &config, sizeof(config))) {
        editor.message = "写入元件配置失败";
        return;
    }
    editor.message.clear();
    char detail[192] = {};
    std::snprintf(detail, sizeof(detail),
                  "float-ops: type 0x%llx instance 0x%llx rounding=%s",
                  static_cast<unsigned long long>(type),
                  static_cast<unsigned long long>(editor.instance),
                  roundingCode(static_cast<tcfp::FPRounding>(code)));
    note(detail);
}

void commitDisplayMode(uint8_t code) {
    DisplayConfig config = displayDefault();
    (void)currentConfig(editor.instance, kDisplayId, &config, sizeof(config));
    config.format = kFormatBinary32;
    config.mode = code;
    if (!writeConfig(editor.instance, &config, sizeof(config))) {
        editor.message = "写入元件配置失败";
        return;
    }
    editor.message.clear();
    char detail[192] = {};
    std::snprintf(detail, sizeof(detail), "float-ops: display 0x%llx mode=%u",
                  static_cast<unsigned long long>(editor.instance), code);
    note(detail);
}

/* The rows of the drawer, drawn in a window of this Mod's own.

   Why not draw them directly in the game's panel: measured on this build, the
   drawer never becomes the ImGui window under the mouse (`IsWindowHovered` is
   false for it while the mouse sits on a row the plugin drew inside it), so a
   plugin's widget there is painted but can never be hovered, clicked or typed
   into - which is exactly what the player reported ("the input box and the
   check box do nothing").  The same measurement is in docs/sdk/ui.md.

   So the rows live in a window this Mod owns, placed exactly over the panel's
   row area: no title bar, no resize, no saved state, and no background, so the
   game's own panel shows through and the rows read as part of it.  The window
   is emitted after the drawer, so it remains in front without forcibly taking
   keyboard focus.  In particular it must not call setNextWindowFocus every
   frame: doing that prevents the game's top menu from receiving input while a
   float component remains selected.

   There is no editor window of the Mod's own any more: the rows in the panel
   are the whole editor (the player asked for exactly that), and each field
   commits the way the game's own panel fields do - when it is left. */
constexpr int kRowWindowFlags = tc::ui::Window_NoTitleBar | tc::ui::Window_NoResize |
                                tc::ui::Window_NoMove | tc::ui::Window_NoScrollbar |
                                tc::ui::Window_NoCollapse | tc::ui::Window_AlwaysAutoResize |
                                tc::ui::Window_NoSavedSettings | tc::ui::Window_NoBackground |
                                tc::ui::Window_NoFocusOnAppearing;

PanelReport pendingReport;

/* The screen position of the game panel's content origin: the cursor's screen
   position minus its window-local one.  Reading it beats assuming where the
   drawer sits, and it follows a moved or resized window. */
tc::ui::Vec2 contentOrigin() {
    const tc::ui::Vec2 screen = tc::ui::cursorScreenPos();
    return tc::ui::Vec2{screen.x - tc::ui::cursorPosX(), screen.y - tc::ui::cursorPosY()};
}

HitBox itemBox() {
    HitBox box;
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    if (itemRect(&minX, &minY, &maxX, &maxY)) {
        box.min_x = minX;
        box.min_y = minY;
        box.max_x = maxX;
        box.max_y = maxY;
        box.valid = true;
    }
    return box;
}

HitBox unionBox(const HitBox& left, const HitBox& right) {
    if (!left.valid) return right;
    if (!right.valid) return left;
    HitBox box;
    box.min_x = left.min_x < right.min_x ? left.min_x : right.min_x;
    box.min_y = left.min_y < right.min_y ? left.min_y : right.min_y;
    box.max_x = left.max_x > right.max_x ? left.max_x : right.max_x;
    box.max_y = left.max_y > right.max_y ? left.max_y : right.max_y;
    box.valid = true;
    return box;
}

void endRow(RowKind kind, const HitBox& box, const HitBox& field, bool hovered,
            bool focused = false, const HitBox& choice = HitBox{}) {
    if (editor.rowCount >= 4) return;
    PanelRow& row = editor.rows[editor.rowCount];
    row.kind = kind;
    row.box = box;
    row.live = hovered;
    RowReport& report = pendingReport.rows[editor.rowCount];
    report.kind = kind;
    report.min_x = box.min_x;
    report.min_y = box.min_y;
    report.max_x = box.max_x;
    report.max_y = box.max_y;
    report.valid = box.valid;
    report.field_min_x = field.min_x;
    report.field_min_y = field.min_y;
    report.field_max_x = field.max_x;
    report.field_max_y = field.max_y;
    report.field_valid = field.valid;
    report.choice_min_x = choice.min_x;
    report.choice_min_y = choice.min_y;
    report.choice_max_x = choice.max_x;
    report.choice_max_y = choice.max_y;
    report.choice_valid = choice.valid;
    report.live = hovered;
    report.focused = focused;
    ++editor.rowCount;
}

/* The game's own text came from `igGetItemRectMin/Max`, which the loader already
   uses for the foundry button: the same two exports give a row's screen box. */
bool itemRect(float* minX, float* minY, float* maxX, float* maxY) {
    using GetVec2 = void (*)(V2*);
    static GetVec2 getMin = nullptr;
    static GetVec2 getMax = nullptr;
    static bool probed = false;
    if (!probed) {
        probed = true;
        if (host && host->engine_proc) {
            void* minProc = host->engine_proc(host->context, "igGetItemRectMin");
            void* maxProc = host->engine_proc(host->context, "igGetItemRectMax");
            std::memcpy(&getMin, &minProc, sizeof(getMin));
            std::memcpy(&getMax, &maxProc, sizeof(getMax));
        }
    }
    if (!getMin || !getMax) return false;
    V2 minimum{}, maximum{};
    getMin(&minimum);
    getMax(&maximum);
    *minX = minimum.x; *minY = minimum.y;
    *maxX = maximum.x; *maxY = maximum.y;
    return maximum.x > minimum.x && maximum.y > minimum.y;
}

/* Every row: the game's own shape - a label on the left, the field on the
   right - with the field being a real widget, committed the way the game's own
   panel fields are (when the field loses focus after an edit). */
void rowLabel(float fieldWidth) {
    tc::ui::text("标签");
    const HitBox labelBox = itemBox();
    tc::ui::sameLine();
    tc::ui::setNextItemWidth(fieldWidth);
    tc::ui::inputText("##label", editor.label, sizeof(editor.label));
    const HitBox fieldBox = itemBox();
    const bool hovered = tc::ui::isItemHovered(0);
    const bool focused = tc::ui::isItemFocused();
    const bool done = tc::ui::isItemDeactivatedAfterEdit();
    endRow(RowKind::label, unionBox(labelBox, fieldBox), fieldBox, hovered, focused);
    if (done) {
        /* The field is committed when the player leaves it, exactly like the
           game's own panel fields. */
        note("float-ops: the label field was committed after editing");
        commitLabel();
    }
}

void rowValue(float fieldWidth) {
    tc::ui::text("常量值");
    const HitBox labelBox = itemBox();
    tc::ui::sameLine();
    tc::ui::setNextItemWidth(fieldWidth);
    tc::ui::inputText("##value", editor.value, sizeof(editor.value));
    const HitBox fieldBox = itemBox();
    const bool hovered = tc::ui::isItemHovered(0);
    const bool done = tc::ui::isItemDeactivatedAfterEdit();
    endRow(RowKind::value, unionBox(labelBox, fieldBox), fieldBox, hovered);
    if (done) commitValue();
}

/* The rounding row serves both the M2 adder and every catalogue type that
   rounds: the choice is written back to whichever configuration that type owns
   (commitRounding) and the compact codes are the archive values (plan 7.2). */
void rowRounding(uint64_t type) {
    tc::ui::text("舍入模式");
    HitBox box = itemBox();
    HitBox field;
    HitBox lastChoice;
    bool hovered = false;
    const tcfp::FPRounding current = roundingOf(roundingOfType(type, editor.instance));
    for (uint8_t code = 0; code <= 4; ++code) {
        const tcfp::FPRounding mode = static_cast<tcfp::FPRounding>(code);
        tc::ui::sameLine();
        const bool picked = tc::ui::radioButton(roundingCode(mode), current == mode);
        const HitBox button = itemBox();
        box = unionBox(box, button);
        field = unionBox(field, button);
        lastChoice = button;
        if (tc::ui::isItemHovered(0)) hovered = true;
        if (picked) commitRounding(type, code);
    }
    /* The last radio is RUP, the mode that proves the write-back: a playtest
       clicks that one box rather than trying to guess the spacing. */
    endRow(RowKind::rounding, box, field, hovered, false, lastChoice);
}

/* A row with no widget: the catalogue types that have no rounding choice (and
   the two conversions, whose saturation policy is worth saying out loud) show
   what their pins and flags mean.  It is read-only, so it never takes a click
   away from the rows that do have widgets. */
void rowInfo(const char* text) {
    tc::ui::textDisabled(text);
    const HitBox box = itemBox();
    endRow(RowKind::info, box, box, false);
}

void rowDisplayMode() {
    DisplayConfig config = displayDefault();
    (void)currentConfig(editor.instance, kDisplayId, &config, sizeof(config));
    tc::ui::text("显示方式");
    HitBox box = itemBox();
    HitBox field;
    bool hovered = false;
    const tcfp::DisplayMode current = displayModeOf(config.mode);
    for (uint8_t code = 0; code <= 3; ++code) {
        const tcfp::DisplayMode mode = static_cast<tcfp::DisplayMode>(code);
        tc::ui::sameLine();
        const bool picked = tc::ui::radioButton(displayModeName(mode), current == mode);
        const HitBox button = itemBox();
        box = unionBox(box, button);
        field = unionBox(field, button);
        if (tc::ui::isItemHovered(0)) hovered = true;
        if (picked) commitDisplayMode(code);
    }
    endRow(RowKind::display, box, field, hovered);
}

/* The drawer's slot.  Called only for this Mod's own selected component, with
   the game's own rows already laid out and the cursor under them. */
void panelDraw(void*, const TCFrame*, uint64_t instance, float width, float height) {
    if (!instance || !uiReady) return;
    const uint64_t type = typeOfInstance(instance);
    if (!type) return;
    /* The rows follow the *record*, not the click: they are read when the drawer
       turns to another instance, and re-read when the record moves under them -
       a saved value the loader restored after the rows were filled, an undo, a
       clone, or the same instance id on a board that was entered again.  A field
       the player is editing is left alone, so nothing typed is overwritten
       mid-edit. */
    const bool switched = editor.instance != instance;
    const bool moved = !switched && !tc::ui::isAnyItemActive() &&
                       !recordMatchesRows(instance, type);
    bool reloaded = false;
    if (switched || moved) {
        loadRows(instance, type);
        reloaded = true;
        /* One bounded line per switch: the drawer opened on one of this Mod's
           components, which is what the true-game case asserts (the editor rows
           themselves are pixels in the panel); and one per re-read, which is the
           evidence that a restored configuration reached the fields by itself.  The
           line carries what the fields now hold, so "the panel shows the saved
           value" is read off the log instead of inferred from the record. */
        static int logged = 0;
        if (logged < 6) {
            ++logged;
            char detail[256] = {};
            if (switched) {
                std::snprintf(
                    detail, sizeof(detail),
                    "float-ops: the drawer is editing instance 0x%llx (type 0x%llx, "
                    "label \"%s\", value \"%s\")",
                    static_cast<unsigned long long>(instance),
                    static_cast<unsigned long long>(type), editor.label, editor.value);
            } else {
                std::snprintf(
                    detail, sizeof(detail),
                    "float-ops: the drawer re-read instance 0x%llx (type 0x%llx, "
                    "label \"%s\", value \"%s\") because its record moved",
                    static_cast<unsigned long long>(instance),
                    static_cast<unsigned long long>(type), editor.label, editor.value);
            }
            note(detail);
        }
    }
    editor.rowCount = 0;
    pendingReport = PanelReport{};
    pendingReport.instance = instance;
    pendingReport.type = type;
    pendingReport.width = width;
    pendingReport.height = height;
    pendingReport.frame = tc::ui::frameCount();
    /* The game's own rows for a component sit in the upper half (title, then
       its description on the left, the pin picture on the right); the stock
       parts' own label/value fields are in the lower left, which is where the
       Mod's rows go, in the same column. */
    const tc::ui::Vec2 origin = contentOrigin();
    /* The drawer's own answer, taken before this Mod's row window is opened (the
       two differ, and that difference is the reason the row window exists). */
    pendingReport.panel_hovered = tc::ui::isWindowHovered(0);
    const float left = width * 0.08f;
    const float rowWidth = width * 0.42f;
    const float fieldWidth = rowWidth * 0.52f;
    const float top = origin.y + height * 0.52f;
    tc::ui::setNextWindowPos(tc::ui::Vec2{origin.x + left, top}, tc::ui::Cond_Always,
                             tc::ui::Vec2{0.f, 0.f});
    if (auto rows = tc::ui::Window("###float-ops-rows", nullptr, kRowWindowFlags)) {
        rowLabel(fieldWidth);
        if (const CatalogueInfo* info = catalogueInfo(type)) {
            /* Every catalogue type keeps its label in the same blob; the second
               row is the one choice it has - a rounding mode where the
               arithmetic rounds, its pins' meaning where it does not (plan
               7.1: a configured component shows its configuration on the
               board, and a component without a choice does not pretend to have
               one). */
            if (info->rounding)
                rowRounding(type);
            else if (info->hint && *info->hint)
                rowInfo(info->hint);
        } else {
            switch (type) {
                case kConstantId: rowValue(fieldWidth); break;
                case kAddId: rowRounding(type); break;
                case kDisplayId: rowDisplayMode(); break;
                default: break;
            }
        }
        if (!editor.message.empty()) tc::ui::textDisabled(editor.message.c_str());
        /* The row window's own answer: this is what a player's click reaches. */
        pendingReport.window_hovered = tc::ui::isWindowHovered(0);
        pendingReport.window_focused = tc::ui::isWindowFocused(0);
        pendingReport.rows_drawn = true;
    }
    /* What ImGui thinks of this window and of the rows it just drew.  The rows
       keep their own hit test either way, so this is evidence rather than
       control flow. */
    pendingReport.rowCount = editor.rowCount;
    pendingReport.any_active = tc::ui::isAnyItemActive();
    {
        MousePoint point{};
        if (getMousePos) getMousePos(&point);
        pendingReport.mouse_x = point.x;
        pendingReport.mouse_y = point.y;
    }
    /* The file is what a playtest clicks by, so it only has to be rewritten when
       the rows moved (or every few seconds, so a stale file cannot outlive a
       window resize). */
    /* A reload is written out even when the geometry did not move: the rows it
       just painted are the ones a re-entered board has to show, and the file is
       what the true-game case reads them back from. */
    if (reloaded || !sameGeometry(lastPanelReport, pendingReport) ||
        pendingReport.frame - lastPanelWriteFrame > 600) {
        lastPanelReport = pendingReport;
        lastPanelWriteFrame = pendingReport.frame;
        writePanelReport(pendingReport);
    }
    tracePanel(pendingReport);
}

}  // namespace

/* The editors are rows inside the game's own component drawer, not windows of
   this Mod's own: selection is what opens them, exactly like the stock parts'
   label and value fields.  The component *palette* is not this Mod's either -
   the float types carry a "浮点/" name prefix, which is how the game's own
   palette code files them under a folder it builds itself
   (docs/research/palette-categories.md). */
void registerEditors(const TCHost* host) {
    if (!host) return;
    /* One slot for all three types: the drawer shows one component at a time,
       and the rows follow the instance it hands over. */
    const int status = tc::ui::registerComponentPanel("editor", &panelDraw, nullptr, host);
    if (status == 0) {
        note("float-ops: the editor lives in the game's component drawer");
        return;
    }
    char detail[192] = {};
    std::snprintf(detail, sizeof(detail),
                  "float-ops: this loader has no component drawer slot (%d); "
                  "update the loader to edit values in the game's own panel", status);
    note(detail);
}

void frameCallback(void*, const TCFrame* frame) {
    (void)frame;
    /* The board first: a level that was left and entered again keeps its
       component ids, so everything the old board left behind has to go before
       the rows, the render boxes or a queued save can be mistaken for the new
       board's state. */
    serviceBoardChange();
    /* Storage commits and Constant binds happen while the game is still
       mutating/compiling the board.  Refresh on the following frame, when that
       work is visible to the simulation. */
    servicePendingBoardRefresh();
    servicePendingCircuitSave();
    if (!getMousePos) return;
    MousePoint point{};
    getMousePos(&point);
    refreshHover(point);
    /* No editor window: the rows in the game's own panel are the whole editor
       (see the row window in panelDraw).  Clicking a component still only
       selects it, the way it does for the stock parts. */
}

}  // namespace internals
}  // namespace floatops
