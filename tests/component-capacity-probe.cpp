// Development-only real-game probe for the persistence budget: one instance with
// a full 1024-byte configuration (the documented cap, 128 chunk entries plus the
// four header entries) written through tc.component.storage, saved with the
// game's own saver, and read back after a restart.  It also reports how much the
// schematic grew, because "the cap is usable" and "the cap is cheap" are two
// different questions and the plan asked for the first one to be measured before
// the second is assumed.

#include "../sdk/tc_mod.h"
#include "../sdk/tc_component_types.h"
#include "../sdk/tc_component_instances.h"
#include "../sdk/tc_component_storage.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

struct V2 {
    float x, y;
};

constexpr uint64_t kCustomId = 0x4E4F54315F303031ULL;
constexpr uint32_t kSchema = 7;
/* The documented persistence cap.  The in-memory service takes 64 KiB; this is
   what a single record may carry. */
constexpr uint32_t kConfigSize = 1024;
constexpr uint64_t kComponentStride = 0x238, kHeader = 8, kCustomKind = 0x4e;
constexpr uint64_t kTailTableOffset = 0x190;

const TCHost* host;
tc::TCMod mod;
void* model;
bool registered, done, started;
double start_time, current_time, loadTime;
bool (*invisible_original)(const char*, V2, int);
int test_frame = -1, test_button_index;
using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
UpdateWire update_original;
using LoadLevel = void (*)(void*, const tc::TCNimString*);
LoadLevel load_level;
using SetSimTest = void (*)(void*, int64_t, int64_t);
SetSimTest set_sim_test;
using CompileRequest = void (*)(void*, tc::TCNimString*, int64_t);
CompileRequest compile_request;
using GetCommandSetting = uint64_t (*)(uint32_t);
GetCommandSetting get_command_setting;
using SaveThisSchematic = void (*)(const tc::TCNimString*, void*, void*, void*, uint64_t);
SaveThisSchematic save_this_schematic;
tc::TCNimString* loaded_level_global;
tc::component_instances::Api instances{};
tc::component_storage::Api storage{};
int stage = 0;
double stage_time = 0;

void log(const std::string& message) {
    host->log(host->context, message.c_str());
}

int64_t readU64(const void* base, size_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
    return static_cast<int64_t>(value);
}

const unsigned char* subjectRecord() {
    const auto* board = static_cast<const unsigned char*>(model) + 0x78;
    const uint64_t count = static_cast<uint64_t>(readU64(board, 0));
    const unsigned char* data = nullptr;
    std::memcpy(&data, board + 8, sizeof(data));
    for (uint64_t i = 0; i < count && data; ++i) {
        const unsigned char* record = data + kHeader + i * kComponentStride;
        if (record[0] != kCustomKind) continue;
        if (readU64(record, 0x188) != static_cast<int64_t>(kCustomId)) continue;
        return record;
    }
    return nullptr;
}

/* Live entries in the record's own tail table: what the circuit actually carries
   for this one component. */
uint64_t tailEntries(const unsigned char* record) {
    if (!record) return 0;
    const uint64_t slots = static_cast<uint64_t>(readU64(record, kTailTableOffset));
    const unsigned char* elements = nullptr;
    std::memcpy(&elements, record + kTailTableOffset + 8, sizeof(elements));
    if (!slots || slots > 0x100000 || !elements) return 0;
    uint64_t live = 0;
    for (uint64_t i = 0; i < slots; ++i)
        if (readU64(elements + 8 + i * 24, 0) != 0) ++live;
    return live;
}

uint64_t schematicSize() {
    const char* appdata = std::getenv("APPDATA");
    if (!appdata) return 0;
    const auto path = std::filesystem::u8path(appdata) /
        "Turing Complete Mods/profiles/default/schematics/not_gate/Default/circuit.data";
    std::error_code status;
    const auto size = std::filesystem::file_size(path, status);
    return status ? 0 : static_cast<uint64_t>(size);
}

