#ifndef TC_SIMULATION_H
#define TC_SIMULATION_H

#include "tc_mod_api.h"
#include <stdint.h>

namespace tc {

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

    bool load(const TCHost* host) {
        if (host == nullptr) return false;
        submit = reinterpret_cast<TCSimSubmitFn>(
            host->resolve_symbol(host->context,
                                 "sim_do__modelZsimulationZcompile95thread_u3036"));
        get_cycle = reinterpret_cast<TCSimGetCycleFn>(
            host->resolve_symbol(host->context,
                                 "sim_get_cycle__modelZsimulationZcompile95thread_u3041"));
        settings = static_cast<void**>(
            host->resolve_symbol(host->context,
                                 "simulation_settings__modelZsimulator95types_u83"));
        get_setting = reinterpret_cast<TCSimGetSettingFn>(
            host->resolve_symbol(host->context,
                                 "get_command_setting__modelZsimulator95types_u124"));
        set_setting = reinterpret_cast<TCSimSetSettingFn>(
            host->resolve_symbol(host->context,
                                 "set_command_setting__modelZsimulator95types_u131"));
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

    int64_t cycle() const {
        return get_cycle ? get_cycle() : -1;
    }

    void submitCommand(void* model, uint8_t command, int64_t target) const {
        if (submit) submit(model, command, target);
    }

    void run(void* model, int64_t target) const {
        submitCommand(model, 0, target);
    }

    int64_t commandSetting(uint8_t key) const {
        return get_setting ? get_setting(key) : 0;
    }

    void setCommandSetting(uint8_t key, int64_t value) const {
        if (set_setting) set_setting(key, value);
    }
};

}  // namespace tc

#endif  // TC_SIMULATION_H
