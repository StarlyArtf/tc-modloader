#ifndef TC_NATIVE_COMPONENT_H
#define TC_NATIVE_COMPONENT_H
#include "tc_mod_api.h"
#include <cstddef>
#include <string>
#include <vector>

namespace tc {
// No circuit.data, manual routing, or game model binding is required.
// Call during tc_mod_load. Retain a stable, nonzero ID across releases/saves.
struct TCNativeComponent {
    uint64_t id = 0;
    std::string name, description, shapeSvg;
    std::vector<TCComponentPin> inputs, outputs;
    uint64_t gates = 1, delay = 1;
    TCLogicCallback callback = nullptr;
    void* user = nullptr;

    static bool available(const TCHost* host) {
        return host && host->api_version == TC_MOD_API_VERSION &&
            host->size >= offsetof(TCHost, register_component) + sizeof(host->register_component) &&
            host->register_component;
    }
    int registerWith(const TCHost* host) const {
        if (!available(host)) return -1;
        if (inputs.size() > 8 || outputs.size() > 8) return -2;
        TCNativeComponentDefinition d{};
        d.size = sizeof(d); d.custom_id = id; d.name = name.c_str();
        d.description = description.c_str();
        d.shape_svg = shapeSvg.empty() ? nullptr : shapeSvg.c_str();
        d.input_count = static_cast<uint32_t>(inputs.size());
        d.output_count = static_cast<uint32_t>(outputs.size());
        d.inputs = inputs.data(); d.outputs = outputs.data();
        d.gate_cost = gates; d.delay = delay; d.callback = callback; d.user = user;
        return host->register_component(host->context, &d);
    }
};
}
#endif
