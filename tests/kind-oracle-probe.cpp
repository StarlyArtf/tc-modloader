/* Real-machine oracle for one built-in kind.

   The sandbox's simulator has to agree with the game about what every built-in
   component does, and for the kinds whose value does not come from the wires
   (registers, delay lines, the level's own Input, probes, counters) no amount of
   reading the game's files settles it.  This probe is the cheap half of the
   measurement: it puts **one built-in kind** on the board through the command
   bus and writes down what the game itself says about it - the component record,
   the wire records with their state-slot offsets, and the level's frame and
   cycle numbers.

   What it does *not* do is drive values or read the simulation state: that needs
   the game's own program to run a cycle, which the driver side of the playtest
   (`tests/kind-oracle-playtest.ps1`) supplies by advancing the level.  Splitting
   it this way keeps the probe read-only and one-file simple.

   Configuration (environment):
     TC_KIND_ORACLE_KIND        required, e.g. 0x2c or 44
     TC_KIND_ORACLE_X / _Y      optional, default 0/0
     TC_KIND_ORACLE_ROTATION    optional, default 0
     TC_ENTER_BOARD_INDEX       which home-page entry opens the board (default 2,
                                the sequence tests/enter-board.cpp uses)

   The way into a board is borrowed wholesale from `tests/enter-board.cpp`: it
   presses one of the home page's own invisible buttons, which is how every
   driver in this repository gets from the menu into a level.  Its loader is
   renamed and called from ours, so the two never fight over a hook
   (enter-board deliberately takes only `igInvisibleButton`).

   Output: <plugin-data>/board-dump.txt, and a final line
   `PASS kind-oracle kind=0xNN ...` (or `FAIL ...`). */
#include "../sdk/tc_mod.h"
#include "../sdk/tc_service_api.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>
#include "../sdk/tc_io_value.h"
#include "../sdk/tc_hook_api.h"

/* `enter-board.cpp` brings its own includes and its own file-scope statics, so it
   goes into a namespace of its own; the C headers above are already in. */
namespace enter_board_probe {
#if !defined(_WINDOWS_) && !defined(_INC_WINDOWS)
#endif
#define tc_mod_load enter_board_load
#include "enter-board.cpp"
#undef tc_mod_load
}  // namespace enter_board_probe

namespace {

const TCHost* host = nullptr;
std::filesystem::path data_folder;
int target_kind = -1;
int target_x = 0;
int target_y = 0;
int target_rotation = 0;
/* The game's own simulation state array, as a flat byte buffer.  A wire record's
   `state_byte_offset` is an index into it, which is how a board's values can be
   read without simulating anything ourselves. */
unsigned char* state_buffer = nullptr;
size_t state_size = 0;
int run_cycles = 0;
int settle_frames = 5;  /* kept for the config log; the rewrite waits by step */
/* A truth-table sweep: drive the level's global input at this component index
   through `sweep_from..sweep_to` and record the board's wire values each time.
   `-1` means "do not sweep". */
int sweep_input = -1;
uint64_t sweep_from = 0;
uint64_t sweep_to = 0;
uint64_t sweep_value = 0;
std::vector<std::pair<uint64_t, std::string>> sweep_rows;
/* Extra placements, `kind@x,y;kind@x,y`, applied before the target.  A constant
   beside a kind is how a board gets driven without the level's own input_replay. */
std::vector<std::tuple<int, int, int>> extra_placements;
int extra_index = 0;
/* The game's simulator model, captured from a hook; `preorder` needs it to
   recompile the board (see `recompile`). */
void* sim_model = nullptr;
bool recompiled = false;
using Preorder = void (*)(void*, tc::TCNimString*, int64_t);
using SetSimTest = void (*)(void*, int64_t, int64_t);
SetSimTest set_sim_test_fn = nullptr;
Preorder preorder_fn = nullptr;

void logLine(const std::string& text) {
    if (host && host->log) host->log(host->context, text.c_str());
}

std::string format(const char* pattern, ...) {
    char buffer[512];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(buffer, sizeof(buffer), pattern, args);
    va_end(args);
    return std::string(buffer);
}

int parseNumber(const char* text, int fallback) {
    if (!text || !*text) return fallback;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 0);
    if (!end || *end) return fallback;
    return static_cast<int>(value);
}

