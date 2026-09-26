/* Test-only driver for the punch tape's runtime wide constants.

   Three questions have to be answered on a real level, and only a driver inside
   the game can answer them:

     1. does the update sequence the tape's click handler runs stay cheap, and
        does it leave the game's compile counter alone (no recompile)?
     2. does a real mouse click on the tape's own squares reach that handler?
     3. does the circuit actually compute with the new value - the generated
        refresh program stores the constant at a #SIMULATION_STATE offset, so the
        driver reads that offset back through the simulation service.

   The tape reports its canvas origin in screen space (the drawer's own window
   position is not published anywhere); the driver turns one bit into a client
   coordinate from that line, the display ratio the loader logs, and the same
   cell arithmetic the panel uses.

   Everything it learns is logged with the PUNCHDRV: prefix; the playtest parses
   that out of tc-modloader-data/loader.log.  Writes are test-only and land in a
   throwaway sandbox copy of the game. */
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_handle_api.h"
#include <windows.h>
#include <stdint.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static const TCHost* host;
using SelectFn = void (*)(int64_t index, void* record);
using ClearSelectionsFn = void (*)();
using AddComponentFn = bool (*)(void* board, void* component);
using SetSettingFn = void (*)(void* board, int64_t setting, int64_t component, int64_t value);
using StopAndRefreshFn = void (*)(void* board);
using SetDynamicConstantFn = void (*)(uint64_t component, uint64_t value);
using UpgradeContextFn = void (*)(void* contextState, uint8_t state);

static SelectFn selectComponent;
static ClearSelectionsFn clearSelections;
static AddComponentFn addComponent;
static SetSettingFn setSetting;
static StopAndRefreshFn stopAndRefresh;
static SetDynamicConstantFn setDynamicConstant;
static UpgradeContextFn upgradeContext;
static uint64_t* currentWordSize;
static TCSimulationApiV1 simulationApi{};

static constexpr uint64_t kComponentStride = 0x238;
static constexpr uint64_t kRecordHeader = 8;
static constexpr uint64_t kSettingCountOffset = 0xa8;
static constexpr uint64_t kSettingsOffset = 0xb0;
static constexpr uint64_t kValueWidthOffset = 0xe0;
static constexpr uint64_t kValueWidthMirrorOffset = 0xe8;
static constexpr uint8_t kConstantKind = 0x2e;

static std::string gameDirectory; /* narrow, with a trailing backslash */
static std::string logPath;

struct BoardView {
    void* board = nullptr;
    uint8_t* data = nullptr;
    uint64_t count = 0;
};

/* The tape's layout, as the panel reported it (pixel sizes in hundredths). */
struct Geometry {
    int index = -1;
    int originX = 0, originY = 0;
    int cell = 0, cellGap = 0, groupGap = 0, rowGap = 0;
    int groups = 1, width = 0;
    std::string context; /* the drawer's presenter context, hex, as logged */
    bool valid = false;
};

static void say(const std::string& message) {
    if (host && host->log) host->log(host->context, ("PUNCHDRV: " + message).c_str());
}

static std::string readText(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static size_t countOccurrences(const std::string& text, const std::string& needle) {
    size_t total = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++total;
    return total;
}

static std::string lastLineWith(const std::string& text, const std::string& needle) {
    const size_t at = text.rfind(needle);
    if (at == std::string::npos) return std::string();
    const size_t start = text.rfind('\n', at);
    const size_t startAt = start == std::string::npos ? 0 : start + 1;
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    return text.substr(startAt, end - startAt);
}

static bool boardView(BoardView* view) {
    TCGameHandle handle{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &handle) != TC_HANDLE_OK) return false;
    const void* raw = nullptr;
    if (tc::resolveGameHandle(host, &handle, &raw) != TC_HANDLE_OK || !raw) return false;
    auto* bytes = const_cast<uint8_t*>(static_cast<const uint8_t*>(raw));
    memcpy(&view->count, bytes + 0x78, sizeof(view->count));
    memcpy(&view->data, bytes + 0x80, sizeof(view->data));
    if (!view->data || view->count == 0 || view->count > 1000000) return false;
    view->board = bytes;
    return true;
}

