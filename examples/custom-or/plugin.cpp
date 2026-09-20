#include "../../sdk/tc_mod.h"

#include "../circuit-and/and_fixture.hpp"

#include <windows.h>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct V2 {
    float x, y;
};

const TCHost* host;
tc::TCMod mod;

using LoadLevel = void (*)(void*, const tc::TCNimString*);
using SetSimTest = void (*)(void*, int64_t, uint8_t);
using CompileRequest = void (*)(void*, const tc::TCNimString*, uint8_t);
using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
using InvisibleButton = bool (*)(const char*, V2, int);
using SimGetTestState = int64_t (*)();

LoadLevel load_level;
SetSimTest set_sim_test;
CompileRequest compile_request;
UpdateWire update_original;
InvisibleButton invisible_original;
SimGetTestState sim_test_state;

// Live counter per compiled component instance.  The callback runs on the
// simulation thread while the frame callback reads these from the main thread.
struct InstanceStats {
    std::atomic<uint64_t> instance{0};
    std::atomic<uint64_t> cycles{0};
    std::atomic<uint64_t> peeks{0};
    std::atomic<uint64_t> resets{0};
    std::atomic<uint64_t> peekLogs{0};
};
std::array<InstanceStats, 8> stats;
std::atomic<uint64_t> instancesSeen{0};
// Widest output count seen, i.e. the number of level output pins the compiled
// board writes history for.
std::atomic<uint32_t> outputSlots{1};

bool autotest = false;
std::string autotestLevel = "and_gate";
bool autotestStarted = false;
bool autotestDone = false;
int autotestStage = 0;
double autotestStart = 0;
double autotestStageTime = 0;
double autotestElapsed = 0;
int64_t autotestTarget = 1;
int64_t autotestCycleLimit = 15;
int autotestButtonFrame = -1;
int autotestButton = 0;
uint64_t autotestTotalPeeks = 0;
uint64_t autotestTotalResets = 0;
void* autotestModel = nullptr;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

std::string hex64(uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << value << std::dec;
    return text.str();
}

uint64_t totalCalls() {
    uint64_t total = 0;
    for (const auto& slot : stats) total += slot.cycles.load();
    return total;
}

// The level's own output pin history is what the in-game table displays.  The
// game writes one 9-byte slot per output component and table row: high
// impedance flag first, value second.  The row written during the loop
// iteration with local cycle c lands in slot c + 1, so table row r lives at
// index r * 9.  The boards used by this playtest have one output component.
void logGameOutputs() {
    const void* global = mod.simulation.outputHistoryPins();
    if (!global) return;
    auto* history = *reinterpret_cast<unsigned char**>(const_cast<void*>(global));
    if (!history) return;
    std::ostringstream raw;
    raw << "custom-or: game output history raw=" << std::hex;
    for (size_t i = 0; i < 36; ++i) {
        raw << (i ? " " : "") << static_cast<unsigned>(history[i]);
    }
    log(raw.str());
    const uint32_t slots = outputSlots.load();
    for (int cycle = 0; cycle < 8; ++cycle) {
        for (uint32_t pin = 0; pin < slots && pin < 4; ++pin) {
            const size_t index = (static_cast<size_t>(cycle) * slots + pin) * 9;
            std::string prefix = "custom-or: game output history cycle=" +
                                 std::to_string(cycle);
            if (pin) prefix += " pin=" + std::to_string(pin);
            log(prefix + " z=" + std::to_string(history[index]) +
                " value=" + std::to_string(history[index + 1]));
        }
    }
}

InstanceStats& slotFor(uint64_t instance) {
    for (auto& slot : stats) {
        if (slot.instance.load() == instance) return slot;
    }
    for (auto& slot : stats) {
        uint64_t expected = 0;
        if (slot.instance.compare_exchange_strong(expected, instance)) {
            ++instancesSeen;
            return slot;
        }
        if (slot.instance.load() == instance) return slot;
    }
    return stats[0];
}

std::string describe(const uint64_t* values, uint32_t count) {
    std::string text = "[";
    for (uint32_t i = 0; i < count && i < 8; ++i) {
        if (i) text += ",";
        // Whole words: one-bit pins simply read as 0 or 1.
        text += std::to_string(values[i]);
    }
    return text + "]";
}

