/* In-process driver for the native UI page playtest.

   Compiled into a *test* build of the menu-demo plugin (see build.ps1); the
   shipped dev.menu-demo package does not include it.

   Why in-process: the sandbox game window is parked off the desktop and its
   framebuffer cannot be read from outside, so the game itself clicks.  The
   driver:

   * clicks the loader's real home-page button (rectangle taken from the
     loader's own log line, not a guessed offset),
   * clicks the page's own Apply button at a position the page reports while it
     draws,
   * clicks the host's back button,
   * logs every step with a DRIVER: prefix for the playtest to assert on.

   It never guesses the layout: both unknowns - the button rectangle and the
   window the game uses - come from the game. */
#pragma once
#ifdef TC_DEMO_DRIVER
#include <windows.h>
#include <string>
#include <vector>

namespace tc_demo_driver {

/* Moves the cursor to the client position and queues a real mouse event pair
   in the game's own message loop.  Both steps matter: this build's game reads
   the cursor position itself (a synthetic WM_MOUSEMOVE with a made-up lParam
   leaves ImGui's mouse at 0,0), while the button messages are what make ImGui
   see a press and a release in different frames. */
inline void clickClient(HWND window, int x, int y) {
    POINT screen{x, y};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    Sleep(30);
    const LPARAM point = MAKELPARAM(x, y);
    PostMessageW(window, WM_MOUSEMOVE, 0, point);
    Sleep(40);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
    Sleep(60);
    PostMessageW(window, WM_LBUTTONUP, 0, point);
}

struct Driver {
    void (*log)(const char*) = nullptr;
    HWND window = nullptr;
    /* Page-entry button centre in game coordinates, from the loader's log. */
    int open_x = 0, open_y = 0;
    std::string open_label;
    /* Game coordinates -> OS client coordinates. */
    float click_scale_x = 1.f, click_scale_y = 1.f;
    /* Host header height: the content region starts below it.  The host
       documents this value for pages, so both sides agree by construction. */
    float content_y = 64.f;
    /* Top of the next widget inside the content region; the page advances it
       as it draws, so the click point tracks the real layout. */
    float next_item_y = 0.f;
    float click_x = 0.f, click_y = 0.f;
    bool click_known = false;

    int start_frame = 0, page_frame = 0, drawn_frames = 0;
    int stage = 0;  // 0 idle, 1 entry clicked, 2 page seen, 3 apply clicked, 4 back clicked
    bool started = false, page_open = false;
    int entry_clicks = 0;
    /* Mouse position the game reported, the page window's own position, and
       whether the button was hovered, all from the frame that drew it. */
    float mouse_x = 0.f, mouse_y = 0.f, window_x = 0.f, window_y = 0.f;
    bool hovered = false;
    bool reported_calibration = false;
    bool reported_hover = false;
    /* Screen offset of the game window; the driver's clicks are placed with
       the same rule the game uses to turn a screen position into its own. */
    int window_origin_x = 0, window_origin_y = 0;

