#ifndef TC_BOARD_MODEL_H
#define TC_BOARD_MODEL_H

// Minimal board-selection model for the pinned Turing Complete 2.1.334
// build.  Instead of parsing Nim's selected-component/wire hash sets by hand,
// this wrapper delegates to the game's own verified contains function.
//
//   selected_components__modelZboardZboard_u22
//   selected_wires__modelZboardZboard_u30
//   contains__modelZboardZboard_u1842

#include "tc_mod_api.h"
#include <stdint.h>

namespace tc {

using TCBoardContainsFn = uint8_t (*)(const void* set, uint64_t id);

struct TCBoardModel {
    const void* selected_components = nullptr;
    const void* selected_wires = nullptr;
    TCBoardContainsFn contains = nullptr;

    bool load(const TCHost* host) {
        if (host == nullptr) return false;
        selected_components = host->resolve_symbol(
            host->context, "selected_components__modelZboardZboard_u22");
        selected_wires = host->resolve_symbol(
            host->context, "selected_wires__modelZboardZboard_u30");
        contains = reinterpret_cast<TCBoardContainsFn>(
            host->resolve_symbol(host->context,
                                 "contains__modelZboardZboard_u1842"));
        return valid();
    }

    bool valid() const {
        return selected_components != nullptr &&
               selected_wires != nullptr && contains != nullptr;
    }

    bool isComponentSelected(uint64_t component_id) const {
        return valid() && contains(selected_components, component_id) != 0;
    }

    bool isWireSelected(uint64_t wire_id) const {
        return valid() && contains(selected_wires, wire_id) != 0;
    }
};

}  // namespace tc

#endif  // TC_BOARD_MODEL_H
