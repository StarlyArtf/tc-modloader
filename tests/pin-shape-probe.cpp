/* Development-only probe: does a component with no inputs (a source) or no
   outputs (a sink) survive the game's importer and its scheduler?

   The loader's declarative path now *encodes* those scaffolds
   (src/component_definition.hpp) and the bridge accepts them
   (src/native_logic.hpp), but the game is the one who decides: it may refuse a
   definition without input pins, and it may drop a driver whose input or output
   is not connected to anything.  This probe registers one shape at a time with a
   callback that logs every invocation, so the loader log says which of the two
   happened - see docs/research/custom-component-pins.md.

   The level runner is the byte-adder example's (load a level, compile, run and
   judge); only the registration is replaced. */
#define tc_mod_load byte_adder_example_load
#include "../examples/byte-adder/plugin.cpp"
#undef tc_mod_load

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "jit-code-dump.hpp"

#include "../sdk/tc_component_types.h"
#include "../sdk/tc_component_instances.h"
#include "../sdk/tc_component_storage.h"
#include "../sdk/tc_component_geometry.h"
#include "../sdk/tc_component_render.h"
#include "../sdk/tc_command_api.h"

/* ---------------------------------------------------------------------------
   Wide definitions: how many pins will the game's importer actually take?

   Callback inputs now follow the game's measured 32-pin boundary through staged
   payloads.  A definition with more pins is still built here byte by
   byte - the same v14 layout src/component_definition.hpp writes, minus its
   eight-pin guard - and imported with the mod's own component model, so the
   answer is the importer's, not ours.  See docs/research/custom-component-pins.md.
   ------------------------------------------------------------------------- */
namespace wide {
struct Writer {
    std::vector<uint8_t> bytes;
    void number(uint64_t value, unsigned size) {
        for (unsigned i = 0; i < size; ++i) bytes.push_back(uint8_t(value >> (i * 8)));
    }
    void string(const char* text) {
        const size_t length = std::strlen(text);
        number(length, 2);
        bytes.insert(bytes.end(), text, text + length);
    }
    void component(unsigned kind, int x, int y, uint64_t id, const char* name, unsigned bits,
                   int ordinal) {
        number(kind, 2); number(x, 2); number(y, 2); number(0, 1); number(id, 8);
        string(name); number(ordinal ? 1 : 0, 2);
        if (ordinal) number(kind == 0x4f ? 2 : 0, 8);
        number(0, 8); number(-2 * ordinal, 2); number(bits, 8);
        number(0, 1); number(UINT64_MAX, 8); number(0, 8); number(0, 1);
        number(0, 1); number(0, 2); number(0, 2);
    }
};
inline std::vector<uint8_t> literals(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out{14};   /* component definition save format */
    uint64_t remaining = raw.size();
    do {
        uint8_t byte = uint8_t(remaining & 0x7f);
        remaining >>= 7;
        out.push_back(uint8_t(byte | (remaining ? 0x80 : 0)));
    } while (remaining);
    for (size_t at = 0; at < raw.size();) {
        const size_t chunk = raw.size() - at < 60 ? raw.size() - at : 60;
        out.push_back(uint8_t((chunk - 1) << 2));
        out.insert(out.end(), raw.begin() + at, raw.begin() + at + chunk);
        at += chunk;
    }
    return out;
}
/* n one-bit inputs, one one-bit output.  Same shape as the loader's scaffold:
   n collectors, n-1 dependency gates, one driver, and the output pin. */
inline std::vector<uint8_t> definition(unsigned n, uint64_t id) {
    Writer writer;
    writer.number(id, 8); writer.number(0, 4); writer.number(1, 8); writer.number(1, 8);
    writer.number(1, 1); writer.number(10000, 8); writer.number(0, 2); writer.string("");
    writer.number(0, 1); writer.number(0, 2); writer.number(0, 2); writer.string("");
    for (int i = 0; i < 512; ++i) writer.number(0, 1);
    /* n input pins, n collectors, n-1 dependency gates, one driver, one pin. */
    writer.number(3 * n + 1, 8);
    auto inputY = [n](unsigned i) { return int(i) * 8 - int(n - 1) * 4; };
    for (unsigned i = 0; i < n; ++i)
        writer.component(0x4f, -18, inputY(i), 0x1000 + i, "", 1, int(i) + 1);
    for (unsigned i = 0; i < n; ++i)
        writer.component(0x12, -6, inputY(i), 0x2000 + i, "", 1, 0);
    for (unsigned i = 1; i < n; ++i)
        writer.component(0x17, 40 + int(i) * 12, 100 + int(i) * 8, 0x3000 + i, "", 64, 0);
    writer.component(0x12, 4, 0, 0x4000, "", 1, 0);
    writer.component(0x51, 13, 0, 0x5000, "", 1, 1);
    /* The same wire list the loader's scaffold writes, so a refusal can only be
       about the pin count and not about a wire this probe got wrong. */
    Writer wires;
    unsigned wireCount = 0;
    auto point = [&](int x, int y) { wires.number(x, 2); wires.number(y, 2); };
    auto step = [&](int x, int y) {
        if (x) wires.number(unsigned((x > 0 ? 0 : 0x8000) | std::abs(x)), 2);
        if (y) wires.number(unsigned((y > 0 ? 0x4000 : 0xc000) | std::abs(y)), 2);
    };
    for (unsigned i = 0; i < n; ++i) {                    /* input pin -> collector */
        ++wireCount;
        wires.number(0, 1); wires.string("");
        point(-15, inputY(i)); step(8, 0); wires.number(0, 2);
    }
    int previousX = -4, previousY = inputY(0);
    for (unsigned i = 1; i < n; ++i) {                    /* dependency chain */
        const int x = 40 + int(i) * 12, y = 100 + int(i) * 8;
        ++wireCount;
        wires.number(0, 1); wires.string("");
        point(previousX, previousY); step(x - 3 - previousX, 0); step(0, y - 1 - previousY);
        step(2, 0); wires.number(0, 2);
        ++wireCount;
        wires.number(0, 1); wires.string("");
        point(-4, inputY(i)); step(x - 5 + 4, 0); step(0, y + 1 - inputY(i)); step(2, 0);
        wires.number(0, 2);
        previousX = x + 2; previousY = y;
    }
    ++wireCount;                                          /* chain -> driver input */
    wires.number(0, 1); wires.string("");
    point(previousX, previousY);
    step(4, 0);
    step(0, -80 - previousY);
    step(3 - (previousX + 4), 0);
    step(0, 80);
    wires.number(0, 2);
    ++wireCount;                                          /* driver -> output pin */
    wires.number(0, 1); wires.string("");
    point(6, 0); step(4, 0); wires.number(0, 2);
    writer.number(wireCount, 8);
    writer.bytes.insert(writer.bytes.end(), wires.bytes.begin(), wires.bytes.end());
    return literals(writer.bytes);
}
}  // namespace wide

static TCHost probeHost;
static int (*realRegister)(void*, const TCNativeComponentDefinition*);

constexpr uint64_t kSourceId = 0x535243305F303031ULL;  /* "SRC0_001" */
constexpr uint64_t kSinkId = 0x534E4B305F303031ULL;    /* "SNK0_001" */
constexpr uint64_t kWide9Id = 0x574944395F303031ULL;   /* "WID9_001" */
constexpr uint64_t kWide2048Id = 0x57323034385F3031ULL; /* "W2048_01" */
constexpr uint64_t kWideSourceId = 0x5736345352433031ULL; /* "W64SRC01" */