/* Writes the result file the playtest reads and logs the same text.  A run that
   reached the dump is a PASS: the dump *is* the measurement, so the verdict goes
   on top and the evidence stays in the loader log as well. */
void finish(const std::string& text) {
    const bool failed = text.rfind("FAIL", 0) == 0;
    std::string output = text;
    if (!failed) {
        output = format("PASS kind-oracle kind=0x%02x x=%d y=%d rotation=%d\n", target_kind, target_x, target_y,
                        target_rotation) +
                 text;
    }
    if (!data_folder.empty()) std::ofstream(data_folder / "result.txt") << output;
    logLine("kind-oracle: " + output.substr(0, output.find('\n')));
    if (failed) return;
    std::string rest = text;
    while (!rest.empty()) {
        const size_t end = rest.find('\n');
        const std::string line = rest.substr(0, end);
        if (!line.empty()) logLine("kind-oracle dump: " + line);
        if (end == std::string::npos) break;
        rest = rest.substr(end + 1);
    }
}

/* Reads the whole board through the public Board service: every component
   record and every wire record, with the state-slot offset a wire owns.

   `captureBoardObjects` is declared against the V3 shape, and a V4+ service
   carries those six fields at the same offsets, so the prefix is copied out
   rather than reinterpreted: the documented layout is the one the helper
   checks. */
std::string dumpBoard(const TCBoardApiV4& board) {
    TCBoardApiV3 v3{};
    v3.size = sizeof(TCBoardApiV3);
    v3.version = TC_BOARD_API_VERSION_3;
    v3.context = board.context;
    v3.get_current = board.get_current;
    v3.validate = board.validate;
    v3.resolve = board.resolve;
    v3.capture_snapshot = board.capture_snapshot;
    v3.capture_objects = board.capture_objects;

    TCGameHandle board_handle{};
    if (v3.get_current(v3.context, &board_handle) != TC_HANDLE_OK) return "FAIL no current board\n";
    /* `buffers` is never optional: the loader refuses a null one, refuses a
       `version` that is not the snapshot version, and reports capacity through
       the same `out`.  So the call is made twice with a first guess, and the
       counts the failed call filled in size the retry. */
    std::vector<TCGameHandle> components(64);
    std::vector<TCGameHandle> wires(64);
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    int status = TC_SNAPSHOT_ERR_CAPACITY;
    for (int attempt = 0; attempt < 3; ++attempt) {
        TCBoardObjectBuffersV1 buffers{};
        buffers.size = sizeof(buffers);
        buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
        buffers.components = components.empty() ? nullptr : components.data();
        buffers.component_capacity = components.size();
        buffers.wires = wires.empty() ? nullptr : wires.data();
        buffers.wire_capacity = wires.size();
        TCBoardObjectSnapshotV1 probe{};
        probe.size = sizeof(probe);
        status = tc::captureBoardObjects(&v3, &board_handle, &probe, &buffers);
        if (status == TC_SNAPSHOT_OK) {
            snapshot = probe;
            break;
        }
        if (status != TC_SNAPSHOT_ERR_CAPACITY) break;
        components.resize(probe.component_count ? probe.component_count : 64);
        wires.resize(probe.wire_count ? probe.wire_count : 64);
    }
    if (status != TC_SNAPSHOT_OK) return format("FAIL board capture status=%d\n", status);

    std::string text = format("board frame=%lld generation=%llu components=%llu wires=%llu written=%llu/%llu\n",                              static_cast<long long>(snapshot.engine_frame),
                              static_cast<unsigned long long>(snapshot.snapshot_generation),
                              static_cast<unsigned long long>(snapshot.component_count),
                              static_cast<unsigned long long>(snapshot.wire_count),
                              static_cast<unsigned long long>(snapshot.component_written),
                              static_cast<unsigned long long>(snapshot.wire_written));
    for (size_t index = 0; index < snapshot.component_written && index < components.size(); ++index) {
        TCComponentInfoV1 info{};
        info.size = sizeof(info);
        if (tc::readComponent(&board, &components[index], &info) != TC_SNAPSHOT_OK) {
            text += format("component index=%zu unreadable\n", index);
            continue;
        }
        text += format("component index=%zu kind=0x%02x x=%d y=%d rotation=%u id=%llu flags=0x%x"
                       " custom=0x%llx\n",
                       index, info.kind, info.x, info.y, info.rotation,
                       static_cast<unsigned long long>(info.id), info.flags,
                       static_cast<unsigned long long>(info.custom_prototype_id));
    }
    for (size_t index = 0; index < snapshot.wire_written && index < wires.size(); ++index) {
        TCWireInfoV1 info{};
        info.size = sizeof(info);
        if (tc::readWire(&board, &wires[index], &info) != TC_SNAPSHOT_OK) {
            text += format("wire index=%zu unreadable\n", index);
            continue;
        }
        text += format("wire index=%zu id=%llu from=%d,%d to=%d,%d width=%u slot=%llu flags=0x%x", index,
                       static_cast<unsigned long long>(info.id), info.x1, info.y1, info.x2, info.y2,
                       info.bit_width, static_cast<unsigned long long>(info.state_byte_offset), info.flags);
        /* The wire's current value is whatever the game left in that byte. */
        if (state_buffer && info.state_byte_offset < state_size) {
            text += format(" value=%u", static_cast<unsigned>(state_buffer[info.state_byte_offset]));
        }
        text += "\n";
    }
    return text;
}

}  // namespace

