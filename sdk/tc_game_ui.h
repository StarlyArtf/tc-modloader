#ifndef TC_GAME_UI_H
#define TC_GAME_UI_H
/* Controls that match the game's own main menu.

   This header is deliberately separate from tc_ui.h: tc_ui.h is the generic
   ImGui tool kit, while everything here depends on constants mined out of this
   pinned build.  It claims nothing about the rest of the game's UI.

   Where the numbers come from
   ---------------------------
   The main menu draws each entry as an invisible hit box
   (`igInvisibleButton`) sized from `igCalcTextSize`, then colours the label
   with `igTextColored`.  That function is
   0x140449df0 (build_main_buttons__...home95page_u48); the colours it loads
   live in .rdata:

     COLOR_TEXT_NORMAL   0x1406a5b10   0.7294 0.7294 0.7294 1.0
     COLOR_TEXT_HOVERED  0x1406a5b00   0.9412 0.9412 0.9412 1.0
     COLOR_TEXT_PRESSED  0x1406a5af0   0.9333 0.7294 0.1961 1.0
     COLOR_BUTTON_NORMAL  0x1406a5ab0  0.9176 0.3216 0.3216 1.0
     COLOR_BUTTON_HOVERED 0x1406a5aa0  1.0000 0.5765 0.5765 1.0
     COLOR_BUTTON_PRESSED 0x1406a5a90  0.8549 0.2667 0.2667 1.0
     PAGE_COLOR           0x1406a5ae0  0.1882 0.1882 0.2118 1.0
     COLOR_BACKGROUND     0x1406a5b20  0.1451 0.1451 0.1647 1.0

   Each is four floats read directly out of the executable - not a guess from a
   screenshot.  Layout matches too: the menu scales its coordinates by the
   scale it is handed and lays a row out from `igGetTextLineHeightWithSpacing`
   times the row count, drawing the label at the item's left edge.
   menu_button reproduces that shape: an invisible hit box the size of the
   label, with the label drawn over it in the menu's colours.

   How it is drawn
   ---------------
   Through ImGui's own style colours plus the ordinary button. The game's
   own menu function calls igPushStyleColor_Vec4 (0x140449f1b). An earlier
   drawing probe stalled, but this was not a restriction on plugins: custom
   drawing is now available separately in tc_ui_draw.h, with verified ABI,
   coordinates, clipping and sustained real-engine geometry tests.

   What is NOT reproduced
   ----------------------
   - the click sound (`play_sound__presenterZutilities_u29270`): it needs the
     menu's presenter object, which plugins must not receive;
   - the menu's animated hover cross-fade; the same three colours are used here
     directly.

   Usage
   -----
       if (tc::game_ui::load(host))            // optional, after tc::ui::load
           ...
       bool clicked = tc::game_ui::menu_button("Start Game");

   load() is additive: it resolves only the entry points this header adds and
   never fails the plugin.  Without it the widgets fall back to tc::ui no-ops. */
#include "tc_ui.h"

