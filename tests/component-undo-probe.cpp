// Development-only real-game probe: can a configuration commit become one Ctrl+Z?
//
// The experiment drives the host's configuration transaction: a registered
// component is bound (so tc.component.storage owns its record), two configurations
// are written through the service, and then the game's own undo entry is asked to
// undo - through the same command bus the placement case uses.  The loader answers
// that press from its own edit stack, so the claim under test is:
//
//   one undo  -> the record holds the first configuration again;
//   one redo  -> the record holds the second configuration again.
//
// The record is read directly (table at +0x190) rather than through the service,
// because a binding keeps an in-memory copy and would hide a board-level mistake.
//
// The configuration is written straight into the record instead of through
// tc.component.storage on purpose: a binding keeps an in-memory copy until the
// next compile, so the service is not a trustworthy way to read a board edit back.

#include "../sdk/tc_mod.h"
#include "../sdk/tc_component_types.h"
#include "../sdk/tc_command_api.h"
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

constexpr uint64_t kCustomId = 0x4E4F54315F303031ULL;   /* NOT1_001, as in the fixture */
constexpr uint32_t kSchema = 7, kConfigSize = 4;
constexpr uint8_t kDefaultConfig[kConfigSize] = {0x11, 0x22, 0x33, 0x44};
constexpr uint8_t kFirstConfig[kConfigSize] = {0xa1, 0xa2, 0xa3, 0xa4};
constexpr uint8_t kSecondConfig[kConfigSize] = {0xb1, 0xb2, 0xb3, 0xb4};
/* The end state of the grouped edit: one undo has to take the record back to
   kSecondConfig, not to anything in between. */
constexpr uint8_t kThirdConfig[kConfigSize] = {0xc1, 0xc2, 0xc3, 0xc4};
constexpr uint8_t kFourthConfig[kConfigSize] = {0xd1, 0xd2, 0xd3, 0xd4};
constexpr uint64_t kComponentStride = 0x238, kHeader = 8, kCustomKind = 0x4e;
constexpr uint64_t kTailTableOffset = 0x190;

const TCHost* host;
tc::TCMod mod;
void* model;
bool registered, done, started;
double start_time, current_time;
double loadTime;
bool (*invisible_original)(const char*, V2, int);
int test_frame = -1, test_button_index;

using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
UpdateWire update_original;
using LoadLevel = void (*)(void*, const tc::TCNimString*);
LoadLevel load_level;
using AddComponent = bool (*)(void*, void*);
AddComponent add_component;
using SelectComponent = void (*)(void*, void*);
SelectComponent select_component;
using TryRotate = void* (*)(void*);
TryRotate try_rotate_component;
using TableSet = void (*)(void*, int64_t, int64_t);
TableSet tail_set;
TCBoardApiV5 boardApi{};
TCCommandApiV2 commandApi{};
tc::component_instances::Api instances{};
tc::component_storage::Api storage{};
/* V2 adds the explicit edit transaction (plan §9.3): begin, several writes, then
   commit as one undo step or abort back to the bytes at begin. */
tc::component_storage::ApiV2 storage2{};
TCGameHandle boardHandle{};
uint64_t commandRequest = 0;
bool pendingUndo = false, pendingRedo = false;
bool pendingDuplicate = false;
std::string pendingText;
int placeX = 0, placeY = 6;
uint64_t subjectInstance = 0;
int subjectX = -5, subjectY = 0;
/* The clone notification, counted the way a definition would count it. */
uint32_t cloneCalls;
uint32_t cloneConfigSeen;
int cloneX = 0, cloneY = 6;
bool pendingCloneCheck = false;
double cloneDeadline = 0;
void lifecycleClone(TCLogicIOV2* io) {
    if (io->phase != TC_LOGIC_CLONE) return;
    ++cloneCalls;
    if (io->config && io->config_size) cloneConfigSeen = io->config[0];
}
using SetSimTest = void (*)(void*, int64_t, int64_t);
SetSimTest set_sim_test;
using CompileRequest = void (*)(void*, tc::TCNimString*, int64_t);
CompileRequest compile_request;
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

int64_t readU64(const void* base, size_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
    return static_cast<int64_t>(value);
}

