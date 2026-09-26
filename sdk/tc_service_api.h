#ifndef TC_SERVICE_API_H
#define TC_SERVICE_API_H
#include <stdint.h>
#include "tc_handle_api.h"
#include "tc_logic_api.h"   /* TCLogicCallbackV2 in the tc.component.types table */

/* Versioned host services.  Service ids and table layouts are stable C ABI;
   each service evolves independently instead of growing TCHost forever. */
#define TC_SERVICE_BOARD "tc.board"
#define TC_BOARD_API_VERSION_1 1u
#define TC_BOARD_API_VERSION_2 2u
#define TC_BOARD_API_VERSION_3 3u
#define TC_BOARD_API_VERSION_4 4u
#define TC_BOARD_API_VERSION_5 5u
#define TC_BOARD_API_VERSION_6 6u
#define TC_BOARD_SNAPSHOT_VERSION_1 1u
#define TC_BOARD_OBJECT_SNAPSHOT_VERSION_1 1u
#define TC_COMPONENT_INFO_VERSION_1 1u
#define TC_WIRE_INFO_VERSION_1 1u
#define TC_COMPONENT_PINS_VERSION_1 1u
#define TC_WIRE_ENDS_VERSION_1 1u

#define TC_SERVICE_SIMULATION "tc.simulation"
#define TC_SIMULATION_API_VERSION_1 1u
#define TC_SIMULATION_API_VERSION_2 2u
#define TC_SIMULATION_STATE_VERSION_1 1u

#define TC_SERVICE_OK 0
#define TC_SERVICE_ERR_UNAVAILABLE (-1) /* unknown service / old loader */
#define TC_SERVICE_ERR_ARGUMENT (-2)    /* null id or output */
#define TC_SERVICE_ERR_VERSION (-3)     /* known service, unsupported version */
#define TC_SERVICE_ERR_SIZE (-4)        /* output table is too small */

typedef struct TCServiceHeader {
    uint32_t size;
    uint32_t version;
} TCServiceHeader;

/* Board service version 1 deliberately starts with identity/lifetime only.
   Snapshot reads are added to a later table version after their buffer and
   consistency contract is finalized. */
typedef struct TCBoardApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*get_current)(void* context,TCGameHandle* out);
    int (*validate)(void* context,const TCGameHandle* handle);
    int (*resolve)(void* context,const TCGameHandle* handle,const void** out);
} TCBoardApiV1;

/* A value-only view captured during one call on the game's main/render thread.
   It contains no borrowed game pointers, so the result remains safe to inspect
   after the call.  The embedded Board handle still has to be validated before
   it is used for another operation. */
#define TC_BOARD_SNAPSHOT_HAS_ENGINE_FRAME (1u << 0)
#define TC_BOARD_SNAPSHOT_HAS_SIMULATION_CYCLE (1u << 1)
#define TC_BOARD_SNAPSHOT_HAS_SELECTION (1u << 2)
#define TC_BOARD_SNAPSHOT_HAS_PREVIOUS_SELECTION (1u << 3)

#define TC_SNAPSHOT_OK 0
#define TC_SNAPSHOT_ERR_UNAVAILABLE (-1)
#define TC_SNAPSHOT_ERR_ARGUMENT (-2)
#define TC_SNAPSHOT_ERR_STALE (-3)
#define TC_SNAPSHOT_ERR_THREAD (-4)
#define TC_SNAPSHOT_ERR_SIZE (-5)
#define TC_SNAPSHOT_ERR_RETRY (-6)
#define TC_SNAPSHOT_ERR_CAPACITY (-7)

typedef struct TCBoardSnapshotV1 {
    uint32_t size;
    uint32_t version;
    TCGameHandle board;
    int64_t engine_frame;
    int64_t simulation_cycle;
    uint64_t selected_component_count;
    uint64_t selected_wire_count;
    uint64_t previous_selected_component_count;
    uint64_t previous_selected_wire_count;
    uint32_t flags;
    uint32_t reserved;
} TCBoardSnapshotV1;

/* Version 2 repeats the complete V1 prefix and adds a same-frame snapshot.
   Query it explicitly; querying V1 continues to return the smaller V1 table. */
typedef struct TCBoardApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*get_current)(void* context,TCGameHandle* out);
    int (*validate)(void* context,const TCGameHandle* handle);
    int (*resolve)(void* context,const TCGameHandle* handle,const void** out);
    int (*capture_snapshot)(void* context,const TCGameHandle* handle,
                            TCBoardSnapshotV1* out,uint32_t out_size);
} TCBoardApiV2;

/* Caller-owned buffers for the Board's complete Component/Wire handle set.
   A count-only call uses null pointers and zero capacities. */
typedef struct TCBoardObjectBuffersV1 {
    uint32_t size;
    uint32_t version;
    TCGameHandle* components;
    uint64_t component_capacity;
    TCGameHandle* wires;
    uint64_t wire_capacity;
} TCBoardObjectBuffersV1;

#define TC_BOARD_OBJECT_SNAPSHOT_HAS_COMPONENTS (1u << 0)
#define TC_BOARD_OBJECT_SNAPSHOT_HAS_WIRES (1u << 1)

typedef struct TCBoardObjectSnapshotV1 {
    uint32_t size;
    uint32_t version;
    TCGameHandle board;
    int64_t engine_frame;
    uint64_t snapshot_generation;
    uint64_t component_count;
    uint64_t wire_count;
    uint64_t component_written;
    uint64_t wire_written;
    uint32_t flags;
    uint32_t reserved;
} TCBoardObjectSnapshotV1;

/* V3 adds safe enumeration.  Child handles are valid only in the captured
   engine frame and become stale when the next frame starts or the Board dies. */
typedef struct TCBoardApiV3 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*get_current)(void* context,TCGameHandle* out);
    int (*validate)(void* context,const TCGameHandle* handle);
    int (*resolve)(void* context,const TCGameHandle* handle,const void** out);
    int (*capture_snapshot)(void* context,const TCGameHandle* handle,
                            TCBoardSnapshotV1* out,uint32_t out_size);
    int (*capture_objects)(void* context,const TCGameHandle* handle,
                           TCBoardObjectSnapshotV1* out,uint32_t out_size,
                           const TCBoardObjectBuffersV1* buffers);
} TCBoardApiV3;

/* What a Component/Wire handle from capture_objects actually is.  Every field
   below was read out of the pinned build's own structures and is documented in
   docs/sdk/services.md; fields whose meaning is still unverified stay out of
   the table instead of being guessed.

   Reads use the same error family as the snapshot calls because they share the
   same preconditions: TC_SNAPSHOT_OK, TC_SNAPSHOT_ERR_ARGUMENT (null, short
   buffer, or a handle of the other kind), TC_SNAPSHOT_ERR_STALE (handle or
   record no longer part of this Board), TC_SNAPSHOT_ERR_THREAD,
   TC_SNAPSHOT_ERR_UNAVAILABLE (object tables not readable) and
   TC_SNAPSHOT_ERR_RETRY (the Board changed while it was read). */
#define TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE (1u << 0)

typedef struct TCComponentInfoV1 {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t kind;              /* record kind byte; 0x4e is a custom instance */
    TCGameHandle handle;        /* echo of the handle that was read */
    uint64_t id;                /* the component's own 64-bit id on the board */
    int32_t x;                  /* schematic grid coordinates, not pixels */
    int32_t y;
    uint32_t rotation;          /* the record's direction/rotation byte */
    uint32_t reserved0;
    uint64_t custom_prototype_id; /* valid with HAS_CUSTOM_PROTOTYPE */
    uint64_t reserved1;
    uint64_t reserved2;
} TCComponentInfoV1;

#define TC_WIRE_INFO_HAS_ENDPOINT (1u << 0)
#define TC_WIRE_INFO_HAS_WIDTH (1u << 1)
#define TC_WIRE_INFO_HAS_STATE_SLOT (1u << 2)

typedef struct TCWireInfoV1 {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved0;
    TCGameHandle handle;        /* echo of the handle that was read */
    uint64_t id;                /* index in the wire sequence, the game's own wire id */
    int32_t x1;                 /* the wire's two endpoints, schematic grid coords */
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint32_t bit_width;         /* valid with HAS_WIDTH */
    uint32_t reserved1;
    uint64_t state_byte_offset; /* byte offset into the simulation state buffer */
    uint64_t reserved2;
} TCWireInfoV1;

/* V4 adds per-object reads on top of the V3 object snapshot.  A handle is still
   only valid in the engine frame that issued it, so read it in the same frame
   the enumeration happened in; the next frame returns STALE. */
typedef struct TCBoardApiV4 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*get_current)(void* context,TCGameHandle* out);
    int (*validate)(void* context,const TCGameHandle* handle);
    int (*resolve)(void* context,const TCGameHandle* handle,const void** out);
    int (*capture_snapshot)(void* context,const TCGameHandle* handle,
                            TCBoardSnapshotV1* out,uint32_t out_size);
    int (*capture_objects)(void* context,const TCGameHandle* handle,
                           TCBoardObjectSnapshotV1* out,uint32_t out_size,
                           const TCBoardObjectBuffersV1* buffers);
    int (*read_component)(void* context,const TCGameHandle* handle,
                          TCComponentInfoV1* out,uint32_t out_size);
    int (*read_wire)(void* context,const TCGameHandle* handle,
                     TCWireInfoV1* out,uint32_t out_size);
} TCBoardApiV4;

