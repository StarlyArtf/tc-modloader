/* Driver for the pin-order Mod's drag, for the real machine.

   The Mod's handles are only worth anything if a player's drag really moves a
   pin, and only the game can answer that: this Mod plays the drag with the same
   posted mouse messages tests/punch-tape-runtime-driver.cpp uses (move, press,
   move while held, release), aimed at the entry rectangles the Mod itself
   logged.  Afterwards it asks the tc.pin_order service what the panel is
   showing and reports it, so a playtest can compare the order it asked for with
   the order the panel has.

   Aiming needs the entry rectangles, so the run has to set
   TC_MODLOADER_PIN_ORDER_LOG=1; without them the driver says so and stops
   instead of clicking blind. */
#include "tc_mod_api.h"
#include "../sdk/tc_pin_order.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static const TCHost* host;
static std::string gameDirectory;
static std::string logPath;
static TCPinOrderApiV1 pins{};
static bool pinsReady;

static std::string readText(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static void say(const std::string& message) {
    if (host && host->log) host->log(host->context, ("pin order driver: " + message).c_str());
}

static HWND gameWindow() {
    struct Finder {
        DWORD process;
        HWND found;
    } finder{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            auto& finder = *reinterpret_cast<Finder*>(data);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != finder.process || !IsWindowVisible(window)) return TRUE;
            RECT client{};
            if (!GetClientRect(window, &client) || client.right < 320 || client.bottom < 240)
                return TRUE;
            finder.found = window;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&finder));
    return finder.found;
}

/* The game's ImGui surface can be larger than its window; the loader logs both
   and the ratio between them, which is what a posted click needs. */
static float displayRatio() {
    const std::string text = readText(logPath);
    const size_t at = text.rfind("Display: dpi-awareness=");
    if (at == std::string::npos) return 1.f;
    const size_t ratio = text.find("ratio=", at);
    if (ratio == std::string::npos) return 1.f;
    float x = 1.f, y = 1.f;
    if (std::sscanf(text.c_str() + ratio, "ratio=%f,%f", &x, &y) != 2) return 1.f;
    return x > 0.f ? x : 1.f;
}

/* Where the Mod said the entry's label is, on screen. */
struct Grip {
    float x = 0.f, y = 0.f;
};

/* The entries the Mod logged for one group, in the order the panel shows them:
   "pin order: inputs #0 key=2 width=1 name="Carry in" labelY=67 ... screen=(26,331)". */
static std::vector<Grip> readRows(const std::string& group) {
    std::vector<Grip> rows;
    const std::string text = readText(logPath);
    const std::string needle = "pin order: " + group + " #";
    size_t at = text.find(needle);
    while (at != std::string::npos) {
        const size_t screen = text.find("screen=(", at);
        if (screen == std::string::npos || screen - at > 256) break;
        Grip row;
        if (std::sscanf(text.c_str() + screen, "screen=(%f,%f)", &row.x, &row.y) == 2)
            rows.push_back(row);
        at = text.find(needle, at + needle.size());
    }
    return rows;
}

static void postMove(HWND window, int clientX, int clientY, bool held) {
    PostMessageW(window, WM_MOUSEMOVE, held ? MK_LBUTTON : 0, MAKELPARAM(clientX, clientY));
}

/* One click on a row's down button, in the game's own client coordinates.  The
   buttons sit at the entry's left edge, straddling its label line: the up one
   above it, the down one below (see examples/pin-order/plugin.cpp, kButton*). */
static bool clickDown(HWND window, const std::vector<Grip>& rows, size_t row) {
    if (row >= rows.size()) return false;
    const float ratio = displayRatio();
    const POINT point{static_cast<LONG>((rows[row].x + 10.f) * ratio),
                      static_cast<LONG>((rows[row].y + 10.f) * ratio)};
    POINT screen = point;
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    Sleep(60);
    postMove(window, point.x, point.y, false);
    Sleep(80);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    Sleep(90);
    PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    return true;
}

static std::string orderOf(uint32_t group) {
    uint64_t keys[64]{};
    uint32_t count = 0;
    const int status = tc::pin_order::order(pins, group, keys, 64, &count);
    std::string text;
    for (uint32_t index = 0; index < count && index < 64; ++index) {
        if (!text.empty()) text += ",";
        text += std::to_string(keys[index]);
    }
    return text + (status == TC_PIN_ORDER_OK ? "" : " (status " + std::to_string(status) + ")");
}

static int frames;
static int stage;
static int waitUntil;
static std::string orderBefore;

static void drive() {
    ++frames;
    if (!pinsReady || stage >= 3) return;
    const uint32_t inputs = tc::pin_order::count(pins, TC_PIN_ORDER_GROUP_INPUTS);
    if (inputs < 2) return;
    const std::vector<Grip> rows = readRows("inputs");
    if (rows.size() < inputs) return;
    /* The panel has drawn every input at least twice by now: the rectangles the
       driver reads are the ones the handles are really at. */
    if (stage == 0) {
        if (frames < 240) return;
        stage = 1;
        return;
    }
    if (stage == 1) {
        orderBefore = orderOf(TC_PIN_ORDER_GROUP_INPUTS);
        HWND window = gameWindow();
        if (!window) {
            say("no game window; the click is skipped");
            stage = 3;
            return;
        }
        /* One press of the first entry's down button: it should trade places
           with the entry under it. */
        const bool posted = clickDown(window, rows, 0);
        say("clicked the down button of inputs #0" + std::string(posted ? "" : " (not posted)") +
            "; before=" + orderBefore);
        /* The move lands on the frame after the click, and the panel is drawn
           with the new order after that. */
        waitUntil = frames + 30;
        stage = 2;
        return;
    }
    if (stage == 2 && frames >= waitUntil) {
        say("order inputs before=[" + orderBefore + "] after=[" +
            orderOf(TC_PIN_ORDER_GROUP_INPUTS) + "]");
        stage = 3;
    }
}

static void frame(void*, const TCFrame*) { drive(); }

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    std::wstring directory(path);
    const size_t slash = directory.find_last_of(L'\\');
    directory = slash == std::wstring::npos ? directory : directory.substr(0, slash + 1);
    gameDirectory.assign(directory.begin(), directory.end());
    logPath = gameDirectory + "tc-modloader-data\\loader.log";
    pinsReady = tc::pin_order::table(h, &pins) && tc::pin_order::ready(pins);
    if (!pinsReady) {
        say("tc.pin_order unavailable");
        return 0;
    }
    if (tc::pin_order::count(pins, TC_PIN_ORDER_GROUP_INPUTS) == 0)
        say("waiting for an IO panel");
    out->on_frame = &frame;
    return 0;
}
