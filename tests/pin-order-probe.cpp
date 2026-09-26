/* Test-only probe for "let the player reorder the IO panel's pins".

   Two things are pinned here, because the Mod that draws the handles depends on
   both:

     * the tc.pin_order service - what the panel is showing right now, group by
       group, with the key each entry is addressed by.  The layout the service
       reads (and permutes) is dumped by the loader itself, under
       TC_MODLOADER_PIN_ORDER_LOG=dump, so this side only has to check that the
       service answers with the pins the level really has;
     * the anchors the panel lays its entries out with.  igSetCursorPos is
       called once per entry from inside build_io_state_view, which is how the
       Mod knows where an entry is and when to draw.  Both the window-local and
       the screen position are logged, so a screenshot can be read together with
       the log.

   This probe owns the igSetCursorPos hook, so it must not be loaded together
   with the Mod itself (the Mod hooks the same call); the playtest loads one or
   the other. */
#include "tc_mod_api.h"
#include "../sdk/tc_pin_order.h"

#include <windows.h>
#include <stdint.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const TCHost* host;

static const char kIoStateView[] =
    "build_io_state_view__presenterZboard95uiZio95state95view_u100";

static void* ioStateView;
static int frames;
static int reports;

struct Vec2 {
    float x, y;
};
static void (*setCursorPosOriginal)(Vec2);
static void (*setCursorPosYOriginal)(float);
static void (*cursorScreenPos)(Vec2*);
static int (*frameCount)();
static int lastFrame = -1;

static void log(const char* text) {
    if (host && host->log) host->log(host->context, text);
}

static bool insidePanel(const void* caller) {
    const auto at = reinterpret_cast<uintptr_t>(caller);
    const auto panel = reinterpret_cast<uintptr_t>(ioStateView);
    return panel && at >= panel && at < panel + 0x3fc0;
}

/* The service, once per frame the panel is up: what each group holds right
   now, in the order the panel draws it. */
static void reportGroups() {
    TCPinOrderApiV1 pins{};
    if (!tc::pin_order::table(host, &pins) || !tc::pin_order::ready(pins)) {
        if (reports++ < 1) log("pin order service: unavailable");
        return;
    }
    for (uint32_t group = 0; group < TC_PIN_ORDER_GROUP_COUNT; ++group) {
        const uint32_t count = tc::pin_order::count(pins, group);
        if (count == 0) continue;
        char line[192];
        std::snprintf(line, sizeof(line), "pin order service: group=%u count=%u", group, count);
        log(line);
        for (uint32_t index = 0; index < count; ++index) {
            TCPinOrderEntryV1 entry{};
            const int status = tc::pin_order::entry(pins, group, index, &entry);
            std::snprintf(line, sizeof(line),
                          "pin order service:   #%u key=%llu width=%llu name=\"%s\" status=%d",
                          index, static_cast<unsigned long long>(entry.key),
                          static_cast<unsigned long long>(entry.width), entry.name, status);
            log(line);
        }
        uint64_t keys[64]{};
        uint32_t length = 0;
        const int status = tc::pin_order::order(pins, group, keys, 64, &length);
        std::snprintf(line, sizeof(line), "pin order service: group=%u order status=%d length=%u",
                      group, status, length);
        log(line);
    }
}

/* The anchors: the panel positions every entry with this call while it is
   inside build_io_state_view. */
static void setCursorPosDetour(Vec2 pos) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (setCursorPosOriginal) setCursorPosOriginal(pos);
    if (!insidePanel(reinterpret_cast<const void*>(caller))) return;
    /* One report per frame, taken while the panel is up: the service answers
       only while it is, which is the same rule the Mod lives under. */
    const int frame = frameCount ? frameCount() : 0;
    if (frame != lastFrame) {
        lastFrame = frame;
        ++frames;
        if (frames <= 3) reportGroups();
    }
    if (frames > 2 || reports > 80) return;
    ++reports;
    Vec2 screen{};
    if (cursorScreenPos) cursorScreenPos(&screen);
    char line[192];
    std::snprintf(line, sizeof(line),
                  "pin order anchor: frame=%d caller=+0x%llx local=(%.0f,%.0f) screen=(%.0f,%.0f)",
                  frames, static_cast<unsigned long long>(caller - reinterpret_cast<uintptr_t>(ioStateView)),
                  pos.x, pos.y, screen.x, screen.y);
    log(line);
}

/* The panel's scalar anchors: the section headings, and the value line under a
   wide pin's squares - which is also what the punch-tape Mod shifts when it
   makes an entry taller. */
static void setCursorPosYDetour(float y) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (setCursorPosYOriginal) setCursorPosYOriginal(y);
    if (!insidePanel(reinterpret_cast<const void*>(caller)) || frames > 2 || reports > 120) return;
    ++reports;
    char line[160];
    std::snprintf(line, sizeof(line), "pin order anchor: frame=%d scalarY caller=+0x%llx y=%.0f",
                  frames,
                  static_cast<unsigned long long>(caller - reinterpret_cast<uintptr_t>(ioStateView)), y);
    log(line);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != 1 || !out || out->size < sizeof(TCPlugin)) return 1;
    host = h;
    if (!h->resolve_symbol || !h->log || !h->create_hook) return 2;
    ioStateView = h->resolve_symbol(h->context, kIoStateView);
    if (!ioStateView) {
        log("pin order probe: io state view symbol missing");
        return 3;
    }
    void* cursorScreen = h->engine_proc(h->context, "igGetCursorScreenPos");
    std::memcpy(&cursorScreenPos, &cursorScreen, sizeof(cursorScreen));
    void* getFrameCount = h->engine_proc(h->context, "igGetFrameCount");
    std::memcpy(&frameCount, &getFrameCount, sizeof(getFrameCount));
    void* setCursorPos = h->resolve_symbol(h->context, "igSetCursorPos");
    if (!setCursorPos) {
        log("pin order probe: igSetCursorPos symbol missing");
        return 4;
    }
    if (h->create_hook(h->context, setCursorPos, reinterpret_cast<void*>(&setCursorPosDetour),
                       reinterpret_cast<void**>(&setCursorPosOriginal)) != 0) {
        log("pin order probe: igSetCursorPos hook refused");
        return 5;
    }
    void* setCursorPosY = h->resolve_symbol(h->context, "igSetCursorPosY");
    if (setCursorPosY &&
        h->create_hook(h->context, setCursorPosY, reinterpret_cast<void*>(&setCursorPosYDetour),
                       reinterpret_cast<void**>(&setCursorPosYOriginal)) == 0)
        log("pin order probe: scalar anchor trace installed");
    log("pin order probe: anchor hook installed");
    (void)out;
    return 0;
}