/* One entry per pin of a component's prototype, copied out of the game's own
   pin records.  `x`/`y` are relative to the component's position and `bits` is
   the declared width; a prototype may instead store AUTO_SIZE, in which case
   WIDTH_AUTO is set and the width follows the connected net. */
#define TC_PIN_INPUT 0u
#define TC_PIN_OUTPUT 1u
#define TC_PIN_INFO_HAS_POSITION (1u << 0)
#define TC_PIN_INFO_WIDTH_AUTO (1u << 1)

typedef struct TCPinInfoV1 {
    uint32_t size;
    uint32_t version;
    uint32_t direction;    /* TC_PIN_INPUT or TC_PIN_OUTPUT */
    uint32_t flags;
    int32_t x;
    int32_t y;
    uint32_t bits;         /* declared width when it is a number, else 0 */
    uint32_t reserved0;
    uint64_t word_size_raw; /* the untouched value the prototype stores */
} TCPinInfoV1;

#define TC_BOARD_PINS_HAS_PROTOTYPE (1u << 0)

typedef struct TCComponentPinsV1 {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t kind;              /* the component record's kind byte */
    TCGameHandle component;     /* echo of the handle that was read */
    uint64_t custom_prototype_id; /* valid with HAS_PROTOTYPE when kind is 0x4e */
    uint64_t input_count;
    uint64_t output_count;
    uint64_t pin_written;
    uint64_t reserved0;
} TCComponentPinsV1;

/* Caller-owned storage for the pin list, inputs first then outputs.  A
   count-only call passes a null pointer and zero capacity. */
typedef struct TCComponentPinBuffersV1 {
    uint32_t size;
    uint32_t version;
    uint32_t reserved0;
    uint32_t reserved1;
    TCPinInfoV1* pins;
    uint64_t pin_capacity;
} TCComponentPinBuffersV1;

/* V5 completes the object read: a handle is enough to learn what the component
   is and which pins it has, so a Mod no longer has to fetch a prototype itself.
   The host owns the prototype snapshot for the duration of the call and copies
   the pin values out, so nothing borrowed escapes. */
typedef struct TCBoardApiV5 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*get_current)(void* context,TCGameHandle* out);
    int (*validate)(void* context,const TCGameHandle* handle);
    int (*resolve)(void* context,const TCGameHandle* handle,const void** out);
    int (*capture_snapshot)(void* context,const TCGameHandle* handle,
                            TCBoardSnapshotV1* out,uint32_t out_size);
    int (*capture_objects)(void* context,const TCGameHandle* handle,
                           TCBoardObjectSnapshotV1* out,uint32_t out_size,
                           const TCBoardObjectBuffersV1* buffers);
    int (*read_component)(void* context,const TCGameHandle* handle,
                          TCComponentInfoV1* out,uint32_t out_size);
    int (*read_wire)(void* context,const TCGameHandle* handle,
                     TCWireInfoV1* out,uint32_t out_size);
    int (*read_component_pins)(void* context,const TCGameHandle* handle,
                               TCComponentPinsV1* out,uint32_t out_size,
                               const TCComponentPinBuffersV1* buffers);
} TCBoardApiV5;

/* Which pin of which component sits on one end of a wire.  The game connects
   geometrically: a pin's position on the grid is its component's position plus
   the pin's relative offset (what read_component_pins reports), and a wire end
   that lands on that point is the connection the player sees.  The campaign
   level's wire from (9,0) to (-9,0), the output pin at (10,0) whose input port
   is (-1,0) and the input pin at (-10,0) whose port is (1,0) are the measured
   example behind this rule (docs/sdk/services.md). */
#define TC_WIRE_END_HAS_COMPONENT (1u << 0)

typedef struct TCWireEndV1 {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t direction;     /* TC_PIN_INPUT / TC_PIN_OUTPUT of the pin found */
    uint32_t pin_index;     /* index within that direction */
    uint32_t reserved0;
    int32_t x;              /* the end's grid position, echoed */
    int32_t y;
    TCGameHandle component; /* valid with HAS_COMPONENT */
} TCWireEndV1;

typedef struct TCWireEndsV1 {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved0;
    TCGameHandle wire;      /* echo of the handle that was read */
    TCWireEndV1 ends[2];    /* ends[0] is (x1,y1), ends[1] is (x2,y2) */
} TCWireEndsV1;

/* V6 adds the connection query on top of the V5 prefix. */
typedef struct TCBoardApiV6 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*get_current)(void* context,TCGameHandle* out);
    int (*validate)(void* context,const TCGameHandle* handle);
    int (*resolve)(void* context,const TCGameHandle* handle,const void** out);
    int (*capture_snapshot)(void* context,const TCGameHandle* handle,
                            TCBoardSnapshotV1* out,uint32_t out_size);
    int (*capture_objects)(void* context,const TCGameHandle* handle,
                           TCBoardObjectSnapshotV1* out,uint32_t out_size,
                           const TCBoardObjectBuffersV1* buffers);
    int (*read_component)(void* context,const TCGameHandle* handle,
                          TCComponentInfoV1* out,uint32_t out_size);
    int (*read_wire)(void* context,const TCGameHandle* handle,
                     TCWireInfoV1* out,uint32_t out_size);
    int (*read_component_pins)(void* context,const TCGameHandle* handle,
                               TCComponentPinsV1* out,uint32_t out_size,
                               const TCComponentPinBuffersV1* buffers);
    int (*read_wire_ends)(void* context,const TCGameHandle* handle,
                          TCWireEndsV1* out,uint32_t out_size);
} TCBoardApiV6;

/* Simulation reads.  The values live in the game's own simulation state buffer;
   a wire's `state_byte_offset` (Board V4) is the address to read it at, and the
   wire's `bit_width` is how many low bits are meaningful.  The timing rule from
   the waveform work applies here too: the state is written when a step
   finishes, so a read in the frame a cycle changes may still show the previous
   cycle. */
#define TC_SIMULATION_STATE_HAS_CYCLE (1u << 0)
#define TC_SIMULATION_STATE_HAS_ENGINE_FRAME (1u << 1)
#define TC_SIMULATION_STATE_HAS_STATE_BUFFER (1u << 2)

typedef struct TCSimulationStateV1 {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved0;
    int64_t cycle;         /* the game's sim.cycle; -1 before a run started */
    int64_t engine_frame;  /* the loader's cached frame at this call */
    uint64_t state_size;   /* bytes in the simulation state buffer */
    uint64_t reserved1;
} TCSimulationStateV1;

#define TC_SIMULATION_OK 0
#define TC_SIMULATION_ERR_UNAVAILABLE (-1)
#define TC_SIMULATION_ERR_ARGUMENT (-2)
#define TC_SIMULATION_ERR_THREAD (-3)
#define TC_SIMULATION_ERR_RANGE (-4) /* offset/bits outside the state buffer */

typedef struct TCSimulationApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*get_state)(void* context,TCSimulationStateV1* out,uint32_t out_size);
    /* Reads `bits` (1..64) from the state buffer at `byte_offset`, mirroring the
       game's own sim_state_read_bits: read a word, keep the low bits. */
    int (*read_value)(void* context,uint64_t byte_offset,uint32_t bits,uint64_t* out);
} TCSimulationApiV1;

/* ---------------------------------------------------------------------------
   Simulation, V2: control and consistent reads.

   V1 could only read.  A Mod that wants to *show* a simulation (a scope, a
   value watcher, a level test driver) also has to say "run this far", "step",
   "stop", and has to read several signals that belong to the *same* cycle.  V2
   is that layer, and it is also where the loader owns the pieces that used to
   be spread over the symbol aliases, the command bus and each Mod's own
   bookkeeping.

   Threading (see docs/sdk/simulation.md for the full table): every entry may be
   called from the Mod frame callback, which is the game's render thread.
   `run_*`/`pause`/`reset`/`step` are *requests*: they hand the game's own
   sim_do the same work its UI would, so they travel the sim.do hook chain and
   the simulation itself still runs on the game's simulation thread.  `snapshot`
   is a plain read of the state buffer and says whether the cycle moved while it
   read (retry when `stable` is 0). */
#define TC_SIMULATION_ERR_STATE (-5) /* nothing to run yet: no board/simulation model */

/* One signal to read out of the simulation state buffer.  `channel_id` is the
   caller's own label for it and is echoed back, so a caller can keep its
   channels in a fixed order without owning the service. */
typedef struct TCSimChannelV1 {
    uint32_t size;
    uint32_t version;
    uint64_t channel_id;
    uint64_t byte_offset;
    uint32_t bits;      /* 1..64; 0 marks a channel that does not resolve */
    uint32_t reserved0;
} TCSimChannelV1;
#define TCSIM_CHANNEL_VERSION_1 1u

/* What the control side has been asked to do.  `slice` shortens a run request
   to at most that many cycles, which is how a caller samples every cycle
   without the loader rewriting the compiled program; `clamped` counts the
   requests that were shortened, so "the run stopped early" is visible instead
   of mysterious. */
