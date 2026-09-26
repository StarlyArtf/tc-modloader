#pragma once
/* Per-cycle capture: the heart of a scope.

   The simulation steps inside generated code, so a per-cycle point can only
   come from the generated program itself.  The loader rewrites that source
   (the same hook the native-logic bridge uses) and inserts

       game_engine.'tc_scope_tick'(U64 cycle)

   immediately before every `cycle += 1` - that is, after the cycle's own work
   has been written and before the front end is told the cycle finished.  The
   tick therefore sees the *stable state of cycle N*, whoever is driving the
   run: the player's run button, a single step, or the level's own tests.

   Threads.  `tick` runs on the simulation thread, inside the generated loop,
   once per cycle, and must stay free when nothing is recording (one relaxed
   atomic load and a return).  Everything else - configure/start/stop/read/
   status - runs on the render thread.  The handover is a single ring buffer:
   the writer owns its cursor, the reader copies what is there, and rows the
   writer overwrote before the reader looked are not hidden - a cycle that did
   not simply advance by one is counted as a gap, which is exactly the number
   an oscilloscope must be able to show as zero. */

#include "../sdk/tc_service_api.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tc::scope_capture {

inline constexpr uint32_t kMaxChannels = 64;
inline constexpr uint32_t kMaxDepth = 1u << 20;
inline constexpr uint32_t kMaxBits = 64;

/* Reads one 64-bit word of the simulation state.  The loader installs the
   game's own sim_state_read_u64; the offline test installs a synthetic one. */
using StateReader = uint64_t (*)(uint64_t byte_offset);

struct Channel {
    uint64_t id = 0;
    uint64_t offset = 0;
    uint32_t bits = 1;
};

struct Trigger {
    uint32_t channel = 0;
    uint32_t edge = 0;   /* 0 none, 1 rising, 2 falling, 3 either, 4 match */
    uint64_t value = 0;
    uint64_t mask = ~uint64_t{0};
    uint32_t holdoff = 0;
};

class Store {
public:
    /* ---- render thread ---------------------------------------------------- */

    void setStateReader(StateReader reader) { reader_ = reader; }

    int configure(const TCSimChannelV1* channels, uint32_t count, uint32_t depth,
                  const TCCaptureTriggerV1* trigger) {
        if (!channels || count == 0) return TC_SIM_CAPTURE_ERR_ARGUMENT;
        if (count > kMaxChannels) return TC_SIM_CAPTURE_ERR_RANGE;
        if (depth == 0 || depth > kMaxDepth) return TC_SIM_CAPTURE_ERR_RANGE;
        recording_.store(false, std::memory_order_release);
        /* The writer is stopped, so the ring can be rebuilt in place. */
        count_ = 0;
        for (uint32_t index = 0; index < count; ++index) {
            const TCSimChannelV1& channel = channels[index];
            if (channel.size < sizeof(TCSimChannelV1) ||
                channel.version != TCSIM_CHANNEL_VERSION_1)
                return TC_SIM_CAPTURE_ERR_ARGUMENT;
            if (channel.bits == 0 || channel.bits > kMaxBits)
                return TC_SIM_CAPTURE_ERR_ARGUMENT;
            channels_[index] = {channel.channel_id, channel.byte_offset, channel.bits};
        }
        depth_ = depth;
        count_ = count;
        cycles_.assign(depth, 0);
        for (uint32_t index = 0; index < kMaxChannels; ++index)
            columns_[index].assign(index < count ? depth : 0, 0);
        trigger_ = Trigger{};
        if (trigger) {
            if (trigger->size < sizeof(TCCaptureTriggerV1) ||
                trigger->version != TCCAPTURE_TRIGGER_VERSION_1)
                return TC_SIM_CAPTURE_ERR_ARGUMENT;
            if (trigger->edge > TCCAPTURE_EDGE_MATCH) return TC_SIM_CAPTURE_ERR_ARGUMENT;
            if (trigger->edge != TCCAPTURE_EDGE_NONE && trigger->channel >= count)
                return TC_SIM_CAPTURE_ERR_RANGE;
            trigger_.channel = trigger->channel;
            trigger_.edge = trigger->edge;
            trigger_.value = trigger->value;
            trigger_.mask = trigger->mask ? trigger->mask : ~uint64_t{0};
            trigger_.holdoff = trigger->holdoff;
        }
        reset();
        return TC_SIM_CAPTURE_OK;
    }

