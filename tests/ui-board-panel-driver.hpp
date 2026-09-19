/* In-process driver for the board side-panel playtest.

   Compiled into a *test* build of examples/board-panel (see build.ps1); the
   shipped example package does not contain it.

   Why in-process: the sandbox game window is parked off the desktop and its
   framebuffer cannot be read from outside, so the game itself clicks.  The
   driver also records what the *game* reads, not only what the plugin draws:
   build_board_ui samples igIsAnyItemActive (0x14046b58e),
   igIsWindowBgActive (0x14046b5ae) and igIsWindowHovered (0x14046b5dd) and
   hands the resulting pair to handle_io_on_board(), which is where every
   board mouse action starts.  Logging those three values while a real click
   is in flight is what turns "the click must not fall through to the board"
   into evidence instead of a claim.

   The driver never guesses a click position: the panel's own screen rectangle
   comes from the loader's log line and the button's position inside it from
   the plugin, both handed over in TC_BOARD_CLICK by the playtest script. */
#pragma once
#ifdef TC_BOARD_DRIVER
#include "../sdk/tc_game_model.h"
#include "../sdk/tc_event.h"
#include <windows.h>
#include <cstring>
#include <string>
#include <vector>

namespace tc_board_driver {

struct V2 { float x, y; };

/* Verified call sites of the pinned build (see src/compat.hpp). */
constexpr uintptr_t kBoardSampleRva = 0x46b593;   /* return of igIsAnyItemActive  */
constexpr uintptr_t kBoardBgRva     = 0x46b5ae;   /* return of igIsWindowBgActive */
constexpr uintptr_t kBoardHoverRva  = 0x46b5e2;   /* return of igIsWindowHovered  */
constexpr uintptr_t kHomeStartRva   = 0x449df0;
constexpr uintptr_t kHomeEndRva     = 0x44b610;

inline int frameCount();

inline uintptr_t callerRva(void* returnAddress) {
    return reinterpret_cast<uintptr_t>(returnAddress) -
           reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

struct Driver {
    const TCHost* host = nullptr;
    void (*log)(const char*) = nullptr;
    HWND window = nullptr;

    /* Hooks installed by start(). */
    bool (*invisibleOriginal)(const char*, V2, int) = nullptr;
    bool (*updateWireOriginal)(void*, void*, void*, uint32_t, uint8_t) = nullptr;
    uint32_t (*boardUiOriginal)(void*, void*) = nullptr;
    bool (*activeOriginal)() = nullptr;
    bool (*bgActiveOriginal)() = nullptr;
    bool (*windowHoveredOriginal)(int) = nullptr;
    void (*loadLevelOriginal)(void*, const tc::TCNimString*) = nullptr;
    void (*changeSceneOriginal)(void*, int) = nullptr;
    /* The context pointer and scene ids the game itself uses, recorded from
       its own change_scene() calls, so the driver's own scene change uses the
       same object the game does instead of a guess. */
    void* scene_context = nullptr;
    /* The context is learned from the loader's SCENE_CHANGE event (it carries
       the value the game itself passed to change_scene), or from this driver's
       own hook when an older loader leaves the point hookable. */
    bool scene_context_logged = false;
    int scene_args[6]{};
    int scene_calls = 0;
    tc::TCGameModel game;
    bool game_loaded = false;

    /* "installed" is set when the hooks are in place, "started" on the first
       on_frame tick: they are different events, and sharing one flag is what
       made the first tick skip its own setup (window, click point). */
    bool installed = false, started = false, done = false, entered = false, left = false;
    int start_frame = 0, menu_clicks = 0, menu_frame = -1;
    double start_time = 0.0, elapsed = 0.0;
    void* model = nullptr;
    void* board_context = nullptr;
    int board_frames = 0, board_frames_at_entry = 0, board_frames_at_leave = -1;
    int board_frames_after_leave = 0;

    /* Panel state, reported by the plugin's draw callback.  All panel-local. */
    int panel_frames = 0, panel_first_frame = -1;
    float content_w = 0.f, content_h = 0.f;
    float ping_x = -1.f, ping_y = -1.f, ping_w = 0.f, ping_h = 0.f;
    int ping_clicks = 0;
    bool logged_panel = false, logged_item = false;

    /* Click target: game coordinates plus the game -> client scale, both from
       TC_BOARD_CLICK (written by the playtest script out of the two log
       lines).  Format: "scale <gameW> <gameH> <clientW> <clientH>;ping <x> <y>;
       sentinel <x> <y>;canvas <x> <y>" - every point is in game coordinates. */
    bool click_known = false;
    float click_x = 0.f, click_y = 0.f;
    bool sentinel_known = false;
    float sentinel_x = 0.f, sentinel_y = 0.f;
    /* The button's own absolute position, reported by the plugin while it
       draws (screen/ImGui coordinates).  Clicking this instead of a point the
       script computed keeps the test honest after a resize. */
    bool ping_screen_known = false;
    float ping_screen_x = 0.f, ping_screen_y = 0.f;
    /* Press point of the content region's scrollbar, in game coordinates: the
       drag from there down is what proves the region scrolls for a user. */
    bool scrollbar_known = false;
    float scrollbar_x = 0.f, scrollbar_y = 0.f;
    /* Header toggle of the panel, in game coordinates (from the loader's
       geometry line, read by the playtest script). */
    bool toggle_known = false;
    float toggle_x = 0.f, toggle_y = 0.f;
    float scale_x = 1.f, scale_y = 1.f;
    /* Positive control: a point on the circuit board itself, away from the
       panel.  Both are game (surface) coordinates. */
    float canvas_x = 1280.f, canvas_y = 700.f;

    /* Content region: the offset the plugin reports each frame (it carries the
       scroll offset) and the clipped item below the fold. */
    float content_offset = 0.f, content_offset_at_start = 0.f;
    bool offset_known = false, scrolled = false;
    int scroll_stage = 0;
    float sentinel_local_x = -1.f, sentinel_local_y = -1.f;
    float sentinel_w = 0.f, sentinel_h = 0.f;
    int sentinel_hovers = 0, sentinel_clicks = 0;
    bool sentinel_last_hovered = false;
    /* Window-size stage: the client size is changed while the panel is up, and
       the button is clicked again at the position the plugin reports *after*
       the change.  That is the automatable half of the DPI question: whatever
       the surface/client ratio becomes, the game's own coordinates, our
       geometry and the real mouse must still agree. */
    bool resize_sent = false, resize_clicked = false;
    int resize_frame = -1, resize_click_frame = -1, scroll_done_frame = -1;
    /* Fold/unfold stage: collapse, try the button (must not fire), expand, click
       the button again (must fire). */
    int collapse_click_frame = -1, collapsed_ping_frame = -1, expand_click_frame = -1;
    bool collapse_clicked = false, collapsed_ping_sent = false, expand_clicked = false;
    bool expand_ping_sent = false;

    /* The game's own input sampling, once per frame while the board draws. */
    int observed_active = -1, observed_bg = -1, observed_hover = -1, observed_frame = -1;
    int last_state = -1, last_state_frame = -1, idle_lines = 0;
    /* 0 = no click in flight, 1 = panel click window, 2 = canvas click window */
    int tag = 0, tag_until_frame = -1, last_tag = -1;
    int panel_clicks_sent = 0, canvas_clicks_sent = 0, sentinel_clicks_sent = 0;
    int panel_click_frame = -1, sentinel_click_frame = -1, canvas_click_frame = -1;
    int board_frames_at_panel_click = -1, board_frames_at_canvas_click = -1;
    int board_frames_at_sentinel_click = -1;
    /* Board-side effects: a wire/board click handler running is the thing a
       click that fell through would cause. */
    int wire_calls = 0, wire_calls_at_panel_click = -1, wire_calls_at_canvas_click = -1;
    int wire_calls_logged = -1;
    /* Which call sites of the sampled entry points actually run.  A board
       whose per-frame code never reaches the sampling site is exactly what
       "the panel never drew" looks like, so the driver reports the first few
       distinct call sites of each. */
    int active_calls = 0, bg_calls = 0, hover_calls = 0;
    uintptr_t active_sites[6]{}, bg_sites[6]{}, hover_sites[6]{};
    int active_site_count = 0, bg_site_count = 0, hover_site_count = 0;

    void say(const std::string& message) { if (log) log(message.c_str()); }

    void readConfiguration() {
        char buffer[512]{};
        const DWORD length = GetEnvironmentVariableA("TC_BOARD_CLICK", buffer, sizeof(buffer));
        if (length == 0 || length >= sizeof(buffer)) return;
        const std::string value(buffer, length);
        /* "scale <gameW> <gameH> <clientW> <clientH>;ping <x> <y>;…" */
        try {
            for (size_t entry = 0; entry < value.size();) {
                size_t end = value.find(';', entry);
                if (end == std::string::npos) end = value.size();
                std::vector<float> numbers;
                std::string name;
                for (size_t start = entry; start < end;) {
                    size_t space = value.find(' ', start);
                    if (space == std::string::npos || space > end) space = end;
                    const std::string field = value.substr(start, space - start);
                    if (name.empty()) name = field;
                    else if (!field.empty()) numbers.push_back(std::stof(field));
                    if (space >= end) break;
                    start = space + 1;
                }
                if (name == "scale" && numbers.size() >= 4 && numbers[0] > 1.f &&
                    numbers[1] > 1.f && numbers[2] > 1.f && numbers[3] > 1.f) {
                    scale_x = numbers[2] / numbers[0];
                    scale_y = numbers[3] / numbers[1];
                } else if (name == "ping" && numbers.size() >= 2) {
                    click_x = numbers[0]; click_y = numbers[1];
                    click_known = click_x > 0.f && click_y > 0.f;
                } else if (name == "sentinel" && numbers.size() >= 2) {
                    sentinel_x = numbers[0]; sentinel_y = numbers[1];
                    sentinel_known = sentinel_x > 0.f && sentinel_y > 0.f;
                } else if (name == "canvas" && numbers.size() >= 2) {
                    canvas_x = numbers[0]; canvas_y = numbers[1];
                } else if (name == "scrollbar" && numbers.size() >= 2) {
                    scrollbar_x = numbers[0]; scrollbar_y = numbers[1];
                    scrollbar_known = scrollbar_x > 0.f && scrollbar_y > 0.f;
                } else if (name == "collapse" && numbers.size() >= 2) {
                    toggle_x = numbers[0]; toggle_y = numbers[1];
                    toggle_known = toggle_x > 0.f && toggle_y > 0.f;
                }
                entry = end + 1;
            }
        } catch (...) {
            click_known = false;
        }
    }

    /* Moves the cursor to the client position and queues a real mouse event
       pair in the game's own message loop.  Both steps matter: this build's
       game reads the cursor position itself (a synthetic WM_MOUSEMOVE with a
       made-up lParam leaves ImGui's mouse at 0,0), while the button messages
       are what make ImGui see a press and a release in different frames. */
    void clickClient(int x, int y) {
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

    void clickGame(float gameX, float gameY) {
        clickClient(static_cast<int>(gameX * scale_x), static_cast<int>(gameY * scale_y));
    }

    /* Where to click for the Ping button: the plugin's own absolute report when
       it has one, otherwise the point the playtest script computed. */
    bool pingPoint(float& x, float& y) {
        if (ping_screen_known && ping_w > 0.f && ping_h > 0.f) {
            x = ping_screen_x + ping_w * 0.5f;
            y = ping_screen_y + ping_h * 0.5f;
            return true;
        }
        if (click_known) {
            x = click_x;
            y = click_y;
            return true;
        }
        return false;
    }

    void resizeWindow(float factor) {
        RECT client{};
        if (!window || !GetClientRect(window, &client)) return;
        const int width = static_cast<int>(client.right * factor);
        const int height = static_cast<int>(client.bottom * factor);
        if (width < 640 || height < 480) return;
        SetWindowPos(window, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /* Queues wheel notches with the cursor over the panel.  WM_MOUSEWHEEL
       carries *screen* coordinates in lParam (unlike the press messages), and
       the delta is a signed 16-bit value in the high word. */
    void wheelOverPanel(int notches) {
        POINT screen{static_cast<int>(click_x * scale_x), static_cast<int>(click_y * scale_y)};
        ClientToScreen(window, &screen);
        SetCursorPos(screen.x, screen.y);
        Sleep(30);
        for (int index = 0; index < notches; ++index) {
            const WPARAM delta = MAKEWPARAM(0, static_cast<WORD>(-120));
            /* SendMessageW, not PostMessageW: the wheel is consumed by the
               window procedure (GLFW -> the game's ImGui backend) rather than
               by a queue entry this process has to wait for. */
            SendMessageW(window, WM_MOUSEWHEEL, delta, MAKELPARAM(screen.x, screen.y));
            Sleep(35);
        }
    }

    /* Real drag with the left button held: the content region's scrollbar is
       an ordinary ImGui item, so pressing on its grab and moving down scrolls
       the region exactly like a user would. */
    void dragGame(float gameX, float gameY, float deltaY) {
        const int clientX = static_cast<int>(gameX * scale_x);
        const int startY = static_cast<int>(gameY * scale_y);
        const int endY = static_cast<int>((gameY + deltaY) * scale_y);
        POINT screen{clientX, startY};
        ClientToScreen(window, &screen);
        SetCursorPos(screen.x, screen.y);
        Sleep(40);
        PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(clientX, startY));
        Sleep(40);
        PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(clientX, startY));
        for (int step = 1; step <= 6; ++step) {
            const int y = startY + (endY - startY) * step / 6;
            Sleep(45);
            screen.y = y;
            POINT moved{clientX, y};
            ClientToScreen(window, &moved);
            SetCursorPos(moved.x, moved.y);
            PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(clientX, y));
        }
        Sleep(50);
        PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(clientX, endY));
    }

    static BOOL CALLBACK windowCallback(HWND candidate, LPARAM data) {
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
        EnumWindows(windowCallback, reinterpret_cast<LPARAM>(&found));
        return found;
    }

    void loadLevel() {
        if (!loadLevelOriginal || !model || !game_loaded) return;
        const char* name = "sandbox";
        tc::TCNimString level{};
        const size_t length = std::strlen(name);
        game.raw_new_string(&level, static_cast<int64_t>(length));
        if (!level.data) {
            say("DRIVER: cannot allocate the level name");
            return;
        }
        std::memcpy(static_cast<unsigned char*>(level.data) + 8, name, length);
        level.length = length;
        static_cast<unsigned char*>(level.data)[8 + length] = 0;
        loadLevelOriginal(model, &level);
        entered = true;
        board_frames_at_entry = board_frames;
        say("DRIVER: entered the sandbox level (board frames so far=" +
            std::to_string(board_frames) + ")");
    }

    void changeScene() {
        /* The game's own change_scene context, from the loader's SCENE_CHANGE
           event (or, on a loader that still lets the point be hooked, from this
           driver's detour).  The fallbacks are guesses and are only used when
           neither arrived. */
        void* context = scene_context ? scene_context : (model ? model : board_context);
        if (!changeSceneOriginal || !context) {
            say("DRIVER: cannot change the scene (no context)");
            return;
        }
        say("DRIVER: calling change_scene ctx=" + hex((uintptr_t)context) + " scene=0");
        changeSceneOriginal(context, 0);
    }

    /* One entry point for everything the driver does after the frame starts. */
    /* The game's own "which level is loaded" string, so a run can tell "the
       level never loaded" from "the level loaded but the board UI is not
       reaching its input sampling site". */
    std::string currentLevel() {
        if (!host || !host->resolve_symbol) return "?";
        const auto value = reinterpret_cast<const tc::TCNimString*>(
            host->resolve_symbol(host->context, "loaded_level__modelZmodel95types_u840"));
        if (!value || !value->data || value->length == 0 || value->length > 64) return "(none)";
        return std::string(static_cast<const char*>(value->data) + 8, value->length);
    }

    static std::string hex(uintptr_t value) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (int shift = 60; shift >= 0; shift -= 4) out.push_back(digits[(value >> shift) & 0xf]);
        const size_t first = out.find_first_not_of('0');
        return first == std::string::npos ? std::string("0") : out.substr(first);
    }