/* The frame callback: wait for a board, place the kind, then write the dump.
   One step per frame, so the queued command has time to run between the submit
   and the read. */
/* The V3 prefix of the board service, which is the shape the capture helper
   takes (see `dumpBoard`). */
TCBoardApiV3 makeV3(const TCBoardApiV4& board) {
    TCBoardApiV3 v3{};
    v3.size = sizeof(TCBoardApiV3);
    v3.version = TC_BOARD_API_VERSION_3;
    v3.context = board.context;
    v3.get_current = board.get_current;
    v3.validate = board.validate;
    v3.resolve = board.resolve;
    v3.capture_snapshot = board.capture_snapshot;
    v3.capture_objects = board.capture_objects;
    return v3;
}

/* One row of a sweep: every wire's id, its state offset and the byte the game
   left there.  Small enough to keep for each input value. */
std::string wireValues(TCBoardApiV4& board, const TCBoardApiV3& v3, const TCGameHandle& board_handle) {
    std::vector<TCGameHandle> components(64);
    std::vector<TCGameHandle> wires(64);
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    int status = TC_SNAPSHOT_ERR_CAPACITY;
    for (int attempt = 0; attempt < 3; ++attempt) {
        TCBoardObjectBuffersV1 buffers{};
        buffers.size = sizeof(buffers);
        buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
        buffers.components = components.data();
        buffers.component_capacity = components.size();
        buffers.wires = wires.data();
        buffers.wire_capacity = wires.size();
        TCBoardObjectSnapshotV1 probe{};
        probe.size = sizeof(probe);
        status = tc::captureBoardObjects(&v3, &board_handle, &probe, &buffers);
        if (status == TC_SNAPSHOT_OK) {
            snapshot = probe;
            break;
        }
        if (status != TC_SNAPSHOT_ERR_CAPACITY) break;
        components.resize(probe.component_count ? probe.component_count : 64);
        wires.resize(probe.wire_count ? probe.wire_count : 64);
    }
    if (status != TC_SNAPSHOT_OK) return format("FAIL sweep capture status=%d\n", status);
    std::string row;
    for (size_t index = 0; index < snapshot.wire_written && index < wires.size(); ++index) {
        TCWireInfoV1 info{};
        info.size = sizeof(info);
        if (tc::readWire(&board, &wires[index], &info) != TC_SNAPSHOT_OK) continue;
        if (!row.empty()) row += " ";
        /* A wire wider than one byte owns several slots in a row, and the game
           writes the value little-endian across them: read them all, or a 2-bit
           wire looks frozen at its low byte (measured: the decoder's own input
           wire read 0 while the level input read 0..3). */
        unsigned value = 0;
        const unsigned bytes = info.bit_width <= 8 ? 1u : (info.bit_width <= 16 ? 2u : 4u);
        for (unsigned index_byte = 0; index_byte < bytes; ++index_byte) {
            const uint64_t offset = info.state_byte_offset + index_byte;
            if (state_buffer && offset < state_size) {
                value |= static_cast<unsigned>(state_buffer[offset]) << (8u * index_byte);
            }
        }
        row += format("w%llu@%llu=%u", static_cast<unsigned long long>(info.id),
                      static_cast<unsigned long long>(info.state_byte_offset), value);
    }
    return row;
}

