/* Route 1 follow-up: find the per-cycle I/O trace of a level.

   The waveform/VCD work needs one thing to be true before anything else: that
   a plugin can read, for every cycle, the value of each level input and output.
   The previous reconnaissance located two history buffers by symbol
   (simulation_input_replay / simulation_output_history_pins) but only for a
   two-input one-output level and without the per-pin layout.

   This probe drives the stock and_gate level - whose own test feeds inputs
   0,1,2,3 and expects outputs 0,0,0,1 - and after every cycle dumps:

     * every byte of both history buffers that is non-zero or changed,
     * sim_state_read_u64(index) for the indices the game's own UI reads.

   Read-only apart from running the simulation: it never writes game state.
   The report goes to <plugin data>/trace-map.txt and the run summary to the
   loader log. */
#include "../sdk/tc_mod.h"
#include "../sdk/tc_trace.h"
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct V2 { float x, y; };

constexpr uint64_t kSmallBufferSize = 0x1000;
constexpr int kSnapshots = 6;

const TCHost* host;
tc::TCMod mod;
tc::trace::Sampler trace;
void* model = nullptr;
bool model_logged = false;
bool done = false;
bool started = false;
int stage = 0;
double start_time = 0, elapsed = 0, stage_time = 0;
int64_t desired_cycle = 1;

using LoadLevel = void (*)(void*, const tc::TCNimString*);
using SetSimTest = void (*)(void*, int64_t, uint8_t);
using CompileRequest = void (*)(void*, const tc::TCNimString*, uint8_t);
using StateReadU64 = uint64_t (*)(int64_t);
using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
using InvisibleButton = bool (*)(const char*, V2, int);

LoadLevel load_level;
SetSimTest set_sim_test;
CompileRequest compile_request;
StateReadU64 state_read;
UpdateWire update_original;
InvisibleButton invisible_original;

void* input_replay_global = nullptr;
void* output_history_global = nullptr;

std::vector<std::vector<unsigned char>> input_snapshots, output_snapshots;
std::vector<int64_t> snapshot_cycles;
std::map<int64_t, std::vector<uint64_t>> state_read_series;
int test_frame = -1;
int test_button_index = 0;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value << std::dec;
    return out.str();
}

unsigned char* smallBuffer(void* global) {
    if (!global) return nullptr;
    unsigned char* buffer = *reinterpret_cast<unsigned char**>(global);
    if (!buffer || reinterpret_cast<uintptr_t>(buffer) < 0x10000) return nullptr;
    return buffer;
}

