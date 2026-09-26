#ifndef TC_SIMULATION_H
#define TC_SIMULATION_H

#include "tc_mod_api.h"
#include "tc_service_api.h"

#include <stdint.h>

/* The simulation, through TC_SERVICE_SIMULATION.

   V1 is the read side (`state`, `read`); V2 adds the control side
   (`run_to`/`run_for`/`pause`/`reset`/`step`, `set_slice`) and a consistent
   multi-channel read (`sample`).  Everything goes through the loader, so a Mod
   never resolves a mangled game symbol of its own:

   ```cpp
   tc::simulation::Api sim{};
   if (!tc::simulation::table(host, &sim)) return 2;   // old loader: no control
   int64_t cycle = tc::simulation::cycle(sim);

   TCSimChannelV1 channels[2] = {};
   channels[0].size = sizeof(TCSimChannelV1); channels[0].version = TCSIM_CHANNEL_VERSION_1;
   channels[0].byte_offset = 0x40; channels[0].bits = 1;
   uint64_t values[2] = {};
   uint32_t stable = 0;
   tc::simulation::sample(sim, channels, 2, values, &cycle, &stable);   // retry while !stable

   tc::simulation::setSlice(sim, 1);        // one cycle per request: sample every cycle
   tc::simulation::runFor(sim, 64);         // walk it
   ```

   `run_*`/`pause`/`reset`/`step` are *requests*: they call the game's own
   sim_do entry, so they travel the sim.do hook chain exactly like the player's
   run button, and the simulation still runs on the game's simulation thread.
   `sample` is a plain read; `stable` says whether the cycle moved during it. */

namespace tc {
namespace simulation {

using Api = TCSimulationApiV2;
using Channel = TCSimChannelV1;

/* Queries the table.  Returns false when the loader predates V2, which is the
   signal to fall back to V1 reads (or to leave the simulation alone). */
inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_SIMULATION, TC_SIMULATION_API_VERSION_2,
                            &queried, sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) || queried.version != TC_SIMULATION_API_VERSION_2 ||
        !queried.context || !queried.get_state || !queried.read_value || !queried.cycle ||
        !queried.state_size || !queried.snapshot || !queried.run_to || !queried.run_for ||
        !queried.pause || !queried.reset || !queried.step || !queried.set_slice ||
        !queried.control)
        return false;
    *out = queried;
    return true;
}

/* The V1 table, for a Mod that only reads. */
inline bool tableV1(const TCHost* host, TCSimulationApiV1* out) {
    if (!host || !out || !host->query_service) return false;
    TCSimulationApiV1 queried{};
    if (host->query_service(host->context, TC_SERVICE_SIMULATION, TC_SIMULATION_API_VERSION_1,
                            &queried, sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) || queried.version != TC_SIMULATION_API_VERSION_1 ||
        !queried.context || !queried.get_state || !queried.read_value)
        return false;
    *out = queried;
    return true;
}

inline int state(const Api& api, TCSimulationStateV1* out) {
    return api.get_state ? api.get_state(api.context, out, sizeof(*out))
                         : TC_SIMULATION_ERR_UNAVAILABLE;
}
inline int read(const Api& api, uint64_t byteOffset, uint32_t bits, uint64_t* out) {
    return api.read_value ? api.read_value(api.context, byteOffset, bits, out)
                          : TC_SIMULATION_ERR_UNAVAILABLE;
}
/* -1 when the simulation has not started. */
inline int64_t cycle(const Api& api) {
    int64_t value = -1;
    if (api.cycle) api.cycle(api.context, &value);
    return value;
}
inline uint64_t stateSize(const Api& api) {
    uint64_t value = 0;
    if (api.state_size) api.state_size(api.context, &value);
    return value;
}
/* One read of every channel.  `out_cycle` and `out_stable` may be null; while
   `stable` is 0 the values straddled a step, so read again. */
inline int sample(const Api& api, const Channel* channels, uint32_t count, uint64_t* values,
                  int64_t* outCycle = nullptr, uint32_t* outStable = nullptr) {
    if (!api.snapshot) return TC_SIMULATION_ERR_UNAVAILABLE;
    return api.snapshot(api.context, channels, count, values, count, outCycle, outStable);
}
/* One channel, for the common case. */
inline int sampleOne(const Api& api, uint64_t byteOffset, uint32_t bits, uint64_t* out) {
    Channel channel{};
    channel.size = sizeof(Channel);
    channel.version = TCSIM_CHANNEL_VERSION_1;
    channel.byte_offset = byteOffset;
    channel.bits = bits;
    return sample(api, &channel, 1, out);
}

inline int runTo(const Api& api, int64_t targetCycle) {
    return api.run_to ? api.run_to(api.context, targetCycle) : TC_SIMULATION_ERR_UNAVAILABLE;
}
inline int runFor(const Api& api, int64_t cycles) {
    return api.run_for ? api.run_for(api.context, cycles) : TC_SIMULATION_ERR_UNAVAILABLE;
}
inline int pause(const Api& api) {
    return api.pause ? api.pause(api.context) : TC_SIMULATION_ERR_UNAVAILABLE;
}
inline int reset(const Api& api) {
    return api.reset ? api.reset(api.context) : TC_SIMULATION_ERR_UNAVAILABLE;
}
inline int step(const Api& api, uint32_t cycles) {
    return api.step ? api.step(api.context, cycles) : TC_SIMULATION_ERR_UNAVAILABLE;
}
/* Cycles per run request; 0 disables slicing.  A caller that samples every
   cycle sets 1 and re-issues its run until `control().clamped` stops growing. */
