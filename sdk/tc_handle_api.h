#ifndef TC_HANDLE_API_H
#define TC_HANDLE_API_H
#include <stdint.h>

/* Stable game-object identity.  A handle is copied by value and may be kept,
   but it resolves only while its owning game object belongs to the current
   generation.  Never persist handles to disk. */
#define TC_GAME_OBJECT_BOARD 1u
#define TC_GAME_OBJECT_COMPONENT 2u /* reserved until snapshot enumeration */
#define TC_GAME_OBJECT_WIRE 3u      /* reserved until snapshot enumeration */
#define TC_GAME_OBJECT_LEVEL 4u     /* reserved */
typedef struct TCGameHandle {
    uint32_t size;
    uint32_t kind;
    uint64_t generation;
    uint64_t token;
} TCGameHandle;

#define TC_HANDLE_OK 0
#define TC_HANDLE_ERR_UNAVAILABLE (-1)
#define TC_HANDLE_ERR_ARGUMENT (-2)
#define TC_HANDLE_ERR_KIND (-3)
#define TC_HANDLE_ERR_STALE (-4)
#endif
