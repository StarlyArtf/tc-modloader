/* Development probe: how big is a custom component's clickable area on the
   board, and what decides it?

   This is step 2 of the appearance measurement (docs/research/component-
   appearance.md): the text-box Mod needs "the whole note is draggable" and
   "the game draws nothing for this component", and both depend on one unknown
   - the board footprint the game uses for hit testing.  The probe measures it
   instead of guessing:

     1. it registers three ordinary native types: default geometry, a 12x6
        footprint, and the same footprint placed at 90 degrees;
     2. once a board is up it places one instance of each at known board
        coordinates through the command bus;
     3. it clicks the two clean transition points on the default and expanded
        instances and reads the game's selection set after every click.

   This is deliberately a negative contract test.  If both shapes miss at
   (1,0) and hit at (2,0), set_footprint did not change pointer hit testing and
   geometry needs a separate hit-box path.  Every target click is bracketed by
   direct clears of the game's current and previous selection sets.

   Pair this with dev.enter-board so a level is actually entered. */
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_component_geometry.h"
#include "../sdk/tc_board_model.h"
#include "../sdk/tc_command_api.h"
#include "../sdk/tc_ui.h"
#include "../sdk/tc_ui_draw.h"

#include <windows.h>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct V2 { float x, y; };

const TCHost* host = nullptr;
tc::TCBoardModel boardModel;
tc::component_geometry::Api geometryApi{};
TCBoardApiV6 boardApi{};
TCCommandApiV2 commandApi{};
V2 (*worldToScreen)(V2) = nullptr;
void* (*igGetIO)() = nullptr;
void (*igGetMousePos)(V2*) = nullptr;
bool (*igIsMouseDown)(int) = nullptr;
bool (*igIsMouseClicked)(int,bool) = nullptr;
void (*addMouseButtonEvent)(void*,int,bool)=nullptr;
void (*addMousePosEvent)(void*,float,float)=nullptr;
bool injectedMouseDown=false;
void (*clearSelections)()=nullptr;
void (*clearComponentSet)(void*)=nullptr;
void* selectedComponentsRaw=nullptr;
void* previousSelectedComponentsRaw=nullptr;

std::vector<TCGameHandle> componentHandles;

/* The probe drives the real cursor, so a human moving the mouse during a run
   would otherwise be indistinguishable from the geometry under test.  Two
   guards: ClipCursor pins the pointer while the button is down, and every
   sample records whether the pointer and ImGui's own position were still where
   the probe put them.  A sample that is not clean is never written into a map. */
POINT lastScreenPoint{0,0};
V2 lastDisplayPoint{0.f,0.f};
int dirtySamples = 0;
/* The probe borrows the pointer for one press and gives it straight back, so a
   run no longer parks the user's cursor on the board for two minutes. */
POINT savedCursor{0,0};
bool cursorBorrowed = false;
/* Handing the pointer back after every sample is friendlier to whoever is
   using the machine, but it also changes the measurement: with the restore on,
   the two-point case that used to select at (2,0) stops selecting anywhere.
   The gesture that produced the recorded evidence therefore keeps the cursor
   parked unless TC_HITBOX_RESTORE=1 asks for the polite behaviour. */
bool restoreCursor = false;
/* Manual mode: the probe injects nothing at all.  A human clicks in the sandbox
   and every press is recorded with the board coordinates the game itself
   computed, which sidesteps every synthetic-input artefact above. */
bool manualMode = false;
bool manualWasDown = false;
int manualPresses = 0;
/* Place the types, report where they are, and inject nothing at all.  Used to
   photograph the board (TC_MODLOADER_SHOT) and see where the game actually
   draws each component, instead of deriving that from the record's coordinates. */
bool placeOnly = false;
/* Ask the game itself which component is at a board point, instead of injecting
   input and watching for a reaction.  `handle_no_action_yet` reads the answer
   from `get_component_id(board, point)` (a table at board+0x2c0 keyed by a
   packed board point), so calling that function over a grid *is* the game's own
   hit test - no mouse, no latch, no panel. */
bool queryMode = false;
/* With this set the probe places the types, installs the hover hook and then
   leaves the mouse alone, so a human can move over the board while the game's
   own answers are recorded. */
bool queryIdle = false;
/* Draw the declared footprint box on the board so it can be compared with the
   component the game draws and with the area the mouse can actually grab.  The
   measurement kept disagreeing with the declaration; a picture settles where
   the box really is. */
bool drawFootprints = false;
/* Built-in kinds to place instead of the probe's own types: the appearance work
   needs "put kind K on an empty board and photograph it" without any input. */
std::vector<uint32_t> placeKinds;
/* Layout for the appearance runs: columns, x spacing, y spacing, and how many
   rotations to place per kind. */
int kindColumns = 5;
int kindSpacingX = 15;
int kindSpacingY = 16;
int kindRotations = 1;
/* Grid origin, so a run can keep clear of the game's left tool column and top
   bar (the default -24 y row sits close to them). */
int kindOriginX = 0;
int kindOriginY = 0;
/* After placing the kinds, click the first one so the game draws its selection
   highlight.  The highlight is the cheapest way to see the instance quad the
   sprite is stretched over - the one number the content-box maths can only
   infer. */
bool highlightFirstKind = false;
V2 highlightCentre{0.f, 0.f};
void* (*igGetBackgroundDrawList)(void*) = nullptr;
void* (*igGetMainViewport)() = nullptr;
void* (*igGetForegroundDrawList)(void*) = nullptr;
void (*igEndOriginal)() = nullptr;
void drawFootprintOverlay();
void report(const std::string& message);
/* The overlay must be added while the frame is still open.  A plugin's on_frame
   runs after the game's igEnd, and touching a draw list there crashes the game
   (measured: access violation inside the callback).  Drawing just *before* the
   engine's own EndFrame puts the boxes on top of what the game drew, and `igEnd`
   is a `void(void)` entry point so forwarding is exact. */
void hookEndFrame() {
    static bool reported = false;
    if (!reported) { reported = true; report("hitbox: overlay frame hook ran"); }
    drawFootprintOverlay();
    if (igEndOriginal) igEndOriginal();
}
/* After placing, recompile/refresh the board the way a normal edit does.  The
   user's 2026-09-22 observation is the reason: components placed through the
   command bus were not draggable until a component placed by hand made them so,
   which says the placement leaves the board's derived hit state stale. */
bool refreshAfterPlace = false;
bool upgradeAfterPlace = false;
void (*refreshBoard)(void*) = nullptr;
/* The game's manual placement calls upgrade(presenter+0x1a3b8, 0x30) right
   after add_component succeeds.  The probe cannot name that pointer, so it
   records it from the game's own call and replays the same upgrade after the
   command-bus placement. */
void report(const std::string& message);
void (*upgradeState)(void*, uint8_t) = nullptr;
void* presenterSlot = nullptr;

void hookUpgrade(void* slot, uint8_t target) {
    if (!presenterSlot) {
        presenterSlot = slot;
        char line[128];
        std::snprintf(line, sizeof(line), "hitbox: presenter slot=0x%llx target=%u",
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(slot)),
                      static_cast<unsigned>(target));
        report(line);
    }
    if (upgradeState) upgradeState(slot, target);
}
int64_t (*getComponentId)(void*, int32_t) = nullptr;
void* rawBoardForQuery = nullptr;
/* The hover observer: hook the game's own "which component is at this point"
   lookup, so every answer the game computes during real hovering is recorded
   with the key the game itself used.  That settles both the key format (which
   the direct calls could only guess at) and the map. */
int64_t (*getComponentIdOriginal)(void*, int32_t) = nullptr;
int hoverSamples = 0;
constexpr int kMaxHoverSamples = 3000;
void report(const std::string& message);

int64_t hookGetComponentId(void* board, int32_t point) {
    const int64_t id = getComponentIdOriginal ? getComponentIdOriginal(board, point) : -1;
    /* Log transitions only: the game asks every frame, and the interesting
       thing is where the answer changes while the mouse moves. */
    static int32_t lastPoint = 0x7fffffff;
    static int64_t lastResult = 0x7fffffffffffffffLL;
    if ((point != lastPoint || id != lastResult) && hoverSamples < kMaxHoverSamples) {
        lastPoint = point;
        lastResult = id;
        ++hoverSamples;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "hitbox: hover point=0x%08x x=%d y=%d component=%lld",
                      static_cast<unsigned>(point),
                      static_cast<int>(static_cast<int16_t>(point & 0xffff)),
                      static_cast<int>(static_cast<int16_t>((point >> 16) & 0xffff)),
                      static_cast<long long>(id));
        report(line);
    }
    return id;
}
/* A click is committed by the game on its own schedule, so a human press is
   recorded twice: once when the button goes down and once when the game's
   selection set has settled after the release.  The pair is what tells "this
   click selected the component" apart from "nothing was ever hit". */
bool manualPending = false;
double manualPendingX = 0.0, manualPendingY = 0.0;
unsigned long long manualPendingAt = 0;

/* ImGui-only injection.  `SetCursorPos` moves the user's real pointer, so the
   probe has to defend every sample against a human hand and against its own
   coordinate mapping.  The game reads the pointer from its own ImGui IO, so the
   probe can instead write the position straight into `ImGuiIO_AddMousePosEvent`
   and never touch the desktop.

   The catch is that the Win32 backend re-adds the operating system's cursor
   position every frame while the cursor is tracked over a focused window; that
   event would arrive after ours and win.  Parking the game window off the
   desktop (the same trick `tests/ui-page-playtest.ps1` uses) makes the cursor
   leave the window for good, so the only mouse position the game sees is the
   one the probe wrote.  `TC_HITBOX_INJECT=imgui` selects this device; the
   default stays the real-cursor one so the recorded two-point case keeps
   reproducing. */
bool injectImgui = false;
/* Pure SendInput: move the pointer and press with the same calls a real mouse
   makes, and inject nothing else - no SetCursorPos, no posted messages, no
   ImGui events.  Everything above failed to move even a built-in AND gate, so
   the mixture itself is the suspect. */