    int start() {
        const uint32_t count = count_;
        if (count == 0) return TC_SIM_CAPTURE_ERR_STATE;
        reset();
        recording_.store(true, std::memory_order_release);
        return TC_SIM_CAPTURE_OK;
    }

    int stop() {
        recording_.store(false, std::memory_order_release);
        return TC_SIM_CAPTURE_OK;
    }

    /* Copies the rows currently held, oldest first.  `values` is row-major with
       `channelCount` entries per row; a caller that passes less room than the
       capture holds gets ERR_RANGE and the number it could take. */
    int read(uint64_t* cycles, uint64_t* values, uint32_t capacity, uint32_t* rows) const {
        if (!cycles || !values || !rows || capacity == 0) return TC_SIM_CAPTURE_ERR_ARGUMENT;
        const uint32_t count = count_;
        const uint32_t depth = depth_;
        if (count == 0 || depth == 0) return TC_SIM_CAPTURE_ERR_STATE;
        (void)writeSeq_.load(std::memory_order_acquire);
        const uint32_t held = held_;
        const uint32_t take = std::min(held, capacity);
        const uint32_t oldest = (write_ + depth - held) % depth;
        for (uint32_t row = 0; row < take; ++row) {
            const uint32_t slot = (oldest + row) % depth;
            cycles[row] = cycles_[slot];
            for (uint32_t channel = 0; channel < count; ++channel)
                values[static_cast<std::size_t>(row) * count + channel] = columns_[channel][slot];
        }
        *rows = take;
        return take == held ? TC_SIM_CAPTURE_OK : TC_SIM_CAPTURE_ERR_RANGE;
    }

    void fill(TCCaptureStatusV1* out) const {
        *out = TCCaptureStatusV1{};
        out->size = sizeof(*out);
        out->version = TCCAPTURE_STATUS_VERSION_1;
        out->channel_count = count_;
        out->depth = depth_;
        out->rows = held_;
        out->written = written_;
        out->first_cycle = held_ ? cycles_[(write_ + depth_ - held_) % depth_] : 0;
        out->last_cycle = held_ ? lastCycle_ : 0;
        out->gaps = gaps_;
        out->restarts = restarts_;
        out->trigger_cycle = triggered_ ? triggerCycle_ : 0;
        out->triggered = triggered_ ? 1u : 0u;
        out->recording = recording_.load(std::memory_order_relaxed) ? 1u : 0u;
        out->injected = injected_.load(std::memory_order_relaxed) ? 1u : 0u;
    }

    /* ---- compile time ----------------------------------------------------- */

    /* Inserts the tick before every per-cycle step.  Returns how many call
       sites were instrumented (0 leaves the source untouched, so a program
       without a cycle loop is never rewritten for this). */
    int inject(std::string& text) {
        static const std::string step = "cycle += 1";
        int inserted = 0;
        std::size_t at = 0;
        while ((at = text.find(step, at)) != std::string::npos) {
            const std::size_t lineStart = text.rfind('\n', at);
            const std::size_t from = lineStart == std::string::npos ? 0 : lineStart + 1;
            std::size_t indentEnd = from;
            while (indentEnd < text.size() &&
                   (text[indentEnd] == ' ' || text[indentEnd] == '\t'))
                ++indentEnd;
            const std::string indent = text.substr(from, indentEnd - from);
            const std::string call = indent + "game_engine.'tc_scope_tick'(U64 cycle)\n";
            /* Insert at the start of the line so the step keeps its own
               indentation and the injected call gets the same one. */
            text.insert(from, call);
            at = from + call.size() + (at - from) + step.size();
            ++inserted;
        }
        if (inserted) injected_.store(1, std::memory_order_relaxed);
        return inserted;
    }

    /* Called once per compile, on the compile thread: the count of call sites
       is worth one log line, so "the capture never sees a cycle" can be told
       apart from "the program was never instrumented". */
    void noteInjected(int sites) {
        injected_.store(sites > 0, std::memory_order_relaxed);
        injectedSites_ += static_cast<uint64_t>(sites);
    }
    uint64_t injectedSites() const { return injectedSites_; }

    /* ---- simulation thread ------------------------------------------------ */

