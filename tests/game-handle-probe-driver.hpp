/* In-process driver for the Board handle probe.

   Compiled into the playtest build of tests/game-handle-probe.cpp
   (TC_HANDLE_PROBE_DRIVER).  It exists because the interesting assertions are
   about a *real* level lifetime, and only the game can produce one: the
   sandbox window is borderless fullscreen, so no outside process can click it.

   The sequence, one step per stage:

     menu       the plugin reports "Board unavailable" (nothing is loaded);
     board A    press the first home-page level entry, exactly like the other
                drivers, and wait for LEVEL_LOAD;
     leave A    press Escape - the player's own way out of a level, which makes
                the game call change_scene(ctx,0) itself; the loader's detour runs
                in front of that call and invalidates the handle;
     board B    back on the home page, press a *different* entry, so the second
                board is a different level and both the entry and the exit are the
                player's own actions;
     leave B    Escape again, which has to invalidate board B's handle too.

   change_scene is loader-owned since Board handles hang off it, so this driver
   never hooks it: the loader's own detour raises TC_EVENT_SCENE_CHANGE (carrying
   the context the game passed) and invalidates the handle in front of the game's
   function.  The driver keeps its own event subscription instead of reading the
   plugin's state, so the two agree only if the host really delivered both.

   The driver still knows how to call change_scene with that context: that is the
   fallback for a step the player's way cannot reach, and it says
   "self-exit=not-found" (or "entry=fallback") when it falls back, so the
   difference shows up in the log instead of being papered over. */
#pragma once
#ifdef TC_HANDLE_PROBE_DRIVER
#include "../sdk/tc_event.h"
#include "../sdk/tc_game_model.h"
#include "../sdk/tc_handle_api.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace tc_handle_driver {

struct V2 { float x, y; };

/* Which campaign entry loads a level, measured on the sandbox profile with
   TC_HANDLE_PROBE_TRACE=1: the five level blocks all draw the id "capture", so
   the call site is what tells them apart, and of the five only this one raises
   LEVEL_LOAD.  #3 and #4 open that level's own screen instead (its measured
   buttons are "Reset" and an unlabelled one; pressing the latter does not load
   the level either), and #5 makes the game exit by itself with code 0 - it is not
   a level at all.  Pressing by index alone is not reliable for the same reason:
   the screen draws a different number of buttons depending on the state.  So the
   probe uses this entry for both boards and verifies the *repeat* of the player's
   entry, which is the lifecycle fact a handle has to get right; a save with more
   levels unlocked would let the same mechanism press a different one. */
constexpr int kLevelEntryRva = 0x44a308u;

struct Driver {
    const TCHost* host = nullptr;
    void (*log)(const char*) = nullptr;

    bool (*invisibleOriginal)(const char*, V2, int) = nullptr;
    bool (*buttonOriginal)(const char*, V2) = nullptr;
    uint32_t (*boardUiOriginal)(void*, void*) = nullptr;
    void (*changeScene)(void*, int) = nullptr;

    /* Filled by this driver's own event subscription. */
    TCGameHandle board{};
    bool haveBoard = false;
    int levelLoads = 0, sceneChanges = 0, staleAfterScene = 0;

    void* board_context = nullptr;
    HWND window = nullptr;
    bool installed = false, started = false, done = false;
    /* The player's own paths come first; the driver's own calls are fallbacks for
       the case where a step cannot be reached that way. */
    bool fallbackLeave = false, fallbackEntry = false;
    /* The call site of the level entry to swallow next, 0 for none.  Pressing the
       exact button (see kLevelEntryRva) is what keeps the entry reachable after a
       level has been left: the campaign screen draws a different set of buttons
       depending on the state, so counting presses would press the wrong one. */
    uintptr_t pressTarget = 0;
    int stage = 0, start_frame = 0;
    int entryPresses = 0;
    double start_time = 0, elapsed = 0, stage_time = 0;
    /* The load counts when a step was taken, so "the next board" means a load
       that arrives *after* the step instead of one board A already produced. */
    int loadsAtExit = -1, scenesBeforeStep = -1, staleBeforeStep = -1;
    void (*loadLevel)(void*, const tc::TCNimString*) = nullptr;
    tc::TCGameModel game;
    bool gameLoaded = false;

    void say(const std::string& message) { if (log) log(message.c_str()); }

    /* Discovery aid (TC_HANDLE_PROBE_TRACE=1): while the driver is waiting for
        the entry press that starts a board, list every invisible button the game
       draws together with the call site it came from.  The screen after leaving
       a level is not the home page, so finding the button that leads back into a
       level is a measurement, not a guess. */
    void traceButton(uintptr_t rva, const char* id, int frame) {
        static std::vector<std::pair<uintptr_t, std::string>> seen;
        if (seen.size() >= 40) return;
        const std::string label = id ? id : "(null)";
        for (const auto& entry : seen)
            if (entry.first == rva && entry.second == label) return;
        seen.push_back({rva, label});
        say("DRIVER: button rva=0x" + hex(rva) + " id=" + label + " frame=" +
            std::to_string(frame));
    }

