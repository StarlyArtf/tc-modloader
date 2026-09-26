// Development-only real-game probe for tc.component.storage's persistent half.
//
// It registers one declarative component with a four-byte configuration, loads
// the not_gate level whose saved schematic carries an instance of it, writes the
// configuration through the service, saves the schematic with the game's own
// saver, and reports the value it read back.  The PowerShell wrapper runs it
// three times in one isolated profile: the values the second and third launch
// read can only come from the file the first one wrote.
//
// TC_STORAGE_MIGRATE=accept|reject switches the same probe to the schema
// migration path: the fixture's circuit carries a record written under schema 6
// and the definition registers schema 7, so the launch has to report what the
// migration was given and what the instance ended up running on.

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

namespace {

struct V2 {
    float x, y;
};

/* "NOT1_001": the definition tests/and-component-fixture.cpp writes into
   build/nl_not1board.data, wired for the declarative pin geometry. */
constexpr uint64_t kCustomId = 0x4E4F54315F303031ULL;
constexpr uint32_t kSchema = 7;
constexpr uint32_t kConfigSize = 4;
constexpr uint8_t kDefaultConfig[kConfigSize] = {0x11, 0x22, 0x33, 0x44};
constexpr uint8_t kFirstConfig[kConfigSize] = {0xaa, 0xbb, 0xcc, 0xdd};
constexpr uint8_t kSecondConfig[kConfigSize] = {0x01, 0x23, 0x45, 0x67};
/* What build/nl_not1legacy.data carries and what the migration turns it into;
   the fixture generates both from the same literals (tests/and-component-fixture.cpp). */
constexpr uint32_t kLegacySchema = 6;
constexpr uint8_t kLegacyConfig[kConfigSize] = {0x0f, 0x1e, 0x2d, 0x3c};
constexpr uint8_t kMigratedConfig[kConfigSize] = {0xa5, 0x5a, 0x0f, 0xf0};

/* persist writes and reads a configuration back; accept and reject exercise a
   circuit that carries an older schema. */
enum class StorageMode {
    Persist,
    MigrateAccept,
    MigrateReject,
};

const TCHost* host;
tc::TCMod mod;
StorageMode mode = StorageMode::Persist;
uint32_t migrationCalls;
bool migrationSawLegacy;
/* The definition's configuration-change notification: the run reports how often
   it fired, so the playtest can assert that a write commits once, that an
   unchanged write commits nothing, and that loading a stored record (or migrating
   an old one) is a load rather than a change. */
uint32_t configChanges;
uint32_t configChangedSeen;
/* The load and save moments: `load` says the configuration came out of the file,
   `save` counts the dispatches that reached this instance before the game wrote. */
uint32_t loadCalls, saveCalls;
void loaded(TCLogicIOV2* io) {
    if (io->phase != TC_LOGIC_LOAD) return;
    ++loadCalls;
}
void saving(TCLogicIOV2* io) {
    if (io->phase != TC_LOGIC_SAVE) return;
    ++saveCalls;
}
void configChanged(TCLogicIOV2* io) {
    if (io->phase != TC_LOGIC_CONFIG_CHANGED) return;
    ++configChanges;
    if (io->config && io->config_size) configChangedSeen = io->config[0];
}
void* model;
bool registered;
bool loaded_level;
bool done;
bool started;
double start_time;
double current_time;
bool (*invisible_original)(const char*, V2, int);
int test_frame = -1;
int test_button_index;
using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
UpdateWire update_original;
using LoadLevel = void (*)(void*, const tc::TCNimString*);
LoadLevel load_level;
tc::TCNimString* loaded_level_global;
using GetCommandSetting = uint64_t (*)(uint32_t);
GetCommandSetting get_command_setting;
using SaveThisSchematic = void (*)(const tc::TCNimString*, void*, void*, void*, uint64_t);
SaveThisSchematic save_this_schematic;
/* The same explicit compile the example's autotest uses: a level that is merely
   loaded is not compiled, and an uncompiled board has no bound instance. */
using SetSimTest = void (*)(void*, int64_t, int64_t);
SetSimTest set_sim_test;
using CompileRequest = void (*)(void*, tc::TCNimString*, int64_t);
CompileRequest compile_request;
tc::component_instances::Api instances{};
tc::component_storage::Api storage{};
int stage = 0;
double stage_time = 0;

void log(const std::string& message) {
    host->log(host->context, message.c_str());
}

std::string hex(const uint8_t* bytes, uint32_t count) {
    std::string text;
    for (uint32_t i = 0; i < count; ++i) {
        char pair[4];
        std::snprintf(pair, sizeof(pair), "%02x", bytes[i]);
        text += pair;
    }
    return text;
}

/* The level's own test only passes when the callback negates its input, which
   is what keeps the compile (and therefore the binding) exercised by the real
   level rather than by a free-floating component. */
void logic(TCLogicIOV2* io) {
    if (io->phase == TC_LOGIC_RESET) return;
    for (uint32_t i = 0; i < io->output_count; ++i) io->outputs[i] = 0;
    if (io->input_count && io->output_count) io->outputs[0] = io->inputs[0] ? 0u : 1u;
}

/* What the circuit stored under schema 6 is converted into.  The callback
   records what it was handed, so the run can assert the host passed the *old*
   bytes (a migration that silently received the default would be worse than no
   migration at all). */
int migrateConfig(void*, uint32_t from_schema, const void* from_data, uint32_t from_bytes,
                  uint32_t to_schema, void* out, uint32_t capacity) {
    ++migrationCalls;
    const auto* bytes = static_cast<const uint8_t*>(from_data);
    migrationSawLegacy = from_schema == kLegacySchema && from_bytes == kConfigSize &&
                         to_schema == kSchema && capacity == kConfigSize && bytes &&
                         std::memcmp(bytes, kLegacyConfig, kConfigSize) == 0;
    if (mode == StorageMode::MigrateReject) return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
    std::memcpy(out, kMigratedConfig, kConfigSize);
    return TC_COMPONENT_CONFIG_MIGRATE_OK;
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

/* The game's own schematic save for the level the fixture was installed as. */
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

/* Which launch of the sandbox this is.  A fresh profile starts at 1, and the
   wrapper asserts a different outcome per launch, so the run number is part of
   the evidence rather than something the reader has to infer from the order. */
uint32_t nextLaunch() {
    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    const auto path = folder / "launch.txt";
    uint32_t launch = 0;
    { std::ifstream input(path); input >> launch; }
    std::ofstream(path, std::ios::trunc) << (launch + 1);
    return launch + 1;
}

/* Reads the configuration, decides what this launch has to do, and reports the
   value it wrote back.  The launches are distinguished by what the previous one
   stored in the schematic. */
void runStorage(const std::function<void(const std::string&)>& finish) {
    const uint32_t launch = nextLaunch();
    TCComponentInstanceHandle handles[4]{};
    uint32_t written = 0, total = 0;
    if (tc::component_instances::enumerate(instances, kCustomId, handles, 4, &written, &total) !=
            TC_COMPONENT_INSTANCES_OK ||
        !written) {
        finish("FAIL storage probe found no instance of " + std::to_string(kCustomId));
        return;
    }
    const TCComponentInstanceHandle handle = handles[0];
    TCComponentStorageInfoV1 info{};
    info.size = sizeof(info);
    if (storage.info(storage.context, &handle, &info) != TC_COMPONENT_STORAGE_OK) {
        finish("FAIL storage info refused the instance");
        return;
    }
    log("storage: instance=0x" + std::to_string(handle.instance_id) +
        " config_size=" + std::to_string(info.config_size) +
        " schema=" + std::to_string(info.config_schema) +
        " revision=" + std::to_string(info.config_revision) +
        " flags=" + std::to_string(info.flags));
    if (info.config_size != kConfigSize || info.config_schema != kSchema) {
        finish("FAIL storage info reported config_size=" + std::to_string(info.config_size) +
               " schema=" + std::to_string(info.config_schema));
        return;
    }
    if (!(info.flags & TC_COMPONENT_STORAGE_HAS_PERSISTENCE)) {
        finish("FAIL storage reported no persistence for the instance");
        return;
    }
    uint8_t value[kConfigSize]{};
    uint32_t bytes = 0;
    if (storage.read_config(storage.context, &handle, value, sizeof(value), &bytes) !=
            TC_COMPONENT_STORAGE_OK ||
        bytes != kConfigSize) {
        finish("FAIL storage read_config failed");
        return;
    }

    if (mode == StorageMode::Persist) {
        if (migrationCalls) {
            finish("FAIL storage called the migration for a record that matches its definition");
            return;
        }
        const uint8_t* expected = value;
        const uint64_t revisionBefore = info.config_revision;
        std::string step;
        bool wrote = false;
        if (std::memcmp(value, kDefaultConfig, kConfigSize) == 0) {
            step = "default";
            expected = kFirstConfig;
            wrote = true;
        } else if (std::memcmp(value, kFirstConfig, kConfigSize) == 0) {
            step = "readback-first";
            expected = kSecondConfig;
            wrote = true;
        } else if (std::memcmp(value, kSecondConfig, kConfigSize) == 0) {
            step = "readback-second";
            expected = kSecondConfig;
        } else {
            finish("FAIL storage probe read an unexpected configuration " + hex(value, kConfigSize));
            return;
        }
        if (revisionBefore != 1) {
            finish("FAIL storage revision before the write was " + std::to_string(revisionBefore));
            return;
        }
        if (wrote) {
            if (storage.write_config(storage.context, &handle, kSchema, expected, kConfigSize) !=
                TC_COMPONENT_STORAGE_OK) {
                finish("FAIL storage write_config refused " + hex(expected, kConfigSize));
                return;
            }
            TCComponentStorageInfoV1 after{};
            after.size = sizeof(after);
            if (storage.info(storage.context, &handle, &after) != TC_COMPONENT_STORAGE_OK ||
                after.config_revision != revisionBefore + 1) {
                finish("FAIL storage write_config did not advance the revision");
                return;
            }
        }
        uint8_t readBack[kConfigSize]{};
        if (storage.read_config(storage.context, &handle, readBack, sizeof(readBack), &bytes) !=
                TC_COMPONENT_STORAGE_OK ||
            bytes != kConfigSize || std::memcmp(readBack, expected, kConfigSize) != 0) {
            finish("FAIL storage read back " + hex(readBack, kConfigSize) + " after " +
                   hex(expected, kConfigSize));
            return;
        }
        if (wrote) saveSchematic();
        finish("PASS storage launch=" + std::to_string(launch) + " " + step + " value=" +
               hex(value, kConfigSize) + " stored=" + hex(expected, kConfigSize) + " revision=" +
               std::to_string(revisionBefore) + "->" +
               std::to_string(wrote ? revisionBefore + 1 : revisionBefore) + " persistence=1" +
               " config_changes=" + std::to_string(configChanges) + " loads=" + std::to_string(loadCalls) + " saves=" + std::to_string(saveCalls));
        return;
    }

    /* Migration: the circuit carries a schema-6 record and this definition
       registers schema 7.  Accept commits the converted record (so the next
       launch reads it back without a migration), reject changes nothing (so the
       next launch is offered the same bytes again). */
    const bool migrated = std::memcmp(value, kMigratedConfig, kConfigSize) == 0;
    const bool legacy = std::memcmp(value, kLegacyConfig, kConfigSize) == 0;
    const bool refused = std::memcmp(value, kDefaultConfig, kConfigSize) == 0;
    if (mode == StorageMode::MigrateAccept) {
        if (legacy) {
            finish("FAIL the schema-6 bytes were applied as if they were current: value=" +
                   hex(value, kConfigSize));
            return;
        }
        if (!migrated) {
            finish("FAIL migration did not become the live configuration: value=" +
                   hex(value, kConfigSize) + " migrations=" + std::to_string(migrationCalls));
            return;
        }
        if (migrationCalls == 1) {
            if (!migrationSawLegacy) {
                finish("FAIL migration was called with the wrong bytes");
                return;
            }
            saveSchematic();
            finish("PASS storage launch=" + std::to_string(launch) +
                   " migrated value=" + hex(value, kConfigSize) + " migrations=1 legacy=1" +
                   " config_changes=" + std::to_string(configChanges) + " loads=" + std::to_string(loadCalls) + " saves=" + std::to_string(saveCalls));
            return;
        }
        if (migrationCalls != 0) {
            finish("FAIL migration ran again for an upgraded record, calls=" +
                   std::to_string(migrationCalls));
            return;
        }
        finish("PASS storage launch=" + std::to_string(launch) + " upgraded-record value=" +
               hex(value, kConfigSize) + " migrations=0 legacy=0 config_changes=" +
               std::to_string(configChanges) + " loads=" + std::to_string(loadCalls) + " saves=" + std::to_string(saveCalls));
        return;
    }
    /* Reject: the instance runs on the default and the old bytes survive, which
       is why every launch is offered them again. */
    if (migrated || legacy) {
        finish("FAIL a refused migration changed the live configuration: value=" +
               hex(value, kConfigSize));
        return;
    }
    if (!refused) {
        finish("FAIL rejected migration changed the live configuration: value=" +
               hex(value, kConfigSize));
        return;
    }
    if (migrationCalls != 1 || !migrationSawLegacy) {
        finish("FAIL rejected migration was not offered the stored bytes, calls=" +
               std::to_string(migrationCalls) + " legacy=" + std::to_string(migrationSawLegacy ? 1 : 0));
        return;
    }
    finish("PASS storage launch=" + std::to_string(launch) + " " +
           (launch > 1 ? "refused-again" : "refused") + " value=" + hex(value, kConfigSize) +
           " migrations=1 legacy=1 config_changes=" + std::to_string(configChanges) + " loads=" + std::to_string(loadCalls) + " saves=" + std::to_string(saveCalls));
    return;
}

}  // namespace

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    current_time = value->time_seconds - start_time;
    if (done || !registered || !model || current_time < 5.0) return;
    if (stage == 0) {
        const char* text = "not_gate";
        const size_t length = std::strlen(text);
        tc::TCNimString name{};
        mod.game.raw_new_string(&name, static_cast<int64_t>(length));
        name.length = length;
        std::memcpy(static_cast<unsigned char*>(name.data) + 8, text, length);
        static_cast<unsigned char*>(name.data)[8 + length] = 0;
        load_level(model, &name);
        loaded_level = true;
        stage = 1;
        stage_time = current_time;
        return;
    }
    if (stage == 1) {
        if (current_time < stage_time + 2.0) return;
        if (set_sim_test) set_sim_test(model, 0, 1);
        tc::TCNimString progress{};
        compile_request(model, &progress, 0);
        log("storage: requested a compile of the loaded level");
        stage = 2;
        stage_time = current_time;
        return;
    }
    if (current_time < stage_time + 2.0) return;
    done = true;

    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    auto finish = [&](const std::string& text) {
        log(text);
        std::ofstream(folder / "result.txt") << text;
    };
    runStorage(finish);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;
    if (const char* requested = std::getenv("TC_STORAGE_MIGRATE")) {
        if (std::strcmp(requested, "accept") == 0) mode = StorageMode::MigrateAccept;
        else if (std::strcmp(requested, "reject") == 0) mode = StorageMode::MigrateReject;
    }

