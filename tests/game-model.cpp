#include "../sdk/tc_game_model.h"
#include "../sdk/tc_board_model.h"
#include "../sdk/tc_game_state.h"
#include "../sdk/tc_simulation.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr uint8_t kBuiltInKind = 7;
constexpr uint16_t kCustomId = 123;

uint64_t gAutoSize = 0x1020304050607080ULL;
uint64_t gCustomCount = 0;
std::map<uint64_t, tc::TCPrototype> gCustomPrototypes;
std::vector<uint64_t> gCustomIds;
alignas(16) unsigned char gCustomLiveValues[16 * 16]{};
alignas(16) unsigned char gPrototypeTable[16]{};
alignas(16) unsigned char gPrototypeBuckets[4 * tc::kPrototypeBucketStride]{};
alignas(8) unsigned char gSelectedComponents[24]{};
alignas(8) unsigned char gSelectedWires[24]{};
alignas(8) unsigned char gPrevSelectedComponents[24]{};
alignas(8) unsigned char gPrevSelectedWires[24]{};
uint8_t gIsCampaign = 1;
alignas(8) unsigned char gLevelProgress[16]{};
alignas(8) unsigned char gCampaignName[16]{};
alignas(8) unsigned char gSimulationCircuitState[16]{};
void* gSimulationSettingsValue = reinterpret_cast<void*>(1);
void* gSimulationSettingsPointer = &gSimulationSettingsValue;
int64_t gSimCycle = 100;
int64_t gSimSetting = 7;
int64_t gSimObservedTarget = -1;
uint8_t gSimObservedCommand = 255;
std::set<uint64_t> gSelectedComponentIds;
std::set<uint64_t> gSelectedWireIds;
std::set<uint64_t> gPrevSelectedComponentIds;
std::set<uint64_t> gPrevSelectedWireIds;

void setupPrototypeTable() {
    uint64_t length = 4;
    std::memcpy(gPrototypeTable, &length, sizeof(length));
    void* buckets = gPrototypeBuckets;
    std::memcpy(gPrototypeTable + 8, &buckets, sizeof(buckets));

    const uint8_t keys[] = {0x31, 0x52, 0x5f};
    for (int i = 0; i < 3; ++i) {
        uint64_t hash = i + 1;
        unsigned char* bucket = gPrototypeBuckets + i * tc::kPrototypeBucketStride;
        std::memcpy(bucket + tc::kPrototypeBucketHashOffset, &hash, sizeof(hash));
        std::memcpy(bucket + tc::kPrototypeBucketKeyOffset, &keys[i], 1);
    }
}

void rebuildLiveValues() {
    std::memset(gCustomLiveValues, 0, sizeof(gCustomLiveValues));
    for (std::size_t i = 0; i < gCustomIds.size(); ++i) {
        std::memcpy(gCustomLiveValues + i * 16, &gCustomIds[i], 8);
        uint16_t low = static_cast<uint16_t>(gCustomIds[i]);
        std::memcpy(gCustomLiveValues + i * 16 + 8, &low, 2);
    }
    gCustomCount = gCustomIds.size();
}

void writeU64(unsigned char* dst, uint64_t value) {
    std::memcpy(dst, &value, sizeof(value));
}

void writePtr(unsigned char* dst, void* value) {
    std::memcpy(dst, &value, sizeof(value));
}

void fakeGetPrototype(const void* key, void* out) {
    const auto* kind = static_cast<const tc::TCPrototypeKind*>(key);
    assert(kind->tag != tc::kPrototypeKindCustom);
    assert(kind->custom_id == 0);

    static tc::TCPin inputs[2]{};
    static tc::TCPin outputs[1]{};
    writeU64(inputs[0].bytes + 0x10, 0x11);
    writeU64(inputs[1].bytes + 0x10, 0x22);
    writeU64(outputs[0].bytes + 0x10, 0x33);

    auto* prototype = static_cast<tc::TCPrototype*>(out);
    writeU64(prototype->bytes + 0x60, 2);
    writePtr(prototype->bytes + 0x68, inputs);
    writeU64(prototype->bytes + 0x80, 1);
    writePtr(prototype->bytes + 0x88, outputs);
}