typedef struct TCSimulationControlV1 {
    uint32_t size;
    uint32_t version;
    uint32_t slice;         /* cycles per run request; 0 = not sliced */
    uint32_t reserved0;
    uint64_t requests;      /* run requests forwarded to the game */
    uint64_t clamped;       /* requests whose target the slice shortened */
    int64_t last_target;    /* requested target of the last request */
    int64_t last_effective; /* target the game was actually given */
    int64_t last_cycle;     /* sim.cycle when the last request was made */
} TCSimulationControlV1;
#define TC_SIMULATION_CONTROL_VERSION_1 1u

typedef struct TCSimulationApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* V1 prefix, unchanged. */
    int (*get_state)(void* context,TCSimulationStateV1* out,uint32_t out_size);
    int (*read_value)(void* context,uint64_t byte_offset,uint32_t bits,uint64_t* out);
    /* V2. */
    int (*cycle)(void* context,int64_t* out);
    int (*state_size)(void* context,uint64_t* out);
    /* Reads every channel once.  `values` holds `count` entries; `out_cycle`
       and `out_stable` may be null.  `stable` is 1 when the cycle was the same
       before and after the read - when it is 0 the values straddle a step and
       the caller should read again. */
    int (*snapshot)(void* context,const TCSimChannelV1* channels,uint32_t count,
                    uint64_t* values,uint32_t values_capacity,int64_t* out_cycle,
                    uint32_t* out_stable);
    /* Requests, exactly as the game's own run/pause/reset buttons make them. */
    int (*run_to)(void* context,int64_t target_cycle);
    int (*run_for)(void* context,int64_t cycles);
    int (*pause)(void* context);
    int (*reset)(void* context);
    int (*step)(void* context,uint32_t cycles);
    /* 0 disables slicing.  Takes effect on the next run request. */
    int (*set_slice)(void* context,uint32_t cycles);
    int (*control)(void* context,TCSimulationControlV1* out,uint32_t out_size);
} TCSimulationApiV2;

/* ---------------------------------------------------------------------------
   tc.sim.channel: "which signal is this, in the simulation state buffer?"

   A wire's state byte offset and bit width live in the board's own wire record,
   which every Mod that wants to watch something used to dig out for itself -
   and which goes stale the moment the circuit is recompiled.  This service
   turns a wire handle into a TCSimChannelV1, and re-resolves channels by their
   endpoints afterwards. */
#define TC_SERVICE_SIM_CHANNEL "tc.sim.channel"
#define TC_SIM_CHANNEL_API_VERSION_1 1u
#define TC_SIM_CHANNEL_OK 0
#define TC_SIM_CHANNEL_ERR_UNAVAILABLE (-1)
#define TC_SIM_CHANNEL_ERR_ARGUMENT (-2)
#define TC_SIM_CHANNEL_ERR_HANDLE (-3) /* stale/foreign wire handle */
#define TC_SIM_CHANNEL_ERR_STATE (-4)  /* the wire carries no state slot yet */
#define TC_SIM_CHANNEL_ERR_THREAD (-5) /* not the game's own thread */

typedef struct TCSimWireChannelV1 {
    uint32_t size;
    uint32_t version;
    uint64_t channel_id;   /* the caller's own id, or the wire id when 0 */
    uint64_t byte_offset;
    uint32_t bits;         /* 1..64; 0 marks a wire that does not resolve */
    uint32_t reserved0;
    uint64_t wire_id;      /* the game's wire index, the identity to re-resolve by */
    int32_t x1;            /* the wire's endpoints, schematic grid coordinates */
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint64_t reserved1;
} TCSimWireChannelV1;
#define TCSIM_WIRE_CHANNEL_VERSION_1 1u

typedef struct TCSimChannelApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* A wire handle from a board object snapshot, in the frame that issued it. */
    int (*from_wire)(void* context,const TCGameHandle* wire,TCSimWireChannelV1* out,
                     uint32_t out_size);
    /* Re-resolves channels in place, by wire id first and by endpoints after a
       recompile moved the records; a channel that no longer resolves keeps its
       id and gets bits = 0.  `resolved` (may be null) counts the live ones. */
    int (*resolve)(void* context,const TCGameHandle* board,TCSimWireChannelV1* channels,
                   uint32_t count,uint32_t* resolved);
} TCSimChannelApiV1;

/* ---------------------------------------------------------------------------
   tc.sim.capture: every cycle, not every frame.

   The existing waveform Sampler reads on the render thread, so a fast run
   skips cycles.  This service is the other half: the loader instruments the
   compiled program with a per-cycle tick (see src/scope_capture.hpp), the tick
   writes the configured channels into a ring on the simulation thread, and a
   Mod reads that ring for drawing and export.

   The ring holds the most recent `depth` cycles - that *is* the pre-trigger
   window - and the trigger only marks the row it fired on, so a caller can draw
   "so many cycles before and after" without any extra buffering. */
#define TC_SERVICE_SIM_CAPTURE "tc.sim.capture"
#define TC_SIM_CAPTURE_API_VERSION_1 1u
#define TC_SIM_CAPTURE_OK 0
#define TC_SIM_CAPTURE_ERR_UNAVAILABLE (-1)
#define TC_SIM_CAPTURE_ERR_ARGUMENT (-2)
#define TC_SIM_CAPTURE_ERR_STATE (-3) /* nothing configured / not recording */
#define TC_SIM_CAPTURE_ERR_RANGE (-4) /* count or depth out of range */

#define TCCAPTURE_EDGE_NONE 0u
#define TCCAPTURE_EDGE_RISING 1u  /* 0 -> not 0 */
#define TCCAPTURE_EDGE_FALLING 2u /* not 0 -> 0 */
#define TCCAPTURE_EDGE_EITHER 3u
#define TCCAPTURE_EDGE_MATCH 4u /* (value & mask) == (sample & mask) */
#define TCCAPTURE_TRIGGER_VERSION_1 1u

typedef struct TCCaptureTriggerV1 {
    uint32_t size;
    uint32_t version;
    uint32_t channel; /* index into the configured channels */
    uint32_t edge;    /* TCCAPTURE_EDGE_* */
    uint64_t value;   /* EDGE_MATCH only */
    uint64_t mask;    /* EDGE_MATCH only; 0 means "all bits" */
    uint32_t holdoff; /* cycles ignored after a trigger */
    uint32_t reserved0;
} TCCaptureTriggerV1;

#define TCCAPTURE_STATUS_VERSION_1 1u

typedef struct TCCaptureStatusV1 {
    uint32_t size;
    uint32_t version;
    uint32_t channel_count;
    uint32_t depth;
    uint64_t rows;          /* rows held right now */
    uint64_t written;       /* rows written since the last reset */
    uint64_t first_cycle;   /* oldest row's cycle */
    uint64_t last_cycle;    /* newest row's cycle */
    uint64_t gaps;          /* times the cycle advanced by more than one */
    uint64_t restarts;      /* times the cycle went backwards (a new run) */
    uint64_t trigger_cycle; /* the cycle the trigger fired on, or 0 */
    uint32_t triggered;
    uint32_t recording;
    uint32_t injected; /* the running program carries the tick (see below) */
    uint32_t reserved0;
} TCCaptureStatusV1;

typedef struct TCCaptureApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Channels are TCSimChannelV1 as elsewhere; `depth` is how many cycles the
       ring keeps (1 .. 1048576) and `trigger` may be null for "record only". */
    int (*configure)(void* context,const TCSimChannelV1* channels,uint32_t count,
                     uint32_t depth,const TCCaptureTriggerV1* trigger);
    int (*start)(void* context);
    int (*stop)(void* context);
    /* Oldest row first.  `values` is row-major with `channel_count` entries per
       row; when the caller's room is smaller than what is held, this returns
       ERR_RANGE and `rows` says how many fit. */
    int (*read)(void* context,uint64_t* cycles,uint64_t* values,uint32_t capacity,
                uint32_t* rows);
    int (*status)(void* context,TCCaptureStatusV1* out,uint32_t out_size);
} TCCaptureApiV1;

/* IO values: the game's own value editors (a level's or a component's global
   inputs, and the constant component's value field) are ordinary game objects,
   and a Mod that wants to show or edit them should not have to re-derive how
   the game parses, truncates and stores them.  This service is that layer:

     * evaluate / format_value are the game's own number handling - the same
       code the native value field runs, so `0xFFFFFFFF^(1<<23)`, `~(1<<23)` and
       a plain decimal all behave exactly like typing them into the game.  The
       loader keeps a fallback parser/formatter for builds where those entry
       points are missing, so the calls keep working.
     * read_input / write_input / flip_input / input_width address a global
       input by its component index on the board - the same index the game's own
       IO panel passes around.  write_input truncates to the input's width, like
       the native editor; flip_input is the click path a native bit square uses.
     * write_constant_* is the wide-constant path: the value is stored in the
       component's setting, a runtime slot is updated and the running simulation
       is refreshed, so a click does not recompile the board.

   All of it runs on the game's main/render thread, the same rule as the Board
   and Simulation services: call it where the game builds UI or handles a click,
   never from a logic component's own thread. */
#define TC_SERVICE_IO_VALUE "tc.io_value"
#define TC_IO_VALUE_API_VERSION_1 1u

