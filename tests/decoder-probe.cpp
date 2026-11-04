/* A Mod that registers the wide example as a *component*: the five-input,
   thirty-two-output one-hot decoder of examples/composite-decoder/decoder.hpp,
   handed to the loader through `tc.component.compose` V2 (begin / node /
   connect / commit) instead of the offline encoder.

   The circuit is the example's own: this file only translates the
   component_netlist::Definition the generator builds into service calls, so the
   offline case (tests/component-decoder.cpp) and what the game compiles are the
   same netlist with the same layout.  Place the type in a sandbox and open it:
   the game draws the thirty-two-line tree inside, and the two crossing wires the
   layout needs are the ones tests/component-netlist.cpp pins the routing rule
   with (a crossing is not a connection).

   Read-only with respect to game state: it adds its own type. */
#include "../examples/composite-decoder/decoder.hpp"
#include "../sdk/tc_component_compose.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

const TCHost* probeHost = nullptr;
tc::component_compose::ApiV2 compose{};
bool registered = false;
std::string refusal;

void probeLog(const std::string& message) {
    if (probeHost) probeHost->log(probeHost->context, message.c_str());
}

std::string hex64(uint64_t value) {
    char text[24]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

/* The generator -> service adapter.  The definition is validated first, with the
   same call the offline case uses, so a layout the builder would refuse is
   reported here instead of turning into a half-registered type. */
bool registerDecoder() {
    composite_decoder::Definition definition;
    if (!composite_decoder::build(definition, 0x44454335335F3031ull, "Decoder 5 to 32")) {
        refusal = "the example's generator refused to build";
        return false;
    }
    std::string why;
    const tc::component_netlist::Error verdict = tc::component_netlist::validate(definition, why);
    if (verdict != tc::component_netlist::Error::None) {
        refusal = std::string("the layout does not route: ") + why;
        return false;
    }

    /* The pin names have to outlive begin(), which borrows them. */
    std::vector<std::string> inputNames, outputNames;
    std::vector<TCComponentPinV2> inputs, outputs;
    for (const auto& pin : definition.inputs) {
        inputNames.push_back(pin.name);
        inputs.push_back(TCComponentPinV2{inputNames.back().c_str(), nullptr, pin.bits, 0});
    }
    for (const auto& pin : definition.outputs) {
        outputNames.push_back(pin.name);
        outputs.push_back(TCComponentPinV2{outputNames.back().c_str(), nullptr, pin.bits, 0});
    }

    TCComposeTypeV1 type{};
    type.size = sizeof(type);
    type.version = 1u;
    type.custom_id = definition.customId;
    type.type_id = "test.decoder-probe/dec5to32";
    type.name = "Decoder 5 to 32";
    type.description = "Five bits in, one-hot on thirty-two outputs, built from the game's own parts.";
    type.gate_cost = definition.gateCost;
    type.delay = definition.delay;
    type.inputs = inputs.data();
    type.input_count = static_cast<uint32_t>(inputs.size());
    type.outputs = outputs.data();
    type.output_count = static_cast<uint32_t>(outputs.size());

    uint64_t session = 0;
    const int opened = tc::component_compose::begin(compose, type, &session);
    if (opened != TC_COMPONENT_COMPOSE_OK) {
        refusal = "begin refused with " + std::to_string(opened);
        return false;
    }
    if (tc::component_compose::pinLane(compose, session, definition.pinLane) !=
        TC_COMPONENT_COMPOSE_OK) {
        refusal = "pin_lane refused";
        tc::component_compose::abort(compose, session);
        return false;
    }
    for (const auto& node : definition.nodes) {
        uint32_t index = 0;
        if (tc::component_compose::node(compose, session, node.typeId.c_str(), node.x, node.y,
                                        node.bits, &index) != TC_COMPONENT_COMPOSE_OK) {
            refusal = "node " + node.typeId + " refused: " +
                      tc::component_compose::status(compose, session);
            tc::component_compose::abort(compose, session);
            return false;
        }
    }
    for (const auto& wire : definition.connections) {
        if (tc::component_compose::connect(compose, session, wire.fromNode, wire.fromPin, wire.toNode,
                                           wire.toPin) != TC_COMPONENT_COMPOSE_OK) {
            refusal = "connect refused: " + std::string(tc::component_compose::status(compose, session));
            tc::component_compose::abort(compose, session);
            return false;
        }
    }
    if (tc::component_compose::commit(compose, session) != TC_COMPONENT_COMPOSE_OK) {
        refusal = "commit refused: " + std::string(tc::component_compose::status(compose, session));
        return false;
    }
    probeLog("decoder: registered " + hex64(definition.customId) + " with " +
             std::to_string(definition.nodes.size()) + " inner parts and " +
             std::to_string(definition.connections.size()) + " wires (" +
             std::to_string(composite_decoder::gateCount(definition)) + " two-input ANDs)");
    return true;
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    (void)plugin;
    if (!h || h->api_version != TC_MOD_API_VERSION) return 1;
    probeHost = h;
    if (!tc::component_compose::tableV2(h, &compose)) {
        probeLog("decoder: this loader has no tc.component.compose V2");
        return 2;
    }
    registered = registerDecoder();
    if (!registered) probeLog("decoder: registration failed: " + refusal);
    return registered ? 0 : 3;
}