static std::atomic<int> sourceCalls{0};
static std::atomic<int> sinkCalls{0};
static std::atomic<int> wide9Calls{0};
static std::atomic<int> wide2048Calls{0};
static std::atomic<int> wide9Rotations{0};
static std::atomic<int> wide9Moves{0};
static std::atomic<int> wide9Resizes{0};
static std::atomic<int64_t> sourceLastCycle{-2};
static std::atomic<int64_t> sinkLastCycle{-2};
static std::atomic<int64_t> wide9LastCycle{-2};
static std::atomic<int64_t> wide2048LastCycle{-2};
static void (*wrappedFrame)(void*, const TCFrame*) = nullptr;
static bool tookOver = false;
static int64_t probeTarget = 0;
static bool benchmarkUncapped = false;
static bool benchmarkSubmitted = false;

/* Once a second: where the level runner actually is.  "The callback ran once
   and then nothing" reads very differently from "the simulation never left
   cycle 0", and only the runner's own numbers can tell them apart. */
/* The instance walk (definitions further down, after registerWide9): watchFrame
   drives it, so the state and the helpers are declared here. */
using AddComponent = bool (*)(void*, void*);
using DeleteComponent = void (*)(void*, int64_t);
static AddComponent addComponent;
static DeleteComponent deleteComponent;
static int instancesStage;
static double instancesStageAt;
static TCComponentInstanceHandle firstHandle;
static TCComponentInstanceHandle secondHandle;
static tc::component_instances::Api instances;
static bool instancesReady;
static tc::component_storage::Api componentStorage;
static bool componentStorageReady;
static bool instancesReported;
static std::vector<unsigned char> wide9Placement(int16_t x, int16_t y);
static std::string describeInstances(tc::component_instances::Api& api,
                                     tc::component_instances::Handle* kept, int keep);
static int wide9BoardIndex(int ordinal);

/* ---- placement lifecycle: the input the probe drives ----------------------

   TC_LOGIC_MOVE is the player's own drag, so the probe does what a player does:
   it finds the game window, asks the game where a cell is on screen and drags
   the component.  TC_LOGIC_ROTATE depends on whatever input this build uses to
   rotate a component; the probe tries the candidates in order and the log says
   which one the game acted on (the poll only fires when the record's direction
   byte really changed). */
struct ProbePoint { float x, y; };
using ProbeWorldToScreen = ProbePoint (*)(ProbePoint);
static ProbeWorldToScreen probeWorldToScreen = nullptr;
static HWND probeWindow = nullptr;
static float probeScaleX = 1.f, probeScaleY = 1.f;
static int probeClientWidth = 0, probeClientHeight = 0;
static bool probeInputArmed = false;
static int probeMovePhase = 0;
static uint64_t probeMoveInstance = 0;
static tc::component_geometry::ApiV2 probeGeometryV2{};
static bool probeGeometryReady = false;
static bool probeResizePending = false;
static int probeResizeStatus = -99;
static int probeResizeCount = 0;
/* Where the second instance is on screen, taken from its own render frame: the
   frame the host hands a Mod already carries the board-to-screen transform, so
   the drag does not have to re-derive it. */
static float probeSecondX = 0.f, probeSecondY = 0.f, probeSecondUnit = 0.f;
static bool probeSecondSeen = false;
static float probeMoveStartX = 0.f, probeMoveStartY = 0.f;
static bool probeStopRequested = false;
/* The board's own input path reads ImGui's IO, which is also what the hitbox
   probes inject into; posted window messages move the pointer but are not what
   the board's drag logic samples. */
static void* (*probeGetIo)() = nullptr;
static void (*probeAddMousePos)(void*, float, float) = nullptr;
static void (*probeAddMouseButton)(void*, int, bool) = nullptr;
static LPARAM probeLastClient = 0;
/* The render callback is the one place the host hands out a live game handle for
   an instance, so that is where the per-instance footprint change is made. */
static void probeRenderCallback(void*, const TCComponentRenderFrameV1* frame) {
    if (!frame) return;
    if (!secondHandle.instance_id || frame->instance_id != secondHandle.instance_id) return;
    probeSecondX = frame->origin_x;
    probeSecondY = frame->origin_y;
    /* One board cell in screen pixels: the length of the frame's local x axis. */
    probeSecondUnit = std::sqrt(frame->axis_x_x * frame->axis_x_x +
                                frame->axis_x_y * frame->axis_x_y);
    probeSecondSeen = probeSecondUnit > 1.f;
    if (!probeResizePending || !probeGeometryReady) return;
    probeResizeStatus = tc::component_geometry::setInstanceFootprint(
        probeGeometryV2, frame->component, 3.f, 2.f);
    probeResizePending = false;
    ++probeResizeCount;
    log("pin-shape: geometry instance resize=" + std::to_string(probeResizeStatus) +
        " (half=3,2)");
}

