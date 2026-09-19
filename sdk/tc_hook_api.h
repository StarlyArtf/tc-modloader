#ifndef TC_HOOK_API_H
#define TC_HOOK_API_H
#include <stdint.h>

/* Hook chain ABI, version 1 (loader 0.6.0).

   A hook point is a game function whose signature the loader knows and has
   measured for this build (the catalogue lives in src/symbol_profile.hpp).  Any
   number of plugins may join the same point: the loader owns the one detour on
   the game's function and calls the plugins in a defined order, so two mods that
   both want to watch sim_do no longer fight over the target.

   Rules for a callback (it runs on whichever thread the game called the hook
   point from - the simulation thread for sim.do):

   * Read and write the typed argument struct through call->args.  Changing a
     field changes what the game's own function receives.
   * Return 0 to let the next plugin in the chain run.
   * Return non-zero to stop the chain: no later plugin runs.  Whether the game's
     own function still runs is a separate decision - set call->skip_original to
     1 to suppress it.
   * To run the rest of the chain (and then the game's own function) and still do
     something afterwards - for example to look at a return value - call
     call->run_chain(call) and return non-zero.  The game's function never runs
     twice, no matter how many links ask for it.
   * Never throw across the boundary and never keep the argument pointer.

   Callbacks are registered during tc_mod_load.  A plugin that is rejected, or
   whose initialization fails, leaves no link behind. */

#define TC_HOOK_SIM_DO 1u
/* void(void* model, uint8_t command, int64_t target)
   command: 0 run, 1 refresh/stop, 2 mode_reset.  target is the cycle to run to
   (INT64_MAX means "run continuously"), and may be negative for reset. */
typedef struct TCHookSimDoArgs {
    uint32_t size;
    uint32_t command;
    void* model;
    int64_t target;
} TCHookSimDoArgs;

#define TC_HOOK_LEVEL_LOAD 2u
/* void(void* boardModel, const TCNimString* name)
   Called once when the game loads a level, with the board model a plugin needs
   to read the wire table. */
typedef struct TCHookLevelLoadArgs {
    uint32_t size;
    uint32_t reserved;
    void* board_model;
    const void* name;
} TCHookLevelLoadArgs;

/* Only the points whose signature has been measured for this build are
   catalogued here.  A plugin can still create_hook any other EXE symbol it has
   verified itself; the catalogue is what the loader can promise.  Promoting a
   new point means adding a typed argument struct, an id, and a detour in the
   loader (see docs/reference/symbols.md). */

typedef struct TCHookCall TCHookCall;
struct TCHookCall {
    uint32_t size;
    uint32_t hook_id;
    /* Simulation cycle when the call was made, or -1 when it is not known. */
    int64_t cycle;
    /* The user pointer given by the plugin that registered this callback. */
    void* user;
    /* The typed argument struct for this hook point (see TC_HOOK_* above). */
    void* args;
    /* Set to 1 to keep the game's own function from running. */
    int32_t skip_original;
    int32_t reserved;
    /* Runs the remaining links and then the game's own function, once.  Returns
       0 when it ran them, 1 when the call had already been consumed. */
    int32_t (*run_chain)(TCHookCall* call);
    /* Loader-private state; plugins must not touch it. */
    void* loader_state;
};

/* Return 0 to continue the chain, non-zero to stop after this callback. */
typedef int (*TCHookCallback)(TCHookCall* call);

/* Registration result codes, shared by every hook-chain entry point. */
#define TC_HOOK_OK 0
#define TC_HOOK_ERR_UNAVAILABLE (-1)  /* older loader / called outside tc_mod_load */
#define TC_HOOK_ERR_ID (-2)           /* unknown hook id for this build */
#define TC_HOOK_ERR_ARGUMENT (-3)     /* null callback, bad priority field, ... */
#define TC_HOOK_ERR_CAPACITY (-4)     /* too many links for this hook point */
#define TC_HOOK_ERR_TARGET (-5)       /* the game function could not be resolved */

#endif  /* TC_HOOK_API_H */