void fakeGetCustomPrototype(uint64_t custom_id, void* out) {
    assert(custom_id == kCustomId);
    static tc::TCPin inputs[3]{};
    writeU64(inputs[0].bytes + 0x10, 0x44);
    writeU64(inputs[1].bytes + 0x10, 0x55);
    writeU64(inputs[2].bytes + 0x10, 0x66);

    auto* prototype = static_cast<tc::TCPrototype*>(out);
    writeU64(prototype->bytes + 0x60, 3);
    writePtr(prototype->bytes + 0x68, inputs);
    writeU64(prototype->bytes + 0x80, 0);
    writePtr(prototype->bytes + 0x88, nullptr);
}

uint64_t fakeInputWordSize(uint8_t kind, uint16_t pin_index,
                           const void* expected) {
    assert(kind == kBuiltInKind);
    assert(expected == &gAutoSize);
    return static_cast<uint64_t>(kind) * 100 + pin_index;
}

uint64_t fakeOutputWordSize(uint8_t kind, uint16_t pin_index,
                            const void* expected) {
    assert(kind == kBuiltInKind);
    assert(expected == &gAutoSize);
    return static_cast<uint64_t>(kind) * 1000 + pin_index;
}

void fakeRawNewString(void* out, int64_t length) {
    auto* value = static_cast<tc::TCNimString*>(out);
    value->length = length;
    value->data = std::malloc(static_cast<size_t>(length) + 16);
    std::memset(value->data, 0, static_cast<size_t>(length) + 16);
}

void fakeSetCustomPrototype(uint64_t id, const void* prototype) {
    const auto* proto = static_cast<const tc::TCPrototype*>(prototype);
    gCustomPrototypes[id] = *proto;
    if (std::find(gCustomIds.begin(), gCustomIds.end(), id) ==
        gCustomIds.end()) {
        gCustomIds.push_back(id);
    }
    rebuildLiveValues();
}

void fakeDelCustomPrototype(uint64_t id) {
    gCustomPrototypes.erase(id);
    gCustomIds.erase(std::remove(gCustomIds.begin(), gCustomIds.end(), id),
                     gCustomIds.end());
    rebuildLiveValues();
}

uint8_t fakeContainsCustomPrototype(uint64_t id) {
    return gCustomPrototypes.count(id) ? 1 : 0;
}

uint8_t fakeNotContainsCustomPrototype(uint64_t id) {
    return gCustomPrototypes.count(id) ? 0 : 1;
}

uint8_t fakeBoardContains(const void* set, uint64_t id) {
    if (set == gSelectedComponents) return gSelectedComponentIds.count(id) ? 1 : 0;
    if (set == gSelectedWires) return gSelectedWireIds.count(id) ? 1 : 0;
    if (set == gPrevSelectedComponents) return gPrevSelectedComponentIds.count(id) ? 1 : 0;
    if (set == gPrevSelectedWires) return gPrevSelectedWireIds.count(id) ? 1 : 0;
    return 0;
}

void fakeSimSubmit(void*, uint8_t command, int64_t target) {
    gSimObservedCommand = command;
    gSimObservedTarget = target;
}

int64_t fakeSimGetCycle() {
    return gSimCycle;
}

int64_t fakeSimGetSetting(uint8_t) {
    return gSimSetting;
}

void fakeSimSetSetting(uint8_t, int64_t value) {
    gSimSetting = value;
}