#define TC_IO_VALUE_OK 0
#define TC_IO_VALUE_ERR_UNAVAILABLE (-1) /* entry point or service missing */
#define TC_IO_VALUE_ERR_ARGUMENT (-2)    /* null/bad argument */
#define TC_IO_VALUE_ERR_SYNTAX (-3)      /* expression did not parse */
#define TC_IO_VALUE_ERR_RANGE (-4)       /* value/width/index out of range */
#define TC_IO_VALUE_ERR_STATE (-5)       /* no board, or wrong component kind */
#define TC_IO_VALUE_ERR_THREAD (-6)      /* called off the game's UI thread */
#define TC_IO_VALUE_ERR_SIZE (-7)        /* output buffer too small */
#define TC_IO_VALUE_ERR_INTERNAL (-8)    /* the game entry point failed */

/* Display styles of the game's own value fields. */
#define TC_IO_VALUE_FORMAT_BINARY 0u
#define TC_IO_VALUE_FORMAT_HEX 1u
#define TC_IO_VALUE_FORMAT_UNSIGNED 2u
#define TC_IO_VALUE_FORMAT_SIGNED 3u

typedef struct TCIoValueApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Parses one expression with the game's own evaluator (fallback: the
       loader's parser, same syntax subset).  `out` receives the low 64 bits. */
    int (*evaluate)(void* context,const char* expression,uint64_t* out);
    /* Formats a value the way the game's value fields do, into the caller's
       buffer (NUL terminated, truncated to fit when `out_size` is too small). */
    int (*format_value)(void* context,uint64_t value,uint32_t width,uint32_t format,
                        char* out,uint32_t out_size);
    /* Global input by component index on the board. */
    int (*read_input)(void* context,const TCGameHandle* board,uint64_t index,uint64_t* out);
    int (*write_input)(void* context,const TCGameHandle* board,uint64_t index,uint64_t value);
    int (*flip_input)(void* context,const TCGameHandle* board,uint64_t index,uint64_t bit);
    int (*input_width)(void* context,const TCGameHandle* board,uint64_t index,uint32_t* out);
    /* Wide constant: complete write path (setting + runtime slot + refresh). */
    int (*write_constant)(void* context,const TCGameHandle* board,uint64_t index,uint64_t value);
    /* Advanced: only the runtime slot, for a caller that manages the setting
       and the refresh itself.  `component_id` is the component's own 64-bit id. */
    int (*write_constant_slot)(void* context,uint64_t component_id,uint64_t value);
} TCIoValueApiV1;

/* Pin order: the game's left IO panel lists the board's input and output pins
   in the order its own cache happens to hold them and never lets the player
   change that.  This service exposes that order and lets a Mod replace it:

     * count / entry describe what the panel is showing right now - one entry
       per pin, with the component index the panel itself addresses the pin by
       (`key`), the pin's bit width and its name.  `key` is the pin's identity:
       it is what an order is expressed in, so an order survives the panel
       rebuilding its cache (renaming a pin does that) and is dropped when a
       different set of pins is on screen.
     * move / set_order / clear change the order; the change lands on the next
       frame the panel draws, never inside the one being drawn.
     * order reports what the panel will draw next, as keys.

   The calls are for the game's main/render thread - the panel is rebuilt and
   drawn there - and they only succeed while an IO panel is on screen (a level
   or a component workshop); elsewhere they report TC_PIN_ORDER_ERR_STATE. */
#define TC_SERVICE_PIN_ORDER "tc.pin_order"
#define TC_PIN_ORDER_API_VERSION_1 1u
#define TC_PIN_ORDER_API_VERSION_2 2u

#define TC_PIN_ORDER_OK 0
#define TC_PIN_ORDER_ERR_UNAVAILABLE (-1) /* service missing (older loader) */
#define TC_PIN_ORDER_ERR_ARGUMENT (-2)    /* null/bad argument */
#define TC_PIN_ORDER_ERR_GROUP (-3)       /* unknown group */
#define TC_PIN_ORDER_ERR_RANGE (-4)       /* index or buffer out of range */
#define TC_PIN_ORDER_ERR_STATE (-5)       /* no panel cache: nothing to order */
#define TC_PIN_ORDER_ERR_NOT_FOUND (-6)   /* no layout contribution for entry/frame */

/* The panel's three cached groups.  Inputs and outputs are the two sections a
   level shows; the third is the group the component workshop uses for its own
   memory and register pins. */
#define TC_PIN_ORDER_GROUP_INPUTS 0u
#define TC_PIN_ORDER_GROUP_OUTPUTS 1u
#define TC_PIN_ORDER_GROUP_MEMORY 2u
#define TC_PIN_ORDER_GROUP_COUNT 3u

typedef struct TCPinOrderEntryV1 {
    uint64_t key;      /* the pin's component index: its identity in an order */
    uint64_t width;    /* the pin's bit width */
    uint32_t name_size;   /* strlen(name) */
    uint32_t reserved;    /* 0 */
    char name[64];        /* NUL terminated; longer names are truncated */
} TCPinOrderEntryV1;

typedef struct TCPinOrderApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Entries the panel is showing in this group, 0 when it is not up. */
    uint32_t (*count)(void* context,uint32_t group);
    /* One entry by the position it has now. */
    int (*entry)(void* context,uint32_t group,uint32_t index,TCPinOrderEntryV1* out);
    /* Moves the entry at `from` to the slot `to`, both positions in the order
       the panel shows now. */
    int (*move)(void* context,uint32_t group,uint32_t from,uint32_t to);
    /* The keys the panel will draw next, in order.  `count` receives the whole
       length even when the caller's buffer is smaller (then ERR_RANGE). */
    int (*order)(void* context,uint32_t group,uint64_t* keys,uint32_t capacity,
                 uint32_t* count);
    /* Installs a whole order.  Keys the panel is not showing are ignored and
       the pins they name keep their own relative order at the end. */
    int (*set_order)(void* context,uint32_t group,const uint64_t* keys,uint32_t count);
    /* Drops the Mod's order and returns to the panel's own order. */
    int (*reset)(void* context,uint32_t group);
} TCPinOrderApiV1;

/* A control drawn inside one pin entry contributes its real screen-space
   rectangle here.  More than one producer may report the same entry in the
   same frame; the loader unions their rectangles.  The frame is explicit
   because decorators normally draw the previous frame's completed layout.

   This is the ownership boundary between a Mod that inserts content and
   another Mod that decorates the containing row: neither has to know the
   other's implementation or infer its size from cursor anchors. */
#define TC_PIN_ORDER_BOUNDS_VERSION_1 1u
typedef struct TCPinOrderBoundsV1 {
    uint32_t size;
    uint32_t version;
    int32_t frame;
    uint32_t group;
    uint64_t key;
    float min_x;
    float min_y;
    float max_x;
    float max_y;
} TCPinOrderBoundsV1;

/* V2 preserves the complete V1 prefix and adds the entry-layout broker. */
typedef struct TCPinOrderApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    uint32_t (*count)(void* context,uint32_t group);
    int (*entry)(void* context,uint32_t group,uint32_t index,TCPinOrderEntryV1* out);
    int (*move)(void* context,uint32_t group,uint32_t from,uint32_t to);
    int (*order)(void* context,uint32_t group,uint64_t* keys,uint32_t capacity,
                 uint32_t* count);
    int (*set_order)(void* context,uint32_t group,const uint64_t* keys,uint32_t count);
    int (*reset)(void* context,uint32_t group);
    int (*include_bounds)(void* context,const TCPinOrderBoundsV1* bounds);
    int (*bounds)(void* context,int32_t frame,uint32_t group,uint64_t key,
                  TCPinOrderBoundsV1* out,uint32_t out_size);
} TCPinOrderApiV2;

/* The component catalogue: what custom component types this session registered,
   what shape each one has, and why a registration was refused.

   The registration entry points (`register_logic`, `register_component`) keep
   working exactly as before; this service is the read-only view over what they
   registered, which is what a Mod needs to build a type list, a diagnostic
   report, or to check a shape before registering.

   Types are keyed by the `custom_id` the game itself uses (the definition's
   identity).  `type_id` is the Mod-namespaced spelling of it for logs and UI
   ("<owner mod>/0x<custom id>"); the plan's declared string ids arrive with the
   type-definition V2 stage.  `pin_id` is the pin's place in its direction,
   encoded as (direction << 32) | index, which is what the pin arrays of
   TCNativeComponentDefinition use.

   A type whose registration failed stays in the catalogue with `active` 0 and a
   `status` string naming the reason, so a Mod author can see what the loader
   rejected instead of only finding a missing component in the editor. */
#define TC_SERVICE_COMPONENT_REGISTRY "tc.component.registry"
#define TC_COMPONENT_REGISTRY_API_VERSION_1 1u

#define TC_COMPONENT_REGISTRY_OK 0
#define TC_COMPONENT_REGISTRY_ERR_UNAVAILABLE (-1) /* service missing (older loader) */
#define TC_COMPONENT_REGISTRY_ERR_ARGUMENT (-2)    /* null or undersized output */
#define TC_COMPONENT_REGISTRY_ERR_RANGE (-3)       /* index past the catalogue */
#define TC_COMPONENT_REGISTRY_ERR_UNKNOWN (-4)     /* no type with that id */
#define TC_COMPONENT_REGISTRY_ERR_SIZE (-5)        /* string buffer too small */

/* How the type's behaviour is provided.  Only NATIVE_CALLBACK exists today;
   the other three are the plan's implementation kinds and are reported once the
   stages that introduce them land. */