/* The hook whose first argument is the simulator model.  `handle_update_wire` is
   the same one tests/sim-state-probe.cpp captures it from; the loader allows one
   hook per function, and enter-board only took `igInvisibleButton`, so this one
   is free. */
bool (*update_wire_original)(void*, void*, void*, uint32_t, uint8_t) = nullptr;
/* The same model also arrives through the `sim.do` chain, which is the point that
   fires on every run request - a board that nobody touches never calls
   `handle_update_wire`, so this is the reliable half. */
int interceptedSimDo(TCHookCall* call) {
    auto* args = tc::hook::simDoArgs(call);
    logLine(format("kind-oracle: sim.do link fired, args=%p model=%p", static_cast<void*>(args),
                   args ? args->model : nullptr));
    if (args) sim_model = args->model;
    return 0;
}

bool hookedUpdateWire(void* model, void* context, void* input, uint32_t point, uint8_t flags) {
    sim_model = model;
    return update_wire_original ? update_wire_original(model, context, input, point, flags) : false;
}

/* Ask the game to compile this board again.  Until it does, the state array still
   holds the bytes the level was loaded with, which is why every earlier attempt
   read a frozen board (docs/research/simcore-kind-coverage.md sections 5.6-5.8). */
void recompile() {
    if (!preorder_fn || !sim_model) return;
    /* The context tests/component-capacity-probe.cpp uses: it puts the simulator
    into test mode first, and only then asks for the compile.  The game may well
    require that before it will rebuild a board. */
    if (set_sim_test_fn) set_sim_test_fn(sim_model, 0, 1);
    /* Retried every frame until the model shows up: it only arrives once the game
       has handled a run request, which is after the first sweep step. */
    if (recompiled) return;
    tc::TCNimString progress{};
    preorder_fn(sim_model, &progress, 0);
    recompiled = true;
    /* `preorder` returns nothing; the progress string is the only thing it hands
       back, so log it - it is how the game reports what the compile did. */
    if (progress.data && progress.length < 4096) {
        logLine("kind-oracle: preorder progress len=" + std::to_string(progress.length) + " text=" +
                std::string(static_cast<const char*>(progress.data) + 8, progress.length));
    } else {
        logLine("kind-oracle: preorder progress empty");
    }
    logLine("kind-oracle: asked the game to recompile the board");
}
/* The probe's frame state machine, rewritten as a flat pipeline.

   The version before this one nested its stages, and three separate bugs hid in
   that nesting (a status check behind the wrong branch, a compile request that
   was never reached, a log line that never printed).  This one has no nesting:
   one function per step, each returning the next step, and every transition
   logged.  Nothing is inferred - the log says which step ran. */
