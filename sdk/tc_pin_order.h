#ifndef TC_PIN_ORDER_H
#define TC_PIN_ORDER_H
#include "tc_mod_api.h"

/* C++ convenience layer over TC_SERVICE_PIN_ORDER.

   A Mod that wants the player to drag the pins of the game's own IO panel into
   another order asks this service rather than touching the panel's cache: it
   reads what is on screen, asks for a move, and the loader applies it the next
   time the panel is built.

   ```
   TCPinOrderApiV1 pins{};
   if (tc::pin_order::table(host, &pins)) {
       TCPinOrderEntryV1 row{};
       if (pins.entry(pins.context, TC_PIN_ORDER_GROUP_INPUTS, 0, &row) == TC_PIN_ORDER_OK)
           pins.move(pins.context, TC_PIN_ORDER_GROUP_INPUTS, 0, 2);
   }
   ```

   Threading: the calls describe (and edit) the panel, so they belong on the
   game's main/render thread, where the panel is built and drawn. */

namespace tc {
namespace pin_order {

/* Queries the table.  Returns false when the loader predates the service, which
   is the signal to leave the panel alone. */
inline bool table(const TCHost* host, TCPinOrderApiV1* out) {
    if (!host || !out || !host->query_service) return false;
    TCPinOrderApiV1 queried{};
    if (host->query_service(host->context, TC_SERVICE_PIN_ORDER, TC_PIN_ORDER_API_VERSION_1,
                            &queried, sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.version != TC_PIN_ORDER_API_VERSION_1 || queried.size < sizeof(queried))
        return false;
    *out = queried;
    return true;
}

/* V2 adds a frame-local layout broker.  A producer reports the real rectangle
   of controls it inserted into an entry; a decorator can then fit the entry
   without knowing which Mod produced those controls. */
inline bool table(const TCHost* host, TCPinOrderApiV2* out) {
    if (!host || !out || !host->query_service) return false;
    TCPinOrderApiV2 queried{};
    if (host->query_service(host->context, TC_SERVICE_PIN_ORDER, TC_PIN_ORDER_API_VERSION_2,
                            &queried, sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.version != TC_PIN_ORDER_API_VERSION_2 || queried.size < sizeof(queried))
        return false;
    *out = queried;
    return true;
}

inline bool ready(const TCPinOrderApiV1& api) {
    return api.size >= sizeof(TCPinOrderApiV1) && api.version == TC_PIN_ORDER_API_VERSION_1 &&
           api.count && api.entry && api.move && api.order && api.set_order && api.reset;
}

inline bool ready(const TCPinOrderApiV2& api) {
    return api.size >= sizeof(TCPinOrderApiV2) && api.version == TC_PIN_ORDER_API_VERSION_2 &&
           api.count && api.entry && api.move && api.order && api.set_order && api.reset &&
           api.include_bounds && api.bounds;
}

/* Entries the panel is showing in this group right now (0 when it is not up). */
inline uint32_t count(const TCPinOrderApiV1& api, uint32_t group) {
    return api.count ? api.count(api.context, group) : 0;
}

inline int entry(const TCPinOrderApiV1& api, uint32_t group, uint32_t index,
                 TCPinOrderEntryV1* out) {
    return api.entry ? api.entry(api.context, group, index, out) : TC_PIN_ORDER_ERR_UNAVAILABLE;
}

inline int move(const TCPinOrderApiV1& api, uint32_t group, uint32_t from, uint32_t to) {
    return api.move ? api.move(api.context, group, from, to) : TC_PIN_ORDER_ERR_UNAVAILABLE;
}

inline int order(const TCPinOrderApiV1& api, uint32_t group, uint64_t* keys, uint32_t capacity,
                 uint32_t* count) {
    return api.order ? api.order(api.context, group, keys, capacity, count)
                     : TC_PIN_ORDER_ERR_UNAVAILABLE;
}

inline int setOrder(const TCPinOrderApiV1& api, uint32_t group, const uint64_t* keys,
                    uint32_t count) {
    return api.set_order ? api.set_order(api.context, group, keys, count)
                         : TC_PIN_ORDER_ERR_UNAVAILABLE;
}

inline int reset(const TCPinOrderApiV1& api, uint32_t group) {
    return api.reset ? api.reset(api.context, group) : TC_PIN_ORDER_ERR_UNAVAILABLE;
}

inline int includeBounds(const TCPinOrderApiV2& api, const TCPinOrderBoundsV1& bounds) {
    return api.include_bounds ? api.include_bounds(api.context, &bounds)
                              : TC_PIN_ORDER_ERR_UNAVAILABLE;
}

inline int bounds(const TCPinOrderApiV2& api, int32_t frame, uint32_t group, uint64_t key,
                  TCPinOrderBoundsV1* out) {
    return api.bounds ? api.bounds(api.context, frame, group, key, out, sizeof(*out))
                      : TC_PIN_ORDER_ERR_UNAVAILABLE;
}

/* Copies an entry's name into a caller buffer, always NUL terminated. */
inline const char* name(const TCPinOrderEntryV1& entryValue) { return entryValue.name; }

}  // namespace pin_order
}  // namespace tc

#endif  // TC_PIN_ORDER_H
