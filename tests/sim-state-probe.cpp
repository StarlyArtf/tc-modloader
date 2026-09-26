// Route 1 reconnaissance: map circuit pins to simulation state slots.
//
// Runs the stock and_gate level one cycle at a time and snapshots the whole
// simulation state after each cycle.  The level's own test defines the
// expected output sequence (0,0,0,1) for inputs (0,0),(0,1),(1,0),(1,1), so
// byte slots whose value sequence matches a known pattern are the pin storage
// we are looking for.  Read-only: it never writes game state.

#include "../sdk/tc_mod.h"
#include "../sdk/tc_custom_logic.h"
#include "../simcore/include/tcsim/board.hpp"
#include "../simcore/include/tcsim/devices.hpp"
#include "../simcore/include/tcsim/engine.hpp"
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct V2 {
    float x, y;
};

constexpr uint64_t kSimulationStateSize = 0x9c4000;  // c_alloc in simulator init
constexpr uint64_t kSmallBufferSize = 0x1000;
constexpr int64_t kSnapshotCount = 4;

const TCHost* host;
tc::TCMod mod;
void* model = nullptr;
bool model_logged = false;

using LoadLevel = void (*)(void*, const tc::TCNimString*);
LoadLevel load_level;
using SetSimTest = void (*)(void*, int64_t, uint8_t);
SetSimTest set_sim_test;
using CompileRequest = void (*)(void*, const tc::TCNimString*, uint8_t);
CompileRequest compile_request;

using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
UpdateWire update_original;
using SimDo = void (*)(void*, uint8_t, int64_t);
SimDo sim_do_original;
using InvisibleButton = bool (*)(const char*, V2, int);
InvisibleButton invisible_original;
using StateReadU64 = uint64_t (*)(int64_t);
StateReadU64 state_read_original;
using GetSimState = void* (*)(void*, const void*);
GetSimState get_sim_state_original;
using GetTestState = int64_t (*)();
GetTestState get_test_state;

bool started = false;
double start_time = 0;
double elapsed = 0;
int stage = 0;
double stage_time = 0;
bool done = false;
/* Deep mode (TC_SIM_STATE_DEEP=1) dumps what S2 needs to know: the whole
   component record, the tables its unknown pointers lead to, and which game
   call site reads which state offset.  The write test
   (TC_SIM_STATE_WRITE=1) then answers the question S2's write-back contract
   rests on: if we put a value in the state array ourselves, does it stay
   there, and does the game keep reading it? */
bool deep_dump = false;
bool write_test = false;
/* Where does the game keep the cycle counter?  `sim_get_cycle` reads it, the
   simulation thread increments it, and ownership of it is what S2's advance
   control needs (the user has allowed touching it).  TC_SIM_STATE_CYCLE=1 scans
   the model the sim.do chain hands over for an int64 that equals the current
   cycle and then goes up by exactly one: same method as finding the state
   slots.  Read-only. */
bool cycle_probe = false;
int64_t probe_last_cycle = -2;
std::vector<size_t> probe_candidates;
std::map<size_t, std::pair<int64_t, int64_t>> probe_hits;
std::string cycle_report;
/* Drive mode (TC_SIM_STATE_DRIVE=1) is S2 in one experiment: build a netlist
   from the live board, run simcore for that circuit, and put *our* value into
   the game's state array.  TC_SIM_STATE_INVERT=1 writes the opposite of the
   value we computed, which is how the probe proves the slot shows what we wrote
   rather than what the game's own program wrote a moment earlier. */
bool drive_test = false;
bool drive_invert = false;
int drive_cycles = 4;
int drive_remaining = 0;
int64_t drive_next_cycle = -1;
tcsim::Engine* drive_engine = nullptr;
tcsim::BuildResult drive_build;
std::ostringstream drive_log;
int64_t write_cycle = -1;
std::vector<uint64_t> write_offsets;
std::map<uint64_t, unsigned char> write_before;
std::map<uint64_t, unsigned char> write_after;
std::map<uint64_t, uint64_t> write_reads_after;
int64_t desired_cycle = 1;
int test_frame = -1;
int test_button_index = 0;
bool imported_custom = false;
std::string custom_logic;
tc::TCCustomLogicRuntime custom_runtime;
void* input_replay_global = nullptr;
void* output_history_global = nullptr;
unsigned char* smallBuffer(void* global);

constexpr uint64_t kCustomComponentId = 0x414E44325F303031ULL;
/* Pattern the write test puts into the state array: 1010_0101, so a 1-bit slot
   reads back as 1 and a stale byte is obvious. */
constexpr unsigned char kWriteMarker = 0xA5;

void customLogicOr(tc::TCCustomLogicIO* io) {
    io->outputs[0] = (io->inputs[0] | io->inputs[1]) & 1;
}

uint64_t customLevelInput(int64_t cycle, uint32_t pin, void*) {
    const uint64_t value = cycle < 0 ? 0 : (static_cast<uint64_t>(cycle) & 3);
    return pin == 0 ? (value & 1) : ((value >> 1) & 1);
}

