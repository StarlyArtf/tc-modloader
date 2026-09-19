#ifndef TC_EVENT_H
#define TC_EVENT_H

/* Typed helpers for the host event bus (sdk/tc_event_api.h has the contract).

   The bus exists so that "tell me when a level is loaded" does not mean "hook the
   game's level loader yourself": the loader watches the point, shares it between
   plugins, and hands out a TCEvent. */

#include "tc_mod_api.h"

namespace tc {
namespace events {

inline bool available(const TCHost* host) {
    return hostHas(host, TC_CAP_EVENTS);
}

inline int subscribe(const TCHost* host, uint32_t kinds, TCEventCallback callback, void* user) {
    return addEventListener(host, kinds, callback, user);
}

/* Convenience wrappers, one per kind. */
inline int onLevelLoad(const TCHost* host, TCEventCallback callback, void* user) {
    return subscribe(host, TC_EVENT_LEVEL_LOAD, callback, user);
}
inline int onSceneChange(const TCHost* host, TCEventCallback callback, void* user) {
    return subscribe(host, TC_EVENT_SCENE_CHANGE, callback, user);
}
inline int onSimCommand(const TCHost* host, TCEventCallback callback, void* user) {
    return subscribe(host, TC_EVENT_SIM_COMMAND, callback, user);
}
inline int onSave(const TCHost* host, TCEventCallback callback, void* user) {
    return subscribe(host, TC_EVENT_SAVE, callback, user);
}

/* Kind check plus a typed view of the payload, so a listener that subscribes to
   several kinds can switch on one call. */
inline bool is(TCEvent* event, uint32_t kind) {
    return event && event->kind == kind;
}
inline void* levelBoardModel(TCEvent* event) {
    return is(event, TC_EVENT_LEVEL_LOAD) ? event->subject : nullptr;
}
inline const char* levelName(TCEvent* event) {
    return is(event, TC_EVENT_LEVEL_LOAD) ? event->name : nullptr;
}
/* The context the game passed to change_scene when this event was raised: the
   value to hand back to change_scene if the mod wants to leave the scene
   itself.  Borrowed for the call - copy nothing from it and keep no pointer. */
inline void* sceneContext(TCEvent* event) {
    return is(event, TC_EVENT_SCENE_CHANGE) ? event->subject : nullptr;
}
/* 0 run, 1 refresh/stop, 2 mode_reset (the same values as sim.do). */
inline int simCommand(TCEvent* event) {
    return is(event, TC_EVENT_SIM_COMMAND) ? static_cast<int>(event->flags) : -1;
}
inline int64_t saveCount(TCEvent* event) {
    return is(event, TC_EVENT_SAVE) ? static_cast<int64_t>(event->flags) : -1;
}

inline const char* kindName(uint32_t kind) {
    switch (kind) {
        case TC_EVENT_LEVEL_LOAD: return "level.load";
        case TC_EVENT_SCENE_CHANGE: return "scene.change";
        case TC_EVENT_SIM_COMMAND: return "sim.command";
        case TC_EVENT_SAVE: return "save";
        default: return "unknown";
    }
}

inline const char* errorText(int status) {
    switch (status) {
        case TC_EVENT_OK: return "ok";
        case TC_EVENT_ERR_UNAVAILABLE: return "event bus unavailable (older loader or outside tc_mod_load)";
        case TC_EVENT_ERR_ARGUMENT: return "invalid argument";
        case TC_EVENT_ERR_CAPACITY: return "too many listeners";
        default: return "unknown error";
    }
}

}  // namespace events
}  // namespace tc

#endif  // TC_EVENT_H