const unsigned char* componentData(uint64_t* count) {
    const auto* board = static_cast<const unsigned char*>(model) + 0x78;
    *count = static_cast<uint64_t>(readU64(board, 0));
    const unsigned char* data = nullptr;
    std::memcpy(&data, board + 8, sizeof(data));
    return data;
}

/* The record of our type at a grid point: the game's placement writes the point
   first, so this is how the experiment finds the instance it just added. */
const unsigned char* recordAt(int x, int y) {
    uint64_t count = 0;
    const unsigned char* data = componentData(&count);
    for (uint64_t i = 0; i < count && data; ++i) {
        const unsigned char* record = data + kHeader + i * kComponentStride;
        if (record[0] != kCustomKind) continue;
        if (readU64(record, 0x188) != static_cast<int64_t>(kCustomId)) continue;
        int32_t point = 0;
        std::memcpy(&point, record + 2, sizeof(point));
        if (static_cast<int16_t>(point & 0xffff) != x) continue;
        if (static_cast<int16_t>(point >> 16) != y) continue;
        return record;
    }
    return nullptr;
}

uint64_t liveComponents() {
    uint64_t count = 0;
    const unsigned char* data = componentData(&count);
    uint64_t live = 0;
    for (uint64_t i = 0; i < count && data; ++i)
        if ((data + kHeader + i * kComponentStride)[0]) ++live;
    return live;
}

/* The configuration as the record itself carries it: the table at +0x190, its
   storage at +0x198, 24-byte elements from storage + 8, key at +8, value at +0x10. */
std::string recordConfigOf(const unsigned char* record) {
    if (!record) return {};
    const uint64_t slots = static_cast<uint64_t>(readU64(record, kTailTableOffset));
    const unsigned char* elements = nullptr;
    std::memcpy(&elements, record + kTailTableOffset + 8, sizeof(elements));
    if (!slots || slots > 0x100000 || !elements) return {};
    int64_t chunk = -1;
    for (uint64_t i = 0; i < slots; ++i) {
        const unsigned char* element = elements + 8 + i * 24;
        if (readU64(element, 0) == 0) continue;
        if (readU64(element, 8) == 0x54434D3300001000LL) chunk = readU64(element, 0x10);
    }
    if (chunk < 0) return {};
    const uint64_t word = static_cast<uint64_t>(chunk);
    uint8_t bytes[kConfigSize]{};
    std::memcpy(bytes, &word, kConfigSize);
    return hex(bytes, kConfigSize);
}

/* Writes four bytes into a record through the game's own setter, in the same
   order the loader's storage path uses: data chunk first, then the header the
   reader gates on. */
bool writeRecordConfig(unsigned char* record, const uint8_t* bytes) {
    if (!record || !tail_set) return false;
    void* table = record + kTailTableOffset;
    uint64_t chunk = 0;
    std::memcpy(&chunk, bytes, kConfigSize);
    tail_set(table, static_cast<int64_t>(0x54434D3300001000LL), static_cast<int64_t>(chunk));
    tail_set(table, static_cast<int64_t>(0x54434D3300000002LL), readU64(record, 0x188));
    tail_set(table, static_cast<int64_t>(0x54434D3300000001LL), 1);
    tail_set(table, static_cast<int64_t>(0x54434D3300000003LL),
             static_cast<int64_t>((static_cast<uint64_t>(kSchema) << 32) | kConfigSize));
    tail_set(table, static_cast<int64_t>(0x54434D3300000004LL), 0x0123456789abcdefLL);
    return recordConfigOf(record) == hex(bytes, kConfigSize);
}

/* The instance the experiment edits: the fixture's own component, which is bound
   because the type is registered and the level was compiled. */
bool findSubject() {
    TCComponentInstanceHandle handles[4]{};
    uint32_t written = 0, total = 0;
    if (tc::component_instances::enumerate(instances, kCustomId, handles, 4, &written, &total) !=
            TC_COMPONENT_INSTANCES_OK ||
        !written)
        return false;
    subjectInstance = handles[0].instance_id;
    return true;
}

