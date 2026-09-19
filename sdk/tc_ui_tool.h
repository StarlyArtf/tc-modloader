#ifndef TC_UI_TOOL_H
#define TC_UI_TOOL_H
/* Helpers for a control that lives in the game's own tool column
   (tc::ui::registerBoardToolbar, kind TC_UI_SLOT_BOARD_TOOLBAR).

   Everything here exists because doing it by hand goes wrong in specific,
   measured ways:

     * the game's tools are 80x80 rounded tiles, two per row, 96 px apart, with
       the background colour (53,50,68) - a control of another size or spacing
       looks foreign, so ToolTile draws to those metrics;
     * hovering is decided *geometrically* (is the mouse inside the tile's
       rectangle), not from ImGui's item state: a popup drawn over the tile takes
       the item hover away, and a popup that closes on hover loss flickers every
       frame;
     * a hover popup must stay open while the mouse is on the tile *or* inside
       the popup, and must not be torn down in the middle of a drag - HoverPopup
       encodes exactly that, and places itself beside the tile so it never covers
       it.

   Usage:

     tc::ui::ToolTile tile("wirecolour");
     tile.colourIcon(currentColour);              // or tile.drawIcon(...) for custom art
     if (tile.clicked()) useCurrentColour();
     // Expanded contents: an ordinary window next to the tile.  The host pushes
     // the game's text font for the whole tool draw, so a window's text renders
     // exactly like the board panels' (measured: both 45 px).
     if (auto panel = tc::ui::panel("##palette", nullptr, {420, 560},
                                    {tile.origin().x + tile.size().x + 8, tile.origin().y - 8},
                                    1.f, 0, tc::ui::Cond_Always)) {
         // draw the expanded contents here
     }

   Measured facts this header exists for (this game build, 2026-09-18):

     * the tool column leaves the game's *icon* font current at the injection
       point, and an icon font has no Latin or CJK glyphs - text submitted there
       draws nothing at all while rectangles still show.  The loader borrows the
       game's own text font for plugin tool draws (src/native.hpp,
       resolveToolTextFont / boardToolFrame), so plugin text works with no font
       handling of its own;
     * the game draws a label it owns as igPushFont(2) -> igText -> igPopFont
       inside the tile, i.e. a plugin runs outside that font scope;
     * an ImGui popup opened from the column inherits that scope (and used to be
       the reason nothing showed); HoverPopup below keeps the popup shape for
       completeness, but a plain window is the verified form - see
       examples/wire-palette/plugin.cpp and docs/research/toolbar-tool-handoff.md. */
#include "tc_ui.h"
#include "tc_ui_draw.h"
#include <windows.h>

namespace tc {
namespace ui {

/* Metrics of the game's own tool tiles, measured on the pinned build. */
struct ToolTileStyle {
    float size = 80.f;
    float rounding = 10.f;
    Vec4 background{53.f / 255.f, 50.f / 255.f, 68.f / 255.f, 1.f};
    Vec4 hovered{68.f / 255.f, 64.f / 255.f, 86.f / 255.f, 1.f};
    float iconInset = 18.f;
    float iconRounding = 8.f;
};

/* Packed-colour conversion for the drawing canvas, which takes ImU32. */
inline Color packColour(Vec4 colour) {
    return rgba(static_cast<unsigned char>(colour.x * 255.f),
                static_cast<unsigned char>(colour.y * 255.f),
                static_cast<unsigned char>(colour.z * 255.f),
                static_cast<unsigned char>(colour.w * 255.f));
}

/* One tile in the game's tool column.  Construction draws it; the accessors
   report what the player is doing to it. */
class ToolTile {
public:
    ToolTile(const char* id, ToolTileStyle style = {})
        : style_(style), canvas_(id, {style.size, style.size}) {
        if (!canvas_) {
            /* No drawing capability: a plain colour button of the same size keeps
               the tool usable and the layout unchanged. */
            fallback_ = true;
            ui::colorButton(id, style_.background, 0, {style_.size, style_.size});
            hovered_ = ui::isItemHovered();
            clicked_ = hovered_ && ui::isMouseClicked(0);
            return;
        }
        const Vec2 mouse = canvas_.mousePosition();
        hovered_ = mouse.x >= 0.f && mouse.y >= 0.f && mouse.x < style_.size &&
                   mouse.y < style_.size;
        clicked_ = canvas_.clicked();
        canvas_.rectFilled({0.f, 0.f}, canvas_.size(),
                           packColour(hovered_ ? style_.hovered : style_.background),
                           style_.rounding);
    }