bool writeFullConfig() {
    TCComponentInstanceHandle handles[4]{};
    uint32_t written = 0, total = 0;
    if (tc::component_instances::enumerate(instances, kCustomId, handles, 4, &written, &total) !=
            TC_COMPONENT_INSTANCES_OK ||
        !written)
        return false;
    std::vector<uint8_t> bytes(kConfigSize);
    for (uint32_t i = 0; i < kConfigSize; ++i) bytes[i] = static_cast<uint8_t>(i * 31u + 7u);
    return storage.write_config(storage.context, &handles[0], kSchema, bytes.data(), kConfigSize) ==
           TC_COMPONENT_STORAGE_OK;
}

std::string readFullConfig() {
    TCComponentInstanceHandle handles[4]{};
    uint32_t written = 0, total = 0;
    if (tc::component_instances::enumerate(instances, kCustomId, handles, 4, &written, &total) !=
            TC_COMPONENT_INSTANCES_OK ||
        !written)
        return {};
    std::vector<uint8_t> bytes(kConfigSize, 0);
    uint32_t count = 0;
    if (storage.read_config(storage.context, &handles[0], bytes.data(),
                            static_cast<uint32_t>(bytes.size()), &count) !=
            TC_COMPONENT_STORAGE_OK ||
        count != kConfigSize)
        return {};
    bool ok = true;
    for (uint32_t i = 0; i < kConfigSize; ++i)
        if (bytes[i] != static_cast<uint8_t>(i * 31u + 7u)) ok = false;
    return ok ? "identical" : "different";
}

/* Which launch this is: the first one writes and saves, the second one only
   reads what the file came back with. */
uint32_t nextLaunch() {
    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    const auto path = folder / "launch.txt";
    uint32_t launch = 0;
    { std::ifstream input(path); input >> launch; }
    std::ofstream(path, std::ios::trunc) << (launch + 1);
    return launch + 1;
}

void saveSchematic() {
    if (!loaded_level_global || !model) return;
    const char* level_text = "not_gate";
    tc::TCNimString level{};
    mod.game.raw_new_string(&level, 8);
    level.length = 8;
    std::memcpy(static_cast<unsigned char*>(level.data) + 8, level_text, 8);
    static_cast<unsigned char*>(level.data)[16] = 0;
    *loaded_level_global = level;
    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !save_this_schematic || !get_command_setting) return;
    const auto path = (std::filesystem::u8path(appdata) /
        "Turing Complete Mods/profiles/default/schematics/not_gate/Default/circuit.data").generic_u8string();
    tc::TCNimString target{};
    mod.game.raw_new_string(&target, static_cast<int64_t>(path.size()));
    target.length = path.size();
    std::memcpy(static_cast<unsigned char*>(target.data) + 8, path.data(), path.size());
    static_cast<unsigned char*>(target.data)[8 + path.size()] = 0;
    void* board = static_cast<unsigned char*>(model) + 0x78;
    void* model_field = nullptr;
    std::memcpy(&model_field, static_cast<unsigned char*>(model) + 0x48, sizeof(model_field));
    save_this_schematic(&target, model, model_field, board, get_command_setting(2));
}

void logic(TCLogicIOV2* io) {
    if (io->phase == TC_LOGIC_RESET) return;
    for (uint32_t i = 0; i < io->output_count; ++i) io->outputs[i] = 0;
    if (io->input_count && io->output_count) io->outputs[0] = io->inputs[0] ? 0u : 1u;
}

