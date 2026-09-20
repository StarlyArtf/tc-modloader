#ifndef TC_GAME_STATE_H
#define TC_GAME_STATE_H

#include "tc_game_model.h"
#include <stdint.h>
#include <string.h>

namespace tc {

struct TCGameStateModel {
    const uint8_t* is_campaign = nullptr;
    const void* level_progress = nullptr;
    const void* campaign_name = nullptr;
    const void* simulation_circuit_state = nullptr;
    const uint64_t* current_word_size = nullptr;
    const uint64_t* level_used_input = nullptr;
    const uint64_t* level_used_outputs = nullptr;
    const uint8_t* display_numbers_as = nullptr;
    const uint8_t* run_cycle_control_allowed = nullptr;

    bool load(const TCHost* host) {
        if (host == nullptr) return false;
        is_campaign = static_cast<const uint8_t*>(
            host->resolve_symbol(host->context, "is_campaign__modelZmodel95types_u739"));
        level_progress = host->resolve_symbol(
            host->context, "level_progress__modelZmodel95types_u835");
        campaign_name = host->resolve_symbol(
            host->context, "campaign_name__modelZmodel95types_u836");
        simulation_circuit_state = host->resolve_symbol(
            host->context, "simulation_circuit_state__modelZsimulator95types_u78");
        current_word_size = static_cast<const uint64_t*>(
            host->resolve_symbol(host->context,
                                 "current_word_size__modelZmodel95types_u741"));
        level_used_input = static_cast<const uint64_t*>(
            host->resolve_symbol(host->context,
                                 "level_used_input__modelZsimulationZcontroller_u3"));
        level_used_outputs = static_cast<const uint64_t*>(
            host->resolve_symbol(host->context,
                                 "level_used_outputs__modelZsimulationZcontroller_u4"));
        display_numbers_as = static_cast<const uint8_t*>(
            host->resolve_symbol(
                host->context,
                "display_numbers_as__presenterZutilitiesZhelper95functions_u100"));
        run_cycle_control_allowed = static_cast<const uint8_t*>(
            host->resolve_symbol(
                host->context,
                "is_run_cycle_control_allowed__presenterZutilities_u29533"));
        return is_campaign != nullptr;
    }

    bool valid() const {
        return is_campaign != nullptr;
    }

    bool isCampaign() const {
        return is_campaign != nullptr && *is_campaign != 0;
    }

    const void* levelProgress() const {
        return level_progress;
    }

    const void* campaignNamePtr() const {
        return campaign_name;
    }

    TCNimString campaignName() const {
        TCNimString value{};
        if (campaign_name) memcpy(&value, campaign_name, sizeof(value));
        return value;
    }

    const char* campaignNameCStr() const {
        TCNimString value = campaignName();
        return value.data ? static_cast<const char*>(value.data) + 8 : nullptr;
    }

    const void* simulationCircuitState() const {
        return simulation_circuit_state;
    }

    uint64_t currentWordSize() const {
        return current_word_size ? *current_word_size : 0;
    }

    uint64_t levelUsedInput() const {
        return level_used_input ? *level_used_input : 0;
    }

    uint64_t levelUsedOutputs() const {
        return level_used_outputs ? *level_used_outputs : 0;
    }

    uint8_t displayNumbersAs() const {
        return display_numbers_as ? *display_numbers_as : 0;
    }

    bool isRunCycleControlAllowed() const {
        return run_cycle_control_allowed && *run_cycle_control_allowed;
    }
};

}  // namespace tc

#endif  // TC_GAME_STATE_H