static int64_t settingCount(const uint8_t* component) {
    int64_t count = 0;
    memcpy(&count, component + kSettingCountOffset, sizeof(count));
    return (count > 0 && count < 1024) ? count : 0;
}

static bool settingValue(const uint8_t* component, int64_t* out) {
    if (settingCount(component) <= 0) return false;
    const uint8_t* array = nullptr;
    memcpy(&array, component + kSettingsOffset, sizeof(array));
    if (!array) return false;
    memcpy(out, array + 8, sizeof(*out));
    return true;
}

static uint64_t recordedWidth(const uint8_t* component) {
    uint64_t own = 0, mirror = 0;
    memcpy(&own, component + kValueWidthOffset, sizeof(own));
    memcpy(&mirror, component + kValueWidthMirrorOffset, sizeof(mirror));
    if (own >= 1 && own <= 64) return own;
    if (mirror >= 1 && mirror <= 64) return mirror;
    if (currentWordSize && *currentWordSize >= 1 && *currentWordSize <= 64) return *currentWordSize;
    return 0;
}

/* Files the loader writes for every compile it sees: two per board pass. */
static uint64_t countSourceDumps() {
    WIN32_FIND_DATAA found{};
    const HANDLE search = FindFirstFileA((gameDirectory + "native-logic-source*.txt").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return 0;
    uint64_t total = 0;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++total;
    } while (FindNextFileA(search, &found));
    FindClose(search);
    return total;
}

/* Where the generated refresh program keeps this constant.  The code generator
   emits one store per wide constant, so reading that offset through the
   simulation service is the game's own state read, not a buffer guess. */
