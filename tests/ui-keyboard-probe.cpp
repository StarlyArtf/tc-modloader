/* Development-only keyboard / IME probe.

   It answers, with log lines instead of assumptions:
     1. does a plugin widget receive key presses through the game's own input
        path (not through a plugin-side workaround)?
     2. do typed characters - including a non-ASCII one - reach a plugin
        ImGui InputText?
     3. does Tab move the focus between plugin widgets, and what happens to
        Escape while a page is open?

   The page is opened by tests/ui-keyboard-driver.hpp, which types into it with
   real window messages (WM_KEYDOWN/UP, WM_CHAR, WM_IME_CHAR).  Chinese IME
   composition (candidate window, preedit) cannot be driven reliably from a
   script, so that part stays a manual checklist in docs/sdk/ui.md; what this
   probe proves automatically is the character and key delivery underneath it.

   Nothing here is a public API: it is a dev package built by build.ps1. */
#include "../sdk/tc_ui.h"
#include <cstring>
#include <string>
#include <windows.h>
#include <imm.h>
#ifdef TC_KEY_DRIVER
#include "ui-keyboard-driver.hpp"
#endif

static const TCHost* host;
static char text[64];
static char panelText[64];
static bool flag;
static std::string lastText = "(unset)";
static std::string lastPanelText = "(unset)";
static bool loggedFirst;
static int panelFrames;
static bool inputFocused;
static bool focusState[3] = {false, false, false};

/* ImGuiKey values of this build (ImGuiKey_ enum, 1.92). */
enum {
    kKeyTab = 512,
    kKeyLeft = 513,
    kKeyRight = 514,
    kKeyUp = 515,
    kKeyDown = 516,
    kKeySpace = 524,
    kKeyEnter = 525,
    kKeyEscape = 526,
    kKeyA = 546,
    kKeyF5 = 576
};

struct Watched { int code; const char* name; bool down; };
static Watched watched[] = {
    {kKeyTab, "Tab", false},     {kKeyEnter, "Enter", false},
    {kKeyEscape, "Escape", false}, {kKeySpace, "Space", false},
    {kKeyA, "A", false},         {kKeyUp, "Up", false},
    {kKeyDown, "Down", false},   {kKeyF5, "F5", false},
};

struct Keys {
    bool (*pressed)(int, bool) = nullptr;
    bool (*down)(int) = nullptr;
    void (*focusHere)(int) = nullptr;
    bool (*itemFocused)() = nullptr;
};
static Keys keys;

static void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

/* Printable form of the buffer, so a log line proves what was received even
   when it is not ASCII. */
static std::string describe(const char* value) {
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        if (*p >= 32 && *p < 127) out.push_back(static_cast<char>(*p));
        else {
            static const char* digits = "0123456789abcdef";
            out += "\\x";
            out.push_back(digits[*p >> 4]);
            out.push_back(digits[*p & 15]);
        }
    }
    return out;
}

static void focusProbe(const char* name, int index) {
    if (!keys.itemFocused) return;
    const bool focused = keys.itemFocused();
    if (focused == focusState[index]) return;
    focusState[index] = focused;
    report(std::string("Keyboard probe: focus ") + name + (focused ? "=1" : "=0"));
}

/* Where the input method puts its composition and candidate windows, recorded
   while a probe text field has focus.  This is the part of the Chinese input
   test that cannot be scripted: the probe cannot type pinyin, but it can report
   what the system was asked to do, so "the candidate window is somewhere else"
   becomes a number instead of a judgement call.  Coordinates are client
   coordinates of the game window; the loader logs the same frame's content
   region in ImGui coordinates, and the loader's own log line carries both
   sizes, so the two can be compared afterwards. */
struct ImeWatch { HWND window = nullptr; POINT composition{-99999, -99999};
    POINT candidate{-99999, -99999}; DWORD style = 0xffffffff; bool logged = false; };
static ImeWatch ime;

static HWND gameWindow() {
    if (ime.window && IsWindow(ime.window)) return ime.window;
    struct Finder { DWORD process; HWND found; } finder{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& f = *reinterpret_cast<Finder*>(data);
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner != f.process || !IsWindowVisible(window)) return TRUE;
        RECT rect{};
        if (!GetClientRect(window, &rect) || rect.right < 320 || rect.bottom < 240) return TRUE;
        f.found = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&finder));
    ime.window = finder.found;
    return ime.window;
}

static void watchIme(const char* where, bool fieldActive) {
    if (!fieldActive) return;
    HWND window = gameWindow();
    if (!window) return;
    HIMC context = ImmGetContext(window);
    if (!context) return;
    POINT composition = ime.composition, candidate = ime.candidate;
    DWORD style = 0;
    COMPOSITIONFORM form{};
    if (ImmGetCompositionWindow(context, &form)) {
        style = form.dwStyle;
        composition = form.ptCurrentPos;
    }
    CANDIDATEFORM candidates{};
    if (ImmGetCandidateWindow(context, 0, &candidates)) candidate = candidates.ptCurrentPos;
    ImmReleaseContext(window, context);
    if (!ime.logged || composition.x != ime.composition.x || composition.y != ime.composition.y ||
        candidate.x != ime.candidate.x || candidate.y != ime.candidate.y || style != ime.style) {
        ime.logged = true;
        ime.composition = composition;
        ime.candidate = candidate;
        ime.style = style;
        report(std::string("Keyboard probe: IME ") + where + " composition=" +
               std::to_string(composition.x) + "," + std::to_string(composition.y) +
               " candidate=" + std::to_string(candidate.x) + "," +
               std::to_string(candidate.y) + " style=0x" + std::to_string(style));
    }
}