inline int setSlice(const Api& api, uint32_t cycles) {
    return api.set_slice ? api.set_slice(api.context, cycles)
                         : TC_SIMULATION_ERR_UNAVAILABLE;
}
inline int control(const Api& api, TCSimulationControlV1* out) {
    if (!api.control || !out) return TC_SIMULATION_ERR_UNAVAILABLE;
    return api.control(api.context, out, sizeof(*out));
}

}  // namespace simulation

/* ---------------------------------------------------------------------------
   The raw model, for the few things the service table does not cover: the
   game's command-settings block and the replay buffers the waveform Sampler
   scans.  Function addresses come from the alias profile; only the buffers -
   which have no alias yet - are still looked up by name. */
using TCSimSubmitFn = void (*)(void* model, uint8_t command, int64_t target);
using TCSimGetCycleFn = int64_t (*)();
using TCSimGetSettingFn = int64_t (*)(uint8_t key);
using TCSimSetSettingFn = void (*)(uint8_t key, int64_t value);

struct TCSimulationModel {
    TCSimSubmitFn submit = nullptr;
    TCSimGetCycleFn get_cycle = nullptr;
    void** settings = nullptr;
    TCSimGetSettingFn get_setting = nullptr;
    TCSimSetSettingFn set_setting = nullptr;
    const void* input_replay = nullptr;
    const void* output_history_pins = nullptr;
    const void* keyboard_character = nullptr;
    const void* keyboard_coordinate = nullptr;

    bool load(const TCHost* host) {
        if (host == nullptr) return false;
        /* Everything with an alias goes through the alias profile, so a new game
           build is one edit in src/symbol_profile.hpp; the measured name stays
           as the fallback for a host that does not publish aliases. */
        const auto resolve=[&](const char* alias,const char* name)->void*{
            if(host->resolve_alias){
                void* found=host->resolve_alias(host->context,alias);
                if(found)return found;
            }
            return host->resolve_symbol?host->resolve_symbol(host->context,name):nullptr;
        };
        submit = reinterpret_cast<TCSimSubmitFn>(
            resolve("sim.do", "sim_do__modelZsimulationZcompile95thread_u3036"));
        get_cycle = reinterpret_cast<TCSimGetCycleFn>(
            resolve("sim.cycle", "sim_get_cycle__modelZsimulationZcompile95thread_u3041"));
        settings = static_cast<void**>(
            resolve("sim.settings", "simulation_settings__modelZsimulator95types_u83"));
        get_setting = reinterpret_cast<TCSimGetSettingFn>(
            resolve("sim.setting.get", "get_command_setting__modelZsimulator95types_u124"));
        set_setting = reinterpret_cast<TCSimSetSettingFn>(
            resolve("sim.setting.set", "set_command_setting__modelZsimulator95types_u131"));
        /* The replay buffers are data with no alias yet; keep the measured
           names, and note that a new game build moves them. */
        input_replay = resolve(nullptr, "simulation_input_replay__modelZsimulator95types_u84");
        output_history_pins = resolve(nullptr, "simulation_output_history_pins__modelZsimulator95types_u85");
        keyboard_character = resolve(nullptr, "simulation_keyboard_character__modelZsimulator95types_u88");
        keyboard_coordinate = resolve(nullptr, "simulation_keyboard_coordinate__modelZsimulator95types_u89");
        return valid();
    }

    bool valid() const {
        return submit != nullptr && get_cycle != nullptr &&
               settings != nullptr && get_setting != nullptr &&
               set_setting != nullptr;
    }

    bool settingsReady() const {
        return settings != nullptr && *settings != nullptr;
    }

    void* settingsValue() const {
        return settingsReady() ? *settings : nullptr;
    }

    int64_t cycle() const {
        return get_cycle ? get_cycle() : -1;
    }

    void submitCommand(void* model, uint8_t command, int64_t target) const {
        if (submit) submit(model, command, target);
    }

    void run(void* model, int64_t target) const {
        submitCommand(model, 0, target);
    }

    void pause(void* model) const {
        submitCommand(model, 1, 0);
    }

    void reset(void* model) const {
        submitCommand(model, 2, -1);
    }

    int64_t commandSetting(uint8_t key) const {
        return get_setting ? get_setting(key) : 0;
    }

    void setCommandSetting(uint8_t key, int64_t value) const {
        if (set_setting) set_setting(key, value);
    }

    const void* inputReplay() const { return input_replay; }
    const void* outputHistoryPins() const { return output_history_pins; }
    const void* keyboardCharacter() const { return keyboard_character; }
    const void* keyboardCoordinate() const { return keyboard_coordinate; }
};

}  // namespace tc

#endif  // TC_SIMULATION_H
