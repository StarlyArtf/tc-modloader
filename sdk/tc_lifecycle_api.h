#ifndef TC_LIFECYCLE_API_H
#define TC_LIFECYCLE_API_H
#include <stdint.h>
#include "tc_handle_api.h"

#define TC_SERVICE_LIFECYCLE "tc.lifecycle"
#define TC_LIFECYCLE_API_VERSION_1 1u
#define TC_LIFECYCLE_EVENT_VERSION_1 1u

#define TC_LIFECYCLE_BOARD_ENTERED (1u << 0)
#define TC_LIFECYCLE_BOARD_LEFT (1u << 1)
#define TC_LIFECYCLE_OBJECTS_CHANGED (1u << 2)
#define TC_LIFECYCLE_SELECTION_CHANGED (1u << 3)

#define TC_LIFECYCLE_HAS_OBJECT_COUNTS (1u << 0)
#define TC_LIFECYCLE_HAS_SELECTION_COUNTS (1u << 1)

typedef struct TCGameLifecycleEventV1 {
    uint32_t size;
    uint32_t version;
    uint32_t kind;
    uint32_t flags;
    uint64_t sequence;
    int64_t engine_frame;
    int64_t simulation_cycle;
    TCGameHandle board;
    uint64_t component_count;
    uint64_t wire_count;
    uint64_t selected_component_count;
    uint64_t selected_wire_count;
    void* user;
} TCGameLifecycleEventV1;

typedef void (*TCGameLifecycleCallback)(TCGameLifecycleEventV1* event);

#define TC_LIFECYCLE_OK 0
#define TC_LIFECYCLE_ERR_UNAVAILABLE (-1)
#define TC_LIFECYCLE_ERR_ARGUMENT (-2)
#define TC_LIFECYCLE_ERR_CAPACITY (-3)

typedef struct TCGameLifecycleApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*subscribe)(void* context,uint32_t kinds,TCGameLifecycleCallback callback,void* user);
} TCGameLifecycleApiV1;

#endif
