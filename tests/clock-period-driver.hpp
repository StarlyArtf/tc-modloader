/* In-process driver for the clock's value window (examples/clock).

   Compiled into the clock plugin itself (build.ps1 -DTC_CLOCK_DRIVER), so it
   can read the two things the interaction is made of - the rectangle the
   drawing code registered for the click and the period the callback mirrors -
   instead of guessing at either one from outside the process.

   What it does, with real window messages to the game's own window:

     1. waits for the board to paint a clock and registers its corner box,
     2. clicks the box and checks the value window opened for that component,
     3. types "8" and Enter,
     4. checks the period the callback now holds *and* the bytes the instance's
        configuration carries (the durable copy the schematic stores).

   Everything is logged with a CLOCK-DRIVER: prefix; the playtest asserts on the
   PASS line.  Failures say what was true instead, so a red run is diagnosable
   from the log alone. */

#pragma once
#ifdef TC_CLOCK_DRIVER

#include <windows.h>

#include <string>

namespace tc_clock_driver {

struct Driver {
    const TCHost* host = nullptr;
    HWND window = nullptr;
    bool installed = false;
    bool done = false;
    int frames = 0;
    int stage = 0;
    int stage_frame = 0;
    uint64_t target = 0;      /* the board component whose box was clicked */
    uint64_t binding = 0;     /* the instance the configuration write lands on */
    uint8_t before = 0;

