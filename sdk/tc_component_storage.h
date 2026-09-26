#ifndef TC_COMPONENT_STORAGE_H
#define TC_COMPONENT_STORAGE_H

#include "tc_mod_api.h"
#include "tc_service_api.h"

#include <stdint.h>

/* tc.component.storage V1 - host-owned configuration and simulation-state
   snapshots for a live component instance.

   Configuration is declared by TCComponentTypeDefinitionV2 and is visible as a
   read-only byte view in TCLogicIOV2.  It survives RESET.  Simulation state is
   the callback's `state_words` buffer and is cleared by RESET.  V1 is an
   in-memory ownership/snapshot layer; saving blobs into circuit.data, schema
   migration and Undo integration are intentionally later service versions. */

namespace tc {
namespace component_storage {

using Api = TCComponentStorageApiV1;
using Info = TCComponentStorageInfoV1;
using Handle = TCComponentInstanceHandle;

inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_STORAGE,
                            TC_COMPONENT_STORAGE_API_VERSION_1, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_COMPONENT_STORAGE_API_VERSION_1 || !queried.context ||
        !queried.info || !queried.read_config || !queried.write_config ||
        !queried.capture_state || !queried.restore_state)
        return false;
    *out = queried;
    return true;
}

inline int info(const Api& api, const Handle& handle, Info* out) {
    if (!out || out->size < sizeof(*out)) return TC_COMPONENT_STORAGE_ERR_ARGUMENT;
    return api.info ? api.info(api.context, &handle, out)
                    : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
}

inline int readConfig(const Api& api, const Handle& handle, void* out,
                      uint32_t capacity, uint32_t* bytes = nullptr) {
    uint32_t count = 0;
    const int status = api.read_config
                           ? api.read_config(api.context, &handle, out, capacity, &count)
                           : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
    if (bytes) *bytes = count;
    return status;
}

inline int writeConfig(const Api& api, const Handle& handle, uint32_t schema,
                       const void* data, uint32_t bytes) {
    return api.write_config
               ? api.write_config(api.context, &handle, schema, data, bytes)
               : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
}

inline int captureState(const Api& api, const Handle& handle, void* out,
                        uint32_t capacity, uint32_t* bytes = nullptr) {
    uint32_t count = 0;
    const int status = api.capture_state
                           ? api.capture_state(api.context, &handle, out, capacity, &count)
                           : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
    if (bytes) *bytes = count;
    return status;
}

inline int restoreState(const Api& api, const Handle& handle, const void* data,
                        uint32_t bytes) {
    return api.restore_state
               ? api.restore_state(api.context, &handle, data, bytes)
               : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
}

/* V2 adds an explicit edit transaction: `begin`, then any number of
   `writeConfig`, then `commit` (the span becomes one undo step) or `abort` (the
   configuration goes back to what it was when the transaction opened).  A tool
   that edits several instances says where the player-visible action begins and
   ends instead of the host guessing from frame boundaries. */
using ApiV2 = TCComponentStorageApiV2;

inline bool tableV2(const TCHost* host, ApiV2* out) {
    if (!host || !out || !host->query_service) return false;
    ApiV2 queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_STORAGE,
                            TC_COMPONENT_STORAGE_API_VERSION_2, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_COMPONENT_STORAGE_API_VERSION_2 || !queried.context ||
        !queried.info || !queried.read_config || !queried.write_config ||
        !queried.capture_state || !queried.restore_state || !queried.begin_edit ||
        !queried.commit_edit || !queried.abort_edit)
        return false;
    *out = queried;
    return true;
}

inline int beginEdit(const ApiV2& api, const Handle& handle) {
    return api.begin_edit ? api.begin_edit(api.context, &handle)
                          : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
}
inline int commitEdit(const ApiV2& api, const Handle& handle) {
    return api.commit_edit ? api.commit_edit(api.context, &handle)
                           : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
}
inline int abortEdit(const ApiV2& api, const Handle& handle) {
    return api.abort_edit ? api.abort_edit(api.context, &handle)
                          : TC_COMPONENT_STORAGE_ERR_UNAVAILABLE;
}

inline const char* errorText(int status) {
    switch (status) {
        case TC_COMPONENT_STORAGE_OK: return "ok";
        case TC_COMPONENT_STORAGE_ERR_UNAVAILABLE: return "this loader has no tc.component.storage";
        case TC_COMPONENT_STORAGE_ERR_ARGUMENT: return "null or undersized argument";
        case TC_COMPONENT_STORAGE_ERR_STALE: return "the instance handle is stale";
        case TC_COMPONENT_STORAGE_ERR_SIZE: return "the blob size or buffer capacity does not match";
        case TC_COMPONENT_STORAGE_ERR_SCHEMA: return "the configuration schema does not match";
        case TC_COMPONENT_STORAGE_ERR_STATE: return "no edit transaction is open, or one already is";
        default: return "unknown status";
    }
}

}  // namespace component_storage
}  // namespace tc

#endif  // TC_COMPONENT_STORAGE_H