bool injectSendInput = false;
bool windowParked = false;
constexpr int kParkX = -4000;
constexpr int kParkY = 200;

void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

void logic(TCLogicIO* io){if(io&&io->output_count)io->outputs[0]=0;}

struct Type {
    const char* name;
    uint64_t id;
    bool expanded;
    uint32_t rotation;
    /* what the probe found on the board */
    uint64_t instance = 0;
    uint64_t selectionId = 0;
    int32_t x = 0, y = 0;
    /* live position just before the current press, for the move observable */
    int32_t beforeX = 0, beforeY = 0;
    /* Half extents measured once, when the instance was found.  Re-reading them
       later needs a live handle, and those go stale after a board edit, which
       silently skipped the whole overlay. */
    float halfWidth = 0.f, halfHeight = 0.f;
    /* Outputs the registration declares.  A decorative board object (the
       text-note shape) has none, and whether its box is still draggable is a
       product question of its own. */
    uint32_t pinCount = 1;
};
/* The fourth entry is a positive control: a built-in AND gate (kind 0x04, no
   custom prototype).  Built-in components are known to be selectable and
   draggable by their body, so if the instrument cannot see *that*, nothing it
   says about the custom types means anything.  It is placed through the same
   command, with custom_prototype_id left zero. */
constexpr uint32_t kBuiltinAndKind = 0x04;
/* The built-in control is scanned first on purpose: a component that is
   selected pops its panel up over the board, so the control has to be measured
   before anything else can have been selected. */
Type types[4] = {
    {"hit-and", 0, false, 0, 0, 0, 0, 0, 0, 0},
    {"hit-default", 0x4849545F30303031ULL, false, 0, 0, 0, 0, 0, 0, 0},
    {"hit-expanded", 0x4849545F30303032ULL, true, 0, 0, 0, 0, 0, 0, 0},
    /* The text-note shape: no pins at all, but a 12x6 footprint. */
    {"hit-note", 0x4849545F30303034ULL, true, 0, 0, 0, 0, 0, 0, 0, 0.f, 0.f, 0},
};
constexpr int kTypeCount = 4;

struct Offset { int x, y; };

/* (1,0) is outside the stock selectable pixels; (2,0) lands on the stock
   output/picture area.  The 12x6 footprint ought to include both if footprint
   and pointer hit testing were the same engine geometry.

   `TC_HITBOX_SCAN=map` replaces the two transition points with a whole grid
   around each instance's centre, which turns "the footprint is not the pointer
   hit box" into a map of the region the game really uses.  Everything else
   (gesture, clearing, sampling) stays identical so the two runs stay
   comparable. */
Offset offsets[128] = {{1,0},{2,0}};
int offsetCount = 2;
bool mapMode = false;
/* The contract case presses without hovering first; the map hovers one frame
   before the press by default.  TC_HITBOX_HOVER=0 makes the map use the exact
   same gesture as the two-point case, which is the gesture that is known to
   select at (2,0). */
bool hoverFirst = true;
/* The gesture moves the pointer this far while the button is held, because a
   bare press apparently commits nothing in this game. */
int dragPixels = 8;
/* A real mouse drag arrives as many small moves, one per frame.  A single
   teleport of `dragPixels` was not recognised as a drag at all - measured
   2026-09-22, even a built-in AND gate did not move - so the gesture is spread
   over this many frames by default. */
int dragSteps = 4;
int dragStep = 0;
/* The selection set turned out to be a latch: measured 2026-09-22, three
   consecutive presses on the same spot read 0,0,1 and every later point in the
   run read 1 as well, empty board included.  So "is it selected" answers "has
   this component been selected at some point during this run", which cannot be
   turned into a per-point hit map.  `TC_HITBOX_MEASURE=move` switches the
   observable to the component's own position: pressing and dragging a
   component carries it along, pressing empty board does not. */
bool measureMove = false;
/* A press on empty board clears the game's selection, but the game re-asserts
   its own selection while a component stays selected, so a point-by-point map
   fills up with 1s after the first hit.  With this on, every sample first makes
   a reset click on a far empty point and records what was selected afterwards;
   the target press then starts from a known-empty selection. */
bool resetEachSample = false;
int preselected = -1;
/* A successful drag moves the component, which both drifts the grid and can run
   it into a neighbour's collision box.  With this on the probe plays the move
   back with the game's own undo, so every sample starts from the same place. */
bool undoAfterMove = false;
uint8_t (*undoBoard)(void*) = nullptr;
/* The undo above turned out not to cover a move (the game's undo stack holds
   structural edits), so the grid alternates the drag direction instead: a drag
   is always detected either way, and consecutive samples cancel each other's
   drift. */
bool alternateDrag = false;
/* Per-phase wait in map mode.  The two-point case waits 350 ms; the grid runs
   shorter unless TC_HITBOX_TICK says otherwise, and this knob exists to find
   out whether that is what makes the two disagree. */
unsigned long long mapTick = 150;
/* Reading the instance back between samples is a diagnostic of its own; it is
   off by default because the two-point case does not do it and the map should
   reproduce that gesture exactly. */
bool refreshBetween = false;
constexpr int kMapMinX = -4, kMapMaxX = 4;
constexpr int kMapMinY = -3, kMapMaxY = 3;
constexpr int kMapWidth = kMapMaxX - kMapMinX + 1;
constexpr int kMapHeight = kMapMaxY - kMapMinY + 1;
int mapHits[kTypeCount][kMapHeight][kMapWidth];
/* Which types the map walks: the two custom ones by default, plus the built-in
   control when TC_HITBOX_TYPES=3 (or 4 for the rotated custom one as well). */
int scannedTypeCount = 2;

void buildOffsets() {
    const char* inject = std::getenv("TC_HITBOX_INJECT");
    if (inject && std::strcmp(inject, "imgui") == 0) injectImgui = true;
    /* Read before the scan branches below return: the overlay is wanted exactly
       in the place-only mode, which returns early. */
    const char* draw = std::getenv("TC_HITBOX_DRAW");
    if (draw && std::strcmp(draw, "1") == 0) drawFootprints = true;
    const char* highlight = std::getenv("TC_HITBOX_HIGHLIGHT");
    if (highlight && std::strcmp(highlight, "1") == 0) highlightFirstKind = true;
    if (const char* kinds = std::getenv("TC_HITBOX_KINDS")) {
        const char* at = kinds;
        while (*at && placeKinds.size() < 48) {
            char* end = nullptr;
            const long value = std::strtol(at, &end, 0);
            if (end == at) break;
            if (value > 0 && value <= 0xffff) placeKinds.push_back(static_cast<uint32_t>(value));
            if (!*end) break;
            at = end + 1;
        }
    }
    if (const char* layout = std::getenv("TC_HITBOX_KIND_LAYOUT")) {
        int columns = 0, dx = 0, dy = 0, rotations = 1;
        if (std::sscanf(layout, "%d,%d,%d,%d", &columns, &dx, &dy, &rotations) >= 3) {
            if (columns >= 1 && columns <= 12) kindColumns = columns;
            if (dx >= 8 && dx <= 64) kindSpacingX = dx;
            if (dy >= 8 && dy <= 64) kindSpacingY = dy;
            if (rotations >= 1 && rotations <= 4) kindRotations = rotations;
        }
    }
    if (const char* origin = std::getenv("TC_HITBOX_KIND_ORIGIN")) {
        int x = 0, y = 0;
        if (std::sscanf(origin, "%d,%d", &x, &y) == 2) {
            kindOriginX = x;
            kindOriginY = y;
        }
    }
    const char* scan = std::getenv("TC_HITBOX_SCAN");
    if (!scan) return;
    if (std::strcmp(scan, "manual") == 0) { manualMode = true; offsetCount = 0; return; }
    if (std::strcmp(scan, "none") == 0) { placeOnly = true; offsetCount = 0; return; }
    /* `query` walks the same offset list as `map` but asks the game instead of
       pressing anything. */
    queryMode = std::strcmp(scan, "query") == 0;
    if (std::strcmp(scan, "map") != 0 && !queryMode) return;
    mapMode = true;
    /* The map still drives the real cursor by default: measured on 2026-09-22,
       an ImGui-only position does reach the game (clean=1 samples read back the
       injected point) but the board never selects anything from it, so that
       device cannot answer the question yet.  `TC_HITBOX_DEVICE=imgui` keeps it
       available for the next attempt (it is the only device that would not need
       the desktop at all). */
    const char* device = std::getenv("TC_HITBOX_DEVICE");
    if (!device || std::strcmp(device, "cursor") == 0) injectImgui = false;
    else if (std::strcmp(device, "imgui") == 0) injectImgui = true;
    else if (std::strcmp(device, "sendinput") == 0) { injectImgui = false; injectSendInput = true; }
    const char* hover = std::getenv("TC_HITBOX_HOVER");
    if (hover && std::strcmp(hover, "0") == 0) hoverFirst = false;
    const char* tick = std::getenv("TC_HITBOX_TICK");
    if (tick && *tick) {
        const long value = std::strtol(tick, nullptr, 10);
        if (value >= 20 && value <= 5000) mapTick = static_cast<unsigned long long>(value);
    }
    const char* drag = std::getenv("TC_HITBOX_DRAG");
    if (drag && *drag) {
        const long value = std::strtol(drag, nullptr, 10);
        if (value >= 4 && value <= 400) dragPixels = static_cast<int>(value);
    }
    const char* steps = std::getenv("TC_HITBOX_DRAGSTEPS");
    if (steps && *steps) {
        const long value = std::strtol(steps, nullptr, 10);
        if (value >= 1 && value <= 32) dragSteps = static_cast<int>(value);
    }
    const char* measure = std::getenv("TC_HITBOX_MEASURE");
    if (measure && std::strcmp(measure, "move") == 0) measureMove = true;
    const char* reset = std::getenv("TC_HITBOX_RESET");
    if (reset && std::strcmp(reset, "1") == 0) resetEachSample = true;
    const char* undo = std::getenv("TC_HITBOX_UNDO");
    if (undo && std::strcmp(undo, "1") == 0) undoAfterMove = true;
    const char* alternate = std::getenv("TC_HITBOX_ALTERNATE");
    if (alternate && std::strcmp(alternate, "1") == 0) alternateDrag = true;
    const char* idle = std::getenv("TC_HITBOX_QUERY_IDLE");
    if (idle && std::strcmp(idle, "1") == 0) queryIdle = true;
    const char* post = std::getenv("TC_HITBOX_POST");
    if (post && std::strcmp(post, "refresh") == 0) refreshAfterPlace = true;
    if (post && std::strcmp(post, "upgrade") == 0) upgradeAfterPlace = true;
    const char* scanned = std::getenv("TC_HITBOX_TYPES");
    if (scanned && *scanned) {
        const long value = std::strtol(scanned, nullptr, 10);
        if (value >= 1 && value <= kTypeCount) scannedTypeCount = static_cast<int>(value);
    }
    const char* refresh = std::getenv("TC_HITBOX_REFRESH");
    if (refresh && std::strcmp(refresh, "1") == 0) refreshBetween = true;
    offsetCount = 0;
    for (int index = 0; index < kTypeCount; ++index)
        for (int row = 0; row < kMapHeight; ++row)
            for (int column = 0; column < kMapWidth; ++column)
                mapHits[index][row][column] = -1;
    /* TC_HITBOX_POINTS="dx,dy;dx,dy" samples just those offsets: a cheap way to
       reproduce one cell of the grid under the two-point case's conditions. */
    const char* list = std::getenv("TC_HITBOX_POINTS");
    if (list && *list) {
        const char* at = list;
        while (*at && offsetCount < 120) {
            int dx = 0, dy = 0;
            if (std::sscanf(at, "%d,%d", &dx, &dy) != 2) break;
            offsets[offsetCount++] = Offset{dx, dy};
            const char* next = std::strchr(at, ';');
            if (!next) break;
            at = next + 1;
        }
    } else {
        for (int y = kMapMinY; y <= kMapMaxY; ++y)
            for (int x = kMapMinX; x <= kMapMaxX; ++x)
                offsets[offsetCount++] = Offset{x, y};
    }
    /* Controls that say whether a sample can be trusted at all.  The selection
       set is currently read while the button is held, and a hit can survive the
       clears that bracket the next sample; the repeats and the far point below
       make that visible instead of turning it into a fake hit region. */
    offsets[offsetCount++] = Offset{kMapMinX, kMapMinY};   /* first grid point again */
    offsets[offsetCount++] = Offset{2, 0};                 /* the point the two-point test calls a hit */
    offsets[offsetCount++] = Offset{kMapMinX, kMapMinY};   /* and once more after that hit */
    offsets[offsetCount++] = Offset{40, 0};                /* far from every component on the board */
    offsets[offsetCount++] = Offset{40, 0};                /* ... twice in a row */
}