#define TC_COMPONENT_IMPL_NATIVE_CALLBACK 0u
#define TC_COMPONENT_IMPL_COMPOSITE 1u
#define TC_COMPONENT_IMPL_BUILTIN_ALIAS 2u
#define TC_COMPONENT_IMPL_PLACEHOLDER 3u

/* What the host could determine about this type.  The plan's wider capability
   set (STATE, RENDER, INPUT, STORAGE, ALIAS) is not derivable from a callback
   registration; those stay 0 until a type declares them. */
#define TC_COMPONENT_CAP_LOGIC (1u << 0)     /* a callback is bridged into the game */
#define TC_COMPONENT_CAP_WIDE_PIN (1u << 1)  /* at least one pin wider than one bit */
#define TC_COMPONENT_CAP_MULTI_PIN (1u << 2) /* more than one pin on either side */

#define TC_COMPONENT_PIN_INPUT 0u
#define TC_COMPONENT_PIN_OUTPUT 1u

typedef struct TCComponentPinInfoV1 {
    uint32_t size;
    uint32_t reserved;
    uint64_t pin_id;   /* (direction << 32) | index */
    uint32_t direction;/* TC_COMPONENT_PIN_* */
    uint32_t bits;     /* declared width, 1..64 for a bridged native callback */
    char name[64];     /* the declared pin name, truncated; "" when unnamed */
} TCComponentPinInfoV1;

typedef struct TCComponentTypeInfoV1 {
    uint32_t size;
    uint32_t reserved;
    uint64_t custom_id;      /* the game-side identity of the definition */
    uint64_t gate_cost;      /* declared statistics, as the component panel shows */
    uint64_t delay;
    uint32_t implementation; /* TC_COMPONENT_IMPL_* */
    uint32_t capabilities;   /* TC_COMPONENT_CAP_* */
    uint32_t schema_version; /* the definition's own data schema, starts at 1 */
    uint32_t active;         /* 1 when the loader bridged/registered it */
    uint32_t input_count;
    uint32_t output_count;
    uint32_t pin_limit;      /* how many pins of one direction the host reports */
    char type_id[80];        /* "<owner mod>/0x<custom id>" */
    char name[64];
    char description[192];
    char owner_mod[80];
    char status[160];        /* why it is not active; empty when it is */
} TCComponentTypeInfoV1;

typedef struct TCComponentRegistryApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Types registered so far this session, refused ones included. */
    uint32_t (*count)(void* context);
    /* One entry by catalogue position (registration order). */
    int (*get)(void* context,uint32_t index,TCComponentTypeInfoV1* out);
    /* One entry by the identity the game uses. */
    int (*find)(void* context,uint64_t custom_id,TCComponentTypeInfoV1* out);
    /* One pin of one direction.  pin_limit says how many are reported. */
    int (*pin)(void* context,uint64_t custom_id,uint32_t direction,uint32_t index,
               TCComponentPinInfoV1* out);
} TCComponentRegistryApiV1;

/* ---------------------------------------------------------------------------
   tc.component.types: registering a component *definition* (V2).

   The V1 entry point (host->register_component) takes a fixed pin array and
   hands the callback a `TCLogicIO` with room for eight pins.  V2 exists for two
   shapes V1 cannot express: more than eight pins on a side, and a per-instance
   state size chosen by the definition instead of a fixed eight words.

   What it does *not* change: a pin is still one uint64_t, 1..64 bits wide, and
   the total input width is still capped (128 bits) because the generated call
   carries exactly two 64-bit payload words - measured, not assumed: a six
   argument call arrives with garbage in the fifth and sixth words.  Multi-word
   signals are therefore out of scope by decision, not by omission.

   Registration happens during tc_mod_load only, like V1.  The definition and
   its strings are borrowed for the duration of the call; the callback, `user`
   and the pin strings that stay in the catalogue are copied. */
#define TC_SERVICE_COMPONENT_TYPES "tc.component.types"
#define TC_COMPONENT_TYPES_API_VERSION_1 1u
#define TC_COMPONENT_TYPES_VERSION_2 2u

#define TC_COMPONENT_TYPES_OK 0
#define TC_COMPONENT_TYPES_ERR_UNAVAILABLE (-1) /* service missing (older loader) */
#define TC_COMPONENT_TYPES_ERR_ARGUMENT (-2)    /* null, undersized or malformed */
#define TC_COMPONENT_TYPES_ERR_DUPLICATE (-3)   /* that custom_id is taken */
#define TC_COMPONENT_TYPES_ERR_UNSUPPORTED (-4) /* shape the host cannot bridge */
#define TC_COMPONENT_TYPES_ERR_GAME (-5)        /* the game refused the definition */
#define TC_COMPONENT_TYPES_ERR_BUDGET (-6)      /* pin widths exceed the host's budget */

/* Configuration migration.  A configuration is saved with the schema its
   definition declared at the time, so a release that changes the layout leaves
   older circuits carrying bytes that no longer fit.  A definition that declares
   a migration callback is asked to convert them the next time an instance is
   bound; without one, the instance simply runs on its default configuration and
   the stored bytes are left alone. */
#define TC_COMPONENT_CONFIG_MIGRATION_VERSION_1 1u

#define TC_COMPONENT_CONFIG_MIGRATE_OK 0     /* `out` holds the converted configuration */
#define TC_COMPONENT_CONFIG_MIGRATE_KEEP 1   /* keep the stored record, run on the default */
#define TC_COMPONENT_CONFIG_MIGRATE_REJECT 2 /* refuse this upgrade; keep the stored record */

/* `from_schema`/`from_data`/`from_bytes` describe what the circuit carries (the
   bytes were checksum-verified but do not have to match the current schema or
   size); `to_schema`/`capacity` describe what this definition registers.  On OK
   the host writes all `capacity` bytes into its own configuration, checksums
   them, and replaces the stored record - so the circuit is upgraded on the next
   save.  KEEP and REJECT change nothing: the record keeps its old bytes and the
   instance runs on the registered default.  Any other return value is treated
   as REJECT.  The callback runs on the game thread while an instance is bound. */
typedef int (*TCComponentConfigMigration)(void* user,uint32_t from_schema,const void* from_data,
                                          uint32_t from_bytes,uint32_t to_schema,
                                          void* out,uint32_t capacity);

/* One pin of a V2 definition.  `pin_id` is the stable, Mod-namespaced identity
   a future editor or storage record can refer to; it may be NULL, in which case
   the loader reports the pin by name (or by index) instead. */
typedef struct TCComponentPinV2 {
    const char* pin_id;
    const char* name;
    uint32_t bits;      /* 1..64 */
    uint32_t reserved;
} TCComponentPinV2;

typedef struct TCComponentTypeDefinitionV2 {
    uint32_t size;
    uint32_t version;              /* TC_COMPONENT_TYPES_VERSION_2 */
    uint64_t custom_id;            /* the stable identity saved in circuits */
    const char* type_id;           /* "mod-id/name"; NULL derives one */
    const char* name;
    const char* description;
    const char* shape_svg;         /* NULL uses the game's default shape */
    const TCComponentPinV2* inputs;
    const TCComponentPinV2* outputs;
    uint32_t input_count;          /* 0..16; one direction may be empty */
    uint32_t output_count;         /* 0..16; not both */
    uint32_t state_words;          /* simulation-state words per instance; 0 = stateless */
    uint32_t reserved;
    uint64_t gate_cost;
    uint64_t delay;
    TCLogicCallbackV2 callback;
    void* user;
    /* Appended after the first release of this table: a caller whose `size`
       stops before this field simply gets no lifecycle notifications, and the
       host never reads past `definition->size`. */
    const struct TCComponentLifecycleV1* lifecycle;
    /* M3 configuration state.  The host copies `default_config` during
       registration and gives every new instance its own copy.  Configuration
       is distinct from `state_words`: reset clears simulation state but never
       configuration.  A short, older definition has no configuration. */
    uint32_t config_schema;
    uint32_t config_size;           /* bytes; 0 = no configuration */
    const void* default_config;     /* required when config_size != 0 */
    /* Appended with the migration cut: a definition may convert a configuration
       that an older release of the same type saved.  A caller whose `size` stops
       before `config_migration_version` simply has no migration, exactly like
       the lifecycle pointer above. */
    uint32_t config_migration_version;   /* TC_COMPONENT_CONFIG_MIGRATION_VERSION_1 */
    uint32_t reserved2;
    TCComponentConfigMigration migrate_config;
    void* migration_user;                /* passed back as the migration's first argument */
    /* Appended with the pin-lane cut: the vertical lane this type's generated
       pins sit on, in board cells either side of the component's centre.  The
       game places a generated pin at `round(circuit_x / 8)` cells (measured:
       circuit x = -18 -> -2, +13 -> +2, y = -4 -> 0, +4 -> +1), so the loader
       writes the requested lane straight into the scaffold's own circuit
       coordinates; the value is therefore quantised to 1/8 of a cell.

       0 - a caller whose `size` stops before this field, and every release
       before this one - means the loader's default `2.0`, which is what all
       earlier builds produced.  A type that draws a stock-sized body (the
       original Constant/Static Value are 4.92 x 2.93 cells with their pins
       outside that box) declares `3.0` so its pins stay clear of the body; the
       lane only moves the pins, it does not change the footprint, the hit box,
       the pin count or the values the callback sees. */
    float pin_lane;
    uint32_t reserved3;
} TCComponentTypeDefinitionV2;

