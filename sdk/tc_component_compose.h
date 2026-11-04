#ifndef TC_COMPONENT_COMPOSE_H
#define TC_COMPONENT_COMPOSE_H
#include "tc_mod_api.h"
#include "tc_service_api.h"
#include <string>

/* C++ convenience layer over TC_SERVICE_COMPONENT_COMPOSE (M4, first cut).

   The service is the built-in type catalogue: what the running build exposes,
   what the game calls each part, the stable spelling a Mod keys on, and the pins
   each part declares.  It exists because a composite component is addressed by
   kind byte, and a kind byte is build data - a Mod that wants to survive a
   different build asks for `builtin/<name>` instead of a number it read once.

   ```cpp
   tc::component_compose::Api compose{};
   if (tc::component_compose::table(host, &compose)) {
       const uint32_t total = tc::component_compose::count(compose);
       TCBuiltinTypeInfoV1 kind{};
       if (tc::component_compose::findByTypeId(compose, "builtin/AND", &kind) ==
           TC_COMPONENT_COMPOSE_OK) {
           TCBuiltinPinV1 pin{};
           tc::component_compose::pin(compose, kind.kind, TC_COMPONENT_PIN_INPUT, 0, &pin);
           // pin.x / pin.y are board-cell offsets, so a wire endpoint can be
           // described without knowing anything about the circuit file format.
       }
   }
   ```

   Threading: filled once while the loader boots (game thread); the queries are
   read-only.  An older loader has no such service and `table()` returns false. */

namespace tc {
namespace component_compose {

/* The result type of a successful query. */
using Api = TCComponentComposeApiV1;
/* V2 keeps the V1 prefix and adds the internal-netlist builder. */
using ApiV2 = TCComponentComposeApiV2;

inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_COMPOSE,
                            TC_COMPONENT_COMPOSE_API_VERSION_1, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.version != TC_COMPONENT_COMPOSE_API_VERSION_1 ||
        queried.size < sizeof(queried) || !queried.count || !queried.get ||
        !queried.find || !queried.find_by_type_id || !queried.pin)
        return false;
    *out = queried;
    return true;
}

inline bool tableV2(const TCHost* host, ApiV2* out) {
    if (!host || !out || !host->query_service) return false;
    ApiV2 queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_COMPOSE,
                            TC_COMPONENT_COMPOSE_API_VERSION_2, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.version != TC_COMPONENT_COMPOSE_API_VERSION_2 ||
        queried.size < sizeof(queried) || !queried.count || !queried.get ||
        !queried.find || !queried.find_by_type_id || !queried.pin || !queried.begin ||
        !queried.node || !queried.connect || !queried.commit || !queried.abort ||
        !queried.status || !queried.clone_metadata || !queried.alias ||
        !queried.pin_lane)
        return false;
    *out = queried;
    return true;
}

/* The V1 view of the same table: a consumer that only reads the catalogue keeps
   one shape and does not care which version the loader published. */
inline Api v1(const ApiV2& api) {
    Api view{};
    view.size = sizeof(view);
    view.version = TC_COMPONENT_COMPOSE_API_VERSION_1;
    view.context = api.context;
    view.count = api.count;
    view.get = api.get;
    view.find = api.find;
    view.find_by_type_id = api.find_by_type_id;
    view.pin = api.pin;
    return view;
}

/* ---- V2: the builder ---------------------------------------------------- */

