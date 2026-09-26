/* Real-machine probe for TC_SERVICE_COMPONENT_REGISTRY.

   The catalogue is filled by the two registration paths, which only run inside
   the game: this Mod registers a definition the loader must refuse (nine input
   pins are outside what the bridge supports), then dumps the whole catalogue
   once another Mod's component shows up in it, so a playtest can compare what
   the session knows with what was really registered.

   It is the "at least one real-machine assertion" the capability needs: the
   offline test drives the store directly, this one proves the loader's own
   registration paths feed it in a running game. */
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_component_registry.h"
#include "../sdk/tc_logic_api.h"

#include <cstdio>
#include <string>
#include <vector>

static const TCHost* host;
static TCComponentRegistryApiV1 registry{};
static bool ready;
static bool dumped;

static void say(const std::string& message) {
    if (host && host->log) host->log(host->context, ("component registry: " + message).c_str());
}

/* A definition outside the bridge's shape rules: nine inputs is one past what
   the loader can schedule, so the registration must fail and the catalogue has
   to say why. */
static constexpr uint64_t kBadId = 0x4241445F53484150ULL; /* "BAD_SHAP" */
static TCComponentPin badInputs[9]{};

static void registerBadShape() {
    for (int index = 0; index < 9; ++index) {
        badInputs[index].name = "in";
        badInputs[index].bits = 1;
    }
    TCNativeComponentDefinition definition{};
    definition.size = sizeof(definition);
    definition.custom_id = kBadId;
    definition.name = "Registry probe (nine inputs)";
    definition.description = "must be refused: the bridge supports eight pins per direction";
    definition.input_count = 9;
    definition.output_count = 1;
    definition.inputs = badInputs;
    definition.outputs = badInputs;
    definition.gate_cost = 1;
    definition.delay = 1;
    definition.callback = [](TCLogicIO*) {};
    const int status = host->register_component(host->context, &definition);
    say(std::string("registering nine inputs returned ") + std::to_string(status) +
        (status == 0 ? " (accepted, which the playtest does not expect)" : " (refused)"));
}

static void dumpCatalogue() {
    const uint32_t total = tc::component_registry::count(registry);
    say("catalogue: " + std::to_string(total) + " type(s)");
    for (uint32_t index = 0; index < total; ++index) {
        TCComponentTypeInfoV1 type = tc::component_registry::typeInfo();
        if (tc::component_registry::get(registry, index, &type) != TC_COMPONENT_REGISTRY_OK)
            continue;
        char line[512];
        int written = std::snprintf(line, sizeof(line),
                                    "type id=%s owner=%s name=\"%s\" %uin/%uout cost=%llu "
                                    "delay=%llu active=%u caps=0x%x",
                                    type.type_id, type.owner_mod, type.name, type.input_count,
                                    type.output_count,
                                    static_cast<unsigned long long>(type.gate_cost),
                                    static_cast<unsigned long long>(type.delay), type.active,
                                    type.capabilities);
        if (type.status[0] && written > 0 && static_cast<size_t>(written) < sizeof(line) - 4)
            std::snprintf(line + written, sizeof(line) - static_cast<size_t>(written),
                          " status=\"%s\"", type.status);
        say(line);
        for (uint32_t direction = 0; direction <= TC_COMPONENT_PIN_OUTPUT; ++direction) {
            const uint32_t pins = direction == TC_COMPONENT_PIN_INPUT ? type.input_count
                                                                      : type.output_count;
            for (uint32_t pinIndex = 0; pinIndex < pins; ++pinIndex) {
                TCComponentPinInfoV1 pin = tc::component_registry::pinInfo();
                if (tc::component_registry::pin(registry, type.custom_id, direction, pinIndex,
                                                &pin) != TC_COMPONENT_REGISTRY_OK)
                    continue;
                std::snprintf(line, sizeof(line),
                              "pin id=%s %s[%u] name=\"%s\" bits=%u",
                              type.type_id,
                              direction == TC_COMPONENT_PIN_INPUT ? "in" : "out", pinIndex,
                              pin.name, pin.bits);
                say(line);
            }
        }
    }
}

static void frame(void*, const TCFrame*) {
    if (!ready || dumped) return;
    /* Wait for the other Mod's component: the probe's own refused registration
       is in the catalogue immediately, the example's valid one follows. */
    if (tc::component_registry::count(registry) < 2) return;
    dumped = true;
    dumpCatalogue();
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    if (!h->register_component || !h->log) return 2;
    if (!tc::component_registry::table(h, &registry) ||
        !tc::component_registry::ready(registry)) {
        say("service unavailable (older loader)");
        return 0;
    }
    ready = true;
    registerBadShape();
    out->on_frame = &frame;
    say("probe loaded");
    return 0;
}
