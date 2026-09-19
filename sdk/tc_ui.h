#ifndef TC_UI_H
#define TC_UI_H
/* UI helpers for native mod plugins.

   The loader does not own an ImGui context: plugins draw with the ImGui the game
   itself links (cimgui 1.92.6, exported by tc_game_engine.dll) from the
   on_frame callback.  This header resolves the exports that plugins need once,
   during tc_mod_load, and exposes typed wrappers so a plugin never hand-writes
   a name list or a function-pointer cast again.

   Resolution is strict on purpose: a name that this build does not export makes
   load() fail and report it, instead of failing later inside a draw call.  All
   names below were verified against the export table of this game build, and
   the ABI of the struct-taking entry points was checked in its machine code.

   Usage:

     #include "../../sdk/tc_ui.h"
     ...
     if (!tc::ui::load(host)) return 3;              // in tc_mod_load
     ...
     static void frame(void*, const TCFrame*) {      // in on_frame
         tc::ui::toggleHotkey(VK_F7, &show);          // optional
         if (auto panel = tc::ui::panel("My panel###TCMine", &show, {420, 320}, {24, 96}))
             tc::ui::text("hello");
     }

   Rules that still apply (see docs/sdk/host-api.md):
   - only call these from on_frame (main/render thread, at most once per frame);
   - never create a second ImGui context;
   - Begin/End and style/font stacks must stay balanced, which the Window guard
     below enforces for window scopes.
*/
#include "tc_mod_api.h"
#include <cstring>
#include <cstddef>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace tc {
namespace ui {

/* Layout-compatible with ImGui's ImVec2 / ImVec4.  Eight-byte aggregates are
   passed in an integer register and four-float aggregates by reference under
   the x64 ABI, which is what the engine's own entry points expect; declaring
   them by value here is therefore correct. */
struct Vec2 { float x, y; };
struct Vec4 { float x, y, z, w; };

/* ImGuiWindowFlags subset.  Values are ImGui's own. */
enum : int {
    Window_None = 0,
    Window_NoTitleBar = 1 << 0,
    Window_NoResize = 1 << 1,
    Window_NoMove = 1 << 2,
    Window_NoScrollbar = 1 << 3,
    Window_NoScrollWithMouse = 1 << 4,
    Window_NoCollapse = 1 << 5,
    Window_AlwaysAutoResize = 1 << 6,
    Window_NoBackground = 1 << 7,
    Window_NoSavedSettings = 1 << 8,
    Window_NoMouseInputs = 1 << 9,
    Window_NoFocusOnAppearing = 1 << 12,
    Window_NoBringToFrontOnFocus = 1 << 13,
    Window_NoDecoration = Window_NoTitleBar | Window_NoResize |
                          Window_NoScrollbar | Window_NoCollapse,
};

/* ImGuiCond: when a size/position request is applied. */
enum : int { Cond_Always = 1, Cond_Once = 2, Cond_FirstUseEver = 4, Cond_Appearing = 8 };

/* Mouse buttons. */
enum : int { Mouse_Left = 0, Mouse_Right = 1, Mouse_Middle = 2 };

/* ImGuiKey values of this build (ImGuiKey_ enum, 1.92).  The entries are
   sequential, so each range below is written once and continues implicitly.
   The values used by the playtests - Tab, A, Escape, F5 - were confirmed
   against real key messages, not only read out of the executable. */
enum : int {
    Key_Tab = 512,
    Key_LeftArrow, Key_RightArrow, Key_UpArrow, Key_DownArrow,
    Key_PageUp, Key_PageDown, Key_Home, Key_End, Key_Insert, Key_Delete,
    Key_Backspace, Key_Space, Key_Enter, Key_Escape,
    Key_0 = 536, Key_1, Key_2, Key_3, Key_4, Key_5, Key_6, Key_7, Key_8, Key_9,
    Key_A = 546, Key_B, Key_C, Key_D, Key_E, Key_F, Key_G, Key_H, Key_I, Key_J,
    Key_K, Key_L, Key_M, Key_N, Key_O, Key_P, Key_Q, Key_R, Key_S, Key_T, Key_U,
    Key_V, Key_W, Key_X, Key_Y, Key_Z,
    Key_F1 = 572, Key_F2, Key_F3, Key_F4, Key_F5, Key_F6, Key_F7, Key_F8, Key_F9,
    Key_F10, Key_F11, Key_F12
};

/* Window flags used by the bundled compact toolbars (cycle-guard,
   mod-inspector): borderless, non-movable, auto-sized, not persisted. */
inline constexpr int kToolbarFlags = Window_NoTitleBar | Window_NoResize | Window_NoMove |
                                     Window_NoScrollbar | Window_NoCollapse |
                                     Window_AlwaysAutoResize | Window_NoSavedSettings;

/* Font scale the bundled panels use; the game's default text is large for
   panels this size.  Pass 0 to panel() to leave the scale untouched. */
inline constexpr float kPanelFontScale = 0.62f;

/* Resolved engine entry points.  Populated by load(); null until then, so the
   wrappers below are safe no-ops if a plugin skipped load(). */
struct Table {
    bool (*begin)(const char*, bool*, int);
    void (*end)();
    void (*setNextWindowPos)(Vec2, int, Vec2);
    void (*setNextWindowSize)(Vec2, int);
    void (*setNextWindowBgAlpha)(float);
    void (*setWindowFontScale)(float);
    float (*getWindowWidth)();
    float (*getWindowHeight)();
    void* (*getMainViewport)();
    int (*getFrameCount)();
    double (*getTime)();
    void (*setCursorPos)(Vec2);
    void (*setNextItemWidth)(float);
    void (*textUnformatted)(const char*, const char*);
    void (*textDisabled)(const char*, ...);
    void (*separator)();
    void (*sameLine)(float, float);
    void (*newLine)();
    void (*spacing)();
    void (*pushTextWrapPos)(float);
    void (*popTextWrapPos)();
    bool (*button)(const char*, Vec2);
    bool (*smallButton)(const char*);
    bool (*invisibleButton)(const char*, Vec2, int);
    bool (*checkbox)(const char*, bool*);
    bool (*radioButton)(const char*, bool);
    void (*beginDisabled)(bool);
    void (*endDisabled)();
    bool (*colorPicker3)(const char*, float*, int);
    bool (*colorButton)(const char*, Vec4, int, Vec2);
    bool (*sliderFloat)(const char*, float*, float, float, const char*, int);
    bool (*inputText)(const char*, char*, unsigned long long, int, void*, void*);
    bool (*beginChild)(const char*, Vec2, int, int);
    void (*endChild)();
    void (*pushId)(const char*);
    void (*popId)();
    void (*openPopup)(const char*, int);
    bool (*beginPopup)(const char*, int);
    bool (*beginPopupModal)(const char*, bool*, int);
    void (*closeCurrentPopup)();
    void (*endPopup)();
    bool (*isItemDeactivatedAfterEdit)();
    bool (*isAnyItemActive)();
    bool (*isMouseDown)(int);
    bool (*isMouseClicked)(int, bool);
    bool (*isItemHovered)(int);
    bool (*isWindowHovered)(int);
    /* Entry points that return an ImVec2 take a hidden out pointer in this
       build: calling them as "return two floats" hands back whatever was in
       the integer return registers (observed: mouse=0,0 then a garbage
       -2147483648).  They are declared with the out parameter instead, which
       is the ABI the engine actually uses. */
    void (*getMousePos)(Vec2*);
    void (*getItemRectSize)(Vec2*);
    /* Screen position of the layout cursor: this one does work (verified in
       tc_ui_draw.h's real-engine tests), so a plugin can turn its own layout
       coordinates into absolute screen coordinates - useful for hit tests,
       overlays and for telling a test where a widget really is. */
    void (*getCursorScreenPos)(Vec2*);
    /* Note: igGetCursorPos, igGetWindowPos and igGetItemRectMin/Max are
       exported by this build but do not deliver a usable value (called with an
       out pointer they return 0x80000000), so they are deliberately absent
       rather than exposed half-working. */
    /* The scalar variants do work, and together they give the cursor position
       the ImVec2 versions cannot. */
    float (*getCursorPosX)();
    float (*getCursorPosY)();
    /* Window-level mouse state.  Single int in, bool out: no ABI ambiguity. */
    bool (*isWindowFocused)(int);
    float (*getFrameHeight)();
    /* Keyboard.  Verified with real key messages in
       tests/ui-keyboard-playtest.ps1; see the note above inputText() for the
       text-input caveat this build has. */
    bool (*isKeyPressed)(int, bool);
    bool (*isKeyDown)(int);
    bool (*isKeyReleased)(int);
    float (*getKeyPressedAmount)(int, float, float);
    void (*setKeyboardFocusHere)(int);
    bool (*isItemFocused)();
};

inline Table& table() { static Table value{}; return value; }
inline std::string& missingList() { static std::string value; return value; }

#define TC_UI_BIND(field, name)                                                        \
    do {                                                                               \
        void* tc_ui_entry = host->engine_proc(host->context, name);                     \
        if (!tc_ui_entry) {                                                            \
            if (!missingList().empty()) missingList() += ", ";                          \
            missingList() += name;                                                     \
        }                                                                              \
        std::memcpy(&table().field, &tc_ui_entry, sizeof(tc_ui_entry));                 \
    } while (0)

/* Resolves every entry point this header wraps.  Call once during tc_mod_load.
   Returns false when an export is missing; the names are then in missing() and
   were written to the loader log. */
inline bool load(const TCHost* host) {
    missingList().clear();
    if (!host || !host->engine_proc || !host->context) {
        missingList() = "engine_proc unavailable";
        return false;
    }
    TC_UI_BIND(begin, "igBegin");
    TC_UI_BIND(end, "igEnd");
    TC_UI_BIND(setNextWindowPos, "igSetNextWindowPos");
    TC_UI_BIND(setNextWindowSize, "igSetNextWindowSize");
    TC_UI_BIND(setNextWindowBgAlpha, "igSetNextWindowBgAlpha");
    TC_UI_BIND(setWindowFontScale, "igSetWindowFontScale");
    TC_UI_BIND(getWindowWidth, "igGetWindowWidth");
    TC_UI_BIND(getWindowHeight, "igGetWindowHeight");
    TC_UI_BIND(getMainViewport, "igGetMainViewport");
    TC_UI_BIND(getFrameCount, "igGetFrameCount");
    TC_UI_BIND(getTime, "igGetTime");
    TC_UI_BIND(setCursorPos, "igSetCursorPos");
    TC_UI_BIND(setNextItemWidth, "igSetNextItemWidth");
    TC_UI_BIND(textUnformatted, "igTextUnformatted");
    TC_UI_BIND(textDisabled, "igTextDisabled");
    TC_UI_BIND(separator, "igSeparator");
    TC_UI_BIND(sameLine, "igSameLine");
    TC_UI_BIND(newLine, "igNewLine");
    TC_UI_BIND(spacing, "igSpacing");
    TC_UI_BIND(pushTextWrapPos, "igPushTextWrapPos");
    TC_UI_BIND(popTextWrapPos, "igPopTextWrapPos");
    TC_UI_BIND(button, "igButton");
    TC_UI_BIND(smallButton, "igSmallButton");
    TC_UI_BIND(invisibleButton, "igInvisibleButton");
    TC_UI_BIND(checkbox, "igCheckbox");
    TC_UI_BIND(radioButton, "igRadioButton_Bool");
    TC_UI_BIND(beginDisabled, "igBeginDisabled");
    TC_UI_BIND(endDisabled, "igEndDisabled");
    TC_UI_BIND(colorPicker3, "igColorPicker3");
    TC_UI_BIND(colorButton, "igColorButton");
    TC_UI_BIND(sliderFloat, "igSliderFloat");
    TC_UI_BIND(inputText, "igInputText");
    TC_UI_BIND(beginChild, "igBeginChild_Str");
    TC_UI_BIND(endChild, "igEndChild");
    TC_UI_BIND(pushId, "igPushID_Str");
    TC_UI_BIND(popId, "igPopID");
    TC_UI_BIND(openPopup, "igOpenPopup_Str");
    TC_UI_BIND(beginPopup, "igBeginPopup");
    TC_UI_BIND(beginPopupModal, "igBeginPopupModal");
    TC_UI_BIND(closeCurrentPopup, "igCloseCurrentPopup");
    TC_UI_BIND(endPopup, "igEndPopup");
    TC_UI_BIND(isItemDeactivatedAfterEdit, "igIsItemDeactivatedAfterEdit");
    TC_UI_BIND(isAnyItemActive, "igIsAnyItemActive");
    TC_UI_BIND(isMouseDown, "igIsMouseDown_Nil");
    TC_UI_BIND(isMouseClicked, "igIsMouseClicked_Bool");
    TC_UI_BIND(isItemHovered, "igIsItemHovered");
    TC_UI_BIND(getMousePos, "igGetMousePos");
    TC_UI_BIND(getItemRectSize, "igGetItemRectSize");
    TC_UI_BIND(getCursorScreenPos, "igGetCursorScreenPos");
    TC_UI_BIND(getCursorPosX, "igGetCursorPosX");
    TC_UI_BIND(getCursorPosY, "igGetCursorPosY");
    TC_UI_BIND(isWindowHovered, "igIsWindowHovered");
    TC_UI_BIND(isWindowFocused, "igIsWindowFocused");
    TC_UI_BIND(getFrameHeight, "igGetFrameHeight");
    TC_UI_BIND(isKeyPressed, "igIsKeyPressed_Bool");
    TC_UI_BIND(isKeyDown, "igIsKeyDown_Nil");
    TC_UI_BIND(isKeyReleased, "igIsKeyReleased_Nil");
    TC_UI_BIND(getKeyPressedAmount, "igGetKeyPressedAmount");
    TC_UI_BIND(setKeyboardFocusHere, "igSetKeyboardFocusHere");
    TC_UI_BIND(isItemFocused, "igIsItemFocused");
#undef TC_UI_BIND
    if (!missingList().empty()) {
        if (host->log)
            host->log(host->context,
                      ("UI: missing engine exports: " + missingList()).c_str());
        return false;
    }
    return true;
}

/* True once load() succeeded. */
inline bool ready() { return table().begin != nullptr; }

/* Comma separated names that load() could not resolve ("" when complete). */
inline const std::string& missing() { return missingList(); }

/* Namespace controls in a shared page: mod ID, then page ID. */
class Id {
    void (*pop_)() = nullptr;
 public:
    explicit Id(const char* id) {
        auto& api = table();
        if (id && api.pushId && api.popId) {
            pop_ = api.popId;
            api.pushId(id);
        }
    }
    ~Id() { if (pop_) pop_(); }
    Id(const Id&) = delete;
    Id& operator=(const Id&) = delete;
    Id(Id&&) = delete;
    Id& operator=(Id&&) = delete;
};

/* ---------------------------------------------------------------------------
   Host UI pages
   ------------------------------------------------------------------------ */

/* Registers a page for this plugin during tc_mod_load.  The host owns the
   container (frame, title, "back" button) and namespaces everything with
   PushID(mod id), PushID(page id) around draw(), so pages from different
   plugins cannot collide - reuse the same widget labels freely.

   draw() runs on the main/render thread inside the host's window: draw content
   only.  The window, the ID scopes and the scrollable content region are the
   host's; the content_width/content_height arguments are that region's
   interior size, in pixels, so a layout can size itself without guessing.
   Do not open a window, do not call End on the host's window, and never let an
   exception escape.

   Returns:
     0  registered (the page appears once this plugin loads successfully)
    -1  this loader has no page registry (older loader); the plugin still runs
    -2  invalid definition (null pointers, empty/oversized page_id)
    -3  the same page_id was already registered by this plugin
    -4  this plugin has reached the host's page limit

   The title is UTF-8 and may be empty.  page_id must be stable and unique
   within the plugin; it is the page's identity, not the visible label. */
inline int registerPage(const char* page_id, const char* title,
                        void (*draw)(void*, const TCFrame*, float, float),
                        void* user = nullptr, const TCHost* host = nullptr) {
    if (!host || !page_id || !draw) return -2;
    /* Host struct sizes are versioned: an older loader provides only the base
       fields, and reading past its size would read unrelated memory. */
    if (host->size < offsetof(TCHost, register_ui_page) + sizeof(void*) ||
        !host->register_ui_page)
        return -1;
    const std::size_t idLength = std::strlen(page_id);
    if (idLength == 0 || idLength >= 64 || std::strstr(page_id, "###")) return -2;
    TCUiPageDefinition definition{};
    definition.size = sizeof(definition);
    definition.page_id = page_id;
    definition.title = title;
    definition.draw = draw;
    definition.user = user;
    return host->register_ui_page(host->context, &definition);
}

/* ---------------------------------------------------------------------------
   Host UI slots
   ------------------------------------------------------------------------ */

/* Registers a panel for this plugin on the circuit board (right edge, above
   the canvas).  Unlike a page - which only exists on the main menu - a slot
   belongs to a screen the game builds itself, so the host:

   * draws it inside the board's own window, only while the board draws, so
     closing the level closes the panel with nothing to clean up;
   * draws it before the game samples its mouse state for the frame, so a
     press, a click or a drag that starts inside the panel belongs to the
     panel and never reaches the circuit board behind it;
   * wraps draw() in PushID(mod id) / PushID(slot id), so two plugins can use
     the same slot_id and the same widget labels without colliding;
   * owns the frame, the title line and the content region.

   draw() runs on the main/render thread inside the host's panel: draw content
   only, do not open a window, do not call End, never throw.  The width and
   height arguments are the usable interior size in pixels.

   preferred_width/preferred_height are in pixels and 0 means "host default";
   the host may shrink a panel to fit the window.  Both are only a request:
   read the numbers handed to draw() instead of assuming the panel size.

   Returns:
     0  registered (the panel appears while the board is on screen)
    -1  this loader has no slot registry (older loader); the plugin still runs
    -2  invalid definition (null pointers, empty/oversized slot_id, bad kind)
    -3  the same slot_id was already registered by this plugin
    -4  this plugin has reached the host's slot limit */
inline int registerBoardPanel(const char* slot_id, const char* title,
                              void (*draw)(void*, const TCFrame*, float, float),
                              void* user = nullptr, const TCHost* host = nullptr,
                              float preferred_width = 0.f,
                              float preferred_height = 0.f) {
    if (!host || !slot_id || !draw) return -2;
    /* Host struct sizes are versioned: an older loader provides only the base
       fields, and reading past its size would read unrelated memory. */
    if (host->size < offsetof(TCHost, register_ui_slot) + sizeof(void*) ||
        !host->register_ui_slot)
        return -1;
    const std::size_t idLength = std::strlen(slot_id);
    if (idLength == 0 || idLength >= 64 || std::strstr(slot_id, "###")) return -2;
    TCUiSlotDefinition definition{};
    definition.size = sizeof(definition);
    definition.kind = TC_UI_SLOT_BOARD_SIDE;
    definition.slot_id = slot_id;
    definition.title = title;
    definition.draw = draw;
    definition.user = user;
    definition.preferred_width = preferred_width;
    definition.preferred_height = preferred_height;
    return host->register_ui_slot(host->context, &definition);
}

/* A tool in the game's own tool column (see TC_UI_SLOT_BOARD_TOOLBAR).  The
   draw callback is called from inside that column's child window, once per
   frame while a board is on screen, with the column's available width/height:
   draw one compact control there and expand on hover with a popup or tooltip. */
inline int registerBoardToolbar(const char* slot_id,
                                void (*draw)(void*, const TCFrame*, float, float),
                                void* user = nullptr, const TCHost* host = nullptr,
                                float preferred_width = 0.f,
                                float preferred_height = 0.f) {
    if (!host || !slot_id || !draw) return -2;
    if (host->size < offsetof(TCHost, register_ui_slot) + sizeof(void*) ||
        !host->register_ui_slot)
        return -1;
    const std::size_t idLength = std::strlen(slot_id);
    if (idLength == 0 || idLength >= 64 || std::strstr(slot_id, "###")) return -2;
    TCUiSlotDefinition definition{};
    definition.size = sizeof(definition);
    definition.kind = TC_UI_SLOT_BOARD_TOOLBAR;
    definition.slot_id = slot_id;
    definition.title = slot_id;
    definition.draw = draw;
    definition.user = user;
    definition.preferred_width = preferred_width;
    definition.preferred_height = preferred_height;
    return host->register_ui_slot(host->context, &definition);
}

/* ---------------------------------------------------------------------------
   Window and panel scopes
   ------------------------------------------------------------------------ */

/* RAII window.  ImGui requires a matching End() for every Begin() call
   regardless of its return value, so the guard tracks "Begin was called"
   separately from "the window is visible"; operator bool reports the latter. */
class Window {
public:
    Window(const char* name, bool* open = nullptr, int flags = Window_None) {
        called_ = table().begin != nullptr;
        if (called_) visible_ = table().begin(name, open, flags);
    }
    Window(Window&& other) noexcept : called_(other.called_), visible_(other.visible_) {
        other.called_ = false;
        other.visible_ = false;
    }
    Window& operator=(Window&&) = delete;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window() { if (called_) table().end(); }
    explicit operator bool() const { return visible_; }
    bool shown() const { return visible_; }

private:
    bool called_ = false;
    bool visible_ = false;
};

/* RAII child region, same balance rule as Window (EndChild is called whenever
   BeginChild was). */
class Child {
public:
    Child(const char* id, Vec2 size = {0, 0}, int childFlags = 0, int windowFlags = 0) {
        called_ = table().beginChild != nullptr;
        if (called_) visible_ = table().beginChild(id, size, childFlags, windowFlags);
    }
    Child(Child&& other) noexcept : called_(other.called_), visible_(other.visible_) {
        other.called_ = false;
        other.visible_ = false;
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() { if (called_) table().endChild(); }
    explicit operator bool() const { return visible_; }

private:
    bool called_ = false;
    bool visible_ = false;
};

/* Places, sizes and scales a panel, then opens it.  size.x/pos.x of 0 (or a
   negative position) leave that axis to ImGui.  fontScale <= 0 keeps the
   current scale.  cond is applied to both the size and the position request.

   The position request always passes an explicit pivot: the engine's
   igSetNextWindowPos reads it from the third argument register, so calling it
   with two arguments passes whatever was in that register. */
inline Window panel(const char* title, bool* open, Vec2 size = {0, 0},
                    Vec2 position = {-1, -1}, float fontScale = kPanelFontScale,
                    int flags = Window_None, int cond = Cond_Once) {
    if ((size.x != 0 || size.y != 0) && table().setNextWindowSize)
        table().setNextWindowSize(size, cond);
    if (position.x >= 0 && position.y >= 0 && table().setNextWindowPos)
        table().setNextWindowPos(position, cond, Vec2{0, 0});
    Window window(title, open, flags);
    if (window && fontScale > 0 && table().setWindowFontScale)
        table().setWindowFontScale(fontScale);
    return window;
}

/* ---------------------------------------------------------------------------
   Text and layout
   ------------------------------------------------------------------------ */

/* UTF-8, no format interpretation - the safe counterpart of the game's own
   formatted text entry points. */
inline void text(const char* value) {
    if (table().textUnformatted) table().textUnformatted(value, nullptr);
}
inline void text(const std::string& value) { text(value.c_str()); }

/* Greyed-out text.  The engine entry point is variadic, so the string is passed
   through "%s" and can never be interpreted as a format string itself. */
inline void textDisabled(const char* value) {
    if (table().textDisabled) table().textDisabled("%s", value);
}
inline void textDisabled(const std::string& value) { textDisabled(value.c_str()); }

/* Wraps following text at the current cursor; the guard pops on scope exit. */
class TextWrap {
public:
    explicit TextWrap(float localPosX = 0.f) : pushed_(table().pushTextWrapPos != nullptr) {
        if (pushed_) table().pushTextWrapPos(localPosX);
    }
    TextWrap(TextWrap&& other) noexcept : pushed_(other.pushed_) { other.pushed_ = false; }
    TextWrap(const TextWrap&) = delete;
    TextWrap& operator=(const TextWrap&) = delete;
    ~TextWrap() { if (pushed_) table().popTextWrapPos(); }

private:
    bool pushed_ = false;
};

/* Disables the widgets inside the scope (greyed out, not clickable). */
class Disabled {
public:
    explicit Disabled(bool disabled = true) : applied_(table().beginDisabled != nullptr) {
        if (applied_) table().beginDisabled(disabled);
    }
    Disabled(Disabled&& other) noexcept : applied_(other.applied_) { other.applied_ = false; }
    Disabled(const Disabled&) = delete;
    Disabled& operator=(const Disabled&) = delete;
    ~Disabled() { if (applied_) table().endDisabled(); }

private:
    bool applied_ = false;
};

inline void separator() { if (table().separator) table().separator(); }
inline void newLine() { if (table().newLine) table().newLine(); }
inline void spacing() { if (table().spacing) table().spacing(); }
inline void sameLine(float offsetFromStartX = 0.f, float spacing = -1.f) {
    if (table().sameLine) table().sameLine(offsetFromStartX, spacing);
}
inline void setCursorPos(Vec2 localPos) {
    if (table().setCursorPos) table().setCursorPos(localPos);
}
inline void setNextItemWidth(float width) {
    if (table().setNextItemWidth) table().setNextItemWidth(width);
}
inline void setNextWindowBgAlpha(float alpha) {
    if (table().setNextWindowBgAlpha) table().setNextWindowBgAlpha(alpha);
}
inline void setNextWindowPos(Vec2 position, int cond = Cond_Always, Vec2 pivot = {0, 0}) {
    if (table().setNextWindowPos) table().setNextWindowPos(position, cond, pivot);
}
inline void setNextWindowSize(Vec2 size, int cond = Cond_Always) {
    if (table().setNextWindowSize) table().setNextWindowSize(size, cond);
}
inline float windowWidth() { return table().getWindowWidth ? table().getWindowWidth() : 0.f; }

/* Popups: the game's own tools expand this way (a small window next to the
   control, closed by clicking away or pressing Escape).  Open one on hover and
   draw its contents inside a beginPopup block. */
inline void openPopup(const char* name, int flags = 0) {
    if (table().openPopup) table().openPopup(name, flags);
}
inline bool beginPopup(const char* name, int flags = 0) {
    return table().beginPopup && table().beginPopup(name, flags);
}
inline void endPopup() {
    if (table().endPopup) table().endPopup();
}
inline float windowHeight() { return table().getWindowHeight ? table().getWindowHeight() : 0.f; }
inline int frameCount() { return table().getFrameCount ? table().getFrameCount() : 0; }

/* ---------------------------------------------------------------------------
   Widgets
   ------------------------------------------------------------------------ */

inline bool button(const char* label, Vec2 size = {0, 0}) {
    return table().button && table().button(label, size);
}
inline bool smallButton(const char* label) {
    return table().smallButton && table().smallButton(label);
}
/* Hit box with no visual: the game's own menu entries are built from this.
   Useful for making an area clickable without drawing anything - and as a
   probe for "is the mouse anywhere inside my region". */
inline bool invisibleButton(const char* id, Vec2 size, int flags = 0) {
    return table().invisibleButton && table().invisibleButton(id, size, flags);
}
inline bool checkbox(const char* label, bool* value) {
    return table().checkbox && table().checkbox(label, value);
}
inline bool radioButton(const char* label, bool active) {
    return table().radioButton && table().radioButton(label, active);
}
inline bool sliderFloat(const char* label, float* value, float min, float max,
                        const char* format = "%.3f", int flags = 0) {
    return table().sliderFloat && table().sliderFloat(label, value, min, max, format, flags);
}
/* Single-line text field.  Verified on the real engine
   (tests/ui-keyboard-playtest.ps1): typing with key presses delivers exactly
   one character each - the game's message loop translates the key into WM_CHAR
   the same way Windows does for every application - a bare WM_CHAR delivers
   one character, and a non-ASCII code unit (你, via WM_CHAR; 好, via
   WM_IME_CHAR) arrives as UTF-8 in the buffer.  The playtest asserts the exact
   buffer after each step, so a doubled or dropped character would fail it. */
inline bool inputText(const char* label, char* buffer, unsigned long long capacity,
                      int flags = 0) {
    return table().inputText &&
           table().inputText(label, buffer, capacity, flags, nullptr, nullptr);
}
inline bool colorPicker3(const char* label, float* rgb, int flags = 0) {
    return table().colorPicker3 && table().colorPicker3(label, rgb, flags);
}
inline bool colorButton(const char* id, Vec4 color, int flags = 0, Vec2 size = {0, 0}) {
    return table().colorButton && table().colorButton(id, color, flags, size);
}
inline bool isItemHovered(int flags = 0) {
    return table().isItemHovered && table().isItemHovered(flags);
}
inline void beginDisabled(bool disabled = true) {
    if (table().beginDisabled) table().beginDisabled(disabled);
}
inline void endDisabled() { if (table().endDisabled) table().endDisabled(); }
inline bool isItemDeactivatedAfterEdit() {
    return table().isItemDeactivatedAfterEdit && table().isItemDeactivatedAfterEdit();
}
inline bool isAnyItemActive() {
    return table().isAnyItemActive && table().isAnyItemActive();
}
inline bool isMouseDown(int buttonIndex = Mouse_Left) {
    return table().isMouseDown && table().isMouseDown(buttonIndex);
}
inline bool isMouseClicked(int buttonIndex = Mouse_Left, bool repeat = false) {
    return table().isMouseClicked && table().isMouseClicked(buttonIndex, repeat);
}
inline Vec2 mousePos() {
    Vec2 value{0, 0};
    if (table().getMousePos) table().getMousePos(&value);
    return value;
}
inline Vec2 itemRectSize() {
    Vec2 value{0, 0};
    if (table().getItemRectSize) table().getItemRectSize(&value);
    return value;
}
/* Where the next widget will be drawn, in screen (viewport) pixels.  Unlike the
   window-local cursor readers this one works in this build, so it is the way to
   get absolute coordinates from inside a page or panel. */
inline Vec2 cursorScreenPos() {
    Vec2 value{0, 0};
    if (table().getCursorScreenPos) table().getCursorScreenPos(&value);
    return value;
}
/* Cursor inside the current window, as two scalars.  igGetCursorPosX/Y return a
   single float each, so unlike the ImVec2 variant they have no return-value
   ambiguity in this build and are safe to call. */
inline float cursorPosX() { return table().getCursorPosX ? table().getCursorPosX() : 0.f; }
inline float cursorPosY() { return table().getCursorPosY ? table().getCursorPosY() : 0.f; }
inline Vec2 cursorPos() { return Vec2{cursorPosX(), cursorPosY()}; }
/* Whether the current window is under the mouse (flags 0 = the current
   window).  This is the coarse question "does this window get mouse input at
   all", which is different from any single item being hovered. */
inline bool isWindowHovered(int flags = 0) {
    return table().isWindowHovered && table().isWindowHovered(flags);
}
/* Close the popup that is currently being drawn (used by HoverPopup). */
inline void closeCurrentPopup() {
    if (table().closeCurrentPopup) table().closeCurrentPopup();
}
/* Seconds since the engine started, as the frame callback reports it. */
inline bool isWindowFocused(int flags = 0) {
    return table().isWindowFocused && table().isWindowFocused(flags);
}
inline float frameHeight() { return table().getFrameHeight ? table().getFrameHeight() : 0.f; }

/* ---------------------------------------------------------------------------
   Keyboard (ImGui keys)
   ------------------------------------------------------------------------ */

/* Keys as ImGui sees them: the same values the game's widgets use, delivered
   through the game's own input path.  They are a different thing from the
   physical hotkeys further down: those read the OS state and keep working
   while ImGui owns the keyboard, while these follow ImGui's focus (a field
   that is being typed into swallows the key).  Use the Key_* enum above.

   Verified on the real engine, including that Tab/Enter/Escape/letters arrive
   with the values the enum claims (tests/ui-keyboard-playtest.ps1). */
namespace keys {
inline bool down(int key) { return table().isKeyDown && table().isKeyDown(key); }
/* True on the frame the key goes down (repeat = also on auto-repeat). */
inline bool pressed(int key, bool repeat = false) {
    return table().isKeyPressed && table().isKeyPressed(key, repeat);
}
inline bool released(int key) {
    return table().isKeyReleased && table().isKeyReleased(key);
}
/* Number of presses within the repeat window: 0, 1 or more.  Handy for
   "how many times was PageDown hit this frame" style scrolling. */
inline float amount(int key, float repeatDelay = 0.25f, float repeatRate = 0.05f) {
    return table().getKeyPressedAmount ? table().getKeyPressedAmount(key, repeatDelay, repeatRate)
                                       : 0.f;
}
}  // namespace keys
/* Gives the keyboard focus to the next widget in this scope (offset 0 = the
   next one submitted, 1 = the one after that).  Call it right before the item
   that should take focus; inside a page or panel this is how a plugin moves
   the caret between its own fields. */
inline void setKeyboardFocusHere(int offset = 0) {
    if (table().setKeyboardFocusHere) table().setKeyboardFocusHere(offset);
}
/* True when the item just submitted owns the keyboard focus. */
inline bool isItemFocused() { return table().isItemFocused && table().isItemFocused(); }

/* ---------------------------------------------------------------------------
   Viewport
   ------------------------------------------------------------------------ */

/* Main viewport size.  The layout of ImGuiViewport is fixed for this build:
   Pos at +8, Size at +16 (two floats), which is what mod-inspector reads too. */
inline Vec2 viewportSize() {
    if (!table().getMainViewport) return Vec2{0, 0};
    const float* data = static_cast<const float*>(table().getMainViewport());
    return data ? Vec2{data[4], data[5]} : Vec2{0, 0};
}

/* Places a panel against the right edge of the viewport, 12 px below the top -
   the position the bundled toolbars use. */
inline Vec2 topRight(float width, float margin = 12.f) {
    const Vec2 viewport = viewportSize();
    return Vec2{viewport.x - width - margin, margin};
}

/* ---------------------------------------------------------------------------
   Hotkeys
   ------------------------------------------------------------------------ */

#if defined(_WIN32)
/* Physical key state; works even while ImGui owns the keyboard.  The key still
   reaches the game, so prefer keys the game does not use. */
inline bool keyDown(int virtualKey) {
    return virtualKey > 0 && virtualKey < 256 &&
           (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

/* Edge triggered: true on the frame the key goes down, then false until it is
   released and pressed again. */
inline bool keyPressed(int virtualKey) {
    if (virtualKey <= 0 || virtualKey > 255) return false;
    static bool wasDown[256]{};
    const bool now = keyDown(virtualKey);
    const bool pressed = now && !wasDown[virtualKey];
    wasDown[virtualKey] = now;
    return pressed;
}

/* Flips *open when the key is pressed.  Returns true if it flipped. */
inline bool toggleHotkey(int virtualKey, bool* open) {
    if (!open || !keyPressed(virtualKey)) return false;
    *open = !*open;
    return true;
}
#endif

}  // namespace ui
}  // namespace tc

#endif  // TC_UI_H