namespace oracle {

enum class Step {
    kWaitForBoard,
    kPlaceExtras,
    kPlaceTarget,
    kWaitForPlacement,
    kWantCompile,
    kRunOnce,
    kSweepWrite,
    kSweepWait,
    kDump,
    kDone,
};

const char* stepName(Step step) {
    switch (step) {
        case Step::kWaitForBoard: return "wait-for-board";
        case Step::kPlaceExtras: return "place-extras";
        case Step::kPlaceTarget: return "place-target";
        case Step::kWaitForPlacement: return "wait-for-placement";
        case Step::kWantCompile: return "want-compile";
        case Step::kRunOnce: return "run-once";
        case Step::kSweepWrite: return "sweep-write";
        case Step::kSweepWait: return "sweep-wait";
        case Step::kDump: return "dump";
        case Step::kDone: return "done";
    }
    return "?";
}

Step step = Step::kWaitForBoard;
uint64_t request = 0;
uint64_t sweep_value = 0;
int frames_in_step = 0;
std::vector<std::pair<uint64_t, std::string>> rows;
bool compile_attempted = false;
bool pending_slot_map = false;

void enter(Step next) {
    if (next != step) {
        logLine(format("kind-oracle: step %s -> %s (after %d frame(s))", stepName(step), stepName(next),
                       frames_in_step));
        step = next;
        frames_in_step = 0;
    }
}

TCBoardApiV3 v3of(const TCBoardApiV4& board) {
    TCBoardApiV3 v3{};
    v3.size = sizeof(TCBoardApiV3);
    v3.version = TC_BOARD_API_VERSION_3;
    v3.context = board.context;
    v3.get_current = board.get_current;
    v3.validate = board.validate;
    v3.resolve = board.resolve;
    v3.capture_snapshot = board.capture_snapshot;
    v3.capture_objects = board.capture_objects;
    return v3;
}

/* The wire table as `id@slot` pairs, so a recompile that renumbers the slots can
   be seen directly instead of inferred from zero values. */
std::string slotMap(TCBoardApiV4& board, const TCBoardApiV3& v3, const TCGameHandle& board_handle) {
    /* Both buffers have to be there: the loader refuses a missing component
       buffer even when only wires are wanted (native.hpp:508).  Learned by
       getting `unavailable` from a wires-only call. */
    std::vector<TCGameHandle> components(256);
    std::vector<TCGameHandle> wires(256);
    TCBoardObjectBuffersV1 buffers{};
    buffers.size = sizeof(buffers);
    buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    buffers.components = components.data();
    buffers.component_capacity = components.size();
    buffers.wires = wires.data();
    buffers.wire_capacity = wires.size();
    TCBoardObjectSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    const int status = tc::captureBoardObjects(&v3, &board_handle, &snapshot, &buffers);
    if (status != TC_SNAPSHOT_OK) return format("unavailable(status=%d)", status);
    std::string text = format("wires=%llu", static_cast<unsigned long long>(snapshot.wire_written));
    for (size_t index = 0; index < snapshot.wire_written && index < wires.size(); ++index) {
        TCWireInfoV1 info{};
        info.size = sizeof(info);
        if (tc::readWire(&board, &wires[index], &info) != TC_SNAPSHOT_OK) continue;
        text += format(" w%llu@%llu", static_cast<unsigned long long>(info.id),
                       static_cast<unsigned long long>(info.state_byte_offset));
    }
    return text;
}

/* Places one component through the command bus.  Returns 0 on success, the
   submit error otherwise, and leaves the request id in `request`. */
int place(TCCommandApiV2& commands, const TCGameHandle& board, int kind, int x, int y, int rotation) {
    TCCommandV2 command{};
    command.size = sizeof(command);
    command.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
    command.subject = board;
    command.kind = static_cast<uint32_t>(kind);
    command.x = x;
    command.y = y;
    command.rotation = static_cast<uint32_t>(rotation);
    return commands.submit(commands.context, &command, &request);
}

void frame() {
    ++frames_in_step;
    /* Every step transition is logged, and the current step is printed while a
       caller is still waiting: this is the line whose absence hid three bugs. */
    if (frames_in_step == 1 && step != Step::kDone) {
        logLine(format("kind-oracle: step %s (frame %d)", stepName(step), frames_in_step));
    }
    if (step == Step::kDone) return;

    TCBoardApiV4 board{};
    if (tc::queryService(host, TC_SERVICE_BOARD, TC_BOARD_API_VERSION_4, &board) != TC_SERVICE_OK ||
        !board.get_current) {
        finish("FAIL kind-oracle: tc.board V4 is unavailable\n");
        enter(Step::kDone);
        return;
    }
    TCGameHandle board_handle{};
    if (board.get_current(board.context, &board_handle) != TC_HANDLE_OK) {
        if (frames_in_step > 3000) {
            finish("FAIL kind-oracle saw no board in 3000 frames\n");
            enter(Step::kDone);
        }
        return; /* stay in kWaitForBoard */
    }

    TCCommandApiV2 commands{};
    const bool have_commands =
        tc::queryService(host, TC_SERVICE_COMMANDS, TC_COMMAND_API_VERSION_2, &commands) == TC_SERVICE_OK &&
        commands.submit != nullptr;

    switch (step) {
        case Step::kWaitForBoard:
            if (!have_commands) {
                finish("FAIL kind-oracle: tc.commands V2 is unavailable\n");
                enter(Step::kDone);
                return;
            }
            enter(extra_placements.empty() ? Step::kPlaceTarget : Step::kPlaceExtras);
            return;

        case Step::kPlaceExtras: {
            const auto& extra = extra_placements[static_cast<size_t>(extra_index)];
            const int submitted =
                place(commands, board_handle, std::get<0>(extra), std::get<1>(extra), std::get<2>(extra), 0);
            logLine(format("kind-oracle: extra %d kind=0x%02x at %d,%d submit=%d", extra_index,
                           std::get<0>(extra), std::get<1>(extra), std::get<2>(extra), submitted));
            if (submitted != TC_COMMAND_OK) {
                finish(format("FAIL kind-oracle extra submit=%d\n", submitted));
                enter(Step::kDone);
                return;
            }
            ++extra_index;
            enter(extra_index < static_cast<int>(extra_placements.size()) ? Step::kPlaceExtras
                                                                         : Step::kWaitForPlacement);
            return;
        }

        case Step::kPlaceTarget: {
            const int submitted = place(commands, board_handle, target_kind, target_x, target_y,
                                        target_rotation);
            logLine(format("kind-oracle: target kind=0x%02x at %d,%d rot=%d submit=%d request=%llu",
                           target_kind, target_x, target_y, target_rotation, submitted,
                           static_cast<unsigned long long>(request)));
            if (submitted != TC_COMMAND_OK) {
                finish(format("FAIL kind-oracle place submit=%d kind=0x%02x\n", submitted, target_kind));
                enter(Step::kDone);
                return;
            }
            enter(Step::kWaitForPlacement);
            return;
        }

        case Step::kWaitForPlacement: {
            if (!have_commands || !commands.get_status) {
                enter(Step::kWantCompile); /* no status API: give the game a few frames instead */
                return;
            }
            TCCommandStatusV1 status{};
            if (commands.get_status(commands.context, request, &status, sizeof(status)) != TC_COMMAND_OK) {
                enter(Step::kWantCompile);
                return;
            }
            if (status.state == TC_COMMAND_STATE_QUEUED || status.state == TC_COMMAND_STATE_RUNNING) {
                if (frames_in_step % 60 == 1) {
                    logLine(format("kind-oracle: placement still state=%u after %d frames", status.state,
                                   frames_in_step));
                }
                return;
            }
            logLine(format("kind-oracle: placement state=%u result=%d frame=%lld", status.state, status.result,
                           static_cast<long long>(status.completed_frame)));
            if (status.state != TC_COMMAND_STATE_SUCCEEDED) {
                finish(format("FAIL kind-oracle place state=%u result=%d kind=0x%02x\n", status.state,
                              status.result, target_kind));
                enter(Step::kDone);
                return;
            }
            enter(Step::kWantCompile);
            return;
        }

        case Step::kWantCompile: {
            /* The model pointer arrives through the sim.do chain, which only fires
               when the game handles a run request; give it a few frames, then ask
               for the compile exactly once. */
            if (frames_in_step < 10) return;            logLine(format("kind-oracle: want-compile model=%p preorder=%p after %d frames", sim_model,
                           reinterpret_cast<void*>(preorder_fn), frames_in_step));
            recompile();
            compile_attempted = true;
            logLine("kind-oracle: slots before = " + slotMap(board, v3of(board), board_handle));
            pending_slot_map = true;
            enter(run_cycles > 0 ? Step::kRunOnce : (sweep_input >= 0 ? Step::kSweepWrite : Step::kDump));
            return;
        }

        case Step::kRunOnce: {
            /* Start the level's own test.  With a test.si in the level folder the
               game drives the inputs and re-computes the board from it, which is
               the only way this board has ever been seen to change; this is the
               call tests/component-capacity-probe.cpp makes before compiling. */
            if (set_sim_test_fn && sim_model) {
                set_sim_test_fn(sim_model, 0, 1);
                logLine("kind-oracle: level test started (set_sim_test)");
            } else {
                logLine(format("kind-oracle: no test start: set_sim_test=%p model=%p",
                               reinterpret_cast<void*>(set_sim_test_fn), sim_model));
            }
            if (!have_commands) {
                enter(sweep_input >= 0 ? Step::kSweepWrite : Step::kDump);
                return;
            }
            TCCommandV2 run{};
            run.size = sizeof(run);
            run.type = TC_COMMAND_SIM_RUN;
            run.subject = board_handle;
            run.argument = run_cycles;
            uint64_t run_request = 0;
            const int submitted = commands.submit(commands.context, &run, &run_request);
            logLine(format("kind-oracle: sim.run submit=%d cycles=%d", submitted, run_cycles));
            /* The game's own verdict on the level's test lives in the state array:
               `sim_test_result` is StateIndex 2, i.e. byte 16 (test result) next to
               `sim_cycle` at byte 8.  This is how "the game checked my model" is
               read without touching anything else. */
            if (state_buffer && state_size > 24) {
                logLine(format("kind-oracle: game state cycle=%lld test_result=%u (parsed cycle=%lld)",
                               static_cast<long long>(*reinterpret_cast<int64_t*>(state_buffer + 8)),
                               static_cast<unsigned>(state_buffer[16]),
                               static_cast<long long>(*reinterpret_cast<int64_t*>(state_buffer + 8))));
            }
            enter(sweep_input >= 0 ? Step::kSweepWrite : Step::kDump);
            return;
        }

        case Step::kSweepWrite: {
            /* Let the game run its own test: with a test.si present the board is
               computed by that test, and only a running simulation advances it. */
            if (have_commands) {
                TCCommandV2 run{};
                run.size = sizeof(run);
                run.type = TC_COMMAND_SIM_RUN;
                run.subject = board_handle;
                run.argument = 30;
                uint64_t run_request = 0;
                commands.submit(commands.context, &run, &run_request);
            }
            TCIoValueApiV1 io{};
            if (tc::io_value::table(host, &io)) {
                const int written = tc::io_value::writeInput(io, board_handle,
                                                             static_cast<uint64_t>(sweep_input), sweep_value);
                logLine(format("kind-oracle: sweep input %d <= %llu (status %d)", sweep_input,
                               static_cast<unsigned long long>(sweep_value), written));
            } else {
                finish("FAIL kind-oracle: tc.io_value is unavailable\n");
                enter(Step::kDone);
                return;
            }
            enter(Step::kSweepWait);
            return;
        }

        case Step::kSweepWait: {
            if (frames_in_step < 120) return;
            if (pending_slot_map) {
                pending_slot_map = false;
                logLine("kind-oracle: slots after  = " + slotMap(board, v3of(board), board_handle));
            }
            const std::string row = wireValues(board, v3of(board), board_handle);
            if (row.rfind("FAIL", 0) == 0) {
                finish(row);
                enter(Step::kDone);
                return;
            }
            rows.emplace_back(sweep_value, row);
            if (sweep_value >= sweep_to) {
                std::string text = format("sweep input=%d range=%llu..%llu rows=%zu\n", sweep_input,
                                          static_cast<unsigned long long>(sweep_from),
                                          static_cast<unsigned long long>(sweep_to), rows.size());
                for (const auto& entry : rows) {
                    text += format("  in=%llu %s\n", static_cast<unsigned long long>(entry.first),
                                   entry.second.c_str());
                }
                finish(text);
                enter(Step::kDone);
                return;
            }
            ++sweep_value;
            enter(Step::kSweepWrite);
            return;
        }

        case Step::kDump:
            finish(dumpBoard(board));
            enter(Step::kDone);
            return;

        case Step::kDone:
            return;
    }
}

}  // namespace oracle

