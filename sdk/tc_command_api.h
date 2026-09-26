#ifndef TC_COMMAND_API_H
#define TC_COMMAND_API_H
#include <stdint.h>
#include "tc_handle_api.h"

#define TC_SERVICE_COMMANDS "tc.commands"
#define TC_COMMAND_API_VERSION_1 1u
#define TC_COMMAND_API_VERSION_2 2u

/* First stable command family.  `subject` must be the current Board handle.
   RUN uses `argument` as the target cycle; STOP and RESET ignore it. */
#define TC_COMMAND_SIM_RUN 1u
#define TC_COMMAND_SIM_STOP 2u
#define TC_COMMAND_SIM_RESET 3u
#define TC_COMMAND_BOARD_UNDO 4u
#define TC_COMMAND_BOARD_REDO 5u
/* Saves the current Board into the active profile's circuit.data. */
#define TC_COMMAND_SAVE 6u
/* Places a component on the current Board.  V2 carries the placement payload;
   the subject is still the Board handle and the call still goes through the
   queue, ownership and status machinery. */
#define TC_COMMAND_BOARD_PLACE_COMPONENT 7u
/* Duplicate a custom instance: `argument` is the source component's instance id,
   `custom_prototype_id` must name its type, and x/y/rotation are the destination.
   The placement is the game's own (one Ctrl+Z removes the copy); the copied
   configuration is written into the new record without an undo step of its own,
   so the duplication stays one player action.  The copy is bound on the next
   compile, where it starts from the source's configuration and hears
   TC_LOGIC_CLONE. */
#define TC_COMMAND_BOARD_DUPLICATE_COMPONENT 8u
/* There is deliberately no wire-placement command yet.  The build's only
   context-free entry (add_wire_from_pos) appends a zero-length wire that the
   game itself refuses to address by point, so a queued command could only leave
   half-wires behind; see docs/research/board-object-fields.md section 4. */

typedef struct TCCommandV1 {
    uint32_t size;
    uint32_t type;
    uint32_t flags;
    uint32_t reserved;
    TCGameHandle subject;
    int64_t argument;
} TCCommandV1;

#define TC_COMMAND_STATE_QUEUED 1u
#define TC_COMMAND_STATE_RUNNING 2u
#define TC_COMMAND_STATE_SUCCEEDED 3u
#define TC_COMMAND_STATE_FAILED 4u
#define TC_COMMAND_STATE_CANCELLED 5u

#define TC_COMMAND_OK 0
#define TC_COMMAND_ERR_UNAVAILABLE (-1)
#define TC_COMMAND_ERR_ARGUMENT (-2)
#define TC_COMMAND_ERR_THREAD (-3)
#define TC_COMMAND_ERR_CAPACITY (-4)
#define TC_COMMAND_ERR_NOT_FOUND (-5)
#define TC_COMMAND_ERR_STATE (-6)
#define TC_COMMAND_ERR_STALE (-7)
#define TC_COMMAND_ERR_EXECUTION (-8)

typedef struct TCCommandStatusV1 {
    uint32_t size;
    uint32_t version;
    uint32_t state;
    int32_t result;
    uint64_t request_id;
    int64_t submitted_frame;
    int64_t completed_frame;
} TCCommandStatusV1;

typedef struct TCCommandApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*submit)(void* context,const TCCommandV1* command,uint64_t* request_id);
    int (*get_status)(void* context,uint64_t request_id,TCCommandStatusV1* out,uint32_t out_size);
    int (*cancel)(void* context,uint64_t request_id);
} TCCommandApiV1;

/* V2 repeats the V1 prefix and appends the placement payload.  A command that
   needs no payload leaves those fields zero; V1 stays queryable and keeps its
   own table, so a Mod compiled against V1 is unaffected. */
typedef struct TCCommandV2 {
    uint32_t size;
    uint32_t type;
    uint32_t flags;
    uint32_t reserved;
    TCGameHandle subject;
    int64_t argument;
    uint64_t custom_prototype_id; /* PLACE_COMPONENT: 0 for a built-in kind */
    uint32_t kind;                /* PLACE_COMPONENT: built-in kind byte */
    uint32_t rotation;
    int32_t x;                    /* PLACE_COMPONENT: schematic grid position */
    int32_t y;
    uint32_t reserved2;
    uint32_t reserved3;
} TCCommandV2;

typedef struct TCCommandApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*submit)(void* context,const TCCommandV2* command,uint64_t* request_id);
    int (*get_status)(void* context,uint64_t request_id,TCCommandStatusV1* out,uint32_t out_size);
    int (*cancel)(void* context,uint64_t request_id);
} TCCommandApiV2;

#endif