void customLevelInputWrite(int64_t cycle, uint32_t, uint64_t, void*) {
    unsigned char* replay = smallBuffer(input_replay_global);
    if (replay) {
        const auto input = static_cast<unsigned char>(cycle < 0 ? 0 : (cycle & 3));
        replay[0] = replay[8] = input;
    }
}

void customLevelOutputWrite(int64_t, uint32_t, uint64_t value, void*) {
    unsigned char* history = smallBuffer(output_history_global);
    if (history) history[55] = history[64] = static_cast<unsigned char>(value & 1);
}

int64_t customTest(int64_t cycle, const uint64_t* outputs, uint32_t, void*) {
    if (cycle < 0) return 0;
    if (cycle >= 4) return 1;
    static const uint8_t expected[4] = {0, 0, 0, 1};
    if ((outputs[0] & 1) != expected[cycle]) return 2;
    return cycle == 3 ? 1 : 0;
}

void* state_global = nullptr;  // address of the raw simulation_state pointer
unsigned char* state_buffer = nullptr;
std::vector<std::vector<unsigned char>> snapshots;
std::vector<std::vector<unsigned char>> input_replay_snapshots;
std::vector<std::vector<unsigned char>> output_history_snapshots;
std::vector<int64_t> snapshot_cycles;
std::mutex read_mutex;
std::map<int64_t, uint64_t> state_read_counts;
std::map<int64_t, uint64_t> state_read_last;
/* Which game code reads which slot: the drawing path and the simulation path
   ask for different offsets, and telling them apart is what answers "if we
   write this slot, does the board show it?".  Key is (byte offset, caller). */
std::map<std::pair<int64_t, uintptr_t>, uint64_t> state_read_callers;
std::map<int64_t, uint64_t> state_read_last_caller;
/* Thread split: the render thread reads a slot to draw it, the simulation
   thread reads it to evaluate a component.  Knowing which is which is what
   says whether a slot is a write-back target. */
std::map<std::pair<int64_t, uint32_t>, uint64_t> state_read_threads;
uint32_t main_thread_id = 0;
std::mutex gss_mutex;
std::map<std::string, std::pair<uint64_t, uint64_t>> gss_stats;
std::map<std::string, uint64_t> gss_last1;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

/* The write test's targets: every wire's own state byte (the record at +0x38
   is the offset the game's own reader uses, verified by tc.sim.channel), plus
   the low node window the observations showed the renderer reading. */
void collectWriteTargets() {
    write_offsets.clear();
    if (model) {
        const auto* base = static_cast<const unsigned char*>(model);
        uint64_t wires = 0;
        void* wire_data = nullptr;
        std::memcpy(&wires, base + 0x98, sizeof(wires));
        std::memcpy(&wire_data, base + 0xa0, sizeof(wire_data));
        if (wire_data && wires <= 64) {
            for (uint64_t index = 0; index < wires; ++index) {
                const auto* wire = static_cast<const unsigned char*>(wire_data) + 8 + index * 0x68;
                uint64_t slot = 0;
                std::memcpy(&slot, wire + 0x38, 8);
                if (slot + 8 <= kSimulationStateSize) write_offsets.push_back(slot);
            }
        }
    }
    for (uint64_t offset = 256; offset < 272; ++offset) write_offsets.push_back(offset);
}

std::string hex(void* value) {
    std::ostringstream out;
    out << "0x" << std::hex << reinterpret_cast<uintptr_t>(value) << std::dec;
    return out.str();
}

bool readState() {
    if (!state_global) {
        log("sim-state: missing simulation_state global");
        return false;
    }
    unsigned char* buffer =
        *reinterpret_cast<unsigned char**>(state_global);
    if (!buffer || reinterpret_cast<uintptr_t>(buffer) < 0x10000) {
        log("sim-state: invalid simulation_state pointer " + hex(buffer));
        return false;
    }
    state_buffer = buffer;
    snapshots.emplace_back(buffer, buffer + kSimulationStateSize);

    const auto copy_global = [&](void* global,
                                 std::vector<std::vector<unsigned char>>& out) {
        if (!global) return;
        unsigned char* small =
            *reinterpret_cast<unsigned char**>(global);
        if (!small || reinterpret_cast<uintptr_t>(small) < 0x10000) return;
        out.emplace_back(small, small + kSmallBufferSize);
    };
    copy_global(input_replay_global, input_replay_snapshots);
    copy_global(output_history_global, output_history_snapshots);

    snapshot_cycles.push_back(mod.simulation.cycle());
    return true;
}

unsigned char* smallBuffer(void* global) {
    if (!global) return nullptr;
    unsigned char* buffer = *reinterpret_cast<unsigned char**>(global);
    if (!buffer || reinterpret_cast<uintptr_t>(buffer) < 0x10000) return nullptr;
    return buffer;
}

/* A pointer read out of a record is only followed when the whole range is one
   committed, readable region - the same rule src/board_objects.hpp uses, kept
   local so this probe stays a single translation unit. */
