#ifndef TC_IO_VALUE_H
#define TC_IO_VALUE_H
#include "tc_mod_api.h"

/* C++ convenience layer over TC_SERVICE_IO_VALUE.

   The service is the supported way to read and write the game's own values - a
   level's or a component's global inputs, and a constant's value field - so a
   Mod never has to guess how the game parses an expression, truncates it to the
   field's width or stores the result.  Everything here is a thin wrapper: ask
   for the table once, then call it.

   ```
   TCIoValueApiV1 io{};
   if (tc::io_value::table(host, &io)) {
       uint64_t mask = 0;
       if (io.evaluate(io.context, "0xFFFFFFFF^(1<<23)", &mask) == TC_IO_VALUE_OK) {
           io.write_input(io.context, &board, pinIndex, value & ~mask);
       }
   }
   ```

   Threading follows the service: the input and constant writes run on the
   game's main/render thread (the game builds its UI there).  write_constant_slot
   only updates the runtime mirror and is safe from anywhere. */

namespace tc {
namespace io_value {

/* Queries the table.  Returns false when the loader predates the service, which
   is the signal to fall back to whatever the Mod did before. */
inline bool table(const TCHost* host, TCIoValueApiV1* out) {
    if (!host || !out || !host->query_service) return false;
    TCIoValueApiV1 queried{};
    if (host->query_service(host->context, TC_SERVICE_IO_VALUE, TC_IO_VALUE_API_VERSION_1,
                            &queried, sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.version != TC_IO_VALUE_API_VERSION_1 || queried.size < sizeof(queried))
        return false;
    *out = queried;
    return true;
}

inline bool ready(const TCIoValueApiV1& api) {
    return api.size >= sizeof(TCIoValueApiV1) && api.version == TC_IO_VALUE_API_VERSION_1 &&
           api.evaluate && api.read_input && api.write_input && api.flip_input &&
           api.input_width && api.write_constant;
}

/* Parses one expression; anything the game's own value field accepts. */
inline int evaluate(const TCIoValueApiV1& api, const char* expression, uint64_t* out) {
    return api.evaluate ? api.evaluate(api.context, expression, out)
                        : TC_IO_VALUE_ERR_UNAVAILABLE;
}

/* Formats like the game's value fields: TC_IO_VALUE_FORMAT_*. */
inline int format(const TCIoValueApiV1& api, uint64_t value, uint32_t width, uint32_t style,
                  char* out, uint32_t out_size) {
    return api.format_value ? api.format_value(api.context, value, width, style, out, out_size)
                            : TC_IO_VALUE_ERR_UNAVAILABLE;
}

inline int readInput(const TCIoValueApiV1& api, const TCGameHandle& board, uint64_t index,
                     uint64_t* out) {
    return api.read_input ? api.read_input(api.context, &board, index, out)
                          : TC_IO_VALUE_ERR_UNAVAILABLE;
}

inline int writeInput(const TCIoValueApiV1& api, const TCGameHandle& board, uint64_t index,
                      uint64_t value) {
    return api.write_input ? api.write_input(api.context, &board, index, value)
                           : TC_IO_VALUE_ERR_UNAVAILABLE;
}

inline int flipInput(const TCIoValueApiV1& api, const TCGameHandle& board, uint64_t index,
                     uint64_t bit) {
    return api.flip_input ? api.flip_input(api.context, &board, index, bit)
                          : TC_IO_VALUE_ERR_UNAVAILABLE;
}

inline int inputWidth(const TCIoValueApiV1& api, const TCGameHandle& board, uint64_t index,
                      uint32_t* out) {
    return api.input_width ? api.input_width(api.context, &board, index, out)
                           : TC_IO_VALUE_ERR_UNAVAILABLE;
}

/* The whole wide-constant write: setting, runtime slot and simulation refresh. */
inline int writeConstant(const TCIoValueApiV1& api, const TCGameHandle& board, uint64_t index,
                         uint64_t value) {
    return api.write_constant ? api.write_constant(api.context, &board, index, value)
                              : TC_IO_VALUE_ERR_UNAVAILABLE;
}

/* Runtime slot only; the caller owns the setting write and the refresh. */
inline int writeConstantSlot(const TCIoValueApiV1& api, uint64_t component_id, uint64_t value) {
    return api.write_constant_slot ? api.write_constant_slot(api.context, component_id, value)
                                   : TC_IO_VALUE_ERR_UNAVAILABLE;
}

/* Keeps the low `width` bits; the same truncation the service applies. */
inline uint64_t truncate(uint64_t value, uint32_t width) {
    if (width == 0) return 0;
    if (width >= 64) return value;
    return value & ((1ull << width) - 1ull);
}

}  // namespace io_value
}  // namespace tc

#endif
