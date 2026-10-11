/* Development probe for the internal-netlist builder (`tc.component.compose` V2,
   M4's second cut) - the "原版 AND 内部节点" case of the plan.

   The probe reuses the byte-adder example's level runner (the same trick
   tests/pin-shape-probe.cpp uses), replaces nothing in it, and adds one thing:
   it registers a **composite** type - a three-input AND built from the game's own
   two-input AND parts - and puts it in the path of a real campaign level.  So the
   run answers three questions at once:

     1. does the game accept a definition the builder encoded (the import, the
        prototype read-back, the pins);
     2. did the game *compile the netlist* - the gate count it computes from the
        inner circuit has to be 2 while the probe declared 1, and the declared
        delay (2) is the one it keeps;
     3. does a composite **behave**: the board fixture build/nl_and3board.data
        wires the level's three inputs and its output to the instance, and the
        level's own test (and_gate_3) only passes when the truth table is right.

   It also exercises the refusals - a netlist with an undriven pin - and the
   nesting path: a second composite whose inner node *is* the first composite.

   Read-only with respect to game state except for what a Mod is allowed to do:
   it adds its own types. */
#define tc_mod_load byte_adder_example_load
#include "../examples/byte-adder/plugin.cpp"
#undef tc_mod_load

#include "../sdk/tc_component_compose.h"
#include "../sdk/tc_component_registry.h"

#include <atomic>
#include <string>

