#ifndef TC_WIRE_MODEL_H
#define TC_WIRE_MODEL_H

#include "tc_mod_api.h"
#include <stdint.h>

namespace tc {

using TCWirePipetteFn = uint8_t (*)(void* model, uint32_t point);
using TCWireAtFn = int64_t (*)(void* model, uint32_t point);
using TCWireAddFn = void (*)(void* board, uint32_t point, uint8_t color);
using TCWirePlaceFn = void (*)(void*, void*, void*, void*, void*, void*);
using TCWireUpdateFn = bool (*)(void*, void*, void*, uint32_t, uint8_t);

struct TCWireModel {
    TCWirePipetteFn pipette_wire = nullptr;
    TCWireAtFn wire_at = nullptr;
    TCWireAddFn add_wire_from_pos = nullptr;
    TCWirePlaceFn handle_place_wire = nullptr;
    TCWireUpdateFn handle_update_wire = nullptr;
    const int64_t* invalid_wire_id = nullptr;

    bool load(const TCHost* host) {
        if (host == nullptr) return false;
        pipette_wire = reinterpret_cast<TCWirePipetteFn>(
            host->resolve_symbol(host->context,
                                 "pipette_wire__modelZutilities_u2289"));
        wire_at = reinterpret_cast<TCWireAtFn>(
            host->resolve_symbol(
                host->context,
                "get_wire__presenterZutilitiesZhelper95functions_u1916"));
        add_wire_from_pos = reinterpret_cast<TCWireAddFn>(
            host->resolve_symbol(host->context,
                                 "add_wire_from_pos__modelZboardZboard_u28435"));
        handle_place_wire = reinterpret_cast<TCWirePlaceFn>(
            host->resolve_symbol(
                host->context,
                "handle_place_wire__presenterZuser95inputZboard95ioZactionZplace95wire_u2"));
        handle_update_wire = reinterpret_cast<TCWireUpdateFn>(
            host->resolve_symbol(
                host->context,
                "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5"));
        invalid_wire_id = static_cast<const int64_t*>(
            host->resolve_symbol(host->context,
                                 "INVALID_WIRE_ID__modelZsave95mongerZcommon_u3578"));
        return valid();
    }

    bool valid() const {
        return pipette_wire != nullptr && wire_at != nullptr &&
               add_wire_from_pos != nullptr &&
               handle_place_wire != nullptr &&
               handle_update_wire != nullptr &&
               invalid_wire_id != nullptr;
    }

    int64_t invalidId() const {
        return invalid_wire_id ? *invalid_wire_id : -1;
    }

    int64_t wireAt(void* model, uint32_t point) const {
        return wire_at ? wire_at(model, point) : invalidId();
    }

    uint8_t pipetteWire(void* model, uint32_t point) const {
        return pipette_wire ? pipette_wire(model, point) : 255;
    }

    void addWire(void* board, uint32_t point, uint8_t color) const {
        if (add_wire_from_pos) add_wire_from_pos(board, point, color);
    }

    void placeWire(void* a, void* b, void* c, void* d, void* e, void* f) const {
        if (handle_place_wire) handle_place_wire(a, b, c, d, e, f);
    }

    bool updateWire(void* model, void* ctx, void* input, uint32_t point,
                    uint8_t fifth) const {
        return handle_update_wire
                   ? handle_update_wire(model, ctx, input, point, fifth)
                   : false;
    }
};

}  // namespace tc

#endif  // TC_WIRE_MODEL_H