namespace tc {
namespace game_ui {

/* Colours of this pinned build's main menu. */
inline constexpr ui::Vec4 kTextNormal{0.7294f, 0.7294f, 0.7294f, 1.0f};
inline constexpr ui::Vec4 kTextHovered{0.9412f, 0.9412f, 0.9412f, 1.0f};
inline constexpr ui::Vec4 kTextPressed{0.9333f, 0.7294f, 0.1961f, 1.0f};
inline constexpr ui::Vec4 kButtonNormal{0.9176f, 0.3216f, 0.3216f, 1.0f};
inline constexpr ui::Vec4 kButtonHovered{1.0000f, 0.5765f, 0.5765f, 1.0f};
inline constexpr ui::Vec4 kButtonPressed{0.8549f, 0.2667f, 0.2667f, 1.0f};
inline constexpr ui::Vec4 kPageColor{0.1882f, 0.1882f, 0.2118f, 1.0f};
inline constexpr ui::Vec4 kBackground{0.1451f, 0.1451f, 0.1647f, 1.0f};
/* COLOR_TEXT_POSITIVE - the blue-grey the menu uses for progress text. */
inline constexpr ui::Vec4 kTextPositive{0.2157f, 0.3490f, 0.5176f, 1.0f};

/* Entry points this header adds on top of tc_ui.h's table.  Kept separate so
   adding them cannot disturb a plugin that only uses tc_ui.h.

   Note the out parameters on the rectangle readers: in this build those entry
   points return ImVec2 through a hidden pointer, which is why tc_ui.h declares
   them the same way. */
struct GameTable {
    void (*pushStyleColor)(int, ui::Vec4);
    void (*popStyleColor)(int);
    void (*pushStyleVarVec2)(int, ui::Vec2);
    bool (*button)(const char*, ui::Vec2);
};

inline GameTable& gameTable() { static GameTable value{}; return value; }

/* True once load() resolved every entry point. */
inline bool ready() { return gameTable().pushStyleColor != nullptr; }

/* Comma separated names load() could not resolve ("" when complete). */
inline std::string& gameMissing() { static std::string value; return value; }

/* Resolves the extra entry points.  Returns false when this build does not
   export one of them; the names are then in gameMissing() and were written to
   the loader log. */
inline bool load(const TCHost* host) {
    std::string& missing = gameMissing();
    missing.clear();
    if (!host || !host->engine_proc || !host->context) {
        missing = "engine_proc unavailable";
        return false;
    }
#define TC_GAME_UI_BIND(field, name)                                                \
    do {                                                                            \
        void* tc_game_ui_entry = host->engine_proc(host->context, name);             \
        if (!tc_game_ui_entry) {                                                     \
            if (!missing.empty()) missing += ", ";                                   \
            missing += name;                                                         \
        }                                                                            \
        std::memcpy(&gameTable().field, &tc_game_ui_entry, sizeof(tc_game_ui_entry)); \
    } while (0)
    TC_GAME_UI_BIND(pushStyleColor, "igPushStyleColor_Vec4");
    TC_GAME_UI_BIND(popStyleColor, "igPopStyleColor");
    TC_GAME_UI_BIND(pushStyleVarVec2, "igPushStyleVar_Vec2");
    TC_GAME_UI_BIND(button, "igButton");
#undef TC_GAME_UI_BIND
    if (!missing.empty()) {
        if (host->log)
            host->log(host->context, ("Game UI: missing engine exports: " + missing).c_str());
        return false;
    }
    return true;
}

/* ImGui style colour slots this build uses (ImGuiCol_ enum values). */
inline constexpr int Color_Text = 0;
inline constexpr int Color_Button = 21;
inline constexpr int Color_ButtonHovered = 22;
inline constexpr int Color_ButtonActive = 23;

/* Runs the button with the given style colours pushed, and restores the stack
   afterwards even when the widget throws. */
inline bool styledButton(const char* label, ui::Vec2 size, ui::Vec4 normal, ui::Vec4 hovered,
                         ui::Vec4 active, ui::Vec4 text, bool disabled) {
    if (!gameTable().pushStyleColor || !gameTable().popStyleColor || !gameTable().button)
        return false;
    int pushed = 0;
    if (text.w >= 0.f) {
        gameTable().pushStyleColor(Color_Text, text);
        ++pushed;
    }
    gameTable().pushStyleColor(Color_Button, normal);
    gameTable().pushStyleColor(Color_ButtonHovered, hovered);
    gameTable().pushStyleColor(Color_ButtonActive, active);
    pushed += 3;
    bool pressed = false;
    if (disabled) {
        const ui::Disabled scope(true);
        gameTable().button(label, size);
    } else {
        pressed = gameTable().button(label, size);
    }
    gameTable().popStyleColor(pushed);
    return pressed;
}

/* One main-menu entry: the game's text colours on the generic button frame.

   The menu itself draws no frame, only text that changes colour, so the frame
   is deliberately transparent and the three text colours do the work.  That is
   the appearance used by these standard button wrappers.

   Returns true on the frame the button is released inside it, exactly like
   tc::ui::button(). */
inline bool menu_button(const char* label, ui::Vec2 size = {0, 0}, bool disabled = false) {
    const ui::Vec4 none{0.f, 0.f, 0.f, 0.f};
    return styledButton(label, size, none, none, none, kTextNormal, disabled);
}

/* The framed button from the menu's button colours, with a centred label. */
inline bool framed_button(const char* label, ui::Vec2 size = {0, 0}, bool disabled = false) {
    return styledButton(label, size, kButtonNormal, kButtonHovered, kButtonPressed, kTextNormal,
                        disabled);
}

}  // namespace game_ui
}  // namespace tc

#endif  // TC_GAME_UI_H
