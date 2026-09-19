/* Page registration and ID isolation probe.

   Two packages are built from this same source: example.menu-demo and
   dev.menu-demo-peer.  Their pages deliberately use identical widget labels
   ("Apply", "Reset", "value") to prove the host's PushID(mod)/PushID(page)
   nesting keeps two plugins apart.  If that isolation were missing, ImGui
   would merge the two pages' items and the click counts would interfere.

   Every distinct event is logged once, so a playtest can prove registration,
   opening, clicking and closing without reading pixels. */
#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_game_ui.h"
#include <string>
#ifdef TC_DEMO_DRIVER
#include "../../tests/ui-page-driver.hpp"
#endif

#ifndef TC_DEMO_PAGE_ID
#define TC_DEMO_PAGE_ID "settings"
#endif
#ifndef TC_DEMO_PAGE_TITLE
#define TC_DEMO_PAGE_TITLE "Menu demo"
#endif
#ifndef TC_DEMO_PEER
#define TC_DEMO_PEER 0
#endif

static const TCHost* host;
static int applies, resets;
static bool enabled = true;
static bool drew, loggedApply;

#ifdef TC_DEMO_DRIVER
/* Test builds only: drives the page through real clicks and captures the
   frame.  See tests/ui-page-driver.hpp. */
static void driverLog(const char* message) {
    if (host && host->log) host->log(host->context, message);
}
static void driverTick(void*, const TCFrame* frame) {
    auto& driver = tc_demo_driver::driver();
    if (!driver.window) {
        driver.log = driverLog;
        driver.window = GetActiveWindow();
        if (!driver.window) driver.window = FindWindowW(nullptr, nullptr);
        driver.readConfiguration();
    }
    driver.tick(frame->frame_number);
}
#endif

static void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