static BOOL CALLBACK probeFindWindow(HWND candidate, LPARAM data) {
    DWORD owner = 0;
    GetWindowThreadProcessId(candidate, &owner);
    if (owner != GetCurrentProcessId() || !IsWindowVisible(candidate)) return TRUE;
    RECT rect{};
    if (!GetClientRect(candidate, &rect) || rect.right < 320 || rect.bottom < 240) return TRUE;
    *reinterpret_cast<HWND*>(data) = candidate;
    return FALSE;
}
static void armProbeInput() {
    if (probeInputArmed) return;
    probeInputArmed = true;
    EnumWindows(&probeFindWindow, reinterpret_cast<LPARAM>(&probeWindow));
    if (probeWindow) {
        SetWindowPos(probeWindow, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetForegroundWindow(probeWindow);
    }
    if (host->resolve_alias) {
        void* transform = host->resolve_alias(host->context, "board.world_to_screen");
        std::memcpy(&probeWorldToScreen, &transform, sizeof(transform));
    }
    auto getIo = reinterpret_cast<void* (*)()>(host->engine_proc(host->context, "igGetIO"));
    probeGetIo = getIo;
    void* addPosition = host->engine_proc(host->context, "ImGuiIO_AddMousePosEvent");
    std::memcpy(&probeAddMousePos, &addPosition, sizeof(probeAddMousePos));
    void* addButton = host->engine_proc(host->context, "ImGuiIO_AddMouseButtonEvent");
    std::memcpy(&probeAddMouseButton, &addButton, sizeof(probeAddMouseButton));
    int displayW = 0, displayH = 0;
    if (getIo) {
        const auto* io = static_cast<const unsigned char*>(getIo());
        float display[2]{};
        if (io) std::memcpy(display, io + 8, sizeof(display));
        displayW = static_cast<int>(display[0]);
        displayH = static_cast<int>(display[1]);
    }
    RECT client{};
    if (probeWindow && GetClientRect(probeWindow, &client) && displayW > 1 && displayH > 1) {
        probeClientWidth = client.right;
        probeClientHeight = client.bottom;
        probeScaleX = static_cast<float>(client.right) / static_cast<float>(displayW);
        probeScaleY = static_cast<float>(client.bottom) / static_cast<float>(displayH);
    }
}
static LPARAM probeClientPoint(float gameX, float gameY) {
    const int clientX = static_cast<int>(gameX * probeScaleX);
    const int clientY = static_cast<int>(gameY * probeScaleY);
    POINT screen{clientX, clientY};
    if (probeWindow) ClientToScreen(probeWindow, &screen);
    SetCursorPos(screen.x, screen.y);
    probeLastClient = MAKELPARAM(clientX, clientY);
    return probeLastClient;
}
static void probePress(float gameX, float gameY) {
    const LPARAM point = probeClientPoint(gameX, gameY);
    PostMessageW(probeWindow, WM_MOUSEMOVE, 0, point);
    Sleep(30);
    PostMessageW(probeWindow, WM_LBUTTONDOWN, MK_LBUTTON, point);
}
static void probeDrag(float gameX, float gameY) {
    PostMessageW(probeWindow, WM_MOUSEMOVE, MK_LBUTTON, probeClientPoint(gameX, gameY));
}
static void probeRelease(float gameX, float gameY) {
    PostMessageW(probeWindow, WM_LBUTTONUP, 0, probeClientPoint(gameX, gameY));
}
static void probeKey(WORD virtualKey) {
    const LPARAM data = static_cast<LPARAM>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC) << 16);
    PostMessageW(probeWindow, WM_KEYDOWN, virtualKey, data);
    Sleep(40);
    PostMessageW(probeWindow, WM_KEYUP, virtualKey, data);
}
static void probeWheel(float gameX, float gameY, int notches) {
    const int clientX = static_cast<int>(gameX * probeScaleX);
    const int clientY = static_cast<int>(gameY * probeScaleY);
    POINT screen{clientX, clientY};
    if (probeWindow) ClientToScreen(probeWindow, &screen);
    SetCursorPos(screen.x, screen.y);
    const WORD delta = static_cast<WORD>(notches > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
    for (int index = 0; index < std::abs(notches); ++index)
        PostMessageW(probeWindow, WM_MOUSEWHEEL, MAKEWPARAM(0, delta),
                     MAKELPARAM(screen.x, screen.y));
}
static void probeIoMove(float gameX, float gameY) {
    /* Both halves at once: the real pointer (the board's own drag code reads the
       system cursor) and ImGui's IO (what its widgets read).  The hitbox probes
       found the same split - the cursor has to be pinned over the component and
       the button state injected into the IO the game samples. */
    if (probeWindow) probeClientPoint(gameX, gameY);
    if (probeGetIo && probeAddMousePos) probeAddMousePos(probeGetIo(), gameX, gameY);
}
static void probeIoButton(bool down) {
    if (probeWindow && probeAddMouseButton)
        PostMessageW(probeWindow, down ? WM_LBUTTONDOWN : WM_LBUTTONUP,
                     down ? MK_LBUTTON : 0, probeLastClient);
    if (probeGetIo && probeAddMouseButton) probeAddMouseButton(probeGetIo(), 0, down);
}

/* The component handle of one instance, looked up the way every other board
   reader does: capture the objects, then match the component's own id. */
static bool probeComponentHandle(uint64_t instanceId, TCGameHandle* out) {
    TCBoardApiV3 captureApi{};
    TCBoardApiV4 boardApi{};
    if (!host || !host->query_service || !out) {
        log(std::string("pin-shape: geometry lookup unavailable (host=") +
            (host ? "set" : "null") + " query=" +
            (host && host->query_service ? "set" : "null") + " out=" +
            (out ? "set" : "null") + ")");
        return false;
    }
    if (host->query_service(host->context, TC_SERVICE_BOARD, TC_BOARD_API_VERSION_3,
                            &captureApi, sizeof(captureApi)) != TC_SERVICE_OK) {
        log("pin-shape: geometry board V3 unavailable");
        return false;
    }
    if (host->query_service(host->context, TC_SERVICE_BOARD, TC_BOARD_API_VERSION_4,
                            &boardApi, sizeof(boardApi)) != TC_SERVICE_OK) {
        log("pin-shape: geometry board V4 unavailable");
        return false;
    }
    TCGameHandle board{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) != TC_HANDLE_OK) {
        log("pin-shape: geometry no current board handle");
        return false;
    }
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    TCBoardObjectBuffersV1 buffers{};
    buffers.size = sizeof(buffers);
    buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    int status = tc::captureBoardObjects(&captureApi, &board, &snapshot, &buffers);
    std::vector<TCGameHandle> handles;
    if (status == TC_SNAPSHOT_ERR_CAPACITY || snapshot.component_count) {
        handles.assign(static_cast<size_t>(snapshot.component_count), TCGameHandle{});
        buffers.components = handles.data();
        buffers.component_capacity = handles.size();
        status = tc::captureBoardObjects(&captureApi, &board, &snapshot, &buffers);
    }
    if (status != TC_SNAPSHOT_OK) {
        log("pin-shape: geometry capture failed status=" + std::to_string(status) +
            " count=" + std::to_string(snapshot.component_count) +
            " written=" + std::to_string(snapshot.component_written));
        return false;
    }
    if (!snapshot.component_written)
        log("pin-shape: geometry capture returned no components (status=" +
            std::to_string(status) + " count=" + std::to_string(snapshot.component_count) + ")");
    for (uint64_t index = 0; index < snapshot.component_written; ++index) {
        TCComponentInfoV1 info{};
        info.size = sizeof(info);
        if (tc::readComponent(&boardApi, &handles[index], &info) != TC_SNAPSHOT_OK) continue;
        if (info.id == instanceId)
            log("pin-shape: geometry found instance " + std::to_string(instanceId) +
                " at " + std::to_string(info.x) + "," + std::to_string(info.y));
        if (info.id != instanceId) continue;
        *out = handles[index];
        return true;
    }
    return false;
}
/* Where an instance's cell is on screen, and how many pixels one board cell is
   worth (sampled from the game's own transform, so the drag moves whole cells). */
static bool probeComponentPoint(uint64_t instanceId, float* outX, float* outY, float* unit) {
    if (!probeWorldToScreen || !probeWindow) return false;
    TCGameHandle component{};
    if (!probeComponentHandle(instanceId, &component)) return false;
    TCBoardApiV4 boardApi{};
    if (host->query_service(host->context, TC_SERVICE_BOARD, TC_BOARD_API_VERSION_4,
                            &boardApi, sizeof(boardApi)) != TC_SERVICE_OK)
        return false;
    TCComponentInfoV1 info{};
    info.size = sizeof(info);
    if (tc::readComponent(&boardApi, &component, &info) != TC_SNAPSHOT_OK) return false;
    const ProbePoint here = probeWorldToScreen(ProbePoint{static_cast<float>(info.x),
                                                          static_cast<float>(info.y)});
    const ProbePoint next = probeWorldToScreen(ProbePoint{static_cast<float>(info.x) + 1.f,
                                                          static_cast<float>(info.y)});
    const float displayW = probeScaleX > 0.f ? probeClientWidth / probeScaleX : 0.f;
    *outX = here.x * displayW;
    *outY = here.y * (probeScaleY > 0.f ? probeClientHeight / probeScaleY : 0.f);
    *unit = (next.x - here.x) * displayW;
    return *unit > 1.f;
}

