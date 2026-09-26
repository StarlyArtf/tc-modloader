#ifndef TC_SIM_CHANNEL_H
#define TC_SIM_CHANNEL_H

#include "tc_mod_api.h"
#include "tc_service_api.h"

#include <stdint.h>

/* "Which signal is this, in the simulation state buffer?" - TC_SERVICE_SIM_CHANNEL.

   A wire's state byte offset and bit width live in the board's own wire record.
   Every Mod that wanted to watch something used to dig those out for itself, and
   the offsets went stale the moment the circuit was recompiled.  This service
   turns a wire handle (from a board object snapshot) into a channel, and
   re-resolves channels afterwards - by wire id first, by endpoints when a
   recompile moved the records.

   ```cpp
   tc::sim_channel::Api channels{};
   if (tc::sim_channel::table(host, &channels)) {
       TCSimWireChannelV1 channel{};                       // wire handle from the snapshot
       if (tc::sim_channel::fromWire(channels, &wireHandle, &channel) == TC_SIM_CHANNEL_OK)
           tc::simulation::sampleOne(sim, channel.byte_offset, channel.bits, &value);
   }
   // after a recompile:
   uint32_t live = 0;
   tc::sim_channel::resolve(channels, &boardHandle, probes, probeCount, &live);
   ``` */

namespace tc {
namespace sim_channel {

using Api = TCSimChannelApiV1;
using Channel = TCSimWireChannelV1;

inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_SIM_CHANNEL, TC_SIM_CHANNEL_API_VERSION_1,
                            &queried, sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) || queried.version != TC_SIM_CHANNEL_API_VERSION_1 ||
        !queried.context || !queried.from_wire || !queried.resolve)
        return false;
    *out = queried;
    return true;
}

/* `out->channel_id` is echoed back when set, so a caller can keep its own
   ordering; the wire id is used when it is 0. */
inline int fromWire(const Api& api, const TCGameHandle* wire, Channel* out) {
    if (!api.from_wire || !out) return TC_SIM_CHANNEL_ERR_ARGUMENT;
    out->size = sizeof(*out);
    out->version = TCSIM_WIRE_CHANNEL_VERSION_1;
    return api.from_wire(api.context, wire, out, sizeof(*out));
}

/* Re-resolves in place.  A channel that no longer resolves keeps its id and
   comes back with bits = 0. */
inline int resolve(const Api& api, const TCGameHandle* board, Channel* channels,
                   uint32_t count, uint32_t* outResolved = nullptr) {
    if (!api.resolve) return TC_SIM_CHANNEL_ERR_UNAVAILABLE;
    return api.resolve(api.context, board, channels, count, outResolved);
}

/* True when the channel can be read right now. */
inline bool live(const Channel& channel) {
    return channel.bits >= 1u && channel.bits <= 64u;
}

}  // namespace sim_channel
}  // namespace tc

#endif  // TC_SIM_CHANNEL_H