    static const TCComponentPinV2 inputs[] = {{"a", "Input", 1, 0}};
    static const TCComponentPinV2 outputs[] = {{"y", "Output", 1, 0}};
    tc::component_types::Api types{};
    if (!tc::component_types::table(h, &types)) return 3;
    TCComponentTypeDefinitionV2 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_2;
    definition.custom_id = kCustomId;
    definition.name = "Storage Test";
    definition.inputs = inputs;
    definition.input_count = 1;
    definition.outputs = outputs;
    definition.output_count = 1;
    definition.callback = &logic;
    definition.state_words = 2;
    definition.config_schema = kSchema;
    definition.config_size = kConfigSize;
    definition.default_config = kDefaultConfig;
    definition.config_migration_version = TC_COMPONENT_CONFIG_MIGRATION_VERSION_1;
    definition.migrate_config = &migrateConfig;
    static const TCComponentLifecycleV1 lifecycle = {
        sizeof(TCComponentLifecycleV1), TC_COMPONENT_LIFECYCLE_VERSION_1, nullptr, nullptr,
        &configChanged, nullptr, &loaded, &saving};
    definition.lifecycle = &lifecycle;
    if (tc::component_types::registerDefinition(types, &definition) != TC_COMPONENT_TYPES_OK)
        return 4;
    if (!tc::component_instances::table(h, &instances) ||
        !tc::component_storage::table(h, &storage))
        return 5;
    registered = true;

    load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
        h->context, "load_level__modelZutilities_u7740"));
    loaded_level_global = static_cast<tc::TCNimString*>(h->resolve_symbol(
        h->context, "loaded_level__modelZmodel95types_u840"));
    get_command_setting = reinterpret_cast<GetCommandSetting>(h->resolve_symbol(
        h->context, "get_command_setting__modelZsimulator95types_u124"));
    save_this_schematic = reinterpret_cast<SaveThisSchematic>(h->resolve_symbol(
        h->context, "save_this_schematic__modelZboardZschematics_u132"));
    set_sim_test = reinterpret_cast<SetSimTest>(h->resolve_symbol(
        h->context, "set_sim_test__modelZutilities_u6840"));
    compile_request = reinterpret_cast<CompileRequest>(h->resolve_symbol(
        h->context, "preorder__modelZsimulationZcompile95thread_u3523"));
    auto* update_target = h->resolve_symbol(
        h->context,
        "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
    if (!load_level || !loaded_level_global || !get_command_setting ||
        !save_this_schematic || !set_sim_test || !compile_request ||
        !update_target || !invisible_target)
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
