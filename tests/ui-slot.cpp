/* Offline tests for tc::ui::registerBoardPanel(): the version check that keeps
   an older loader safe, the argument validation, and the definition that is
   handed to the host.  No game, no ImGui - the host here is a stub. */
#include "../sdk/tc_ui.h"
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>

static TCUiSlotDefinition captured;
static int calls;
static int next_result;

static int fakeRegister(void*, const TCUiSlotDefinition* definition) {
    captured = *definition;
    ++calls;
    return next_result;
}

static void drawStub(void*, const TCFrame*, float, float) {}

static TCHost hostWithSlotRegistry() {
    TCHost host{};
    host.size = sizeof(host);
    host.api_version = TC_MOD_API_VERSION;
    host.register_ui_slot = fakeRegister;
    return host;
}

int main() {
    calls = 0;
    /* No host, or an incomplete definition: nothing reaches the host. */
    assert(tc::ui::registerBoardPanel("main", "Panel", drawStub, nullptr, nullptr) == -2);
    TCHost host = hostWithSlotRegistry();
    assert(tc::ui::registerBoardPanel(nullptr, "Panel", drawStub, nullptr, &host) == -2);
    assert(tc::ui::registerBoardPanel("", "Panel", drawStub, nullptr, &host) == -2);
    assert(tc::ui::registerBoardPanel("main", "Panel", nullptr, nullptr, &host) == -2);
    assert(calls == 0);
    /* A host that predates the slot API is refused by size, not by reading
       past the struct it was given - and a host of the right size with the
       entry point left null is refused too. */
    TCHost old{};
    old.size = static_cast<uint32_t>(offsetof(TCHost, register_ui_slot));
    old.api_version = TC_MOD_API_VERSION;
    assert(tc::ui::registerBoardPanel("main", "Panel", drawStub, nullptr, &old) == -1);
    TCHost nulled = hostWithSlotRegistry();
    nulled.register_ui_slot = nullptr;
    assert(tc::ui::registerBoardPanel("main", "Panel", drawStub, nullptr, &nulled) == -1);
    assert(calls == 0);
    /* Bad ids are rejected before the host sees them. */
    assert(tc::ui::registerBoardPanel("bad###id", "Panel", drawStub, nullptr, &host) == -2);
    const std::string tooLong(64, 'x');
    assert(tc::ui::registerBoardPanel(tooLong.c_str(), "Panel", drawStub, nullptr, &host) == -2);
    assert(calls == 0);
    /* A good call reaches the host with the size field, the board-side kind,
       the caller's pointers and its preferred size. */
    next_result = -4;
    assert(tc::ui::registerBoardPanel("main", "Board panel", drawStub, &host, &host, 360.f,
                                      300.f) == -4);
    assert(calls == 1);
    assert(captured.size == sizeof(TCUiSlotDefinition));
    assert(captured.kind == TC_UI_SLOT_BOARD_SIDE);
    assert(std::strcmp(captured.slot_id, "main") == 0);
    assert(std::strcmp(captured.title, "Board panel") == 0);
    assert(captured.draw == drawStub);
    assert(captured.user == &host);
    assert(captured.preferred_width == 360.f && captured.preferred_height == 300.f);
    next_result = 0;
    assert(tc::ui::registerBoardPanel("main", nullptr, drawStub, nullptr, &host) == 0);
    assert(captured.title == nullptr);
    assert(captured.preferred_width == 0.f && captured.preferred_height == 0.f);
    std::cout << "PASS UI slot registration: host version gate, validation, definition "
                 "contents\n";
}