bool readableRegion(const void* address, size_t bytes) {
    if (!address || !bytes) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto* begin = static_cast<const unsigned char*>(region.BaseAddress);
    const auto offset = static_cast<size_t>(static_cast<const unsigned char*>(address) - begin);
    return offset + bytes <= region.RegionSize;
}

std::string hexBytes(const unsigned char* bytes, size_t count) {
    std::ostringstream out;
    for (size_t index = 0; index < count; ++index) {
        char buffer[4];
        std::snprintf(buffer, sizeof(buffer), "%02x", bytes[index]);
        out << buffer;
    }
    return out.str();
}

/* ---------------------------------------------------------------------------
   S2's board -> netlist step, running live: the pin geometry comes from the
   game's own prototype table, the records from the board, and simcore does the
   rest.  Nothing here reads an emitted program. */

bool prototypeGeometry(const tcsim::BoardComponent& component, tcsim::KindPins& out) {
    tc::TCPrototype prototype{};
    if (!mod.game.getPrototype(component.kind, component.custom_id, prototype)) return false;
    const uint64_t inputs = tc::prototypeInputCount(prototype);
    const uint64_t outputs = tc::prototypeOutputCount(prototype);
    if (inputs > 8 || outputs > 4) return false;
    out = tcsim::KindPins{};
    out.kind = component.kind;
    out.name = tc::prototypeNameCStr(prototype);
    out.input_count = static_cast<uint8_t>(inputs);
    out.output_count = static_cast<uint8_t>(outputs);
    for (uint64_t index = 0; index < inputs; ++index) {
        const tc::TCPinPoint point = tc::prototypeInputPinPoint(prototype, index);
        out.in_x[index] = static_cast<int8_t>(point.x);
        out.in_y[index] = static_cast<int8_t>(point.y);
        const uint64_t raw = tc::prototypeInputPinWordSize(prototype, index);
        out.in_bits[index] = tc::pinWordSizeIsAuto(raw) || raw == 0
                                 ? 1
                                 : static_cast<uint16_t>(raw < 64 ? raw : 64);
    }
    for (uint64_t index = 0; index < outputs; ++index) {
        const tc::TCPinPoint point = tc::prototypeOutputPinPoint(prototype, index);
        out.out_x[index] = static_cast<int8_t>(point.x);
        out.out_y[index] = static_cast<int8_t>(point.y);
        const uint64_t raw = tc::prototypeOutputPinWordSize(prototype, index);
        out.out_bits[index] = tc::pinWordSizeIsAuto(raw) || raw == 0
                                  ? 1
                                  : static_cast<uint16_t>(raw < 64 ? raw : 64);
    }
    return true;
}

/* Reads a net's current byte out of the game's state array, the way the game's
   own reader would. */
uint64_t readNetByte(const std::vector<uint64_t>& slots) {
    if (slots.empty()) return 0;
    unsigned char* buffer = *reinterpret_cast<unsigned char**>(state_global);
    if (!buffer) return 0;
    if (state_read_original) return state_read_original(static_cast<int64_t>(slots[0])) & 0xffu;
    return buffer[slots[0]] & 0xffu;
}

void writeNetByte(const std::vector<uint64_t>& slots, uint64_t value) {
    unsigned char* buffer = *reinterpret_cast<unsigned char**>(state_global);
    if (!buffer) return;
    for (uint64_t slot : slots) {
        if (slot < kSimulationStateSize) buffer[slot] = static_cast<unsigned char>(value & 1u);
    }
}

/* Scans the model for the cycle counter: every aligned u64 that reads as the
   current cycle is a candidate, and one that goes up by exactly one on the next
   cycle is the field.  Bounded to the first page, which is where the game's own
   simulation structs live. */
void probeCycleField() {
    if (!cycle_probe || !model) return;
    const int64_t cycle = mod.simulation.cycle();
    if (cycle < 0 || cycle == probe_last_cycle) return;
    const auto* base = static_cast<const unsigned char*>(model);
    std::vector<size_t> hits;
    for (size_t offset = 0; offset + 8 <= 0x800; offset += 8) {
        int64_t value = 0;
        std::memcpy(&value, base + offset, 8);
        if (value == cycle) hits.push_back(offset);
    }
    if (probe_last_cycle >= 0 && cycle == probe_last_cycle + 1) {
        for (size_t offset : hits) {
            if (std::find(probe_candidates.begin(), probe_candidates.end(), offset) == probe_candidates.end()) continue;
            probe_hits[offset] = {cycle - 1, cycle};
        }
    }
    probe_candidates = hits;
    probe_last_cycle = cycle;
    if (probe_hits.empty()) return;
    std::ostringstream text;
    text << "cycle_field model=" << hex(model) << " candidates_this_cycle=" << hits.size() << "\n";
    for (const auto& entry : probe_hits) {
        text << "cycle_field offset=0x" << std::hex << entry.first << std::dec << " value=" << entry.second.first
             << "->" << entry.second.second << "\n";
    }
    cycle_report = text.str();
}