static void draw(void*, const TCFrame* frame, float width, float height) {
    (void)width;
    (void)height;
    const int number = frame ? frame->frame_number : -1;
    if (!loggedFirst) {
        loggedFirst = true;
        report("Keyboard probe: page first drawn on frame " + std::to_string(number));
#ifdef TC_KEY_DRIVER
        tc_key_driver::driver().pageSeen(number);
#endif
    }
    for (size_t index = 0; index < sizeof(watched) / sizeof(watched[0]); ++index) {
        Watched& watch = watched[index];
        if (keys.pressed && keys.pressed(watch.code, false))
            report(std::string("Keyboard probe: key ") + watch.name + " pressed");
        const bool down = keys.down && keys.down(watch.code);
        if (down != watch.down) {
            watch.down = down;
            report(std::string("Keyboard probe: key ") + watch.name + (down ? " down" : " up"));
        }
    }
    tc::ui::text("frame " + std::to_string(tc::ui::frameCount()));
    tc::ui::separator();
    tc::ui::text("text field (focus is taken automatically):");
    /* Take the focus for the first frames so the probe can type into it
       without depending on a click position. */
    if (keys.focusHere && !inputFocused) keys.focusHere(0);
    tc::ui::inputText("###keyboard", text, sizeof(text));
    focusProbe("text", 0);
    watchIme("page", tc::ui::isItemFocused() || focusState[0]);
    if (focusState[0]) inputFocused = true;
    if (tc::ui::button("Second")) report("Keyboard probe: Second clicked");
    focusProbe("second", 1);
    tc::ui::checkbox("flag", &flag);
    focusProbe("flag", 2);
    tc::ui::separator();
    tc::ui::text(std::string("buffer=") + describe(text) + "  length=" +
                 std::to_string(std::strlen(text)));
    const std::string current(text);
    if (current != lastText) {
        lastText = current;
        report("Keyboard probe: buffer \"" + describe(text) + "\" length=" +
               std::to_string(std::strlen(text)));
    }
}

/* The same probe inside a circuit-board side panel.  This is the one that
   cannot be automated end to end: a live board also has its own keyboard
   handling (letters pick components, Space runs the simulation), and whether a
   key typed into this field *also* reaches the board is something a person has
   to look at.  The log records what the panel saw; the screen shows what the
   board did. */
static void drawPanel(void*, const TCFrame* frame, float width, float height) {
    (void)width;
    (void)height;
    ++panelFrames;
    if (panelFrames == 1) {
        report("Keyboard probe: board panel first drawn on frame " +
               std::to_string(frame ? frame->frame_number : -1));
    }
    tc::ui::text("board panel typing probe:");
    tc::ui::inputText("###boardtext", panelText, sizeof(panelText));
    watchIme("board panel", tc::ui::isItemFocused());
    for (size_t index = 0; index < sizeof(watched) / sizeof(watched[0]); ++index) {
        Watched& watch = watched[index];
        if (keys.pressed && keys.pressed(watch.code, false))
            report(std::string("Keyboard probe: board panel saw key ") + watch.name);
    }
    tc::ui::text(std::string("buffer=") + describe(panelText));
    tc::ui::text(std::string("panel frame ") + std::to_string(panelFrames));
    const std::string current(panelText);
    if (current != lastPanelText) {
        lastPanelText = current;
        report("Keyboard probe: board panel buffer \"" + describe(panelText) + "\" length=" +
               std::to_string(std::strlen(panelText)));
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    if (!tc::ui::load(h)) {
        report("Keyboard probe: tc::ui::load failed: " + tc::ui::missing());
        return 3;
    }
    keys.pressed = reinterpret_cast<decltype(keys.pressed)>(
        h->engine_proc(h->context, "igIsKeyPressed_Bool"));
    keys.down = reinterpret_cast<decltype(keys.down)>(
        h->engine_proc(h->context, "igIsKeyDown_Nil"));
    keys.focusHere = reinterpret_cast<decltype(keys.focusHere)>(
        h->engine_proc(h->context, "igSetKeyboardFocusHere"));
    keys.itemFocused = reinterpret_cast<decltype(keys.itemFocused)>(
        h->engine_proc(h->context, "igIsItemFocused"));
    if (!keys.pressed || !keys.down || !keys.focusHere || !keys.itemFocused) {
        report("Keyboard probe: a keyboard entry point is missing");
        return 4;
    }
    if (tc::ui::registerPage("keys", "Keyboard probe", draw, nullptr, h) != 0) {
        report("Keyboard probe: registerPage failed");
        return 5;
    }
    report("Keyboard probe: registered page 'keys'");
    /* Optional: the loader may predate the board panel API, in which case the
       page above is still all there is. */
    const int panel = tc::ui::registerBoardPanel("keys", "Board keys probe", drawPanel, nullptr, h);
    if (panel == 0) report("Keyboard probe: registered board panel 'keys'");
    else if (panel != -1) report("Keyboard probe: registerBoardPanel returned " + std::to_string(panel));
#ifdef TC_KEY_DRIVER
    if (!tc_key_driver::start(h)) return 9;
    out->on_frame = tc_key_driver::tick;
#endif
    return 0;
}
