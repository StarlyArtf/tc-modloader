/* Runtime type lifecycle probe: register, unregister, register the same id
   again, then replace an id that is still live.  The editor's file manager
   produces exactly this order (a new file gets a stable id derived from its
   name, so delete plus create again is the same custom_id).

   Every step logs its status and the game model's answer, so a run says which
   check rejected the call instead of only that something failed. */

#include "../sdk/tc_component_runtime.h"
#include "../sdk/tc_game_model.h"

#include <fstream>
#include <string>

namespace {
const TCHost* host = nullptr;
tc::component_runtime::Api runtime{};
tc::TCGameModel game;
int frames = 0;

constexpr uint64_t kCycle = 0x4c49464543594331ULL;
constexpr uint64_t kReplace = 0x4c49464543594332ULL;

const TCComponentPinV2 oneInput[] = {{"a", "A", 1, 0}};
const TCComponentPinV2 twoInputs[] = {{"a", "A", 1, 0}, {"b", "B", 1, 0}};
const TCComponentPinV2 output[] = {{"y", "Y", 1, 0}};

TCComponentTypeDefinitionV3 cycleType{};
TCComponentTypeDefinitionV3 replaceBefore{};
TCComponentTypeDefinitionV3 replaceAfter{};

std::string log;

void note(const std::string& text) {
    log += text;
    log += "\n";
    if (host && host->log) host->log(host->context, ("lifecycle: " + text).c_str());
}

void write() {
    if (!host || !host->data_directory_utf8) return;
    std::ofstream out(std::string(host->data_directory_utf8) + "/result.txt");
    out << log;
}

void logic(TCLogicIOV3* io) {
    if (!io || !io->outputs || !io->output_count) return;
    io->outputs[0].value = io->input_count > 1
                               ? ((io->inputs[0].value ^ io->inputs[1].value) & 1)
                               : (io->inputs[0].value & 1);
    io->outputs[0].drive_mask = 1;
}

void initialise(TCComponentTypeDefinitionV3& definition, uint64_t id, const char* name,
                const TCComponentPinV2* inputs, uint32_t inputCount) {
    definition = {};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_3;
    definition.custom_id = id;
    definition.type_id = "test.runtime-lifecycle/type";
    definition.name = name;
    definition.inputs = inputs;
    definition.input_count = inputCount;
    definition.outputs = output;
    definition.output_count = 1;
    definition.gate_cost = 1;
    definition.delay = 1;
    definition.callback = &logic;
}

std::string present(uint64_t id) {
    return game.hasCustomPrototype(id) ? "imported" : "absent";
}

/* The component column enumerates the live ordered list (cc_length +
   cc_live_values), not the prototype map, so this is what the player actually
   sees. */
bool inLiveList(uint64_t id) {
    const uint64_t count = game.customPrototypeCount();
    for (uint64_t index = 0; index < count && index < 512; ++index)
        if (game.customPrototypeIdAt(index) == id) return true;
    return false;
}

std::string state(uint64_t id) {
    return present(id) + "/" + (inLiveList(id) ? "in-column" : "not-in-column");
}

void frame(void*, const TCFrame*) {
    ++frames;
    if (frames == 90) {
        const int status = tc::component_runtime::registerType(runtime, &cycleType);
        note("1 register       status=" + std::to_string(status) + " " + state(kCycle));
    } else if (frames == 180) {
        const int status = tc::component_runtime::unregisterType(runtime, kCycle);
        note("2 unregister     status=" + std::to_string(status) + " " + state(kCycle));
    } else if (frames == 270) {
        const int status = tc::component_runtime::registerType(runtime, &cycleType);
        note("3 register again status=" + std::to_string(status) + " " + state(kCycle));
    } else if (frames == 360) {
        const int status = tc::component_runtime::registerType(runtime, &replaceBefore);
        note("4 register second status=" + std::to_string(status) + " " + state(kReplace));
    } else if (frames == 450) {
        const int status = tc::component_runtime::replaceType(runtime, &replaceBefore, &replaceAfter);
        note("5 replace        status=" + std::to_string(status) + " " + state(kReplace));
        write();
    }
}
}  // namespace

extern "C" __declspec(dllexport) int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!tc::component_runtime::table(h, &runtime) ||
        runtime.version < TC_COMPONENT_RUNTIME_API_VERSION_2)
        return 2;
    game.load(h);
    initialise(cycleType, kCycle, "Lifecycle one", oneInput, 1);
    initialise(replaceBefore, kReplace, "Lifecycle before", oneInput, 1);
    initialise(replaceAfter, kReplace, "Lifecycle after", twoInputs, 2);
    plugin->on_frame = &frame;
    return 0;
}
