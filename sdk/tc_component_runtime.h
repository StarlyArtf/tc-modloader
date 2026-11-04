#ifndef TC_COMPONENT_RUNTIME_H
#define TC_COMPONENT_RUNTIME_H

/* tc.component.runtime V2 - mutating editor-owned component types while the
   game runs.

   tc.component.types only answers during tc_mod_load, because a type has to
   exist before the game compiles a board.  This service is the narrow
   exception: it adds, replaces or removes a type, and only while the
   component model is idle (game thread, no board loaded - the main menu, where
   a Mod's own editor page lives).  No live circuit can therefore be invalidated.

   Use it like the other services:

     tc::component_runtime::Api api{};
     if (tc::component_runtime::table(host, &api))
         status = tc::component_runtime::registerType(api, &definition_v3);

   On a loader that predates this service `table` answers false, and the caller
   keeps its "takes effect on the next start" behaviour. */

#include "tc_mod_api.h"
#include "tc_service_api.h"

namespace tc {
namespace component_runtime {

using Api = TCComponentRuntimeApiV2;
using Definition = TCComponentTypeDefinitionV3;

inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_RUNTIME,
                            TC_COMPONENT_RUNTIME_API_VERSION_2, &queried,
                            sizeof(queried)) == TC_SERVICE_OK &&
        queried.size >= sizeof(queried) &&
        queried.version == TC_COMPONENT_RUNTIME_API_VERSION_2 && queried.context &&
        queried.register_type && queried.replace_type && queried.unregister_type) {
        *out = queried;
        return true;
    }
    /* V1 is still enough for create/duplicate on an older loader. */
    TCComponentRuntimeApiV1 legacy{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_RUNTIME,
                            TC_COMPONENT_RUNTIME_API_VERSION_1, &legacy,
                            sizeof(legacy)) != TC_SERVICE_OK ||
        legacy.size < sizeof(legacy) ||
        legacy.version != TC_COMPONENT_RUNTIME_API_VERSION_1 || !legacy.context ||
        !legacy.register_type)
        return false;
    queried.size = sizeof(legacy);
    queried.version = TC_COMPONENT_RUNTIME_API_VERSION_1;
    queried.context = legacy.context;
    queried.register_type = legacy.register_type;
    *out = queried;
    return true;
}

inline int registerType(const Api& api, const Definition* definition) {
    if (!api.register_type) return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
    return api.register_type(api.context, definition);
}

inline int replaceType(const Api& api, const Definition* previous,
                       const Definition* replacement) {
    if (api.version < TC_COMPONENT_RUNTIME_API_VERSION_2 || !api.replace_type)
        return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
    return api.replace_type(api.context, previous, replacement);
}

inline int unregisterType(const Api& api, uint64_t custom_id) {
    if (api.version < TC_COMPONENT_RUNTIME_API_VERSION_2 || !api.unregister_type)
        return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
    return api.unregister_type(api.context, custom_id);
}

inline const char* errorText(int status) {
    switch (status) {
        case TC_COMPONENT_TYPES_OK: return "ok";
        case TC_COMPONENT_TYPES_ERR_UNAVAILABLE: return "this loader has no tc.component.runtime";
        case TC_COMPONENT_TYPES_ERR_ARGUMENT: return "the definition is malformed";
        case TC_COMPONENT_TYPES_ERR_DUPLICATE: return "that custom_id is already known";
        case TC_COMPONENT_TYPES_ERR_UNSUPPORTED: return "the shape cannot be bridged";
        case TC_COMPONENT_TYPES_ERR_GAME: return "the game refused the definition";
        case TC_COMPONENT_TYPES_ERR_BUDGET: return "the pin widths exceed the host's budget";
        case TC_COMPONENT_TYPES_ERR_STATE: return "a board is up; register from the main menu";
        default: return "unknown status";
    }
}

}  // namespace component_runtime
}  // namespace tc

#endif  // TC_COMPONENT_RUNTIME_H
