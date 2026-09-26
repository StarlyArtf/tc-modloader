#ifndef TC_COMPONENT_INSTANCES_H
#define TC_COMPONENT_INSTANCES_H

#include "tc_mod_api.h"
#include "tc_service_api.h"

#include <stdint.h>

/* tc.component.instances V1 - stable references to live component instances.

   ```cpp
   #include "tc_component_instances.h"

   static uint64_t gWatchedId = 0;
   static uint64_t gWatchedGeneration = 0;   // a handle survives frames

   // once, when the level is up:
   tc::component_instances::Api instances{};
   if (tc::component_instances::table(host, &instances)) {
       TCComponentInstanceHandle handles[8];
       uint32_t written = 0, total = 0;
       tc::component_instances::enumerate(instances, kMyCustomId, handles, 8, &written, &total);
       if (written) { gWatchedId = handles[0].instance_id;
                      gWatchedGeneration = handles[0].generation; }
   }

   // later: is it still the same instance?
   tc::component_instances::Handle handle{sizeof(handle),
                                          TC_COMPONENT_INSTANCES_API_VERSION_1,
                                          kMyCustomId, gWatchedId, gWatchedGeneration};
   if (tc::component_instances::validate(instances, handle) != TC_COMPONENT_INSTANCES_OK)
       gWatchedId = 0;                        // destroyed and possibly reused
   ```

   Why a generation: the game reuses instance slots, so an id kept from an
   earlier frame can end up naming a different component.  A handle whose
   generation no longer matches is refused (`ERR_STALE`) instead of resolving to
   whatever is there now. */

namespace tc {
namespace component_instances {

using Api = TCComponentInstancesApiV1;
using Handle = TCComponentInstanceHandle;
using Info = TCComponentInstanceInfoV1;

inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_INSTANCES,
                            TC_COMPONENT_INSTANCES_API_VERSION_1, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_COMPONENT_INSTANCES_API_VERSION_1 || !queried.context ||
        !queried.enumerate || !queried.validate || !queried.info || !queried.state ||
        !queried.reset)
        return false;
    *out = queried;
    return true;
}

/* Handles of live instances; custom_id 0 asks for every type.  Returns the
   number written, or -1 on a refusal (buffer too small: ask again with the
   total the host reported). */
/* The host answers OK only when *every* matching instance fitted in `out`, so a
   fixed-size array silently turns into "no instances" once a board carries more
   native components than that - and the set can still grow while the game binds
   a compiled board, so one retry is not always enough.  Loop until the call
   answers OK, growing to `total` each time; examples/clock and
   examples/float-ops both do, and the failure this prevents is a Mod reporting
   "the component is not on the board" for every edit on a busy board. */
inline int enumerate(const Api& api, uint64_t custom_id, Handle* out, uint32_t capacity,
                     uint32_t* written = nullptr, uint32_t* total = nullptr) {
    if (!api.enumerate || !out || capacity == 0) return TC_COMPONENT_INSTANCES_ERR_ARGUMENT;
    uint32_t count = 0, all = 0;
    const int status = api.enumerate(api.context, custom_id, out, capacity, &count, &all);
    if (written) *written = count;
    if (total) *total = all;
    return status;
}

inline int validate(const Api& api, const Handle& handle) {
    return api.validate ? api.validate(api.context, &handle)
                        : TC_COMPONENT_INSTANCES_ERR_UNAVAILABLE;
}

inline int info(const Api& api, const Handle& handle, Info* out) {
    if (!out || out->size < sizeof(*out)) return TC_COMPONENT_INSTANCES_ERR_ARGUMENT;
    return api.info ? api.info(api.context, &handle, out)
                    : TC_COMPONENT_INSTANCES_ERR_UNAVAILABLE;
}

/* Copies the instance's simulation state, oldest-first; a smaller buffer reports
   the real size in `words`. */
inline int state(const Api& api, const Handle& handle, uint64_t* out, uint32_t capacity,
                 uint32_t* words = nullptr) {
    if (!out || capacity == 0) return TC_COMPONENT_INSTANCES_ERR_ARGUMENT;
    uint32_t count = 0;
    const int status = api.state ? api.state(api.context, &handle, out, capacity, &count)
                                 : TC_COMPONENT_INSTANCES_ERR_UNAVAILABLE;
    if (words) *words = count;
    return status;
}

/* Runs this instance's RESET callback and zeroes its state.  The simulation is
   not touched: the caller decides when that is appropriate. */
inline int reset(const Api& api, const Handle& handle) {
    return api.reset ? api.reset(api.context, &handle)
                     : TC_COMPONENT_INSTANCES_ERR_UNAVAILABLE;
}

/* Builds a handle with this build's size/version already filled in. */
inline Handle makeHandle(uint64_t custom_id, uint64_t instance_id, uint64_t generation) {
    Handle handle{};
    handle.size = sizeof(handle);
    handle.version = TC_COMPONENT_INSTANCES_API_VERSION_1;
    handle.custom_id = custom_id;
    handle.instance_id = instance_id;
    handle.generation = generation;
    return handle;
}

inline const char* errorText(int status) {
    switch (status) {
        case TC_COMPONENT_INSTANCES_OK: return "ok";
        case TC_COMPONENT_INSTANCES_ERR_UNAVAILABLE: return "this loader has no tc.component.instances";
        case TC_COMPONENT_INSTANCES_ERR_ARGUMENT: return "null or undersized argument";
        case TC_COMPONENT_INSTANCES_ERR_RANGE: return "buffer too small or index out of range";
        case TC_COMPONENT_INSTANCES_ERR_STALE: return "the handle's generation no longer matches";
        default: return "unknown status";
    }
}

}  // namespace component_instances
}  // namespace tc

#endif  // TC_COMPONENT_INSTANCES_H