bool hookedUpdate(void* m, void* context, void* input, uint32_t point, uint8_t fifth) {
    model = m;
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
        if (current_time > 4.0 && test_button_index == 2) return true;
    }
    return result;
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    current_time = value->time_seconds - start_time;
    if (done || !registered || !model || current_time < 5.0) return;
    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    auto finish = [&](const std::string& message) {
        done = true;
        log(message);
        std::ofstream(folder / "result.txt") << message;
    };
    if (stage == 0) {
        const char* text = "not_gate";
        const size_t length = std::strlen(text);
        tc::TCNimString name{};
        mod.game.raw_new_string(&name, static_cast<int64_t>(length));
        name.length = length;
        std::memcpy(static_cast<unsigned char*>(name.data) + 8, text, length);
        static_cast<unsigned char*>(name.data)[8 + length] = 0;
        load_level(model, &name);
        stage = 1;
        stage_time = current_time;
        return;
    }
    if (stage == 1) {
        if (current_time < stage_time + 2.0) return;
        if (set_sim_test) set_sim_test(model, 0, 1);
        tc::TCNimString progress{};
        if (compile_request) compile_request(model, &progress, 0);
        stage = 2;
        stage_time = current_time;
        return;
    }
    if (current_time < stage_time + 2.0) return;
    const uint32_t launch = nextLaunch();
    const unsigned char* record = subjectRecord();
    if (!record) {
        finish("FAIL capacity probe found no instance of the registered component");
        return;
    }
    std::string text = "launch=" + std::to_string(launch) +
                       " entries=" + std::to_string(tailEntries(record)) +
                       " file=" + std::to_string(schematicSize());
    if (launch == 1) {
        if (!writeFullConfig()) {
            finish("FAIL the full 1024-byte configuration was refused");
            return;
        }
        text += " wrote=" + readFullConfig() +
                " entriesAfterWrite=" + std::to_string(tailEntries(subjectRecord()));
        saveSchematic();
        finish("PASS capacity written " + text);
        return;
    }
    text += " readback=" + readFullConfig();
    if (readFullConfig() != "identical") {
        finish("FAIL the 1024-byte configuration did not survive the save and restart: " + text);
        return;
    }
    finish("PASS capacity survived save plus restart: " + text);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;

    static const TCComponentPinV2 inputs[] = {{"a", "Input", 1, 0}};
    static const TCComponentPinV2 outputs[] = {{"y", "Output", 1, 0}};
    tc::component_types::Api types{};
    if (!tc::component_types::table(h, &types)) return 3;
    static std::vector<uint8_t> defaultConfig(kConfigSize, 0);
    TCComponentTypeDefinitionV2 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_2;
    definition.custom_id = kCustomId;
    definition.name = "Capacity Test";
    definition.inputs = inputs;
    definition.input_count = 1;
    definition.outputs = outputs;
    definition.output_count = 1;
    definition.callback = &logic;
    definition.state_words = 2;
    definition.config_schema = kSchema;
    definition.config_size = kConfigSize;
    definition.default_config = defaultConfig.data();
    if (tc::component_types::registerDefinition(types, &definition) != TC_COMPONENT_TYPES_OK)
        return 4;
    if (!tc::component_instances::table(h, &instances) || !tc::component_storage::table(h, &storage))
        return 5;
    registered = true;

    load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
        h->context, "load_level__modelZutilities_u7740"));
    set_sim_test = reinterpret_cast<SetSimTest>(h->resolve_symbol(
        h->context, "set_sim_test__modelZutilities_u6840"));
    compile_request = reinterpret_cast<CompileRequest>(h->resolve_symbol(
        h->context, "preorder__modelZsimulationZcompile95thread_u3523"));
    get_command_setting = reinterpret_cast<GetCommandSetting>(h->resolve_symbol(
        h->context, "get_command_setting__modelZsimulator95types_u124"));
    save_this_schematic = reinterpret_cast<SaveThisSchematic>(h->resolve_symbol(
        h->context, "save_this_schematic__modelZboardZschematics_u132"));
    loaded_level_global = static_cast<tc::TCNimString*>(h->resolve_symbol(
        h->context, "loaded_level__modelZmodel95types_u840"));
    auto* update_target = h->resolve_symbol(
        h->context, "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
    if (!load_level || !set_sim_test || !compile_request || !get_command_setting ||
        !save_this_schematic || !loaded_level_global || !update_target || !invisible_target)
        return 6;
    if (h->create_hook(h->context, update_target, reinterpret_cast<void*>(hookedUpdate),
                       reinterpret_cast<void**>(&update_original)) != 0)
        return 7;
    if (h->create_hook(h->context, invisible_target, reinterpret_cast<void*>(hookedInvisible),
                       reinterpret_cast<void**>(&invisible_original)) != 0)
        return 8;
    plugin->on_frame = frame;
    return 0;
}
