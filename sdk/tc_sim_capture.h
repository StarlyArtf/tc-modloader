#ifndef TC_SIM_CAPTURE_H
#define TC_SIM_CAPTURE_H

#include "tc_mod_api.h"
#include "tc_service_api.h"

#include <stdint.h>

/* Every cycle, not every frame - TC_SERVICE_SIM_CAPTURE.

   `tc::trace::Sampler` reads on the render thread; a fast run then skips
   cycles, which is what makes a waveform look like it jumped.  This service is
   the other half: the loader instruments the compiled program with a per-cycle
   tick, the tick writes the configured channels into a ring buffer on the
   simulation thread, and this reads that ring.

   The ring holds the most recent `depth` cycles, so it *is* the pre-trigger
   window; the trigger marks the row it fired on.  `gaps` counts cycles that
   were not simply one apart - zero is what "nothing was missed" looks like.

   ```cpp
   tc::sim_capture::Api capture{};
   if (tc::sim_capture::table(host, &capture)) {
       TCSimChannelV1 channels[2] = {};      // offsets from tc.sim.channel or the board
       channels[0].size = sizeof(TCSimChannelV1); channels[0].version = TCSIM_CHANNEL_VERSION_1;
       channels[0].byte_offset = a; channels[0].bits = 1;
       channels[1] = channels[0]; channels[1].byte_offset = b;
       TCCaptureTriggerV1 trigger{};
       trigger.size = sizeof(trigger); trigger.version = TCCAPTURE_TRIGGER_VERSION_1;
       trigger.channel = 0; trigger.edge = TCCAPTURE_EDGE_RISING;
       tc::sim_capture::configure(capture, channels, 2, 4096, &trigger);
       tc::sim_capture::start(capture);
       tc::simulation::runFor(sim, 200);       // the capture follows the run
       uint64_t cycles[4096] = {}; uint64_t values[4096 * 2] = {}; uint32_t rows = 4096;
       tc::sim_capture::read(capture, cycles, values, 4096, &rows);
   }
   ``` */

namespace tc {
namespace sim_capture {

using Api = TCCaptureApiV1;
using Status = TCCaptureStatusV1;
using Trigger = TCCaptureTriggerV1;

inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_SIM_CAPTURE, TC_SIM_CAPTURE_API_VERSION_1,
                            &queried, sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) || queried.version != TC_SIM_CAPTURE_API_VERSION_1 ||
        !queried.context || !queried.configure || !queried.start || !queried.stop ||
        !queried.read || !queried.status)
        return false;
    *out = queried;
    return true;
}

inline int configure(const Api& api, const TCSimChannelV1* channels, uint32_t count,
                     uint32_t depth, const Trigger* trigger = nullptr) {
    return api.configure ? api.configure(api.context, channels, count, depth, trigger)
                         : TC_SIM_CAPTURE_ERR_UNAVAILABLE;
}
inline int start(const Api& api) {
    return api.start ? api.start(api.context) : TC_SIM_CAPTURE_ERR_UNAVAILABLE;
}
inline int stop(const Api& api) {
    return api.stop ? api.stop(api.context) : TC_SIM_CAPTURE_ERR_UNAVAILABLE;
}
/* Oldest row first; `values` is row-major with status().channel_count per row. */
inline int read(const Api& api, uint64_t* cycles, uint64_t* values, uint32_t capacity,
                uint32_t* rows) {
    return api.read ? api.read(api.context, cycles, values, capacity, rows)
                    : TC_SIM_CAPTURE_ERR_UNAVAILABLE;
}
inline int status(const Api& api, Status* out) {
    if (!api.status || !out) return TC_SIM_CAPTURE_ERR_UNAVAILABLE;
    return api.status(api.context, out, sizeof(*out));
}

/* "Nothing was missed": the tick saw every cycle, no gaps and no restarts. */
inline bool complete(const Status& state) {
    return state.gaps == 0 && state.recording && state.rows > 0;
}

}  // namespace sim_capture
}  // namespace tc

#endif  // TC_SIM_CAPTURE_H