static void watchFrame(void* user, const TCFrame* frame) {
    static double last = 0;
    if (wrappedFrame) wrappedFrame(user, frame);
    /* The level's own test stops the simulation as soon as it fails, and a
       0-pin component cannot be made to satisfy every campaign test.  Once the
       runner has reported, the probe drives a few more cycles itself so "the
       callback runs once per cycle" is measured rather than inferred. */
    /* A failed level test stops the level's own runner before it reports, so
       the probe takes over on a deadline as well: a 0-pin component cannot
       satisfy every campaign test, and "the callback runs every cycle" has to
       be measurable anyway. */
    if (autotestDone || autotestElapsed > 14.0) {
        if (!tookOver) {
            tookOver = true;
            int64_t extraCycles = 12;
            if (const char* requested = std::getenv("TC_PIN_SHAPE_BENCHMARK_CYCLES")) {
                char* end = nullptr;
                const long long parsed = std::strtoll(requested, &end, 10);
                if (end != requested && parsed > extraCycles) {
                    extraCycles = parsed;
                    benchmarkUncapped = true;
                }
            }
            probeTarget = mod.simulation.cycle() + extraCycles;
            log("pin-shape: probe takes over the run up to cycle=" + std::to_string(probeTarget));
        }
        // Benchmark fixtures may carry the default slow speed. Key 2 is
        // ctl_cycle_speed_ms in the game's generated CommandIndex enum.
        if (benchmarkUncapped) mod.simulation.setCommandSetting(2, 1000000000000LL);
        static const bool benchmarkOnce=[] {
            const char* value=std::getenv("TC_PIN_SHAPE_BENCHMARK_ONCE");
            return value && value[0]=='1';
        }();
        if (mod.simulation.cycle() < probeTarget &&
            !(benchmarkUncapped && benchmarkOnce && benchmarkSubmitted)) {
            mod.simulation.run(autotestModel, probeTarget);
            benchmarkSubmitted=true;
        }
    }
    /* The instance walk: two instances are on the board, one is reset (only it
       may change), one is deleted, and the handle it left behind must be
       refused.  That is the whole point of the handle/generation contract. */
    if (tookOver && !instancesReady) {
        /* The table is resolved at load; a missing one is reported once. */
        if (!instancesReported) {
            instancesReported = true;
            log("pin-shape: tc.component.instances unavailable");
        }
    } else if (tookOver && wide9Calls.load() >= 2) {
        /* The level runner stops updating its own elapsed clock once it is
           done, so the walk keeps its own. */
        const double now = frame ? frame->time_seconds : 0.0;
        const auto requestCompile = [] {
            if (!compile_request || !autotestModel) return;
            tc::TCNimString progress{};
            compile_request(autotestModel, &progress, 0);
        };
        if (instancesStage == 0) {
            /* Both instances come from the board fixture: the two-instance case
               is the type's, not the placement helper's (measured: the helper
               refuses to place while the level's own run is in flight, and the
               count stays put). */
            log("pin-shape: instances enumerated: " + describeInstances(instances, nullptr, -1));
            TCComponentInstanceHandle handles[8]{};
            uint32_t written = 0, total = 0;
            tc::component_instances::enumerate(instances, kWide9Id, handles, 8, &written, &total);
            if (written >= 2) {
                firstHandle = handles[0];
                secondHandle = handles[1];
                uint64_t before[4]{}, after[4]{}, other[4]{};
                uint32_t beforeWords = 0, afterWords = 0, otherWords = 0;
                tc::component_instances::state(instances, firstHandle, before, 4, &beforeWords);
                uint8_t configBefore[4]{}, configAfter[4]{};
                uint32_t configBeforeBytes = 0, configAfterBytes = 0;
                TCComponentStorageInfoV1 storageInfo{};
                storageInfo.size = sizeof(storageInfo);
                const int storageInfoStatus = componentStorageReady
                    ? tc::component_storage::info(componentStorage, firstHandle, &storageInfo)
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
                const int configReadBefore = componentStorageReady
                    ? tc::component_storage::readConfig(componentStorage, firstHandle, configBefore,
                                                        sizeof(configBefore), &configBeforeBytes)
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
                const uint8_t replacement[4]{42, 2, 3, 4};
                const int configWrite = componentStorageReady
                    ? tc::component_storage::writeConfig(componentStorage, firstHandle, 3,
                                                         replacement, sizeof(replacement))
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
                const int reset = tc::component_instances::reset(instances, firstHandle);
                const int configReadAfter = componentStorageReady
                    ? tc::component_storage::readConfig(componentStorage, firstHandle, configAfter,
                                                        sizeof(configAfter), &configAfterBytes)
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
                storageInfo.size = sizeof(storageInfo);
                const int storageInfoAfter = componentStorageReady
                    ? tc::component_storage::info(componentStorage, firstHandle, &storageInfo)
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
                tc::component_instances::state(instances, firstHandle, after, 4, &afterWords);
                tc::component_instances::state(instances, secondHandle, other, 4, &otherWords);
                log("pin-shape: instances reset status=" + std::to_string(reset) +
                    " first_before=" + std::to_string(beforeWords ? before[0] : 0) +
                    " first_after=" + std::to_string(afterWords ? after[0] : 0) +
                    " second_state=" + std::to_string(otherWords ? other[0] : 0));
                log("pin-shape: storage info=" + std::to_string(storageInfoStatus) +
                    " read_before=" + std::to_string(configReadBefore) +
                    " write=" + std::to_string(configWrite) +
                    " read_after=" + std::to_string(configReadAfter) +
                    " info_after=" + std::to_string(storageInfoAfter) +
                    " schema=" + std::to_string(storageInfo.config_schema) +
                    " bytes=" + std::to_string(configAfterBytes) +
                    " before=" + std::to_string(configBeforeBytes ? configBefore[0] : 0) +
                    " after=" + std::to_string(configAfterBytes ? configAfter[0] : 0) +
                    " revision=" + std::to_string(storageInfo.config_revision));
            } else {
                log("pin-shape: instances second instance never bound (" +
                    std::to_string(written) + " of " + std::to_string(total) + ")");
            }
            /* The instance's own rectangle: tc.component.geometry V2 changes it
               for this one instance, and the type hears TC_LOGIC_RESIZE. */
            if (probeGeometryReady && secondHandle.instance_id) {
                /* The resize itself happens in the render callback, which is the
                   one place a live game handle for the instance is handed out. */
                probeResizePending = true;
                log("pin-shape: geometry resize requested for the second instance");
            } else {
                log("pin-shape: geometry V2 unavailable; resize not driven");
            }
            armProbeInput();
            probeMoveInstance = secondHandle.instance_id;
            probeMovePhase = 0;
            instancesStage = 4;
            instancesStageAt = now;
        } else if (instancesStage == 4 && now > instancesStageAt + 0.6) {
            /* The player's own move gesture: press on the component, drag it two
               cells to the right, release.  The host's per-frame reading of the
               board record turns that into TC_LOGIC_MOVE. */
            float x = 0.f, y = 0.f, unit = 0.f;
            if (probeMovePhase == 0) {
                /* A player edits a paused board: the game's own input handler
                   ignores pointer work while the simulation is running, so the
                   probe asks the command bus for a stop first. */
                if (!probeStopRequested) {
                    probeStopRequested = true;
                    TCGameHandle board{};
                    TCCommandApiV2 commands{};
                    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) == TC_HANDLE_OK &&
                        host->query_service(host->context, TC_SERVICE_COMMANDS,
                                            TC_COMMAND_API_VERSION_2, &commands,
                                            sizeof(commands)) == TC_SERVICE_OK) {
                        TCCommandV2 stop{};
                        stop.size = sizeof(stop);
                        stop.type = TC_COMMAND_SIM_STOP;
                        stop.subject = board;
                        uint64_t request = 0;
                        const int submitted = commands.submit(commands.context, &stop, &request);
                        log("pin-shape: moving after a SIM_STOP request, submit=" +
                            std::to_string(submitted));
                    } else {
                        log("pin-shape: moving without a SIM_STOP request");
                    }
                    instancesStageAt = now;
                    return;
                }
                if (!probeSecondSeen) {
                    log("pin-shape: move skipped (no component point)");
                    instancesStage = 1;
                    instancesStageAt = now;
                    return;
                }
                x = probeSecondX;
                y = probeSecondY;
                unit = probeSecondUnit;
                log("pin-shape: moving instance at " + std::to_string((int)x) + "," +
                    std::to_string((int)y) + " by two cells (unit=" +
                    std::to_string(unit) + ")");
                /* Hover first, then press - the board's own drag path needs to
                   see the pointer over the component before the button goes
                   down. */
                probeIoMove(x, y);
                probeMoveStartX = x;
                probeMoveStartY = y;
                /* A click first: the game moves the component the player has
                   selected, and a synthetic press that never selects it is just
                   a miss. */
                probeIoButton(true);
                probeMovePhase = 8;
                instancesStageAt = now;
            } else if (probeMovePhase == 8) {
                probeIoButton(false);
                probeMovePhase = 9;
                instancesStageAt = now;
            } else if (probeMovePhase == 9) {
                probeIoMove(x, y);
                probeMovePhase = 1;
                instancesStageAt = now;
            } else if (probeMovePhase == 1) {
                probeIoButton(true);
                probeMovePhase = 2;
                instancesStageAt = now;
            } else if (probeMovePhase == 2) {
                probeIoMove(x + unit * 0.5f, y);
                probeMovePhase = 3;
                instancesStageAt = now;
            } else if (probeMovePhase == 3) {
                probeIoMove(x + unit, y);
                probeMovePhase = 4;
                instancesStageAt = now;
            } else if (probeMovePhase == 4) {
                probeIoMove(x + unit * 1.5f, y);
                probeMovePhase = 5;
                instancesStageAt = now;
            } else if (probeMovePhase == 5) {
                probeIoMove(x + unit * 2.f, y);
                probeMovePhase = 6;
                instancesStageAt = now;
            } else if (probeMovePhase == 6) {
                probeIoButton(false);
                probeMovePhase = 7;
                instancesStageAt = now;
            } else {
                log("pin-shape: instances moves=" + std::to_string(wide9Moves.load()) +
                    " rotations=" + std::to_string(wide9Rotations.load()) +
                    " screen=" + std::to_string((int)probeSecondX) + "," +
                    std::to_string((int)probeSecondY) + " (dragged from " +
                    std::to_string((int)probeMoveStartX) + "," +
                    std::to_string((int)probeMoveStartY) + "); a synthetic board drag is " +
                    "not acted on by this build (see docs/research/component-hitbox-path.md), " +
                    "so the move/rotate gate stays open");
                instancesStage = 1;
                instancesStageAt = now;
            }
        } else if (instancesStage == 1 && now > instancesStageAt + 1.5) {
            const int index = wide9BoardIndex(0);
            const bool removed = deleteComponent && autotestModel && index >= 0
                                     ? (deleteComponent(static_cast<char*>(autotestModel) + 0x78, index), true)
                                     : false;
            log("pin-shape: instances delete index=" + std::to_string(index) +
                (removed ? "" : " (failed)"));
            if (removed) requestCompile();
            instancesStage = 2;
            instancesStageAt = now;
        } else if (instancesStage == 2 && now > instancesStageAt + 2.0) {
            log("pin-shape: instances after delete: " + describeInstances(instances, nullptr, -1));
            auto forged = firstHandle;
            forged.generation += 1;
            log("pin-shape: instances old_handle=" +
                std::to_string(tc::component_instances::validate(instances, firstHandle)) +
                " forged_handle=" +
                std::to_string(tc::component_instances::validate(instances, forged)) +
                " (both " + std::to_string(TC_COMPONENT_INSTANCES_ERR_STALE) + " = stale)");
            log("pin-shape: instances stage done");
            instancesStage = 5;
            instancesStageAt = now;
        } else if (instancesStage == 5 && now > instancesStageAt + 1.2) {
            /* Which input rotates a component in this build?  The probe tries the
               candidates and reports how many rotations each produced - the poll
               only fires when the record's direction byte really changed. */
            static int rotateStep = 0;
            const float x = probeSecondX, y = probeSecondY;
            const int before = wide9Rotations.load();
            if (rotateStep == 0 && probeSecondSeen) {
                probeKey('R');
                log("pin-shape: rotate candidate R produced " +
                    std::to_string(wide9Rotations.load() - before));
                rotateStep = 1;
                instancesStageAt = now;
            } else if (rotateStep == 1 && probeSecondSeen) {
                probeWheel(x, y, -1);
                log("pin-shape: rotate candidate wheel produced " +
                    std::to_string(wide9Rotations.load() - before));
                rotateStep = 2;
                instancesStageAt = now;
            } else {
                /* Leaving the board is the other half of the lifecycle: the
                   instances are not deleted, their bindings are dropped, and that
                   is TC_LOGIC_UNBIND. */
                using LoadLevel = void (*)(void*, const tc::TCNimString*);
                auto loadLevel = reinterpret_cast<LoadLevel>(host->resolve_symbol(
                    host->context, "load_level__modelZutilities_u7740"));
                if (loadLevel && mod.game.raw_new_string) {
                    tc::TCNimString name{};
                    const std::string target = "sandbox";
                    mod.game.raw_new_string(&name, static_cast<int64_t>(target.size()));
                    name.length = target.size();
                    std::memcpy(static_cast<unsigned char*>(name.data) + 8, target.c_str(),
                                target.size());
                    static_cast<unsigned char*>(name.data)[8 + target.size()] = 0;
                    loadLevel(autotestModel, &name);
                    log("pin-shape: leaving the board for " + target);
                } else {
                    log("pin-shape: could not load another level for the unbind case");
                }
                instancesStage = 6;
                instancesStageAt = now;
            }
        } else if (instancesStage == 6 && now > instancesStageAt + 2.0) {
            log("pin-shape: instances placement stage done");
            instancesStage = 3;
        }
    }
    if (!frame || frame->time_seconds - last < 1.0) return;
    last = frame->time_seconds;
    using SimGetTestState = int64_t (*)();
    auto* testState = reinterpret_cast<SimGetTestState>(
        host->resolve_symbol(host->context, "sim_get_test_state__modelZsimulationZcontroller_u26"));
    log("pin-shape: watch t=" + std::to_string(static_cast<int>(frame->time_seconds)) +
        " cycle=" + std::to_string(mod.simulation.cycle()) +
        " stage=" + std::to_string(autotestStage) +
        " target=" + std::to_string(autotestTarget) +
        " verdict=" + std::to_string(testState ? testState() : -1));
}