    void say(const std::string& text) {
        if (host && host->log) host->log(host->context, text.c_str());
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

    void findWindow() {
        HWND found = nullptr;
        EnumWindows(windowCallback, reinterpret_cast<LPARAM>(&found));
        window = found;
        if (!window) return;
        /* The messages below are posted, but the game's own key path (GLFW in
           front of ImGui) only forwards a character while the window has the
           keyboard: without this the field keeps its old text and the run fails
           with "the callback holds period 1 after typing 8".  The nudge is what
           tests/ui-keyboard-driver.hpp does for the same reason. */
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetForegroundWindow(window);
        Sleep(80);
    }

    /* A real press and release at a client coordinate: the cursor is moved too,
       because the game's own ImGui reads the pointer through GLFW. */
    void clickClient(int x, int y) {
        /* The game can replace its window while a level loads (a fullscreen or
           DPI switch recreates it), and a stale handle makes ClientToScreen
           fail - in which case nothing is posted and the click silently never
           happens.  Re-resolve it here, right before the press. */
        if (!window || !IsWindow(window) || !IsWindowVisible(window)) {
            findWindow();
            if (!window) {
                say("CLOCK-DRIVER: no window to click into");
                return;
            }
        }
        POINT screen{x, y};
        if (!ClientToScreen(window, &screen)) {
            say("CLOCK-DRIVER: ClientToScreen failed; the click was not posted");
            return;
        }
        char detail[160] = {};
        snprintf(detail, sizeof(detail),
                 "CLOCK-DRIVER: posting a click at client (%d,%d) -> screen (%ld,%ld)",
                 x, y, static_cast<long>(screen.x), static_cast<long>(screen.y));
        say(detail);
        SetCursorPos(screen.x, screen.y);
        Sleep(30);
        const LPARAM point = MAKELPARAM(x, y);
        PostMessageW(window, WM_MOUSEMOVE, 0, point);
        Sleep(40);
        PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
        Sleep(60);
        PostMessageW(window, WM_LBUTTONUP, 0, point);
    }

    /* GLFW derives the key from the scancode, so the lParam carries a real one
       (the same finding as tests/ui-keyboard-driver.hpp). */
    void keyMessage(int virtualKey, bool down) {
        const UINT scan = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
        LPARAM data = 1 | (static_cast<LPARAM>(scan & 0xff) << 16);
        if (!down) data |= (1LL << 30) | (1LL << 31);
        PostMessageW(window, down ? WM_KEYDOWN : WM_KEYUP, virtualKey, data);
        Sleep(30);
    }

    /* The character itself.  Measured on this board: the key messages reach
       ImGui's key state (Backspace and Enter both landed - the field emptied and
       the value was committed), but the character never arrived, so the driver
       posts the WM_CHAR Windows would have produced for a real key press.  That
       is the same finding as tests/ui-keyboard-playtest.ps1, where a bare
       WM_CHAR delivers exactly one character and posting both doubles it. */
    void charMessage(char value) {
        PostMessageW(window, WM_CHAR, static_cast<WPARAM>(value), 1);
        Sleep(30);
    }

    void pressEnter() {
        keyMessage(VK_RETURN, true);
        keyMessage(VK_RETURN, false);
    }

    void fail(const std::string& why) {
        done = true;
        say("CLOCK-DRIVER: FAIL " + why);
    }

    void pass(const std::string& what) {
        done = true;
        say("CLOCK-DRIVER: PASS " + what);
    }

    /* The period the instance's own configuration carries - the copy a save
       keeps - read through the same services the click path writes with. */
    bool storedPeriod(uint64_t instance, uint8_t* out) {
        if (!g_instancesApi.enumerate || !g_storageApi.read_config) return false;
        std::vector<TCComponentInstanceHandle> handles(16);
        uint32_t written = 0, total = 0;
        int status = tc::component_instances::enumerate(
            g_instancesApi, kClockId, handles.data(),
            static_cast<uint32_t>(handles.size()), &written, &total);
        if (status == TC_COMPONENT_INSTANCES_ERR_RANGE && total > handles.size()) {
            handles.resize(total);
            status = tc::component_instances::enumerate(
                g_instancesApi, kClockId, handles.data(),
                static_cast<uint32_t>(handles.size()), &written, &total);
        }
        if (status != TC_COMPONENT_INSTANCES_OK) return false;
        for (uint32_t i = 0; i < written; ++i) {
            if (handles[i].instance_id != instance) continue;
            ClockConfig config{};
            uint32_t bytes = 0;
            if (tc::component_storage::readConfig(g_storageApi, handles[i], &config,
                                                  sizeof(config), &bytes) !=
                    TC_COMPONENT_STORAGE_OK ||
                bytes < sizeof(config))
                return false;
            if (out) *out = config.period;
            return true;
        }
        return false;
    }

    void tick(const TCFrame* frame) {
        if (done) return;
        ++frames;
        const int now = frame ? frame->frame_number : frames;
        if (!window) {
            findWindow();
            if (!window) {
                if (frames > 400) fail("no game window to drive");
                return;
            }
            say("CLOCK-DRIVER: armed, window found on frame " + std::to_string(now));
            stage_frame = now;
            return;
        }
        if (now < stage_frame + 5) return;

        if (stage == 0) {
            ClockBadge badge{};
            uint64_t box = 0;
            {
                std::lock_guard<std::mutex> lock(g_badgeMutex);
                for (const auto& entry : g_badges) {
                    if (entry.second.max_x <= entry.second.min_x) continue;
                    badge = entry.second;
                    box = entry.first;
                    break;
                }
            }
            if (!box) {
                if (frames > 1200) fail("no clock painted a corner box to click");
                return;
            }
            target = box;
            before = badge.period;
            const int x = static_cast<int>((badge.min_x + badge.max_x) * 0.5f);
            const int y = static_cast<int>((badge.min_y + badge.max_y) * 0.5f);
            char line[224] = {};
            snprintf(line, sizeof(line),
                     "CLOCK-DRIVER: clicking the box of 0x%llx at (%d,%d), period %u",
                     static_cast<unsigned long long>(box), x, y,
                     static_cast<unsigned>(before));
            say(line);
            clickClient(x, y);
            stage = 1;
            stage_frame = now;
            return;
        }

        if (stage == 1) {
            if (!g_editingClock) {
                if (frames > 1600) fail("clicking the box did not open a value window");
                return;
            }
            if (g_editingClock != target) {
                fail("the value window opened for 0x" + std::to_string(g_editingClock) +
                     " instead of the clicked 0x" + std::to_string(target));
                return;
            }
            say("CLOCK-DRIVER: value window open for the clicked clock; clearing the field");
            /* Clear whatever the box was set to, so the typed digit is the whole
               value whether or not the field selected its text on focus. */
            for (int i = 0; i < 4; ++i) keyMessage(VK_BACK, true), keyMessage(VK_BACK, false);
            stage = 2;
            stage_frame = now;
            return;
        }

        /* One message per stage, and a few frames between them: a character and
           the Enter that commits it arriving inside the same ImGui frame is what
           committed an empty field in the first version of this driver. */
        if (stage == 2) {
            say("CLOCK-DRIVER: typing 8");
            charMessage('8');
            stage = 3;
            stage_frame = now;
            return;
        }

        if (stage == 3) {
            say("CLOCK-DRIVER: pressing Enter");
            pressEnter();
            stage = 4;
            stage_frame = now;
            return;
        }

        if (stage == 4) {
            uint8_t mirrored = 0;
            binding = 0;
            {
                std::lock_guard<std::mutex> lock(g_clockMutex);
                for (const auto& entry : g_clocks)
                    if (topLevelOf(entry.first) == target) {
                        mirrored = entry.second.period.load();
                        binding = entry.first;
                    }
            }
            if (!binding) {
                if (frames > 2000) fail("the clicked clock never reached the callback");
                return;
            }
            if (mirrored != 8) {
                fail("the callback holds period " + std::to_string(mirrored) +
                     " after typing 8");
                return;
            }
            uint8_t stored = 0;
            if (!storedPeriod(binding, &stored)) {
                fail("the instance's configuration could not be read back");
                return;
            }
            if (stored != 8) {
                fail("the instance's configuration still holds " + std::to_string(stored));
                return;
            }
            char line[256] = {};
            snprintf(line, sizeof(line),
                     "CLOCK-DRIVER: PASS the box opened the window, \"8\" + Enter set the "
                     "period from %u to %u, and the instance configuration carries %u",
                     static_cast<unsigned>(before), static_cast<unsigned>(mirrored),
                     static_cast<unsigned>(stored));
            pass(line);
        }
    }
};

inline Driver& driver() {
    static Driver value;
    return value;
}

inline void start(const TCHost* host) {
    auto& d = driver();
    if (d.installed) return;
    d.installed = true;
    d.host = host;
}

}  // namespace tc_clock_driver

#endif  // TC_CLOCK_DRIVER