const unsigned char* subjectRecord() {
    uint64_t count = 0;
    const unsigned char* data = componentData(&count);
    for (uint64_t i = 0; i < count && data; ++i) {
        const unsigned char* record = data + kHeader + i * kComponentStride;
        if (record[0] != kCustomKind) continue;
        if (readU64(record, 0x188) != static_cast<int64_t>(kCustomId)) continue;
        if (static_cast<uint64_t>(readU64(record, 8)) != subjectInstance) continue;
        return record;
    }
    return nullptr;
}

std::string subjectConfig() { return recordConfigOf(subjectRecord()); }
std::string recordConfigAt(int x, int y) { return recordConfigOf(recordAt(x, y)); }

bool writeViaService(const uint8_t* bytes) {
    TCComponentInstanceHandle handles[4]{};
    uint32_t written = 0, total = 0;
    if (tc::component_instances::enumerate(instances, kCustomId, handles, 4, &written, &total) !=
            TC_COMPONENT_INSTANCES_OK ||
        !written)
        return false;
    return storage.write_config(storage.context, &handles[0], kSchema, bytes, kConfigSize) ==
           TC_COMPONENT_STORAGE_OK;
}

int submitCommand(uint32_t type, uint64_t* request) {
    TCCommandV2 command{};
    command.size = sizeof(command);
    command.type = type;
    command.subject = boardHandle;
    return commandApi.submit(commandApi.context, &command, request);
}

/* The command bus runs a request after the plugin callbacks of the frame that
   submitted it, so completion is observed on a later frame. */
bool commandFinished() {
    TCCommandStatusV1 status{};
    if (commandApi.get_status(commandApi.context, commandRequest, &status, sizeof(status)) !=
        TC_COMMAND_OK)
        return false;
    return status.state != TC_COMMAND_STATE_QUEUED && status.state != TC_COMMAND_STATE_RUNNING;
}

/* Two commits through the service, then one undo press and one redo press through
   the command bus - the same entry the player's Ctrl+Z reaches. */
void runExperiment(const std::function<void(const std::string&)>& finish) {
    if (!findSubject()) {
        finish("FAIL the undo probe found no bound instance of its component");
        return;
    }
    if (boardApi.get_current(boardApi.context, &boardHandle) != TC_HANDLE_OK) {
        finish("FAIL the undo probe could not name the current Board");
        return;
    }
    if (!writeViaService(kFirstConfig)) {
        finish("FAIL the first configuration could not be written through the service");
        return;
    }
    const std::string first = subjectConfig();
    if (first != hex(kFirstConfig, kConfigSize)) {
        finish("FAIL the first configuration is not on the board record: " + first);
        return;
    }
    if (!writeViaService(kSecondConfig)) {
        finish("FAIL the second configuration could not be written through the service");
        return;
    }
    const std::string second = subjectConfig();
    if (second != hex(kSecondConfig, kConfigSize)) {
        finish("FAIL the second configuration is not on the board record: " + second);
        return;
    }
    /* And now the grouped edit: three writes inside one transaction must cost one
       undo press, not three. */
    TCComponentInstanceHandle handle{};
    {
       uint64_t request = 0;
        TCComponentInstanceHandle handles[4]{};
        uint32_t written = 0, total = 0;
        if (tc::component_instances::enumerate(instances, kCustomId, handles, 4, &written, &total) !=
                TC_COMPONENT_INSTANCES_OK ||
            !written) {
            finish("FAIL the undo probe lost the instance before the grouped edit");
            return;
        }
        handle = handles[0];
        (void)request;
    }
    if (tc::component_storage::beginEdit(storage2, handle) != TC_COMPONENT_STORAGE_OK ||
        tc::component_storage::beginEdit(storage2, handle) != TC_COMPONENT_STORAGE_ERR_STATE) {
        finish("FAIL the edit transaction could not be opened");
        return;
    }
    if (!writeViaService(kThirdConfig) || !writeViaService(kFourthConfig)) {
        finish("FAIL a write inside the transaction was refused");
        return;
    }
    if (subjectConfig() != hex(kFourthConfig, kConfigSize)) {
        finish("FAIL the grouped edit did not reach its end state: " + subjectConfig());
        return;
    }
    if (tc::component_storage::commitEdit(storage2, handle) != TC_COMPONENT_STORAGE_OK ||
        tc::component_storage::commitEdit(storage2, handle) != TC_COMPONENT_STORAGE_ERR_STATE) {
        finish("FAIL the edit transaction could not be committed");
        return;
    }
    const std::string grouped = subjectConfig();
    pendingText = "instance=" + std::to_string(subjectInstance) + " wrote=" + first + " then=" + second +
                  " grouped=" + grouped;
    uint64_t request = 0;
    if (submitCommand(TC_COMMAND_BOARD_UNDO, &request) != TC_COMMAND_OK) {
        finish("FAIL the undo command was refused; " + pendingText);
        return;
    }
    commandRequest = request;
    pendingUndo = true;
}