    /* Same idea for the game's ordinary buttons: their label is the visible text,
       which is how the screen the player lands on after leaving a level can be
       read out of the log. */
    void traceLabel(uintptr_t rva, const char* label, int frame) {
        static std::vector<std::pair<uintptr_t, std::string>> seen;
        if (seen.size() >= 40) return;
        std::string text = label ? label : "(null)";
        if (text.size() > 48) text.resize(48);
        for (const auto& entry : seen)
            if (entry.first == rva && entry.second == text) return;
        seen.push_back({rva, text});
        say("DRIVER: igButton rva=0x" + hex(rva) + " \"" + text + "\" frame=" +
            std::to_string(frame));
    }

    static std::string hex(uintptr_t value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
        return text;
    }

    int frameCount() const {
        auto get = reinterpret_cast<int (*)()>(host->engine_proc(host->context, "igGetFrameCount"));
        return get ? get() : 0;
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

    void leaveBoard() {
        if (!changeScene) { say("DRIVER: cannot change the scene (missing symbol)"); return; }
        if (!board_context) { say("DRIVER: cannot change the scene (no board context yet)"); return; }
        say("DRIVER: calling change_scene ctx=0x" + hex(reinterpret_cast<uintptr_t>(board_context)) +
            " scene=0");
        changeScene(board_context, 0);
    }

    /* Asks for the campaign screen's own level entry to be pressed:
       hookInvisible swallows exactly that button, so the game reacts to it as a
       real click. */
    void pressEntry() {
        ++entryPresses;
        pressTarget = kLevelEntryRva;
        say("DRIVER: pressing the level entry (rva=0x" + hex(pressTarget) + ") to reach the " +
            (entryPresses == 1 ? "first" : "second") + " level");
    }

    /* Starts the player's own exit and remembers where the handle state stood, so
       the wait below can tell "this scene change is the exit" from an earlier
       one. */
    void beginSelfExit() {
        scenesBeforeStep = sceneChanges;
        staleBeforeStep = staleAfterScene;
        say("DRIVER: trying the game's own way out (Escape)");
        pressKey(VK_ESCAPE);
    }

    bool leftSinceStep() const {
        return sceneChanges > scenesBeforeStep && staleAfterScene > staleBeforeStep;
    }

    /* A real key press, the way tests/ui-keyboard-driver.hpp sends one: GLFW (and
       therefore the game's ImGui backend) derives the key from the scancode, not
       from the virtual key, so the message must carry one. */
    void pressKey(int virtualKey) {
        if (!window) return;
        SetForegroundWindow(window);
        const UINT scan = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
        LPARAM down = 1 | (static_cast<LPARAM>(scan & 0xff) << 16);
        LPARAM up = down | (1LL << 30) | (1LL << 31);
        PostMessageW(window, WM_KEYDOWN, virtualKey, down);
        Sleep(30);
        PostMessageW(window, WM_KEYUP, virtualKey, up);
        Sleep(30);
    }

    /* The game's own load_level(), the one the board's toolbar and the level
       screen go through, with a name built the way the game builds one. */
    void loadSecondLevel() {
        if (!loadLevel || !board_context || !gameLoaded) {
            say("DRIVER: cannot load a second level (symbol or string helper missing)");
            return;
        }
        const char* name = "sandbox";
        tc::TCNimString level{};
        const size_t length = std::strlen(name);
        game.raw_new_string(&level, static_cast<int64_t>(length));
        if (!level.data) {
            say("DRIVER: cannot allocate the second level's name");
            return;
        }
        std::memcpy(static_cast<unsigned char*>(level.data) + 8, name, length);
        level.length = length;
        static_cast<unsigned char*>(level.data)[8 + length] = 0;
        say("DRIVER: asking the game to load a second level ('" + std::string(name) + "')");
        loadLevel(board_context, &level);
    }

    void tick(const TCFrame* frame) {
        if (!installed || !frame || done) return;
        if (!started) {
            started = true;
            start_frame = frame->frame_number;
            start_time = frame->time_seconds;
            HWND found = nullptr;
            EnumWindows(windowCallback, reinterpret_cast<LPARAM>(&found));
            window = found;
            if (found)
                SetWindowPos(found, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            say("DRIVER: started frame=" + std::to_string(frame->frame_number));
        }
        elapsed = frame->time_seconds - start_time;
        /* One heartbeat every ~600 frames, like the other drivers: it tells "the
           game stopped calling us" apart from "a step never happened". */
        if ((frame->frame_number - start_frame) % 600 == 0 && frame->frame_number != start_frame)
            say("DRIVER: alive frame=" + std::to_string(frame->frame_number) + " stage=" +
                std::to_string(stage) + " loads=" + std::to_string(levelLoads) +
                " scenes=" + std::to_string(sceneChanges) +
                " stale=" + std::to_string(staleAfterScene));
        if (elapsed > 95.0) {
            done = true;
            say("DRIVER: giving up after " + std::to_string(static_cast<int>(elapsed)) +
                "s at stage " + std::to_string(stage) + " (loads=" + std::to_string(levelLoads) +
                " scenes=" + std::to_string(sceneChanges) +
                " stale=" + std::to_string(staleAfterScene) + ")");
            return;
        }
        switch (stage) {
            case 0:
                /* The plugin's own "Board unavailable" line is the menu evidence;
                   the driver only waits long enough for it to be there. */
                if (elapsed > 2.0) { stage = 1; say("DRIVER: main menu observed"); }
                break;
            case 1:
                if (elapsed > 3.0) { pressEntry(); stage = 2; stage_time = elapsed; }
                break;
            case 2:
                if (levelLoads >= 1) {
                    stage = 3;
                    stage_time = elapsed;
                    say("DRIVER: board A handle observed");
                } else if (elapsed > stage_time + 15.0) {
                    done = true;
                    say("DRIVER: the first level never loaded");
                }
                break;
            case 3:
                if (elapsed > stage_time + 3.5) {
                    stage = 4;
                    stage_time = elapsed;
                    beginSelfExit();
                }
                break;
            case 4:
                if (leftSinceStep()) {
                    stage = 5;
                    stage_time = elapsed;
                    loadsAtExit = levelLoads;
                    say("DRIVER: self-exit=ok");
                    say("DRIVER: the game left the level by itself and the old handle was "
                        "invalidated");
                } else if (!fallbackLeave && elapsed > stage_time + 6.0) {
                    fallbackLeave = true;
                    stage_time = elapsed;
                    say("DRIVER: self-exit=not-found");
                    say("DRIVER: the game's own exit did not leave the level; falling back to "
                        "change_scene");
                    leaveBoard();
                } else if (fallbackLeave && elapsed > stage_time + 12.0) {
                    done = true;
                    say("DRIVER: the scene change did not invalidate the old handle");
                }
                break;
            case 5:
                /* The campaign screen is back (that is where the game's own exit
                   returns to); press the entry again, so the second board is a
                   fresh load entered the player's way. */
                if (elapsed > stage_time + 2.5) {
                    stage = 6;
                    stage_time = elapsed;
                    pressEntry();
                }
                break;
            case 6:
                if (levelLoads > loadsAtExit) {
                    stage = 7;
                    stage_time = elapsed;
                    say("DRIVER: board B handle observed");
                } else if (!fallbackEntry && elapsed > stage_time + 15.0) {
                    /* The entry press did not land.  Load a level by name instead,
                       exactly like tests/ui-board-panel-driver.hpp, and say so -
                       the sequence still covers a second board, just not through
                       the campaign screen. */
                    fallbackEntry = true;
                    stage_time = elapsed;
                    loadsAtExit = levelLoads;
                    say("DRIVER: entry=fallback");
                    say("DRIVER: the level entry did not load a level; loading one by name");
                    loadSecondLevel();
                } else if (fallbackEntry && elapsed > stage_time + 15.0) {
                    done = true;
                    say("DRIVER: the second level never loaded");
                }
                break;
            case 7:
                if (elapsed > stage_time + 3.5) {
                    stage = 8;
                    stage_time = elapsed;
                    beginSelfExit();
                }
                break;
            case 8:
                if (leftSinceStep()) {
                    stage = 9;
                    stage_time = elapsed;
                    say("DRIVER: self-exit=ok");
                    say("DRIVER: the game left the second level by itself and its handle was "
                        "invalidated");
                } else if (!fallbackLeave && elapsed > stage_time + 6.0) {
                    fallbackLeave = true;
                    stage_time = elapsed;
                    say("DRIVER: self-exit=not-found");
                    say("DRIVER: the game's own exit did not leave the second level; falling "
                        "back to change_scene");
                    leaveBoard();
                } else if (fallbackLeave && elapsed > stage_time + 12.0) {
                    done = true;
                    say("DRIVER: the scene change did not invalidate the old handle");
                }
                break;
            case 9:
                if (elapsed > stage_time + 1.5) {
                    done = true;
                    say("DRIVER: done");
                }
                break;
            default:
                break;
        }
    }
};

inline Driver& driver() { static Driver value; return value; }

inline bool hookInvisible(const char* id, V2 size, int flags) {
    auto& d = driver();
    const bool result = d.invisibleOriginal ? d.invisibleOriginal(id, size, flags) : false;
    const uintptr_t rva = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                          reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (std::getenv("TC_HANDLE_PROBE_TRACE")) d.traceButton(rva, id, d.frameCount());
    if (!d.pressTarget) return result;
    if (rva != d.pressTarget || d.elapsed < 3.0) return result;
    d.say("DRIVER: pressed the level entry at rva=0x" + Driver::hex(d.pressTarget));
    d.pressTarget = 0;
    return true;
    return result;
}

inline uint32_t hookBoardUi(void* context, void* board) {
    auto& d = driver();
    if (!d.board_context) d.board_context = context;
    return d.boardUiOriginal ? d.boardUiOriginal(context, board) : 0u;
}

inline bool hookButton(const char* label, V2 size) {
    auto& d = driver();
    const bool result = d.buttonOriginal ? d.buttonOriginal(label, size) : false;
    const uintptr_t rva = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                          reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (std::getenv("TC_HANDLE_PROBE_TRACE")) d.traceLabel(rva, label, d.frameCount());
    return result;
}

/* This driver's own subscription: it counts what the host delivered instead of
   trusting the plugin's log, and it keeps the handle the invalidation has to
   refuse. */
inline void onEvent(TCEvent* event) {
    auto& d = driver();
    if (tc::events::is(event, TC_EVENT_LEVEL_LOAD)) {
        ++d.levelLoads;
        TCGameHandle issued{};
        if (tc::currentGameHandle(d.host, TC_GAME_OBJECT_BOARD, &issued) == TC_HANDLE_OK) {
            d.board = issued;
            d.haveBoard = true;
        }
        return;
    }
    if (tc::events::is(event, TC_EVENT_SCENE_CHANGE)) {
        ++d.sceneChanges;
        if (d.haveBoard && tc::validateGameHandle(d.host, &d.board) == 0) ++d.staleAfterScene;
    }
}

inline bool start(const TCHost* host, void (*logFunction)(const char*)) {
    auto& d = driver();
    if (d.installed || !host || !host->create_hook || !host->resolve_symbol ||
        !host->engine_proc)
        return false;
    d.host = host;
    d.log = logFunction;
    d.changeScene = reinterpret_cast<decltype(d.changeScene)>(
        host->resolve_symbol(host->context, "change_scene__presenterZcontext_u2958"));
    if (!d.changeScene) {
        d.say("DRIVER: missing symbol change_scene__presenterZcontext_u2958");
        return false;
    }
    d.loadLevel = reinterpret_cast<decltype(d.loadLevel)>(
        host->resolve_symbol(host->context, "load_level__modelZutilities_u7740"));
    d.gameLoaded = d.game.load(host) && d.game.raw_new_string != nullptr;
    if (!d.loadLevel || !d.gameLoaded)
        d.say("DRIVER: the game's own level loader is unavailable; only one board will be measured");
    struct Target { const char* symbol; void* detour; void** original; };
    const Target targets[] = {
        {"igInvisibleButton", reinterpret_cast<void*>(hookInvisible),
         reinterpret_cast<void**>(&d.invisibleOriginal)},
        {"build_board_ui__presenterZboard95ui_u15", reinterpret_cast<void*>(hookBoardUi),
         reinterpret_cast<void**>(&d.boardUiOriginal)},
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
    if (tc::events::subscribe(host, TC_EVENT_LEVEL_LOAD | TC_EVENT_SCENE_CHANGE, &onEvent,
                             nullptr) != TC_EVENT_OK) {
        d.say("DRIVER: cannot subscribe to the event bus");
        return false;
    }
    /* Optional: only the trace mode needs the game's ordinary buttons, so a
       normal run does not hook anything beyond what it presses. */
    if (std::getenv("TC_HANDLE_PROBE_TRACE")) {
        void* address = host->resolve_symbol(host->context, "igButton");
        if (address && host->create_hook(host->context, address,
                                        reinterpret_cast<void*>(hookButton),
                                        reinterpret_cast<void**>(&d.buttonOriginal)) == 0)
            d.say("DRIVER: tracing igButton labels");
        else
            d.say("DRIVER: igButton is not hookable; labels stay unknown");
    }
    d.installed = true;
    return true;
}

/* The loader calls the plugin's on_frame once per engine frame; the plugin
   forwards it here, because a plugin has exactly one such entry point. */
inline void tick(const TCFrame* frame) { driver().tick(frame); }

}  // namespace tc_handle_driver
#endif  // TC_HANDLE_PROBE_DRIVER