static void sourceCallback(TCLogicIO* io) {
    if (io->phase == TC_LOGIC_RESET) {
        log("pin-shape: source reset instance=" + std::to_string(io->instance_id));
        io->outputs[0] = 0;
        return;
    }
    /* A steady zero.  The board drives the level's output with OR(input, this),
       so the level's own test passes and keeps the run going; what the probe
       reports is whether the callback is reached *every cycle*, which the call
       count next to the cycle number shows. */
    io->outputs[0] = 0;
    /* One line per cycle that reaches the callback.  Refresh peeks run the
       callback too (the panel needs the value), so only the cycle phase counts
       here - otherwise "one call per cycle" would be a claim about frames. */
    if (io->phase != TC_LOGIC_CYCLE) return;
    const int calls = sourceCalls.fetch_add(1) + 1;
    if (io->cycle != sourceLastCycle.exchange(io->cycle))
        log("pin-shape: source cycle=" + std::to_string(io->cycle) +
            " instance=" + std::to_string(io->instance_id) +
            " calls=" + std::to_string(calls));
}

static void sinkCallback(TCLogicIO* io) {
    if (io->phase == TC_LOGIC_RESET) {
        log("pin-shape: sink reset instance=" + std::to_string(io->instance_id));
        return;
    }
    if (io->phase != TC_LOGIC_CYCLE) return;
    const int calls = sinkCalls.fetch_add(1) + 1;
    if (io->cycle != sinkLastCycle.exchange(io->cycle))
        log("pin-shape: sink cycle=" + std::to_string(io->cycle) +
            " instance=" + std::to_string(io->instance_id) +
            " in=" + std::to_string(io->inputs[0]) + " calls=" + std::to_string(calls));
}