typedef struct TCComponentTypesApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Registers one V2 definition.  Returns TC_COMPONENT_TYPES_*; a refusal is
       recorded in tc.component.registry with its own reason, exactly like V1. */
    int (*register_definition)(void* context,const TCComponentTypeDefinitionV2* definition);
} TCComponentTypesApiV1;

/* ---------------------------------------------------------------------------
   tc.component.geometry: board-space geometry for a registered custom type.

   The pinned game's safely writable geometry is one local rectangle list, and
   this footprint turned out to be more than placement data: **the pointer drags
   a component anywhere inside its footprint rectangle**, including the parts
   where the game draws nothing at all.  Confirmed by hand on 2026-09-23 in an
   isolated board: a 12x6 type with one pin and a 12x6 type with no pins at all
   could both be dragged from empty corners of the box, while the *drawn* figure
   is only a small label plus a pin dot.  So "the whole note is draggable" needs
   no separate hit-box geometry - declaring the footprint is the lever.
   Earlier rounds reported the opposite ("footprints do not change hit testing")
   because the probe then measured the wrong thing: components placed through
   the command bus were not registered in the board's hit state at all until
   something else refreshed the board, and the selection set it watched latches.
   See docs/research/component-hitbox-path.md.  The footprint still also drives
   placement/occupancy, so a Mod that only wants a drag area should keep in mind
   that the box reserves board space.
   Half extents are measured in board-grid units and rounded outwards to the
   integer grid.  A type declaration may be changed only during tc_mod_load,
   immediately after the caller registered that type through
   tc.component.types (or host->register_component). */
#define TC_SERVICE_COMPONENT_GEOMETRY "tc.component.geometry"
#define TC_COMPONENT_GEOMETRY_API_VERSION_1 1u
#define TC_COMPONENT_GEOMETRY_API_VERSION_2 2u
/* V3: the footprint as whole board cells, with the offset the face actually
   has.  A face that carries more pins on one side is not centred on the
   component's grid position (the game lays a generated side out from row 0, so
   a four-output face sits half a cell to two cells below the position), and a
   *centred* box then either wastes a row above the face or misses its lower
   edge - and the box is what the game hit-tests, drags by and reserves.  The
   measured cost of getting this wrong is in docs/research and
   tests/float-pitch-probe.cpp: the same 2.93-cell face reserved four cells
   instead of three, so two parts could not be placed flush. */
#define TC_COMPONENT_GEOMETRY_API_VERSION_3 3u

#define TC_COMPONENT_GEOMETRY_OK 0
#define TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE (-1)
#define TC_COMPONENT_GEOMETRY_ERR_ARGUMENT (-2)
#define TC_COMPONENT_GEOMETRY_ERR_UNKNOWN (-3)
#define TC_COMPONENT_GEOMETRY_ERR_OWNERSHIP (-4)
#define TC_COMPONENT_GEOMETRY_ERR_RANGE (-5)
#define TC_COMPONENT_GEOMETRY_ERR_GAME (-6)
#define TC_COMPONENT_GEOMETRY_ERR_THREAD (-7)
#define TC_COMPONENT_GEOMETRY_ERR_STALE (-8)

typedef struct TCComponentGeometryApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Sets the board footprint for one type owned by the
       calling Mod.  Valid only while that Mod's tc_mod_load is running. */
    int (*set_footprint)(void* context,uint64_t custom_id,
                         float half_width,float half_height);
    /* Reads the rectangle actually used by a live component.  Odd rotations
       swap the returned board-space half extents. */
    int (*read_footprint)(void* context,const TCGameHandle* component,
                          float* half_width,float* half_height);
} TCComponentGeometryApiV1;

/* V2 keeps the complete V1 prefix and adds a board-space footprint for one
   live instance.  Unlike set_footprint (a load-time type declaration), this
   call is valid while the board is running and may be updated every frame.
   The game's point-to-component query uses it for native selection and drag;
   read_footprint returns it when present.  Half extents are board-space, so
   callers that want rotation-dependent dimensions perform that swap before
   calling. */
typedef struct TCComponentGeometryApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_footprint)(void* context,uint64_t custom_id,
                         float half_width,float half_height);
    int (*read_footprint)(void* context,const TCGameHandle* component,
                          float* half_width,float* half_height);
    int (*set_instance_footprint)(void* context,const TCGameHandle* component,
                                  float half_width,float half_height);
} TCComponentGeometryApiV2;

/* V3 keeps the complete V1/V2 prefix and adds the cell-rect form of
   set_footprint.  `x`/`y` are the top-left cell of the box in board cells
   relative to the component's own grid position (+y runs down the screen, the
   same convention the pins use: the upper input of a two-pin side is row 0 and
   the lower one row +1), and `width`/`height` are whole cells.  A caller that
   wants the stock 4.92 x 2.93-cell face declares (-2, -1, 5, 3); a face that
   hangs below its position declares the same width with its own y.  The host
   stores the rectangle as it stands - no outward rounding - so the cells a Mod
   asks for are the cells it gets.  Valid only while that Mod's tc_mod_load is
   running, like set_footprint. */
typedef struct TCComponentGeometryApiV3 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_footprint)(void* context,uint64_t custom_id,
                         float half_width,float half_height);
    int (*read_footprint)(void* context,const TCGameHandle* component,
                          float* half_width,float* half_height);
    int (*set_instance_footprint)(void* context,const TCGameHandle* component,
                                  float half_width,float half_height);
    int (*set_footprint_cells)(void* context,uint64_t custom_id,
                               int32_t x,int32_t y,uint32_t width,uint32_t height);
} TCComponentGeometryApiV3;

/* ---------------------------------------------------------------------------
   tc.component.render: per-instance drawing on the circuit board.

   V1 is deliberately an overlay service.  It does not suppress the game's
   own custom-component thumbnail/name yet, and it does not lend renderer or
   ImDrawList pointers to a Mod.  Every draw call is checked and forwarded by
   the host while the callback is active; retaining `draw` or `frame` after the
   callback returns is invalid.

   The two axis vectors already include camera zoom, DPI and the component's
   quarter-turn rotation.  A local point (x,y) maps to:
     origin + x * axis_x + y * axis_y
   All coordinates accepted by the draw table are screen pixels. */
#define TC_SERVICE_COMPONENT_RENDER "tc.component.render"
#define TC_COMPONENT_RENDER_API_VERSION_1 1u
#define TC_COMPONENT_RENDER_API_VERSION_2 2u
#define TC_COMPONENT_RENDER_API_VERSION_3 3u
#define TC_COMPONENT_RENDER_API_VERSION_4 4u
#define TC_COMPONENT_RENDER_FRAME_VERSION_1 1u
#define TC_COMPONENT_RENDER_DRAW_VERSION_1 1u

#define TC_COMPONENT_RENDER_OK 0
#define TC_COMPONENT_RENDER_ERR_UNAVAILABLE (-1)
#define TC_COMPONENT_RENDER_ERR_ARGUMENT (-2)
#define TC_COMPONENT_RENDER_ERR_UNKNOWN (-3)
#define TC_COMPONENT_RENDER_ERR_OWNERSHIP (-4)
#define TC_COMPONENT_RENDER_ERR_CAPACITY (-5)
#define TC_COMPONENT_RENDER_ERR_THREAD (-6)

#define TC_COMPONENT_RENDER_MAX_COMMANDS_PER_INSTANCE 4096u

typedef struct TCComponentRenderDrawV1 {
    uint32_t size;
    uint32_t version;
    void* context; /* borrowed; valid only during the draw callback */
    int (*line)(void* context,float x1,float y1,float x2,float y2,
                uint32_t color,float thickness);
    int (*rect)(void* context,float min_x,float min_y,float max_x,float max_y,
                uint32_t color,float rounding,float thickness);
    int (*rect_filled)(void* context,float min_x,float min_y,float max_x,float max_y,
                       uint32_t color,float rounding);
    int (*circle)(void* context,float center_x,float center_y,float radius,
                  uint32_t color,float thickness);
    int (*circle_filled)(void* context,float center_x,float center_y,float radius,
                         uint32_t color);
    int (*text)(void* context,float x,float y,uint32_t color,const char* utf8);
} TCComponentRenderDrawV1;

/* The draw table as it is handed to a draw callback is always the newest one
   the loader owns; a Mod reads `size`/`version` before it uses anything past
   the V1 prefix, exactly like the definition structs.  V2 appends the two
   calls a Mod needs to make its own board text look like the game's:

   - the game paints a component's label and value at a size that follows the
     camera (the label is part of the part's own 20-pixel-per-cell picture and
     the values are drawn by the board's word-watchee meshes), while
     `ImDrawList::AddText_Vec2` has no size argument in this build's cimgui.
     `text_sized` therefore takes the size in screen pixels and applies it for
     the duration of that one call.
   - `measure_text` answers the same question the game answers when it lays a
     label out: the width and height that string occupies when `text_sized`
     paints it at that `size` - the numbers a Mod needs to right-align a name or
     to shrink a value that would not fit its body.  This build paints text at
     about 0.79 of the size it is given, and both entry points agree on that
     scale, so a Mod only has to express its own text in digit heights.

   `bold` picks the game's bold face (`NoroshiCode_Bold`) instead of the face
   the draw list is currently using, which is how a Mod matches a stock part's
   bold label.  Both calls report TC_COMPONENT_RENDER_OK or one of the error
   codes below. */
