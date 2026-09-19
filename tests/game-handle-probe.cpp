/* Development probe for the Board handle registry (sdk/tc_handle_api.h).

   Read-only, the same way tests/hook-chain-probe.cpp is: it hooks nothing by
   itself and asks the host only.  What it exists for is the half of the
   contract a unit test cannot reach - the *real* level lifetime:

     * the main menu has no board, so the query has to answer UNAVAILABLE;
     * entering a level issues a handle whose resolve() is the very board the
       LEVEL_LOAD event carries;
     * leaving the level invalidates that handle (0 / STALE) and the query goes
       back to UNAVAILABLE;
     * the next level gets a different generation and token, so a handle kept
       from the first level can never resolve into the second one.

   The probe prints one PROBE: line per transition.  tests/hook-chain-probe.cpp
   shows the other half of the pattern: the playtest build (TC_HANDLE_PROBE_DRIVER)
   compiles tests/game-handle-probe-driver.hpp into the same package, and that
   driver enters a board, leaves it and enters another one, because no external
   process can click a borderless fullscreen window. */
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_event.h"
#include "../sdk/tc_handle_api.h"
#include <cstdio>
#include <string>

#ifdef TC_HANDLE_PROBE_DRIVER
#include "game-handle-probe-driver.hpp"
#endif

static const TCHost* host;
static TCGameHandle board{};
static bool sawAvailable = false, haveHandle = false, loggedMenu = false;
static int levelLoads = 0, sceneChanges = 0, resolvedMatches = 0, staleAfterScene = 0;
/* The last engine frame the loader called us for: it dates every event, which is
   how "the level loaded and the scene switched in the same frame" is told from
   "the player left the level several frames later". */
static int lastFrame = -1;

static void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

#ifdef TC_HANDLE_PROBE_DRIVER
static void logLine(const char* message) {
    if (host && host->log) host->log(host->context, message);
}
#endif

static std::string hex64(uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
    return text;
}

static const char* handleResult(int status) {
    switch (status) {
        case TC_HANDLE_OK: return "ok";
        case TC_HANDLE_ERR_UNAVAILABLE: return "unavailable";
        case TC_HANDLE_ERR_ARGUMENT: return "argument";
        case TC_HANDLE_ERR_KIND: return "kind";
        case TC_HANDLE_ERR_STALE: return "stale";
        default: return "unknown";
    }
}

static void onEvent(TCEvent* event) {
    if (tc::events::is(event, TC_EVENT_LEVEL_LOAD)) {
        ++levelLoads;
        if (haveHandle)
            report("PROBE: previous handle valid=" +
                   std::to_string(tc::validateGameHandle(host, &board)) +
                   " after the new level load");
        void* subject = tc::events::levelBoardModel(event);
        const char* name = tc::events::levelName(event);
        report("PROBE: level.load name=" + std::string(name ? name : "(none)") +
               " subject=0x" + hex64(reinterpret_cast<uint64_t>(subject)) +
               " frame=" + std::to_string(lastFrame));
        TCGameHandle issued{};
        const int status = tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &issued);
        if (status != TC_HANDLE_OK) {
            report(std::string("PROBE: level handle unavailable (") + handleResult(status) + ")");
            return;
        }
        board = issued;
        haveHandle = true;
        const void* raw = nullptr;
        const int resolvedStatus = tc::resolveGameHandle(host, &board, &raw);
        const bool match = resolvedStatus == TC_HANDLE_OK && raw == subject;
        if (match) ++resolvedMatches;
        report("PROBE: level handle generation=" + std::to_string(board.generation) +
               " token=" + std::to_string(board.token) +
               " size=" + std::to_string(board.size) +
               " resolve=" + std::string(handleResult(resolvedStatus)) +
               " resolve-match=" + (match ? "1" : "0") +
               " valid=" + std::to_string(tc::validateGameHandle(host, &board)));
        TCGameHandle wrong{sizeof(TCGameHandle), TC_GAME_OBJECT_COMPONENT, 0, 0};
        report(std::string("PROBE: component handle kind guard=") +
               handleResult(tc::currentGameHandle(host, TC_GAME_OBJECT_COMPONENT, &wrong)));
        return;
    }
    if (tc::events::is(event, TC_EVENT_SCENE_CHANGE)) {
        ++sceneChanges;
        /* The loader's detour invalidates the registry before it raises this
           event, so even a listener sees the old handle refused. */
        const int valid = haveHandle ? tc::validateGameHandle(host, &board) : -99;
        report("PROBE: scene.change scene=" + std::to_string(static_cast<int>(event->flags)) +
               " frame=" + std::to_string(lastFrame) +
               " old-handle-valid=" + std::to_string(valid));
        if (haveHandle && valid == 0) ++staleAfterScene;
        if (haveHandle) {
            const void* raw = nullptr;
            report(std::string("PROBE: old handle resolve=") +
                   handleResult(tc::resolveGameHandle(host, &board, &raw)) +
                   " pointer=" + (raw ? "set" : "cleared"));
        }
        TCGameHandle now{};
        const int status = tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &now);
        report(std::string("PROBE: current after scene change=") + handleResult(status));
    }
}

static void onFrame(void*, const TCFrame* frame) {
    if (frame) lastFrame = frame->frame_number;
#ifdef TC_HANDLE_PROBE_DRIVER
    /* The driver only has this one per-frame entry point: the loader calls the
       plugin, and the plugin forwards. */
    tc_handle_driver::tick(frame);
#else
    (void)frame;
#endif
    TCGameHandle now{};
    const int status = tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &now);
    if (status == TC_HANDLE_OK) {
        if (!sawAvailable) {
            sawAvailable = true;
            report("PROBE: Board available generation=" + std::to_string(now.generation) +
                   " token=" + std::to_string(now.token));
        }
        return;
    }
    if (!loggedMenu) {
        loggedMenu = true;
        report(std::string("PROBE: Board unavailable on the main menu (") +
               handleResult(status) + ")");
    }
    if (sawAvailable) {
        sawAvailable = false;
        report(std::string("PROBE: Board unavailable again (") + handleResult(status) + ")");
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!out->size || out->size < sizeof(TCPlugin)) return 2;
    if (!tc::hostHas(h, TC_CAP_GAME_HANDLES) || !tc::hostHas(h, TC_CAP_EVENTS)) return 3;
    if (tc::events::subscribe(h, TC_EVENT_LEVEL_LOAD | TC_EVENT_SCENE_CHANGE, &onEvent, nullptr) !=
        TC_EVENT_OK) return 4;
    out->on_frame = &onFrame;
    report("Game handle probe: armed (read-only)");
#ifdef TC_HANDLE_PROBE_DRIVER
    if (!tc_handle_driver::start(h, &logLine)) {
        report("Game handle probe: the driver could not be installed");
        return 5;
    }
#endif
    return 0;
}