    void tick(int frame_number) {
        if (!window || !log) return;
        if (!started) {
            started = true;
            start_frame = frame_number;
            /* Move the window to the desktop origin for the duration of the
               test.  The game derives its mouse position from the real cursor,
               and with the window parked off-screen that derivation lands
               outside every widget, so clicks could not reach the page.  At
               the origin, screen, client and game coordinates all agree. */
            SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client{}, outer{};
            GetClientRect(window, &client);
            GetWindowRect(window, &outer);
            char diagnostic[288]{};
            wsprintfA(diagnostic,
                      "DRIVER: started window=%p client=%ldx%ld outer=%ld,%ld %ldx%ld screen=%dx%d env=%s",
                      window, client.right, client.bottom, outer.left, outer.top,
                      outer.right - outer.left, outer.bottom - outer.top,
                      GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                      open_label.empty() ? "(none)" : open_label.c_str());
            log(diagnostic);
        }
        /* Wait for the loader to boot and the home page to draw. */
        if (stage == 0 && frame_number - start_frame > 120) {
            stage = 1;
        }
        /* Keep clicking the entry until the page actually appears.  A single
           click is not reliable: if another application is confining or
           recentring the cursor, SetCursorPos is overridden and the click
           lands elsewhere.  Retrying costs nothing and makes the run
           deterministic instead of a coin flip. */
        if (stage == 1 && !page_open && entry_clicks < 8 &&
            frame_number - start_frame > 120 + entry_clicks * 45) {
            if (open_x == 0 && open_y == 0) {
                log("DRIVER: the loader never logged a page-entry rectangle; cannot click");
                return;
            }
            clickClient(window, static_cast<int>(open_x * click_scale_x),
                        static_cast<int>(open_y * click_scale_y));
            ++entry_clicks;
            log(("DRIVER: clicked page entry " + open_label + " (attempt " +
                 std::to_string(entry_clicks) + ")").c_str());
        }
        if (stage == 2 && frame_number - page_frame > 40) {
            stage = 3;
            /* The page reports the widget's real size; the click point follows
               from it, and the input path is proven by whether the page's own
               click handler fires. */
            const int apply_x = static_cast<int>(click_x * click_scale_x);
            const int apply_y = static_cast<int>(click_y * click_scale_y);
            /* The page reports the button in *window-local* coordinates
               (cursorPosX/Y) while the mouse the game reads is in screen
               coordinates.  The offset between them is the page window's own
               position, which this build cannot report (igGetWindowPos is
               broken), so try the plausible offsets and let the game's own
               hover state say which one is right. */
            log(("DRIVER: local centre " + std::to_string(apply_x) + "," +
                 std::to_string(apply_y)).c_str());
        }
        if (stage == 3) sweep(frame_number);
        if (stage == 3 && (sweep_done || frame_number - page_frame > 600)) {
            stage = 4;
            log("DRIVER: probe sweep done");
            /* The host's back button: top-left of the viewport, 110x40 at the
               default scale (docs/sdk/ui.md). */
            clickClient(window, static_cast<int>(55 * click_scale_x),
                        static_cast<int>(46 * click_scale_y));
            log("DRIVER: clicked BACK");
            /* If the page closes, the input path works and the problem is
               only the Apply button's position.  If it does not, the host
               window itself is not receiving mouse input. */
        }
    }

    /* Called by the page once per item it is about to draw, with the engine's
       own height for that item. */
    void advanceItem(float height) {
        next_item_y += height;
    }

    void pageVisible(int frame_number) {
        ++drawn_frames;
        /* The page can open on the frame right after the entry click, before
           tick() has advanced the stage, so the transition belongs here. */
        if (stage < 2) stage = 2;
        if (page_open) return;
        page_open = true;
        page_frame = frame_number;
        if (log) log("DRIVER: page visible");
    }

    /* Called by the page when a clickable widget is about to be drawn. */
    void observeClickable(const char* label, float height) {
        (void)label;
        (void)height;
    }

    /* The engine's own size report for the item just drawn, plus the click
       point the page's layout implies for it. */
    /* The button's origin in window-local coordinates, read as two scalars
       before it is drawn.  The click target is this, offset by the window's
       own position which the driver pins to the desktop origin. */
    void observeItemOrigin(const char* label, float x, float y) {
        if (origin_logged || !label || std::string(label) != "Apply") return;
        origin_logged = true;
        item_x = x;
        item_y = y;
        if (log) {
            log(("DRIVER: Apply origin " + std::to_string(static_cast<int>(x)) + "," +
                 std::to_string(static_cast<int>(y))).c_str());
        }
    }
    bool origin_logged = false;
    float item_x = 0.f, item_y = 0.f;

    void observeClickableSize(const char* label, float width, float height) {
        if (saw_item || !label || std::string(label) != "Apply") return;
        saw_item = true;
        item_width = width;
        item_height = height;
        /* Prefer the origin the page reported; fall back to the layout model
           only when it never arrived. */
        click_x = origin_logged ? item_x + width / 2 : 48.f;
        click_y = origin_logged ? item_y + height / 2
                                : content_y + next_item_y + (height > 0.f ? height / 2 : 0.f);
        click_known = width > 0.f && height > 0.f;
        if (log) {
            log(("DRIVER: Apply item size " + std::to_string(static_cast<int>(width)) + "x" +
                 std::to_string(static_cast<int>(height))).c_str());
        }
    }
    bool saw_item = false;
    float item_width = 0.f, item_height = 0.f;