#define TC_COMPONENT_RENDER_DRAW_VERSION_2 2u

typedef struct TCComponentRenderDrawV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*line)(void* context,float x1,float y1,float x2,float y2,
                uint32_t color,float thickness);
    int (*rect)(void* context,float min_x,float min_y,float max_x,float max_y,
                uint32_t color,float rounding,float thickness);
    int (*rect_filled)(void* context,float min_x,float min_y,float max_x,float max_y,
                       uint32_t color,float rounding);
    int (*circle)(void* context,float center_x,float center_y,float radius,
                  uint32_t color,float thickness);
    int (*circle_filled)(void* context,float center_x,float center_y,float radius,
                         uint32_t color);
    int (*text)(void* context,float x,float y,uint32_t color,const char* utf8);
    int (*text_sized)(void* context,float x,float y,float size,uint32_t color,
                      int bold,const char* utf8);
    int (*measure_text)(void* context,float size,int bold,const char* utf8,
                        float* width,float* height);
} TCComponentRenderDrawV2;

typedef struct TCComponentRenderFrameV1 {
    uint32_t size;
    uint32_t version;
    TCGameHandle component; /* frame-scoped board object handle */
    uint64_t custom_id;
    uint64_t instance_id;
    uint32_t rotation;
    uint32_t reserved0;
    float origin_x;
    float origin_y;
    float axis_x_x;
    float axis_x_y;
    float axis_y_x;
    float axis_y_y;
    float clip_min_x;
    float clip_min_y;
    float clip_max_x;
    float clip_max_y;
    const uint8_t* config; /* borrowed snapshot; may be null */
    uint32_t config_size;
    uint32_t config_schema;
    /* The loader's draw table.  It is always the newest table the loader owns,
       so read `draw->version`/`draw->size` before using anything past
       TCComponentRenderDrawV1 (see TCComponentRenderDrawV2). */
    const TCComponentRenderDrawV1* draw;
} TCComponentRenderFrameV1;

typedef void (*TCComponentRenderCallbackV1)(void* user,
                                             const TCComponentRenderFrameV1* frame);

typedef struct TCComponentRenderApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Registration is valid only during tc_mod_load and only for a type owned
       by the calling Mod.  Passing a null callback removes the registration. */
    int (*set_draw_callback)(void* context,uint64_t custom_id,
                             TCComponentRenderCallbackV1 callback,void* user);
} TCComponentRenderApiV1;

/* V2 keeps the complete V1 prefix and adds type-level ownership of the game's
   built-in custom-component picture (design thumbnail, watermark and related
   default mesh).  Disabling it does not change footprint, hit testing, pins in
   the model, selection, deletion or the Mod's overlay callback. */
typedef struct TCComponentRenderApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_draw_callback)(void* context,uint64_t custom_id,
                             TCComponentRenderCallbackV1 callback,void* user);
    int (*set_default_drawing)(void* context,uint64_t custom_id,int enabled);
} TCComponentRenderApiV2;

/* V3 keeps the complete V2 prefix and adds the game's own selection hint - the
   white broken arc a selected component shows while the board is in its
   move-selection state.  That sprite is one instance the game adds per selected
   element, built with a fixed scale from the level tree context, so its size
   follows neither the component's mesh nor the rectangle a Mod declared with
   tc.component.geometry; a Mod that draws its own footprint-sized hint can
   therefore turn the game's one off for its own types.  The host does it per
   type and per draw call: the selected-element hash set entry of a registered
   type is hidden for the duration of the game's own draw, so no sprite instance
   is produced for it at all, and it is restored before anything else runs.
   Only the calling Mod's own types are affected, and only that hint: selection
   itself, hit testing, dragging, deletion, the default drawing and the
   footprint do not change. */
typedef struct TCComponentRenderApiV3 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_draw_callback)(void* context,uint64_t custom_id,
                             TCComponentRenderCallbackV1 callback,void* user);
    int (*set_default_drawing)(void* context,uint64_t custom_id,int enabled);
    int (*set_selection_hint)(void* context,uint64_t custom_id,int enabled);
} TCComponentRenderApiV3;

/* V4 keeps the complete V3 prefix and adds the game's own "edit this component in
   the foundry" button - the one the bottom panel shows while a custom component is
   selected, which loads that component's schematic into the component workshop.

   A Mod-owned type has no player-editable schematic behind it: its logic comes from
   the Mod and its shape from tc.component.geometry, so opening it in the workshop is
   at best confusing and at worst a way to produce a component the Mod no longer
   controls.  A Mod can therefore drop that button for its own types.  The host does
   it at the panel's single button call: it is drawn with transparent colours and
   reports "not clicked", so the panel keeps its exact layout and nothing else in it
   moves, while the button itself is invisible and inert.  It is only hidden while
   every selected component is one of the calling Mod's types; a game component in
   the same selection keeps its own button.

   Only the calling Mod's own types are affected.  Selection, hit testing, dragging,
   deletion, the default drawing, the selection hint, the footprint, and the foundry
   itself (including the toolbar button that opens it) do not change. */
typedef struct TCComponentRenderApiV4 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_draw_callback)(void* context,uint64_t custom_id,
                             TCComponentRenderCallbackV1 callback,void* user);
    int (*set_default_drawing)(void* context,uint64_t custom_id,int enabled);
    int (*set_selection_hint)(void* context,uint64_t custom_id,int enabled);
    int (*set_foundry_button)(void* context,uint64_t custom_id,int enabled);
} TCComponentRenderApiV4;

#define TC_COMPONENT_RENDER_API_VERSION_5 5u

/* V5 keeps the complete V4 prefix and adds the *picture* a type is shown with:
   the item in the game's own component column and the preview picture in the
   bottom drawer.  Placement preview drawing is the separate V6 capability.

   The game builds that picture itself - a custom prototype's design thumbnail plus
   its name watermark - and asks the renderer for it as the texture
   `?snapshot_cc/com_custom_<decimal id>.png`, once per frame, for every item on
   screen (`get_captured_path__presenterZio_u28` -> `create_texture*`;
   docs/research/component-icons.md has the measurements).  This call hands the
   loader a PNG of the Mod's own to answer exactly that request with, so a Mod part
   can look on the shelf the way it looks on the board - the loader writes nothing
   into the game's structures and the game keeps creating, caching and releasing the
   texture itself.

   `png_path` is an absolute path to a readable PNG; NULL (or the path of a file
   that is gone) puts the type back on the game's own picture.  Only the calling
   Mod's own types are affected: hit testing, footprint, the board drawing, the
   drawer's fields and every other type are untouched, and a loader that predates
   this table simply does not publish it. */
typedef struct TCComponentRenderApiV5 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_draw_callback)(void* context,uint64_t custom_id,
                             TCComponentRenderCallbackV1 callback,void* user);
    int (*set_default_drawing)(void* context,uint64_t custom_id,int enabled);
    int (*set_selection_hint)(void* context,uint64_t custom_id,int enabled);
    int (*set_foundry_button)(void* context,uint64_t custom_id,int enabled);
    int (*set_picture)(void* context,uint64_t custom_id,const char* png_path);
} TCComponentRenderApiV5;

#define TC_COMPONENT_RENDER_API_VERSION_6 6u

/* V6 keeps the complete V5 prefix and adds an opt-in drawing takeover for the
   temporary custom-component record the game uses while a part is being
   placed.  The host recognises that record by the game's own ghost flags; it
   does not infer placement from the mouse or from palette geometry.

   When enabled, the game's default custom ghost (pins/name only in the pinned
   build) is suppressed and the type's existing draw callback is invoked at the
   ghost's snapped board position and rotation.  The preview frame has
   instance_id == 0, an invalid/zero component handle and no saved
   configuration, so callbacks naturally use their type defaults.  Disable the
   option to restore the game's own ghost. */
typedef struct TCComponentRenderApiV6 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_draw_callback)(void* context,uint64_t custom_id,
                             TCComponentRenderCallbackV1 callback,void* user);
    int (*set_default_drawing)(void* context,uint64_t custom_id,int enabled);
    int (*set_selection_hint)(void* context,uint64_t custom_id,int enabled);
    int (*set_foundry_button)(void* context,uint64_t custom_id,int enabled);
    int (*set_picture)(void* context,uint64_t custom_id,const char* png_path);
    int (*set_placement_preview)(void* context,uint64_t custom_id,int enabled);
} TCComponentRenderApiV6;

/* ---------------------------------------------------------------------------
   tc.component.instances: stable references to live component instances.

   An `instance_id` alone is not enough: the game reuses slots, so a Mod that
   kept an id from an earlier frame could end up reading a different component.
   A handle therefore carries the generation the host issued when it bound the
   instance; any mismatch is refused with ERR_STALE instead of being resolved to
   whatever is there now.

   Handles are host-issued and may be stored across frames.  They stay valid for
   as long as the instance exists: leaving the board, or deleting the component
   and recompiling, destroys it and fires the type's on_destroy callback. */
#define TC_SERVICE_COMPONENT_INSTANCES "tc.component.instances"
#define TC_COMPONENT_INSTANCES_API_VERSION_1 1u