void* fakeResolve(void*, const char* name) {
    std::string symbol(name ? name : "");
    if (symbol ==
        "get_prototype__modelZboardZcustom95prototype95list_u502") {
        return reinterpret_cast<void*>(&fakeGetPrototype);
    }
    if (symbol ==
        "get_custom_prototype__modelZboardZcustom95prototype95list_u451") {
        return reinterpret_cast<void*>(&fakeGetCustomPrototype);
    }
    if (symbol ==
        "get_input_word_size__modelZboardZprototype95list_u4196") {
        return reinterpret_cast<void*>(&fakeInputWordSize);
    }
    if (symbol ==
        "get_output_word_size__modelZboardZprototype95list_u4353") {
        return reinterpret_cast<void*>(&fakeOutputWordSize);
    }
    if (symbol == "rawNewString") {
        return reinterpret_cast<void*>(&fakeRawNewString);
    }
    if (symbol == "AUTO_SIZE__modelZmodel95types_u54") {
        return &gAutoSize;
    }
    if (symbol == "PROTOTYPES__modelZboardZprototype95list_u3772") {
        return gPrototypeTable;
    }
    if (symbol == "CATEGORY_ORDER__modelZboardZprototype95list_u21") {
        return reinterpret_cast<void*>(0x3000);
    }
    if (symbol ==
        "custom_prototypes_set__modelZboardZcustom95prototype95list_u192") {
        return reinterpret_cast<void*>(&fakeSetCustomPrototype);
    }
    if (symbol ==
        "custom_prototypes_del__modelZboardZcustom95prototype95list_u291") {
        return reinterpret_cast<void*>(&fakeDelCustomPrototype);
    }
    if (symbol ==
        "in_custom_prototypes__modelZboardZcustom95prototype95list_u9") {
        return reinterpret_cast<void*>(&fakeContainsCustomPrototype);
    }
    if (symbol ==
        "notin_custom_prototypes__modelZboardZcustom95prototype95list_u189") {
        return reinterpret_cast<void*>(&fakeNotContainsCustomPrototype);
    }
    if (symbol == "cc_length__modelZboardZcustom95prototype95list_u8") {
        return &gCustomCount;
    }
    if (symbol ==
        "cc_live_values__modelZboardZcustom95prototype95list_u7") {
        return gCustomLiveValues;
    }
    if (symbol == "selected_components__modelZboardZboard_u22") {
        return gSelectedComponents;
    }
    if (symbol == "selected_wires__modelZboardZboard_u30") {
        return gSelectedWires;
    }
    if (symbol == "prev_selected_components__modelZboardZboard_u41") {
        return gPrevSelectedComponents;
    }
    if (symbol == "prev_selected_wires__modelZboardZboard_u44") {
        return gPrevSelectedWires;
    }
    if (symbol == "contains__modelZboardZboard_u1842") {
        return reinterpret_cast<void*>(&fakeBoardContains);
    }
    if (symbol == "is_campaign__modelZmodel95types_u739") {
        return &gIsCampaign;
    }
    if (symbol == "level_progress__modelZmodel95types_u835") {
        return gLevelProgress;
    }
    if (symbol == "campaign_name__modelZmodel95types_u836") {
        return gCampaignName;
    }
    if (symbol == "simulation_circuit_state__modelZsimulator95types_u78") {
        return gSimulationCircuitState;
    }
    if (symbol == "sim_do__modelZsimulationZcompile95thread_u3036") {
        return reinterpret_cast<void*>(&fakeSimSubmit);
    }
    if (symbol == "sim_get_cycle__modelZsimulationZcompile95thread_u3041") {
        return reinterpret_cast<void*>(&fakeSimGetCycle);
    }
    if (symbol == "simulation_settings__modelZsimulator95types_u83") {
        return &gSimulationSettingsPointer;
    }
    if (symbol == "get_command_setting__modelZsimulator95types_u124") {
        return reinterpret_cast<void*>(&fakeSimGetSetting);
    }
    if (symbol == "set_command_setting__modelZsimulator95types_u131") {
        return reinterpret_cast<void*>(&fakeSimSetSetting);
    }
    return nullptr;
}

}  // namespace