    /* Called every page frame: the mouse position the game reported, the
       window's own origin and whether the button is hovered.  The first few
       values are logged, which is what pins down how this machine maps a
       screen position into the game's coordinates. */
    void observeMouse(float mx, float my, bool over, bool window_hovered, bool window_focused) {
        mouse_x = mx; mouse_y = my; hovered = over;
        win_hovered = window_hovered;
        win_focused = window_focused;
        if (!log) return;
        /* The window-level state is logged on change even when no item is
           hovered: that separates "the window never gets the mouse" from
           "the window gets it but the item is elsewhere". */
        if (window_hovered != last_logged_win || window_focused != last_logged_focus) {
            last_logged_win = window_hovered;
            last_logged_focus = window_focused;
            log(("DRIVER: window hovered=" + std::to_string(window_hovered ? 1 : 0) +
                 " focused=" + std::to_string(window_focused ? 1 : 0) + " mouse=" +
                 std::to_string(static_cast<int>(mx)) + "," +
                 std::to_string(static_cast<int>(my))).c_str());
        }
        /* Log every change, not just the first frame.  The earlier version
           logged only frame 1, which is before the driver has moved the mouse
           at all - reading "overApply=0" from that and concluding the page
           receives no input was a mistake this fixes. */
        const int ix = static_cast<int>(mx), iy = static_cast<int>(my);
        if (ix == last_logged_x && iy == last_logged_y && over == last_logged_over) return;
        last_logged_x = ix; last_logged_y = iy; last_logged_over = over;
        log(("DRIVER: mouse " + std::to_string(ix) + "," + std::to_string(iy) + " overApply=" +
             std::to_string(over ? 1 : 0) + " stage=" + std::to_string(stage)).c_str());
        if (over) reported_hover = true;
    }
    int last_logged_x = -99999, last_logged_y = -99999;
    bool last_logged_over = false;
    /* The container window's own size, as the engine reports it. */
    void observeWindowSize(float width, float height) {
        if (size_logged || !log) return;
        size_logged = true;
        log(("DRIVER: container window size " + std::to_string(static_cast<int>(width)) + "x" +
             std::to_string(static_cast<int>(height))).c_str());
    }
    bool size_logged = false;
    /* Result of the full-region probe: non-zero means the page received the
       mouse at the item level, wherever the cursor happened to be. */
    void observeFullProbe(bool item_hovered, bool window_hovered, bool clicked) {
        full_probe_points += 1;
        if (item_hovered) ++full_probe_hits;
        if (clicked) {
            full_probe_clicks += 1;
            if (!click_confirmed && log) {
                click_confirmed = true;
                log("DRIVER: CONFIRMED - a click reached a widget inside the page container");
            }
        }
        /* As soon as the probe hovers, press and release the mouse buttons
           WITHOUT moving the cursor.  The cursor is already inside the page
           (the game puts it wherever it likes), so this tests click delivery
           on its own, with none of the cursor-placement problems that made the
           earlier sweeps unreliable. */
        if (item_hovered && probe_clicks < 3 && !click_pressed && window) {
            click_pressed = true;
            PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(0, 0));
        } else if (click_pressed) {
            click_pressed = false;
            ++probe_clicks;
            PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(0, 0));
        }
        if (!probe_logged && (item_hovered || full_probe_points > 30)) {
            probe_logged = true;
            if (log) {
                log(("DRIVER: full-region probe itemHovered=" +
                     std::to_string(item_hovered ? 1 : 0) + " windowHovered=" +
                     std::to_string(window_hovered ? 1 : 0) + " mouse=" +
                     std::to_string(static_cast<int>(mouse_x)) + "," +
                     std::to_string(static_cast<int>(mouse_y)) + " clicks=" +
                     std::to_string(full_probe_clicks)).c_str());
            }
        }
    }
    int full_probe_points = 0, full_probe_hits = 0, full_probe_clicks = 0, probe_clicks = 0;
    bool probe_logged = false, click_pressed = false, click_confirmed = false;
    bool win_hovered = false, win_focused = false;
    bool last_logged_win = false, last_logged_focus = false;
    int sweep_index = 0;

    /* Walk a grid over the top-left corner of the screen, one point per frame.

       Read-then-move on purpose: the page updates `hovered` during the frame,
       after this runs, so the value read here belongs to the point set on the
       previous frame.  Logging after the move (the first version of this
       probe) reported the *previous* point's hover and made the whole sweep
       unreadable.

       The grid is wide and dense enough to cover any plausible position of the
       page window; if no point reports a hover, the window is not receiving
       mouse input at all rather than merely being in the wrong place. */
    void sweep(int frame_number) {
        if (hovered && !hover_logged) {
            hover_logged = true;
            if (log) {
                log(("DRIVER: HOVER at " + std::to_string(static_cast<int>(mouse_x)) + "," +
                     std::to_string(static_cast<int>(mouse_y))).c_str());
            }
            clickClient(window, static_cast<int>(mouse_x), static_cast<int>(mouse_y));
            return;
        }
        if (hover_logged || sweep_index >= kSweepPoints) {
            if (!sweep_done) {
                sweep_done = true;
                if (log) {
                    log(("DRIVER: sweep finished, hover=" + std::to_string(hover_logged ? 1 : 0) +
                         " points=" + std::to_string(sweep_index) + " winHovered=" +
                         std::to_string(hover_points) + " windowRect=[" +
                         std::to_string(hover_min_x) + "," + std::to_string(hover_min_y) + " .. " +
                         std::to_string(hover_max_x) + "," + std::to_string(hover_max_y) +
                         "] itemHovered=" + std::to_string(item_hover_points)).c_str());
                }
            }
            return;
        }
        if (frame_number - page_frame < 50 + sweep_index) return;
        /* Coarse sweep of the whole screen.  Its purpose is to find the page
           window's actual on-screen rectangle, which this build cannot report
           (igGetWindowPos is broken) and which is clearly not where the
           window-local coordinates suggested.  One point per frame, and the
           window/item state read at the top of the next call belongs to the
           point set here - that pairing is what makes the sweep readable. */
        recordHover();
        const int step = 100;
        const int columns = 26;
        const int x = (sweep_index % columns) * step;
        const int y = (sweep_index / columns) * step;
        POINT target{x, y};
        ClientToScreen(window, &target);
        SetCursorPos(target.x, target.y);
        PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        ++sweep_index;
    }
    /* Records the state observed for the point set on the previous frame. */
    void recordHover() {
        if (!win_hovered) return;
        ++hover_points;
        const int x = static_cast<int>(mouse_x), y = static_cast<int>(mouse_y);
        if (hover_points == 1 || x < hover_min_x) hover_min_x = x;
        if (hover_points == 1 || y < hover_min_y) hover_min_y = y;
        if (hover_points == 1 || x > hover_max_x) hover_max_x = x;
        if (hover_points == 1 || y > hover_max_y) hover_max_y = y;
        if (log) {
            log(("DRIVER: window hovered at " + std::to_string(x) + "," + std::to_string(y) +
                 " itemOver=" + std::to_string(hovered ? 1 : 0)).c_str());
        }
    }
    static constexpr int kSweepPoints = 26 * 16;
    bool hover_logged = false, sweep_done = false;
    int hover_points = 0, item_hover_points = 0;
    int hover_min_x = 0, hover_min_y = 0, hover_max_x = 0, hover_max_y = 0;

    void readConfiguration() {
        char buffer[192]{};
        const DWORD length = GetEnvironmentVariableA("TC_DEMO_OPEN_BUTTON", buffer, sizeof(buffer));
        if (length == 0 || length >= sizeof(buffer)) return;
        const std::string value(buffer, length);
        /* "<id> <centreX> <centreY> <gameW> <gameH> <windowW> <windowH>" */
        std::vector<std::string> fields;
        for (size_t start = 0; start <= value.size();) {
            const size_t end = value.find(' ', start);
            fields.push_back(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (fields.size() < 7) return;
        try {
            open_label = fields[0];
            open_x = std::stoi(fields[1]);
            open_y = std::stoi(fields[2]);
            const float game_width = std::stof(fields[3]), game_height = std::stof(fields[4]);
            const float window_width = std::stof(fields[5]), window_height = std::stof(fields[6]);
            if (game_width > 1.f && window_width > 1.f) click_scale_x = window_width / game_width;
            if (game_height > 1.f && window_height > 1.f) click_scale_y = window_height / game_height;
        } catch (...) {
        }
    }
};

inline Driver& driver() { static Driver value; return value; }
}  // namespace tc_demo_driver
#endif  // TC_DEMO_DRIVER