bool hookedUpdate(void* m, void* context, void* input, uint32_t point,
                  uint8_t fifth) {
    model = m;
    if (model && !model_logged) {
        model_logged = true;
        log("sim-state: model captured from handle_update_wire " + hex(model));
    }
    return update_original ? update_original(m, context, input, point, fifth)
                           : false;
}

/* A link in the loader's sim.do chain: several mods may watch or steer run
   requests at once, and returning non-zero (with skip_original set) is how this
   probe keeps a run request from reaching the game while it drives the level
   itself. */
int interceptedSimDo(TCHookCall* call) {
    auto* args = tc::hook::simDoArgs(call);
    if (!args) return 0;
    model = args->model;
    if (model && !model_logged) {
        model_logged = true;
        log("sim-state: model captured from sim_do " + hex(model));
    }
    if (imported_custom && !custom_logic.empty() && args->command == 0) {
        if (custom_runtime.run(mod, args->model, args->target)) {
            call->skip_original = 1;
            return 1;
        }
    }
    return 0;
}

uint64_t hookedStateReadU64(int64_t index) {
    uint64_t value = state_read_original ? state_read_original(index) : 0;
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                             reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::lock_guard<std::mutex> lock(read_mutex);
    ++state_read_counts[index];
    state_read_last[index] = value;
    ++state_read_callers[{index, caller}];
    state_read_last_caller[index] = caller;
    ++state_read_threads[{index, static_cast<uint32_t>(GetCurrentThreadId())}];
    return value;
}

void* hookedGetSimState(void* result, const void* descriptor) {
    void* out = get_sim_state_original
                    ? get_sim_state_original(result, descriptor)
                    : nullptr;
    uint64_t value = 0;
    uint64_t value1 = 0;
    if (result) std::memcpy(&value, result, sizeof(value));
    if (result) std::memcpy(&value1, static_cast<const unsigned char*>(result) + 8,
                             sizeof(value1));
    std::string key = descriptor ? "desc:" : "desc:null";
    if (descriptor) {
        const auto* bytes = static_cast<const unsigned char*>(descriptor);
        std::ostringstream text;
        for (size_t i = 0; i < 0x40; ++i) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02x", bytes[i]);
            text << buf;
        }
        key += text.str();
    }
    std::lock_guard<std::mutex> lock(gss_mutex);
    auto& entry = gss_stats[key];
    entry.first += 1;
    entry.second = value;
    gss_last1[key] = value1;
    return out;
}

bool hookedInvisible(const char* id, V2 size, int flags) {
    const bool result = invisible_original(id, size, flags);
    const auto rva = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                     reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (rva >= 0x449df0 && rva < 0x44b610) {
        const auto get_frame = reinterpret_cast<int (*)()>(
            host->engine_proc(host->context, "igGetFrameCount"));
        const int frame = get_frame ? get_frame() : 0;
        if (frame != test_frame) {
            test_frame = frame;
            test_button_index = 0;
        }
        ++test_button_index;
        if (elapsed > 4.0 && test_button_index == 2) return true;
    }
    return result;
}

void loadLevel() {
    const char* text = "and_gate";
    const size_t length = std::strlen(text);
    tc::TCNimString name{};
    mod.game.raw_new_string(&name, static_cast<int64_t>(length));
    name.length = length;
    std::memcpy(static_cast<unsigned char*>(name.data) + 8, text, length);
    static_cast<unsigned char*>(name.data)[8 + length] = 0;
    load_level(model, &name);
}