    void noteSite(uintptr_t rva, uintptr_t* sites, int& count, const char* name, int total) {
        for (int index = 0; index < count; ++index)
            if (sites[index] == rva) return;
        if (count >= 6) return;
        sites[count++] = rva;
        say(std::string("DRIVER: ") + name + " first called from rva=0x" + hex(rva) +
            " (call #" + std::to_string(total) + ")");
    }

    void tick(const TCFrame* frame) {
        if (!frame || done) return;
        if (!started) {
            started = true;
            start_frame = frame->frame_number;
            start_time = frame->time_seconds;
            readConfiguration();
            window = findWindow();
            /* Park the window at the desktop origin for the duration of the
               test: the game derives its mouse position from the real cursor,
               and screen, client and game coordinates only line up there. */
            if (window)
                SetWindowPos(window, nullptr, 0, 0, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            /* Wheel input is delivered to the window procedure, and some
               backends only translate it for the foreground window. */
            if (window) SetForegroundWindow(window);
            RECT client{};
            if (window) GetClientRect(window, &client);
            say("DRIVER: started frame=" + std::to_string(frame->frame_number) + " window=" +
                std::to_string((uintptr_t)window) + " client=" + std::to_string(client.right) +
                "x" + std::to_string(client.bottom) + " click=" +
                (click_known ? "known" : "missing"));
        }
        elapsed = frame->time_seconds - start_time;
        /* One heartbeat every ~10 s: it separates "the game stopped calling us"
           (a hang or a crash after a scene change) from "the board is simply
           not drawing the panel". */
        if (frame->frame_number - start_frame > 0 && (frame->frame_number - start_frame) % 600 == 0)
            say("DRIVER: alive frame=" + std::to_string(frame->frame_number) + " entered=" +
                std::to_string(entered ? 1 : 0) + " left=" + std::to_string(left ? 1 : 0) +
                " board=" + std::to_string(board_frames) + " panel=" +
                std::to_string(panel_frames) + " level=" + currentLevel() + " samples=" +
                std::to_string(active_calls) + "/" + std::to_string(bg_calls) + "/" +
                std::to_string(hover_calls));

        /* The board model only becomes known once the game has built a board,
           so the driver first presses one of the home page's own entries (see
           the igInvisibleButton hook) and then loads the sandbox level. */
        if (!entered && model) loadLevel();
        if (!entered) {
            if (frame->frame_number - start_frame > 1800) {
                done = true;
                say("DRIVER: never reached a board; giving up");
            }
            return;
        }

        if (panel_click_frame < 0 && click_known && panel_frames > 30 &&
            frame->frame_number > start_frame + 240) {
            panel_click_frame = frame->frame_number;
            board_frames_at_panel_click = board_frames;
            wire_calls_at_panel_click = wire_calls;
            tag = 1;
            tag_until_frame = frame->frame_number + 15;
            ++panel_clicks_sent;
            float point_x = click_x, point_y = click_y;
            pingPoint(point_x, point_y);
            clickGame(point_x, point_y);
            say("DRIVER: panel click sent " + std::to_string((int)point_x) + "," +
                std::to_string((int)point_y) + " tag=panel");
        }
        if (panel_click_frame > 0 && canvas_click_frame < 0 &&
            sentinel_click_frame > 0 && resize_clicked &&
            frame->frame_number > resize_click_frame + 60) {
            canvas_click_frame = frame->frame_number;
            board_frames_at_canvas_click = board_frames;
            wire_calls_at_canvas_click = wire_calls;
            tag = 2;
            tag_until_frame = frame->frame_number + 15;
            ++canvas_clicks_sent;
            clickGame(canvas_x, canvas_y);
            say("DRIVER: canvas click sent " + std::to_string((int)canvas_x) + "," +
                std::to_string((int)canvas_y) + " tag=canvas");
        }
        /* The clipped item below the fold: clicking where it would be must not
           reach it (ImGui culls it) and must not reach the board either. */
        if (panel_click_frame > 0 && sentinel_click_frame < 0 && sentinel_known &&
            frame->frame_number > panel_click_frame + 60) {
            sentinel_click_frame = frame->frame_number;
            sentinel_clicks_sent = 1;
            board_frames_at_sentinel_click = board_frames;
            tag = 3;
            tag_until_frame = frame->frame_number + 15;
            clickGame(sentinel_x, sentinel_y);
            say("DRIVER: sentinel click sent " + std::to_string((int)sentinel_x) + "," +
                std::to_string((int)sentinel_y) + " tag=sentinel (local " +
                std::to_string((int)sentinel_local_x) + "," +
                std::to_string((int)sentinel_local_y) + ")");
        }
        /* Scrolling: wheel notches over the panel must move the content
           region, and the board must not treat them as its own input. */
        if (sentinel_click_frame > 0 && scroll_stage == 0 &&
            frame->frame_number > sentinel_click_frame + 45) {
            scroll_stage = 1;
            tag = 4;
            tag_until_frame = frame->frame_number + 25;
            content_offset_at_start = content_offset;
            say("DRIVER: wheel sent over the panel, offset before=" +
                std::to_string((int)content_offset) + ", clipped probe before scrolling hovers=" +
                std::to_string(sentinel_hovers) + " clicks=" + std::to_string(sentinel_clicks));
            wheelOverPanel(6);
        }
        /* A user drags the content region's scrollbar; the wheel is not a
           reliable input path in this build (see docs), the scrollbar is. */
        if (scroll_stage == 1 && scrollbar_known &&
            frame->frame_number > sentinel_click_frame + 120) {
            scroll_stage = 2;
            tag = 5;
            tag_until_frame = frame->frame_number + 45;
            say("DRIVER: scrollbar drag sent " + std::to_string((int)scrollbar_x) + "," +
                std::to_string((int)scrollbar_y) + " +220 tag=scrollbar");
            dragGame(scrollbar_x, scrollbar_y, 220.f);
        }
        if (scroll_stage == 2 && frame->frame_number > sentinel_click_frame + 300) {
            scroll_stage = 3;
            scroll_done_frame = frame->frame_number;
            /* The authoritative scroll offset is the loader's own line
               ("Board panel content scroll ... y=…"); the playtest asserts on
               that, because the plugin's cursor reading is not scroll-based in
               this build. */
            say("DRIVER: scroll stage done, content offset now=" +
                std::to_string((int)content_offset));
        }
        /* The window gets smaller while the panel is up; the button must still
           be clickable where the plugin says it is.  This runs after the fold
           test: that one uses the toggle position recorded by the probe phase,
           and the resize would move it. */
        if (expand_ping_sent && !resize_sent && frame->frame_number > expand_click_frame + 120) {
            resize_sent = true;
            resize_frame = frame->frame_number;
            RECT before{};
            if (window) GetClientRect(window, &before);
            resizeWindow(0.7f);
            RECT after{};
            if (window) GetClientRect(window, &after);
            say("DRIVER: resized the game window from " + std::to_string(before.right) + "x" +
                std::to_string(before.bottom) + " to " + std::to_string(after.right) + "x" +
                std::to_string(after.bottom));
        }
        if (resize_sent && !resize_clicked && frame->frame_number > resize_frame + 60) {
            float point_x = 0.f, point_y = 0.f;
            if (pingPoint(point_x, point_y)) {
                resize_clicked = true;
                resize_click_frame = frame->frame_number;
                tag = 6;
                tag_until_frame = frame->frame_number + 15;
                clickGame(point_x, point_y);
                say("DRIVER: panel click after resize sent " + std::to_string((int)point_x) + "," +
                    std::to_string((int)point_y) + " tag=after-resize");
            }
        }
        /* Fold / unfold: the host's toggle folds the panel, the plugin's button
           must stop being clickable while folded, and unfolding brings it back. */
        if (scroll_stage >= 3 && !collapse_clicked && toggle_known && scroll_done_frame > 0 &&
            frame->frame_number > scroll_done_frame + 30) {
            collapse_clicked = true;
            collapse_click_frame = frame->frame_number;
            tag = 7;
            tag_until_frame = frame->frame_number + 15;
            clickGame(toggle_x, toggle_y);
            say("DRIVER: collapse toggle clicked at " + std::to_string((int)toggle_x) + "," +
                std::to_string((int)toggle_y) + " tag=collapse");
        }
        if (collapse_clicked && !collapsed_ping_sent &&
            frame->frame_number > collapse_click_frame + 50) {
            collapsed_ping_sent = true;
            collapsed_ping_frame = frame->frame_number;
            float point_x = 0.f, point_y = 0.f;
            if (pingPoint(point_x, point_y)) {
                tag = 8;
                tag_until_frame = frame->frame_number + 15;
                clickGame(point_x, point_y);
                say("DRIVER: button click while folded sent " + std::to_string((int)point_x) + "," +
                    std::to_string((int)point_y) + " tag=folded (must not register)");
            }
        }
        if (collapsed_ping_sent && !expand_clicked &&
            frame->frame_number > collapsed_ping_frame + 60) {
            expand_clicked = true;
            expand_click_frame = frame->frame_number;
            tag = 9;
            tag_until_frame = frame->frame_number + 15;
            clickGame(toggle_x, toggle_y);
            say("DRIVER: expand toggle clicked at " + std::to_string((int)toggle_x) + "," +
                std::to_string((int)toggle_y) + " tag=expand");
        }
        if (expand_clicked && !expand_ping_sent && frame->frame_number > expand_click_frame + 60) {
            float point_x = 0.f, point_y = 0.f;
            if (pingPoint(point_x, point_y)) {
                expand_ping_sent = true;
                tag = 10;
                tag_until_frame = frame->frame_number + 15;
                clickGame(point_x, point_y);
                say("DRIVER: button click after expanding sent " + std::to_string((int)point_x) +
                    "," + std::to_string((int)point_y) + " tag=expanded");
            }
        }
        /* Leaving the level is the scene-lifecycle check: the panel is drawn by
           the board's own code, so it has to stop drawing together with it. */
        if (canvas_click_frame > 0 && !left && frame->frame_number > canvas_click_frame + 70) {
            left = true;
            board_frames_at_leave = board_frames;
            say("DRIVER: leaving the board");
            changeScene();
        }
        if (left && board_frames_at_leave >= 0 && board_frames == board_frames_at_leave) {
            if (++board_frames_after_leave == 90) {
                done = true;
                say("DRIVER: board stopped drawing after the scene change; panel frames=" +
                    std::to_string(panel_frames) + " wires=" + std::to_string(wire_calls) +
                    " (at panel click " + std::to_string(wire_calls_at_panel_click) +
                    ", at canvas click " + std::to_string(wire_calls_at_canvas_click) +
                    "), content offset " + std::to_string((int)content_offset_at_start) + "->" +
                    std::to_string((int)content_offset) + " scrolled=" +
                    (scrolled ? "1" : "0") + " clipped probe hovers=" +
                    std::to_string(sentinel_hovers) + " clicks=" +
                    std::to_string(sentinel_clicks));
            }
        }
        if (frame->frame_number - start_frame > 4000) {
            done = true;
            say("DRIVER: giving up after the frame budget; board frames=" +
                std::to_string(board_frames) + " panel frames=" + std::to_string(panel_frames));
        }
    }

    /* Called by the plugin every frame its panel draws. */
    void panelFrame(int frame_number, float width, float height) {
        ++panel_frames;
        if (panel_first_frame < 0) panel_first_frame = frame_number;
        if (content_w != width || content_h != height) {
            const bool first = panel_frames == 1;
            content_w = width;
            content_h = height;
            /* A size change after the first frame is the window being resized
               (or the panel being re-laid out): the playtest uses it to prove
               the content region followed the window. */
            if (!first)
                say("DRIVER: panel content resized to " + std::to_string((int)width) + "x" +
                    std::to_string((int)height));
        }
        if (!logged_panel) {
            logged_panel = true;
            say("DRIVER: panel first drawn on frame " + std::to_string(frame_number) +
                ", content " + std::to_string((int)width) + "x" +
                std::to_string((int)height));
        }
    }

    /* Called by the plugin when it draws its clickable button: the origin
       before the item and the size right after it, both in panel-local
       coordinates.  The combined line is what the playtest turns into a click
       point, so the driver never has to guess the layout. */
    void panelItemOrigin(const char* label, float x, float y) {
        if (logged_item || !label || std::string(label) != "Ping") return;
        ping_x = x; ping_y = y;
        say("DRIVER: panel button " + std::string(label) + " origin=" +
            std::to_string((int)x) + "," + std::to_string((int)y));
    }

    void panelItemSize(const char* label, float width, float height) {
        if (logged_item || !label || std::string(label) != "Ping") return;
        if (ping_x < 0.f || width <= 0.f || height <= 0.f) return;
        logged_item = true;
        ping_w = width; ping_h = height;
        say("DRIVER: panel button " + std::string(label) + " size=" +
            std::to_string((int)width) + "x" + std::to_string((int)height));
    }

    void panelClicked(int count) {
        ping_clicks = count;
        say("DRIVER: panel button click reported count=" + std::to_string(count));
    }

    void panelHovered(int frame_number) {
        say("DRIVER: panel button hovered on frame " + std::to_string(frame_number));
    }

    /* The plugin's own absolute position for the button it is about to draw. */
    void panelItemScreen(float x, float y) {
        if (ping_screen_known && ping_screen_x == x && ping_screen_y == y) return;
        ping_screen_known = x > 0.f && y > 0.f;
        ping_screen_x = x;
        ping_screen_y = y;
        if (ping_screen_known)
            say("DRIVER: panel button screen=" + std::to_string((int)x) + "," +
                std::to_string((int)y));
    }

    /* The content region's own cursor position, reported by the plugin every
       frame: it carries the scroll offset, so a change is a scroll. */
    void panelOffset(float value) {
        if (!offset_known) {
            offset_known = true;
            content_offset = value;
            content_offset_at_start = value;
            say("DRIVER: content offset y=" + std::to_string((int)value));
            return;
        }
        if (value == content_offset) return;
        const float previous = content_offset;
        content_offset = value;
        if (scroll_stage >= 1 && !scrolled) {
            scrolled = true;
            say("DRIVER: content scrolled from " + std::to_string((int)previous) + " to " +
                std::to_string((int)value));
        } else {
            say("DRIVER: content offset y=" + std::to_string((int)value));
        }
    }

    void panelSentinelOrigin(float x, float y) {
        sentinel_local_x = x;
        sentinel_local_y = y;
    }

    void panelSentinelSize(float width, float height) {
        if (sentinel_w == width && sentinel_h == height) return;
        sentinel_w = width;
        sentinel_h = height;
        say("DRIVER: clipped probe item local=" + std::to_string((int)sentinel_local_x) + "," +
            std::to_string((int)sentinel_local_y) + " size=" + std::to_string((int)width) + "x" +
            std::to_string((int)height));
    }

    void panelSentinelState(bool hovered, bool clicked) {
        if (hovered) ++sentinel_hovers;
        if (clicked) ++sentinel_clicks;
        /* Log transitions only: once the region is scrolled the item can come
           into view, and one line per frame would drown the log. */
        if (hovered == sentinel_last_hovered && !clicked) return;
        sentinel_last_hovered = hovered;
        if (hovered || clicked)
            say("DRIVER: clipped probe item hovered=" + std::to_string(hovered ? 1 : 0) +
                " clicked=" + std::to_string(clicked ? 1 : 0));
    }

    /* Records what the game read at its own board input sampling site.  Once
       all three values of a frame are in, the gate the game derives from them
       is logged whenever it changes, and periodically while a click of ours is
       in flight.  The first byte of the pair build_board_ui returns is
       "no ImGui item outside the window background is active" (the function
       inverts it at 0x14046bbc7), the second is the window-hovered answer. */
    void observeSample(int kind, bool value) {
        const int frame = frameCount();
        if (frame != observed_frame) {
            observed_frame = frame;
            observed_active = observed_bg = observed_hover = -1;
        }
        if (kind == 0) observed_active = value ? 1 : 0;
        if (kind == 1) observed_bg = value ? 1 : 0;
        if (kind == 2) observed_hover = value ? 1 : 0;
        /* The window-hovered sample is read last, so this is the state the
           game acts on.  igIsWindowBgActive is only consulted when an item is
           active (0x14046b5a7), so "not called this frame" means "not active",
           not "unknown". */
        if (kind != 2 || observed_active < 0 || observed_hover < 0) return;
        const int busy = (observed_active && observed_bg == 0) ? 1 : 0;
        const int state = (busy << 1) | observed_hover;
        const bool tagged = tag != 0;
        const bool changed = state != last_state || tag != last_tag;
        const bool periodic = tagged && (frame % 5 == 0);
        if (!changed && !periodic) return;
        if (state == 0 && !tagged && !changed) return;
        if (state == 0 && !tagged && idle_lines++ > 1) return;
        last_state = state;
        last_tag = tag;
        last_state_frame = frame;
        const char* name = tag == 1 ? "panel"
                          : tag == 2 ? "canvas"
                          : tag == 3 ? "sentinel"
                          : tag == 4 ? "wheel"
                          : tag == 5 ? "scrollbar"
                          : tag == 6 ? "after-resize"
                          : tag == 7 ? "collapse"
                          : tag == 8 ? "folded"
                          : tag == 9 ? "expand"
                          : tag == 10 ? "expanded"
                                     : "idle";
        say(std::string("DRIVER: gate[") + name + "] frame=" + std::to_string(frame) +
            " active=" + std::to_string(observed_active) + " bg=" +
            std::to_string(observed_bg) + " hovered=" + std::to_string(observed_hover) +
            " => busy=" + std::to_string(busy));
    }

    /* Called every frame from the plugin's on_frame, after tick(): expires the
       click tag so a stale tag cannot colour a later gate line. */
    void expireTag(int frame_number) {
        if (tag != 0 && frame_number > tag_until_frame) tag = 0;
    }
};

inline Driver& driver() { static Driver value; return value; }

/* The board's own call sites are the only ones that matter; every other call
   goes straight through untouched. */
inline bool hookActive() {
    auto& d = driver();
    ++d.active_calls;
    const bool result = d.activeOriginal ? d.activeOriginal() : false;
    const uintptr_t rva = callerRva(__builtin_return_address(0));
    d.noteSite(rva, d.active_sites, d.active_site_count, "igIsAnyItemActive", d.active_calls);
    if (rva == kBoardSampleRva) d.observeSample(0, result);
    return result;
}
inline bool hookBgActive() {
    auto& d = driver();
    ++d.bg_calls;
    const bool result = d.bgActiveOriginal ? d.bgActiveOriginal() : false;
    const uintptr_t rva = callerRva(__builtin_return_address(0));
    d.noteSite(rva, d.bg_sites, d.bg_site_count, "igIsWindowBgActive", d.bg_calls);
    if (rva == kBoardBgRva) d.observeSample(1, result);
    return result;
}
inline bool hookWindowHovered(int flags) {
    auto& d = driver();
    ++d.hover_calls;
    const bool result = d.windowHoveredOriginal ? d.windowHoveredOriginal(flags) : false;
    const uintptr_t rva = callerRva(__builtin_return_address(0));
    d.noteSite(rva, d.hover_sites, d.hover_site_count, "igIsWindowHovered", d.hover_calls);
    if (rva == kBoardHoverRva) d.observeSample(2, result);
    return result;
}
inline bool hookInvisible(const char* id, V2 size, int flags) {
    auto& d = driver();
    const bool result = d.invisibleOriginal ? d.invisibleOriginal(id, size, flags) : false;
    const uintptr_t rva = callerRva(__builtin_return_address(0));
    /* Press the second home page entry once, to get from the menu into a level
       without inventing a scene change of our own. */
    if (!d.entered && rva >= kHomeStartRva && rva < kHomeEndRva && d.elapsed > 3.0) {
        const int frame = frameCount();
        if (frame != d.menu_frame) { d.menu_frame = frame; d.menu_clicks = 0; }
        if (++d.menu_clicks == 2) {
            d.say("DRIVER: pressing a home page entry to reach a board");
            return true;
        }
    }
    return result;
}
inline bool hookUpdateWire(void* m, void* context, void* input, uint32_t point, uint8_t fifth) {
    auto& d = driver();
    if (!d.model) d.model = m;
    ++d.wire_calls;
    return d.updateWireOriginal ? d.updateWireOriginal(m, context, input, point, fifth) : false;
}
/* build_board_ui(context, board): its first argument is the same context the
   board's own toolbar hands to change_scene(), and its return value is the
   pair the game acts on - so this detour forwards it. */
/* Only reachable when the loader of the running build still lets this point be
   hooked; with the current loader the event below is the path that fills
   scene_context in. */
inline void hookChangeScene(void* context, int scene) {
    auto& d = driver();
    if (d.scene_calls < 6) {
        d.scene_args[d.scene_calls++] = scene;
        d.scene_context = context;
        d.say("DRIVER: the game called change_scene ctx=0x" + Driver::hex((uintptr_t)context) +
              " scene=" + std::to_string(scene));
    }
    if (d.changeSceneOriginal) d.changeSceneOriginal(context, scene);
}
/* The loader raises this before it calls the game's own change_scene, and its
   subject is the context the game passed, so the driver's own scene change can
   use exactly the object the game uses instead of guessing one. */
inline void onSceneChangeEvent(TCEvent* event) {
    auto& d = driver();
    void* context = tc::events::sceneContext(event);
    if (context) {
        d.scene_context = context;
        if (!d.scene_context_logged) {
            d.scene_context_logged = true;
            d.say("DRIVER: scene change event scene=" +
                  std::to_string(static_cast<int>(event->flags)) + " ctx=0x" + Driver::hex(
                      reinterpret_cast<uintptr_t>(context)));
        }
    }
}
inline uint32_t hookBoardUi(void* context, void* board) {
    auto& d = driver();
    if (!d.board_context) d.board_context = context;
    ++d.board_frames;
    return d.boardUiOriginal ? d.boardUiOriginal(context, board) : 0;
}

inline bool start(const TCHost* host, void (*logFunction)(const char*)) {
    auto& d = driver();
    if (d.installed || !host || !host->create_hook || !host->resolve_symbol) return false;
    d.host = host;
    d.log = logFunction;
    d.game_loaded = d.game.load(host) && d.game.raw_new_string != nullptr;
    d.loadLevelOriginal = reinterpret_cast<decltype(d.loadLevelOriginal)>(
        host->resolve_symbol(host->context, "load_level__modelZutilities_u7740"));
    d.changeSceneOriginal = reinterpret_cast<decltype(d.changeSceneOriginal)>(
        host->resolve_symbol(host->context, "change_scene__presenterZcontext_u2958"));
    if (!d.loadLevelOriginal || !d.changeSceneOriginal) {
        d.say("DRIVER: level entry symbols are missing");
        return false;
    }
    struct Target { const char* symbol; void* detour; void** original; };
    const Target targets[] = {
        {"igIsAnyItemActive", reinterpret_cast<void*>(hookActive),
         reinterpret_cast<void**>(&d.activeOriginal)},
        {"igIsWindowBgActive", reinterpret_cast<void*>(hookBgActive),
         reinterpret_cast<void**>(&d.bgActiveOriginal)},
        {"igIsWindowHovered", reinterpret_cast<void*>(hookWindowHovered),
         reinterpret_cast<void**>(&d.windowHoveredOriginal)},
        {"igInvisibleButton", reinterpret_cast<void*>(hookInvisible),
         reinterpret_cast<void**>(&d.invisibleOriginal)},
        {"build_board_ui__presenterZboard95ui_u15", reinterpret_cast<void*>(hookBoardUi),
         reinterpret_cast<void**>(&d.boardUiOriginal)},
        {"handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5",
         reinterpret_cast<void*>(hookUpdateWire),
         reinterpret_cast<void**>(&d.updateWireOriginal)},
    };
    for (const auto& target : targets) {
        void* address = host->resolve_symbol(host->context, target.symbol);
        if (!address) {
            d.say(std::string("DRIVER: missing symbol ") + target.symbol);
            return false;
        }
        if (host->create_hook(host->context, address, target.detour, target.original) != 0) {
            d.say(std::string("DRIVER: cannot hook ") + target.symbol);
            return false;
        }
    }
    /* change_scene is loader-owned since the Board handle registry started
       hanging off it: the loader's detour invalidates handles and raises
       TC_EVENT_SCENE_CHANGE, so create_hook refuses the target and the driver
       must not treat that as a failure.  It only loses the "the game called
       change_scene" diagnostics; the context it needs comes from
       build_board_ui(), which is handed the same object the board's own
       toolbar passes to change_scene (see changeScene()). */
    {
        auto* address = reinterpret_cast<unsigned char*>(
            host->resolve_symbol(host->context, "change_scene__presenterZcontext_u2958"));
        if (!address) {
            d.say("DRIVER: missing symbol change_scene__presenterZcontext_u2958");
            return false;
        }
        d.changeSceneOriginal = reinterpret_cast<decltype(d.changeSceneOriginal)>(address);
        if (host->create_hook(host->context, address, reinterpret_cast<void*>(hookChangeScene),
                              reinterpret_cast<void**>(&d.changeSceneOriginal)) != 0) {
            /* The loader refused (it owns the point), or MinHook did: either way
               the original address is what the driver calls.  The loader's own
               detour is in front of it, so handle invalidation and the event
               still happen. */
            d.changeSceneOriginal = reinterpret_cast<decltype(d.changeSceneOriginal)>(address);
            d.say("DRIVER: the loader owns scene.change; the SCENE_CHANGE event supplies the "
                  "context to leave with");
        }
    }
    /* The context for the driver's own scene change comes from the event bus. */
    if (tc::events::subscribe(host, TC_EVENT_SCENE_CHANGE, &onSceneChangeEvent, nullptr) !=
        TC_EVENT_OK)
        d.say("DRIVER: no scene-change events; the scene step will fall back to a guessed context");
    d.installed = true;
    return true;
}

inline void tick(void*, const TCFrame* frame) {
    auto& d = driver();
    d.tick(frame);
    if (frame) d.expireTag(frame->frame_number);
}

inline int frameCount() {
    auto& d = driver();
    if (!d.host || !d.host->engine_proc) return 0;
    auto get = reinterpret_cast<int (*)()>(
        d.host->engine_proc(d.host->context, "igGetFrameCount"));
    return get ? get() : 0;
}

}  // namespace tc_board_driver
#endif  // TC_BOARD_DRIVER
