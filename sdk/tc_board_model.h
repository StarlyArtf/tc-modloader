#ifndef TC_BOARD_MODEL_H
#define TC_BOARD_MODEL_H

// Minimal board-selection model for the pinned Turing Complete 2.1.334
// build.  Instead of parsing Nim's selected-component/wire hash sets by hand,
// this wrapper delegates to the game's own verified contains function.
//
//   selected_components__modelZboardZboard_u22
//   selected_wires__modelZboardZboard_u30
//   prev_selected_components__modelZboardZboard_u41
//   prev_selected_wires__modelZboardZboard_u44
//   contains__modelZboardZboard_u1842

#include "tc_mod_api.h"
#include <stdint.h>
#include <string.h>

namespace tc {

using TCBoardContainsFn = uint8_t (*)(const void* set, uint64_t id);
using TCBoardLenFn = uint64_t (*)(const void* set);

struct TCBoardModel {
    const void* selected_components = nullptr;
    const void* selected_wires = nullptr;
    const void* prev_selected_components = nullptr;
    const void* prev_selected_wires = nullptr;
    const void* selection_kind_cache = nullptr;
    TCBoardContainsFn contains = nullptr;
    TCBoardLenFn len = nullptr;

    bool load(const TCHost* host) {
        if (host == nullptr) return false;
        selected_components = host->resolve_symbol(
            host->context, "selected_components__modelZboardZboard_u22");
        selected_wires = host->resolve_symbol(
            host->context, "selected_wires__modelZboardZboard_u30");
        prev_selected_components = host->resolve_symbol(
            host->context, "prev_selected_components__modelZboardZboard_u41");
        prev_selected_wires = host->resolve_symbol(
            host->context, "prev_selected_wires__modelZboardZboard_u44");
        selection_kind_cache = host->resolve_symbol(
            host->context, "selection_kind_cache__modelZboardZboard_u9060");
        contains = reinterpret_cast<TCBoardContainsFn>(
            host->resolve_symbol(host->context,
                                 "contains__modelZboardZboard_u1842"));
        len = reinterpret_cast<TCBoardLenFn>(
            host->resolve_symbol(host->context,
                                 "len__modelZboardZboard_u19087"));
        return valid();
    }

    bool valid() const {
        return selected_components != nullptr &&
               selected_wires != nullptr && contains != nullptr &&
               len != nullptr;
    }

    bool isComponentSelected(uint64_t component_id) const {
        return valid() && contains(selected_components, component_id) != 0;
    }

    bool isWireSelected(uint64_t wire_id) const {
        return valid() && contains(selected_wires, wire_id) != 0;
    }

    bool hasPreviousSelection() const {
        return prev_selected_components != nullptr &&
               prev_selected_wires != nullptr;
    }

    bool isComponentPreviouslySelected(uint64_t component_id) const {
        return valid() && hasPreviousSelection() &&
               contains(prev_selected_components, component_id) != 0;
    }

    bool isWirePreviouslySelected(uint64_t wire_id) const {
        return valid() && hasPreviousSelection() &&
               contains(prev_selected_wires, wire_id) != 0;
    }

    uint64_t selectedComponentCount() const {
        return valid() ? len(selected_components) : 0;
    }

    uint64_t selectedWireCount() const {
        return valid() ? len(selected_wires) : 0;
    }

    uint64_t previousSelectedComponentCount() const {
        return valid() && hasPreviousSelection()
                   ? len(prev_selected_components)
                   : 0;
    }

    uint64_t previousSelectedWireCount() const {
        return valid() && hasPreviousSelection()
                   ? len(prev_selected_wires)
                   : 0;
    }

    bool selectionEmpty() const {
        return selectedComponentCount() == 0 && selectedWireCount() == 0;
    }

    bool previousSelectionEmpty() const {
        return previousSelectedComponentCount() == 0 &&
               previousSelectedWireCount() == 0;
    }

    const void* selectionKindCache() const {
        return selection_kind_cache;
    }

    uint64_t selectedComponentIdAt(uint64_t index) const {
        return setIdAt(selected_components, index);
    }

    uint64_t selectedWireIdAt(uint64_t index) const {
        return setIdAt(selected_wires, index);
    }

    uint64_t previousSelectedComponentIdAt(uint64_t index) const {
        return setIdAt(prev_selected_components, index);
    }

    uint64_t previousSelectedWireIdAt(uint64_t index) const {
        return setIdAt(prev_selected_wires, index);
    }

 private:
    uint64_t setIdAt(const void* set, uint64_t index) const {
        if (!set || !len || index >= len(set)) return 0;
        uint64_t capacity = 0;
        void* data = nullptr;
        memcpy(&capacity, set, sizeof(capacity));
        memcpy(&data, static_cast<const unsigned char*>(set) + 8,
               sizeof(data));
        if (!data || capacity > 4096) return 0;

        uint64_t seen = 0;
        for (uint64_t i = 0; i < capacity; ++i) {
            const auto* bucket =
                static_cast<const unsigned char*>(data) + i * 0x20;
            uint64_t hash = 0;
            memcpy(&hash, bucket + 0x08, sizeof(hash));
            if (hash == 0) continue;
            if (seen == index) {
                uint64_t id = 0;
                memcpy(&id, bucket + 0x10, sizeof(id));
                return id;
            }
            ++seen;
        }
        return 0;
    }
};

}  // namespace tc

#endif  // TC_BOARD_MODEL_H