// Shapes are declared by the imported definition, so the bookkeeping and the
// logging stay independent of the pin count.
void resetInstance(TCLogicIO* io, InstanceStats& slot) {
    slot.resets.fetch_add(1);
    log("custom-or: reset instance=" + hex64(io->instance_id) +
        " calls=" + std::to_string(slot.cycles.load()) +
        " persisted_state=" + std::to_string(io->state[0]));
    for (uint32_t i = 0; i < 8; ++i) io->state[i] = 0;
    for (uint32_t i = 0; i < 8; ++i) io->outputs[i] = 0;
}

void publishOutputs(TCLogicIO* io, InstanceStats& slot) {
    if (io->output_count > outputSlots.load()) outputSlots.store(io->output_count);
    if (io->phase == TC_LOGIC_REFRESH) {
        slot.peeks.fetch_add(1);
        if (slot.peekLogs.fetch_add(1) < 3) {
            log("custom-or: peek cycle=" + std::to_string(io->cycle) +
                " instance=" + hex64(io->instance_id) +
                " in=" + describe(io->inputs, io->input_count) +
                " out=" + describe(io->outputs, io->output_count) +
                " committed_state=" + std::to_string(io->state[0]));
        }
        return;
    }
    ++io->state[0];  // per-instance call counter, committed per cycle
    io->state[1] = io->outputs[0] & 1;
    slot.cycles.fetch_add(1);
    log("custom-or: cycle=" + std::to_string(io->cycle) +
        " instance=" + hex64(io->instance_id) +
        " in=" + describe(io->inputs, io->input_count) +
        " out=" + describe(io->outputs, io->output_count) +
        " calls=" + std::to_string(io->state[0]));
}

// Component internal circuits are placeholders, so every result below can only
// come from these callbacks.
void logicOr(TCLogicIO* io) {
    InstanceStats& slot = slotFor(io->instance_id);
    if (io->phase == TC_LOGIC_RESET) {
        resetInstance(io, slot);
        return;
    }
    io->outputs[0] = ((io->inputs[0] | io->inputs[1]) & 1);
    publishOutputs(io, slot);
}

void logicNot(TCLogicIO* io) {
    InstanceStats& slot = slotFor(io->instance_id);
    if (io->phase == TC_LOGIC_RESET) {
        resetInstance(io, slot);
        return;
    }
    io->outputs[0] = (~io->inputs[0]) & 1;
    publishOutputs(io, slot);
}

void logicAnd3(TCLogicIO* io) {
    InstanceStats& slot = slotFor(io->instance_id);
    if (io->phase == TC_LOGIC_RESET) {
        resetInstance(io, slot);
        return;
    }
    io->outputs[0] = (io->inputs[0] & io->inputs[1] & io->inputs[2]) & 1;
    publishOutputs(io, slot);
}

// Three one-bit inputs, two outputs: sum then carry.
void logicAdder(TCLogicIO* io) {
    InstanceStats& slot = slotFor(io->instance_id);
    if (io->phase == TC_LOGIC_RESET) {
        resetInstance(io, slot);
        return;
    }
    const uint64_t total = (io->inputs[0] & 1) + (io->inputs[1] & 1) + (io->inputs[2] & 1);
    io->outputs[0] = total & 1;
    io->outputs[1] = (total >> 1) & 1;
    publishOutputs(io, slot);
}

// One 8-bit input, one 8-bit output: the ABI carries whole words, so the same
// bookkeeping works and only the arithmetic differs.
void logicDoubleByte(TCLogicIO* io) {
    InstanceStats& slot = slotFor(io->instance_id);
    if (io->phase == TC_LOGIC_RESET) {
        resetInstance(io, slot);
        return;
    }
    io->outputs[0] = (io->inputs[0] * 2) & 0xff;
    publishOutputs(io, slot);
}

// Two 8-bit inputs, one 8-bit output.
void logicXorByte(TCLogicIO* io) {
    InstanceStats& slot = slotFor(io->instance_id);
    if (io->phase == TC_LOGIC_RESET) {
        resetInstance(io, slot);
        return;
    }
    io->outputs[0] = (io->inputs[0] ^ io->inputs[1]) & 0xff;
    publishOutputs(io, slot);
}