int stage = 0;
int typeIndex = 0;
int offsetIndex = 0;
int clickPhase = 0;
unsigned long long stageTick = 0;

/* The sandbox game window: the process's only visible window with a real
   client area.  Shared by the injection device and by the cleanliness check. */
HWND gameWindow() {
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

/* Moves the game window off the desktop and takes the focus away from it.

   Both halves matter.  Off the desktop so the real cursor can never be over it
   with a chance of being mistaken for the injected position; unfocused because
   the Win32 backend re-adds the operating system's cursor position every frame
   while the game is the foreground window, and that event is queued after the
   probe's and therefore wins.  Measured: with the window parked but focused,
   the game read mouse=(5658,163) - the user's own pointer, translated into the
   parked window - instead of the injected point. */
void parkGameWindow() {
    HWND window = gameWindow();
    if (!window) return;
    SetWindowPos(window, nullptr, kParkX, kParkY, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    /* Hand the foreground away instead of taking it: the desktop window is
       always available and never belongs to the game. */
    HWND shell = GetShellWindow();
    if (shell && shell != window) SetForegroundWindow(shell);
    windowParked = true;
}

bool cursorOverGameWindow() {
    HWND window = gameWindow();
    if (!window) return false;
    POINT pointer{};
    if (!GetCursorPos(&pointer)) return false;
    POINT corner{0, 0};
    if (!ClientToScreen(window, &corner)) return false;
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    return pointer.x >= corner.x && pointer.x < corner.x + client.right &&
           pointer.y >= corner.y && pointer.y < corner.y + client.bottom;
}

bool gameWindowIsForeground() {
    HWND window = gameWindow();
    return window && GetForegroundWindow() == window;
}

/* SetForegroundWindow silently fails when the calling thread does not own the
   foreground window, which is exactly the case for a sandbox game started by a
   script.  Attaching to the foreground thread first is the documented way
   around the foreground lock; without this the injected *real* button events
   (SendInput) go to whatever window does have the focus, while in-process ImGui
   events still arrive - which is the asymmetry the 2026-09-22 runs showed. */
void forceForeground(HWND window) {
    if (!window || GetForegroundWindow() == window) return;
    const DWORD targetThread = GetWindowThreadProcessId(window, nullptr);
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD thisThread = GetCurrentThreadId();
    if (foregroundThread && foregroundThread != thisThread)
        AttachThreadInput(thisThread, foregroundThread, TRUE);
    if (targetThread && targetThread != thisThread)
        AttachThreadInput(thisThread, targetThread, TRUE);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SetFocus(window);
    if (targetThread && targetThread != thisThread)
        AttachThreadInput(thisThread, targetThread, FALSE);
    if (foregroundThread && foregroundThread != thisThread)
        AttachThreadInput(thisThread, foregroundThread, FALSE);
}

void mouseAtScreenPoint(V2 point,V2 display,bool down) {
    if (injectSendInput) {
        /* Absolute coordinates are normalised to the primary monitor's virtual
           extent, which is what MOUSEEVENTF_ABSOLUTE expects. */
        const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        if (screenWidth > 0 && screenHeight > 0) {
            INPUT move{};
            move.type = INPUT_MOUSE;
            move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            move.mi.dx = static_cast<LONG>(point.x * 65535.0 /
                                          static_cast<double>(screenWidth - 1) + 0.5);
            move.mi.dy = static_cast<LONG>(point.y * 65535.0 /
                                          static_cast<double>(screenHeight - 1) + 0.5);
            SendInput(1, &move, sizeof(move));
        }
        if (down != injectedMouseDown) {
            INPUT button{};
            button.type = INPUT_MOUSE;
            button.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            SendInput(1, &button, sizeof(button));
            injectedMouseDown = down;
        }
        lastDisplayPoint = point;
        lastScreenPoint = POINT{LONG_MIN, LONG_MIN};
        return;
    }
    if (injectImgui) {
        lastDisplayPoint = point;
        lastScreenPoint = POINT{LONG_MIN, LONG_MIN};
        if (addMousePosEvent && igGetIO) addMousePosEvent(igGetIO(), point.x, point.y);
        if (addMouseButtonEvent && igGetIO) {
            addMouseButtonEvent(igGetIO(), 0, down);
            injectedMouseDown = down;
        }
        return;
    }
    HWND found = gameWindow();
    if (!found) return;
    /* ImGui reports framebuffer/game coordinates; Win32 messages use client
       coordinates, which differ under DPI virtualization on the test host. */
    forceForeground(found);
    static bool loggedForeground = false;
    if (!loggedForeground) {
        loggedForeground = true;
        report(std::string("hitbox: window foreground=") +
               (GetForegroundWindow() == found ? "1" : "0"));
    }
    RECT client{};GetClientRect(found,&client);
    const int clientX=static_cast<int>(std::lround(point.x*client.right/display.x));
    const int clientY=static_cast<int>(std::lround(point.y*client.bottom/display.y));
    POINT screen{clientX,clientY};
    ClientToScreen(found, &screen);
    if(down&&!cursorBorrowed){GetCursorPos(&savedCursor);cursorBorrowed=true;}
    SetCursorPos(screen.x, screen.y);
    lastScreenPoint=screen;
    lastDisplayPoint=point;
    if(down){
        /* A one pixel clip: the physical mouse cannot drag the cursor away from
           the sample point while the button the probe injected is held. */
        RECT pinned{screen.x,screen.y,screen.x+1,screen.y+1};
        ClipCursor(&pinned);
    }else{
        ClipCursor(nullptr);
        if(cursorBorrowed&&restoreCursor){SetCursorPos(savedCursor.x,savedCursor.y);cursorBorrowed=false;}
        else if(cursorBorrowed){cursorBorrowed=false;}
    }
    const LPARAM packed = MAKELPARAM(clientX,clientY);
    PostMessageW(found,WM_MOUSEMOVE,0,packed);
    /* Three button channels, because they reach different code paths:

         ImGui event      - what the toolbar, palette and panels read;
         posted WM_*      - what a board *panel* reads (see
                            tests/ui-board-panel-driver.hpp);
         injected real    - what anything polling the physical button state
                            reads (GetAsyncKeyState).

       Measured 2026-09-22: with ImGui events and posted messages alone, even a
       built-in AND gate ignored a press-and-drag on its body, while its output
       pin did react - the same pattern the custom types show.  A real injected
       press is the one channel that was missing. */
    if (down != injectedMouseDown) {
        PostMessageW(found, down ? WM_LBUTTONDOWN : WM_LBUTTONUP,
                     down ? MK_LBUTTON : 0, packed);
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        const UINT sent = SendInput(1, &input, sizeof(input));
        if (sent != 1) {
            static bool loggedFailure = false;
            if (!loggedFailure) {
                loggedFailure = true;
                report("hitbox: SendInput failed error=" + std::to_string(GetLastError()));
            }
        }
    }
    if(addMouseButtonEvent&&igGetIO){addMouseButtonEvent(igGetIO(),0,down);injectedMouseDown=down;}
}

void unload(void*){
    if(injectedMouseDown){
        HWND window=gameWindow();
        if(window)PostMessageW(window,WM_LBUTTONUP,0,0);
    }
    if(injectedMouseDown&&addMouseButtonEvent&&igGetIO)addMouseButtonEvent(igGetIO(),0,false);
    injectedMouseDown=false;
    ClipCursor(nullptr);
    if(cursorBorrowed){SetCursorPos(savedCursor.x,savedCursor.y);cursorBorrowed=false;}
    if(windowParked){
        HWND window=gameWindow();
        if(window)SetWindowPos(window,nullptr,0,0,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        windowParked=false;
    }
}

bool displaySize(V2* out) {
    if (!igGetIO || !out) return false;
    void* io = igGetIO();
    if (!io) return false;
    *out = *reinterpret_cast<V2*>(static_cast<unsigned char*>(io) + 8);
    return out->x > 8.f && out->y > 8.f;
}

V2 boardPointToScreen(int32_t x, int32_t y, const V2& display) {
    const V2 origin = worldToScreen({0.f, 0.f});
    const V2 unitX = worldToScreen({1.f, 0.f});
    const V2 unitY = worldToScreen({0.f, 1.f});
    const V2 o{origin.x * display.x, origin.y * display.y};
    const V2 ex{unitX.x * display.x - o.x, unitX.y * display.y - o.y};
    const V2 ey{unitY.x * display.x - o.x, unitY.y * display.y - o.y};
    return {o.x + static_cast<float>(x) * ex.x + static_cast<float>(y) * ey.x,
            o.y + static_cast<float>(x) * ex.y + static_cast<float>(y) * ey.y};
}

bool currentBoard(TCGameHandle* out) {
    return boardApi.get_current && out &&
           boardApi.get_current(boardApi.context, out) == TC_HANDLE_OK;
}

/* Which components the game says are selected right now. */
std::vector<uint64_t> selection() {
    std::vector<uint64_t> ids;
    if (!boardModel.valid()) return ids;
    const uint64_t count = boardModel.selectedComponentCount();
    for (uint64_t index = 0; index < count && index < 64; ++index)
        ids.push_back(boardModel.selectedComponentIdAt(index));
    return ids;
}

bool contains(const std::vector<uint64_t>& ids, uint64_t wanted) {
    for (uint64_t id : ids) if (id == wanted) return true;
    return false;
}

/* The loader's own board snapshot, which is the counter the other playtests
   assert on; comparing it with the selection set tells "the click did nothing"
   apart from "the set read is wrong". */
uint64_t snapshotSelectionCount() {
    TCGameHandle board{};
    if (!currentBoard(&board)) return 0xFFFFFFFFull;
    TCBoardApiV2 api{};
    if (tc::boardService(host, &api) != TC_SERVICE_OK) return 0xFFFFFFFEull;
    TCBoardSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = TC_BOARD_SNAPSHOT_VERSION_1;
    if (tc::captureBoardSnapshot(&api, &board, &snapshot) != TC_SNAPSHOT_OK) return 0xFFFFFFFDull;
    if (!(snapshot.flags & TC_BOARD_SNAPSHOT_HAS_SELECTION)) return 0xFFFFFFFCull;
    return snapshot.selected_component_count;
}

std::string levelName() {
    const void* address = host ? host->resolve_alias(host->context, "level.loaded") : nullptr;
    struct NimStringHeader {
        int64_t length;
        const char* data;
    };
    if (!address) return "?";
    const auto* header = static_cast<const NimStringHeader*>(address);
    if (header->length <= 0 || header->length > 512 || !header->data) return "<none>";
    return std::string(header->data, static_cast<size_t>(header->length));
}

void placeTypes() {
    TCGameHandle board{};
    if (!currentBoard(&board) || !commandApi.submit) return;
    if (stage == 0) stageTick = GetTickCount64();
    /* Appearance mode: place the requested built-in kinds in a spread-out grid
       and report where each one's record sits on screen. */
    if (!placeKinds.empty()) {
        int slot = 0;
        for (size_t index = 0; index < placeKinds.size(); ++index) {
            for (int rotation = 0; rotation < kindRotations; ++rotation, ++slot) {
            TCCommandV2 command{};
            command.size = sizeof(command);
            command.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
            command.subject = board;
            command.custom_prototype_id = 0;
            command.kind = placeKinds[index];
            /* The grid has to stay clear of the left palette (about 355 px, i.e.
               -36 units) and of the top bar; the defaults put 5 columns inside
               x -30..30. */
            const int columns = kindColumns > 0 ? kindColumns : 5;
            command.x = static_cast<int32_t>((slot % columns) * kindSpacingX -
                                             (columns - 1) * kindSpacingX / 2 + kindOriginX);
            command.y = static_cast<int32_t>((slot / columns) * kindSpacingY - 24 + kindOriginY);
            command.rotation = static_cast<uint32_t>(rotation);
            uint64_t request = 0;
            const int status = commandApi.submit(commandApi.context, &command, &request);
            V2 display{0.f, 0.f};
            const bool haveDisplay = displaySize(&display);
            const V2 origin = haveDisplay
                ? boardPointToScreen(command.x, command.y, display) : V2{0.f, 0.f};
            char line[192];
            std::snprintf(line, sizeof(line),
                          "hitbox: kind 0x%02x rot=%d at (%d,%d) screen=(%.0f,%.0f) status=%d",
                          static_cast<unsigned>(placeKinds[index]),
                          static_cast<int>(command.rotation), command.x, command.y,
                          static_cast<double>(origin.x), static_cast<double>(origin.y), status);
            report(line);
            }
        }
        report("hitbox: kinds placed");
        if (highlightFirstKind && !placeKinds.empty()) {
            V2 display{0.f, 0.f};
            if (displaySize(&display)) {
                const int columns = kindColumns > 0 ? kindColumns : 5;
                const int firstX = -(columns - 1) * kindSpacingX / 2 + kindOriginX;
                const V2 centre = boardPointToScreen(firstX, -24 + kindOriginY, display);
                mouseAtScreenPoint(centre, display, true);
                highlightCentre = centre;
                stage = 5;   /* drag, then release on later frames */
                report("hitbox: highlight press sent");
                return;
            }
        }
        report("hitbox: scan finished");
        stage = 3;
        return;
    }
    for (int index = 0; index < kTypeCount; ++index) {
        TCCommandV2 command{};
        command.size = sizeof(command);
        command.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
        command.subject = board;
        const bool builtin = types[index].id == 0;
        command.custom_prototype_id = builtin ? 0 : types[index].id;
        command.kind = builtin ? kBuiltinAndKind : 0x4e;
        if (builtin) {
            /* Away from every custom instance in both axes.  Two lessons went
               into this: a control below the row is covered by the selected
               component's panel, and one close enough to touch another
               component's collision box cannot be dragged at all (the user
               pointed that out).  16 units of clear board above the row. */
            command.x = 0;
            command.y = -16;
        } else {
            /* 30 units apart and all inside the camera: the 12x6 instance needs
               the room, and a placement that overlaps its neighbour would be
               refused (or worse, silently moved) by the game. */
            command.x = (index - 1) * 30 - 20;
            command.y = 0;
        }
        command.rotation = types[index].rotation;
        uint64_t request = 0;
        const int status = commandApi.submit(commandApi.context, &command, &request);
        report(std::string("hitbox: place ") + types[index].name + " at (" +
               std::to_string(command.x) + "," + std::to_string(command.y) + ") status=" +
               std::to_string(status));
    }
    stage = 1;
    stageTick = GetTickCount64();
    if (refreshAfterPlace && refreshBoard) {
        /* The refresh takes the board model, which is the same object the
           placement command used. */
        const void* raw = nullptr;
        if (host->resolve_game_handle &&
            host->resolve_game_handle(host->context, &board, &raw) == TC_HANDLE_OK && raw) {
            refreshBoard(const_cast<void*>(raw));
            report("hitbox: board refreshed after placement");
        } else {
            report("hitbox: board refresh skipped (no raw board)");
        }
    }
    if (upgradeAfterPlace) {
        if (presenterSlot && upgradeState) {
            upgradeState(presenterSlot, 0x30);
            report("hitbox: replayed the game's post-placement upgrade");
        } else {
            report("hitbox: post-placement upgrade skipped (presenter slot not seen yet)");
        }
    }
}

void findInstances() {
    TCGameHandle board{};
    if (!currentBoard(&board)) return;
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    TCBoardObjectBuffersV1 buffers{};
    buffers.size = sizeof(buffers);
    buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    int status = boardApi.capture_objects(boardApi.context, &board, &snapshot,
                                          static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    std::vector<TCGameHandle> probeWires;
    for (int attempt = 0; attempt < 4 && status == TC_SNAPSHOT_ERR_CAPACITY; ++attempt) {
        componentHandles.assign(static_cast<size_t>(snapshot.component_count), TCGameHandle{});
        /* Both buffers are required by the capacity check, wires included. */
        probeWires.assign(static_cast<size_t>(snapshot.wire_count), TCGameHandle{});
        buffers.components = componentHandles.data();
        buffers.component_capacity = componentHandles.size();
        buffers.wires = probeWires.data();
        buffers.wire_capacity = probeWires.size();
        status = boardApi.capture_objects(boardApi.context, &board, &snapshot,
                                          static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    }
    if (status != TC_SNAPSHOT_OK) return;
    for (uint64_t index = 0; index < snapshot.component_written; ++index) {
        TCComponentInfoV1 info{};
        info.size = sizeof(info);
        if (boardApi.read_component(boardApi.context, &componentHandles[index], &info,
                                    static_cast<uint32_t>(sizeof(info))) != TC_SNAPSHOT_OK)
            continue;
        const bool custom = info.kind == 0x4e &&
                            (info.flags & TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE) != 0;
        for (Type& type : types) {
            if (type.instance) continue;
            const bool matches = type.id ? (custom && info.custom_prototype_id == type.id)
                                         : (info.kind == kBuiltinAndKind && !custom);
            if (!matches) continue;
            type.instance = info.id;
            type.selectionId=index;
            type.x = info.x;
            type.y = info.y;
            report(std::string("hitbox: found ") + type.name + " instance=0x" +
                   std::to_string(type.instance) + " at (" + std::to_string(type.x) + "," +
                   std::to_string(type.y) + ")");
        }
    }
    bool ready = true;
    for (int index = 0; index < kTypeCount; ++index)
        if (!types[index].instance) ready = false;
    if (ready) {
        stage = 2;
        stageTick = GetTickCount64();
        typeIndex = 0;
        offsetIndex = 0;
        clickPhase = resetEachSample ? 6 : ((mapMode && hoverFirst) ? 5 : 0);
        if (injectImgui) {
            parkGameWindow();
            report("hitbox: injection=imgui the game window is parked off the desktop");
        }
        /* The live half extents, so a click log can be read against the box the
           game itself reports instead of the probe's assumption. */
        for (Type& type : types) {
            if (type.selectionId >= componentHandles.size()) continue;
            float halfWidth = 0.f, halfHeight = 0.f;
            const int status = tc::component_geometry::readFootprint(
                geometryApi, componentHandles[type.selectionId], &halfWidth, &halfHeight);
            type.halfWidth = halfWidth;
            type.halfHeight = halfHeight;
            char line[160];
            std::snprintf(line, sizeof(line), "hitbox: box %s half=(%.2f,%.2f) status=%d",
                          type.name, static_cast<double>(halfWidth),
                          static_cast<double>(halfHeight), status);
            report(line);
        }
        if (manualMode) {
            V2 display{0.f,0.f};
            if (displaySize(&display)) {
                for (const Type& type : types) {
                    const V2 centre = boardPointToScreen(type.x, type.y, display);
                    char line[192];
                    std::snprintf(line, sizeof(line),
                                  "hitbox: manual target %s board=(%d,%d) screen=(%.0f,%.0f)",
                                  type.name, type.x, type.y,
                                  static_cast<double>(centre.x), static_cast<double>(centre.y));
                    report(line);
                }
            }
            report("hitbox: manual ready - click the components with the real mouse");
        }
        if (placeOnly) {
            /* Where the game says each placed record is, and where the probe's
               own camera transform puts that point on screen.  Compare both
               against the frame captured with TC_MODLOADER_SHOT: the drawn
               figure is what the mouse has to be on, and this is the only way
               to see whether the two agree. */
            V2 display{0.f,0.f};
            if (displaySize(&display)) {
                for (const Type& type : types) {
                    const V2 origin = boardPointToScreen(type.x, type.y, display);
                    const V2 pin = boardPointToScreen(type.x + 2, type.y, display);
                    char line[224];
                    std::snprintf(line, sizeof(line),
                                  "hitbox: place-only %s board=(%d,%d) origin_screen=(%.0f,%.0f) pin_screen=(%.0f,%.0f)",
                                  type.name, type.x, type.y, static_cast<double>(origin.x),
                                  static_cast<double>(origin.y), static_cast<double>(pin.x),
                                  static_cast<double>(pin.y));
                    report(line);
                }
            }
            report("hitbox: place-only finished");
            report("hitbox: scan finished");
            stage = 3;
            return;
        }
        if (queryMode) {
            /* The board only builds its point -> component table while the mouse
               is over the board, so each sample hovers (position only, no
               buttons - so nothing is selected and no panel opens) and then
               asks the game itself which component is at that point. */
            const void* rawBoard = nullptr;
            const int resolveStatus = host->resolve_game_handle
                ? host->resolve_game_handle(host->context, &board, &rawBoard) : -999;
            if (resolveStatus != TC_HANDLE_OK || !rawBoard) {
                report("hitbox: query could not resolve the raw board object status=" +
                       std::to_string(resolveStatus));
                report("hitbox: scan finished");
                stage = 3;
                return;
            }
            if (!getComponentId) {
                report("hitbox: query has no get_component_id");
                report("hitbox: scan finished");
                stage = 3;
                return;
            }
            rawBoardForQuery = const_cast<void*>(rawBoard);
            report(std::string("hitbox: query board resolved; ") + types[0].name + " at (" +
                   std::to_string(types[0].x) + "," + std::to_string(types[0].y) + ") want=" +
                   std::to_string(types[0].selectionId));
            if (queryIdle) {
                report("hitbox: idle - move the mouse over the board; the game's own "
                       "answers are recorded as they change");
            }
            return;
        }
    }
}

/* A drag can carry the instance away from the coordinates it was sampled at,
   so the map re-reads the live position between samples and every offset stays
   relative to the component itself. */
bool livePosition(const Type& type, int32_t* x, int32_t* y) {
    if (!type.instance) return false;
    /* Resolve through a fresh snapshot rather than the handle taken when the
       instance was first found: a board edit (including the placement itself)
       can replace the object arrays, and the old handle then reads STALE, which
       silently looked like "the component did not move".  The instance id is
       stable across moves. */
    TCGameHandle board{};
    if (!currentBoard(&board)) return false;
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    TCBoardObjectBuffersV1 buffers{};
    buffers.size = sizeof(buffers);
    buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    std::vector<TCGameHandle> handles, wireHandles;
    /* The count from the capacity probe can be stale by the time the buffers are
       handed over (an edit between the two calls grows the board), so a
       CAPACITY answer is retried instead of being treated as a failure. */
    int status = TC_SNAPSHOT_ERR_CAPACITY;
    for (int attempt = 0; attempt < 4 && status == TC_SNAPSHOT_ERR_CAPACITY; ++attempt) {
        handles.assign(static_cast<size_t>(snapshot.component_count), TCGameHandle{});
        /* The capacity check covers components *and* wires: a drag can leave a
           wire behind, and without a wire buffer every later capture answers
           CAPACITY forever. */
        wireHandles.assign(static_cast<size_t>(snapshot.wire_count), TCGameHandle{});
        buffers.components = handles.data();
        buffers.component_capacity = handles.size();
        buffers.wires = wireHandles.data();
        buffers.wire_capacity = wireHandles.size();
        status = boardApi.capture_objects(boardApi.context, &board, &snapshot,
                                          static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    }
    if (status != TC_SNAPSHOT_OK) {
        /* Which capture failed matters: CAPACITY means the buffers were too
           small, everything else means the board itself could not be read. */
        static int reported = -12345;
        if (reported != status) {
            reported = status;
            report("hitbox: live position capture failed status=" + std::to_string(status));
        }
        return false;
    }
    int readOk = 0;
    for (uint64_t index = 0; index < snapshot.component_written; ++index) {
        TCComponentInfoV1 info{};
        info.size = sizeof(info);
        if (boardApi.read_component(boardApi.context, &handles[index], &info,
                                    static_cast<uint32_t>(sizeof(info))) != TC_SNAPSHOT_OK)
            continue;
        ++readOk;
        if (info.id != type.instance) continue;
        *x = info.x;
        *y = info.y;
        return true;
    }
    /* A move can hand the component a fresh instance id, so fall back to the
       sequence slot it was found in. */
    if (type.selectionId < snapshot.component_written) {
        TCComponentInfoV1 info{};
        info.size = sizeof(info);
        if (boardApi.read_component(boardApi.context, &handles[type.selectionId], &info,
                                    static_cast<uint32_t>(sizeof(info))) == TC_SNAPSHOT_OK) {
            *x = info.x;
            *y = info.y;
            return true;
        }
    }
    /* One line per distinct failure shape: "the snapshot had nothing readable"
       and "the instance is no longer in the snapshot" need different fixes. */
    static std::string reportedShape;
    const std::string shape = "written=" + std::to_string(snapshot.component_written) +
                              " count=" + std::to_string(snapshot.component_count) +
                              " readOk=" + std::to_string(readOk) +
                              " slot=" + std::to_string(type.selectionId);
    if (reportedShape != shape) {
        reportedShape = shape;
        report("hitbox: live position miss " + shape);
    }
    return false;
}

void refreshPosition(Type& type) {
    int32_t x = 0, y = 0;
    if (!livePosition(type, &x, &y)) return;
    type.x = x;
    type.y = y;
}

/* Screen (ImGui display space) -> board units, using the same camera transform
   the probe already uses forwards.  The game's own mapping is affine, so two
   basis vectors and an origin are enough. */
void screenToBoard(const V2& display, float screenX, float screenY, float* bx, float* by) {
    *bx = 0.f;
    *by = 0.f;
    if (!worldToScreen || display.x <= 0.f || display.y <= 0.f) return;
    const V2 origin = worldToScreen({0.f, 0.f});
    const V2 unitX = worldToScreen({1.f, 0.f});
    const V2 unitY = worldToScreen({0.f, 1.f});
    const double ox = origin.x * display.x, oy = origin.y * display.y;
    const double ax = unitX.x * display.x - ox, ay = unitX.y * display.y - oy;
    const double cx = unitY.x * display.x - ox, cy = unitY.y * display.y - oy;
    const double determinant = ax * cy - ay * cx;
    if (std::fabs(determinant) < 1e-9) return;
    const double px = screenX - ox, py = screenY - oy;
    *bx = static_cast<float>((px * cy - py * cx) / determinant);
    *by = static_cast<float>((ax * py - ay * px) / determinant);
}

/* One line per human press: where the game thinks the cursor is in board
   coordinates, which components are selected right then, and each placed type's
   offset from its own centre. */
/* Which of the probe's types the game currently has selected.  The selection
   set holds component sequence indices, which is what findInstances() recorded
   per type; the raw ids in the log are only useful next to that mapping. */
std::string selectedTypeNames() {
    const std::vector<uint64_t> selected = selection();
    std::string line;
    for (const Type& type : types) {
        if (!type.instance || !contains(selected, type.selectionId)) continue;
        if (!line.empty()) line += ",";
        line += type.name;
    }
    return line.empty() ? std::string("none") : line;
}

void manualTick() {
    static const unsigned long long logTick = 0;
    static unsigned long long lastLog = 0;
    (void)logTick;
    const unsigned long long now = GetTickCount64();
    const bool down = igIsMouseDown && igIsMouseDown(0);
    /* A heartbeat so a human can see whether the game is receiving the mouse at
       all: it prints where the cursor is in board units once a second. */
    if (now - lastLog > 1000) {
        lastLog = now;
        V2 display{0.f, 0.f};
        V2 mouse{-1.f, -1.f};
        if (displaySize(&display) && igGetMousePos) igGetMousePos(&mouse);
        float hx = 0.f, hy = 0.f;
        screenToBoard(display, mouse.x, mouse.y, &hx, &hy);
        char beat[192];
        std::snprintf(beat, sizeof(beat),
                      "hitbox: manual cursor board=(%.2f,%.2f) mouse=(%.0f,%.0f) down=%d",
                      static_cast<double>(hx), static_cast<double>(hy),
                      static_cast<double>(mouse.x), static_cast<double>(mouse.y), down ? 1 : 0);
        report(beat);
    }
    if (down && !manualWasDown) {
        V2 display{0.f, 0.f};
        V2 mouse{-1.f, -1.f};
        if (displaySize(&display) && igGetMousePos) igGetMousePos(&mouse);
        float bx = 0.f, by = 0.f;
        screenToBoard(display, mouse.x, mouse.y, &bx, &by);
        const std::vector<uint64_t> selected = selection();
        std::string line = "hitbox: manual press board=(" + std::to_string(bx) + "," +
                           std::to_string(by) + ") mouse=(" + std::to_string(mouse.x) + "," +
                           std::to_string(mouse.y) + ") selected=";
        for (size_t index = 0; index < selected.size() && index < 4; ++index)
            line += (index ? "," : "") + std::to_string(selected[index]);
        if (selected.empty()) line += "none";
        for (const Type& type : types) {
            if (!type.instance) continue;
            line += " " + std::string(type.name) + "_off=(" + std::to_string(bx - type.x) + "," +
                    std::to_string(by - type.y) + ")";
        }
        report(line);
        char summary[256];
        std::snprintf(summary, sizeof(summary),
                      "hitbox: manual summary presses=%d last_board=(%.2f,%.2f)",
                      ++manualPresses, static_cast<double>(bx), static_cast<double>(by));
        report(summary);
    }
    if (!down && manualWasDown) {
        V2 display{0.f, 0.f};
        V2 mouse{-1.f, -1.f};
        if (displaySize(&display) && igGetMousePos) igGetMousePos(&mouse);
        float bx = 0.f, by = 0.f;
        screenToBoard(display, mouse.x, mouse.y, &bx, &by);
        manualPending = true;
        manualPendingX = bx;
        manualPendingY = by;
        manualPendingAt = now;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "hitbox: manual release board=(%.2f,%.2f) selected=%s",
                      bx, by, selectedTypeNames().c_str());
        report(line);
    }
    /* The game may commit the selection on a later frame than the release, so
       the settled answer is reported separately instead of being missed. */
    if (manualPending && now - manualPendingAt < 600) {
        const std::string hit = selectedTypeNames();
        if (hit != "none") {
            char line[224];
            std::snprintf(line, sizeof(line),
                          "hitbox: manual settled board=(%.2f,%.2f) selected=%s",
                          manualPendingX, manualPendingY, hit.c_str());
            report(line);
            manualPending = false;
        }
    } else if (manualPending) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "hitbox: manual settled board=(%.2f,%.2f) selected=none",
                      manualPendingX, manualPendingY);
        report(line);
        manualPending = false;
    }
    manualWasDown = down;
}

void reportMap(int index) {
    char header[160];
    std::snprintf(header, sizeof(header), "hitbox: map %s dx=%d..%d dy=%d..%d",
                  types[index].name, kMapMinX, kMapMaxX, kMapMinY, kMapMaxY);
    report(header);
    for (int row = 0; row < kMapHeight; ++row) {
        char line[96];
        int at = std::snprintf(line, sizeof(line), "hitbox: maprow %s dy=%d ",
                               types[index].name, kMapMinY + row);
        for (int column = 0; column < kMapWidth; ++column){
            const int cell=mapHits[index][row][column];
            line[at++]=cell<0?'?':(cell?'1':'0');
        }
        line[at] = '\0';
        report(line);
    }
}

/* The declared footprint box, drawn where the probe's own camera transform puts
   it, plus the record point itself.  Cyan box = declared half extents; the small
   cross = the record position the game reports. */
void drawFootprintOverlay() {
    using namespace tc::ui;
    static int diagnostics = 0;
    const bool ready = drawingReady();
    if (!ready) {
        if (diagnostics++ == 0) report("hitbox: overlay skipped (drawing table not ready)");
        return;
    }
    /* This build's cimgui entry point takes the viewport (see
       examples/text-box/plugin.cpp): calling it with no argument hands AddRect
       a garbage list and crashes. */
    /* The board is drawn by the game's own renderer, and a background-list
       overlay stayed invisible under it; the foreground list is the one that
       lands on top. */
    void* viewport = igGetMainViewport ? igGetMainViewport() : nullptr;
    void* list = nullptr;
    if (igGetForegroundDrawList && viewport) list = igGetForegroundDrawList(viewport);
    if (!list && igGetBackgroundDrawList && viewport) list = igGetBackgroundDrawList(viewport);
    if (!list) {
        if (diagnostics++ == 0)
            report(std::string("hitbox: overlay skipped (no draw list; viewport=") +
                   (viewport ? "1" : "0") + ")");
        return;
    }
    V2 display{0.f, 0.f};
    if (!displaySize(&display)) {
        if (diagnostics++ == 0) report("hitbox: overlay skipped (no display size)");
        return;
    }
    if (diagnostics++ == 0)
        report("hitbox: overlay running; types with instance: " +
               std::to_string([&]{int n=0;for(const Type& t:types)if(t.instance)++n;return n;}()));
    /* Sanity marker: one translucent full-screen fill.  If this does not show up
       in the frame capture, the problem is the draw point, not the box maths. */
    if (std::getenv("TC_HITBOX_DRAW_TEST"))
        drawing_detail::table().rectFilled(list, Vec2{0.f, 0.f},
                                           Vec2{display.x, display.y},
                                           rgba(255, 0, 0, 60), 0.f, 0);
    for (Type& type : types) {
        if (!type.instance) continue;
        /* Half extents cached when the instance was found: a live re-read would
           need a handle that has been invalidated by the placement itself. */
        const float halfWidth = type.halfWidth;
        const float halfHeight = type.halfHeight;
        if (halfWidth <= 0.f && halfHeight <= 0.f) continue;
        /* Keep the fraction: truncating 0.5 to 0 turned a 1x1 box into a
           degenerate point that never showed up. */
        const V2 origin = boardPointToScreen(type.x, type.y, display);
        const V2 unitX = boardPointToScreen(type.x + 1, type.y, display);
        const V2 unitY = boardPointToScreen(type.x, type.y + 1, display);
        const float scaleX = unitX.x - origin.x, scaleY = unitY.y - origin.y;
        V2 a{origin.x - halfWidth * scaleX, origin.y - halfHeight * scaleY};
        V2 b{origin.x + halfWidth * scaleX, origin.y + halfHeight * scaleY};
        V2 low{std::min(a.x, b.x), std::min(a.y, b.y)};
        V2 high{std::max(a.x, b.x), std::max(a.y, b.y)};
        const Vec2 lowPoint{low.x, low.y};
        const Vec2 highPoint{high.x, high.y};
        {
            static std::string reported;
            const std::string key = std::string(type.name) + ":" + std::to_string(halfWidth) +
                                    "," + std::to_string(halfHeight);
            if (reported.find(key) == std::string::npos) {
                if (!reported.empty()) reported += ";";
                reported += key;
                char line[224];
                std::snprintf(line, sizeof(line),
                              "hitbox: overlay box %s half=(%.1f,%.1f) low=(%.0f,%.0f) high=(%.0f,%.0f)",
                              type.name, static_cast<double>(halfWidth),
                              static_cast<double>(halfHeight), static_cast<double>(low.x),
                              static_cast<double>(low.y), static_cast<double>(high.x),
                              static_cast<double>(high.y));
                report(line);
            }
        }
        drawing_detail::table().rectFilled(list, lowPoint, highPoint, rgba(0, 200, 255, 36), 0.f, 0);
        drawing_detail::table().rect(list, lowPoint, highPoint, rgba(0, 255, 255, 255), 0.f, 0, 2.f);
        drawing_detail::table().line(list, Vec2{origin.x - 6.f, origin.y},
                                     Vec2{origin.x + 6.f, origin.y}, rgba(255, 80, 80, 255), 2.f);
        drawing_detail::table().line(list, Vec2{origin.x, origin.y - 6.f},
                                     Vec2{origin.x, origin.y + 6.f}, rgba(255, 80, 80, 255), 2.f);
        char label[96];
        std::snprintf(label, sizeof(label), "%s half=%.1f,%.1f", type.name,
                      static_cast<double>(halfWidth), static_cast<double>(halfHeight));
        drawing_detail::table().text(list, Vec2{low.x + 2.f, low.y + 2.f},
                                     rgba(255, 255, 255, 255), label, nullptr);
    }
}

/* Hover one sampled point with no button held, then ask the game which
   component it puts there.  This is the game's own answer, taken from the same
   table its click handler reads - nothing is injected except the pointer
   position, so a sample cannot latch, and no panel can be involved. */
void queryTick() {
    V2 display{0.f, 0.f};
    if (!displaySize(&display) || !rawBoardForQuery || !getComponentId) return;
    if (queryIdle) return;   /* the human is driving; the hook does the recording */
    if (typeIndex >= scannedTypeCount) {
        report("hitbox: query finished");
        report("hitbox: scan finished");
        stage = 3;
        return;
    }
    Type& type = types[typeIndex];
    if (offsetIndex >= offsetCount) {
        ++typeIndex;
        offsetIndex = 0;
        clickPhase = 0;
        stageTick = GetTickCount64();
        return;
    }
    const Offset& offset = offsets[offsetIndex];
    const int x = type.x + offset.x;
    const int y = type.y + offset.y;
    if (clickPhase == 0) {
        const V2 point = boardPointToScreen(x, y, display);
        mouseAtScreenPoint(point, display, false);
        clickPhase = 1;
        stageTick = GetTickCount64();
        return;
    }
    const int32_t packed = static_cast<int32_t>(
        (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16) |
        static_cast<uint16_t>(x));
    const int64_t id = getComponentId(rawBoardForQuery, packed);
    char line[224];
    std::snprintf(line, sizeof(line),
                  "hitbox: query %s offset=(%d,%d) board=(%d,%d) component=%lld want=%lld",
                  type.name, offset.x, offset.y, x, y, static_cast<long long>(id),
                  static_cast<long long>(type.selectionId));
    report(line);
    ++offsetIndex;
    clickPhase = 0;
    stageTick = GetTickCount64();
}

void scanHitArea() {
    V2 display{0.f, 0.f};
    if (!displaySize(&display)) return;
    if (typeIndex >= scannedTypeCount) {
        report("hitbox: dirty samples=" + std::to_string(dirtySamples));
        report("hitbox: scan finished");
        stage = 3;
        return;
    }
    Type& type = types[typeIndex];
    if (offsetIndex >= offsetCount) {
        if (mapMode) reportMap(typeIndex);
        ++typeIndex;
        offsetIndex = 0;
        clickPhase = resetEachSample ? 6 : ((mapMode && hoverFirst) ? 5 : 0);
        stageTick = GetTickCount64();
        return;
    }
    const int dx = offsets[offsetIndex].x;
    const int dy = offsets[offsetIndex].y;
    const V2 point = boardPointToScreen(type.x + dx, type.y + dy, display);
    /* A far, empty board point: the reset click lands there, never on a
       component, so it clears the selection without selecting anything. */
    const V2 emptyPoint = boardPointToScreen(type.x + 40, type.y + 8, display);
    if (resetEachSample && clickPhase == 6) {
        if (clearSelections) clearSelections();
        mouseAtScreenPoint(emptyPoint, display, true);
        clickPhase = 7;
        stageTick = GetTickCount64();
        return;
    }
    if (resetEachSample && clickPhase == 7) {
        mouseAtScreenPoint(emptyPoint, display, false);
        /* The game answers the release on a later frame, so the settled reading
           gets a phase of its own; reading it here would always show the state
           from before the click. */
        clickPhase = 8;
        stageTick = GetTickCount64();
        return;
    }
    if (resetEachSample && clickPhase == 8) {
        preselected = static_cast<int>(selection().size());
        /* Leave the reset and continue with the target press; going back to 6
           would loop between the two reset phases forever. */
        clickPhase = (mapMode && hoverFirst) ? 5 : 0;
        if (mapMode) {
            char line[160];
            std::snprintf(line, sizeof(line), "hitbox: reset %s after=(%d,%d) presel=%d",
                          type.name, dx, dy, preselected);
            report(line);
        }
        stageTick = GetTickCount64();
        return;
    }
    /* The map hovers the target for a whole frame before pressing.  Without
       that pre-move the game keeps answering about the previous press, which is
       how a point 40 units away can report the component as selected. */
    if(clickPhase==5){
        if(mapMode&&hoverFirst)mouseAtScreenPoint(point,display,false);
        clickPhase=0;stageTick=GetTickCount64();return;
    }
    if(clickPhase==3){
        if(clearSelections)clearSelections();
        if(clearComponentSet&&selectedComponentsRaw)clearComponentSet(selectedComponentsRaw);
        if(clearComponentSet&&previousSelectedComponentsRaw)clearComponentSet(previousSelectedComponentsRaw);
        if(mapMode){
            char cleared[160];
            std::snprintf(cleared, sizeof(cleared),
                          "hitbox: cleared %s after=(%d,%d) selected=%llu", type.name, dx, dy,
                          static_cast<unsigned long long>(selection().size()));
            report(cleared);
        }
        if(mapMode&&refreshBetween)refreshPosition(type);
        clickPhase=4;stageTick=GetTickCount64();return;
    }
    if(clickPhase==4){
        clickPhase=(mapMode&&hoverFirst)?5:0;stageTick=GetTickCount64();return;
    }
    /* Keep DOWN visible for a whole game frame. */
    if(clickPhase==0){
        if(clearSelections)clearSelections();
        if(measureMove){
            int32_t x = 0, y = 0;
            if(livePosition(type, &x, &y)){type.beforeX = x; type.beforeY = y;}
            else {type.beforeX = type.x; type.beforeY = type.y;}
        }
        mouseAtScreenPoint(point,display,true);clickPhase=1;stageTick=GetTickCount64();return;
    }
    if(clickPhase==1){
        /* Both cases observe the target held past the drag threshold: a bare
           press (no movement) selects nothing anywhere, which is a property of
           the game's gesture, not of the geometry.  The map re-reads the live
           position between samples so the drag cannot drift it.  The move is
           split over several frames because the game ignores a single jump. */
        ++dragStep;
        const float fraction=static_cast<float>(dragStep)/static_cast<float>(dragSteps);
        const float direction=alternateDrag&&(offsetIndex%2)?-1.f:1.f;
        V2 dragged{point.x+direction*static_cast<float>(dragPixels)*fraction,point.y};
        mouseAtScreenPoint(dragged,display,true);
        if(dragStep>=dragSteps){dragStep=0;clickPhase=2;}
        stageTick=GetTickCount64();return;
    }
    /* phase 2 is observed while the target is held past the drag threshold,
       which is when the game has already selected it. */
    const std::vector<uint64_t> selected = selection();
    bool hit = contains(selected, type.selectionId);
    int32_t movedX = 0, movedY = 0;
    int positionRead = -1;
    if (measureMove) {
        /* The press either grabbed this component (and the drag carried it) or
           it did not.  Unlike the selection set this cannot latch: it is a
           difference between two readings of the same instance. */
        int32_t x = 0, y = 0;
        positionRead = livePosition(type, &x, &y) ? 1 : 0;
        if (positionRead) {
            movedX = x - type.beforeX;
            movedY = y - type.beforeY;
            type.x = x;
            type.y = y;
        }
        hit = movedX != 0 || movedY != 0;
    }
    V2 mouse{-1.f,-1.f};if(igGetMousePos)igGetMousePos(&mouse);
    POINT pointer{};GetCursorPos(&pointer);
    const bool pointerOk=std::abs(pointer.x-lastScreenPoint.x)<=1&&
                         std::abs(pointer.y-lastScreenPoint.y)<=1;
    const bool positionOk=std::abs(mouse.x-lastDisplayPoint.x)<1.5f&&
                          std::abs(mouse.y-lastDisplayPoint.y)<1.5f;
    /* With ImGui-only injection the probe never placed the real pointer, so
       "the cursor is still where we put it" is not the question.  What has to
       hold instead is that the game's ImGui still reports the position the
       probe wrote, and that the real cursor is nowhere over the game window:
       if it were, the backend's own update would be indistinguishable from the
       injected one and the sample could not be attributed. */
    const bool clean = injectImgui ? (positionOk && !cursorOverGameWindow())
                                   : (injectSendInput ? positionOk : (pointerOk && positionOk));
    if(!clean){
        ++dirtySamples;
        char dirty[224];
        if (injectImgui)
            std::snprintf(dirty, sizeof(dirty),
                          "hitbox: dirty %s offset=(%d,%d) device=imgui cursorOverWindow=%d mouse=(%.0f,%.0f) wanted=(%.0f,%.0f)",
                          type.name, dx, dy, cursorOverGameWindow() ? 1 : 0,
                          static_cast<double>(mouse.x), static_cast<double>(mouse.y),
                          static_cast<double>(lastDisplayPoint.x),
                          static_cast<double>(lastDisplayPoint.y));
        else
            std::snprintf(dirty, sizeof(dirty),
                          "hitbox: dirty %s offset=(%d,%d) pointer=(%ld,%ld) wanted=(%ld,%ld) mouse=(%.0f,%.0f) wanted=(%.0f,%.0f)",
                          type.name, dx, dy, pointer.x, pointer.y, lastScreenPoint.x, lastScreenPoint.y,
                          static_cast<double>(mouse.x), static_cast<double>(mouse.y),
                          static_cast<double>(lastDisplayPoint.x), static_cast<double>(lastDisplayPoint.y));
        report(dirty);
    }
    /* Only grid points fill the map, and a repeated grid point keeps its first
       reading so a later control cannot rewrite it.  Contaminated samples are
       dropped instead of being averaged into the map. */
    if (mapMode && clean && dx >= kMapMinX && dx <= kMapMaxX && dy >= kMapMinY && dy <= kMapMaxY &&
        mapHits[typeIndex][dy - kMapMinY][dx - kMapMinX] < 0)
        mapHits[typeIndex][dy - kMapMinY][dx - kMapMinX] = hit ? 1 : 0;
    char line[352];
    std::snprintf(line, sizeof(line),
                  "hitbox: %s offset=(%d,%d) screen=(%.0f,%.0f) mouse=(%.0f,%.0f) clean=%d down=%d clicked=%d selected=%d presel=%d moved=(%d,%d) posread=%d at=(%d,%d) key=%llu want=%llu snapshot=%llu",
                  type.name, dx, dy, static_cast<double>(point.x), static_cast<double>(point.y),
                  static_cast<double>(mouse.x),static_cast<double>(mouse.y),
                  clean?1:0,
                  igIsMouseDown&&igIsMouseDown(0)?1:0,
                  igIsMouseClicked&&igIsMouseClicked(0,false)?1:0,
                  hit ? 1 : 0,
                  preselected,
                  movedX, movedY, positionRead, type.x, type.y,
                  static_cast<unsigned long long>(selected.empty()?UINT64_MAX:selected[0]),
                  static_cast<unsigned long long>(type.selectionId),
                  static_cast<unsigned long long>(snapshotSelectionCount()));
    report(line);
    mouseAtScreenPoint(point,display,false);
    if (measureMove && undoAfterMove && (movedX != 0 || movedY != 0) && undoBoard) {
        /* Put the component back where the sample found it: the game's own undo
           removes exactly the move this gesture just made. */
        TCGameHandle board{};
        const void* rawBoard = nullptr;
        if (currentBoard(&board) && host->resolve_game_handle &&
            host->resolve_game_handle(host->context, &board, &rawBoard) == TC_HANDLE_OK && rawBoard) {
            undoBoard(const_cast<void*>(rawBoard));
            int32_t x = 0, y = 0;
            if (livePosition(type, &x, &y)) { type.x = x; type.y = y; }
        }
    }
    ++offsetIndex;
    clickPhase=3;
    stageTick = GetTickCount64();
}

void frame(void*, const TCFrame*) {
    if (!host) return;
    const unsigned long long now = GetTickCount64();
    /* Releasing the highlight probe's press: one frame after the press, so the
       game sees a click rather than a folded-away down/up pair. */
    if (stage == 5) {
        V2 display{0.f, 0.f};
        if (displaySize(&display)) {
            /* The game only commits a selection once the press has moved a few
               pixels, which is why a bare click selects nothing. */
            mouseAtScreenPoint(V2{highlightCentre.x + 8.f, highlightCentre.y}, display, true);
            stage = 6;
            report("hitbox: highlight drag sent");
            return;
        }
    }
    if (stage == 6) {
        V2 display{0.f, 0.f};
        if (displaySize(&display))
            mouseAtScreenPoint(highlightCentre, display, false);
        report("hitbox: highlight release sent");
        report("hitbox: scan finished");
        stage = 3;
        return;
    }
    /* Same place the text-box example draws from: on_frame with the background
       draw list is a supported overlay point. */
    if (drawFootprints) drawFootprintOverlay();
    /* The game re-centres its own window when a level finishes loading, so the
       parking is re-asserted while a scan runs; a window that drifted back onto
       the desktop could let the real cursor into a sample. */
    static unsigned long long lastPark = 0;
    if (injectImgui && stage == 2 && now - lastPark > 1000) {
        lastPark = now;
        parkGameWindow();
    }
    if (manualMode) {
        /* Same placement stage as the automated cases, then hand over to the
           human: no synthetic input, no selection clearing. */
        TCGameHandle board{};
        if (!currentBoard(&board)) return;
        if (stage == 0) { if (now - stageTick > 1200) placeTypes(); return; }
        if (stage == 1) { if (now - stageTick > 1500) { findInstances(); return; } }
        if (stage == 2) manualTick();
        return;
    }
    if (stage == 3) return;
    /* A level has to be up before anything can be placed. */
    TCGameHandle board{};
    if (!currentBoard(&board)) return;
    if (stage == 0 && now - stageTick > 1200) {
        placeTypes();
        return;
    }
    if (stage == 1 && now - stageTick > 1500) {
        static bool loggedBoard = false;
        if (!loggedBoard) {
            loggedBoard = true;
            const std::string level = levelName();
            report("hitbox: board level=" + level +
                   " snapshotSelection=" + std::to_string(snapshotSelectionCount()));
        }
        findInstances();
        return;
    }
    /* The map takes a click (two phases) plus two settling phases per point, so
       it shortens the per-phase wait to keep the whole grid inside one run. */
    const unsigned long long tick = mapMode ? mapTick : 350;
    if (stage == 2 && now - stageTick > tick) {
        if (queryMode) { queryTick(); return; }
        scanHitArea();
        return;
    }
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    buildOffsets();
    {
        const char* restore = std::getenv("TC_HITBOX_RESTORE");
        restoreCursor = restore && std::strcmp(restore, "1") == 0;
    }
    if (!boardModel.load(h)) report("hitbox: the selection model is unavailable");
    if (tc::boardService(h, &boardApi) != TC_SERVICE_OK) return 3;
    tc::commandService(h, &commandApi);
    worldToScreen = nullptr;
    void* transform = h->resolve_alias(h->context, "board.world_to_screen");
    std::memcpy(&worldToScreen, &transform, sizeof(worldToScreen));
    void* io = h->engine_proc(h->context, "igGetIO");
    std::memcpy(&igGetIO, &io, sizeof(igGetIO));
    void* mouse = h->engine_proc(h->context,"igGetMousePos");
    std::memcpy(&igGetMousePos,&mouse,sizeof(igGetMousePos));
    void* mouseDown=h->engine_proc(h->context,"igIsMouseDown_Nil");
    std::memcpy(&igIsMouseDown,&mouseDown,sizeof(igIsMouseDown));
    void* mouseClicked=h->engine_proc(h->context,"igIsMouseClicked_Bool");
    std::memcpy(&igIsMouseClicked,&mouseClicked,sizeof(igIsMouseClicked));
    void* addButton=h->engine_proc(h->context,"ImGuiIO_AddMouseButtonEvent");
    std::memcpy(&addMouseButtonEvent,&addButton,sizeof(addMouseButtonEvent));
    void* addPosition=h->engine_proc(h->context,"ImGuiIO_AddMousePosEvent");
    std::memcpy(&addMousePosEvent,&addPosition,sizeof(addMousePosEvent));
    if (drawFootprints) {
        /* The board overlay is drawn on ImGui's background list, which is what a
           place-only run needs: nothing is injected, the boxes just appear where
           the probe's camera transform says they are. */
        const bool uiReady = tc::ui::load(h);
        const bool drawReady = uiReady && tc::ui::loadDrawing(h);
        void* background = h->engine_proc(h->context, "igGetBackgroundDrawList");
        std::memcpy(&igGetBackgroundDrawList, &background, sizeof(igGetBackgroundDrawList));
        void* viewport = h->engine_proc(h->context, "igGetMainViewport");
        std::memcpy(&igGetMainViewport, &viewport, sizeof(igGetMainViewport));
        void* foreground = h->engine_proc(h->context, "igGetForegroundDrawList_ViewportPtr");
        std::memcpy(&igGetForegroundDrawList, &foreground, sizeof(igGetForegroundDrawList));
        report(std::string("hitbox: footprint overlay ui=") + (uiReady ? "1" : "0") +
               " drawing=" + (drawReady ? "1" : "0") +
               " list=" + (igGetBackgroundDrawList ? "1" : "0") +
               " viewport=" + (igGetMainViewport ? "1" : "0") +
               " foreground=" + (igGetForegroundDrawList ? "1" : "0"));
    }
    if (injectImgui && !addMousePosEvent) {
        report("hitbox: ImGuiIO_AddMousePosEvent is missing; falling back to the real cursor");
        injectImgui = false;
    }
    void* clear=h->resolve_symbol(h->context,"clear_selections__modelZboardZboard_u8323");
    std::memcpy(&clearSelections,&clear,sizeof(clearSelections));
    void* lookup=h->resolve_symbol(h->context,
        "get_component_id__presenterZutilitiesZhelper95functions_u2052");
    std::memcpy(&getComponentId,&lookup,sizeof(getComponentId));
    if (queryMode && !getComponentId) report("hitbox: get_component_id did not resolve");
    void* refresh=h->resolve_symbol(h->context,
        "sim_stop_and_refresh__modelZsimulationZcompile95thread_u3043");
    std::memcpy(&refreshBoard,&refresh,sizeof(refreshBoard));
    if (refreshAfterPlace && !refreshBoard) report("hitbox: sim_stop_and_refresh did not resolve");
    if (upgradeAfterPlace) {
        void* upgrade=h->resolve_symbol(h->context,
            "upgrade__presenterZcontext_u2766");
        if (upgrade && h->create_hook) {
            const int hooked=h->create_hook(h->context, upgrade,
                                           reinterpret_cast<void*>(&hookUpgrade),
                                           reinterpret_cast<void**>(&upgradeState));
            report("hitbox: upgrade hook=" + std::to_string(hooked));
        } else {
            report("hitbox: upgrade did not resolve");
        }
    }
    if (undoAfterMove) {
        void* undo=h->resolve_symbol(h->context,
            "undo_board__presenterZutilitiesZhelper95functions_u8367");
        std::memcpy(&undoBoard,&undo,sizeof(undoBoard));
        if(!undoBoard) report("hitbox: board undo did not resolve");
    }
    if (queryMode && getComponentId && h->create_hook) {
        const int hooked = h->create_hook(h->context, reinterpret_cast<void*>(getComponentId),
                                         reinterpret_cast<void*>(&hookGetComponentId),
                                         reinterpret_cast<void**>(&getComponentIdOriginal));
        report("hitbox: hover hook=" + std::to_string(hooked) +
               (hooked == 0 ? " (the game's own lookups are recorded now)" : " (rejected)"));
    }
    void* clearSet=h->resolve_symbol(h->context,"clear__modelZboardZboard_u8371");
    std::memcpy(&clearComponentSet,&clearSet,sizeof(clearComponentSet));
    selectedComponentsRaw=h->resolve_symbol(h->context,"selected_components__modelZboardZboard_u22");
    previousSelectedComponentsRaw=h->resolve_symbol(h->context,"prev_selected_components__modelZboardZboard_u41");
    if(!tc::component_geometry::table(h,&geometryApi))return 4;
    static const TCComponentPin output{"out",1};
    int registeredTypes = 0;
    for (Type& type : types) {
        /* The built-in control already exists in the game; the probe only places
           it, so it is neither registered nor given a footprint. */
        if (type.id == 0) continue;
        TCNativeComponentDefinition definition{};
        definition.size=sizeof(definition);definition.custom_id=type.id;
        definition.name=type.name;definition.description="M5 hit-area probe";
        definition.output_count=type.pinCount;
        definition.outputs=type.pinCount?&output:nullptr;
        definition.gate_cost=1;definition.delay=1;definition.callback=&logic;
        const int registered=h->register_component(h->context,&definition);
        if(registered!=0){
            report(std::string("hitbox: ")+type.name+" registration="+std::to_string(registered));
            return 5;
        }
        if(type.expanded){
            const int geometry=tc::component_geometry::setFootprint(geometryApi,type.id,6.f,3.f);
            if(geometry!=TC_COMPONENT_GEOMETRY_OK){
                report(std::string("hitbox: ")+type.name+" geometry="+std::to_string(geometry));
                return 6;
            }
        }
        ++registeredTypes;
    }
    report("hitbox: " + std::to_string(registeredTypes) + " types registered (plus one built-in "
           "AND control); scan=" + (mapMode ? "map" : "points") +
           " points=" + std::to_string(offsetCount) + " device=" +
           (injectImgui ? "imgui" : (manualMode ? "manual" : "cursor")) +
           "; waiting for a board");
    out->on_frame = &frame;
    out->on_unload=&unload;
    return 0;
}
