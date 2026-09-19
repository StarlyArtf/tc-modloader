/* In-process driver for the keyboard probe (tests/ui-keyboard-probe.cpp).

   It clicks the loader's page entry with real mouse messages - the same
   technique as tests/ui-page-driver.hpp - and then types into the open page
   with real keyboard messages:

     * WM_KEYDOWN/WM_KEYUP with a proper scancode (GLFW keys are scancodes, so
       a made-up lParam would not be recognised),
     * WM_CHAR for ASCII,
     * WM_CHAR and WM_IME_CHAR with a Chinese code unit, to see which of the
       two the game's input path forwards to ImGui.

   Everything it sends is logged with a DRIVER: prefix; what the plugin
   received is logged by the plugin itself, and the playtest asserts on that
   pair.  Chinese IME *composition* (candidate window, preedit) is not
   scriptable and stays a manual check. */
#pragma once
#ifdef TC_KEY_DRIVER
#include <windows.h>
#include <string>
#include <vector>

namespace tc_key_driver {

struct Driver {
    HWND window = nullptr;
    const TCHost* host = nullptr;
    void (*log)(const char*) = nullptr;
    /* "installed" is set by start(), "started" on the first tick: sharing one
       flag makes the first tick skip its own setup. */
    bool installed = false, started = false, done = false;
    int start_frame = 0, stage = 0, entry_clicks = 0;
    float entry_x = 0.f, entry_y = 0.f;
    float scale_x = 1.f, scale_y = 1.f;
    bool entry_known = false;
    int page_frame = -1;

    void say(const std::string& message) { if (log) log(message.c_str()); }