// Select, A, B (all 8-bit) -> one 8-bit output.  The level treats select == 1
// as "B", anything else as "A".
void logicMuxByte(TCLogicIO* io) {
    InstanceStats& slot = slotFor(io->instance_id);
    if (io->phase == TC_LOGIC_RESET) {
        resetInstance(io, slot);
        return;
    }
    io->outputs[0] = (io->inputs[0] == 1 ? io->inputs[2] : io->inputs[1]) & 0xff;
    publishOutputs(io, slot);
}

// 8-bit value plus 3-bit shift amount -> arithmetic right shift.
void logicShiftByte(TCLogicIO* io) {
    InstanceStats& slot = slotFor(io->instance_id);
    if (io->phase == TC_LOGIC_RESET) {
        resetInstance(io, slot);
        return;
    }
    const auto value = static_cast<int8_t>(io->inputs[0] & 0xff);
    const uint32_t shift = static_cast<uint32_t>(io->inputs[1] & 7);
    io->outputs[0] = static_cast<uint8_t>(static_cast<int32_t>(value) >> shift);
    publishOutputs(io, slot);
}

// Carry in (1 bit) + A, B (8 bit) -> sum (8 bit) + carry out (1 bit).
void logicAddByte(TCLogicIO* io) {
    InstanceStats& slot = slotFor(io->instance_id);
    if (io->phase == TC_LOGIC_RESET) {
        resetInstance(io, slot);
        return;
    }
    const uint64_t total = (io->inputs[0] & 1) + (io->inputs[1] & 0xff) + (io->inputs[2] & 0xff);
    io->outputs[0] = total & 0xff;
    io->outputs[1] = (total >> 8) & 1;
    publishOutputs(io, slot);
}

bool hookedUpdate(void* model, void* context, void* input, uint32_t point,
                  uint8_t fifth) {
    autotestModel = model;
    return update_original ? update_original(model, context, input, point, fifth)
                           : false;
}

bool hookedInvisible(const char* id, V2 size, int flags) {
    const bool result = invisible_original(id, size, flags);
    const auto rva = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                     reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (rva >= 0x449df0 && rva < 0x44b610) {
        const auto get_frame = reinterpret_cast<int (*)()>(
            host->engine_proc(host->context, "igGetFrameCount"));
        const int frame = get_frame ? get_frame() : 0;
        if (frame != autotestButtonFrame) {
            autotestButtonFrame = frame;
            autotestButton = 0;
        }
        ++autotestButton;
        if (autotestElapsed > 4.0 && autotestButton == 2) return true;
    }
    return result;
}

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
}

// Diagnostic: the generated program bakes the UI buffer address into
// `var ui_buffer = Ptr <n>`; levels write their table cells there through
// set_text(text, offset).  Reading the cells shows exactly what the game
// compared, which is otherwise invisible in headless runs.
void logLevelText() {
    const auto source = readFile("native-logic-source.txt");
    if (source.empty()) return;
    const std::string text(source.begin(), source.end());
    const size_t marker = text.find("var ui_buffer");
    if (marker == std::string::npos) return;
    const size_t digits = text.find("Ptr ", marker);
    if (digits == std::string::npos) return;
    uint64_t address = 0;
    size_t cursor = digits + 4;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
        address = address * 10 + static_cast<uint64_t>(text[cursor] - '0');
        ++cursor;
    }
    if (!address) return;
    auto* buffer = reinterpret_cast<unsigned char*>(address);
    log("custom-or: level text probe bytes=" + std::to_string(source.size()) +
        " buffer=0x" + hex64(address));
    for (size_t offset : {20000u, 50000u, 80000u}) {
        uint64_t length = 0;
        std::memcpy(&length, buffer + offset, sizeof(length));
        std::string value;
        if (length <= 64) {
            value.assign(reinterpret_cast<const char*>(buffer + offset + 8),
                         static_cast<size_t>(length));
        } else {
            value = "<length " + std::to_string(length) + ">";
        }
        log("custom-or: level text offset=" + std::to_string(offset) + " value=" + value);
    }
}