#define TC_COMPONENT_INSTANCES_OK 0
#define TC_COMPONENT_INSTANCES_ERR_UNAVAILABLE (-1)
#define TC_COMPONENT_INSTANCES_ERR_ARGUMENT (-2)
#define TC_COMPONENT_INSTANCES_ERR_RANGE (-3)   /* buffer too small / index past the end */
#define TC_COMPONENT_INSTANCES_ERR_STALE (-4)   /* the handle's generation no longer matches */

typedef struct TCComponentInstanceHandle {
    uint32_t size;
    uint32_t version;
    uint64_t custom_id;    /* which registered type the instance is of */
    uint64_t instance_id;  /* the game's component identity on the board */
    uint64_t generation;   /* host-issued; changes when the slot is rebound */
} TCComponentInstanceHandle;

#define TC_COMPONENT_INSTANCE_HAS_STATE (1u << 0)   /* state_words > 0 */
#define TC_COMPONENT_INSTANCE_BOUND (1u << 1)       /* bound to a live callback */

typedef struct TCComponentInstanceInfoV1 {
    uint32_t size;
    uint32_t flags;                 /* TC_COMPONENT_INSTANCE_* */
    TCComponentInstanceHandle handle;
    uint32_t state_words;           /* simulation-state words the callback owns */
    uint32_t input_count;
    uint32_t output_count;
    uint32_t reserve;
    uint64_t cycle_calls;           /* CYCLE callbacks served for this instance */
    uint64_t peeks;                 /* REFRESH callbacks served */
    uint64_t resets;                /* RESET callbacks served */
    char type_id[80];               /* "<owner mod>/0x<custom id>" */
    char owner_mod[80];
} TCComponentInstanceInfoV1;

/* Lifecycle callbacks, appended to TCComponentTypeDefinitionV2 as an optional
   pointer: a definition whose `size` stops before that field behaves exactly as
   before (no lifecycle notifications).  Both callbacks run where the change
   happened - instance discover/destroy during a compile, or a board switch for
   destroy - so they must be cheap and must not touch UI or the board.  They get
   the instance's own IO view (borrowed arrays, same contract as the simulation
   callbacks); writing state or outputs is allowed and committed immediately. */
#define TC_COMPONENT_LIFECYCLE_VERSION_1 1u
typedef struct TCComponentLifecycleV1 {
    uint32_t size;
    uint32_t version;
    TCLogicCallbackV2 on_create;    /* first time this instance is bound */
    TCLogicCallbackV2 on_destroy;   /* the instance is gone or the board left */
    /* Appended after the first release of this structure: a lifecycle block whose
       `size` stops before this field simply gets no configuration notifications. */
    TCLogicCallbackV2 on_config_changed;   /* TC_LOGIC_CONFIG_CHANGED */
    /* Appended with the duplication cut: fired for an instance the host created by
       duplicating another one, after on_create. */
    TCLogicCallbackV2 on_clone;            /* TC_LOGIC_CLONE */
    /* Appended with the load/save cut. */
    TCLogicCallbackV2 on_load;             /* TC_LOGIC_LOAD */
    TCLogicCallbackV2 on_save;             /* TC_LOGIC_SAVE */
} TCComponentLifecycleV1;

typedef struct TCComponentInstancesApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Handles of live instances, optionally filtered by custom_id (0 = all).
       `written` is filled with the number produced; a short buffer returns
       ERR_RANGE and `written` set to the count that would fit plus the total in
       `total` when that pointer is given. */
    int (*enumerate)(void* context,uint64_t custom_id,TCComponentInstanceHandle* out,
                     uint32_t capacity,uint32_t* written,uint32_t* total);
    /* OK while the instance is alive; ERR_STALE for a superseded generation. */
    int (*validate)(void* context,const TCComponentInstanceHandle* handle);
    int (*info)(void* context,const TCComponentInstanceHandle* handle,
                TCComponentInstanceInfoV1* out);
    /* Copies the instance's simulation state (oldest-first).  A buffer smaller
       than the state returns ERR_RANGE with `words` set to the real size. */
    int (*state)(void* context,const TCComponentInstanceHandle* handle,uint64_t* out,
                 uint32_t capacity,uint32_t* words);
    /* Runs this one instance's RESET callback and zeroes its state, without
       touching the simulation: the caller decides when that is appropriate. */
    int (*reset)(void* context,const TCComponentInstanceHandle* handle);
} TCComponentInstancesApiV1;

/* ---------------------------------------------------------------------------
   tc.component.storage: host-owned per-instance configuration and simulation
   state snapshots.

   V1 deliberately stops before circuit-file persistence and Undo integration:
   it establishes the ownership and reset semantics those later layers need.
   Configuration is a byte blob with a definition-owned schema number;
   simulation state is the existing `state_words` buffer exposed as bytes.
   Every operation resolves a generation-checked instance handle. */
#define TC_SERVICE_COMPONENT_STORAGE "tc.component.storage"
#define TC_COMPONENT_STORAGE_API_VERSION_1 1u
#define TC_COMPONENT_STORAGE_INFO_VERSION_1 1u

#define TC_COMPONENT_STORAGE_OK 0
#define TC_COMPONENT_STORAGE_ERR_UNAVAILABLE (-1)
#define TC_COMPONENT_STORAGE_ERR_ARGUMENT (-2)
#define TC_COMPONENT_STORAGE_ERR_STALE (-3)
#define TC_COMPONENT_STORAGE_ERR_SIZE (-4)
#define TC_COMPONENT_STORAGE_ERR_SCHEMA (-5)
#define TC_COMPONENT_STORAGE_ERR_STATE (-6)   /* no edit transaction open, or one is */

#define TC_COMPONENT_STORAGE_HAS_CONFIG (1u << 0)
#define TC_COMPONENT_STORAGE_HAS_STATE (1u << 1)
/* The instance's component record owns a key/value table this build can
   address, so a successful write_config is also stored in the circuit file and
   comes back with the next load.  Cleared when the loader cannot address the
   record on this game build: the configuration then lives in memory only, and
   write_config returns UNAVAILABLE instead of pretending it was stored. */
#define TC_COMPONENT_STORAGE_HAS_PERSISTENCE (1u << 2)

typedef struct TCComponentStorageInfoV1 {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t config_schema;
    uint32_t config_size;           /* bytes */
    uint32_t state_size;            /* bytes; state_words * sizeof(uint64_t) */
    uint64_t config_revision;       /* starts at 1, changes after a write */
    TCComponentInstanceHandle handle;
} TCComponentStorageInfoV1;

typedef struct TCComponentStorageApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*info)(void* context,const TCComponentInstanceHandle* handle,
                TCComponentStorageInfoV1* out);
    /* Read calls support a count-only query (`out == NULL`, capacity == 0).
       `bytes` always receives the required size when non-NULL. */
    int (*read_config)(void* context,const TCComponentInstanceHandle* handle,
                       void* out,uint32_t capacity,uint32_t* bytes);
    /* Atomic whole-blob replacement.  Schema and byte count must match the
       registered definition; partial writes are intentionally not exposed. */
    int (*write_config)(void* context,const TCComponentInstanceHandle* handle,
                        uint32_t schema,const void* data,uint32_t bytes);
    int (*capture_state)(void* context,const TCComponentInstanceHandle* handle,
                         void* out,uint32_t capacity,uint32_t* bytes);
    /* Restores a previously captured state blob.  This does not run RESET and
       does not change configuration. */
    int (*restore_state)(void* context,const TCComponentInstanceHandle* handle,
                         const void* data,uint32_t bytes);
} TCComponentStorageApiV1;

/* tc.component.storage V2 - the same five calls plus an explicit edit
   transaction.

   Plan §9.3 asks for grouped edits rather than "guess the grouping from frame
   boundaries": a tool that changes a configuration in several steps (or several
   instances) must be able to say where the player-visible action begins and ends.

   `begin_edit` remembers the configuration the instance has right now; the
   `write_config` calls in between change it as usual but produce no undo step of
   their own; `commit_edit` turns the whole span into **one** undo step (the
   pre-begin bytes to the current ones); `abort_edit` puts the pre-begin bytes back
   and leaves the undo stack alone.  A begin without a matching commit/abort is
   dropped when the instance is released or the board changes. */
#define TC_COMPONENT_STORAGE_API_VERSION_2 2u

typedef struct TCComponentStorageApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*info)(void* context,const TCComponentInstanceHandle* handle,
                TCComponentStorageInfoV1* out);
    int (*read_config)(void* context,const TCComponentInstanceHandle* handle,
                       void* out,uint32_t capacity,uint32_t* bytes);
    int (*write_config)(void* context,const TCComponentInstanceHandle* handle,
                        uint32_t schema,const void* data,uint32_t bytes);
    int (*capture_state)(void* context,const TCComponentInstanceHandle* handle,
                         void* out,uint32_t capacity,uint32_t* bytes);
    int (*restore_state)(void* context,const TCComponentInstanceHandle* handle,
                         const void* data,uint32_t bytes);
    /* Opens the transaction.  ERR_STATE when one is already open on this handle. */
    int (*begin_edit)(void* context,const TCComponentInstanceHandle* handle);
    /* Closes it as one undo step.  ERR_STATE when none is open. */
    int (*commit_edit)(void* context,const TCComponentInstanceHandle* handle);
    /* Closes it and restores the configuration the transaction started from.
       ERR_STATE when none is open. */
    int (*abort_edit)(void* context,const TCComponentInstanceHandle* handle);
} TCComponentStorageApiV2;

#endif
