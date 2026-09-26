#pragma once

/* Frame-local layout contributions for entries in the game's IO panel.

   The panel itself and Mods can draw controls inside the same logical pin
   entry.  Cursor anchors describe where the panel starts rows, but they do not
   describe the controls that were added between those anchors (and the last
   entry has no following row to measure against).  This store is the broker:
   producers include screen-space rectangles for an entry and decorators query
   their union on the next frame.

   Only the UI thread uses the store.  Two completed frames are retained because
   decorators intentionally paint the previous frame's finished layout. */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tc::pin_order_bounds {

struct Bounds {
    int32_t frame = -1;
    uint32_t group = 0;
    uint64_t key = 0;
    float min_x = 0.f;
    float min_y = 0.f;
    float max_x = 0.f;
    float max_y = 0.f;
};

class Store {
public:
    int include(const Bounds& value) {
        if (value.frame < 0 || value.group >= 3 || !finite(value.min_x) ||
            !finite(value.min_y) || !finite(value.max_x) || !finite(value.max_y) ||
            value.max_x < value.min_x || value.max_y < value.min_y)
            return -2;

        if (value.frame > newest_frame_) {
            newest_frame_ = value.frame;
            const int32_t oldest = newest_frame_ > 2 ? newest_frame_ - 2 : 0;
            entries_.erase(
                std::remove_if(entries_.begin(), entries_.end(),
                               [oldest](const Bounds& item) { return item.frame < oldest; }),
                entries_.end());
        }
        for (Bounds& item : entries_) {
            if (item.frame != value.frame || item.group != value.group || item.key != value.key)
                continue;
            item.min_x = std::min(item.min_x, value.min_x);
            item.min_y = std::min(item.min_y, value.min_y);
            item.max_x = std::max(item.max_x, value.max_x);
            item.max_y = std::max(item.max_y, value.max_y);
            return 0;
        }
        /* A broken producer must not make an unbounded per-frame registry. */
        if (entries_.size() >= 4096) return -4;
        entries_.push_back(value);
        return 0;
    }

    int get(int32_t frame, uint32_t group, uint64_t key, Bounds* out) const {
        if (!out || frame < 0 || group >= 3) return -2;
        for (const Bounds& item : entries_) {
            if (item.frame == frame && item.group == group && item.key == key) {
                *out = item;
                return 0;
            }
        }
        return -6;
    }

    void clear() {
        entries_.clear();
        newest_frame_ = -1;
    }

private:
    static bool finite(float value) { return std::isfinite(value); }
    std::vector<Bounds> entries_;
    int32_t newest_frame_ = -1;
};

inline Store& store() {
    static Store value;
    return value;
}

}  // namespace tc::pin_order_bounds
