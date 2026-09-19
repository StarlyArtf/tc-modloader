/* Offline tests for the keyboard helpers in tc_ui.h: strict name resolution
   (a missing export is reported, not silently ignored) and argument
   forwarding of every wrapper.  The behaviour of the game's own key path is
   covered by tests/ui-keyboard-playtest.ps1 on the real engine. */
#include "../sdk/tc_ui.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static std::vector<std::string> calls;
static void dummy() {}

static void* resolveAll(void*, const char* name) {
    calls.push_back(name ? name : "(null)");
    auto fn = &dummy;
    void* pointer = nullptr;
    std::memcpy(&pointer, &fn, sizeof(pointer));
    return pointer;
}

static void* resolveWithout(void*, const char* name) {
    return std::strcmp(name, "igIsKeyPressed_Bool") == 0 ? nullptr : resolveAll(nullptr, name);
}

int main() {
    using namespace tc::ui;
    /* Nothing is bound before load(): every wrapper must be a safe no-op. */
    assert(!keys::down(Key_Tab) && !keys::pressed(Key_Enter) && !keys::released(Key_Escape));
    assert(keys::amount(Key_F5) == 0.f && !isItemFocused());
    setKeyboardFocusHere(0);

    /* A missing export fails load() and is named, like every other binding. */
    TCHost host{};
    host.size = sizeof(host);
    host.context = &host;
    host.engine_proc = resolveWithout;
    assert(!load(&host));
    assert(missing() == "igIsKeyPressed_Bool");

    /* With a complete table the wrappers forward exactly what they are given. */
    host.engine_proc = resolveAll;
    assert(load(&host));
    struct Recorded {
        int key = -1;
        bool repeat = true;
        float delay = 0.f, rate = 0.f;
        int focus = -99;
        int down = 0, pressed = 0, released = 0, focused = 0;
    };
    static Recorded seen;
    table().isKeyDown = [](int key) { seen.key = key; ++seen.down; return key == Key_Tab; };
    table().isKeyPressed = [](int key, bool repeat) {
        seen.key = key; seen.repeat = repeat; ++seen.pressed; return true;
    };
    table().isKeyReleased = [](int key) { seen.key = key; ++seen.released; return true; };
    table().getKeyPressedAmount = [](int key, float delay, float rate) {
        seen.key = key; seen.delay = delay; seen.rate = rate; return 3.f;
    };
    table().setKeyboardFocusHere = [](int offset) { seen.focus = offset; };
    table().isItemFocused = []() { ++seen.focused; return true; };

    assert(keys::down(Key_Tab) && seen.down == 1 && seen.key == Key_Tab);
    assert(keys::down(Key_A) == false && seen.key == Key_A);
    assert(keys::pressed(Key_Enter) && seen.pressed == 1 && seen.key == Key_Enter &&
           seen.repeat == false);
    assert(keys::pressed(Key_Enter, true) && seen.repeat == true);
    assert(keys::released(Key_Escape) && seen.released == 1);
    assert(keys::amount(Key_F5, 0.4f, 0.1f) == 3.f && seen.key == Key_F5 && seen.delay == 0.4f &&
           seen.rate == 0.1f);
    setKeyboardFocusHere(-1);
    assert(seen.focus == -1);
    assert(isItemFocused() && seen.focused == 1);

    /* The enum values the playtest confirmed with real key messages. */
    static_assert(Key_Tab == 512 && Key_Enter == 525 && Key_Escape == 526 &&
                  Key_Space == 524 && Key_A == 546 && Key_F5 == 576 && Key_LeftArrow == 513);
    std::cout << "PASS UI keyboard helpers: strict resolution, no-op before load, argument "
                 "forwarding\n";
}