static void draw(void*, const TCFrame* frame, float width, float height) {
    if (!drew) {
        drew = true;
        report(std::string("Menu demo: page '") + TC_DEMO_PAGE_ID + "' first drawn on frame " +
               std::to_string(frame ? frame->frame_number : -1) + ", content " +
               std::to_string(static_cast<int>(width)) + "x" +
               std::to_string(static_cast<int>(height)));
    }
    tc::ui::text(std::string("page: ") + TC_DEMO_PAGE_ID);
    tc::ui::text(std::string("frame: ") + std::to_string(tc::ui::frameCount()));
    tc::ui::separator();
#ifdef TC_DEMO_DRIVER
    {
        auto& driver = tc_demo_driver::driver();
        driver.pageVisible(frame ? frame->frame_number : 0);
        /* On the page's first two frames, tell the driver where each item
           lands: only the page knows, and a style change here must not
           silently break the playtest's click target. */
        if (driver.drawn_frames <= 2) {
            const float line = tc::ui::frameHeight();
            driver.advanceItem(line);  // "page: <id>"
            driver.advanceItem(line);  // "frame: <n>"
            driver.advanceItem(line);  // separator
            driver.observeClickable("Apply", line);
        }
    }
#endif
    /* Same visible labels in both plugins on purpose. */
    /* The game's own menu look for the primary action; the generic ImGui
       button for the rest, so the difference is visible side by side. */
#ifdef TC_DEMO_DRIVER
    /* Where the button is about to be drawn, read as two scalars (the ImVec2
       readers are broken in this build).  This is the click target. */
    tc_demo_driver::driver().observeItemOrigin("Apply", tc::ui::cursorPosX(),
                                               tc::ui::cursorPosY());
#endif
    if (tc::game_ui::menu_button("Apply")) {
        ++applies;
        if (!loggedApply) {
            loggedApply = true;
            report("Menu demo: Apply clicked on " TC_DEMO_PAGE_ID);
        }
    }
#ifdef TC_DEMO_DRIVER
    {
        /* Read the button's own measurements immediately, while it is still
           the item that was drawn last. */
        const tc::ui::Vec2 applied = tc::ui::itemRectSize();
        tc_demo_driver::driver().observeClickableSize("Apply", applied.x, applied.y);
        /* Full-content-region probe, submitted after Apply so Apply wins when
           the cursor is on it and this wins everywhere else in the page.

           This is the input test that does not depend on placing the cursor:
           whichever point the game leaves the mouse at, if that point is
           anywhere inside the page the probe hovers.  The earlier sweeps tried
           to move the cursor to exact coordinates, which this game's own
           recentring defeats. */
        /* Offset to the right so the host's back button (top-left) stays
           clickable while this probe is in place. */
        tc::ui::setCursorPos({220, 0});
        const bool probeClicked =
            tc::ui::invisibleButton("driver full probe", {width - 220, height}, 0);
        tc_demo_driver::driver().observeFullProbe(tc::ui::isItemHovered(),
                                                  tc::ui::isWindowHovered(), probeClicked);
        /* Is the page even receiving mouse input?  A page whose window never
           sees the cursor logs "hover=0" here, which points at the host window
           rather than at the click position. */
        const tc::ui::Vec2 mouse = tc::ui::mousePos();
        tc_demo_driver::driver().observeMouse(mouse.x, mouse.y, tc::ui::isItemHovered(),
                                              tc::ui::isWindowHovered(),
                                              tc::ui::isWindowFocused());
        /* The container's real on-screen size, measured with the scalar
           readers (igGetWindowWidth/Height return a single float each).  The
           content size handed to the callback is the *viewport*, not the
           window, so until now the window's actual size was unknown. */
        tc_demo_driver::driver().observeWindowSize(tc::ui::windowWidth(),
                                                   tc::ui::windowHeight());
    }
#endif
    tc::ui::sameLine();
    if (tc::ui::button("Reset")) ++resets;
    tc::ui::separator();
    tc::ui::checkbox("value", &enabled);
    tc::ui::text(std::string("Apply=") + std::to_string(applies) + "  Reset=" +
                 std::to_string(resets) + "  value=" + (enabled ? "on" : "off"));
    tc::ui::textDisabled("Apply uses the game's own menu style; Reset uses plain ImGui.");
    /* A popup inside the content region: proves the host's ID scopes cover
       plugin popups too, without the plugin opening its own window. */
    if (tc::ui::button("Popup check")) {
        if (tc::ui::table().openPopup) tc::ui::table().openPopup("demo popup", 0);
    }
    if (tc::ui::table().beginPopupModal) {
        bool open = true;
        if (tc::ui::table().beginPopupModal("demo popup", &open, 0)) {
            tc::ui::text("popup is inside the page ID scope");
            if (tc::ui::button("Close")) tc::ui::table().closeCurrentPopup();
            tc::ui::table().endPopup();
        }
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE ||
        !out || out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    if (!tc::ui::load(h)) {  // names of any missing exports go to the log
        report("Menu demo: tc::ui::load failed: " + tc::ui::missing());
        return 3;
    }
    /* Optional: game-look widgets.  A build without those exports still runs
       the page, just with the generic look. */
    if (!tc::game_ui::load(h))
        report("Menu demo: game UI widgets unavailable: " + tc::game_ui::gameMissing());
    const int result = tc::ui::registerPage(TC_DEMO_PAGE_ID, TC_DEMO_PAGE_TITLE, draw,
                                           nullptr, h);
    if (result != 0) {
        report("Menu demo: registerPage failed with " + std::to_string(result));
        return 4;
    }
#if TC_DEMO_PEER
    /* Second registration with the same page_id must be rejected (code -3),
       and a malformed one with -2.  Both are load failures we want to see
       reported rather than tolerated. */
    if (tc::ui::registerPage(TC_DEMO_PAGE_ID, "duplicate", draw, nullptr, h) != -3)
        return 5;
    if (tc::ui::registerPage("bad###id", "invalid", draw, nullptr, h) != -2) return 6;
    if (tc::ui::registerPage("", "invalid", draw, nullptr, h) != -2) return 7;
    if (tc::ui::registerPage(nullptr, "invalid", draw, nullptr, h) != -2) return 8;
#endif
    report(std::string("Menu demo: registered page '") + TC_DEMO_PAGE_ID + "'");
#ifdef TC_DEMO_DRIVER
    /* on_frame and the page share this plugin on purpose: the loader has to
       keep their frame counters separate, or one would skip frames. */
    /* on_frame and the page share this plugin on purpose: the loader has to
       keep their frame counters separate, or one would skip frames. */
    out->on_frame = driverTick;
#endif
    return 0;
}