namespace {

/* The three-input AND the and_gate_3 fixture board expects: build/nl_and3board.data
   places an instance of this id at (-5,0) and wires the level's inputs to
   (-7,-1), (-7,0), (-7,1) and the level's output to (-3,0) - which is exactly
   the pin lane this definition gets. */
constexpr uint64_t kAnd3Id = 0x414E44335F303031ULL;    /* "AND3_001" */
constexpr uint64_t kNestedId = 0x4E455354335F3031ULL;  /* "NEST3_01" */
constexpr uint64_t kBrokenId = 0x42524F4B335F3031ULL;  /* "BROK3_01" */
constexpr uint64_t kWideId = 0x57494445355F3031ULL;    /* "WIDE5_01" */

const TCHost* probeHost = nullptr;
tc::component_compose::ApiV2 compose{};
TCComponentRegistryApiV1 catalogue{};
bool registered = false;
bool nested = false;
bool refused = false;
bool cloned = false;
bool aliasRefused = false;
bool wide = false;
std::string refusedWhy;
int64_t levelVerdict = -1;
bool done = false;
double startedAt = 0.0;

void probeLog(const std::string& message) {
    if (probeHost) probeHost->log(probeHost->context, message.c_str());
}

std::string hex64(uint64_t value) {
    char text[24]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

/* Registers the composite through the builder.  Two AND nodes: the first folds
   A and B, the second folds that with C.  The declared gate cost is deliberately
   wrong (1) - the number the game computes back is the evidence that it really
   walked the nodes. */
bool registerAnd3() {
    TCComposeTypeV1 type{};
    type.size = sizeof(type);
    type.version = 1u;
    type.custom_id = kAnd3Id;
    type.type_id = "test.component-composite/and3";
    type.name = "Composite AND3";
    type.description = "Three-input AND built from two of the game's own AND parts.";
    type.gate_cost = 1;
    type.delay = 2;
    const TCComponentPinV2 inputs[3] = {{"A", "A", 1, 0}, {"B", "B", 1, 0}, {"C", "C", 1, 0}};
    const TCComponentPinV2 output{"Out", "Out", 1, 0};
    type.inputs = inputs;
    type.input_count = 3;
    type.outputs = &output;
    type.output_count = 1;
    uint64_t session = 0;
    probeLog("composite: begin");
    const int opened = tc::component_compose::begin(compose, type, &session);
    probeLog("composite: begin=" + std::to_string(opened) + " session=" + std::to_string(session));
    if (opened != TC_COMPONENT_COMPOSE_OK) {
        probeLog("composite: begin refused " + std::to_string(opened));
        return false;
    }
    uint32_t first = 0, second = 0;
    /* The AND part's pins: in (-1,-1) and (-1,1), out (2,0) - the catalogue's
       own numbers, so the layout below is derived rather than guessed. */
    probeLog("composite: node 1");
    const int placed = tc::component_compose::node(compose, session, "builtin/AND", 4, -1, 1, &first);
    probeLog("composite: node1=" + std::to_string(placed) + " index=" + std::to_string(first));
    const int placedSecond = tc::component_compose::node(compose, session, "builtin/AND", 20, 1, 1, &second);
    if (placed != TC_COMPONENT_COMPOSE_OK || placedSecond != TC_COMPONENT_COMPOSE_OK) {
        probeLog("composite: node refused " + std::to_string(placed) + "/" + std::to_string(placedSecond) +
            " " + tc::component_compose::status(compose, session));
        tc::component_compose::abort(compose, session);
        return false;
    }
    /* Output pin node index: inputs + inner nodes + the pin's own position. */
    const uint32_t outNode = tc::component_compose::outputPinNode(type, 2, 0);
    const int wires[] = {
        tc::component_compose::connect(compose, session, 0, 0, first, 0),   /* A -> AND1 in0 */
        tc::component_compose::connect(compose, session, 1, 0, first, 1),   /* B -> AND1 in1 */
        tc::component_compose::connect(compose, session, first, 0, second, 0), /* AND1 -> AND2 in0 */
        tc::component_compose::connect(compose, session, 2, 0, second, 1),  /* C -> AND2 in1 */
        tc::component_compose::connect(compose, session, second, 0, outNode, 0), /* AND2 -> Out */
    };
    for (int status : wires)
        if (status != TC_COMPONENT_COMPOSE_OK) {
            probeLog("composite: connect refused " + std::to_string(status) + " " +
                tc::component_compose::status(compose, session));
            tc::component_compose::abort(compose, session);
            return false;
        }
    probeLog("composite: commit");
    const int committed = tc::component_compose::commit(compose, session);
    if (committed != TC_COMPONENT_COMPOSE_OK) {
        probeLog("composite: commit refused " + std::to_string(committed) + " " +
            tc::component_compose::status(compose, session));
        return false;
    }
    return true;
}

/* The nesting path: a composite whose inner node is the composite above.  It
   forwards A/B/C to that part and its output back out, so its own behaviour is
   AND3 as well - and if the game refused to nest, the import would fail. */
bool registerNested() {
    TCComposeTypeV1 type{};
    type.size = sizeof(type);
    type.version = 1u;
    type.custom_id = kNestedId;
    type.type_id = "test.component-composite/nested";
    type.name = "Composite AND3 (nested)";
    type.description = "A composite that uses the composite above as its only part.";
    type.gate_cost = 1;
    type.delay = 2;
    const TCComponentPinV2 inputs[3] = {{"A", "A", 1, 0}, {"B", "B", 1, 0}, {"C", "C", 1, 0}};
    const TCComponentPinV2 output{"Out", "Out", 1, 0};
    type.inputs = inputs;
    type.input_count = 3;
    type.outputs = &output;
    type.output_count = 1;
    uint64_t session = 0;
    if (tc::component_compose::begin(compose, type, &session) != TC_COMPONENT_COMPOSE_OK) return false;
    uint32_t inner = 0;
    /* The spelling a nested part is named by is the one the catalogue reports -
       "<owner mod>/0x<custom id>" - so the probe asks the catalogue for it
       instead of assuming a prefix. */
    std::string partSpelling;
    if (tc::component_registry::ready(catalogue)) {
        TCComponentTypeInfoV1 info = tc::component_registry::typeInfo();
        if (tc::component_registry::find(catalogue, kAnd3Id, &info) == TC_COMPONENT_REGISTRY_OK)
            partSpelling = info.type_id;
    }
    if (partSpelling.empty()) {
        probeLog("composite: the nested part has no catalogue spelling yet");
        return false;
    }
    if (tc::component_compose::node(compose, session, partSpelling.c_str(), 4, 0, 1, &inner) !=
        TC_COMPONENT_COMPOSE_OK) {
        probeLog("composite: nesting refused " + std::string(tc::component_compose::status(compose, session)));
        tc::component_compose::abort(compose, session);
        return false;
    }
    const uint32_t outNode = tc::component_compose::outputPinNode(type, 1, 0);
    const int wires[] = {
        tc::component_compose::connect(compose, session, 0, 0, inner, 0),
        tc::component_compose::connect(compose, session, 1, 0, inner, 1),
        tc::component_compose::connect(compose, session, 2, 0, inner, 2),
        tc::component_compose::connect(compose, session, inner, 0, outNode, 0),
    };
    for (int status : wires)
        if (status != TC_COMPONENT_COMPOSE_OK) {
            probeLog("composite: nested connect refused " + std::to_string(status) + " " +
                tc::component_compose::status(compose, session));
            tc::component_compose::abort(compose, session);
            return false;
        }
    const int committed = tc::component_compose::commit(compose, session);
    if (committed != TC_COMPONENT_COMPOSE_OK) {
        probeLog("composite: nested commit refused " + std::to_string(committed) + " " +
            tc::component_compose::status(compose, session));
        return false;
    }
    return true;
}

/* A netlist the builder has to refuse: the second AND's second input is left
   unwired, which the game would silently read as 0. */
bool refuseBroken() {
    TCComposeTypeV1 type{};
    type.size = sizeof(type);
    type.version = 1u;
    type.custom_id = kBrokenId;
    type.name = "Composite (broken)";
    type.gate_cost = 1;
    type.delay = 1;
    const TCComponentPinV2 input{"A", "A", 1, 0};
    type.inputs = &input;
    type.input_count = 1;
    type.outputs = &input;
    type.output_count = 1;
    uint64_t session = 0;
    if (tc::component_compose::begin(compose, type, &session) != TC_COMPONENT_COMPOSE_OK) return false;
    uint32_t part = 0;
    if (tc::component_compose::node(compose, session, "builtin/AND", 4, 0, 1, &part) !=
        TC_COMPONENT_COMPOSE_OK) {
        tc::component_compose::abort(compose, session);
        return false;
    }
    /* Only in0 is driven; in1 and the output pin are left dangling. */
    tc::component_compose::connect(compose, session, 0, 0, part, 0);
    const int committed = tc::component_compose::commit(compose, session);
    refusedWhy = tc::component_compose::status(compose, session);
    tc::component_compose::abort(compose, session);
    return committed == TC_COMPONENT_COMPOSE_ERR_NETLIST && !refusedWhy.empty();
}

/* What the game made of the definition: the pins it reports and the gate count it
   recomputed from the nodes. */
void reportPrototype(const char* label, uint64_t customId) {
    tc::TCPrototype prototype{};
    if (!mod.game.getCustomPrototype(customId, prototype)) {
        probeLog(std::string("composite: ") + label + " prototype missing");
        return;
    }
    probeLog(std::string("composite: ") + label + " custom=" + hex64(customId) +
        " in=" + std::to_string(tc::prototypeInputCount(prototype)) +
        " out=" + std::to_string(tc::prototypeOutputCount(prototype)) +
        " gates=" + std::to_string(tc::prototypeGateCost(prototype)) +
        " delay=" + std::to_string(tc::prototypeDelay(prototype)) +
        " in0=(" + std::to_string(tc::prototypeInputPinPoint(prototype, 0).x) + "," +
        std::to_string(tc::prototypeInputPinPoint(prototype, 0).y) + ")" +
        " out0=(" + std::to_string(tc::prototypeOutputPinPoint(prototype, 0).x) + "," +
        std::to_string(tc::prototypeOutputPinPoint(prototype, 0).y) + ")");
    mod.components.releasePrototype(prototype);
}

void reportCatalogue(uint64_t customId) {
    if (!tc::component_registry::ready(catalogue)) return;
    TCComponentTypeInfoV1 info = tc::component_registry::typeInfo();
    if (tc::component_registry::find(catalogue, customId, &info) != TC_COMPONENT_REGISTRY_OK) {
        probeLog("composite: catalogue entry missing for " + hex64(customId));
        return;
    }
    probeLog("composite: catalogue custom=" + hex64(customId) +
        " implementation=" + std::to_string(info.implementation) +
        " capabilities=" + std::to_string(info.capabilities) +
        " active=" + std::to_string(info.active) +
        " in=" + std::to_string(info.input_count) + " out=" + std::to_string(info.output_count));
}

/* Cut three, path one: `clone_builtin_metadata`.  The probe copies the AND part's
   name and description onto its nested composite and reads the game's prototype
   back, so the assertion is about what the game stores, not about the call's
   return value. */
bool cloneMetadataAndReport() {
    if (!tc::component_registry::ready(catalogue)) return false;
    TCComponentTypeInfoV1 before = tc::component_registry::typeInfo();
    if (tc::component_registry::find(catalogue, kNestedId, &before) != TC_COMPONENT_REGISTRY_OK)
        return false;
    const int status = tc::component_compose::cloneMetadata(
        compose, kNestedId, "builtin/AND", TC_COMPOSE_CLONE_NAME | TC_COMPOSE_CLONE_DESCRIPTION);
    probeLog("composite: clone status=" + std::to_string(status));
    if (status != TC_COMPONENT_COMPOSE_OK) return false;
    tc::TCPrototype prototype{};
    if (!mod.game.getCustomPrototype(kNestedId, prototype)) return false;
    const char* name = tc::prototypeNameCStr(prototype);
    const char* description = tc::prototypeDescriptionCStr(prototype);
    const std::string storedName = name ? name : "";
    const std::string storedDescription = description ? description : "";
    mod.components.releasePrototype(prototype);
    TCComponentTypeInfoV1 after = tc::component_registry::typeInfo();
    const bool known =
        tc::component_registry::find(catalogue, kNestedId, &after) == TC_COMPONENT_REGISTRY_OK;
    probeLog("composite: clone prototype name=\"" + storedName + "\" description=\"" +
             storedDescription + "\"");
    probeLog("composite: clone catalogue name=\"" + std::string(known ? after.name : "") +
             "\" was=\"" + std::string(before.name) + "\"");
    return storedName == "AND" && !storedDescription.empty() && known &&
           std::string(after.name) == "AND";
}

/* Cut three, path three: the full alias.  The probe asks for the one thing plan
   section 8.3 asks to be refused explicitly rather than faked, and records the
   answer. */
bool aliasIsRefused() {
    const int status = tc::component_compose::alias(compose, kNestedId, "builtin/AND");
    probeLog("composite: alias status=" + std::to_string(status));
    return status == TC_COMPONENT_COMPOSE_ERR_UNSUPPORTED;
}

/* A wide type: two inputs, thirty-two outputs.  The plan's example decoder needs
   thirty-two output pins, and the builder's own cap is the loader's (thirty-two,
   the same ceiling the game's importer was measured to take); this registers one
   so the real machine answers "does the game take thirty-two pin nodes".

   The inner circuit is deliberately trivial - one "ON" source per output, wired
   straight across - because what is being measured here is the *pin count*, not
   the logic. */
bool registerWide() {
    TCComposeTypeV1 type{};
    type.size = sizeof(type);
    type.version = 1u;
    type.custom_id = kWideId;
    type.name = "Composite wide (2 in, 32 out)";
    type.gate_cost = 1;
    type.delay = 1;
    const TCComponentPinV2 inputs[2] = {{"A", "A", 1, 0}, {"B", "B", 1, 0}};
    std::vector<TCComponentPinV2> outputs;
    for (int line = 0; line < 32; ++line) {
        const std::string name = "D" + std::to_string(line);
        outputs.push_back(TCComponentPinV2{"", nullptr, 1, 0});
        /* The struct borrows the strings only for the duration of the call, so a
           stable buffer is enough: keep them in a vector that outlives begin(). */
        (void)name;
    }
    static std::vector<std::string> names;
    names.clear();
    for (int line = 0; line < 32; ++line) names.push_back("D" + std::to_string(line));
    for (size_t line = 0; line < outputs.size(); ++line) outputs[line].name = names[line].c_str();
    type.inputs = inputs;
    type.input_count = 2;
    type.outputs = outputs.data();
    type.output_count = 32;
    uint64_t session = 0;
    if (tc::component_compose::begin(compose, type, &session) != TC_COMPONENT_COMPOSE_OK) {
        probeLog("composite: wide begin refused");
        return false;
    }
    /* The wide pin lane: the input column moves left and the output column
       right, which is the room a decoder inner circuit is laid out in. */
    const int lane = tc::component_compose::pinLane(compose, session, 16);
    probeLog("composite: wide lane=" + std::to_string(lane));
    if (lane != TC_COMPONENT_COMPOSE_OK) {
        tc::component_compose::abort(compose, session);
        return false;
    }
    const uint32_t firstOutput = type.input_count + 32u; /* after the ON nodes */
    for (uint32_t line = 0; line < 32; ++line) {
        uint32_t source = 0;
        /* The pin lanes the loader gives the type's own pins: whole cells apart,
           centred - so each ON source can be placed on its output's own row and
           the wire runs straight across. */
        const int32_t y = (static_cast<int32_t>(line) - 15) * 8;
        if (tc::component_compose::node(compose, session, "builtin/ON", -80, y, 1, &source) !=
            TC_COMPONENT_COMPOSE_OK) {
            probeLog("composite: wide node refused " +
                     std::string(tc::component_compose::status(compose, session)));
            tc::component_compose::abort(compose, session);
            return false;
        }
        if (tc::component_compose::connect(compose, session, source, 0, firstOutput + line, 0) !=
            TC_COMPONENT_COMPOSE_OK) {
            probeLog("composite: wide connect refused " +
                     std::string(tc::component_compose::status(compose, session)));
            tc::component_compose::abort(compose, session);
            return false;
        }
    }
    const int committed = tc::component_compose::commit(compose, session);
    probeLog("composite: wide commit=" + std::to_string(committed) + " " +
             tc::component_compose::status(compose, session));
    return committed == TC_COMPONENT_COMPOSE_OK;
}

void probeFrame(void*, const TCFrame* value) {
    if (!startedAt) startedAt = value->time_seconds;
    if (done) return;
    /* Let the example's own level runner do its work first. */
    frame(nullptr, value);
    if (!autotestDone) return;
    if (levelVerdict < 0) {
        auto* testState = reinterpret_cast<int64_t (*)()>(probeHost->resolve_symbol(
            probeHost->context, "sim_get_test_state__modelZsimulationZcontroller_u26"));
        levelVerdict = testState ? testState() : 0;
        probeLog("composite: level verdict=" + std::to_string(levelVerdict));
    }
    done = true;
    const bool ok =
        registered && nested && refused && cloned && aliasRefused && wide && levelVerdict == 1;
    const std::string text =
        ok ? "PASS composite: the netlist builder registered an AND3 made of the game's own AND parts "
             "(the game recomputed 2 gates from the nodes), refused a netlist with an undriven pin, "
             "nested one composite inside another, cloned a built-in part's metadata onto a type, "
             "reported the full alias as unsupported, and the and_gate_3 level passed with it on the board"
           : "FAIL composite: registered=" + std::to_string(registered) +
                 " nested=" + std::to_string(nested) + " refused=" + std::to_string(refused) +
                 " cloned=" + std::to_string(cloned) +
                 " aliasRefused=" + std::to_string(aliasRefused) +
                 " wide=" + std::to_string(wide) +
                 " verdict=" + std::to_string(levelVerdict);
    std::ofstream(std::string(probeHost->data_directory_utf8) + "/result.txt") << text;
    probeLog(text);
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    probeHost = h;
    /* The example's own registration, host binding and frame hook first. */
    const int inherited = byte_adder_example_load(h, plugin);
    if (inherited) return inherited;
    if (!tc::component_compose::tableV2(h, &compose)) {
        probeLog("composite: this loader has no tc.component.compose V2");
        return 2;
    }
    if (tc::component_registry::table(h, &catalogue))
        probeLog("composite: catalogue available");
    registered = registerAnd3();
    if (registered) {
        reportPrototype("and3", kAnd3Id);
        reportCatalogue(kAnd3Id);
    }
    nested = registerNested();
    if (nested) {
        reportPrototype("nested", kNestedId);
        reportCatalogue(kNestedId);
    }
    refused = refuseBroken();
    probeLog("composite: refusals broken=" + std::to_string(refused) + " status=" + refusedWhy);
    cloned = cloneMetadataAndReport();
    aliasRefused = aliasIsRefused();
    wide = registerWide();
    if (wide) reportPrototype("wide", kWideId);
    plugin->on_frame = probeFrame;
    return 0;
}