void finishExperiment(const std::function<void(const std::string&)>& finish) {
    const std::string config = subjectConfig();
    /* One press: the grouped edit goes back to what the record held when the
       transaction opened - kSecondConfig, not anything from inside the group. */
    if (config != hex(kSecondConfig, kConfigSize)) {
        finish("FAIL one undo did not revert the whole grouped edit (record has " + config +
               ", expected " + hex(kSecondConfig, kConfigSize) + "); " + pendingText);
        return;
    }
    uint64_t request = 0;
    if (submitCommand(TC_COMMAND_BOARD_REDO, &request) != TC_COMMAND_OK) {
        finish("FAIL the redo command was refused; " + pendingText);
        return;
    }
    commandRequest = request;
    pendingRedo = true;
}

void finishRedo(const std::function<void(const std::string&)>& finish) {
    const std::string config = subjectConfig();
    if (config != hex(kFourthConfig, kConfigSize)) {
        finish("FAIL one redo did not re-apply the whole grouped edit (record has " + config +
               ", expected " + hex(kFourthConfig, kConfigSize) + "); " + pendingText);
        return;
    }
    /* And now a duplication: the copy has to arrive with the source's bytes, and
       its first bind has to announce TC_LOGIC_CLONE. */
    TCCommandV2 duplicate{};
    duplicate.size = sizeof(duplicate);
    duplicate.type = TC_COMMAND_BOARD_DUPLICATE_COMPONENT;
    duplicate.subject = boardHandle;
    duplicate.custom_prototype_id = kCustomId;
    duplicate.argument = static_cast<int64_t>(subjectInstance);
    duplicate.x = cloneX;
    duplicate.y = cloneY;
    uint64_t request = 0;
    if (commandApi.submit(commandApi.context, &duplicate, &request) != TC_COMMAND_OK) {
        finish("FAIL the duplicate command was refused; " + pendingText);
        return;
    }
    commandRequest = request;
    pendingDuplicate = true;
}

/* The duplication has landed: the copy must carry the source's configuration. */
void finishDuplicate(const std::function<void(const std::string&)>& finish) {
    const std::string copied = recordConfigAt(cloneX, cloneY);
    const std::string source = subjectConfig();
    if (copied.empty() || copied != source) {
        finish("FAIL the duplicated component did not carry the source's configuration (copy has " +
               copied + ", source has " + source + "); " + pendingText);
        return;
    }
    pendingText += " clone=" + copied;
    /* A second compile binds the copy; that bind is what announces the clone. */
    tc::TCNimString progress{};
    if (compile_request) compile_request(model, &progress, 0);
    pendingCloneCheck = true;
    cloneDeadline = current_time + 2.0;
}