void frame(void*, const TCFrame*) { oracle::frame(); }


/* The chain that turns a home-page button press into a board entry.  This probe
   has to own `igInvisibleButton` because only one hook can: `enter_board_load`
   installs its own on top of ours when it sees `host->create_hook` already
   taken, so the final chain is oracle -> enter-board -> original. */
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION || plugin->size < sizeof(TCPlugin)) return 1;
    host = h;
    target_kind = parseNumber(std::getenv("TC_KIND_ORACLE_KIND"), -1);
    target_x = parseNumber(std::getenv("TC_KIND_ORACLE_X"), 0);
    target_y = parseNumber(std::getenv("TC_KIND_ORACLE_Y"), 0);
    target_rotation = parseNumber(std::getenv("TC_KIND_ORACLE_ROTATION"), 0);
    run_cycles = parseNumber(std::getenv("TC_KIND_ORACLE_RUN"), 0);
    sweep_input = parseNumber(std::getenv("TC_KIND_ORACLE_SWEEP_INPUT"), -1);
    sweep_from = static_cast<uint64_t>(parseNumber(std::getenv("TC_KIND_ORACLE_SWEEP_FROM"), 0));
    sweep_to = static_cast<uint64_t>(parseNumber(std::getenv("TC_KIND_ORACLE_SWEEP_TO"), 0));
    sweep_value = sweep_from;
    if (const char* extra = std::getenv("TC_KIND_ORACLE_EXTRA")) {
        int kind = 0, x = 0, y = 0;
        const char* cursor = extra;
        while (std::sscanf(cursor, "%i@%i,%i", &kind, &x, &y) == 3) {
            extra_placements.emplace_back(kind, x, y);
            const char* next = std::strchr(cursor, ';');
            if (!next) break;
            cursor = next + 1;
        }
    }
    /* A sweep has to run the simulation between values, so it implies a run even
       when the caller forgot TC_KIND_ORACLE_RUN.  Learned the hard way: the first
       sweep run dumped the board, because without a run the stage machine never
       entered the sweep. */
    if (sweep_input >= 0 && run_cycles <= 0) run_cycles = 1;
    logLine(format("kind-oracle: config kind=0x%02x run=%d sweep_input=%d range=%llu..%llu", target_kind,
                   run_cycles, sweep_input, static_cast<unsigned long long>(sweep_from),
                   static_cast<unsigned long long>(sweep_to)));

    /* The game's simulation state array: a wire record's `state_byte_offset` is
       an index into the buffer this global points at. */
    if (void* state_global = h->resolve_symbol(h->context, "simulation_state__modelZsimulator95types_u81")) {
        state_buffer = *reinterpret_cast<unsigned char**>(state_global);
        state_size = 0x9c4000; /* the simulation's own allocation, from its init */
    }
    const auto folder = std::filesystem::u8path(h->data_directory_utf8);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    data_folder = folder;
    if (target_kind < 1 || target_kind > 0xff) {
        finish("FAIL kind-oracle needs TC_KIND_ORACLE_KIND\n");
        return 2;
    }

    /* Enter a board first: the level loader is the only way from the menu to a
       board, and the dump needs a board.  `enter_board_load` installs the one
       `igInvisibleButton` hook the loader allows (a second hook on the same
       function is refused - measured: "could not chain onto the board entry
       hook"), and its frame callback stays ours. */
    if (const int armed = enter_board_probe::enter_board_load(h, plugin); armed != 0) {
        finish(format("FAIL kind-oracle could not arm the board entry (step %d)\n", armed));
        return 5;
    }
    /* Capture the simulator model from handle_update_wire, and resolve the
       game's compile entry point.  Both are needed to recompute the board. */
    if (void* update_target = h->resolve_symbol(h->context,
            "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5")) {
        if (h->create_hook(h->context, update_target, reinterpret_cast<void*>(hookedUpdateWire),
                           reinterpret_cast<void**>(&update_wire_original)) == 0) {
            logLine("kind-oracle: model capture armed");
        } else {
            logLine("kind-oracle: could not hook handle_update_wire; the board cannot be recompiled");
        }
    }
    if (tc::hook::addSimDo(h, 0, &interceptedSimDo, nullptr) == TC_HOOK_OK) {
        logLine("kind-oracle: joined the sim.do chain for the model pointer");
    } else {
        logLine("kind-oracle: could not join the sim.do chain");
    }    void* preorder_symbol = h->resolve_symbol(
        h->context, "preorder__modelZsimulationZcompile95thread_u3523");
    preorder_fn = reinterpret_cast<Preorder>(preorder_symbol);
    set_sim_test_fn = reinterpret_cast<SetSimTest>(h->resolve_symbol(
        h->context, "set_sim_test__modelZutilities_u6840"));
    logLine(format("kind-oracle: set_sim_test=%p", reinterpret_cast<void*>(set_sim_test_fn)));
    logLine(format("kind-oracle: preorder symbol=%p fn=%p resolve=%p", preorder_symbol,
                   reinterpret_cast<void*>(preorder_fn), reinterpret_cast<void*>(h->resolve_symbol)));
    plugin->on_frame = frame;
    return 0;
}
