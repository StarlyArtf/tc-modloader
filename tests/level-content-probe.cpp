/* Test-only probe for docs/research/level-interface.md: which part of a level
   comes from disk?

   It does two things and writes both answers where a playtest can read them:

     * subscribes to TC_EVENT_LEVEL_LOAD and logs the level's own name (the
       loader passes the real name through since 2026-09-20);
     * photographs the frame a couple of seconds after that load, so the level's
       *title* and *dialogue* - the two things the game takes from
       campaign/<name>/meta.txt - can be read off the picture.

   The case (tests/level-content-playtest.ps1) runs it against a sandbox whose
   campaign/sandbox folder has been rewritten, so the picture either shows the
   modded title or it does not. */

#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_event_api.h"
#include "../sdk/tc_event.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

namespace {

const TCHost* host = nullptr;
bool loaded = false;
int frames_after_load = 0;
int captures = 0;
std::string level_name;

void say(const std::string& text) {
    if (host && host->log) host->log(host->context, text.c_str());
}

/* The capture: glReadPixels into a bottom-up 32-bit BMP, the same technique
   tests/icon-probe.cpp and tests/float-reentry-driver.cpp use. */
using GetIntegervFn = void (*)(unsigned, int*);
using ReadPixelsFn = void (*)(int, int, int, int, unsigned, unsigned, void*);
using PixelStoreFn = void (*)(unsigned, int);
GetIntegervFn getIntegerv = nullptr;
ReadPixelsFn readPixels = nullptr;
PixelStoreFn pixelStore = nullptr;

void loadGl() {
    HMODULE module = LoadLibraryA("opengl32.dll");
    if (!module) return;
    void* address = reinterpret_cast<void*>(GetProcAddress(module, "glGetIntegerv"));
    std::memcpy(&getIntegerv, &address, sizeof(getIntegerv));
    address = reinterpret_cast<void*>(GetProcAddress(module, "glReadPixels"));
    std::memcpy(&readPixels, &address, sizeof(readPixels));
    address = reinterpret_cast<void*>(GetProcAddress(module, "glPixelStorei"));
    std::memcpy(&pixelStore, &address, sizeof(pixelStore));
}

void writeLe32(unsigned char* out, unsigned value) {
    out[0] = static_cast<unsigned char>(value & 0xff);
    out[1] = static_cast<unsigned char>((value >> 8) & 0xff);
    out[2] = static_cast<unsigned char>((value >> 16) & 0xff);
    out[3] = static_cast<unsigned char>((value >> 24) & 0xff);
}

bool capture(const char* path) {
    if (!getIntegerv || !readPixels || !pixelStore || !path) return false;
    int viewport[4] = {};
    getIntegerv(0x0ba2 /* GL_VIEWPORT */, viewport);
    const int width = viewport[2], height = viewport[3];
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4u);
    pixelStore(0x0d05 /* GL_PACK_ALIGNMENT */, 1);
    readPixels(viewport[0], viewport[1], width, height, 0x1908 /* GL_RGBA */,
               0x1401 /* GL_UNSIGNED_BYTE */, pixels.data());
    std::FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const unsigned pixelBytes = static_cast<unsigned>(width) * height * 4u;
    unsigned char header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    writeLe32(header + 2, 54u + pixelBytes);
    writeLe32(header + 10, 54u);
    writeLe32(header + 14, 40u);
    writeLe32(header + 18, static_cast<unsigned>(width));
    writeLe32(header + 22, static_cast<unsigned>(-height));
    header[26] = 1; header[28] = 32;
    writeLe32(header + 34, pixelBytes);
    std::fwrite(header, 1, sizeof(header), file);
    std::vector<unsigned char> row(static_cast<size_t>(width) * 4u);
    for (int y = 0; y < height; ++y) {
        const unsigned char* source =
            pixels.data() + static_cast<size_t>(height - 1 - y) * width * 4u;
        for (int x = 0; x < width; ++x) {
            row[static_cast<size_t>(x) * 4 + 0] = source[static_cast<size_t>(x) * 4 + 2];
            row[static_cast<size_t>(x) * 4 + 1] = source[static_cast<size_t>(x) * 4 + 1];
            row[static_cast<size_t>(x) * 4 + 2] = source[static_cast<size_t>(x) * 4 + 0];
            row[static_cast<size_t>(x) * 4 + 3] = 255;
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    return true;
}

void onEvent(TCEvent* event) {
    if (!tc::events::is(event, TC_EVENT_LEVEL_LOAD)) return;
    if (const char* name = tc::events::levelName(event))
        if (*name) level_name = name;
    loaded = true;
    frames_after_load = 0;
    say("PROBE: level.load name=\"" + level_name + "\"");
}

BOOL CALLBACK findWindowCallback(HWND candidate, LPARAM data) {
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
    EnumWindows(findWindowCallback, reinterpret_cast<LPARAM>(&found));
    return found;
}

void captureLevel(const char* label) {
    if (!host || !host->data_directory_utf8) return;
    char path[512] = {};
    std::snprintf(path, sizeof(path), "%s\\%s.bmp", host->data_directory_utf8, label);
    say(std::string("PROBE: ") + (capture(path) ? "captured the level" :
                                  "could not capture the level") +
        " (" + level_name + ", " + label + ") -> " + path);
    ++captures;
}

void frame(void*, const TCFrame*) {
    if (!loaded) return;
    ++frames_after_load;
    /* Two pictures: the level as it opens - the intro dialogue is part of what
       meta.txt decides - and then the board, after clicking the dialogue's own
       continue button out of the way (a synthetic click, the same way the other
       drivers press things).  The button sits at a fixed place in the layout the
       case's window size produces. */
    if (frames_after_load == 150) {
        captureLevel("level");
        if (HWND window = findWindow()) {
            const LPARAM point = static_cast<LPARAM>((578 << 16) | 706);
            PostMessageW(window, WM_MOUSEMOVE, 0, point);
            Sleep(50);
            PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
            Sleep(60);
            PostMessageW(window, WM_LBUTTONUP, 0, point);
        }
        return;
    }
    if (frames_after_load < 400) return;
    loaded = false;
    captureLevel("level2");
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!h->log) return 2;
    loadGl();
    if (tc::events::subscribe(h, TC_EVENT_LEVEL_LOAD, &onEvent, nullptr) != TC_EVENT_OK) {
        say("PROBE: cannot subscribe to the event bus");
        return 3;
    }
    out->on_frame = &frame;
    say("PROBE: armed");
    return 0;
}
