/* Circuit-board side panel example.

   Registers one panel on the board's right edge with
   tc::ui::registerBoardPanel().  The host owns the container, the input
   ownership and the lifetime; this file only draws content, exactly like a
   main-menu page does.  What the example exercises:

   * a game-look button and a checkbox (interaction),
   * the drawing API inside a slot (tc_ui_draw.h),
   * the panel disappearing with the level: the host draws it from the board's
     own per-frame code, so there is nothing here to open or close.

   The playtest build adds tests/ui-board-panel-driver.hpp, which clicks the
   Ping button with real mouse messages and records what the game itself read
   at its board input sampling site. */
#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_game_ui.h"
#include "../../sdk/tc_ui_draw.h"
#include <string>
#ifdef TC_BOARD_DRIVER
#include "../../tests/ui-board-panel-driver.hpp"
#endif

static const TCHost* host;
static int pings, drawn;
static bool monitor = true;
static bool loggedFirst;
static bool loggedHover;
#ifdef TC_BOARD_DRIVER
/* Only the playtest build reports the button's rectangle to the driver. */
static bool loggedOrigin;
#endif

/* Rows of filler text: enough to overflow the panel, so the content region the
   host provides has something to scroll (wheel over the panel) and to clip. */
static constexpr int kRows = 24;

static void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

/* Same text, plus the loader's Mods page (host->report_status): a player sees
   why a panel is missing without opening the log.  Used for load-time state
   only - per-frame chatter belongs in report().  level: 0 info, 1 warning,
   2 error. */
static void reportStatus(const std::string& message, int level) {
    report(message);
    if (host) tc::reportStatus(host, level, message.c_str());
}

#ifdef TC_BOARD_DRIVER
static void driverLog(const char* message) {
    if (host && host->log) host->log(host->context, message);
}
#endif

