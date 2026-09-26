#pragma once
/* L2 of the sandbox simulator (docs/PLAN-sandbox-simulator.md section 4.3):
   a layered event queue ordered by (time, layer, sequence), fed by a time wheel
   with an overflow heap for anything past the wheel's horizon.

   Two properties the rest of the engine relies on:

     - `push` only ever schedules at or after `now()`, and an event scheduled
       for the time being processed is delivered in a *later* delta round, not
       inside the current one.  That is what makes a zero-delay loop converge
       round by round instead of recursing, and it is why the engine can count
       the rounds and report a zero-delay oscillation;
     - within one time step, events come out in (layer, sequence) order, so the
       active layer drains before the NBA layer and the run is reproducible.

   The wheel holds every deadline within `span` ticks of now, which is bounded
   by construction (an instance is sized from the netlist's longest delay), and
   the heap holds the rest, so a pathological delay cannot overflow the ring. */

#include "time.hpp"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <vector>

namespace tcsim {

enum class Layer : uint8_t { kActive = 0, kNBA = 1, kMonitor = 2 };

struct Event {
    Tick when = 0;
    Layer layer = Layer::kActive;
    uint64_t seq = 0;
    uint32_t kind = 0;
    uint32_t device = 0;
    uint16_t pin = 0;
    uint16_t flags = 0;
    uint64_t token = 0;
    BitVector value;
    Logic drive = Logic::kZ;
    Strength strength = Strength::kStrong;
};

inline bool eventLess(const Event& left, const Event& right) {
    if (left.when != right.when) return left.when < right.when;
    if (left.layer != right.layer) return left.layer < right.layer;
    return left.seq < right.seq;
}

class Scheduler {
public:
    /* `span` must be a power of two and strictly greater than the longest
       delay the netlist can produce; the engine sizes it that way. */
    explicit Scheduler(Tick span = 1024) {
        Tick slots = 1;
        while (slots < span) slots <<= 1;
        span_ = slots;
        mask_ = static_cast<uint64_t>(slots - 1);
        wheel_.resize(static_cast<size_t>(slots));
    }

    Tick span() const { return span_; }
    Tick now() const { return now_; }
    uint64_t pushes() const { return pushes_; }
    /* Deadlines that would have been in the past; a netlist bug, so the engine
       exposes the count instead of silently rewriting history. */
    uint64_t clamped() const { return clamped_; }
    /* Wheel entries that turned up outside their own span; must stay 0. */
    uint64_t dropped() const { return dropped_; }

    void push(Event event) {
        if (event.when < now_) {
            event.when = now_;
            ++clamped_;
        }
        event.seq = ++sequence_;
        ++pushes_;
        if (event.when - now_ < span_) {
            wheel_[static_cast<size_t>(static_cast<uint64_t>(event.when) & mask_)].push_back(std::move(event));
            return;
        }
        overflow_.push(std::move(event));
    }

    bool empty() const { return ready_.empty() && earliestWheel() == kInfiniteTick && overflow_.empty(); }

    Tick nextWhen() const {
        if (!ready_.empty()) return ready_.front().when;
        const Tick wheel = earliestWheel();
        const Tick heap = overflow_.empty() ? kInfiniteTick : overflow_.top().when;
        return wheel < heap ? wheel : heap;
    }

    /* The earliest event; `empty()` must be false. */
    Event pop() {
        if (ready_.empty()) fill();
        Event event = std::move(ready_.front());
        ready_.erase(ready_.begin());
        now_ = event.when;
        return event;
    }

    /* Times still on the books, for tests and diagnostics. */
    size_t size() const {
        size_t total = ready_.size() + overflow_.size();
        for (const std::vector<Event>& bucket : wheel_) total += bucket.size();
        return total;
    }

private:
    struct LaterFirst {
        bool operator()(const Event& left, const Event& right) const { return eventLess(right, left); }
    };

    Tick earliestWheel() const {
        const uint64_t base = static_cast<uint64_t>(now_) & mask_;
        for (Tick offset = 0; offset < span_; ++offset) {
            const uint64_t index = (base + static_cast<uint64_t>(offset)) & mask_;
            if (!wheel_[static_cast<size_t>(index)].empty()) return now_ + offset;
        }
        return kInfiniteTick;
    }
    void fill() {
        const Tick when = nextWhen();
        ready_.clear();
        std::vector<Event>& bucket = wheel_[static_cast<size_t>(static_cast<uint64_t>(when) & mask_)];
        if (!bucket.empty()) {
            std::vector<Event> keep;
            keep.reserve(bucket.size());
            for (Event& event : bucket) {
                if (event.when == when) {
                    ready_.push_back(std::move(event));
                    continue;
                }
                /* Unreachable while every deadline is inside the wheel's span:
                   an entry four buckets ahead would have been pushed into the
                   heap instead.  Counted rather than dropped in silence. */
                ++dropped_;
                keep.push_back(std::move(event));
            }
            bucket.swap(keep);
        }
        while (!overflow_.empty() && overflow_.top().when == when) {
            ready_.push_back(overflow_.top());
            overflow_.pop();
        }
        std::stable_sort(ready_.begin(), ready_.end(), eventLess);
    }

    Tick span_ = 1024;
    uint64_t mask_ = 1023;
    Tick now_ = 0;
    uint64_t sequence_ = 0;
    uint64_t pushes_ = 0;
    uint64_t clamped_ = 0;
    uint64_t dropped_ = 0;
    std::vector<std::vector<Event>> wheel_;
    std::priority_queue<Event, std::vector<Event>, LaterFirst> overflow_;
    std::vector<Event> ready_;
};

}  // namespace tcsim