void autotestFrame(const TCFrame* value) {
    if (!autotest || autotestDone) return;
    if (!autotestStarted) {
        autotestStarted = true;
        autotestStart = value->time_seconds;
    }
    autotestElapsed = value->time_seconds - autotestStart;
    if (!autotestModel || autotestElapsed < 5.0) return;

    switch (autotestStage) {
        case 0: {
            tc::TCNimString name{};
            const size_t length = autotestLevel.size();
            mod.game.raw_new_string(&name, static_cast<int64_t>(length));
            name.length = length;
            std::memcpy(static_cast<unsigned char*>(name.data) + 8,
                        autotestLevel.data(), length);
            static_cast<unsigned char*>(name.data)[8 + length] = 0;
            load_level(autotestModel, &name);
            log("custom-or: loaded level " + autotestLevel);
            autotestStage = 1;
            autotestStageTime = autotestElapsed;
            return;
        }
        case 1:
            if (autotestElapsed < autotestStageTime + 2.0) return;
            if (set_sim_test) set_sim_test(autotestModel, 0, 1);
            {
                tc::TCNimString progress{};
                if (compile_request) compile_request(autotestModel, &progress, 0);
            }
            autotestStage = 2;
            autotestStageTime = autotestElapsed;
            return;
        case 2:
            if (autotestElapsed < autotestStageTime + 3.0) return;
            autotestTarget = 1;
            mod.simulation.run(autotestModel, autotestTarget);
            autotestStage = 3;
            return;
        case 3: {
            // Levels have different test lengths: stop as soon as the game
            // reports a verdict, otherwise after four cycles.
            const int64_t verdict = sim_test_state ? sim_test_state() : 0;
            if (verdict == 0 && mod.simulation.cycle() < autotestTarget) return;
            if (verdict == 0 && autotestTarget < autotestCycleLimit) {
                ++autotestTarget;
                mod.simulation.run(autotestModel, autotestTarget);
                return;
            }
            log("custom-or: run finished cycle=" +
                std::to_string(mod.simulation.cycle()) +
                " callback_calls=" + std::to_string(totalCalls()) +
                " instances=" + std::to_string(instancesSeen.load()) +
                " verdict=" + std::to_string(verdict));
            log("custom-or: game test result=" +
                std::to_string(sim_test_state ? sim_test_state() : -1));
            logGameOutputs();
            logLevelText();
            // Refresh path: report the current output without advancing.
            mod.simulation.submitCommand(autotestModel, 1, 0);
            autotestStage = 4;
            autotestStageTime = autotestElapsed;
            return;
        }
        case 4:
            if (autotestElapsed < autotestStageTime + 2.0) return;
            autotestTotalPeeks = 0;
            for (const auto& slot : stats) autotestTotalPeeks += slot.peeks.load();
            log("custom-or: after refresh peeks=" +
                std::to_string(autotestTotalPeeks) +
                " callback_calls=" + std::to_string(totalCalls()));
            mod.simulation.reset(autotestModel);
            autotestStage = 5;
            autotestStageTime = autotestElapsed;
            return;
        case 5:
            if (autotestElapsed < autotestStageTime + 2.0) return;
            autotestTotalResets = 0;
            for (const auto& slot : stats) autotestTotalResets += slot.resets.load();
            log("custom-or: after reset resets=" +
                std::to_string(autotestTotalResets) +
                " callback_calls=" + std::to_string(totalCalls()));
            mod.simulation.run(autotestModel, 1);
            autotestStage = 6;
            return;
        default:
            if (mod.simulation.cycle() < 1) return;
            log("custom-or: run after reset cycle=" +
                std::to_string(mod.simulation.cycle()) +
                " callback_calls=" + std::to_string(totalCalls()));
            autotestDone = true;
            log("custom-or: autotest finished; instances=" +
                std::to_string(instancesSeen.load()) +
                " resets=" + std::to_string(autotestTotalResets));
            return;
    }
}

void frame(void*, const TCFrame* value) {
    autotestFrame(value);
}