static void draw(void*, const TCFrame* frame, float width, float height) {
    ++drawn;
    const int number = frame ? frame->frame_number : -1;
    if (!loggedFirst) {
        loggedFirst = true;
        report("Board panel: first drawn on frame " + std::to_string(number) + ", content " +
               std::to_string(static_cast<int>(width)) + "x" +
               std::to_string(static_cast<int>(height)));
    }
#ifdef TC_BOARD_DRIVER
    tc_board_driver::driver().panelFrame(number, width, height);
    /* The cursor at the very start of the content child: it carries the
       region's scroll offset, so a change here is a scroll and nothing else. */
    tc_board_driver::driver().panelOffset(tc::ui::cursorPosY());
#endif
    tc::ui::text("board frame " + std::to_string(tc::ui::frameCount()));
    tc::ui::text("panel draws " + std::to_string(drawn));
#ifdef TC_BOARD_DRIVER
    /* Periodic proof that the callback keeps running (it must also run while
       the player has the panel folded). */
    if (drawn % 120 == 0)
        report("Board panel: draw count=" + std::to_string(drawn));
#endif
    tc::ui::separator();
    const float buttonWidth = width > 60.f ? width * 0.7f : 120.f;
#ifdef TC_BOARD_DRIVER
    if (!loggedOrigin) {
        loggedOrigin = true;
        tc_board_driver::driver().panelItemOrigin("Ping", tc::ui::cursorPosX(),
                                                  tc::ui::cursorPosY());
    }
    /* Absolute position every frame: the driver clicks this instead of a point
       the playtest script computed, so a window resize (or a DPI/ratio change)
       cannot leave it clicking yesterday's coordinates.  The driver only logs
       the value when it changes. */
    {
        const tc::ui::Vec2 screen = tc::ui::cursorScreenPos();
        tc_board_driver::driver().panelItemScreen(screen.x, screen.y);
    }
#endif
    if (tc::game_ui::framed_button("Ping", {buttonWidth, 0.f})) {
        ++pings;
        report("Board panel: Ping clicked, count=" + std::to_string(pings));
#ifdef TC_BOARD_DRIVER
        tc_board_driver::driver().panelClicked(pings);
#endif
    }
    /* Reported so a playtest can show that ImGui gave the mouse to this button
       - a click that reaches one item cannot also reach the board behind it. */
    if (!loggedHover && tc::ui::isItemHovered()) {
        loggedHover = true;
        report("Board panel: Ping hovered on frame " + std::to_string(number));
#ifdef TC_BOARD_DRIVER
        tc_board_driver::driver().panelHovered(number);
#endif
    }
#ifdef TC_BOARD_DRIVER
    {
        /* The size is reported every frame too: the click point is the button's
           centre, so both halves have to follow a re-layout. */
        const tc::ui::Vec2 size = tc::ui::itemRectSize();
        tc_board_driver::driver().panelItemSize("Ping", size.x, size.y);
    }
#endif
    tc::ui::checkbox("monitor", &monitor);
    tc::ui::text("Ping=" + std::to_string(pings) + "  monitor=" + (monitor ? "on" : "off"));
    tc::ui::separator();
    /* Taller than the panel on purpose: the host puts the content in a child
       window, so this list scrolls (wheel over the panel) and is clipped to
       the panel instead of drawing over the board. */
    tc::ui::text("scrollable rows:");
    for (int row = 0; row < kRows; ++row)
        tc::ui::text("row " + std::to_string(row) + (row == kRows - 1 ? "  (last)" : ""));
    /* The drawing API works inside a slot as well: the canvas clips to the
       panel and takes its layout space there. */
    if (tc::ui::drawingReady()) {
        const float canvasWidth = width > 24.f ? width - 16.f : 0.f;
        const float canvasHeight = height > 140.f ? 96.f : 0.f;
        if (canvasWidth > 40.f && canvasHeight > 40.f) {
            tc::ui::Canvas canvas("signal", {canvasWidth, canvasHeight});
            if (canvas) {
                using tc::ui::rgba;
                canvas.rectFilled({0, 0}, canvas.size(), rgba(24, 26, 33));
                const float step = canvasWidth / 8.f;
                for (int index = 1; index < 8; ++index)
                    canvas.line({step * index, 0}, {step * index, canvasHeight},
                                rgba(52, 57, 70));
                const float phase = static_cast<float>(number % 60) / 60.f;
                tc::ui::Vec2 previous{0.f, canvasHeight * 0.5f};
                for (int index = 1; index <= 24; ++index) {
                    const float x = canvasWidth * static_cast<float>(index) / 24.f;
                    const float y = canvasHeight * 0.5f -
                                    std::sin((x / canvasWidth) * 12.566f + phase * 6.283f) *
                                        canvasHeight * 0.28f;
                    canvas.line(previous, {x, y}, rgba(120, 200, 255), 2);
                    previous = {x, y};
                }
                canvas.circleFilled({canvasWidth * 0.5f, canvasHeight * 0.5f}, 3,
                                    rgba(245, 185, 70));
                canvas.text({4, 4}, rgba(200, 205, 215), "slot canvas");
            }
        }
    }
    tc::ui::textDisabled(std::string("host-owned panel: ") +
                         (host && host->mod_id ? host->mod_id : "unknown mod"));
#ifdef TC_BOARD_DRIVER
    {
        /* Off-screen probe: an item below the visible content must not become
           hoverable or clickable.  That is what proves the region is clipped
           rather than merely not painted. */
        tc::ui::setCursorPos({0.f, height + 160.f});
        auto& probe = tc_board_driver::driver();
        probe.panelSentinelOrigin(tc::ui::cursorPosX(), tc::ui::cursorPosY());
        const bool sentinel = tc::ui::button("###sentinel", {200.f, 40.f});
        const tc::ui::Vec2 sentinelSize = tc::ui::itemRectSize();
        probe.panelSentinelSize(sentinelSize.x, sentinelSize.y);
        probe.panelSentinelState(tc::ui::isItemHovered(), sentinel);
    }
#endif
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    /* Ask for what this mod needs instead of discovering it later: the panel API
       is the whole point of this package. */
    if (!tc::hostHas(h, TC_CAP_UI_SLOT)) return 2;
    if (!tc::ui::load(h)) {
        reportStatus("Board panel: tc::ui::load failed: " + tc::ui::missing(), 2);
        return 3;
    }
    /* Both are optional: without them the panel still works, with a generic
       look and without the waveform canvas. */
    if (!tc::game_ui::load(h))
        reportStatus("Board panel: game UI widgets unavailable: " + tc::game_ui::gameMissing(), 1);
    if (!tc::ui::loadDrawing(h))
        reportStatus("Board panel: drawing unavailable: " + tc::ui::drawingMissing(), 1);
    const int result = tc::ui::registerBoardPanel("main", "Board panel", draw, nullptr, h);
    if (result != 0) {
        reportStatus("Board panel: registerBoardPanel failed with " + std::to_string(result), 2);
        return 4;
    }
    reportStatus("Board panel: registered slot 'main'", 0);
#ifdef TC_BOARD_DRIVER
    if (!tc_board_driver::start(h, driverLog)) return 9;
    out->on_frame = tc_board_driver::tick;
#endif
    return 0;
}
