#pragma once
/* The loader-side view of the capability bits declared in sdk/tc_mod_api.h.

   loader_capabilities() is the single place that decides what this build
   advertises to plugins and what a package may ask for in mod.json.  A bit
   belongs there only when the matching entry points are really implemented:
   advertising a capability is a promise the loader has to keep. */
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include "../sdk/tc_mod_api.h"

namespace tc {

inline uint64_t loader_capabilities() {
    return TC_CAP_LOG | TC_CAP_SYMBOL | TC_CAP_HOOK | TC_CAP_LOGIC |
           TC_CAP_COMPONENT | TC_CAP_UI_PAGE | TC_CAP_UI_SLOT |
           TC_CAP_TEXTURE | TC_CAP_STATUS | TC_CAP_SYMBOL_ALIAS |
           TC_CAP_HOOK_CHAIN | TC_CAP_EVENTS | TC_CAP_GAME_HANDLES;
}

/* Stable names, also the spelling used by mod.json's "capabilities" list. */
struct CapabilityName {
    const char* name;
    uint64_t bit;
};

inline const std::vector<CapabilityName>& capability_table() {
    static const std::vector<CapabilityName> value{
        {"log", TC_CAP_LOG},
        {"symbol", TC_CAP_SYMBOL},
        {"hook", TC_CAP_HOOK},
        {"logic", TC_CAP_LOGIC},
        {"component", TC_CAP_COMPONENT},
        {"ui_page", TC_CAP_UI_PAGE},
        {"ui_slot", TC_CAP_UI_SLOT},
        {"texture", TC_CAP_TEXTURE},
        {"status", TC_CAP_STATUS},
        {"symbol_alias", TC_CAP_SYMBOL_ALIAS},
        {"hook_chain", TC_CAP_HOOK_CHAIN},
        {"events", TC_CAP_EVENTS},
        {"game_handles", TC_CAP_GAME_HANDLES},
    };
    return value;
}

inline uint64_t capability_bit(const std::string& name) {
    for (const auto& entry : capability_table())
        if (name == entry.name) return entry.bit;
    return 0;
}

/* "log, hook, ..." in table order; used by logs and the Mods page. */
inline std::string capability_names(uint64_t bits) {
    std::string out;
    for (const auto& entry : capability_table()) {
        if (!(bits & entry.bit)) continue;
        if (!out.empty()) out += ", ";
        out += entry.name;
    }
    return out;
}

}  // namespace tc
