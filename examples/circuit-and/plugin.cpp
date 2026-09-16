#include "../../sdk/tc_mod.h"
#include "and_fixture.hpp"
#include <cstring>
#include <fstream>
#include <string>

static const TCHost* host;
static tc::TCGameModel game;
static tc::TCComponentModel components;

static void log(const std::string& message) {
    if (host) host->log(host->context, message.c_str());
}

// Optional diagnostic override for the cached design statistics.  When the
// file <plugin data>/design-stats.txt contains "<gates> <delay>", those numbers
// are written into the registered prototype instead of the fixture defaults.
// This is how the game reports a component's own cost, so it is also the knob
// for checking that the values mod authors write really reach the UI.
static void applyDesignStatsOverride(uint64_t& gates, uint64_t& delay) {
    if (!host || !host->data_directory_utf8) return;
    std::ifstream file(std::string(host->data_directory_utf8) + "/design-stats.txt");
    long long parsed_gates = 0;
    long long parsed_delay = 0;
    if (file >> parsed_gates >> parsed_delay && parsed_gates >= 0 &&
        parsed_delay >= 0) {
        gates = static_cast<uint64_t>(parsed_gates);
        delay = static_cast<uint64_t>(parsed_delay);
        log("circuit-and: design stats override gates=" + std::to_string(gates) +
            " delay=" + std::to_string(delay));
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION ||
        h->size < sizeof(TCHost) || plugin->size < sizeof(TCPlugin)) {
        return 1;
    }
    host = h;
    if (!game.load(h) || !game.valid()) return 2;
    if (!components.load(h) || !components.valid()) return 3;

    const auto bytes = tc_example::buildAndCircuitFile();
    // importCircuit requires a valid trailing directory, but the definition is
    // fully contained in the byte string; the game only uses it for errors.
    auto imported = components.importCircuit(
        "AND2 Test", bytes.data(), bytes.size(), "D:/tc-modloader/");
    if (!imported.ok() || imported.custom_id != tc_example::kAndComponentId) {
        log("circuit-and: import failed");
        return 4;
    }

    tc::TCPrototype prototype{};
    if (!game.getCustomPrototype(imported.custom_id, prototype)) {
        log("circuit-and: prototype lookup failed");
        return 5;
    }
    if (!game.setPrototypeName(prototype, "AND2 Test") ||
        !game.setPrototypeDescription(
            prototype, "Two-input AND gate registered by example.circuit-and")) {
        log("circuit-and: metadata update failed");
        return 6;
    }

    std::string shape;
    tc::TCPrototype builtin{};
    if (game.cloneBuiltinPrototype(0x04, builtin)) {
        const char* svg = tc::prototypeShapeSvgCStr(builtin);
        if (svg) shape = svg;
    }
    if (!shape.empty() && !game.setPrototypeShapeSvg(prototype, shape.c_str())) {
        log("circuit-and: shape update failed");
        return 7;
    }
    // The game keeps the delay cached in the definition header verbatim, so a
    // definition must carry its real critical path.  Write the design's own
    // statistics explicitly instead of trusting whatever the source bytes say.
    uint64_t design_gates = tc_example::kDesignGates;
    uint64_t design_delay = tc_example::kDesignDelay;
    applyDesignStatsOverride(design_gates, design_delay);
    if (!game.setPrototypeGateCost(prototype, design_gates) ||
        !game.setPrototypeDelay(prototype, design_delay)) {
        log("circuit-and: design cost update failed");
        return 8;
    }
    const uint64_t gate_cost = tc::prototypeGateCost(prototype);
    const uint64_t delay_cost = tc::prototypeDelay(prototype);
    if (!game.setCustomPrototype(imported.custom_id, prototype)) {
        log("circuit-and: prototype write failed");
        return 9;
    }
    if (components.releasePrototype(prototype) != tc::TCComponentStatus::Ok) {
        log("circuit-and: snapshot release failed");
        return 10;
    }

    // Kind 0x4e instances read their cost straight from the prototype fields
    // above, so no entry in the per-kind score table is needed.  A board's
    // critical path is computed from the inlined design, which for this
    // fixture is the same single gate delay.
    log("circuit-and: registered AND2 Test id=" +
        std::to_string(imported.custom_id) + " design gates=" +
        std::to_string(gate_cost) + " delay=" + std::to_string(delay_cost));
    return 0;
}