/* The V2 shape: nine inputs and one output through tc.component.types, with the
   definition - not a fixed eight words - choosing the state size. */
static void wide9Callback(TCLogicIOV2* io) {
    if (io->phase == TC_LOGIC_RESET) {
        log("pin-shape: wide9 reset instance=" + std::to_string(io->instance_id) +
            " state_words=" + std::to_string(io->state_words));
        if (io->state && io->state_words) io->state[0] = 0;
        if (io->outputs) io->outputs[0] = 0;
        return;
    }
    if (io->outputs) io->outputs[0] = 0;   /* or_gate passes while this stays 0 */
    if (io->phase != TC_LOGIC_CYCLE) return;
    const int calls = wide9Calls.fetch_add(1) + 1;
    if (io->cycle != wide9LastCycle.exchange(io->cycle)) {
        std::string inputs;
        for (uint32_t i = 0; i < io->input_count; ++i) {
            if (i) inputs += ",";
            inputs += std::to_string(io->inputs ? io->inputs[i] : 0);
        }
        log("pin-shape: wide9 cycle=" + std::to_string(io->cycle) +
            " inputs=" + std::to_string(io->input_count) + "[" + inputs + "]" +
            " state=" + std::to_string(io->state && io->state_words ? io->state[0] : 0) +
            " calls=" + std::to_string(calls));
    }
    if (io->state && io->state_words) io->state[0] = io->state[0] + 1;
}

/* Maximum staged input shape: input 31 is driven by a known 64-bit source while
   the first 31 pins are unconnected.  Seeing that last word intact proves data
   beyond the direct 128-bit payload crossed the 16-call staging sequence. */
static void wide2048Callback(TCLogicIOV2* io) {
    if (io->outputs && io->output_count) io->outputs[0] = 0;
    if (io->phase != TC_LOGIC_CYCLE) return;
    const int calls = wide2048Calls.fetch_add(1) + 1;
    if (io->cycle == wide2048LastCycle.exchange(io->cycle)) return;
    uint32_t nonzero = 0;
    for (uint32_t i = 0; io->inputs && i < io->input_count; ++i)
        if (io->inputs[i]) ++nonzero;
    log("pin-shape: wide2048 cycle=" + std::to_string(io->cycle) +
        " inputs=" + std::to_string(io->input_count) +
        " nonzero=" + std::to_string(nonzero) +
        " first=" + std::to_string(io->inputs && io->input_count ? io->inputs[0] : 0) +
        " last=" + std::to_string(io->inputs && io->input_count ? io->inputs[io->input_count - 1] : 0) +
        " calls=" + std::to_string(calls));
}

static void wideSourceCallback(TCLogicIOV2* io) {
    if (io->outputs && io->output_count)
        io->outputs[0] = 0x0123456789abcdefULL;
}

/* Registers the nine-input shape through the V2 service (not through
   register_component, whose callback struct carries eight pins). */
/* Lifecycle: the host announces an instance when it first binds it and before
   it is released.  Both run where the change happened, and both are allowed to
   write state. */
static void wide9Create(TCLogicIOV2* io) {
    if (io->state && io->state_words) io->state[0] = 0x1000;
    log("pin-shape: wide9 on_create instance=" + std::to_string(io->instance_id) +
        " phase=" + std::to_string(io->phase) + " state_words=" + std::to_string(io->state_words));
}
static void wide9Destroy(TCLogicIOV2* io) {
    log("pin-shape: wide9 on_destroy instance=" + std::to_string(io->instance_id) +
        " phase=" + std::to_string(io->phase) +
        " state=" + std::to_string(io->state && io->state_words ? io->state[0] : 0));
}
/* The placement tail of TCLogicIOV2 (2026-09-28): where the instance sits, which
   way it points and how big it is.  Move / rotate / resize are board edits the
   player (or the host) performed, and the three callbacks read the new values
   from that tail.  The tail is gated by `size`, so an older loader's IO simply
   does not carry it. */
static void logPlacement(const char* what, TCLogicIOV2* io) {
    const bool tail = io && io->size >= offsetof(TCLogicIOV2, half_height) +
                                      sizeof(io->half_height);
    log(std::string("pin-shape: wide9 ") + what + " instance=" +
        std::to_string(io ? io->instance_id : 0) +
        (tail ? " cell=" + std::to_string(io->cell_x) + "," + std::to_string(io->cell_y) +
                    " rotation=" + std::to_string(io->rotation) +
                    " half=" + std::to_string(io->half_width) + "," +
                    std::to_string(io->half_height)
              : std::string(" (no placement tail)")));
}
static void wide9Move(TCLogicIOV2* io) { ++wide9Moves; logPlacement("on_move", io); }
static void wide9Rotate(TCLogicIOV2* io) { ++wide9Rotations; logPlacement("on_rotate", io); }
static void wide9Resize(TCLogicIOV2* io) { ++wide9Resizes; logPlacement("on_resize", io); }
static void wide9Compile(TCLogicIOV2* io) {
    log("pin-shape: wide9 on_compile instance=" + std::to_string(io->instance_id) +
        " phase=" + std::to_string(io->phase));
}
static void wide9Unbind(TCLogicIOV2* io) {
    log("pin-shape: wide9 on_unbind instance=" + std::to_string(io->instance_id) +
        " phase=" + std::to_string(io->phase));
}
static const TCComponentLifecycleV1 wide9Lifecycle = [] {
    TCComponentLifecycleV1 value{};
    value.size = sizeof(value);
    value.version = TC_COMPONENT_LIFECYCLE_VERSION_1;
    value.on_create = &wide9Create;
    value.on_destroy = &wide9Destroy;
    value.on_move = &wide9Move;
    value.on_rotate = &wide9Rotate;
    value.on_resize = &wide9Resize;
    value.on_compile = &wide9Compile;
    value.on_unbind = &wide9Unbind;
    return value;
}();
static const uint8_t wide9DefaultConfig[4] = {7, 2, 3, 4};