std::string readAutotestLevel(const std::string& directory) {
    std::ifstream file(directory + "/autotest.txt");
    if (!file) return {};
    std::string text;
    std::getline(file, text);
    // Optional second line: how many cycles the autotest may run before it
    // gives up on a verdict.  Levels whose test only wins after thousands of
    // cycles are checked by "no mismatch so far" instead.
    std::string limit;
    if (std::getline(file, limit)) {
        try {
            const int64_t value = std::stoll(limit);
            if (value > 0 && value < 100000) autotestCycleLimit = value;
        } catch (...) {
        }
    }
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

// Registers one imported definition with the callback that matches its shape.
struct ExtraShape {
    const char* file;
    const char* name;
    uint64_t inputs, outputs;
    TCLogicCallback callback;
};
const ExtraShape kExtraShapes[] = {
    {"nl_def_not1.data", "Native NOT", 1, 1, &logicNot},
    // Multi-input and multi-output definitions: the runtime supports them, but
    // the fixture writer's serialization of three input pins is still rejected
    // by the game's importer, so these files are optional.
    {"nl_def_and3.data", "Native AND3", 3, 1, &logicAnd3},
    {"nl_def_adder.data", "Native Adder", 3, 2, &logicAdder},
    {"nl_def_double8.data", "Native Double Byte", 1, 1, &logicDoubleByte},
    {"nl_def_xor8.data", "Native XOR Byte", 2, 1, &logicXorByte},
    {"nl_def_mux8.data", "Native Mux Byte", 3, 1, &logicMuxByte},
    {"nl_def_asr8.data", "Native Shift Byte", 2, 1, &logicShiftByte},
    {"nl_def_adder8.data", "Native Add Byte", 3, 2, &logicAddByte},
};

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;

    const auto bytes = tc_example::buildAndCircuitFile();
    auto imported = mod.components.importCircuit(
        "OR2 Native", bytes.data(), bytes.size(), "D:/tc-modloader/");
    if (!imported.ok() || imported.custom_id != tc_example::kAndComponentId) {
        log("custom-or: component import failed");
        return 3;
    }

    if (h->size < offsetof(TCHost, register_logic) + sizeof(h->register_logic) ||
        !h->register_logic) {
        return 4;
    }
    TCLogicDefinition definition{sizeof(TCLogicDefinition), 2, imported.custom_id,
                                 &logicOr, nullptr};
    if (h->register_logic(h->context, &definition) != 0) return 5;

    // Extra shapes used by the interface-shape regression.  The definition
    // files carry their own 64-bit id, so each one registers separately.
    for (const auto& shape : kExtraShapes) {
        const auto bytes = readFile(std::string(h->data_directory_utf8) + "/" + shape.file);
        if (bytes.empty()) continue;
        auto extra = mod.components.importCircuit(shape.name, bytes.data(), bytes.size(),
                                                  "D:/tc-modloader/");
        if (!extra.ok()) {
            log(std::string("custom-or: import failed for ") + shape.file);
            continue;
        }
        TCLogicDefinition extraDefinition{sizeof(TCLogicDefinition), 2, extra.custom_id,
                                          shape.callback, nullptr};
        if (h->register_logic(h->context, &extraDefinition) != 0) {
            log(std::string("custom-or: registration failed for ") + shape.file);
            continue;
        }
        log(std::string("custom-or: registered ") + shape.name + " id=" +
            std::to_string(extra.custom_id));
    }

    autotestLevel = readAutotestLevel(h->data_directory_utf8);
    autotest = !autotestLevel.empty();
    if (autotest) {
        load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
            h->context, "load_level__modelZutilities_u7740"));
        set_sim_test = reinterpret_cast<SetSimTest>(h->resolve_symbol(
            h->context, "set_sim_test__modelZutilities_u6840"));
        compile_request = reinterpret_cast<CompileRequest>(h->resolve_symbol(
            h->context, "preorder__modelZsimulationZcompile95thread_u3523"));
        sim_test_state = reinterpret_cast<SimGetTestState>(h->resolve_symbol(
            h->context, "sim_get_test_state__modelZsimulationZcontroller_u26"));
        auto* update_target = h->resolve_symbol(
            h->context,
            "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
        auto* invisible_target =
            h->resolve_symbol(h->context, "igInvisibleButton");
        if (!load_level || !set_sim_test || !compile_request || !update_target ||
            !invisible_target) {
            return 8;
        }
        if (h->create_hook(h->context, update_target,
                           reinterpret_cast<void*>(&hookedUpdate),
                           reinterpret_cast<void**>(&update_original)) != 0) {
            return 9;
        }
        if (h->create_hook(h->context, invisible_target,
                           reinterpret_cast<void*>(&hookedInvisible),
                           reinterpret_cast<void**>(&invisible_original)) != 0) {
            return 10;
        }
        plugin->on_frame = frame;
    }

    log("custom-or: registered native OR2 id=" +
        std::to_string(imported.custom_id));
    return 0;
}