int main() {
    static_assert(offsetof(tc::TCPrototypeKind, custom_id) == 0x188,
                  "custom_id offset changed");
    static_assert(sizeof(tc::TCPrototype) == 0x5a8,
                  "Prototype size changed");
    static_assert(sizeof(tc::TCPin) == 0x38, "Pin size changed");
    setupPrototypeTable();

    TCHost host{};
    host.api_version = TC_MOD_API_VERSION;
    host.size = sizeof(host);
    host.context = nullptr;
    host.resolve_symbol = fakeResolve;

    tc::TCGameModel model;
    if (!model.load(&host)) {
        std::cerr << "model.load failed\n";
        return 1;
    }
    if (!model.valid() || !model.wordSizeValid()) {
        std::cerr << "model did not resolve required symbols\n";
        return 1;
    }
    if (!model.stringAllocValid()) {
        std::cerr << "model did not resolve rawNewString\n";
        return 1;
    }

    tc::TCBoardModel board;
    if (!board.load(&host) || !board.valid()) {
        std::cerr << "board model did not resolve\n";
        return 1;
    }

    tc::TCGameStateModel state;
    if (!state.load(&host) || !state.valid() || !state.isCampaign() ||
        state.levelProgress() != gLevelProgress ||
        state.campaignNamePtr() != gCampaignName ||
        state.simulationCircuitState() != gSimulationCircuitState) {
        std::cerr << "game state model mismatch\n";
        return 1;
    }

    tc::TCSimulationModel sim;
    if (!sim.load(&host) || !sim.valid() || !sim.settingsReady() ||
        sim.cycle() != 100 || sim.commandSetting(2) != 7) {
        std::cerr << "simulation model mismatch\n";
        return 1;
    }
    sim.run(nullptr, 250);
    if (gSimObservedCommand != 0 || gSimObservedTarget != 250) {
        std::cerr << "simulation run command mismatch\n";
        return 1;
    }
    sim.setCommandSetting(2, 99);
    if (gSimSetting != 99) {
        std::cerr << "simulation setting update mismatch\n";
        return 1;
    }
    gSelectedComponentIds.insert(42);
    gSelectedWireIds.insert(7);
    gPrevSelectedComponentIds.insert(43);
    gPrevSelectedWireIds.insert(8);
    if (!board.isComponentSelected(42) || board.isComponentSelected(1) ||
        !board.isWireSelected(7) || board.isWireSelected(42) ||
        !board.isComponentPreviouslySelected(43) ||
        board.isComponentPreviouslySelected(42) ||
        !board.isWirePreviouslySelected(8) ||
        board.isWirePreviouslySelected(7)) {
        std::cerr << "board selection query mismatch\n";
        return 1;
    }
    if (!model.mutationValid()) {
        std::cerr << "model did not resolve mutation symbols\n";
        return 1;
    }

    if (model.builtinPrototypeCount() != 3 ||
        model.builtinPrototypeKindAt(0) != 0x31 ||
        model.builtinPrototypeKindAt(2) != 0x5f ||
        !model.isBuiltinPrototypeKind(0x52) ||
        model.isBuiltinPrototypeKind(0x99) ||
        model.builtinPrototypeIndexForKind(0x5f) != 2) {
        std::cerr << "built-in prototype enumeration mismatch\n";
        return 1;
    }

    tc::TCPrototype cloned{};
    if (!model.cloneBuiltinPrototype(0x52, cloned) ||
        tc::prototypeInputCount(cloned) != 2 ||
        tc::prototypeOutputCount(cloned) != 1) {
        std::cerr << "built-in prototype clone mismatch\n";
        return 1;
    }
    if (model.cloneBuiltinPrototype(0x99, cloned)) {
        std::cerr << "unknown built-in prototype was accepted\n";
        return 1;
    }

    tc::TCPrototype registered{};
    if (!model.registerBuiltinAsCustom(0x52, 777, registered) ||
        !model.hasCustomPrototype(777) ||
        model.customPrototypeCount() != 1) {
        std::cerr << "built-in to custom registration mismatch\n";
        return 1;
    }
    if (!model.removeCustomPrototype(777) ||
        model.hasCustomPrototype(777) ||
        model.customPrototypeCount() != 0) {
        std::cerr << "template registration cleanup mismatch\n";
        return 1;
    }

    tc::TCPrototypeBuilder builder(model, 0x52);
    if (!builder.ready() ||
        tc::prototypeInputCount(builder.prototype()) != 2) {
        std::cerr << "prototype builder template mismatch\n";
        return 1;
    }
    if (!builder.setName("My Gate") ||
        !builder.setDescription("Custom gate") ||
        !builder.setShapeSvg("<svg viewBox=\"0 0 1 1\"/>")) {
        std::cerr << "prototype builder string assignment failed\n";
        return 1;
    }
    if (std::strcmp(tc::prototypeNameCStr(builder.prototype()), "My Gate") != 0 ||
        std::strcmp(tc::prototypeDescriptionCStr(builder.prototype()),
                    "Custom gate") != 0 ||
        std::strcmp(tc::prototypeShapeSvgCStr(builder.prototype()),
                    "<svg viewBox=\"0 0 1 1\"/>") != 0) {
        std::cerr << "prototype string field mismatch\n";
        return 1;
    }
    tc::TCPin builderPins[2]{};
    builder.setInputCount(2);
    builder.setInputPins(builderPins);
    builder.setOutputCount(1);
    tc::TCPin builderOutput[1]{};
    builder.setOutputPins(builderOutput);
    builder.setCategoryRaw(0x12345678ULL);
    builder.setFlagsRaw(0x9abcdef0ULL);
    if (tc::prototypeCategoryRaw(builder.prototype()) != 0x12345678ULL ||
        tc::prototypeFlagsRaw(builder.prototype()) != 0x9abcdef0ULL) {
        std::cerr << "prototype builder raw field mismatch\n";
        return 1;
    }
    if (!builder.registerAsCustom(888) ||
        !model.hasCustomPrototype(888) ||
        model.customPrototypeCount() != 1) {
        std::cerr << "prototype builder registration mismatch\n";
        return 1;
    }
    if (!model.removeCustomPrototype(888) ||
        model.customPrototypeCount() != 0) {
        std::cerr << "prototype builder cleanup mismatch\n";
        return 1;
    }

    tc::TCPrototype builtin{};
    if (!model.getPrototype(kBuiltInKind, 0, builtin)) {
        std::cerr << "getPrototype builtin failed\n";
        return 1;
    }
    if (tc::prototypeInputCount(builtin) != 2 ||
        tc::prototypeOutputCount(builtin) != 1) {
        std::cerr << "prototype counts mismatch\n";
        return 1;
    }
    if (tc::pinWordSizeRaw(*tc::prototypeInputPin(builtin, 1)) != 0x22 ||
        tc::pinWordSizeRaw(*tc::prototypeOutputPin(builtin, 0)) != 0x33) {
        std::cerr << "pin raw word size mismatch\n";
        return 1;
    }
    if (model.inputWordSize(kBuiltInKind, 4) != 704 ||
        model.outputWordSize(kBuiltInKind, 4) != 7004) {
        std::cerr << "word size wrapper mismatch\n";
        return 1;
    }

    tc::TCPrototype custom{};
    if (!model.getPrototype(tc::kPrototypeKindCustom, kCustomId, custom)) {
        std::cerr << "getPrototype custom failed\n";
        return 1;
    }
    if (tc::prototypeInputCount(custom) != 3 ||
        tc::prototypeOutputCount(custom) != 0 ||
        tc::prototypeOutputPin(custom, 0) != nullptr) {
        std::cerr << "custom prototype fields mismatch\n";
        return 1;
    }

    tc::TCPrototype toRegister{};
    tc::TCPin registerPins[2]{};
    tc::prototypeSetInputCount(toRegister, 2);
    tc::prototypeSetInputPins(toRegister, registerPins);
    tc::prototypeSetOutputCount(toRegister, 0);
    tc::prototypeSetOutputPins(toRegister, nullptr);
    if (!model.setCustomPrototype(kCustomId, toRegister) ||
        !model.hasCustomPrototype(kCustomId) ||
        model.customPrototypeCount() != 1 ||
        model.customPrototypeIdAt(0) != kCustomId) {
        std::cerr << "custom prototype set/has/count mismatch\n";
        return 1;
    }
    if (!model.removeCustomPrototype(kCustomId) ||
        model.hasCustomPrototype(kCustomId) ||
        model.customPrototypeCount() != 0) {
        std::cerr << "custom prototype remove mismatch\n";
        return 1;
    }

    std::cout << "PASS game object model header: offsets, resolution, "
                 "prototype, pin and custom registration access\n";
    return 0;
}