void finish() {
    if (done) return;
    done = true;

    std::ostringstream report;
    report << "state_buffer=" << hex(state_buffer)
           << " state_size=" << kSimulationStateSize
           << " snapshots=" << snapshots.size() << " cycles=";
    for (int64_t cycle : snapshot_cycles) report << cycle << " ";
    report << "\n";

    if (!snapshots.empty()) {
        const auto& first = snapshots.front();
        uint64_t changed = 0;
        uint64_t printed = 0;
        constexpr uint64_t kPrintedLimit = 10000;
        for (uint64_t index = 0; index < kSimulationStateSize; ++index) {
            bool varies = false;
            for (const auto& snap : snapshots) {
                if (snap[index] != first[index]) {
                    varies = true;
                    break;
                }
            }
            if (!varies) continue;
            ++changed;
            if (printed++ < kPrintedLimit) {
                report << "slot " << index << " seq=";
                for (const auto& snap : snapshots) {
                    report << static_cast<int>(snap[index]) << ",";
                }
                report << "\n";
            }
        }
        report << "changed_slots=" << changed;
        if (printed > kPrintedLimit) report << " printed=" << kPrintedLimit;
        report << "\n";
    } else {
        report << "changed_slots=0\n";
    }

    if (snapshots.size() == 4) {
        for (uint64_t word_offset : {256u, 264u, 272u, 280u}) {
            report << "qword " << word_offset << " seq=";
            for (const auto& snap : snapshots) {
                uint64_t value = 0;
                if (word_offset + 8 <= snap.size()) {
                    std::memcpy(&value, snap.data() + word_offset, 8);
                }
                report << value << ",";
            }
            report << "\n";
        }
    }

    const auto report_small = [&](const char* label,
                                  const std::vector<std::vector<unsigned char>>& snaps) {
        report << label << " snapshots=" << snaps.size() << "\n";
        if (snaps.empty()) return;
        const auto& first = snaps.front();
        uint64_t changed = 0;
        uint64_t printed = 0;
        constexpr uint64_t kPrintedLimit = 2000;
        for (uint64_t index = 0; index < kSmallBufferSize; ++index) {
            bool varies = false;
            for (const auto& snap : snaps) {
                if (snap[index] != first[index]) {
                    varies = true;
                    break;
                }
            }
            if (!varies) continue;
            ++changed;
            if (printed++ < kPrintedLimit) {
                report << label << " slot " << index << " seq=";
                for (const auto& snap : snaps) {
                    report << static_cast<int>(snap[index]) << ",";
                }
                report << "\n";
            }
        }
        report << label << " changed_slots=" << changed;
        if (printed > kPrintedLimit) report << " printed=" << kPrintedLimit;
        report << "\n";
    };
    report_small("input_replay", input_replay_snapshots);
    report_small("output_history", output_history_snapshots);

    if (model) {
        auto* base = static_cast<unsigned char*>(model);
        uint64_t components = 0;
        uint64_t wires = 0;
        void* component_data = nullptr;
        void* wire_data = nullptr;
        std::memcpy(&components, base + 0x78, sizeof(components));
        std::memcpy(&component_data, base + 0x80, sizeof(component_data));
        std::memcpy(&wires, base + 0x98, sizeof(wires));
        std::memcpy(&wire_data, base + 0xa0, sizeof(wire_data));
        report << "board components=" << components << " wires=" << wires << "\n";
        if (component_data && components <= 32) {
            for (uint64_t i = 0; i < components; ++i) {
                const auto* component =
                    static_cast<const unsigned char*>(component_data) + 8 + i * 0x238;
                uint16_t kind = 0;
                int16_t x = 0;
                int16_t y = 0;
                uint64_t id = 0;
                std::memcpy(&kind, component, sizeof(kind));
                std::memcpy(&x, component + 2, sizeof(x));
                std::memcpy(&y, component + 4, sizeof(y));
                std::memcpy(&id, component + 0x188, sizeof(id));
                report << "board_component " << i << " kind=0x" << std::hex
                       << kind << std::dec << " pos=" << x << "," << y
                       << " id=" << id << " bytes=";
                for (size_t j = 0; j < 0x40; ++j) {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "%02x", component[j]);
                    report << buf;
                }
                report << "\n";
            }
        }
        if (wire_data && wires <= 32) {
            for (uint64_t i = 0; i < wires; ++i) {
                const auto* wire =
                    static_cast<const unsigned char*>(wire_data) + 8 + i * 0x68;
                report << "board_wire " << i << " bytes=";
                for (size_t j = 0; j < 0x60; ++j) {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "%02x", wire[j]);
                    report << buf;
                }
                report << "\n";
            }
        }
        if (deep_dump && wire_data && wires <= 64) {
            for (uint64_t i = 0; i < wires; ++i) {
                const auto* wire =
                    static_cast<const unsigned char*>(wire_data) + 8 + i * 0x68;
                int16_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                uint32_t width = 0;
                uint64_t slot = 0;
                std::memcpy(&x1, wire + 0x18, 2);
                std::memcpy(&y1, wire + 0x1a, 2);
                std::memcpy(&x2, wire + 0x1c, 2);
                std::memcpy(&y2, wire + 0x1e, 2);
                std::memcpy(&width, wire + 0x30, 4);
                std::memcpy(&slot, wire + 0x38, 8);
                report << "deep_wire " << i << " from=" << x1 << "," << y1 << " to=" << x2 << "," << y2
                       << " width=" << width << " slot=" << slot << "\n";
            }
        }
        if (deep_dump && component_data && components <= 64) {
            for (uint64_t i = 0; i < components; ++i) {
                const auto* component =
                    static_cast<const unsigned char*>(component_data) + 8 + i * 0x238;
                report << "deep_component " << i << " bytes=" << hexBytes(component, 0x1a0) << "\n";
                for (size_t offset : {size_t{0x28}, size_t{0x30}, size_t{0x38}, size_t{0x40}}) {
                    uint64_t value = 0;
                    std::memcpy(&value, component + offset, 8);
                    report << "deep_component " << i << " word@0x" << std::hex << offset << std::dec
                           << "=" << value;
                    if (readableRegion(reinterpret_cast<const void*>(value), 0x40)) {
                        report << " pointee=" << hexBytes(reinterpret_cast<const unsigned char*>(value), 0x40);
                    }
                    report << "\n";
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(read_mutex);
        report << "state_read_u64 observed indices=" << state_read_counts.size() << "\n";
        uint64_t printed = 0;
        for (const auto& entry : state_read_counts) {
            if (printed++ < 200) {
                report << "read_index " << entry.first << " calls=" << entry.second
                       << " last=" << state_read_last[entry.first] << "\n";
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(gss_mutex);
        report << "get_sim_state observed descriptors=" << gss_stats.size() << "\n";
        uint64_t printed = 0;
        for (const auto& entry : gss_stats) {
            if (printed++ < 100) {
                report << entry.first << " calls=" << entry.second.first
                       << " last0=" << entry.second.second
                       << " last1=" << gss_last1[entry.first] << "\n";
            }
        }
    }

    if (deep_dump) {
        std::lock_guard<std::mutex> lock(read_mutex);
        report << "main_thread=" << main_thread_id << "\n";
        std::map<int64_t, std::pair<uint64_t, uint64_t>> by_slot;
        for (const auto& entry : state_read_threads) {
            const int64_t offset = entry.first.first;
            const bool render = entry.first.second == main_thread_id;
            auto& counts = by_slot[offset];
            if (render) counts.first += entry.second;
            else counts.second += entry.second;
        }
        report << "deep_read_slots=" << by_slot.size() << "\n";
        uint64_t printed = 0;
        for (const auto& entry : by_slot) {
            if (printed++ >= 160) break;
            report << "read_slot " << entry.first << " render=" << entry.second.first
                   << " other=" << entry.second.second << "\n";
        }
        printed = 0;
        report << "deep_read_callers=" << state_read_callers.size() << "\n";
        for (const auto& entry : state_read_callers) {
            if (printed++ >= 160) break;
            report << "read_caller offset=" << entry.first.first << " caller_rva=0x" << std::hex
                   << entry.first.second << std::dec << " calls=" << entry.second << "\n";
        }
    }

    if (write_test) {
        report << "write_test cycle=" << write_cycle << " slots=" << write_offsets.size() << "\n";
        for (uint64_t offset : write_offsets) {
            report << "write_slot " << offset << " marker=" << static_cast<int>(kWriteMarker)
                   << " before=" << static_cast<int>(write_before[offset])
                   << " after=" << static_cast<int>(write_after[offset])
                   << " reads_after=" << write_reads_after[offset] << "\n";
        }
    }

    if (drive_test) {
        report << "drive_invert=" << (drive_invert ? 1 : 0) << " cycles=" << drive_cycles << "\n";
        report << drive_log.str();
    }
    if (cycle_probe) {
        report << "cycle_probe=1\n";
        if (cycle_report.empty()) {
            report << "cycle_field none: no aligned word in the first page tracked the cycle\n";
        } else {
            report << cycle_report;
        }
    }

    const auto folder = std::string(host->data_directory_utf8);
    std::ofstream(folder + "/state-map.txt") << report.str();
    log("sim-state: saved state-map.txt; snapshots=" +
        std::to_string(snapshots.size()) +
        " state_size=" + std::to_string(kSimulationStateSize));
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
        main_thread_id = static_cast<uint32_t>(GetCurrentThreadId());
    }
    elapsed = value->time_seconds - start_time;
    if (done || !model || elapsed < 5.0) return;
    probeCycleField();

    if (stage == 0) {
        loadLevel();
        log("sim-state: loaded and_gate with model " + hex(model));
        stage = 1;
        stage_time = elapsed;
        return;
    }

    if (stage == 1) {
        if (elapsed < stage_time + 2.0) return;
        if (set_sim_test) set_sim_test(model, 0, 1);
        tc::TCNimString progress{};
        if (compile_request) {
            compile_request(model, &progress, 0);
        }
        log("sim-state: requested async compilation");
        stage = 2;
        stage_time = elapsed;
        return;
    }

    if (stage == 2) {
        if (elapsed < stage_time + 3.0) return;
        const int64_t now = mod.simulation.cycle();
        if (readState()) {
            log("sim-state: baseline snapshot at cycle " + std::to_string(now) +
                " buffer=" + hex(state_buffer));
        }
        desired_cycle = (now < 0 ? 0 : now) + 1;
        log("sim-state: running to cycle " + std::to_string(desired_cycle) +
            " current=" + std::to_string(now));
        mod.simulation.run(model, desired_cycle);
        stage = 3;
        stage_time = elapsed;
        return;
    }

    if (stage == 3) {
        const int64_t cycle = mod.simulation.cycle();
        if (cycle < desired_cycle) {
            if (elapsed > stage_time + 5.0) {
                log("sim-state: timed out waiting for cycle " +
                    std::to_string(desired_cycle) +
                    " current=" + std::to_string(cycle));
                stage = 4;
            }
            return;
        }
        if (readState()) {
            log("sim-state: snapshot at cycle " + std::to_string(cycle) +
                " buffer=" + hex(state_buffer) +
                " test_state=" + (get_test_state ? std::to_string(get_test_state()) : "?"));
        }
        ++desired_cycle;
        stage_time = elapsed;
        if (snapshots.size() >= static_cast<size_t>(kSnapshotCount)) {
            stage = 4;
            return;
        }
        mod.simulation.run(model, desired_cycle);
        return;
    }

    if (stage == 4) {
        if (!write_test) {
            if (drive_test) {
                stage = 6;
                return;
            }
            finish();
            return;
        }
        unsigned char* buffer = *reinterpret_cast<unsigned char**>(state_global);
        if (!buffer) {
            write_test = false;
            finish();
            return;
        }
        collectWriteTargets();
        for (uint64_t offset : write_offsets) {
            if (offset >= kSimulationStateSize) continue;
            write_before[offset] = buffer[offset];
            buffer[offset] = kWriteMarker;
        }
        write_cycle = mod.simulation.cycle();
        stage = 5;
        stage_time = elapsed;
        log("sim-state: wrote marker into " + std::to_string(write_offsets.size()) +
            " slots at cycle " + std::to_string(write_cycle));
        mod.simulation.run(model, write_cycle + 2);
        return;
    }

    if (stage == 5) {
        const int64_t cycle = mod.simulation.cycle();
        if (cycle < write_cycle + 2 && elapsed < stage_time + 10.0) return;
        unsigned char* buffer = *reinterpret_cast<unsigned char**>(state_global);
        for (uint64_t offset : write_offsets) {
            write_after[offset] = buffer ? buffer[offset] : 0;
            uint64_t reads = 0;
            {
                std::lock_guard<std::mutex> lock(read_mutex);
                for (const auto& entry : state_read_threads) {
                    if (entry.first.first == static_cast<int64_t>(offset)) reads += entry.second;
                }
            }
            write_reads_after[offset] = reads;
        }
        if (drive_test) {
            stage = 6;
            return;
        }
        finish();
        return;
    }

    /* S2: our engine drives the board's slots. */
    if (stage == 6) {
        const auto* board_bytes = static_cast<const unsigned char*>(model);
        const tcsim::BoardView view = tcsim::readBoard(board_bytes);
        tcsim::BuildOptions build_options;
        build_options.gate_delay = tcsim::ArcDelay{tcsim::kTicksPerUnit, tcsim::kTicksPerUnit};
        drive_build = tcsim::buildNetList(view, &prototypeGeometry, build_options);
        drive_log << "netlist nets=" << drive_build.netlist.netCount()
                  << " devices=" << drive_build.netlist.deviceCount()
                  << " sources=" << drive_build.source_nets.size()
                  << " observed=" << drive_build.observed_nets.size()
                  << " unconnected_pins=" << drive_build.unconnected_pins
                  << " fatal=" << (drive_build.fatal() ? 1 : 0) << "\n";
        for (const tcsim::BuildNote& note : drive_build.notes) {
            drive_log << "note: " << note.text << (note.fatal ? " [fatal]" : "") << "\n";
        }
        if (drive_build.fatal()) {
            finish();
            return;
        }
        drive_engine = new tcsim::Engine(drive_build.netlist, tcsim::EngineOptions{});
        drive_remaining = drive_cycles;
        stage = 7;
        return;
    }
    if (stage == 7) {
        if (!drive_engine) {
            finish();
            return;
        }
        const int64_t cycle = mod.simulation.cycle();
        /* Feed the nets the game drives from the game's own array, run one
           cycle of our simulator, then publish our values back. */
        for (tcsim::NetId net : drive_build.source_nets) {
            const uint64_t value = readNetByte(drive_build.slots_of_net[net]);
            drive_engine->addStimulus(drive_engine->now(), net, tcsim::BitVector(1, value ? tcsim::Logic::kOne
                                                                                          : tcsim::Logic::kZero));
        }
        drive_engine->runUntil(drive_engine->now() + tcsim::kTicksPerUnit * 8);
        std::ostringstream row;
        row << "cycle " << cycle << " engine=" << drive_engine->now();
        for (size_t net_index = 0; net_index < drive_build.netlist.netCount(); ++net_index) {
            const tcsim::NetId net = static_cast<tcsim::NetId>(net_index);
            uint64_t value = drive_engine->netValue(net).size() && drive_engine->netValue(net)[0] == tcsim::Logic::kOne
                                 ? 1
                                 : 0;
            const bool is_source = std::find(drive_build.source_nets.begin(), drive_build.source_nets.end(), net) !=
                                   drive_build.source_nets.end();
            if (drive_invert && !is_source) value ^= 1u;
            const uint64_t before = readNetByte(drive_build.slots_of_net[net]);
            writeNetByte(drive_build.slots_of_net[net], value);
            const uint64_t after = readNetByte(drive_build.slots_of_net[net]);
            row << " net" << net_index << "=" << value << " program=" << before << " read=" << after;
        }
        drive_log << row.str() << "\n";
        if (--drive_remaining <= 0) {
            delete drive_engine;
            drive_engine = nullptr;
            finish();
            return;
        }
        drive_next_cycle = cycle + 1;
        mod.simulation.run(model, drive_next_cycle);
        stage = 8;
        stage_time = elapsed;
        return;
    }
    if (stage == 8) {
        if (mod.simulation.cycle() < drive_next_cycle && elapsed < stage_time + 5.0) {
            return;
        }
        stage = 7;
        return;
    }

    finish();
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;

    const std::string folder = h->data_directory_utf8;
    deep_dump = std::getenv("TC_SIM_STATE_DEEP") != nullptr;
    write_test = std::getenv("TC_SIM_STATE_WRITE") != nullptr;
    drive_test = std::getenv("TC_SIM_STATE_DRIVE") != nullptr;
    drive_invert = std::getenv("TC_SIM_STATE_INVERT") != nullptr;
    cycle_probe = std::getenv("TC_SIM_STATE_CYCLE") != nullptr;
    if (const char* cycles = std::getenv("TC_SIM_STATE_CYCLES")) {
        const int parsed = std::atoi(cycles);
        if (parsed > 0) drive_cycles = parsed;
    }
    {
        std::ifstream logic(folder + "/logic.txt");
        if (logic) std::getline(logic, custom_logic);
    }
    {
        std::ifstream fixture(folder + "/fixtures/and2_component.data",
                              std::ios::binary);
        if (fixture) {
            std::string bytes((std::istreambuf_iterator<char>(fixture)), {});
            const std::string directory = folder + "/fixtures/";
            auto imported = mod.components.importCircuit(
                "AND2 Test", bytes.data(), bytes.size(), directory.c_str());
            if (!imported.ok()) return 8;
            imported_custom = true;
            log("sim-state: imported custom AND2 id=" +
                std::to_string(imported.custom_id));
        }
    }

    load_level = reinterpret_cast<LoadLevel>(
        h->resolve_symbol(h->context, "load_level__modelZutilities_u7740"));
    set_sim_test = reinterpret_cast<SetSimTest>(h->resolve_symbol(
        h->context, "set_sim_test__modelZutilities_u6840"));
    compile_request = reinterpret_cast<CompileRequest>(h->resolve_symbol(
        h->context, "preorder__modelZsimulationZcompile95thread_u3523"));
    state_global = h->resolve_symbol(
        h->context, "simulation_state__modelZsimulator95types_u81");
    input_replay_global = h->resolve_symbol(
        h->context, "simulation_input_replay__modelZsimulator95types_u84");
    output_history_global = h->resolve_symbol(
        h->context, "simulation_output_history_pins__modelZsimulator95types_u85");
    if (!load_level || !compile_request || !state_global) return 3;

    auto* update_target = h->resolve_symbol(
        h->context,
        "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
    auto* sim_do_target = h->resolve_symbol(
        h->context, "sim_do__modelZsimulationZcompile95thread_u3036");
    auto* state_read_target = h->resolve_symbol(
        h->context, "sim_state_read_u64__modelZsimulator95types_u159");
    auto* get_sim_state_target = h->resolve_symbol(
        h->context, "get_sim_state__modelZsimulationZcontroller_u102");
    get_test_state = reinterpret_cast<GetTestState>(h->resolve_symbol(
        h->context, "sim_get_test_state__modelZsimulationZcontroller_u26"));
    if (!update_target || !invisible_target || !sim_do_target) return 4;

    if (h->create_hook(h->context, update_target,
                       reinterpret_cast<void*>(&hookedUpdate),
                       reinterpret_cast<void**>(&update_original)) != 0) {
        return 5;
    }
    /* sim_do is a loader hook chain point: joining the chain is what lets this
       probe run next to another mod that also watches run requests. */
    if (tc::hook::addSimDo(h, 0, &interceptedSimDo, nullptr) != TC_HOOK_OK) return 6;
    if (h->create_hook(h->context, invisible_target,
                       reinterpret_cast<void*>(&hookedInvisible),
                       reinterpret_cast<void**>(&invisible_original)) != 0) {
        return 7;
    }
    if (state_read_target &&
        h->create_hook(h->context, state_read_target,
                       reinterpret_cast<void*>(&hookedStateReadU64),
                       reinterpret_cast<void**>(&state_read_original)) != 0) {
        return 9;
    }
    if (get_sim_state_target &&
        h->create_hook(h->context, get_sim_state_target,
                       reinterpret_cast<void*>(&hookedGetSimState),
                       reinterpret_cast<void**>(&get_sim_state_original)) != 0) {
        return 10;
    }

    unsigned char* initial_buffer =
        *reinterpret_cast<unsigned char**>(state_global);
    state_buffer = initial_buffer;
    if (!custom_runtime.load(h)) return 11;
    tc::TCCustomLogicComponent definition{};
    definition.custom_id = kCustomComponentId;
    definition.input_count = 2;
    definition.output_count = 1;
    definition.logic = &customLogicOr;
    definition.level_input = &customLevelInput;
    definition.level_input_write = &customLevelInputWrite;
    definition.level_output_write = &customLevelOutputWrite;
    definition.test = &customTest;
    if (!custom_runtime.add(definition)) return 12;
    log("sim-state: state global=" + hex(state_global) +
        " buffer=" + hex(initial_buffer) +
        " size=" + std::to_string(kSimulationStateSize) +
        " custom=" + std::to_string(imported_custom ? 1 : 0) +
        " logic=" + (custom_logic.empty() ? "native" : custom_logic));
    plugin->on_frame = frame;
    return 0;
}
