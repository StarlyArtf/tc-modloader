#pragma once
/* L3's timing half (docs/PLAN-sandbox-simulator.md section 4.4): delay lives on
   a timing arc (input pin -> output pin), carries a different number for a
   rising and a falling transition, and is inertial by default.

   The default matters: iverilog's `assign #d` and primitive delays are
   inertial, so a pulse narrower than the delay never reaches the output.  The
   plan keeps transport delay as an explicit second mode for the lessons that
   want to watch a glitch travel; the cross-check can only cover the inertial
   one, because iverilog has no transport mode for gate primitives. */

#include "value.hpp"
#include "time.hpp"

#include <cstdint>
#include <vector>

namespace tcsim {

enum class DelayKind : uint8_t { kInertial = 0, kTransport = 1 };

struct ArcDelay {
    Tick tplh = 0;  /* low -> high */
    Tick tphl = 0;  /* high -> low  */

    /* A transition to an unknown value has one number on both edges, so it
       takes the slower of the two - never the faster one, or an X would
       appear before the hardware could have reached it. */
    Tick forTransition(Logic to) const {
        if (to == Logic::kOne) return tplh;
        if (to == Logic::kZero) return tphl;
        return tplh > tphl ? tplh : tphl;
    }
    bool uniform() const { return tplh == tphl; }
};

struct Arc {
    uint16_t input = 0;   /* index into DeviceSpec::inputs */
    uint16_t output = 0;  /* index into DeviceSpec::outputs */
    ArcDelay delay{};
};

/* A timing library is data (section 4.4): arc -> delay, per device type, with
   the three corners a real library offers.  Nothing loads a file yet - S5 owns
   the file format - but the engine already reads delays from here, so a
   library can be attached without touching the simulator. */
enum class DelayCorner : uint8_t { kMin = 0, kTyp = 1, kMax = 2 };

struct TimingEntry {
    std::string device;
    uint16_t input = 0;
    uint16_t output = 0;
    ArcDelay min{};
    ArcDelay typ{};
    ArcDelay max{};
};

class TimingLibrary {
public:
    void add(const TimingEntry& entry) { entries_.push_back(entry); }
    void clear() { entries_.clear(); }
    size_t size() const { return entries_.size(); }
    DelayCorner corner = DelayCorner::kTyp;
    /* The arc's delay, or `fallback` when the library says nothing about it -
       a board with no library attached keeps working per-device constants. */
    ArcDelay delayFor(const std::string& device, uint16_t input, uint16_t output,
                      const ArcDelay& fallback) const {
        for (const TimingEntry& entry : entries_) {
            if (entry.device != device || entry.input != input || entry.output != output) continue;
            switch (corner) {
                case DelayCorner::kMin: return entry.min;
                case DelayCorner::kMax: return entry.max;
                case DelayCorner::kTyp: break;
            }
            return entry.typ;
        }
        return fallback;
    }

private:
    std::vector<TimingEntry> entries_;
};

}  // namespace tcsim