static void registerWide9(const TCHost* h) {
    tc::component_types::Api types{};
    if (!tc::component_types::table(h, &types)) {
        log("pin-shape: tc.component.types unavailable; wide9 not registered");
        return;
    }
    static const TCComponentPinV2 ins[9] = {
        {"in0","Input 0",1,0}, {"in1","Input 1",1,0}, {"in2","Input 2",1,0},
        {"in3","Input 3",1,0}, {"in4","Input 4",1,0}, {"in5","Input 5",1,0},
        {"in6","Input 6",1,0}, {"in7","Input 7",1,0}, {"in8","Input 8",1,0}};
    static const TCComponentPinV2 outs[1] = {{"out","Output",1,0}};
    TCComponentTypeDefinitionV2 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_2;
    definition.custom_id = kWide9Id;
    definition.type_id = "dev.pin-shape-probe/wide9";
    definition.name = "Pin shape wide9";
    definition.description = "9 inputs, 1 output, 2 state words";
    definition.inputs = ins; definition.input_count = 9;
    definition.outputs = outs; definition.output_count = 1;
    definition.state_words = 2;
    definition.gate_cost = 1; definition.delay = 1;
    definition.callback = &wide9Callback;
    definition.lifecycle = &wide9Lifecycle;
    definition.config_schema = 3;
    definition.config_size = sizeof(wide9DefaultConfig);
    definition.default_config = wide9DefaultConfig;
    const int result = tc::component_types::registerDefinition(types, &definition);
    log("pin-shape: wide9 V2 registration (9in/1out) -> " + std::to_string(result) +
        " (" + tc::component_types::errorText(result) + ")");
}

/* ---- clocked: exact output cones plus one hidden tick ---------------------

   The visible drivers run independently in REFRESH from their declared input
   cones.  A hidden sink sees the complete input tuple and performs the sole
   CYCLE call, so state advances once without adding an edge back to any public
   output.

   This shape is the full_adder board's (three inputs, two outputs), and the
   callback is a real adder, so the level passes only if every output reaches
   the board: the tick has to deliver all three inputs (the callback logs the
   clock it saw) and the carry has to be read back from it.  The state word is a
   pure call counter, so "one tick per cycle" is a number in the log: a second
   call per cycle would advance it twice. */
constexpr uint64_t kClockedId = 0x414444525F303031ULL;   /* the full_adder board */
std::atomic<uint64_t> clockedCalls{0};
std::atomic<int64_t> clockedLastCycle{-1};

static void clockedCallback(TCLogicIOV3* io) {
    if (io->phase == TC_LOGIC_RESET) {
        if (io->state && io->state_words) io->state[0] = 0;
        if (io->outputs && io->output_count >= 2) {
            io->outputs[0] = {0, 1}; io->outputs[1] = {0, 1};
        }
        clockedCalls.store(0);
        clockedLastCycle.store(-1);
        log("pin-shape: clocked reset instance=" + std::to_string(io->instance_id));
        return;
    }
    const uint64_t clk = io->inputs ? (io->inputs[0].value & 1) : 0;
    const uint64_t total = io->inputs ? ((io->inputs[0].value & 1) +
                                         (io->inputs[1].value & 1) +
                                         (io->inputs[2].value & 1)) : 0;
    if (io->outputs && io->output_count >= 2) {
        io->outputs[0] = {total & 1, 1};
        io->outputs[1] = {(total >> 1) & 1, 1};
    }
    /* Refresh runs against a state copy and must not advance anything; only the
       cycle phase is the tick. */
    if (io->phase != TC_LOGIC_CYCLE) return;
    const uint64_t calls = clockedCalls.fetch_add(1) + 1;
    uint64_t state = 0;
    if (io->state && io->state_words) {
        state = ++io->state[0];
    }
    if (io->cycle != clockedLastCycle.exchange(io->cycle)) {
        log("pin-shape: clocked cycle=" + std::to_string(io->cycle) +
            " calls=" + std::to_string(calls) +
            " state=" + std::to_string(state) +
            " clk=" + std::to_string(clk));
    }
}

static void registerClocked(const TCHost* h) {
    tc::component_types::ApiV2 types{};
    if (!tc::component_types::tableV2(h, &types)) {
        log("pin-shape: tc.component.types unavailable; clocked not registered");
        return;
    }
    static const TCComponentPinV2 ins[3] = {
        {"clk","Clock",1,0}, {"a","A",1,0}, {"b","B",1,0}};
    static const TCComponentPinV2 outs[2] = {{"sum","Sum",1,0}, {"carry","Carry",1,0}};
    TCComponentTypeDefinitionV3 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_3;
    definition.custom_id = kClockedId;
    definition.type_id = "dev.pin-shape-probe/clocked";
    definition.name = "Pin shape clocked";
    definition.description = "clocked adder: one tick per cycle";
    definition.inputs = ins; definition.input_count = 3;
    definition.outputs = outs; definition.output_count = 2;
    definition.state_words = 4;
    definition.gate_cost = 1; definition.delay = 1;
    definition.callback = &clockedCallback;
    static const uint32_t dependencies[2] = {0x7u, 0x7u};
    definition.flags = TC_COMPONENT_TYPE_OUTPUT_DEPENDENCIES |
                       TC_COMPONENT_TYPE_CLOCKED_TICK;
    definition.output_dependencies = dependencies;
    definition.output_dependency_count = 2;
    const int result = tc::component_types::registerDefinitionV3(types, &definition);
    log("pin-shape: clocked V3 registration (3in/2out, one tick per cycle) -> " +
        std::to_string(result) + " (" + tc::component_types::errorText(result) + ")");
}

static void registerWide2048(const TCHost* h) {
    tc::component_types::Api types{};
    if (!tc::component_types::table(h, &types)) {
        log("pin-shape: tc.component.types unavailable; wide2048 not registered");
        return;
    }
    static const TCComponentPinV2 sourceOut[1] = {{"out","Output",64,0}};
    TCComponentTypeDefinitionV2 source{};
    source.size = sizeof(source); source.version = TC_COMPONENT_TYPES_VERSION_2;
    source.custom_id = kWideSourceId; source.type_id = "dev.pin-shape-probe/wide64-source";
    source.name = "Pin shape wide64 source"; source.description = "Known 64-bit source";
    source.outputs = sourceOut; source.output_count = 1;
    source.gate_cost = 1; source.delay = 1; source.callback = &wideSourceCallback;
    const int sourceResult = tc::component_types::registerDefinition(types, &source);
    log("pin-shape: wide64 source registration (0in/64out) -> " +
        std::to_string(sourceResult) + " (" + tc::component_types::errorText(sourceResult) + ")");

    std::array<std::string,32> ids{}, labels{};
    std::array<TCComponentPinV2,32> ins{};
    for (uint32_t i = 0; i < ins.size(); ++i) {
        ids[i] = "in" + std::to_string(i);
        labels[i] = "Input " + std::to_string(i);
        ins[i] = {ids[i].c_str(), labels[i].c_str(), 64, 0};
    }
    static const TCComponentPinV2 outs[1] = {{"out","Output",1,0}};
    TCComponentTypeDefinitionV2 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_2;
    definition.custom_id = kWide2048Id;
    definition.type_id = "dev.pin-shape-probe/wide2048";
    definition.name = "Pin shape wide2048";
    definition.description = "32 x 64-bit inputs, 1 output";
    definition.inputs = ins.data(); definition.input_count = static_cast<uint32_t>(ins.size());
    definition.outputs = outs; definition.output_count = 1;
    definition.gate_cost = 1; definition.delay = 1;
    definition.callback = &wide2048Callback;
    const int result = tc::component_types::registerDefinition(types, &definition);
    log("pin-shape: wide2048 V2 registration (32x64in/1out, 2048 bits) -> " +
        std::to_string(result) + " (" + tc::component_types::errorText(result) + ")");
}

/* ---- instances: placement, queries, reset and destroy ---------------------

   The nine-pin board starts with one instance.  This puts a second one on the
   board at run time (the same placement template the component menu uses), then
   drives the tc.component.instances service against both, resets one, deletes
   the other and checks the handle it left behind is refused. */