    void readConfiguration() {
        char buffer[192]{};
        const DWORD length = GetEnvironmentVariableA("TC_KEY_ENTRY", buffer, sizeof(buffer));
        if (length == 0 || length >= sizeof(buffer)) return;
        const std::string value(buffer, length);
        /* "<id> <centreX> <centreY> <gameW> <gameH> <windowW> <windowH>" */
        std::vector<std::string> fields;
        for (size_t start = 0; start <= value.size();) {
            const size_t end = value.find(' ', start);
            fields.push_back(value.substr(
                start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (fields.size() < 7) return;
        try {
            entry_x = std::stof(fields[1]);
            entry_y = std::stof(fields[2]);
            const float game_width = std::stof(fields[3]);
            const float game_height = std::stof(fields[4]);
            const float window_width = std::stof(fields[5]);
            const float window_height = std::stof(fields[6]);
            if (game_width > 1.f && window_width > 1.f) scale_x = window_width / game_width;
            if (game_height > 1.f && window_height > 1.f) scale_y = window_height / game_height;
            entry_known = entry_x > 0.f && entry_y > 0.f;
        } catch (...) {
            entry_known = false;
        }
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

    void clickGame(float gameX, float gameY) {
        const int clientX = static_cast<int>(gameX * scale_x);
        const int clientY = static_cast<int>(gameY * scale_y);
        POINT screen{clientX, clientY};
        ClientToScreen(window, &screen);
        SetCursorPos(screen.x, screen.y);
        Sleep(30);
        const LPARAM point = MAKELPARAM(clientX, clientY);
        PostMessageW(window, WM_MOUSEMOVE, 0, point);
        Sleep(40);
        PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
        Sleep(60);
        PostMessageW(window, WM_LBUTTONUP, 0, point);
    }

    /* Scancode in the way a real keyboard sends it: GLFW (and therefore the
       game's ImGui backend) derives the key from the scancode, not from the
       virtual key. */
    void keyMessage(int virtualKey, bool down) {
        const UINT scan = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
        LPARAM data = 1 | (static_cast<LPARAM>(scan & 0xff) << 16);
        if (!down) data |= (1LL << 30) | (1LL << 31);
        PostMessageW(window, down ? WM_KEYDOWN : WM_KEYUP, virtualKey, data);
        Sleep(30);
    }

    void charMessage(unsigned code, bool ime) {
        PostMessageW(window, ime ? WM_IME_CHAR : WM_CHAR, code, 1);
        Sleep(30);
    }

    /* Typing the way a real keyboard does it: only the key messages are posted.
       Windows (and this game's message loop, via TranslateMessage) turns a
       key press into WM_CHAR by itself - posting a WM_CHAR on top of that is
       what produced the "double character" reading in the first version of
       this driver.  `charOnly` below is the other half of the pair: it proves
       the character path on its own, with no key message involved. */
    void typeKey(char value) {
        const int virtualKey = VkKeyScanA(value) & 0xff;
        keyMessage(virtualKey, true);
        keyMessage(virtualKey, false);
    }

    void charOnly(char value) { charMessage(static_cast<unsigned char>(value), false); }

    void pageSeen(int frame_number) {
        if (page_frame < 0) {
            page_frame = frame_number;
            say("DRIVER: page visible on frame " + std::to_string(frame_number));
        }
    }

    void tick(void*, const TCFrame* frame) {
        if (!frame || done) return;
        if (!started) {
            started = true;
            start_frame = frame->frame_number;
            readConfiguration();
            HWND found = nullptr;
            EnumWindows(windowCallback, reinterpret_cast<LPARAM>(&found));
            window = found;
            if (window)
                SetWindowPos(window, nullptr, 0, 0, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (window) SetForegroundWindow(window);
            say("DRIVER: started frame=" + std::to_string(frame->frame_number) + " window=" +
                std::to_string(reinterpret_cast<uintptr_t>(window)) + " entry=" +
                (entry_known ? "known" : "missing"));
        }
        const int elapsed = frame->frame_number - start_frame;
        /* Open the page: the entry rectangle comes from the loader's own log
           (the playtest passes it in), and the click is retried because the
           cursor can be recentred by other programs on this machine. */
        if (stage == 0 && elapsed > 120) stage = 1;
        if (stage == 1) {
            if (page_frame >= 0) {
                stage = 2;
                return;
            }
            if (entry_known && entry_clicks < 10 && elapsed > 120 + entry_clicks * 45) {
                ++entry_clicks;
                say("DRIVER: clicking page entry (attempt " + std::to_string(entry_clicks) + ")");
                clickGame(entry_x, entry_y);
            }
            if (elapsed > 900) {
                done = true;
                say("DRIVER: the page never opened; giving up");
            }
            return;
        }
        if (stage == 2) {
            stage = 3;
            say("DRIVER: typing \"abc\" as key messages (a real keyboard sends no WM_CHAR of its own)");
            typeKey('a');
            typeKey('b');
            typeKey('c');
            return;
        }
        if (stage == 3 && page_frame >= 0 && frame->frame_number > page_frame + 60) {
            stage = 4;
            say("DRIVER: pressing Tab");
            keyMessage(VK_TAB, true);
            keyMessage(VK_TAB, false);
            return;
        }
        if (stage == 4 && frame->frame_number > page_frame + 120) {
            stage = 5;
            say("DRIVER: sending a Chinese code unit (WM_CHAR then WM_IME_CHAR)");
            charMessage(0x4f60, false);   /* 你 */
            charMessage(0x597d, true);    /* 好, the way an IME commits it */
            return;
        }
        if (stage == 5 && frame->frame_number > page_frame + 180) {
            stage = 6;
            say("DRIVER: sending 'z' as WM_CHAR only (no key message)");
            charOnly('z');
            return;
        }
        if (stage == 6 && frame->frame_number > page_frame + 240) {
            stage = 7;
            say("DRIVER: pressing Escape");
            keyMessage(VK_ESCAPE, true);
            keyMessage(VK_ESCAPE, false);
            return;
        }
        if (stage == 7 && frame->frame_number > page_frame + 300) {
            done = true;
            say("DRIVER: done");
        }
    }
};

inline Driver& driver() { static Driver value; return value; }

inline bool start(const TCHost* host) {
    auto& d = driver();
    if (d.installed) return false;
    d.host = host;
    d.log = [](const char* message) {
        tc_key_driver::Driver& self = driver();
        if (self.host && self.host->log && self.host->context)
            self.host->log(self.host->context, message);
    };
    d.installed = true;
    return true;
}

inline void tick(void*, const TCFrame* frame) { driver().tick(nullptr, frame); }

}  // namespace tc_key_driver
#endif  // TC_KEY_DRIVER