static bool stateOffsetFor(const std::string& componentId, uint64_t* offset) {
    const std::string needle = "tc_dynamic_constant'(U64 " + componentId + ",";
    /* A big board's generated source is tens of megabytes; only re-read it when
       the newest dump actually changed. */
    static std::string cachedFile, cachedNeedle, cachedText;
    static uint64_t cachedSize = 0;
    WIN32_FIND_DATAA found{};
    const HANDLE search = FindFirstFileA((gameDirectory + "native-logic-source*.txt").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    std::string newest;
    uint64_t newestSize = 0;
    FILETIME newestTime{};
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (CompareFileTime(&found.ftLastWriteTime, &newestTime) > 0) {
            newestTime = found.ftLastWriteTime;
            newest = found.cFileName;
            newestSize = (static_cast<uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
        }
    } while (FindNextFileA(search, &found));
    FindClose(search);
    if (newest.empty()) return false;
    if (newest != cachedFile || newestSize != cachedSize || needle != cachedNeedle) {
        cachedFile = newest;
        cachedNeedle = needle;
        cachedText = readText(gameDirectory + newest);
        cachedSize = newestSize;
    }
    const std::string& text = cachedText;
    const size_t at = text.find(needle);
    if (at == std::string::npos) return false;
    const size_t rawStart = text.rfind('\n', at);
    const size_t lineStart = rawStart == std::string::npos ? 0 : rawStart + 1;
    const size_t equals = text.find(" = ", lineStart);
    if (equals == std::string::npos || equals > at) return false;
    std::string variable = text.substr(lineStart, equals - lineStart);
    const size_t space = variable.find_last_of(" \t");
    if (space != std::string::npos) variable.erase(0, space + 1);
    for (size_t scan = at; (scan = text.find("store(#SIMULATION_STATE + ", scan)) != std::string::npos;) {
        size_t end = text.find('\n', scan);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(scan, end - scan);
        if (line.find(variable) != std::string::npos) {
            unsigned long long parsed = 0;
            if (std::sscanf(line.c_str(), "store(#SIMULATION_STATE + %llu", &parsed) == 1) {
                *offset = static_cast<uint64_t>(parsed);
                return true;
            }
        }
        scan = end;
    }
    return false;
}

static bool readState(uint64_t offset, uint32_t bits, uint64_t* value) {
    return tc::readSimulationValue(&simulationApi, offset, bits, value) == TC_SIMULATION_OK;
}

static bool parseGeometry(const std::string& line, Geometry* geometry) {
    const size_t at = line.find("punch tape: geometry drawer");
    if (at == std::string::npos) return false;
    int index = 0, originX = 0, originY = 0;
    int cell = 0, cellGap = 0, groupGap = 0, rowGap = 0, groups = 0, width = 0;
    if (std::sscanf(line.c_str() + at,
                    "punch tape: geometry drawer%d origin=%d,%d cell=%d gap=%d group=%d row=%d groups=%d width=%d",
                    &index, &originX, &originY, &cell, &cellGap, &groupGap, &rowGap, &groups, &width) != 9)
        return false;
    geometry->index = index;
    geometry->originX = originX;
    geometry->originY = originY;
    geometry->cell = cell;
    geometry->cellGap = cellGap;
    geometry->groupGap = groupGap;
    geometry->rowGap = rowGap;
    geometry->groups = groups > 0 ? groups : 1;
    geometry->width = width;
    const size_t at2 = line.find(" context=", at);
    if (at2 != std::string::npos) {
        const size_t start = at2 + 9;
        size_t end = start;
        while (end < line.size() && line[end] != ' ' && line[end] != '\r') ++end;
        geometry->context = line.substr(start, end - start);
    }
    geometry->valid = true;
    return true;
}

/* The game's ImGui surface can be larger than its window; the loader logs both
   and the ratio between them, which is what a PostMessage click needs. */
static float displayRatio() {
    const std::string line = lastLineWith(readText(logPath), "Display: dpi-awareness=");
    const size_t at = line.find("ratio=");
    if (at == std::string::npos) return 1.f;
    float x = 1.f, y = 1.f;
    if (std::sscanf(line.c_str() + at, "ratio=%f,%f", &x, &y) != 2) return 1.f;
    return x > 0.f ? x : 1.f;
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

static HWND gameWindow() {
    HWND found = nullptr;
    EnumWindows(windowCallback, reinterpret_cast<LPARAM>(&found));
    return found;
}

static float rowWidthOf(int bits, float cell, float gap, float groupGap) {
    if (bits <= 0) return 0.f;
    const int groups = (bits + 7) / 8;
    const float step = cell + gap;
    const float groupWidth = 8.f * step - gap;
    return static_cast<float>(groups) * groupWidth + static_cast<float>(groups - 1) * groupGap;
}

/* Same arithmetic as the panel's cellPositionForBit: bits run high to low
   inside a row, low chunks sit at the bottom. */
static bool cellCentreForBit(int bit, const Geometry& geometry, float* outX, float* outY) {
    const float cell = geometry.cell / 100.f, gap = geometry.cellGap / 100.f;
    const float groupGap = geometry.groupGap / 100.f, rowGap = geometry.rowGap / 100.f;
    const int bitsPerRow = geometry.groups * 8;
    const int width = geometry.width;
    if (bit < 0 || bit >= width || bitsPerRow <= 0) return false;
    const int row = bit / bitsPerRow;
    const int firstBit = row * bitsPerRow;
    const int remaining = width - firstBit;
    const int rowBits = remaining < bitsPerRow ? remaining : bitsPerRow;
    const int within = rowBits - 1 - (bit - firstBit);
    const int group = within / 8, index = within % 8;
    const float step = cell + gap;
    const float groupWidth = 8.f * step - gap;
    const float rowOffset = (rowWidthOf(width < bitsPerRow ? width : bitsPerRow, cell, gap, groupGap) -
                             rowWidthOf(rowBits, cell, gap, groupGap)) * 0.5f;
    *outX = static_cast<float>(geometry.originX) + rowOffset +
            static_cast<float>(group) * (groupWidth + groupGap) +
            static_cast<float>(index) * step + cell * 0.5f;
    *outY = static_cast<float>(geometry.originY) + static_cast<float>(row) * (cell + rowGap) +
            cell * 0.5f;
    return true;
}

static double nowMilliseconds() {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return frequency.QuadPart ? counter.QuadPart * 1000.0 / frequency.QuadPart : 0.0;
}

/* A real click, posted the way tests/ui-keyboard-driver.hpp posts one. */
static bool clickSurfacePoint(float surfaceX, float surfaceY) {
    HWND window = gameWindow();
    if (!window) return false;
    const float ratio = displayRatio();
    const int clientX = static_cast<int>(surfaceX * ratio);
    const int clientY = static_cast<int>(surfaceY * ratio);
    POINT screen{clientX, clientY};
    ClientToScreen(window, &screen);
    SetCursorPos(screen.x, screen.y);
    Sleep(30);
    const LPARAM point = MAKELPARAM(clientX, clientY);
    PostMessageW(window, WM_MOUSEMOVE, 0, point);
    Sleep(40);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
    Sleep(25);
    PostMessageW(window, WM_LBUTTONUP, 0, point);
    return true;
}

static bool done;
static int frames;
static int stage;
static int stageFrames;
static int placeDelay;
static bool reselected;
static bool finished;
static bool recompileAsked;
static uint64_t constantIndex;
static uint64_t constantId;
static uint64_t constantWidth;
static Geometry geometry;

/* The component-description builder moves its presenter context to this state
   right after a native setting commit (0x1a3b8 into that context, state 0x30);
   measured while this feature was built, see docs/HANDOFF-punch-tape-runtime-constants.md. */
static constexpr uint64_t kContextStateOffset = 0x1a3b8;
static constexpr uint8_t kSettingsChangedState = 0x30;

/* The level's own loader (the byte-adder example in autotest mode) announces the
   load, then compiles two seconds later.  A test copy of a wide constant placed
   inside that window is part of that compile; placed afterwards it would sit on
   the board unused, and nothing would recompile the schematic. */
static bool autotestLoadedLevel() {
    return readText(logPath).find("byte-adder: autotest loaded level") != std::string::npos;
}

static void runUpdateSequence(BoardView& view, uint64_t index, uint64_t id, uint64_t mask) {
    const uint64_t dumpsBefore = countSourceDumps();
    say("dumps before=" + std::to_string(dumpsBefore));
    for (int step = 0; step < 3; ++step) {
        const uint64_t pattern = mask & (0x5A5A5A5A5A5A5A5AULL >> (8 * step));
        const double started = nowMilliseconds();
        if (setSetting) setSetting(view.board, 0, static_cast<int64_t>(index), static_cast<int64_t>(pattern));
        if (setDynamicConstant) setDynamicConstant(id, pattern);
        if (stopAndRefresh) stopAndRefresh(view.board);
        const double elapsed = nowMilliseconds() - started;
        char line[192];
        std::snprintf(line, sizeof(line), "sequence %d value=%llu took=%.3f ms", step,
                      static_cast<unsigned long long>(pattern), elapsed);
        say(line);
    }
    Sleep(1200);
    say("dumps after=" + std::to_string(countSourceDumps()));
}

/* The circuit's own copy of the value: the generated refresh program stores the
   constant at a #SIMULATION_STATE offset, and this reads it back through the
   game's own state reader.  A runtime slot that never reaches the circuit would
   still show the new number in the drawer, so this is the check that separates
   "the UI changed" from "the simulation changed". */
static void runStateCheck(const std::string& componentId, uint64_t width, uint64_t expected) {
    uint64_t offset = 0;
    if (!stateOffsetFor(componentId, &offset)) {
        say("no compiled source carries the constant; the circuit was not read back");
        return;
    }
    uint64_t value = 0;
    if (!readState(offset, static_cast<uint32_t>(width), &value)) {
        say("state read at " + std::to_string(offset) + " failed");
        return;
    }
    say("state at " + std::to_string(offset) + " = " + std::to_string(value) +
        " expected " + std::to_string(expected) +
        (value == expected ? " MATCH" : " MISMATCH"));
}

/* Finds a wide constant on the board; returns its index, id and width. */
static bool findWideConstant(BoardView& view, uint64_t* index, uint64_t* id, uint64_t* width) {
    for (uint64_t at = 0; at < view.count; ++at) {
        uint8_t* component = view.data + kRecordHeader + at * kComponentStride;
        if (component[0] != kConstantKind) continue;
        const uint64_t size = recordedWidth(component);
        if (size <= 8 || size > 64) continue;
        int64_t value = 0;
        if (!settingValue(component, &value)) continue;
        uint64_t componentId = 0;
        memcpy(&componentId, component + 8, sizeof(componentId));
        *index = at;
        *id = componentId;
        *width = size;
        return true;
    }
    return false;
}

static bool placeWideConstant(BoardView& view) {
    if (!addComponent) return false;
    std::array<uint8_t, 0x238> component{};
    component[0] = kConstantKind;
    const int16_t x = 0, y = 5;
    memcpy(component.data() + 2, &x, sizeof(x));
    memcpy(component.data() + 4, &y, sizeof(y));
    const uint64_t one = 1, capacity = 0x100;
    memcpy(component.data() + 0x58, &one, sizeof(one));
    memcpy(component.data() + 0x60, &capacity, sizeof(capacity));
    component[0x68] = 1;
    memcpy(component.data() + 0x70, &one, sizeof(one));
    memcpy(component.data() + 0x78, &capacity, sizeof(capacity));
    component[0x80] = 1;
    if (currentWordSize) *currentWordSize = 64;
    const bool placed = addComponent(view.board, component.data());
    say(placed ? "placed a 64-bit constant" : "constant placement failed");
    return placed;
}

static void selectConstant(BoardView& view, uint64_t index) {
    uint8_t* component = view.data + kRecordHeader + index * kComponentStride;
    /* The array index, which is what the game's own mouse path ends in. */
    if (clearSelections) clearSelections();
    if (selectComponent) selectComponent(static_cast<int64_t>(index), component);
}

/* Opening the drawer needs one more step than selecting: the bottom panel draws
   the component description only in that mode, and it reads the component it
   describes from the context.  Measured in build_bottom_panel (0x1403cb3db:
   the mode byte at +0xe90, non-zero draws the description panel) and at the call
   itself, where the fourth argument - the index the description uses - is
   context+0xe48.  Rewritten every frame the driver wants the drawer up, in case
   the game's own UI state machine puts it back. */
static void openDrawer(BoardView& view, uint64_t index) {
    if (!view.board) return;
    auto* context = static_cast<uint8_t*>(view.board);
    memcpy(context + 0xe48, &index, sizeof(index));
    context[0xe90] = 1;
}

/* Asks the drawer's own presenter context for the state transition the game's
   value field uses after a settings commit.  Only called once the tape has
   reported that context and it is the same object this driver already holds as
   the Board handle, so the offset cannot point outside it. */
static void requestRecompile(BoardView& view) {
    if (!upgradeContext || !view.board) return;
    auto* state = static_cast<uint8_t*>(view.board) + kContextStateOffset;
    upgradeContext(state, kSettingsChangedState);
    if (stopAndRefresh) stopAndRefresh(view.board);
    say("asked the presenter to recompile the current board");
}

/* Real clicks on the tape's own squares, one bit each, with the round trip
   measured from the click to the panel reporting the new value. */
static void runRealClicks(uint64_t id, uint64_t width) {
    Geometry geometry;
    if (!parseGeometry(lastLineWith(readText(logPath), "punch tape: geometry drawer"), &geometry)) {
        say("no tape geometry in the log; real clicks skipped");
        return;
    }
    say("geometry origin=" + std::to_string(geometry.originX) + "," + std::to_string(geometry.originY) +
        " cell=" + std::to_string(geometry.cell) + " groups=" + std::to_string(geometry.groups) +
        " width=" + std::to_string(geometry.width) + " ratio=" + std::to_string(displayRatio()));
    uint64_t offset = 0;
    const bool haveOffset = stateOffsetFor(std::to_string(id), &offset);
    say(std::string("state offset ") + (haveOffset ? std::to_string(offset) : std::string("not found")));

    const int bits[] = {3, 12, 29};
    const uint64_t dumpsBefore = countSourceDumps();
    for (int step = 0; step < 3; ++step) {
        const int bit = bits[step];
        if (bit >= static_cast<int>(geometry.width)) break;
        float x = 0.f, y = 0.f;
        if (!cellCentreForBit(bit, geometry, &x, &y)) continue;
        const size_t clicksBefore = countOccurrences(readText(logPath), "punch tape: drawer component #");
        const double started = nowMilliseconds();
        if (!clickSurfacePoint(x, y)) {
            say("no game window; real clicks skipped");
            return;
        }
        std::string line;
        double elapsed = -1.0;
        for (int poll = 0; poll < 400; ++poll) {
            const std::string text = readText(logPath);
            if (countOccurrences(text, "punch tape: drawer component #") > clicksBefore) {
                line = lastLineWith(text, "punch tape: drawer component #");
                elapsed = nowMilliseconds() - started;
                break;
            }
            Sleep(5);
        }
        if (elapsed < 0.0) {
            say("realclick bit=" + std::to_string(bit) + ": the tape never reported a click");
            continue;
        }
        int reportedIndex = -1, reportedBit = -1;
        long long reportedValue = 0;
        const size_t at = line.find("punch tape: drawer component #");
        std::sscanf(line.c_str() + (at == std::string::npos ? 0 : at),
                    "punch tape: drawer component #%d bit %d -> value %lld",
                    &reportedIndex, &reportedBit, &reportedValue);
        char summary[256];
        std::snprintf(summary, sizeof(summary),
                      "realclick bit=%d roundtrip=%.1f ms panel index=%d bit=%d value=%lld",
                      bit, elapsed, reportedIndex, reportedBit, reportedValue);
        say(summary);
        if (haveOffset) {
            uint64_t computed = 0;
            if (readState(offset, static_cast<uint32_t>(width), &computed))
                say("state at " + std::to_string(offset) + " = " + std::to_string(computed));
            else
                say("state read at " + std::to_string(offset) + " failed");
        }
        Sleep(300);
    }
    Sleep(600);
    say("realclick dumps before=" + std::to_string(dumpsBefore) +
        " after=" + std::to_string(countSourceDumps()));
}

static void frame(void*, const TCFrame*) {
    if (done) return;
    if (++frames < 240) return;
    BoardView view;
    if (!boardView(&view)) return;
    switch (stage) {
        case 0: {
            uint64_t index = 0, id = 0, width = 0;
            if (findWideConstant(view, &index, &id, &width)) {
                /* The level brought its own wide constant: nothing to place. */
                constantIndex = index;
                constantId = id;
                constantWidth = width;
                say("found constant index=" + std::to_string(index) + " id=" +
                    std::to_string(id) + " width=" + std::to_string(width) + " (level's own)");
                stage = 1;
                stageFrames = 0;
                break;
            }
            /* The level loader compiles what it just loaded; placing inside that
               pass would be overwritten, so wait it out first. */
            if (!autotestLoadedLevel() && frames < 1200) break;
            if (placeDelay++ < 240) break;
            placeWideConstant(view);
            stage = 1;
            stageFrames = 0;
            break;
        }
        case 1: {
            uint64_t index = 0, id = 0, width = 0;
            if (!findWideConstant(view, &index, &id, &width)) {
                if (stageFrames++ > 600) say("warning: no wide constant appeared on the board");
                break;
            }
            constantIndex = index;
            constantId = id;
            constantWidth = width;
            int64_t value = 0;
            settingValue(view.data + kRecordHeader + index * kComponentStride, &value);
            say("constant index=" + std::to_string(index) + " id=" + std::to_string(id) +
                " width=" + std::to_string(width) + " value=" + std::to_string(value));
            /* The screen path the player uses: select the component so the game's
               drawer opens on it, which is where the tape lives. */
            selectConstant(view, index);
            openDrawer(view, index);
            say("selected constant index=" + std::to_string(index));
            stage = 2;
            stageFrames = 0;
            break;
        }
        case 2: {
            openDrawer(view, constantIndex);
            const std::string line = lastLineWith(readText(logPath), "punch tape: geometry drawer");
            if (parseGeometry(line, &geometry)) {
                char mine[32]{};
                std::snprintf(mine, sizeof(mine), "%llx",
                              static_cast<unsigned long long>(
                                  reinterpret_cast<uintptr_t>(view.board)));
                say("geometry origin=" + std::to_string(geometry.originX) + "," +
                    std::to_string(geometry.originY) + " cell=" + std::to_string(geometry.cell) +
                    " groups=" + std::to_string(geometry.groups) +
                    " width=" + std::to_string(geometry.width));
                say(std::string("board pointer=") + mine + " drawer context=" + geometry.context);
                if (geometry.context == mine) say("the drawer's context is the Board handle");
                stage = 3;
                stageFrames = 0;
            } else if (stageFrames++ > 900) {
                say("warning: the tape never logged its geometry");
                stage = 3;
                stageFrames = 0;
            }
            break;
        }
        case 3: {
            if (!recompileAsked) {
                recompileAsked = true;
                requestRecompile(view);
            }
            uint64_t offset = 0;
            if (stateOffsetFor(std::to_string(constantId), &offset)) {
                say("compiled source carries component " + std::to_string(constantId) +
                    " at state offset " + std::to_string(offset));
                stage = 4;
                stageFrames = 0;
            } else if (stageFrames++ > 1500) {
                say("warning: no generated source carries component " + std::to_string(constantId));
                stage = 4;
                stageFrames = 0;
            }
            break;
        }
        default: {
            /* Keep the drawer up: the screenshot is taken after the measurements
               are done, and the drawer's mode lives in the game's own state. */
            openDrawer(view, constantIndex);
            if (!finished) {
                if (stageFrames++ < 60) break; /* a moment with the drawer up */
                finished = true;
                if (!reselected) {
                    /* A recompile rebuilds the drawer; select again so the tape is
                       up while the clicks are posted. */
                    selectConstant(view, constantIndex);
                    reselected = true;
                }
                const uint64_t mask = constantWidth >= 64 ? ~uint64_t{0}
                                                          : ((uint64_t{1} << constantWidth) - 1);
                runUpdateSequence(view, constantIndex, constantId, mask);
                runStateCheck(std::to_string(constantId), constantWidth,
                              mask & (0x5A5A5A5A5A5A5A5AULL >> 16));
                runRealClicks(constantId, constantWidth);
            }
            break;
        }
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!h->resolve_symbol || !h->log) return 2;
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, 32768)) return 3;
    std::wstring directory(path);
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return 4;
    directory.resize(slash + 1);
    for (wchar_t character : directory) gameDirectory.push_back(static_cast<char>(character & 0x7f));
    logPath = gameDirectory + "tc-modloader-data\\loader.log";
    selectComponent = reinterpret_cast<SelectFn>(h->resolve_symbol(
        h->context, "select_component__modelZboardZboard_u9202"));
    clearSelections = reinterpret_cast<ClearSelectionsFn>(h->resolve_symbol(
        h->context, "clear_selections__modelZboardZboard_u8323"));
    addComponent = reinterpret_cast<AddComponentFn>(h->resolve_symbol(
        h->context, "add_component__presenterZutilitiesZhelper95functions_u5918"));
    setSetting = reinterpret_cast<SetSettingFn>(h->resolve_symbol(
        h->context, "set_setting__presenterZutilitiesZhelper95functions_u2763"));
    stopAndRefresh = reinterpret_cast<StopAndRefreshFn>(h->resolve_symbol(
        h->context, "sim_stop_and_refresh__modelZsimulationZcompile95thread_u3043"));
    upgradeContext = reinterpret_cast<UpgradeContextFn>(h->resolve_symbol(
        h->context, "upgrade__presenterZcontext_u2766"));
    currentWordSize = static_cast<uint64_t*>(h->resolve_symbol(
        h->context, "current_word_size__modelZmodel95types_u741"));
    tc::simulationService(h, &simulationApi);
    HMODULE loader = GetModuleHandleW(L"game_engine.dll");
    FARPROC proc = loader ? GetProcAddress(loader, "tc_dynamic_constant_set") : nullptr;
    memcpy(&setDynamicConstant, &proc, sizeof(setDynamicConstant));
    if (!selectComponent || !clearSelections || !setSetting || !stopAndRefresh) return 5;
    say(std::string("armed; runtime setter ") + (setDynamicConstant ? "present" : "missing") +
        "; simulation service " + (simulationApi.context ? "ready" : "missing"));
    out->on_frame = frame;
    return 0;
}
