#pragma once
/* Which component the game's selection set, its "top level" walks and its undo
   bookkeeping address for a custom instance.

   A custom component's internal circuit can hold another custom component.  The
   generated callback is addressed by the inner instance's own 64-bit id, while
   everything the *player* touches (clicking, dragging, selecting, undoing) is
   addressed by the enclosing component.  The loader's compile walk already sees
   both: every custom record (kind 0x4e) carries its own id at +0x08 and the id
   the game addresses it by at +0x18 - which is its own id when the component
   sits directly on the board (measured on the pinned build: every board-level
   component's +0x18 equals its +0x08).

   This header holds only the rule, so tests/component-instances.cpp can pin it
   offline; src/native_logic.hpp feeds it the records it walks and keeps the
   answer on the instance binding (tc.component.instances V2). */

#include <cstdint>
#include <map>
#include <vector>

namespace tc::logic {

/* One record of the flattened board sequence, as this question needs it. */
struct ComponentNode {
    uint8_t kind = 0;       /* the record's kind byte; 0x4e is a custom instance */
    uint64_t id = 0;        /* record +0x08, the id callbacks are addressed by */
    uint64_t container = 0; /* record +0x18, the id the game addresses it by */
};

/* instance id -> the id the game addresses that component by.  Non-custom
   records do not participate.  A record whose container field is zero maps to
   itself, so a caller never receives 0 for a live instance. */
inline std::map<uint64_t, uint64_t> containerMap(const std::vector<ComponentNode>& nodes) {
    std::map<uint64_t, uint64_t> owners;
    for (const auto& node : nodes) {
        if (node.kind != 0x4e || !node.id) continue;
        owners[node.id] = node.container ? node.container : node.id;
    }
    return owners;
}

}  // namespace tc::logic
