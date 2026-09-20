#ifndef TC_HOOK_H
#define TC_HOOK_H

/* Typed helpers for the loader-owned hook chains (see tc_hook_api.h for the
   contract and the argument structs).

   Before 0.6.0 a plugin that wanted sim_do resolved the mangled Nim name and
   hooked it itself, and a second plugin wanting the same function was rejected.
   Now both plugins join the loader's chain, which also means the detour keeps
   working when the game build is updated and the profile is re-pointed. */

#include "tc_mod_api.h"

namespace tc {
namespace hook {

inline bool available(const TCHost* host) {
    return hostHas(host, TC_CAP_HOOK_CHAIN);
}

/* priority: lower runs first; ties are broken by mod id, then by registration
   order, so a scene replays the same way every time.  Returns TC_HOOK_OK or a
   TC_HOOK_ERR_* code (TC_HOOK_ERR_TARGET means this game build does not expose
   the hook point - refuse to load rather than run half a mod). */
inline int add(const TCHost* host, uint32_t hook_id, int32_t priority, TCHookCallback callback,
               void* user) {
    if (!host || !callback) return TC_HOOK_ERR_ARGUMENT;
    if (!hostHasField(host, offsetof(TCHost, register_hook_chain), sizeof(host->register_hook_chain)) ||
        !host->register_hook_chain)
        return TC_HOOK_ERR_UNAVAILABLE;
    return host->register_hook_chain(host->context, hook_id, priority, callback, user);
}

inline int addSimDo(const TCHost* host, int32_t priority, TCHookCallback callback, void* user) {
    return add(host, TC_HOOK_SIM_DO, priority, callback, user);
}
inline int addLevelLoad(const TCHost* host, int32_t priority, TCHookCallback callback, void* user) {
    return add(host, TC_HOOK_LEVEL_LOAD, priority, callback, user);
}
/* The game's own function for a hook point, without removing the chain: calling
   it goes through the chain like any other caller would.  Handy when a plugin
   wants to reuse the game's behaviour (the cycle-guard toolbar does this to
   request a run).  Null when the point is unavailable. */
inline void* callPoint(const TCHost* host, uint32_t hook_id) {
    const char* alias = hook_id == TC_HOOK_SIM_DO      ? "sim.do"
                        : hook_id == TC_HOOK_LEVEL_LOAD ? "level.load"
                                                        : nullptr;
    return alias ? resolveAlias(host, alias) : nullptr;
}

/* Typed views of call->args; a null result means the call came from a different
   hook point than the callback expected. */
inline TCHookSimDoArgs* simDoArgs(TCHookCall* call) {
    return call && call->hook_id == TC_HOOK_SIM_DO ? static_cast<TCHookSimDoArgs*>(call->args) : nullptr;
}
inline TCHookLevelLoadArgs* levelLoadArgs(TCHookCall* call) {
    return call && call->hook_id == TC_HOOK_LEVEL_LOAD ? static_cast<TCHookLevelLoadArgs*>(call->args)
                                                      : nullptr;
}
/* Runs the rest of the chain and then the game's own function, returning true if
   the call had already been consumed.  Only useful when a link wants to look at
   a return value (board.wire.update) or run code after the game's own handler. */
inline bool runRest(TCHookCall* call) {
    return call && call->run_chain ? call->run_chain(call) != 0 : true;
}

/* Human-readable form of a TC_HOOK_ERR_* code, for log lines. */
inline const char* errorText(int status) {
    switch (status) {
        case TC_HOOK_OK: return "ok";
        case TC_HOOK_ERR_UNAVAILABLE: return "hook chains unavailable (older loader or outside tc_mod_load)";
        case TC_HOOK_ERR_ID: return "unknown hook id";
        case TC_HOOK_ERR_ARGUMENT: return "invalid argument";
        case TC_HOOK_ERR_CAPACITY: return "too many links for this hook point";
        case TC_HOOK_ERR_TARGET: return "hook point not available in this game build";
        default: return "unknown error";
    }
}

}  // namespace hook
}  // namespace tc

#endif  // TC_HOOK_H