static std::vector<unsigned char> wide9Placement(int16_t x, int16_t y) {
    std::vector<unsigned char> placement(0x238, 0);
    placement[0] = 0x4E;                                  /* custom instance */
    const int32_t point = static_cast<int32_t>((static_cast<uint32_t>(y) << 16) |
                                               static_cast<uint16_t>(x));
    std::memcpy(placement.data() + 2, &point, sizeof(point));
    const uint64_t id = kWide9Id;
    std::memcpy(placement.data() + 0x188, &id, sizeof(id));
    const uint64_t one = 1, capacity = 0x100;
    std::memcpy(placement.data() + 0x58, &one, sizeof(one));
    std::memcpy(placement.data() + 0x60, &capacity, sizeof(capacity));
    placement[0x68] = 1;
    std::memcpy(placement.data() + 0x70, &one, sizeof(one));
    std::memcpy(placement.data() + 0x78, &capacity, sizeof(capacity));
    placement[0x80] = 1;
    return placement;
}

/* Index of one of our components in the board's own component array. */
static int wide9BoardIndex(int ordinal) {
    if (!autotestModel) return -1;
    const auto* base = static_cast<const unsigned char*>(autotestModel);
    uint64_t count = 0;
    const unsigned char* data = nullptr;
    std::memcpy(&count, base + 0x78, sizeof(count));
    std::memcpy(&data, base + 0x80, sizeof(data));
    if (!data || count > 100000) return -1;
    int seen = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const unsigned char* record = data + 8 + i * 0x238;
        if (record[0] != 0x4e) continue;
        uint64_t id = 0;
        std::memcpy(&id, record + 0x188, sizeof(id));
        if (id != kWide9Id) continue;
        if (seen++ == ordinal) return static_cast<int>(i);
    }
    return -1;
}

static std::string describeInstances(tc::component_instances::Api& instances,
                                     tc::component_instances::Handle* kept, int keep) {
    TCComponentInstanceHandle handles[8]{};
    uint32_t written = 0, total = 0;
    const int status = tc::component_instances::enumerate(instances, kWide9Id, handles, 8,
                                                          &written, &total);
    std::string text = "enum status=" + std::to_string(status) + " written=" +
                       std::to_string(written) + " total=" + std::to_string(total);
    for (uint32_t i = 0; i < written && i < 2; ++i) {
        TCComponentInstanceInfoV1 info{};
        info.size = sizeof(info);
        const int infoStatus = tc::component_instances::info(instances, handles[i], &info);
        uint64_t state[4]{};
        uint32_t words = 0;
        const int stateStatus = tc::component_instances::state(instances, handles[i], state, 4,
                                                               &words);
        text += " | #" + std::to_string(i) + " instance=0x" + [&] {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%llx",
                          static_cast<unsigned long long>(handles[i].instance_id));
            return std::string(buffer);
        }() + " gen=" + std::to_string(handles[i].generation) +
            " info=" + std::to_string(infoStatus) + " calls=" + std::to_string(info.cycle_calls) +
            " state_status=" + std::to_string(stateStatus) + " words=" + std::to_string(words) +
            " state0=" + std::to_string(words ? state[0] : 0);
        if (kept && static_cast<int>(i) == keep) *kept = handles[i];
    }
    return text;
}

/* Registers the two shapes instead of the example's own component.  A refusal
   is reported and left in place: which shape the game accepts *is* the
   measurement, so the probe does not paper over it. */
static int registerShapes(void* context, const TCNativeComponentDefinition* source) {
    static const TCComponentPin outPin{"Out", 1};
    static const TCComponentPin inPin{"In", 1};
    TCNativeComponentDefinition definition = *source;
    definition.custom_id = kSourceId;
    definition.name = "Pin shape source";
    definition.description = "0 inputs, 1 output";
    definition.inputs = nullptr;
    definition.input_count = 0;
    definition.outputs = &outPin;
    definition.output_count = 1;
    definition.callback = &sourceCallback;
    definition.shape_svg = nullptr;
    const int sourceResult = realRegister(context, &definition);
    log("pin-shape: source registration (0in/1out) -> " + std::to_string(sourceResult));

    definition.custom_id = kSinkId;
    definition.name = "Pin shape sink";
    definition.description = "1 input, 0 outputs";
    definition.inputs = &inPin;
    definition.input_count = 1;
    definition.outputs = nullptr;
    definition.output_count = 0;
    definition.callback = &sinkCallback;
    const int sinkResult = realRegister(context, &definition);
    log("pin-shape: sink registration (1in/0out) -> " + std::to_string(sinkResult));

    /* The level runner only needs one component to exist; a refusal of both is
       still reported through the log above, which is what the probe is for. */
    return sourceResult == 0 ? 0 : (sinkResult == 0 ? 0 : sourceResult);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin) return 1;
    if (jit_code_dump::install(h)!=0) return 1;
    probeHost = *h;
    realRegister = h->register_component;
    probeHost.register_component = &registerShapes;
    const int result = byte_adder_example_load(&probeHost, plugin);
    if (result == 0 && plugin->on_frame) {
        wrappedFrame = plugin->on_frame;
        plugin->on_frame = &watchFrame;
    }
    if (result == 0) { registerWide9(h); registerClocked(h); registerWide2048(h); }
    if (result == 0) {
        instancesReady = tc::component_instances::table(h, &instances);
        if (!instancesReady) log("pin-shape: tc.component.instances unavailable");
        componentStorageReady = tc::component_storage::table(h, &componentStorage);
        if (!componentStorageReady) log("pin-shape: tc.component.storage unavailable");
        probeGeometryReady = tc::component_geometry::tableV2(h, &probeGeometryV2);
        if (!probeGeometryReady) log("pin-shape: tc.component.geometry V2 unavailable");
        tc::component_render::Api render{};
        if (tc::component_render::table(h, &render)) {
            const int callback = tc::component_render::setDrawCallback(
                render, kWide9Id, &probeRenderCallback, nullptr);
            if (callback != TC_COMPONENT_RENDER_OK)
                log("pin-shape: render callback refused status=" + std::to_string(callback));
        } else {
            log("pin-shape: tc.component.render unavailable");
        }
        addComponent = reinterpret_cast<AddComponent>(h->resolve_symbol(
            h->context, "add_component__presenterZutilitiesZhelper95functions_u5918"));
        deleteComponent = reinterpret_cast<DeleteComponent>(h->resolve_symbol(
            h->context, "board_delete_component__modelZboardZboard_u10711"));
    }
    /* Last, so a refused import cannot block the shapes above: the importer
       leaves the game's error flag set when it rejects a definition. */
    if (result == 0) {
        /* The import entry wants a directory that ends in a separator; the host
           hands out the mod's own data directory without one. */
        const std::string directory = std::string(h->data_directory_utf8) + "/";
        uint64_t id = 0x5749444500000000ULL;
        for (unsigned pins : {9u, 16u, 32u}) {
            const auto bytes = wide::definition(pins, id);
            const auto imported = mod.components.importCircuit(
                "Pin shape wide", bytes.data(), bytes.size(), directory.c_str());
            log("pin-shape: wide import " + std::to_string(pins) + "in/1out -> status " +
                std::to_string(static_cast<int>(imported.status)) + " id=" +
                std::to_string(imported.custom_id));
            if (!imported.ok()) break;
            ++id;
        }
    }
    return result;
}