inline int begin(const ApiV2& api, const TCComposeTypeV1& type, uint64_t* session) {
    return api.begin ? api.begin(api.context, &type, session) : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

inline int node(const ApiV2& api, uint64_t session, const char* typeId, int32_t x, int32_t y,
                uint32_t bits, uint32_t* nodeIndex) {
    return api.node ? api.node(api.context, session, typeId, x, y, bits, nodeIndex)
                    : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

inline int connect(const ApiV2& api, uint64_t session, uint32_t fromNode, uint32_t fromPin,
                   uint32_t toNode, uint32_t toPin) {
    return api.connect ? api.connect(api.context, session, fromNode, fromPin, toNode, toPin)
                       : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

inline int commit(const ApiV2& api, uint64_t session) {
    return api.commit ? api.commit(api.context, session) : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

inline int abort(const ApiV2& api, uint64_t session) {
    return api.abort ? api.abort(api.context, session) : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

inline const char* status(const ApiV2& api, uint64_t session) {
    const char* text = api.status ? api.status(api.context, session) : nullptr;
    return text ? text : "";
}

/* Copies the requested metadata of a built-in part onto a type this Mod already
   registered.  The flags are TC_COMPOSE_CLONE_*; the icon is not one of them
   (see the contract comment in tc_service_api.h). */
inline int cloneMetadata(const ApiV2& api, uint64_t customId, const char* builtinTypeId,
                         uint32_t flags) {
    return api.clone_metadata ? api.clone_metadata(api.context, customId, builtinTypeId, flags)
                              : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

/* Plan section 8.3's full alias.  This build answers
   TC_COMPONENT_COMPOSE_ERR_UNSUPPORTED, always - a new id cannot reach a
   built-in's dispatch (see the contract comment).  Treat the call as the
   question "is this build able to alias?" rather than as a promise. */
inline int alias(const ApiV2& api, uint64_t customId, const char* builtinTypeId) {
    return api.alias ? api.alias(api.context, customId, builtinTypeId)
                     : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

/* Widens the pin lane of an open session (1..16 board cells; the default is the
   historical two-cell shape).  The inner circuit is laid out in the space that
   opens between the input and the output column. */
inline int pinLane(const ApiV2& api, uint64_t session, uint32_t laneCells) {
    return api.pin_lane ? api.pin_lane(api.context, session, laneCells)
                        : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

/* Index of the type's own input pin inside the node list, and of its output pin
   `index` - what `connect` expects for a wire that leaves an input pin or
   reaches an output pin. */
inline uint32_t inputPinNode(uint32_t index) { return index; }
inline uint32_t outputPinNode(const TCComposeTypeV1& type, uint32_t innerNodes, uint32_t index) {
    return type.input_count + innerNodes + index;
}

inline uint32_t count(const Api& api) {
    return api.count ? api.count(api.context) : 0;
}

inline int get(const Api& api, uint32_t index, TCBuiltinTypeInfoV1* out) {
    return api.get ? api.get(api.context, index, out)
                   : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

inline int find(const Api& api, uint32_t kind, TCBuiltinTypeInfoV1* out) {
    return api.find ? api.find(api.context, kind, out)
                    : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

inline int findByTypeId(const Api& api, const char* typeId, TCBuiltinTypeInfoV1* out) {
    return api.find_by_type_id ? api.find_by_type_id(api.context, typeId, out)
                               : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

inline int pin(const Api& api, uint32_t kind, uint32_t direction, uint32_t index,
               TCBuiltinPinV1* out) {
    return api.pin ? api.pin(api.context, kind, direction, index, out)
                   : TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
}

/* True when the pin's width is a number of bits the caller can rely on.  A
   marker (auto width, or one of the derived markers this build uses) leaves
   `bits` 0; the raw field is still there for whoever must match it exactly. */
inline bool pinHasBits(const TCBuiltinPinV1& pin) {
    return pin.bits >= 1 && pin.bits <= 64 &&
           !(pin.flags & (TC_BUILTIN_PIN_AUTO_WIDTH | TC_BUILTIN_PIN_RAW_WIDTH));
}

/* One readable line per entry, for logs and tooling. */
inline std::string describe(const TCBuiltinTypeInfoV1& type) {
    std::string text = "0x";
    const char* hex = "0123456789abcdef";
    text += hex[(type.kind >> 4) & 0xf];
    text += hex[type.kind & 0xf];
    text += " " + std::string(type.name) + " [" + std::string(type.type_id) + "] " +
            std::to_string(type.input_count) + "in/" + std::to_string(type.output_count) + "out";
    if (type.flags & TC_BUILTIN_FLAG_NO_DECLARED_INPUTS) text += " (no declared inputs)";
    if (type.flags & TC_BUILTIN_FLAG_HAS_SHAPE) text += " (shape)";
    return text;
}

}  // namespace component_compose
}  // namespace tc

#endif  // TC_COMPONENT_COMPOSE_H
