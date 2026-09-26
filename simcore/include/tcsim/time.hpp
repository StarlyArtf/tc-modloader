#pragma once
/* L1 of the sandbox simulator (docs/PLAN-sandbox-simulator.md section 4.2):
   time is a 64-bit integer count of ticks, with one game unit equal to 1024
   ticks until the game's own constant is wired in.  A cycle is just a
   conversion point: the engine itself has no notion of one. */

#include <cstdint>

namespace tcsim {

using Tick = int64_t;

inline constexpr Tick kTicksPerUnit = 1024;          /* TC_SIM_TICKS_PER_UNIT */
inline constexpr Tick kDefaultCycleUnits = 8;        /* the game's K */
inline constexpr Tick kInfiniteTick = (static_cast<Tick>(1) << 62);

inline constexpr Tick ticksForUnits(int64_t units, Tick ticks_per_unit = kTicksPerUnit) {
    return units * ticks_per_unit;
}
inline constexpr int64_t unitsForTicks(Tick ticks, Tick ticks_per_unit = kTicksPerUnit) {
    return ticks_per_unit > 0 ? ticks / ticks_per_unit : 0;
}
/* Rounds up, so a delay that is not a whole number of units still occupies at
   least one unit in a per-unit report. */
inline constexpr int64_t unitsCeil(Tick ticks, Tick ticks_per_unit = kTicksPerUnit) {
    if (ticks_per_unit <= 0) return 0;
    return (ticks + ticks_per_unit - 1) / ticks_per_unit;
}

struct TimeBase {
    Tick ticks_per_unit = kTicksPerUnit;
    Tick cycle_units = kDefaultCycleUnits;
    Tick ticksPerCycle() const { return ticks_per_unit * cycle_units; }
    Tick cyclesForTicks(Tick ticks) const { return ticks_per_unit > 0 ? ticks / ticks_per_unit : 0; }
    Tick nextCycleBoundary(Tick ticks) const {
        const Tick cycle = ticksPerCycle();
        if (cycle <= 0) return ticks;
        const Tick remainder = ((ticks % cycle) + cycle) % cycle;
        return remainder == 0 ? ticks : ticks + (cycle - remainder);
    }
};

}  // namespace tcsim
