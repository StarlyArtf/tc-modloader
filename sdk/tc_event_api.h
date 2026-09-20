#ifndef TC_EVENT_API_H
#define TC_EVENT_API_H
#include <stdint.h>

/* Host event bus, version 1 (loader 0.6.0).

   Some facts about the running game are things every interesting mod ends up
   looking for, and each of them used to mean "hook the right internal function
   yourself": a level was loaded, the scene changed, the player asked the
   simulation to run or reset, the game saved.  The loader watches those points
   itself (and shares the hook chains where several plugins want the same one),
   then hands out typed events.

   Rules for a listener:

   * It runs on the thread the event came from: the simulation thread for
     TC_EVENT_SIM_COMMAND, the game's UI/loading thread for the others.  Do not
     call UI APIs from a simulation-thread event; copy what you need and act on
     it in on_frame.
   * Never throw and never keep the event pointer or `name`.
   * Registration happens during tc_mod_load.  A rejected plugin is dropped from
     the bus like every other registration.
   * `kinds` is a bitmask: one listener can ask for several kinds, and `flags`
     tells it which one arrived, so a single callback can handle them all. */

/* The kinds are bits: `kinds` is a mask, `TCEvent.kind` is the single bit that
   arrived, so one listener can handle several kinds and switch on `kind`. */
#define TC_EVENT_LEVEL_LOAD   (1u<<0)  /* flags: 0; subject: board model; name: level name */
#define TC_EVENT_SCENE_CHANGE (1u<<1)  /* flags: the scene being switched to; subject: the context
                                          the game passes to change_scene (borrowed - hand it straight
                                          back to change_scene, do not keep it) */
#define TC_EVENT_SIM_COMMAND  (1u<<2)  /* flags: the TCHookSimDoArgs.command value (0/1/2) */
#define TC_EVENT_SAVE         (1u<<3)  /* flags: the game's save counter after the save */

typedef struct TCEvent {
    uint32_t size;
    uint32_t kind;
    uint32_t flags;
    uint32_t reserved;
    /* Simulation cycle when the event was raised, or -1 when it is not known. */
    int64_t cycle;
    /* Kind-specific subject: the board model for TC_EVENT_LEVEL_LOAD, the
       change_scene context for TC_EVENT_SCENE_CHANGE.  Never persist either. */
    void* subject;
    /* Level name for TC_EVENT_LEVEL_LOAD, null otherwise.  Borrowed for the call. */
    const char* name;
    /* The user pointer given by the plugin that registered this listener. */
    void* user;
} TCEvent;

typedef void (*TCEventCallback)(TCEvent* event);

#define TC_EVENT_OK 0
#define TC_EVENT_ERR_UNAVAILABLE (-1)  /* older loader / called outside tc_mod_load */
#define TC_EVENT_ERR_ARGUMENT (-2)     /* null callback or empty kind mask */
#define TC_EVENT_ERR_CAPACITY (-3)     /* too many listeners */

#endif  /* TC_EVENT_API_H */