    void tick(uint64_t cycle) {
        if (!recording_.load(std::memory_order_relaxed)) return;
        const uint32_t count = count_;
        const uint32_t depth = depth_;
        if (count == 0 || depth == 0) return;
        if (held_ > 0 && cycle == lastCycle_) {
            /* The same cycle reported again.  A compiled program carries more
               than one per-cycle call site (the burst loop and the single-step
               path), so one cycle can tick twice; treating that as a rewind
               would clear the ring on every cycle and leave a one-row window.
               Rewrite the row instead - the later report is the settled one. */
            write_ = (write_ + depth_ - 1) % depth_;
            /* Undo the bookkeeping the first report did, so the rewrite puts
               the window back exactly where it was. */
            if (held_ > 0) --held_;
            if (written_ > 0) --written_;
            writeRow(cycle);
            return;
        }
        if (held_ > 0 && cycle < lastCycle_) {
            /* The cycle went backwards: the level was reset or a new run
               started.  A capture of two runs spliced together is worse than no
               capture, so start over. */
            ++restarts_;
            resetRing();
        } else if (held_ > 0 && cycle > lastCycle_ + 1) {
            ++gaps_;
        }
        writeRow(cycle);
    }

private:
    /* One row, written on the simulation thread: the only place the state
       buffer is touched. */
    void writeRow(uint64_t cycle) {
        const uint32_t count = count_;
        const uint32_t depth = depth_;
        uint64_t previous = 0;
        for (uint32_t index = 0; index < count; ++index) {
            const Channel& channel = channels_[index];
            uint64_t value = reader_ ? reader_(channel.offset) : 0;
            if (channel.bits < kMaxBits)
                value &= (uint64_t{1} << channel.bits) - 1u;
            columns_[index][write_] = value;
            if (index == trigger_.channel) previous = previousValue_;
            if (index == trigger_.channel) previousValue_ = value;
        }
        cycles_[write_] = cycle;
        lastCycle_ = cycle;
        write_ = (write_ + 1) % depth;
        if (held_ < depth) ++held_;
        ++written_;
        writeSeq_.fetch_add(1, std::memory_order_release);
        if (!triggered_ && trigger_.edge != TCCAPTURE_EDGE_NONE) evaluateTrigger(previous, cycle);
    }

    void resetRing() {
        write_ = 0;
        held_ = 0;
        written_ = 0;
        gaps_ = 0;
        triggered_ = false;
        triggerCycle_ = 0;
        previousValue_ = 0;
        lastCycle_ = 0;
        writeSeq_.store(0, std::memory_order_release);
    }
    void reset() {
        resetRing();
        restarts_ = 0;
    }

    void evaluateTrigger(uint64_t previous, uint64_t cycle) {
        const Channel& channel = channels_[trigger_.channel];
        const uint64_t mask = channel.bits < kMaxBits
                                  ? ((uint64_t{1} << channel.bits) - 1u) & trigger_.mask
                                  : trigger_.mask;
        const uint64_t now = columns_[trigger_.channel][write_ == 0 ? depth_ - 1 : write_ - 1] & mask;
        const uint64_t before = previous & mask;
        bool hit = false;
        switch (trigger_.edge) {
            case TCCAPTURE_EDGE_RISING: hit = before == 0 && now != 0; break;
            case TCCAPTURE_EDGE_FALLING: hit = before != 0 && now == 0; break;
            case TCCAPTURE_EDGE_EITHER: hit = (before == 0) != (now == 0); break;
            case TCCAPTURE_EDGE_MATCH: hit = now == (trigger_.value & mask); break;
            default: break;
        }
        if (!hit) return;
        triggered_ = true;
        triggerCycle_ = cycle;
    }

    Channel channels_[kMaxChannels];
    std::vector<uint64_t> columns_[kMaxChannels];
    std::vector<uint64_t> cycles_;
    uint32_t count_ = 0;
    uint32_t depth_ = 0;
    Trigger trigger_{};
    StateReader reader_ = nullptr;
    std::atomic<bool> recording_{false};
    std::atomic<bool> injected_{false};
    std::atomic<uint64_t> writeSeq_{0};
    /* The ring: only the simulation thread writes these, the render thread
       only reads them after the sequence number. */
    uint32_t write_ = 0;
    uint32_t held_ = 0;
    uint64_t written_ = 0;
    uint64_t gaps_ = 0;
    uint64_t restarts_ = 0;
    uint64_t lastCycle_ = 0;
    uint64_t previousValue_ = 0;
    bool triggered_ = false;
    uint64_t triggerCycle_ = 0;
    uint64_t injectedSites_ = 0;
};

inline Store& store() {
    static Store value;
    return value;
}

}  // namespace tc::scope_capture
