#ifndef TC_COMPONENT_REGISTRY_H
#define TC_COMPONENT_REGISTRY_H
#include "tc_mod_api.h"
#include <string>

/* C++ convenience layer over TC_SERVICE_COMPONENT_REGISTRY.

   The registration side of a custom component already exists (register_logic
   for an imported definition, register_component for a declarative one), but
   nothing could ask "what component types does this session know?", "what pins
   does that type have?" or "why was my registration refused?".  This service is
   that view: one entry per registered type, with its owner Mod, its shape, its
   declared costs and - when the loader refused it - the reason.

   It is a read-only view of what the loader registered in this session; the
   plan's later stages build the write side (type declarations, variable-length
   pins and signals) on top of the same catalogue.

   ```
   TCComponentRegistryApiV1 registry{};
   if (tc::component_registry::table(host, &registry)) {
       for (uint32_t index = 0; index < registry.count(registry.context); ++index) {
           TCComponentTypeInfoV1 type{index};   // or TCComponentTypeInfoInit
           if (registry.get(registry.context, index, &type) == TC_COMPONENT_REGISTRY_OK)
               log(type.name);
       }
   }
   ```

   Threading: registrations happen while a Mod loads (the main thread), and the
   service reports what is registered at that moment; reading it from a frame
   callback sees every type that loaded before this frame. */

namespace tc {
namespace component_registry {

/* Every query struct starts with size/version so the host can grow it. */
inline TCComponentTypeInfoV1 typeInfo() {
    TCComponentTypeInfoV1 value{};
    value.size = sizeof(value);
    return value;
}
inline TCComponentPinInfoV1 pinInfo() {
    TCComponentPinInfoV1 value{};
    value.size = sizeof(value);
    return value;
}

inline bool table(const TCHost* host, TCComponentRegistryApiV1* out) {
    if (!host || !out || !host->query_service) return false;
    TCComponentRegistryApiV1 queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_REGISTRY,
                            TC_COMPONENT_REGISTRY_API_VERSION_1, &queried, sizeof(queried)) !=
        TC_SERVICE_OK)
        return false;
    if (queried.version != TC_COMPONENT_REGISTRY_API_VERSION_1 ||
        queried.size < sizeof(queried))
        return false;
    *out = queried;
    return true;
}

inline bool ready(const TCComponentRegistryApiV1& api) {
    return api.size >= sizeof(TCComponentRegistryApiV1) &&
           api.version == TC_COMPONENT_REGISTRY_API_VERSION_1 && api.count && api.get &&
           api.find && api.pin;
}

/* Types registered so far this session (including refused ones). */
inline uint32_t count(const TCComponentRegistryApiV1& api) {
    return api.count ? api.count(api.context) : 0;
}

inline int get(const TCComponentRegistryApiV1& api, uint32_t index,
               TCComponentTypeInfoV1* out) {
    return api.get ? api.get(api.context, index, out) : TC_COMPONENT_REGISTRY_ERR_UNAVAILABLE;
}

inline int find(const TCComponentRegistryApiV1& api, uint64_t custom_id,
                TCComponentTypeInfoV1* out) {
    return api.find ? api.find(api.context, custom_id, out)
                    : TC_COMPONENT_REGISTRY_ERR_UNAVAILABLE;
}

inline int pin(const TCComponentRegistryApiV1& api, uint64_t custom_id, uint32_t direction,
               uint32_t index, TCComponentPinInfoV1* out) {
    return api.pin ? api.pin(api.context, custom_id, direction, index, out)
                   : TC_COMPONENT_REGISTRY_ERR_UNAVAILABLE;
}

/* One readable line per type, for logs and the Mods page. */
inline std::string describe(const TCComponentTypeInfoV1& type) {
    std::string text = std::string(type.owner_mod) + "/" + std::string(type.name) + " [" +
                       type.type_id + "] " + std::to_string(type.input_count) + "in/" +
                       std::to_string(type.output_count) + "out cost=" +
                       std::to_string(type.gate_cost) + " delay=" + std::to_string(type.delay);
    if (!type.active) text += std::string(" refused: ") + type.status;
    return text;
}

}  // namespace component_registry
}  // namespace tc

#endif  // TC_COMPONENT_REGISTRY_H