    bool drawn() const { return static_cast<bool>(canvas_) || fallback_; }
    /* True while the mouse is inside the tile's rectangle, whatever is drawn on
       top of it.  Use this, not ImGui hover, to keep a popup open. */
    bool hovered() const { return hovered_; }
    bool clicked() const { return clicked_; }
    Vec2 origin() const { return canvas_ ? canvas_.origin() : Vec2{0.f, 0.f}; }
    Vec2 size() const { return {style_.size, style_.size}; }
    ToolTileStyle style() const { return style_; }

    /* The common case: the tile's icon is a solid colour (a colour tool). */
    void colourIcon(Vec4 colour) {
        if (!canvas_) return;
        const unsigned char r = static_cast<unsigned char>(colour.x * 255.f);
        const unsigned char g = static_cast<unsigned char>(colour.y * 255.f);
        const unsigned char b = static_cast<unsigned char>(colour.z * 255.f);
        canvas_.rectFilled({style_.iconInset, style_.iconInset},
                           {style_.size - style_.iconInset, style_.size - style_.iconInset},
                           rgba(r, g, b, 255), style_.iconRounding);
    }
    /* Anything else: draw inside the tile's canvas in tile-local coordinates
       (0,0 is the tile's top-left) and the icon inset is included by the caller. */
    template <class F>
    void drawIcon(F&& draw) {
        if (canvas_) draw(canvas_);
    }

private:
    ToolTileStyle style_;
    Canvas canvas_{nullptr, {0.f, 0.f}};
    bool fallback_ = false, hovered_ = false, clicked_ = false;
};

/* A popup that behaves the way the game's own tool popups do: it opens while the
   tile is hovered, stays open while the mouse is on the tile or inside the popup,
   and closes the moment the mouse leaves both - unless a widget is being dragged
   (dragging a colour picker never tears the popup down).

   Kept for callers that want the popup shape.  Prefer a plain window
   (tc::ui::panel, as examples/wire-palette does): it is what was verified on the
   real engine, and the popup form is the one that used to draw its text with the
   tool column's inherited font scope. */
class HoverPopup {
public:
    HoverPopup(const char* id, const ToolTile& tile, Vec2 offset = {8.f, -4.f}) {
        if (!id) return;
        const Vec2 origin = tile.origin();
        if (tile.hovered()) ui::openPopup(id, 0);
        if (tile.drawn())
            ui::setNextWindowPos({origin.x + tile.size().x + offset.x, origin.y + offset.y},
                                 Cond_Appearing);
        visible_ = ui::beginPopup(id, 0);
        if (!visible_) return;
        /* Grace period: the tile and the popup are a few pixels apart, and moving
           the mouse slowly across that gap would otherwise read as "left the
           menu" and close it.  Only a mouse that stays outside both for
           kCloseDelaySeconds closes the popup; a drag never does. */
        static double lastInside = 0.0;
        const double now = static_cast<double>(GetTickCount64()) / 1000.0;
        if (tile.hovered() || ui::isWindowHovered(0)) lastInside = now;
        if (!tile.hovered() && !ui::isWindowHovered(0) && !ui::isAnyItemActive() &&
            !ui::isMouseDown(0) && lastInside > 0.0 && now - lastInside > kCloseDelaySeconds)
            ui::closeCurrentPopup();
    }
    explicit operator bool() const { return visible_; }
    ~HoverPopup() {
        if (visible_) ui::endPopup();
    }
    HoverPopup(const HoverPopup&) = delete;
    HoverPopup& operator=(const HoverPopup&) = delete;

private:
    /* Long enough to cross the gap between tile and popup, short enough to feel
       immediate. */
    static constexpr double kCloseDelaySeconds = 0.25;
    bool visible_ = false;
};

}  // namespace ui
}  // namespace tc

#endif  // TC_UI_TOOL_H
