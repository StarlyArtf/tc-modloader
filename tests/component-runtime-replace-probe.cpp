#include "../sdk/tc_component_runtime.h"
#include "../sdk/tc_game_model.h"

#include <fstream>
#include <string>

namespace {
const TCHost* host = nullptr;
tc::component_runtime::Api runtime{};
int frames = 0;
bool done = false;

constexpr uint64_t kId = 0x52554e54494d4532ULL;
const TCComponentPinV2 oneInput[] = {{"a", "A", 1, 0}};
const TCComponentPinV2 twoInputs[] = {{"a", "A", 1, 0}, {"b", "B", 1, 0}};
const TCComponentPinV2 output[] = {{"y", "Y", 1, 0}};
TCComponentTypeDefinitionV3 before{};
TCComponentTypeDefinitionV3 after{};

void logic(TCLogicIOV3* io) {
    if (!io || !io->outputs || !io->output_count) return;
    io->outputs[0].value = io->input_count > 1
                               ? ((io->inputs[0].value ^ io->inputs[1].value) & 1)
                               : (io->inputs[0].value & 1);
    io->outputs[0].drive_mask = 1;
}

void report(const std::string& text) {
    if (host && host->log) host->log(host->context, text.c_str());
    if (host && host->data_directory_utf8) {
        std::ofstream out(std::string(host->data_directory_utf8) + "/result.txt",
                          std::ios::trunc);
        out << text;
    }
}

void frame(void*, const TCFrame*) {
    if (done) return;
    ++frames;
    if (frames == 60) {
        const int status = tc::component_runtime::registerType(runtime, &before);
        if (status != TC_COMPONENT_TYPES_OK) {
            done = true;
            report("FAIL register status=" + std::to_string(status));
        }
    }
    if (frames == 120) {
        const int status = tc::component_runtime::replaceType(runtime, &before, &after);
        if (status != TC_COMPONENT_TYPES_OK) {
            done = true;
            report("FAIL replace status=" + std::to_string(status));
            return;
        }
        tc::TCGameModel game;
        tc::TCPrototype prototype{};
        if (!game.load(host) || !game.getCustomPrototype(kId, prototype) ||
            tc::prototypeInputCount(prototype) != 2 ||
            tc::prototypeOutputCount(prototype) != 1 ||
            tc::prototypeInputPinWordSize(prototype, 0) != 1 ||
            tc::prototypeInputPinWordSize(prototype, 1) != 1) {
            done = true;
            report("FAIL updated prototype shape was not 2in/1out");
            return;
        }
        done = true;
        report("PASS runtime replacement updated the game prototype to 2in/1out");
    }
}

void initialise(TCComponentTypeDefinitionV3& definition, const char* name,
                const TCComponentPinV2* inputs, uint32_t inputCount) {
    definition = {};
    definition.size = sizeof(definition);
    definition.version = TC_COMPONENT_TYPES_VERSION_3;
    definition.custom_id = kId;
    definition.type_id = "test.runtime-replace/type";
    definition.name = name;
    definition.inputs = inputs;
    definition.input_count = inputCount;
    definition.outputs = output;
    definition.output_count = 1;
    definition.gate_cost = 1;
    definition.delay = 1;
    definition.callback = &logic;
}
}  // namespace

extern "C" __declspec(dllexport) int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!tc::component_runtime::table(h, &runtime) ||
        runtime.version < TC_COMPONENT_RUNTIME_API_VERSION_2)
        return 2;
    initialise(before, "Runtime replace before", oneInput, 1);
    initialise(after, "Runtime replace after", twoInputs, 2);
    plugin->on_frame = &frame;
    return 0;
}