bool hookedUpdate(void* m, void* context, void* input, uint32_t point, uint8_t fifth) {
    model = m;
    if (model && !model_logged) {
        model_logged = true;
        log("trace: model captured from handle_update_wire");
    }
    return update_original ? update_original(m, context, input, point, fifth) : false;
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

void snapshot() {
    const auto copy = [](void* global, std::vector<std::vector<unsigned char>>& out) {
        unsigned char* buffer = smallBuffer(global);
        if (buffer) out.emplace_back(buffer, buffer + kSmallBufferSize);
        else out.emplace_back();
    };
    copy(input_replay_global, input_snapshots);
    copy(output_history_global, output_snapshots);
    snapshot_cycles.push_back(mod.simulation.cycle());
    /* The SDK reader runs alongside the raw dump: this is the code plugins get,
       so the report shows what it resolved and what it read. */
    trace.sample();
    /* What the game's own UI reads: index range around the level's IO slots. */
    for (int64_t index = 240; index <= 300 && state_read; ++index)
        state_read_series[index].push_back(state_read(index));
}

void finish() {
    if (done) return;
    done = true;
    std::ostringstream report;
    report << "cycles=";
    for (int64_t cycle : snapshot_cycles) report << cycle << " ";
    report << "\n";
    const auto dump = [&](const char* label,
                          const std::vector<std::vector<unsigned char>>& snaps) {
        report << "== " << label << " ==\n";
        if (snaps.empty()) { report << "(empty)\n"; return; }
        for (uint64_t offset = 0; offset < kSmallBufferSize; ++offset) {
            bool interesting = false;
            for (const auto& snap : snaps) {
                if (offset < snap.size() && snap[offset] != 0) { interesting = true; break; }
            }
            if (!interesting) continue;
            report << label << " byte " << offset << " seq=";
            for (const auto& snap : snaps)
                report << (offset < snap.size() ? static_cast<int>(snap[offset]) : -1) << ",";
            report << "\n";
        }
        /* Dense head: the first 160 bytes of every snapshot, so the record
           layout (stride between pins) is visible even where values are zero. */
        for (size_t index = 0; index < snaps.size(); ++index) {
            report << label << " head" << index << " cycle=" << snapshot_cycles[index] << ":";
            for (uint64_t offset = 0; offset < 160 && offset < snaps[index].size(); ++offset)
                report << " " << static_cast<int>(snaps[index][offset]);
            report << "\n";
        }
    };
    dump("input_replay", input_snapshots);
    dump("output_history", output_snapshots);
    report << "== state_read_u64 ==\n";
    for (const auto& entry : state_read_series) {
        bool varies = false;
        for (uint64_t value : entry.second)
            if (value != entry.second.front()) { varies = true; break; }
        if (!varies && entry.second.front() == 0) continue;
        report << "index " << entry.first << " seq=";
        for (uint64_t value : entry.second) report << value << ",";
        report << "\n";
    }
    report << "TRACE DONE cycles=" << snapshot_cycles.size() << "\n";
    /* Slot discovery: the level declares how many input/output pins it uses;
       the history buffers are found by looking for byte offsets that moved
       while the level's own test fed inputs.  This is the algorithm the SDK
       reader will use, so the probe reports its result explicitly. */
    report << "level_inputs=" << mod.state.levelUsedInput()
           << " level_outputs=" << mod.state.levelUsedOutputs() << "\n";
    const auto candidates = [&](const std::vector<std::vector<unsigned char>>& snaps,
                                const char* label) {
        std::vector<uint64_t> moved;
        if (snaps.size() < 2) return moved;
        for (uint64_t offset = 0; offset < kSmallBufferSize; ++offset) {
            bool saw_other_value = false;
            for (size_t index = 1; index < snaps.size(); ++index) {
                if (offset < snaps[index].size() && snaps[index][offset] != snaps[0][offset]) {
                    saw_other_value = true;
                    break;
                }
            }
            if (saw_other_value) moved.push_back(offset);
        }
        report << label << "_candidates=" << moved.size() << ":";
        for (uint64_t offset : moved) report << " " << offset;
        report << "\n";
        return moved;
    };
    candidates(input_snapshots, "input");
    candidates(output_snapshots, "output");
    const std::string folder = host->data_directory_utf8;
    /* The SDK sampler's own view, and a VCD file next to the report. */
    report << "sampler ready=" << (trace.ready() ? 1 : 0)
           << " assumed_stride=" << (trace.assumedStride() ? 1 : 0)
           << " inputs=" << trace.inputCount() << " outputs=" << trace.outputCount()
           << " rows=" << trace.rows() << "\n";
    if (trace.inputCount() > 0)
        report << "sampler input slots=";
    for (int index = 0; index < trace.inputCount(); ++index)
        report << (index ? "," : "") << trace.inputSlot(index);
    report << "\n";
    if (trace.outputCount() > 0)
        report << "sampler output slots=";
    for (int index = 0; index < trace.outputCount(); ++index)
        report << (index ? "," : "") << trace.outputSlot(index);
    report << "\n";
    for (size_t row = 0; row < trace.rows(); ++row) {
        report << "sampler row " << row << " cycle=" << trace.cycleAt(row) << " in=";
        for (int index = 0; index < trace.inputCount(); ++index)
            report << (index ? "," : "") << trace.input(row, index);
        report << " out=";
        for (int index = 0; index < trace.outputCount(); ++index)
            report << (index ? "," : "") << trace.output(row, index);
        report << "\n";
    }
    const std::string vcd = folder + "/trace.vcd";
    if (trace.writeVcd(vcd.c_str())) log("trace: wrote trace.vcd");
    else log("trace: could not write trace.vcd");
    log("trace: sampler inputs=" + std::to_string(trace.inputCount()) + " outputs=" +
        std::to_string(trace.outputCount()) + " rows=" + std::to_string(trace.rows()) +
        " assumedStride=" + std::to_string(trace.assumedStride() ? 1 : 0));
    for (size_t row = 0; row < trace.rows(); ++row) {
        std::string line = "trace: row " + std::to_string(row) + " cycle=" +
                           std::to_string(trace.cycleAt(row)) + " in=";
        for (int index = 0; index < trace.inputCount(); ++index)
            line += (index ? "," : "") + std::to_string(trace.input(row, index));
        line += " out=";
        for (int index = 0; index < trace.outputCount(); ++index)
            line += (index ? "," : "") + std::to_string(trace.output(row, index));
        log(line);
    }
    std::ofstream(folder + "/trace-map.txt") << report.str();
    log("trace: saved trace-map.txt snapshots=" + std::to_string(snapshot_cycles.size()));
    for (int64_t cycle : snapshot_cycles) report << cycle << " ";
    log("trace: cycles " + [&] {
        std::string text;
        for (int64_t cycle : snapshot_cycles) text += std::to_string(cycle) + " ";
        return text;
    }());
    log("TRACE DONE");
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    elapsed = value->time_seconds - start_time;
    if (done || !model || elapsed < 5.0) return;
    if (stage == 0) {
        /* Level and schematic are chosen by the run script: and_gate (2 in,
           1 out) is the known-good case, byte_adder (3 in, 2 out) shows what
           the per-pin layout looks like when there is more than one pin. */
        const char* text = std::getenv("TC_TRACE_LEVEL") ? std::getenv("TC_TRACE_LEVEL") : "and_gate";
        tc::TCNimString name{};
        const size_t length = std::strlen(text);
        mod.game.raw_new_string(&name, static_cast<int64_t>(length));
        name.length = length;
        std::memcpy(static_cast<unsigned char*>(name.data) + 8, text, length);
        static_cast<unsigned char*>(name.data)[8 + length] = 0;
        load_level(model, &name);
        log("trace: loaded and_gate");
        stage = 1;
        stage_time = elapsed;
        return;
    }
    if (stage == 1) {
        if (elapsed < stage_time + 2.0) return;
        if (set_sim_test) set_sim_test(model, 0, 1);
        tc::TCNimString progress{};
        if (compile_request) compile_request(model, &progress, 0);
        log("trace: requested compilation");
        stage = 2;
        stage_time = elapsed;
        return;
    }
    if (stage == 2) {
        if (elapsed < stage_time + 3.0) return;
        const int64_t now = mod.simulation.cycle();
        snapshot();
        desired_cycle = (now < 0 ? 0 : now) + 1;
        mod.simulation.run(model, desired_cycle);
        stage = 3;
        stage_time = elapsed;
        return;
    }
    if (stage == 3) {
        if (mod.simulation.cycle() < desired_cycle) {
            if (elapsed > stage_time + 5.0) {
                log("trace: timeout waiting for cycle " + std::to_string(desired_cycle));
                finish();
            }
            return;
        }
        snapshot();
        ++desired_cycle;
        stage_time = elapsed;
        if (snapshot_cycles.size() >= static_cast<size_t>(kSnapshots)) { finish(); return; }
        mod.simulation.run(model, desired_cycle);
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid()) return 2;
    if (!trace.load(h, mod.simulation, mod.state)) {
        log("trace: sampler unavailable (no level I/O history)");
    }
    load_level = reinterpret_cast<LoadLevel>(
        h->resolve_symbol(h->context, "load_level__modelZutilities_u7740"));
    set_sim_test = reinterpret_cast<SetSimTest>(
        h->resolve_symbol(h->context, "set_sim_test__modelZutilities_u6840"));
    compile_request = reinterpret_cast<CompileRequest>(
        h->resolve_symbol(h->context, "preorder__modelZsimulationZcompile95thread_u3523"));
    state_read = reinterpret_cast<StateReadU64>(
        h->resolve_symbol(h->context, "sim_state_read_u64__modelZsimulator95types_u159"));
    input_replay_global = h->resolve_symbol(
        h->context, "simulation_input_replay__modelZsimulator95types_u84");
    output_history_global = h->resolve_symbol(
        h->context, "simulation_output_history_pins__modelZsimulator95types_u85");
    if (!load_level || !compile_request || !input_replay_global || !output_history_global)
        return 3;
    auto* update_target = h->resolve_symbol(
        h->context, "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
    if (!update_target || !invisible_target) return 4;
    if (h->create_hook(h->context, update_target, reinterpret_cast<void*>(hookedUpdate),
                       reinterpret_cast<void**>(&update_original)) != 0)
        return 5;
    if (h->create_hook(h->context, invisible_target, reinterpret_cast<void*>(hookedInvisible),
                       reinterpret_cast<void**>(&invisible_original)) != 0)
        return 6;
    plugin->on_frame = frame;
    return 0;
}
