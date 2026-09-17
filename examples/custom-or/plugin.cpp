#include "../../sdk/tc_mod.h"
#include "../../sdk/tc_custom_logic.h"
#include "../circuit-and/and_fixture.hpp"

#include <windows.h>
#include <cstring>
#include <fstream>
#include <string>

namespace {

struct V2 {
    float x, y;
};

const TCHost* host;
tc::TCMod mod;
tc::TCCustomLogicRuntime custom_logic;
using SimDo = void (*)(void*, uint8_t, int64_t);
SimDo sim_do_original;
using LoadLevel = void (*)(void*, const tc::TCNimString*);
using SetSimTest = void (*)(void*, int64_t, uint8_t);
using CompileRequest = void (*)(void*, const tc::TCNimString*, uint8_t);
using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
using InvisibleButton = bool (*)(const char*, V2, int);

LoadLevel load_level;
SetSimTest set_sim_test;
CompileRequest compile_request;
UpdateWire update_original;
InvisibleButton invisible_original;

bool autotest = false;
bool autotest_started = false;
bool autotest_loaded = false;
bool autotest_done = false;
double autotest_start = 0;
double autotest_load_time = 0;
double autotest_stage_time = 0;
double autotest_elapsed = 0;
int autotest_stage = 0;
int autotest_frame = -1;
int autotest_button = 0;
int64_t autotest_target = 1;
void* autotest_model = nullptr;

void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

void logicOr(tc::TCCustomLogicIO* io) {
    io->outputs[0] = (io->inputs[0] | io->inputs[1]) & 1;
}

uint64_t levelInput(int64_t cycle, uint32_t pin, void*) {
    const uint64_t value = cycle < 0 ? 0 : (static_cast<uint64_t>(cycle) & 3);
    return pin == 0 ? (value & 1) : ((value >> 1) & 1);
}

void writeLevelInput(int64_t cycle, uint32_t, uint64_t, void*) {
    unsigned char** global = static_cast<unsigned char**>(
        const_cast<void*>(mod.simulation.inputReplay()));
    if (!global || !*global) return;
    const auto value = static_cast<unsigned char>(cycle < 0 ? 0 : (cycle & 3));
    (*global)[0] = (*global)[8] = value;
}

void writeLevelOutput(int64_t, uint32_t, uint64_t value, void*) {
    unsigned char** global = static_cast<unsigned char**>(
        const_cast<void*>(mod.simulation.outputHistoryPins()));
    if (!global || !*global) return;
    (*global)[55] = (*global)[64] = static_cast<unsigned char>(value & 1);
}

int64_t testAndGate(int64_t cycle, const uint64_t* outputs, uint32_t, void*) {
    if (cycle < 0) return 0;
    if (cycle >= 4) return 1;
    static const uint8_t expected[4] = {0, 0, 0, 1};
    if ((outputs[0] & 1) != expected[cycle]) {
        log("custom-or: cycle=" + std::to_string(cycle) +
            " output=" + std::to_string(outputs[0] & 1) +
            " expected=" + std::to_string(expected[cycle]) + " fail");
        return 2;
    }
    return cycle == 3 ? 1 : 0;
}

void interceptedSimDo(void* model, uint8_t command, int64_t target) {
    if (command == 0 && custom_logic.run(mod, model, target)) return;
    if (sim_do_original) sim_do_original(model, command, target);
}

bool hookedUpdate(void* model, void* context, void* input, uint32_t point,
                  uint8_t fifth) {
    autotest_model = model;
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
        if (frame != autotest_frame) {
            autotest_frame = frame;
            autotest_button = 0;
        }
        ++autotest_button;
        if (autotest_elapsed > 4.0 && autotest_button == 2) return true;
    }
    return result;
}

void autotestFrame(const TCFrame* value) {
    if (!autotest || autotest_done) return;
    if (!autotest_started) {
        autotest_started = true;
        autotest_start = value->time_seconds;
    }
    autotest_elapsed = value->time_seconds - autotest_start;
    if (!autotest_model || autotest_elapsed < 5.0) return;

    if (!autotest_loaded) {
        const char* text = "and_gate";
        tc::TCNimString name{};
        const size_t length = std::strlen(text);
        mod.game.raw_new_string(&name, static_cast<int64_t>(length));
        name.length = length;
        std::memcpy(static_cast<unsigned char*>(name.data) + 8, text, length);
        static_cast<unsigned char*>(name.data)[8 + length] = 0;
        load_level(autotest_model, &name);
        autotest_loaded = true;
        autotest_load_time = autotest_elapsed;
        return;
    }
    if (autotest_elapsed < autotest_load_time + 2.0) return;
    if (autotest_stage == 0) {
        tc::TCNimString progress{};
        if (set_sim_test) set_sim_test(autotest_model, 0, 1);
        if (compile_request) compile_request(autotest_model, &progress, 0);
        autotest_stage = 1;
        autotest_stage_time = autotest_elapsed;
        return;
    }
    if (autotest_stage == 1) {
        if (autotest_elapsed < autotest_stage_time + 3.0) return;
        const int64_t now = mod.simulation.cycle();
        autotest_target = (now < 0 ? 0 : now) + 1;
        mod.simulation.run(autotest_model, autotest_target);
        autotest_stage = 2;
        return;
    }
    if (mod.simulation.cycle() < autotest_target) return;
    if (autotest_target >= 3) {
        autotest_done = true;
        log("custom-or: autotest finished at cycle " +
            std::to_string(mod.simulation.cycle()));
        return;
    }
    ++autotest_target;
    mod.simulation.run(autotest_model, autotest_target);
}

void frame(void*, const TCFrame* value) {
    autotestFrame(value);
}

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

    if (!custom_logic.load(h)) return 4;
    tc::TCCustomLogicComponent definition{};
    definition.custom_id = imported.custom_id;
    definition.input_count = 2;
    definition.output_count = 1;
    definition.logic = &logicOr;
    definition.level_input = &levelInput;
    definition.level_input_write = &writeLevelInput;
    definition.level_output_write = &writeLevelOutput;
    definition.test = &testAndGate;
    if (!custom_logic.add(definition)) return 5;

    {
        std::ifstream file(std::string(h->data_directory_utf8) + "/autotest.txt");
        autotest = static_cast<bool>(file);
    }

    auto* target = h->resolve_symbol(
        h->context, "sim_do__modelZsimulationZcompile95thread_u3036");
    if (!target) return 6;
    if (h->create_hook(h->context, target,
                       reinterpret_cast<void*>(&interceptedSimDo),
                       reinterpret_cast<void**>(&sim_do_original)) != 0) {
        return 7;
    }

    if (autotest) {
        load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
            h->context, "load_level__modelZutilities_u7740"));
        set_sim_test = reinterpret_cast<SetSimTest>(h->resolve_symbol(
            h->context, "set_sim_test__modelZutilities_u6840"));
        compile_request = reinterpret_cast<CompileRequest>(h->resolve_symbol(
            h->context, "preorder__modelZsimulationZcompile95thread_u3523"));
        auto* update_target = h->resolve_symbol(
            h->context,
            "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
        auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
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
