#pragma once
/* The simulation control side of TC_SERVICE_SIMULATION V2.

   Three things live here and nowhere else:

   * the **model**: sim_do's first argument.  A Mod's run request has to travel
     the game's own sim.do hook chain (that is where cycle-guard and anything
     else watching runs already are), so the loader's own link remembers the
     model the game passes and the service hands it back;
   * the **slice**: "advance at most N cycles per request".  A caller that wants
     one sample per cycle sets slice = 1 and re-issues the run; the loader
     shortens the target instead of letting the game run to the end of it.  The
     arithmetic is a pure function so the offline test can pin it;
   * the **bookkeeping** a caller needs to tell "the run stopped early" from
     "the run was not mine": requests, how many were shortened, and the last
     requested/effective target.

   Only the loader's sim.do link (simulation thread) and the service calls
   (render thread) touch this, so the counters are atomics rather than a lock. */

#include "../sdk/tc_service_api.h"

#include <atomic>
#include <cstdint>

namespace tc::sim_control {

/* What a run request becomes under a slice.  `current` is sim.cycle, which is
   -1 before the first run; a request that is not a forward run (target < 0, or
   the pause/reset commands, which do not go through here) is passed through
   untouched.  Returns the target the game should be given and sets `clamped`
   when the slice shortened it. */
inline int64_t clampTarget(int64_t target,int64_t current,uint32_t slice,bool* clamped){
    if(clamped)*clamped=false;
    if(slice==0||target<0||current<0)return target;
    const int64_t limit=current>INT64_MAX-static_cast<int64_t>(slice)
                            ?INT64_MAX
                            :current+static_cast<int64_t>(slice);
    if(target<=limit)return target;
    if(clamped)*clamped=true;
    return limit;
}

class Store {
public:
    /* Called from the loader's sim.do link, on the simulation thread. */
    void noteSimDo(void* model,int64_t target,int64_t effective,int64_t current,
                   bool clamped){
        if(model)model_.store(model,std::memory_order_release);
        lastTarget_.store(target,std::memory_order_relaxed);
        lastEffective_.store(effective,std::memory_order_relaxed);
        lastCycle_.store(current,std::memory_order_relaxed);
        requests_.fetch_add(1,std::memory_order_relaxed);
        if(clamped)clamped_.fetch_add(1,std::memory_order_relaxed);
    }
    /* level.load and sim.do receive the same model object in the pinned game
       build.  Remember it as soon as a board is entered: a paused board may
       need a refresh before the player has issued the first run command, which
       is exactly when waiting for noteSimDo() would leave the service unusable. */
    void remember(void* model){
        if(model)model_.store(model,std::memory_order_release);
    }
    void* model() const{return model_.load(std::memory_order_acquire);}
    void forget(){model_.store(nullptr,std::memory_order_release);}

    uint32_t slice() const{return slice_.load(std::memory_order_relaxed);}
    void setSlice(uint32_t cycles){slice_.store(cycles,std::memory_order_relaxed);}

    void fill(TCSimulationControlV1* out) const{
        *out=TCSimulationControlV1{};
        out->size=sizeof(*out);
        out->version=TC_SIMULATION_CONTROL_VERSION_1;
        out->slice=slice();
        out->requests=requests_.load(std::memory_order_relaxed);
        out->clamped=clamped_.load(std::memory_order_relaxed);
        out->last_target=lastTarget_.load(std::memory_order_relaxed);
        out->last_effective=lastEffective_.load(std::memory_order_relaxed);
        out->last_cycle=lastCycle_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<void*> model_{nullptr};
    std::atomic<uint32_t> slice_{0};
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> clamped_{0};
    std::atomic<int64_t> lastTarget_{0};
    std::atomic<int64_t> lastEffective_{0};
    std::atomic<int64_t> lastCycle_{-1};
};

/* One store per process, like the other loader-owned stores. */
inline Store& store(){
    static Store value;
    return value;
}

}  // namespace tc::sim_control