void finishCloneCheck(const std::function<void(const std::string&)>& finish) {
    if (!cloneCalls) {
        finish("FAIL the duplicated instance never announced TC_LOGIC_CLONE; " + pendingText);
        return;
    }
    if (cloneConfigSeen != kFourthConfig[0]) {
        finish("FAIL the clone notification saw the wrong configuration (first byte " +
               std::to_string(cloneConfigSeen) + ", expected " +
               std::to_string(kFourthConfig[0]) + "); " + pendingText);
        return;
    }
    finish("PASS one undo reverted a grouped configuration edit, one redo re-applied it, and a "
           "duplicated instance arrived with the source's configuration and announced itself: " +
           pendingText);
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
        /* A level that is merely loaded is not compiled, and an uncompiled board
           has no bound instance for the service to name. */
        if (set_sim_test) set_sim_test(model, 0, 1);
        tc::TCNimString progress{};
        if (compile_request) compile_request(model, &progress, 0);
        log("undo: requested a compile so the instance is bound");
        stage = 2;
        stage_time = current_time;
        return;
    }
    if (!done && current_time < stage_time + 2.0) return;
    if (!pendingUndo && !pendingRedo && !pendingDuplicate && !pendingCloneCheck) {
        runExperiment(finish);
        return;
    }
    if (pendingCloneCheck) {
        if (current_time < cloneDeadline) return;
        pendingCloneCheck = false;
        finishCloneCheck(finish);
        return;
    }
    if (!commandFinished()) return;
    if (pendingDuplicate) {
        pendingDuplicate = false;
        finishDuplicate(finish);
        return;
    }
    if (pendingUndo) {
        pendingUndo = false;
        finishExperiment(finish);
        return;
    }
    pendingRedo = false;
    finishRedo(finish);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;

    static const TCComponentPinV2 inputs[] = {{"a", "Input", 1, 0}};
    static const TCComponentPinV2 outputs[] = {{"y", "Output", 1, 0}};
    tc::component_types::Api types{};
    if (!tc::component_types::table(h, &types)) return 3;
    TCComponentTypeDefinitionV2 definition{};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_2;
    definition.custom_id = kCustomId;
    definition.name = "Undo Test";
    definition.inputs = inputs;
    definition.input_count = 1;
    definition.outputs = outputs;
    definition.output_count = 1;
    definition.callback = &logic;
    definition.state_words = 2;
    definition.config_schema = kSchema;
    definition.config_size = kConfigSize;
    definition.default_config = kDefaultConfig;
    static const TCComponentLifecycleV1 lifecycle = {
        sizeof(TCComponentLifecycleV1), TC_COMPONENT_LIFECYCLE_VERSION_1, nullptr, nullptr,
        nullptr, &lifecycleClone};
    definition.lifecycle = &lifecycle;
    if (tc::component_types::registerDefinition(types, &definition) != TC_COMPONENT_TYPES_OK)
        return 4;
    if (tc::boardService(h, &boardApi) != TC_SERVICE_OK || boardApi.version < TC_BOARD_API_VERSION_5)
        return 5;
    if (tc::commandService(h, &commandApi) != TC_SERVICE_OK ||
        commandApi.version != TC_COMMAND_API_VERSION_2)
        return 6;
    if (!tc::component_instances::table(h, &instances) ||
        !tc::component_storage::table(h, &storage))
        return 10;
    if (!tc::component_storage::tableV2(h, &storage2)) return 11;
    registered = true;

    load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
        h->context, "load_level__modelZutilities_u7740"));
    add_component = reinterpret_cast<AddComponent>(h->resolve_symbol(
        h->context, "add_component__presenterZutilitiesZhelper95functions_u5918"));
    select_component = reinterpret_cast<SelectComponent>(h->resolve_symbol(
        h->context, "select_component__modelZboardZboard_u9202"));
    try_rotate_component = reinterpret_cast<TryRotate>(h->resolve_symbol(
        h->context, "try_rotate_component__modelZboardZboard_u26772"));
    set_sim_test = reinterpret_cast<SetSimTest>(h->resolve_symbol(
        h->context, "set_sim_test__modelZutilities_u6840"));
    compile_request = reinterpret_cast<CompileRequest>(h->resolve_symbol(
        h->context, "preorder__modelZsimulationZcompile95thread_u3523"));
    tail_set = reinterpret_cast<TableSet>(h->resolve_symbol(
        h->context, "X5BX5Deq___modelZsave95mongerZversionsZv7_u70"));
    auto* update_target = h->resolve_symbol(
        h->context, "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
    if (!load_level || !set_sim_test || !compile_request ||
        !add_component || !select_component || !try_rotate_component || !tail_set ||
        !update_target || !invisible_target)
        return 7;
    if (h->create_hook(h->context, update_target, reinterpret_cast<void*>(hookedUpdate),
                       reinterpret_cast<void**>(&update_original)) != 0)
        return 8;
    if (h->create_hook(h->context, invisible_target, reinterpret_cast<void*>(hookedInvisible),
                       reinterpret_cast<void**>(&invisible_original)) != 0)
        return 9;
    plugin->on_frame = frame;
    return 0;
}
