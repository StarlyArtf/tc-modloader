// Route 1 reconnaissance: map circuit pins to simulation state slots.
//
// Runs the stock and_gate level one cycle at a time and snapshots the whole
// simulation state after each cycle.  The level's own test defines the
// expected output sequence (0,0,0,1) for inputs (0,0),(0,1),(1,0),(1,1), so
// byte slots whose value sequence matches a known pattern are the pin storage
// we are looking for.  Read-only: it never writes game state.

#include "../sdk/tc_mod.h"
#include "../sdk/tc_custom_logic.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>
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
std::mutex gss_mutex;
std::map<std::string, std::pair<uint64_t, uint64_t>> gss_stats;
std::map<std::string, uint64_t> gss_last1;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
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

void interceptedSimDo(void* state, uint8_t command, int64_t target) {
    model = state;
    if (model && !model_logged) {
        model_logged = true;
        log("sim-state: model captured from sim_do " + hex(model));
    }
    if (imported_custom && !custom_logic.empty() && command == 0) {
        if (custom_runtime.run(mod, state, target)) return;
    }
    if (sim_do_original) sim_do_original(state, command, target);
}

uint64_t hookedStateReadU64(int64_t index) {
    uint64_t value = state_read_original ? state_read_original(index) : 0;
    std::lock_guard<std::mutex> lock(read_mutex);
    ++state_read_counts[index];
    state_read_last[index] = value;
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
    }
    elapsed = value->time_seconds - start_time;
    if (done || !model || elapsed < 5.0) return;

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

    finish();
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;

    const std::string folder = h->data_directory_utf8;
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
    if (h->create_hook(h->context, sim_do_target,
                       reinterpret_cast<void*>(&interceptedSimDo),
                       reinterpret_cast<void**>(&sim_do_original)) != 0) {
        return 6;
    }
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
