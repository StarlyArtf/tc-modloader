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
using ApiV2 = TCComponentInstancesApiV2;
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

/* The same table plus `top_level_id`.  A Mod that wants the game's own
   selection/top-level id must ask for V2: querying V1 still answers, it just
   has no way to give the caller that pair.  The V1 calls stay callable on the
   returned table (they are the same pointers), so a consumer keeps one ApiV2
   value and passes it to the helpers below.

   ```cpp
   tc::component_instances::ApiV2 instances{};
   if (tc::component_instances::tableV2(host, &instances)) {
       uint64_t top = 0;
       if (tc::component_instances::topLevelId(instances, handle, &top) ==
           TC_COMPONENT_INSTANCES_OK) {
           // `top` is what the game's selected-component set addresses; it
           // equals handle.instance_id for a component placed on the board.
       }
   }
   ```

   Degrading: a loader that only knows V1 answers ERR_VERSION here, and the Mod
   can keep its own fallback (examples/clock reads the older
   `interactive-map.txt` diagnostic in that case) - it must not read the file
   when the service is available, or the two sources can disagree mid-frame. */
inline bool tableV2(const TCHost* host, ApiV2* out) {
    if (!host || !out || !host->query_service) return false;
    ApiV2 queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_INSTANCES,
                            TC_COMPONENT_INSTANCES_API_VERSION_2, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_COMPONENT_INSTANCES_API_VERSION_2 || !queried.context ||
        !queried.enumerate || !queried.validate || !queried.info || !queried.state ||
        !queried.reset || !queried.top_level_id)
        return false;
    *out = queried;
    return true;
}

/* The V1 half of a V2 table, so a consumer that needs both reads like one API.
   Built field by field rather than by casting: the two structs share a layout,
   but they are different C++ types. */
inline Api v1(const ApiV2& api) {
    Api view{};
    view.size = sizeof(view);
    view.version = TC_COMPONENT_INSTANCES_API_VERSION_1;
    view.context = api.context;
    view.enumerate = api.enumerate;
    view.validate = api.validate;
    view.info = api.info;
    view.state = api.state;
    view.reset = api.reset;
    return view;
}

/* The id the game's selection set and top-level lookups use for this instance.
   For a component placed directly on the board this is its own instance id.
   `out` is left at 0 unless the call answers OK, so a caller that ignores the
   status still cannot act on a half-written answer. */
inline int topLevelId(const ApiV2& api, const Handle& handle, uint64_t* out) {
    if (!out) return TC_COMPONENT_INSTANCES_ERR_ARGUMENT;
    if (!api.top_level_id) return TC_COMPONENT_INSTANCES_ERR_UNAVAILABLE;
    *out = 0;
    const int status = api.top_level_id(api.context, &handle, out);
    if (status != TC_COMPONENT_INSTANCES_OK) *out = 0;
    return status;
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
