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
#define TC_COMPONENT_CAP_COMPOSITE (1u << 3) /* the behaviour is a netlist the game compiles */

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
   tc.component.compose: the built-in type catalogue (M4, first cut).

   The plan's M4 lets a Mod build a component out of the game's own parts
   instead of a native callback.  Those parts are addressed by *kind byte*, and
   a kind byte is build data - the same meaning is not guaranteed across builds.
   This service is the mapping: one entry per built-in prototype the running
   build exposes, carrying the name the game gives it, the stable spelling a Mod
   should key on, and the pins the prototype declares.

   The spelling is `builtin/<name>`; a build that carries several kinds under
   one name numbers them `builtin/<name>#2`, `builtin/<name>#3` ... in ascending
   kind order (this build repeats seventeen names - "Splitter" six times,
   "Output" seven, "Input" six, "Delay Line" twice ...).  A
  prototype without a name falls back to `builtin/0x<kind>`.  `find_by_type_id`
   also accepts the name part case-insensitively for the unnumbered spelling, so
   `builtin/and` finds `builtin/AND`.

   The catalogue reports **every** kind the table holds, including the slots the
   game keeps without a name (this build: 21 of 125 - 0x7b is the all-zero one,
   0x4e is the slot custom instances carry).  Those are real answers: they are
   exactly the kinds `get_prototype()` accepts, and an unknown kind raises a Nim
   error a plugin cannot recover from.  An unnamed slot is spelled by its kind
   and usually declares no pins.

   What is *not* here, on purpose:
     * no cost - a built-in's gate/delay numbers are what the game's own scorer
       analyses out of its inner circuit (measured: Delay Line 5 gates, 4 delay),
       and the prototype's own two fields read 0 for every kind in this build;
     * no capability bits - LOGIC/STATE/RENDER/ALIASABLE per kind is the
       research the alias path still owes;
     * the input *positions* of the stateful families.  Those prototypes declare
       no input pins at all (reported through TC_BUILTIN_FLAG_NO_DECLARED_INPUTS),
       and their measured input offsets live in docs/research/component-pipeline.md
       section 11.2 until the alias path turns them into contract.

   The catalogue is read-only, filled once at boot from the same prototype table
   the pin reads use, and answers a Mod from any thread the loader's other read
   services answer on (the game thread). */
#define TC_SERVICE_COMPONENT_COMPOSE "tc.component.compose"
#define TC_COMPONENT_COMPOSE_API_VERSION_1 1u

#define TC_COMPONENT_COMPOSE_OK 0
#define TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE (-1) /* no prototype table this session */
#define TC_COMPONENT_COMPOSE_ERR_ARGUMENT (-2)    /* null or undersized output */
#define TC_COMPONENT_COMPOSE_ERR_RANGE (-3)       /* index or pin past the catalogue */
#define TC_COMPONENT_COMPOSE_ERR_UNKNOWN (-4)     /* no entry with that kind / spelling */

/* No input pin is declared at all.  Measured, not interpreted: the stateful
   families (Delay Line 0x0d/0x37, Register, Counter ...) and the pure sources
   (Input, Push Button, Verilog Input) both land here. */
#define TC_BUILTIN_FLAG_NO_DECLARED_INPUTS (1u << 0)
/* The prototype carries a shape - the SVG the game draws for a built-in part.
   A metadata clone would be the only other thing that reads it. */
#define TC_BUILTIN_FLAG_HAS_SHAPE (1u << 1)
/* Reserved for "this kind can be registered as a full alias under a new id".
   Never set in this build: the game dispatches by the component record's kind
   byte and a Mod type's instances carry 0x4e, so there is no such path (see
   `TCComponentComposeApiV2::alias`). */
#define TC_BUILTIN_FLAG_ALIASABLE (1u << 2)

/* Pin width flags.  `word_size_raw` is always reported verbatim. */
#define TC_BUILTIN_PIN_AUTO_WIDTH (1u << 0) /* raw is the build's auto marker */
#define TC_BUILTIN_PIN_RAW_WIDTH (1u << 1)  /* raw is a marker this build derives
                                               (measured 0x7fff...fe / ...fd on the
                                               two splitters); semantics not
                                               verified, `bits` stays 0 */

#define TC_BUILTIN_PIN_LIMIT 32u /* pins reported per direction, as the registry does */

typedef struct TCBuiltinPinV1 {
    uint32_t size;
    uint32_t flags;        /* TC_BUILTIN_PIN_* */
    uint32_t direction;    /* TC_COMPONENT_PIN_INPUT / TC_COMPONENT_PIN_OUTPUT */
    uint32_t index;
    uint32_t bits;         /* 1..64, or 0 when `word_size_raw` is a marker */
    int32_t x, y;          /* board-cell offset from the node's position */
    uint64_t word_size_raw;
} TCBuiltinPinV1;

typedef struct TCBuiltinTypeInfoV1 {
    uint32_t size;
    uint32_t kind;         /* the kind byte in the running build */
    uint32_t flags;        /* TC_BUILTIN_FLAG_* */
    uint32_t input_count;
    uint32_t output_count;
    uint32_t pin_limit;    /* how many pins of one direction are reported */
    char type_id[80];      /* "builtin/<name>" ("#n" when the name repeats) */
    char name[64];
    char description[192]; /* as the registry reports it: truncated, "" when none */
} TCBuiltinTypeInfoV1;

typedef struct TCComponentComposeApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* How many built-in prototypes this session exposes. */
    uint32_t (*count)(void* context);
    /* One entry by catalogue position (ascending kind). */
    int (*get)(void* context,uint32_t index,TCBuiltinTypeInfoV1* out);
    /* One entry by the kind byte the game uses in this build. */
    int (*find)(void* context,uint32_t kind,TCBuiltinTypeInfoV1* out);
    /* One entry by the stable spelling above. */
    int (*find_by_type_id)(void* context,const char* type_id,TCBuiltinTypeInfoV1* out);
    /* One pin of one direction of one kind. */
    int (*pin)(void* context,uint32_t kind,uint32_t direction,uint32_t index,
               TCBuiltinPinV1* out);
} TCComponentComposeApiV1;

/* ---------------------------------------------------------------------------
   tc.component.types: registering a component *definition* (V2).

   The V1 entry point (host->register_component) takes a fixed pin array and
   hands the callback a `TCLogicIO` with room for eight pins.  V2 exists for two
   shapes V1 cannot express: more than eight pins on a side, and a per-instance
   state size chosen by the definition instead of a fixed eight words.

   A pin is still one uint64_t, 1..64 bits wide.  V2/V3 allow 32 pins per
   direction and 2048 input bits in total.  The generated code does not widen a
   foreign call beyond the measured four-argument JIT limit; it stages inputs in
   128-bit chunks, then performs one callback invocation.  Multi-word signals
   remain out of scope: one pin is still one uint64_t.

   Registration happens during tc_mod_load only, like V1.  The definition and
   its strings are borrowed for the duration of the call; the callback, `user`
   and the pin strings that stay in the catalogue are copied. */
#define TC_SERVICE_COMPONENT_TYPES "tc.component.types"
#define TC_COMPONENT_TYPES_API_VERSION_1 1u
#define TC_COMPONENT_TYPES_API_VERSION_2 2u
#define TC_COMPONENT_TYPES_VERSION_2 2u
#define TC_COMPONENT_TYPES_VERSION_3 3u

#define TC_COMPONENT_TYPES_OK 0
#define TC_COMPONENT_TYPES_ERR_UNAVAILABLE (-1) /* service missing (older loader) */
#define TC_COMPONENT_TYPES_ERR_ARGUMENT (-2)    /* null, undersized or malformed */
#define TC_COMPONENT_TYPES_ERR_DUPLICATE (-3)   /* that custom_id is taken */
#define TC_COMPONENT_TYPES_ERR_UNSUPPORTED (-4) /* shape the host cannot bridge */
#define TC_COMPONENT_TYPES_ERR_GAME (-5)        /* the game refused the definition */
#define TC_COMPONENT_TYPES_ERR_BUDGET (-6)      /* pin widths exceed the host's budget */
#define TC_COMPONENT_TYPES_ERR_STATE (-7)       /* the game is not in a state where its
                                                   component model may change (a board
                                                   is up, or the caller is off the game
                                                   thread); retry from the main menu */

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
    /* Appended with the description cut (2026-09-28): the game prints the
       prototype's description at the top of the bottom component drawer - the
       panel a Mod's property rows live in - so a definition's `description` is
       *not* written into the prototype unless this bit is set.  The text is
       still recorded in the loader's own type catalogue (the Mods page), it just
       stops appearing inside the game's panel.  A caller whose `size` stops
       before this field keeps the new default: description hidden.  The legacy
       V1 table (`register_component`) has no room for a flag and keeps
       publishing its own description. */
    uint32_t flags;                /* TC_COMPONENT_TYPE_* */
    uint32_t reserved4;
    /* Optional exact combinational dependency map, appended in 2026-10.  One
       uint32_t per output; bit N says that output reads input N.  The host only
       consumes this table when TC_COMPONENT_TYPE_OUTPUT_DEPENDENCIES is set.
       Such a callback must be safe to evaluate in REFRESH independently for
       each output: REFRESH may be called more than once in one host cycle and
       must not commit sequential state.  A callback which also sets
       TC_COMPONENT_TYPE_CLOCKED_TICK gets one hidden all-input sink that calls
       it once in CYCLE; its visible outputs are still evaluated independently
       in REFRESH, so the state update cannot create a combinational edge to an
       output.  Older definitions stop before this tail and keep the
       conservative all-inputs-to-all-outputs scaffold. */
    const uint32_t* output_dependencies;
    uint32_t output_dependency_count;
    uint32_t reserved5;
    /* Optional game-JIT programs for stateless, fully-driven combinational
       types.  Prefix N is emitted immediately before output N's generated
       assignment; expression N replaces the callback invocation.  Programs
       use @T@ for the instance token and @0@..@31@ for input expressions.
       The host copies every string during registration. */
    uint32_t inline_expression_version; /* TC_COMPONENT_INLINE_EXPRESSIONS_VERSION_1 */
    uint32_t inline_expression_count;   /* exactly output_count */
    const char* const* inline_prefixes;
    const char* const* inline_expressions;
} TCComponentTypeDefinitionV2;

/* V3 repeats the V2 definition metadata but selects TCLogicIOV3 for the logic
   callback.  Its generated output drivers are the game's own tri-state Switch
   components, so drive_mask=0 really disconnects the pin from the net instead
   of being coerced to a low value.  Lifecycle callbacks remain V1/V2 IO for
   this table revision: placement and configuration notifications do not
   sample or drive pins. */
typedef struct TCComponentTypeDefinitionV3 {
    uint32_t size;
    uint32_t version;              /* TC_COMPONENT_TYPES_VERSION_3 */
    uint64_t custom_id;
    const char* type_id;
    const char* name;
    const char* description;
    const char* shape_svg;
    const TCComponentPinV2* inputs;
    const TCComponentPinV2* outputs;
    uint32_t input_count;
    uint32_t output_count;
    uint32_t state_words;
    uint32_t reserved;
    uint64_t gate_cost;
    uint64_t delay;
    TCLogicCallbackV3 callback;
    void* user;
    const struct TCComponentLifecycleV1* lifecycle;
    uint32_t config_schema;
    uint32_t config_size;
    const void* default_config;
    uint32_t config_migration_version;
    uint32_t reserved2;
    TCComponentConfigMigration migrate_config;
    void* migration_user;
    float pin_lane;
    uint32_t reserved3;
    uint32_t flags;
    uint32_t reserved4;
    const uint32_t* output_dependencies;
    uint32_t output_dependency_count;
    uint32_t reserved5;
    uint32_t inline_expression_version;
    uint32_t inline_expression_count;
    const char* const* inline_prefixes;
    const char* const* inline_expressions;
} TCComponentTypeDefinitionV3;

/* Show this type's `description` in the game's bottom panel.  Without it the
   description stays in the loader's catalogue only. */
#define TC_COMPONENT_TYPE_SHOW_DESCRIPTION (1u<<0)
/* Use the optional per-output input masks at the definition tail.  This removes
   false combinational loops without inserting a delay: the generated inner
   circuit contains exactly the edges the callback declares. */
#define TC_COMPONENT_TYPE_OUTPUT_DEPENDENCIES (1u<<1)
/* With exact output dependencies, add a hidden all-input CYCLE sink.  This is
   for callbacks with sequential state: the sink commits that state once per
   simulation cycle but has no external output, so its input dependencies can
   never close a combinational loop.  Visible output callbacks remain REFRESH
   evaluations and therefore cannot commit state. */
#define TC_COMPONENT_TYPE_CLOCKED_TICK (1u<<2)
/* V3 callbacks that consume the bridge's private selected-output selector may
   coalesce outputs with identical dependency masks.  The first driver invokes
   the callback once for the whole group; later drivers read the published
   values.  Requires OUTPUT_DEPENDENCIES and a V3 callback. */
#define TC_COMPONENT_TYPE_GROUPED_OUTPUTS (1u<<3)
/* Replace callback evaluation with copied straight-line expressions in the
   game's generated simulation program.  Version 1 is stateless and always
   drives every output; the callback remains registered as a compatibility
   fallback for hosts that do not understand this tail. */
#define TC_COMPONENT_TYPE_INLINE_EXPRESSIONS (1u<<4)
#define TC_COMPONENT_INLINE_EXPRESSIONS_VERSION_1 1u

typedef struct TCComponentTypesApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Registers one V2 definition.  Returns TC_COMPONENT_TYPES_*; a refusal is
       recorded in tc.component.registry with its own reason, exactly like V1. */
    int (*register_definition)(void* context,const TCComponentTypeDefinitionV2* definition);
} TCComponentTypesApiV1;

/* V2 keeps the complete V1 prefix and adds the three-state definition entry.
   Querying API V1 continues to return the smaller table. */
typedef struct TCComponentTypesApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*register_definition)(void* context,const TCComponentTypeDefinitionV2* definition);
    int (*register_definition_v3)(void* context,const TCComponentTypeDefinitionV3* definition);
} TCComponentTypesApiV2;

/* tc.component.runtime V1: register a brand-new component type while the game
   is already running, for an editor that creates components from source.

   `tc.component.types` deliberately only answers during tc_mod_load, because a
   type has to exist before the game compiles a board that uses it.  This entry
   is the narrow exception: it adds a type that does not exist yet, and only in
   the window where the component model is idle - on the game thread, with no
   board loaded (the main menu, where a Mod's own editor page lives).  It
   refuses with TC_COMPONENT_TYPES_ERR_STATE outside that window and with
   TC_COMPONENT_TYPES_ERR_DUPLICATE when the id is already known, so it can
   never move a port or a state layout under a live circuit.

   The definition and every string/pin array it points at must stay alive and
   unmodified for the rest of the process, exactly like a load-time one.  The
   type is bridged and activated immediately, so the next board the player loads
   compiles it like any other registered component. */
#define TC_SERVICE_COMPONENT_RUNTIME "tc.component.runtime"
#define TC_COMPONENT_RUNTIME_API_VERSION_1 1u
#define TC_COMPONENT_RUNTIME_API_VERSION_2 2u

typedef struct TCComponentRuntimeApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*register_type)(void* context,const TCComponentTypeDefinitionV3* definition);
} TCComponentRuntimeApiV1;

/* V2 keeps the V1 prefix and adds the two mutations an in-game source editor
   needs.  They have the same idle-model restriction as register_type.

   replace_type is transactional from the caller's point of view: `previous`
   is the currently registered definition and `replacement` has the same
   custom_id.  If importing the replacement fails, the host restores
   `previous` before returning the error.  Both definitions must remain alive
   until the call returns; after success only replacement has to remain alive.

   unregister_type removes a type owned by the calling Mod.  Unknown ids and
   ids owned by another Mod are refused as TC_COMPONENT_TYPES_ERR_ARGUMENT. */
typedef struct TCComponentRuntimeApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*register_type)(void* context,const TCComponentTypeDefinitionV3* definition);
    int (*replace_type)(void* context,const TCComponentTypeDefinitionV3* previous,
                        const TCComponentTypeDefinitionV3* replacement);
    int (*unregister_type)(void* context,uint64_t custom_id);
} TCComponentRuntimeApiV2;

/* V2: the internal-netlist builder.  A composite type has no callback: it is a
   circuit the game itself compiles, built from the parts V1 lists.  The session
   is opened by `begin`, filled by `node` and `connect`, and handed to the game by
   `commit` - which validates first, so a netlist that cannot be compiled (a
   dangling input, two wires into one pin, a combinational loop, a layout whose
   wires cannot be kept apart) is refused before anything is imported.

   What `node` accepts as a spelling: either a built-in spelling from V1
   (`builtin/<name>`) - the host fills in that part's pins from the catalogue -
   or another registered type's `type_id` from `tc.component.registry`, which is
   how one Mod's component becomes a part of another's (`kind` 0x4e inside).

   `x`/`y` are circuit units of the *inner* board, one unit per board cell; the
   part's pins are its own prototype offsets from there.  `bits` is the width the
   part's ports use (1 for the one-bit parts).

   After `commit` the type behaves like any other Mod type for everything that
   does not need a callback: the component column, placement, the command bus
   (place/duplicate/delete), geometry, pictures, editor rows and ownership all
   work; the simulation is the game's own compiled circuit.  What a composite
   deliberately does *not* have: `tc.component.instances` bindings (there is no
   callback to bind), configuration blobs (`tc.component.storage`) and the
   lifecycle callbacks - those belong to a type the loader drives. */
#define TC_COMPONENT_COMPOSE_API_VERSION_2 2u

#define TC_COMPONENT_COMPOSE_ERR_BUILD (-5)     /* no such open session */
#define TC_COMPONENT_COMPOSE_ERR_NETLIST (-6)   /* the netlist was refused; ask `status` */
#define TC_COMPONENT_COMPOSE_ERR_DUPLICATE (-7) /* that custom id already exists */
#define TC_COMPONENT_COMPOSE_ERR_GAME (-8)      /* the game refused to import it */
#define TC_COMPONENT_COMPOSE_ERR_UNSUPPORTED (-9) /* this build cannot do it; see `alias` */

/* `clone_metadata` flags: which fields of the built-in part are copied onto the
   Mod's own registered type.  Pins are deliberately not in this list - a type's
   pins are part of the definition the game already imported, and changing them
   after the fact would invalidate every instance; declare them at registration,
   or use the part as a node of a composite.  Neither is the icon: that is the
   one field that cannot be cloned, because the game draws a custom prototype
   through its own generic path and ignores `shape_svg` there (measured,
   docs/research/component-icons.md section 3) - give the type a picture with
   `tc.component.render` V5 `set_picture` instead. */
#define TC_COMPOSE_CLONE_NAME (1u << 0)
#define TC_COMPOSE_CLONE_DESCRIPTION (1u << 1)
#define TC_COMPOSE_CLONE_COST (1u << 2)

typedef struct TCComposeTypeV1 {
    uint32_t size;
    uint32_t version;
    uint64_t custom_id;
    const char* type_id;     /* optional "<owner>/<name>" spelling; may be NULL */
    const char* name;
    const char* description; /* optional */
    /* The game recomputes the gate count from the netlist when it parses the
       definition; `delay` is trusted verbatim, so declare the real critical
       path (a Delay Line is 4). */
    int64_t gate_cost;
    int64_t delay;
    const TCComponentPinV2* inputs;   /* name + bits, in pin order */
    uint32_t input_count;
    const TCComponentPinV2* outputs;
    uint32_t output_count;
} TCComposeTypeV1;

typedef struct TCComponentComposeApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* V1 prefix: the built-in catalogue. */
    uint32_t (*count)(void* context);
    int (*get)(void* context,uint32_t index,TCBuiltinTypeInfoV1* out);
    int (*find)(void* context,uint32_t kind,TCBuiltinTypeInfoV1* out);
    int (*find_by_type_id)(void* context,const char* type_id,TCBuiltinTypeInfoV1* out);
    int (*pin)(void* context,uint32_t kind,uint32_t direction,uint32_t index,
               TCBuiltinPinV1* out);
    /* V2: one build session. */
    int (*begin)(void* context,const TCComposeTypeV1* type,uint64_t* session);
    /* Adds an inner part; `*node_index` receives the index `connect` uses. */
    int (*node)(void* context,uint64_t session,const char* type_id,int32_t x,int32_t y,
                uint32_t bits,uint32_t* node_index);
    /* Wires one source pin to one sink pin.  The node indices are the ones
       `begin` reserved for the type's own pins - input pin i is node i, output
       pin j is node (input_count + inner nodes + j) - and the ones `node`
       returned.  Fan-out from one source pin is allowed; two wires into one sink
       pin is not. */
    int (*connect)(void* context,uint64_t session,uint32_t from_node,uint32_t from_pin,
                   uint32_t to_node,uint32_t to_pin);
    /* Validates, encodes and imports: on success the type is registered. */
    int (*commit)(void* context,uint64_t session);
    int (*abort)(void* context,uint64_t session);
    /* Why the last call on this session was refused (never NULL; "" when none). */
    const char* (*status)(void* context,uint64_t session);
    /* Copies the named fields of a built-in part onto a type this Mod already
       registered (a composite or a callback type - the call only rewrites
       metadata).  `builtin_type_id` is a V1 spelling, `custom_id` the Mod's own
       type.  This is plan section 8.3's `clone_builtin_metadata`: name and
       description make the type *read* like the original, nothing about
       behaviour is promised. */
    int (*clone_metadata)(void* context,uint64_t custom_id,const char* builtin_type_id,
                          uint32_t flags);
    /* Plan section 8.3's `register_builtin_alias`: the outer instance keeps the
       original part's *complete* dispatch (its rendering, its interaction, its
       serialization).  This build has no such path, and this call says so
       instead of registering a look-alike shell: the game dispatches behaviour
       by the component record's kind byte, and an instance of a Mod type
       carries kind 0x4e, so a new id can never reach a built-in's dispatch.
       The call always answers TC_COMPONENT_COMPOSE_ERR_UNSUPPORTED; what a Mod
       can do instead is `clone_metadata` (the name/description) plus a composite
       built from the same part (the behaviour) plus a V5 picture (the icon).
       The catalogue never sets TC_BUILTIN_FLAG_ALIASABLE for the same reason. */
    int (*alias)(void* context,uint64_t custom_id,const char* builtin_type_id);
    /* How wide this type's own pin lane is, in board cells (1..16; the default is
       the historical two-cell shape).  The input column moves left and the
       output column right, so a wide type has the room to lay its inner circuit
       out between them - which is what a decoder with thirty-two outputs needs.
       Call it on an open session, before `commit`; the same knob the declarative
       API has (`pin_lane`), whose wide-lane wire positions are verified. */
    int (*pin_lane)(void* context,uint64_t session,uint32_t lane_cells);
} TCComponentComposeApiV2;

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

/* V3 keeps the complete V2 prefix and adds the one primitive a component that
   wants a picture needs: a textured quad.

   The image is a texture the loader owns - a Mod loads it once with
   `tc.component.render` V7's `load_image` and names the handle it got back.  A
   Mod therefore never touches the renderer, never decodes a PNG itself and
   never holds anything whose lifetime it would have to reason about: the
   loader creates the GL object, keeps it while the plugin is loaded, and frees
   it when the plugin goes away or the Mod releases the key.

   Corners are screen pixels in ImDrawList order (top-left, top-right,
   bottom-right, bottom-left); a Mod that wants the quad to follow the
   component's own rotation passes corners it computed with
   `tc_component_render.h`'s `localToScreen`, exactly like the line and rect
   primitives.  `tint` is 0xAABBGGRR and multiplies the texture.  The call is
   counted against the same per-instance/per-frame command quota as every other
   primitive and is clipped to the frame's clip rectangle. */
#define TC_COMPONENT_RENDER_DRAW_VERSION_3 3u

typedef struct TCComponentRenderQuadV1 {
    float x0,y0,u0,v0;
    float x1,y1,u1,v1;
    float x2,y2,u2,v2;
    float x3,y3,u3,v3;
    uint32_t tint;      /* 0xAABBGGRR */
    uint32_t reserved;
} TCComponentRenderQuadV1;

typedef struct TCComponentRenderDrawV3 {
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
    int (*image)(void* context,uint64_t image,const TCComponentRenderQuadV1* quad);
} TCComponentRenderDrawV3;

/* V4 adds loader-owned font faces.  font == 0 selects the host's regular/bold
   face; a non-zero handle names a TTF loaded through render V8.  ITALIC is a
   host-side synthetic slant, so it also works when the file only contains an
   upright face. */
#define TC_COMPONENT_RENDER_DRAW_VERSION_4 4u
#define TC_COMPONENT_TEXT_BOLD (1u<<0)
#define TC_COMPONENT_TEXT_ITALIC (1u<<1)
#define TC_COMPONENT_TEXT_KNOWN_FLAGS \
    (TC_COMPONENT_TEXT_BOLD|TC_COMPONENT_TEXT_ITALIC)

typedef struct TCComponentRenderDrawV4 {
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
    int (*image)(void* context,uint64_t image,const TCComponentRenderQuadV1* quad);
    int (*text_font)(void* context,uint64_t font,float x,float y,float size,
                     uint32_t color,uint32_t style,const char* utf8);
    int (*measure_font)(void* context,uint64_t font,float size,uint32_t style,
                        const char* utf8,float* width,float* height);
} TCComponentRenderDrawV4;

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

/* V7 keeps the complete V6 prefix and adds the images a Mod's own draw callback
   paints (TCComponentRenderDrawV3::image).

   V5's `set_picture` answers the *game's* own texture requests - the shelf card
   and the drawer preview - and the game keeps creating, caching and releasing
   that texture.  This is the other half: the loader loads a PNG for the calling
   Mod, owns the GL object and hands the Mod a handle it names in a draw call.

   * `load_image(key, png_path, out)`: `key` is the Mod's own name for the image
     (lowercase `a-z 0-9 . _ -`, at most 64 bytes), `png_path` is an absolute
     path to a readable PNG.  The file is read once, right here; the loader
     keeps the decoded pixels and the GL object, so a later change to the file
     on disk does not change what is drawn.  Loading the same key again replaces
     the pixels: the returned `image` handle stays the same and `revision` goes
     up, so a Mod that caches something keyed by the handle can notice.
   * `release_image(key)`: drops one image; `key == NULL` drops every image of
     the calling Mod.  The GL object is freed on a following frame (the texture
     manager retires it), so a draw call in the current frame is still safe.
   * Validity: the same thread and lifetime rules as the texture API - the game
     thread, while the plugin is loading or active.  An older loader does not
     publish this table at all, and `tableV7` in tc_component_render.h says so.
   * Quota: at most TC_COMPONENT_RENDER_MAX_IMAGES images and
     TC_COMPONENT_RENDER_MAX_IMAGE_BYTES decoded bytes per Mod; the texture
     manager's own per-Mod limits still apply.  Exceeding either is
     TC_COMPONENT_RENDER_ERR_IMAGE, and nothing is replaced on failure.
   * Device rebuild: this build creates the GL context once with the window and
     never recreates it, so there is no rebuild path to promise - the loader
     keeps the decoded pixels, and a Mod must never cache a renderer id (it
     never receives one). */
#define TC_COMPONENT_RENDER_API_VERSION_7 7u
#define TC_COMPONENT_RENDER_IMAGE_VERSION_1 1u

#define TC_COMPONENT_RENDER_MAX_IMAGES 16u
#define TC_COMPONENT_RENDER_MAX_IMAGE_FILE_BYTES (64u*1024u*1024u)
#define TC_COMPONENT_RENDER_MAX_IMAGE_BYTES (16u*1024u*1024u)

#define TC_COMPONENT_RENDER_ERR_IMAGE (-7)   /* key, path, decode, size or quota */

typedef struct TCComponentRenderImageV1 {
    uint32_t size;
    uint32_t version;
    uint64_t image;      /* handle for TCComponentRenderDrawV3::image; 0 on failure */
    uint32_t width;
    uint32_t height;
    uint32_t revision;   /* starts at 1, goes up when the key is loaded again */
    uint32_t reserved;
} TCComponentRenderImageV1;

typedef struct TCComponentRenderApiV7 {
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
    int (*load_image)(void* context,const char* key,const char* png_path,
                      TCComponentRenderImageV1* out);
    int (*release_image)(void* context,const char* key);
} TCComponentRenderApiV7;

/* V8 closes the resource-lifetime gaps left by V7: custom TTF faces live in a
   private atlas per resource, images may be scoped to one live instance, and a
   Mod can observe renderer-device generations without ever receiving a native
   GL id.  Loader-owned decoded pixels are re-uploaded when the GL context
   changes; handles remain stable. */
#define TC_COMPONENT_RENDER_API_VERSION_8 8u
#define TC_COMPONENT_RENDER_FONT_VERSION_1 1u
#define TC_COMPONENT_RENDER_DEVICE_EVENT_VERSION_1 1u
#define TC_COMPONENT_RENDER_DEVICE_REBUILT 1u
#define TC_COMPONENT_RENDER_MAX_FONTS 8u
#define TC_COMPONENT_RENDER_MAX_FONT_FILE_BYTES (16u*1024u*1024u)
#define TC_COMPONENT_RENDER_MAX_FONT_RANGES 16u
#define TC_COMPONENT_RENDER_ERR_FONT (-8)

struct TCComponentInstanceHandle;

typedef struct TCComponentRenderFontRangeV1 {
    uint32_t first; /* inclusive Unicode scalar; V8 accepts BMP ranges */
    uint32_t last;  /* inclusive */
} TCComponentRenderFontRangeV1;

typedef struct TCComponentRenderFontV1 {
    uint32_t size;
    uint32_t version;
    uint64_t font;
    float native_size;
    uint32_t revision;
    uint32_t reserved;
} TCComponentRenderFontV1;

typedef struct TCComponentRenderDeviceEventV1 {
    uint32_t size;
    uint32_t version;
    uint64_t generation;
    uint32_t reason; /* TC_COMPONENT_RENDER_DEVICE_REBUILT */
    uint32_t reserved;
} TCComponentRenderDeviceEventV1;

typedef void (*TCComponentRenderDeviceCallbackV1)(
    void* user,const TCComponentRenderDeviceEventV1* event);

typedef struct TCComponentRenderApiV8 {
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
    int (*load_image)(void* context,const char* key,const char* png_path,
                      TCComponentRenderImageV1* out);
    int (*release_image)(void* context,const char* key);
    int (*load_font)(void* context,const char* key,const char* ttf_path,
                     float native_size,const TCComponentRenderFontRangeV1* ranges,
                     uint32_t range_count,TCComponentRenderFontV1* out);
    int (*release_font)(void* context,const char* key);
    int (*load_instance_image)(void* context,const struct TCComponentInstanceHandle* instance,
                               const char* key,const char* png_path,
                               TCComponentRenderImageV1* out);
    int (*release_instance_image)(void* context,const struct TCComponentInstanceHandle* instance,
                                  const char* key);
    int (*set_device_callback)(void* context,
                               TCComponentRenderDeviceCallbackV1 callback,void* user);
    int (*device_generation)(void* context,uint64_t* generation);
} TCComponentRenderApiV8;

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
    /* Appended with the placement cut (2026-09-28).  Each runs with its own
       binding lock held, like on_create/on_destroy, so they must not call
       services; they read the new placement from TCLogicIOV2's geometry tail. */
    TCLogicCallbackV2 on_move;             /* TC_LOGIC_MOVE */
    TCLogicCallbackV2 on_rotate;           /* TC_LOGIC_ROTATE */
    TCLogicCallbackV2 on_resize;           /* TC_LOGIC_RESIZE */
    /* The board compiled with this instance in it (TC_LOGIC_COMPILE), and the
       host dropped the binding without the instance being deleted - the board
       went away, or the loader is shutting down (TC_LOGIC_UNBIND). */
    TCLogicCallbackV2 on_compile;          /* TC_LOGIC_COMPILE */
    TCLogicCallbackV2 on_unbind;           /* TC_LOGIC_UNBIND */
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

/* V2 keeps the complete V1 prefix and adds the one question a Mod could not
   answer from the instance table alone: which id the game's *selection set* and
   its own top-level lookups address this component by.

   A custom component can hold another custom component in its internal circuit.
   The generated callback is addressed by the inner instance's own 64-bit id,
   while the game's selected-component hash set, its "top level" walks and its
   undo bookkeeping address the enclosing component - so a Mod that reacts to a
   click on its own component has to translate one into the other.

   That pair is produced by the loader's own compile walk, which is why it used
   to be published only as the `interactive-map.txt` diagnostic file next to the
   game (examples/clock parsed it, three directories up from its data folder).
   The call below is that same value through a supported interface: it is the
   component's own id when it sits directly on the board, otherwise the id of
   the custom component that contains it.  A caller that needs the sequence
   index the selection set uses as a *key* still reads it from the Board
   snapshot (`tc.board`), exactly like before - this call only removes the need
   to read a diagnostic file or to walk component records by hand. */
#define TC_COMPONENT_INSTANCES_API_VERSION_2 2u

typedef struct TCComponentInstancesApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*enumerate)(void* context,uint64_t custom_id,TCComponentInstanceHandle* out,
                     uint32_t capacity,uint32_t* written,uint32_t* total);
    int (*validate)(void* context,const TCComponentInstanceHandle* handle);
    int (*info)(void* context,const TCComponentInstanceHandle* handle,
                TCComponentInstanceInfoV1* out);
    int (*state)(void* context,const TCComponentInstanceHandle* handle,uint64_t* out,
                 uint32_t capacity,uint32_t* words);
    int (*reset)(void* context,const TCComponentInstanceHandle* handle);
    /* The id the game addresses this component by when it is selected or walked
       from the top: the instance's own id for a component placed directly on
       the board, otherwise the containing component's id.  `out` receives 0
       when the handle is stale (the call answers ERR_STALE then).  The value is
       captured at compile time, so asking for an instance the loader never
       bound is impossible - a handle only exists for a bound instance. */
    int (*top_level_id)(void* context,const TCComponentInstanceHandle* handle,uint64_t* out);
} TCComponentInstancesApiV2;

/* ---------------------------------------------------------------------------
   tc.component.input: host-routed input for custom components.

   A Mod registers one local-space rectangle and one callback for each of its
   component types.  The host performs the board/screen transform, chooses the
   topmost live instance and keeps hover, pointer-capture and keyboard-focus
   state.  Callbacks therefore never poll ImGui or game globals.  Returning a
   TC_COMPONENT_INPUT_RESULT_* mask is the only way an event changes routing.

   Registration is type-scoped and is only accepted while tc_mod_load runs.
   Handles in events are generation checked.  Focus and capture are discarded
   when the instance disappears, its owner unloads, or the board changes. */
#define TC_SERVICE_COMPONENT_INPUT "tc.component.input"
#define TC_COMPONENT_INPUT_API_VERSION_1 1u
#define TC_COMPONENT_INPUT_EVENT_VERSION_1 1u

#define TC_COMPONENT_INPUT_OK 0
#define TC_COMPONENT_INPUT_ERR_UNAVAILABLE (-1)
#define TC_COMPONENT_INPUT_ERR_ARGUMENT (-2)
#define TC_COMPONENT_INPUT_ERR_OWNERSHIP (-3)
#define TC_COMPONENT_INPUT_ERR_STALE (-4)
#define TC_COMPONENT_INPUT_ERR_STATE (-5)

#define TC_COMPONENT_INPUT_POINTER_ENTER 1u
#define TC_COMPONENT_INPUT_POINTER_LEAVE 2u
#define TC_COMPONENT_INPUT_POINTER_MOVE 3u
#define TC_COMPONENT_INPUT_POINTER_DOWN 4u
#define TC_COMPONENT_INPUT_POINTER_UP 5u
#define TC_COMPONENT_INPUT_POINTER_CLICK 6u
#define TC_COMPONENT_INPUT_POINTER_DOUBLE_CLICK 7u
#define TC_COMPONENT_INPUT_DRAG_BEGIN 8u
#define TC_COMPONENT_INPUT_DRAG_MOVE 9u
#define TC_COMPONENT_INPUT_DRAG_END 10u
#define TC_COMPONENT_INPUT_POINTER_WHEEL 11u
#define TC_COMPONENT_INPUT_KEY_DOWN 12u
#define TC_COMPONENT_INPUT_KEY_UP 13u
#define TC_COMPONENT_INPUT_TEXT_COMMIT 14u
#define TC_COMPONENT_INPUT_IME_BEGIN 15u
#define TC_COMPONENT_INPUT_IME_UPDATE 16u
#define TC_COMPONENT_INPUT_IME_END 17u
#define TC_COMPONENT_INPUT_FOCUS_GAINED 18u
#define TC_COMPONENT_INPUT_FOCUS_LOST 19u
#define TC_COMPONENT_INPUT_CAPTURE_LOST 20u

#define TC_COMPONENT_INPUT_MOD_CTRL (1u << 0)
#define TC_COMPONENT_INPUT_MOD_SHIFT (1u << 1)
#define TC_COMPONENT_INPUT_MOD_ALT (1u << 2)
#define TC_COMPONENT_INPUT_MOD_SUPER (1u << 3)
#define TC_COMPONENT_INPUT_EVENT_REPEAT (1u << 0)

#define TC_COMPONENT_INPUT_RESULT_CONSUMED (1u << 0)
#define TC_COMPONENT_INPUT_RESULT_CAPTURE_POINTER (1u << 1)
#define TC_COMPONENT_INPUT_RESULT_RELEASE_POINTER (1u << 2)
#define TC_COMPONENT_INPUT_RESULT_FOCUS_KEYBOARD (1u << 3)
#define TC_COMPONENT_INPUT_RESULT_RELEASE_KEYBOARD (1u << 4)

#define TC_COMPONENT_INPUT_STATE_HOVERED (1u << 0)
#define TC_COMPONENT_INPUT_STATE_POINTER_CAPTURED (1u << 1)
#define TC_COMPONENT_INPUT_STATE_KEYBOARD_FOCUSED (1u << 2)

typedef struct TCComponentInputEventV1 {
    uint32_t size;
    uint32_t version;
    uint32_t kind;             /* TC_COMPONENT_INPUT_* event kind */
    uint32_t flags;            /* TC_COMPONENT_INPUT_EVENT_* */
    uint64_t sequence;         /* strictly increasing within this host run */
    TCComponentInstanceHandle instance;
    float screen_x;
    float screen_y;
    float board_x;
    float board_y;
    float local_x;
    float local_y;
    float delta_x;
    float delta_y;
    float wheel_x;
    float wheel_y;
    uint32_t button;           /* 0 left, 1 right, 2 middle; UINT32_MAX if none */
    uint32_t key;              /* ImGuiKey value; 0 if none */
    uint32_t scancode;         /* platform scancode when available */
    uint32_t modifiers;        /* TC_COMPONENT_INPUT_MOD_* */
    uint32_t codepoint;        /* TEXT_COMMIT scalar; 0 for UTF-8/IME chunks */
    uint32_t text_size;        /* bytes in text, excluding its trailing zero */
    char text[64];             /* UTF-8 commit/composition chunk */
} TCComponentInputEventV1;

typedef uint32_t (*TCComponentInputCallbackV1)(void* user,
                                               const TCComponentInputEventV1* event);

typedef struct TCComponentInputStateV1 {
    uint32_t size;
    uint32_t version;
    uint32_t flags;            /* TC_COMPONENT_INPUT_STATE_* */
    uint32_t reserve;
    TCComponentInstanceHandle instance;
} TCComponentInputStateV1;

typedef struct TCComponentInputApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Local coordinates use the same origin/axes as TCComponentRenderFrameV1.
       Later calls replace the rectangle/callback for this type. */
    int (*set_handler)(void* context,uint64_t custom_id,
                       float min_x,float min_y,float max_x,float max_y,
                       TCComponentInputCallbackV1 callback,void* user);
    int (*clear_handler)(void* context,uint64_t custom_id);
    /* Explicit focus/capture control outside a callback.  Only the three
       RESULT focus/capture bits are accepted in `requests`. */
    int (*route)(void* context,const TCComponentInstanceHandle* instance,
                 uint32_t requests);
    int (*state)(void* context,const TCComponentInstanceHandle* instance,
                 TCComponentInputStateV1* out);
} TCComponentInputApiV1;

/* ---------------------------------------------------------------------------
   tc.component.channel: bounded instance-local render/simulation exchange.

   Both queues and the snapshot are host-owned.  Operations never wait: when
   another thread currently owns the channel they return ERR_BUSY.  A full
   queue applies the configured policy and records the drop.  Every call
   validates the complete instance handle, so a recycled instance id cannot
   observe bytes from its predecessor. */
#define TC_SERVICE_COMPONENT_CHANNEL "tc.component.channel"
#define TC_COMPONENT_CHANNEL_API_VERSION_1 1u
#define TC_COMPONENT_CHANNEL_CONFIG_VERSION_1 1u
#define TC_COMPONENT_CHANNEL_STATS_VERSION_1 1u

#define TC_COMPONENT_CHANNEL_OK 0
#define TC_COMPONENT_CHANNEL_ERR_UNAVAILABLE (-1)
#define TC_COMPONENT_CHANNEL_ERR_ARGUMENT (-2)
#define TC_COMPONENT_CHANNEL_ERR_OWNERSHIP (-3)
#define TC_COMPONENT_CHANNEL_ERR_STALE (-4)
#define TC_COMPONENT_CHANNEL_ERR_SIZE (-5)
#define TC_COMPONENT_CHANNEL_ERR_FULL (-6)
#define TC_COMPONENT_CHANNEL_ERR_EMPTY (-7)
#define TC_COMPONENT_CHANNEL_ERR_BUSY (-8)
#define TC_COMPONENT_CHANNEL_ERR_STATE (-9)

#define TC_COMPONENT_CHANNEL_RENDER_TO_SIM 1u
#define TC_COMPONENT_CHANNEL_SIM_TO_RENDER 2u
#define TC_COMPONENT_CHANNEL_DROP_NEWEST 0u
#define TC_COMPONENT_CHANNEL_DROP_OLDEST 1u

typedef struct TCComponentChannelConfigV1 {
    uint32_t size;
    uint32_t version;
    uint32_t message_capacity; /* 1..256 messages in each direction */
    uint32_t message_bytes;    /* 1..65536 bytes per message */
    uint32_t snapshot_bytes;   /* 0..1048576 bytes */
    uint32_t drop_policy;      /* TC_COMPONENT_CHANNEL_DROP_* */
} TCComponentChannelConfigV1;

typedef struct TCComponentChannelMessageV1 {
    uint32_t size;
    uint32_t version;
    uint64_t sequence;
    uint32_t bytes;
    uint32_t reserve;
} TCComponentChannelMessageV1;

typedef struct TCComponentChannelStatsV1 {
    uint32_t size;
    uint32_t version;
    uint32_t render_to_sim_queued;
    uint32_t sim_to_render_queued;
    uint64_t render_to_sim_sent;
    uint64_t sim_to_render_sent;
    uint64_t render_to_sim_dropped;
    uint64_t sim_to_render_dropped;
    uint64_t snapshot_revision;
    uint32_t snapshot_bytes;
    uint32_t reserve;
} TCComponentChannelStatsV1;

typedef struct TCComponentChannelApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*open)(void* context,const TCComponentInstanceHandle* instance,
                const TCComponentChannelConfigV1* config);
    int (*close)(void* context,const TCComponentInstanceHandle* instance);
    int (*send)(void* context,const TCComponentInstanceHandle* instance,uint32_t direction,
                const void* data,uint32_t bytes,uint64_t* sequence);
    /* A count-only receive (`out == NULL`, capacity == 0) reports the front
       message without removing it.  ERR_SIZE also leaves it queued. */
    int (*receive)(void* context,const TCComponentInstanceHandle* instance,uint32_t direction,
                   void* out,uint32_t capacity,TCComponentChannelMessageV1* message);
    int (*publish_snapshot)(void* context,const TCComponentInstanceHandle* instance,
                            const void* data,uint32_t bytes,uint64_t* revision);
    /* `known_revision` lets a render callback skip an unchanged copy.  The
       current metadata is still returned; ERR_EMPTY means no snapshot yet. */
    int (*read_snapshot)(void* context,const TCComponentInstanceHandle* instance,
                         uint64_t known_revision,void* out,uint32_t capacity,
                         uint32_t* bytes,uint64_t* revision);
    int (*stats)(void* context,const TCComponentInstanceHandle* instance,
                 TCComponentChannelStatsV1* out);
} TCComponentChannelApiV1;

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

/* tc.component.editor V1 - the declarative half of a component's property
   editor (plan §14).

   A type declares the fields of its own configuration once, during
   `tc_mod_load`; from then on the host draws one row per field over the game's
   own component drawer whenever one of that type's instances is selected, and
   commits each edit through the same path `tc.component.storage` writes use.
   One committed field is one undo step (Ctrl+Z reverts it), and the type's
   `on_config_changed` fires exactly as it does for `write_config`.

   Why the host draws the rows: measured on this build, the game's drawer is
   never the ImGui window under the mouse while the pointer sits on a row a
   plugin painted inside it (`IsWindowHovered` is false for it), so widgets drawn
   straight into the drawer are visible but dead.  Every Mod that wants working
   controls therefore opens its own window layered over the panel - the recipe in
   docs/sdk/ui.md and examples/float-ops/components_ui.cpp.  This service is that
   recipe, owned by the host, so a type gets a working editor from a field table.

   A Mod that wants a custom editor keeps using
   TC_UI_SLOT_BOARD_COMPONENT_PANEL; the host's rows are drawn after the slot
   callbacks, so the two can coexist.

   Field offsets are byte offsets into the instance's configuration blob (the
   same bytes TCLogicIOV2::config and tc.component.storage see).  The blob is a
   byte layout, not a C struct: the loader reads and writes it little-endian, so
   a field can be declared without matching the compiler's padding.

   ```c
   static const TCComponentFieldV1 kFields[] = {
       {"enabled","Enabled",NULL,TC_COMPONENT_FIELD_BOOL,0,0,1,0,0,NULL,0,0,0,0},
       {"mode","Rounding",NULL,TC_COMPONENT_FIELD_ENUM,0,4,1,0,5,kModes,0,0,0,0},
       {"label","Label",NULL,TC_COMPONENT_FIELD_TEXT,0,8,24,0,0,NULL,0,0,0,0},
   };
   TCComponentEditorApiV1 editor{};
   if (host->query_service(host->context, TC_SERVICE_COMPONENT_EDITOR,
                           TC_COMPONENT_EDITOR_API_VERSION_1,
                           &editor, sizeof(editor)) == TC_SERVICE_OK)
       editor.set_fields(editor.context, kMyTypeId, kFields,
                         (uint32_t)(sizeof(kFields)/sizeof(kFields[0])));
   ``` */
#define TC_SERVICE_COMPONENT_EDITOR "tc.component.editor"
#define TC_COMPONENT_EDITOR_API_VERSION_1 1u

/* Field kinds.  Which of a field's other members matter depends on the kind. */
#define TC_COMPONENT_FIELD_BOOL 1u   /* 1 byte, drawn as a checkbox */
#define TC_COMPONENT_FIELD_INT 2u    /* 1, 2 or 4 bytes, signed, decimal box */
#define TC_COMPONENT_FIELD_UINT 3u   /* 1, 2 or 4 bytes, unsigned; `bits` may narrow it */
#define TC_COMPONENT_FIELD_ENUM 4u   /* 1, 2 or 4 byte index into `choice_labels` */
#define TC_COMPONENT_FIELD_TEXT 5u   /* `bytes` of UTF-8, NUL-terminated */
#define TC_COMPONENT_FIELD_FLOAT 6u  /* 4 bytes, IEEE-754 binary32 */
/* 2, 4 or 8 bytes, IEEE-754 binary16/binary32/binary64 typed in *decimal*: the
   same box as FLOAT, for the widths four bytes cannot hold.  The row shows the
   shortest decimal that parses back to the stored pattern, and it accepts an
   ordinary decimal, scientific notation, inf/-inf/nan and -0; text that is not
   a number is refused and the configuration keeps its old value.  Payloads and
   exact bit patterns stay the job of a UINT field with TC_COMPONENT_FIELD_HEX. */
#define TC_COMPONENT_FIELD_FLOAT_WIDTH 7u

/* The row is drawn but cannot be edited (a derived or purely informative value). */
#define TC_COMPONENT_FIELD_READ_ONLY (1u<<0)
/* UINT: show and log the value in hexadecimal.  A field with this flag is drawn
   read-only in V1 (the V1 widget is a signed decimal box). */
#define TC_COMPONENT_FIELD_HEX (1u<<1)

/* Limits: at most 32 rows per type, and the decimal integer widget is a signed
   32-bit box, so a UINT field declares at most 31 meaningful bits unless it
   asks for the V2 hexadecimal widget (which reaches 64).  A text field is at
   most 64 bytes including its terminator. */
#define TC_COMPONENT_EDITOR_MAX_FIELDS 32u
#define TC_COMPONENT_EDITOR_MAX_TEXT_BYTES 64u

/* One declared row.  The array is copied during the call, so a Mod may keep it
   on the stack; the strings must stay valid for the duration of the call. */
typedef struct TCComponentFieldV1 {
    const char* field_id;            /* stable id inside the type; names the widget */
    const char* label;               /* shown at the left of the row */
    const char* tooltip;             /* shown on hover; may be NULL */
    uint32_t kind;                   /* TC_COMPONENT_FIELD_* */
    uint32_t flags;                  /* TC_COMPONENT_FIELD_* flags */
    uint32_t offset;                 /* byte offset into the configuration blob */
    uint32_t bytes;                  /* width of the field in bytes */
    uint32_t bits;                   /* UINT: meaningful bits; 0 means bytes*8 */
    uint32_t choices;                /* ENUM: number of entries in choice_labels */
    const char* const* choice_labels;/* ENUM: one label per index */
    int64_t min_value;               /* INT/UINT: inclusive clamp; min >= max means "the width's range" */
    int64_t max_value;
    double min_float;                /* FLOAT: inclusive clamp; min >= max means "no clamp" */
    double max_float;
} TCComponentFieldV1;

#define TC_COMPONENT_EDITOR_OK 0
#define TC_COMPONENT_EDITOR_ERR_UNAVAILABLE (-1) /* this loader has no tc.component.editor */
#define TC_COMPONENT_EDITOR_ERR_ARGUMENT (-2)    /* null, undersized or malformed */
#define TC_COMPONENT_EDITOR_ERR_OWNERSHIP (-3)   /* the type is not this Mod's */
#define TC_COMPONENT_EDITOR_ERR_FIELD (-4)       /* a field does not fit the configuration */
#define TC_COMPONENT_EDITOR_ERR_LIMIT (-5)       /* more fields than the host accepts */
#define TC_COMPONENT_EDITOR_ERR_THREAD (-6)      /* outside tc_mod_load / not the game thread */

typedef struct TCComponentEditorApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Declares (or replaces) the editable rows of one of the calling Mod's
       registered types.  Refused with ERR_FIELD unless every field fits the
       type's registered configuration, ERR_OWNERSHIP for another Mod's type,
       ERR_LIMIT past TC_COMPONENT_EDITOR_MAX_FIELDS, and ERR_THREAD outside the
       game thread's tc_mod_load window.  Declaring no rows for a type is the
       same as never calling this: the Mod keeps whatever editor it draws in its
       TC_UI_SLOT_BOARD_COMPONENT_PANEL slot, and there is no host editor. */
    int (*set_fields)(void* context,uint64_t custom_id,
                      const TCComponentFieldV1* fields,uint32_t count);
    /* Removes the type's rows; the host stops drawing an editor for it. */
    int (*clear_fields)(void* context,uint64_t custom_id);
} TCComponentEditorApiV1;

/* tc.component.editor V2 - the same two calls, with a richer field table and a
   scrolling row window.

   Everything V1 declared keeps its meaning, so a Mod that only wants V1's
   behaviour can stay on V1.  V2 is what says "this loader understands the flags
   below"; a V1 loader would ignore them silently, which is why a Mod that uses
   them has to ask for V2 and fall back when it is not there.

   Layout flags (they only change where a row is drawn, never what it edits):

     FULL_ROW   the control takes the whole width on the line below its label.
                Use it for text and for anything whose value deserves the room.
     INLINE     this field shares the line with the *previous* one, so two short
                fields read as a pair.  A row that starts a line cannot be
                INLINE; the host refuses the table rather than guessing.
     SEPARATOR  draw a separator line above this row - the cheap way to group a
                long table.
     RADIO      ENUM only: draw every choice as a radio button across the row
                instead of the cycling button.  This is the shape the game's own
                panels use for a handful of named choices, and what
                examples/float-ops draws by hand today.

   Control flags:

     HEX        UINT only: the value is edited as hexadecimal text ("0x1f40"),
                which also raises the width ceiling from 31 bits (the signed
                decimal box) to 64.  A value that cannot be parsed is refused
                with a log line and the configuration keeps its old bytes.

   The row window scrolls: when the declared rows are taller than the space
   under the game's own drawer, the host caps the window at the screen and lets
   ImGui scroll it, instead of drawing rows where nothing can click them. */
#define TC_COMPONENT_EDITOR_API_VERSION_2 2u

/* Draw the control across the full width, on the line below its label. */
#define TC_COMPONENT_FIELD_FULL_ROW (1u<<2)
/* Share the line with the previous field. */
#define TC_COMPONENT_FIELD_INLINE (1u<<3)
/* A separator line above this row. */
#define TC_COMPONENT_FIELD_SEPARATOR (1u<<4)
/* ENUM: one radio button per choice across the row. */
#define TC_COMPONENT_FIELD_RADIO (1u<<5)
/* Every flag this ABI knows; anything else is refused, not ignored. */
#define TC_COMPONENT_FIELD_V2_KNOWN_FLAGS \
    (TC_COMPONENT_FIELD_READ_ONLY|TC_COMPONENT_FIELD_HEX|TC_COMPONENT_FIELD_FULL_ROW| \
     TC_COMPONENT_FIELD_INLINE|TC_COMPONENT_FIELD_SEPARATOR|TC_COMPONENT_FIELD_RADIO)
#define TC_COMPONENT_FIELD_KNOWN_FLAGS \
    TC_COMPONENT_FIELD_V2_KNOWN_FLAGS

/* A UINT field may be at most this wide when it is edited as hexadecimal, and
   at most the decimal widget's 31 bits otherwise. */
#define TC_COMPONENT_EDITOR_MAX_HEX_BITS 64u

typedef struct TCComponentEditorApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_fields)(void* context,uint64_t custom_id,
                      const TCComponentFieldV1* fields,uint32_t count);
    int (*clear_fields)(void* context,uint64_t custom_id);
} TCComponentEditorApiV2;

/* tc.component.editor V3 - richer controls and host-owned interaction.

   V3 deliberately adds a second field descriptor instead of growing
   TCComponentFieldV1.  V1/V2 tables therefore keep their frozen layout, while
   a V3 caller can opt into validation, a custom painted row, context actions,
   double-click and a resize gesture.  Callback pointers are borrowed for the
   lifetime of the loaded Mod; every string/choice/action label is copied by the
   host during set_fields_v2(). */
#define TC_COMPONENT_EDITOR_API_VERSION_3 3u
#define TC_COMPONENT_FIELD_DEFINITION_VERSION_2 2u

/* Four little-endian bytes in the same 0xAABBGGRR form used by the render API. */
#define TC_COMPONENT_FIELD_COLOR_RGBA8 8u
/* No bytes are decoded by the host.  The row is an optional custom canvas and
   interaction target; offset/bytes may be zero. */
#define TC_COMPONENT_FIELD_CUSTOM 9u

/* ENUM: use an ordinary combo/dropdown.  With neither DROPDOWN nor RADIO the
   compact cycling button from V1 is retained. */
#define TC_COMPONENT_FIELD_DROPDOWN (1u<<6)
/* CUSTOM: the row owns a bottom-right resize grip.  The host opens one storage
   edit on press and commits it on release, so all writes made by the action
   callback during the drag become one Undo step. */
#define TC_COMPONENT_FIELD_RESIZE_HANDLE (1u<<7)

#undef TC_COMPONENT_FIELD_KNOWN_FLAGS
#define TC_COMPONENT_FIELD_KNOWN_FLAGS \
    (TC_COMPONENT_FIELD_READ_ONLY|TC_COMPONENT_FIELD_HEX|TC_COMPONENT_FIELD_FULL_ROW| \
     TC_COMPONENT_FIELD_INLINE|TC_COMPONENT_FIELD_SEPARATOR|TC_COMPONENT_FIELD_RADIO| \
     TC_COMPONENT_FIELD_DROPDOWN|TC_COMPONENT_FIELD_RESIZE_HANDLE)
/* The V3 set, spelled out rather than aliased to the name above: that name is
   redefined below, and a macro keeps expanding to whatever it means at the
   point of use - which would quietly hand a V3 caller the V4 bits.  A table
   declared through the V3 entry point stays limited to this set. */
#define TC_COMPONENT_FIELD_V3_KNOWN_FLAGS \
    (TC_COMPONENT_FIELD_READ_ONLY|TC_COMPONENT_FIELD_HEX|TC_COMPONENT_FIELD_FULL_ROW| \
     TC_COMPONENT_FIELD_INLINE|TC_COMPONENT_FIELD_SEPARATOR|TC_COMPONENT_FIELD_RADIO| \
     TC_COMPONENT_FIELD_DROPDOWN|TC_COMPONENT_FIELD_RESIZE_HANDLE)

#define TC_COMPONENT_EDITOR_VALIDATION_OK 0
#define TC_COMPONENT_EDITOR_VALIDATION_WARNING 1
#define TC_COMPONENT_EDITOR_VALIDATION_ERROR (-1)
#define TC_COMPONENT_EDITOR_MAX_VALIDATION_BYTES 192u
#define TC_COMPONENT_EDITOR_MAX_CONTEXT_ACTIONS 8u

typedef int (*TCComponentEditorValidateV1)(
    void* user,const TCComponentInstanceHandle* instance,const char* field_id,
    const uint8_t* proposed_config,uint32_t config_size,char* message,
    uint32_t message_capacity);

/* Action kinds delivered by TCComponentEditorActionCallbackV1.  MENU uses the
   caller-provided action id.  The remaining kinds use action_id == 0. */
#define TC_COMPONENT_EDITOR_ACTION_MENU 1u
#define TC_COMPONENT_EDITOR_ACTION_DOUBLE_CLICK 2u
#define TC_COMPONENT_EDITOR_ACTION_RESIZE_BEGIN 3u
#define TC_COMPONENT_EDITOR_ACTION_RESIZE 4u
#define TC_COMPONENT_EDITOR_ACTION_RESIZE_END 5u
#define TC_COMPONENT_EDITOR_ACTION_RESIZE_CANCEL 6u

typedef struct TCComponentEditorActionV1 {
    uint32_t size;
    uint32_t version;
    TCComponentInstanceHandle instance;
    const char* field_id;
    uint32_t kind;       /* TC_COMPONENT_EDITOR_ACTION_* */
    uint32_t action_id;  /* caller id for MENU; otherwise zero */
    float screen_x;
    float screen_y;
    float delta_x;       /* RESIZE: total pixels since press */
    float delta_y;
    const uint8_t* config; /* borrowed snapshot, valid only in the callback */
    uint32_t config_size;
    uint32_t reserved;
} TCComponentEditorActionV1;

typedef void (*TCComponentEditorActionCallbackV1)(
    void* user,const TCComponentEditorActionV1* action);

/* A custom row is a clipped render canvas.  The draw table is frame-scoped and
   uses screen pixels, just like tc.component.render.  Interaction stays with
   the host and arrives through the action callback above. */
typedef struct TCComponentEditorCustomRowV1 {
    uint32_t size;
    uint32_t version;
    TCComponentInstanceHandle instance;
    const char* field_id;
    float min_x,min_y,max_x,max_y;
    const uint8_t* config;
    uint32_t config_size;
    uint32_t reserved;
    const TCComponentRenderDrawV1* draw;
} TCComponentEditorCustomRowV1;

typedef void (*TCComponentEditorCustomRowCallbackV1)(
    void* user,const TCComponentEditorCustomRowV1* row);

typedef struct TCComponentFieldV2 {
    uint32_t size;
    uint32_t version;
    TCComponentFieldV1 field; /* complete V1/V2 field definition */
    TCComponentEditorValidateV1 validate;
    TCComponentEditorCustomRowCallbackV1 draw_custom;
    TCComponentEditorActionCallbackV1 action;
    void* user;
    float custom_height; /* CUSTOM row height in pixels; 0 uses two text lines */
    uint32_t context_actions;
    const uint32_t* context_action_ids;
    const char* const* context_action_labels;
    uint32_t reserved;
} TCComponentFieldV2;

typedef struct TCComponentEditorApiV3 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_fields)(void* context,uint64_t custom_id,
                      const TCComponentFieldV1* fields,uint32_t count);
    int (*clear_fields)(void* context,uint64_t custom_id);
    int (*set_fields_v2)(void* context,uint64_t custom_id,
                         const TCComponentFieldV2* fields,uint32_t count);
    /* Explicit multi-field transaction.  These are the storage V2 operations
       scoped through the editor service, useful from menu/double-click/custom
       callbacks.  The resize grip performs the same begin/commit automatically. */
    int (*begin_edit)(void* context,const TCComponentInstanceHandle* instance);
    int (*commit_edit)(void* context,const TCComponentInstanceHandle* instance);
    int (*abort_edit)(void* context,const TCComponentInstanceHandle* instance);
} TCComponentEditorApiV3;

/* tc.component.editor V4 - the rest of the editor shapes the plan asks for.

   V4 adds no field descriptor: its table is still TCComponentFieldV2, so a V3
   table keeps its frozen meaning.  What V4 adds is

     * two more ways to pick a colour - a swatch strip the Mod declares and an
       editable hexadecimal box beside the colour control - on top of V3's ImGui
       colour widget, which stays the default;
     * a one-line validation marker instead of a full line of text under the row;
     * one table-level flag: stage the table's edits in the host's working copy
       and write them with a single Apply, so a batch of fields is one undo step.

   Asking for V4 (or calling set_fields_v3) is what says "this loader knows the
   flags below"; a V3 loader refuses them with ERR_FIELD rather than drawing
   something else, and a Mod that wants to keep working on one has to fall back
   to the V4-free table it used before. */
#define TC_COMPONENT_EDITOR_API_VERSION_4 4u

/* COLOR_RGBA8: the row's own control is the Mod's swatch list - `choices` and
   `choice_labels` carry one colour literal per swatch, "#RRGGBB" or
   "#RRGGBBAA" (a leading '#' or "0x" is optional, and "#RGB" is expanded).
   Clicking a swatch commits that colour.  The strip is drawn before the colour
   widget, so PALETTE and COLOR_HEX compose into one mixed row. */
#define TC_COMPONENT_FIELD_COLOR_PALETTE (1u<<8)
/* COLOR_RGBA8: the row also carries an editable text box holding the packed
   value as "0xAABBGGRR" - the exact-bit escape hatch a colour field otherwise
   lacks, the way TC_COMPONENT_FIELD_HEX is one for a UINT.  Text that is not a
   colour (or a hexadecimal word) is refused and the configuration keeps its
   bytes. */
#define TC_COMPONENT_FIELD_COLOR_HEX (1u<<9)
/* Any kind: validation is drawn as a small marker at the end of the row instead
   of a "Error: ..." line under it.  The message appears on hover and is still
   written to the log, so nothing is lost - only the row's height. */
#define TC_COMPONENT_FIELD_VALIDATION_ICON (1u<<10)

#undef TC_COMPONENT_FIELD_KNOWN_FLAGS
#define TC_COMPONENT_FIELD_KNOWN_FLAGS \
    (TC_COMPONENT_FIELD_READ_ONLY|TC_COMPONENT_FIELD_HEX|TC_COMPONENT_FIELD_FULL_ROW| \
     TC_COMPONENT_FIELD_INLINE|TC_COMPONENT_FIELD_SEPARATOR|TC_COMPONENT_FIELD_RADIO| \
     TC_COMPONENT_FIELD_DROPDOWN|TC_COMPONENT_FIELD_RESIZE_HANDLE| \
     TC_COMPONENT_FIELD_COLOR_PALETTE|TC_COMPONENT_FIELD_COLOR_HEX| \
     TC_COMPONENT_FIELD_VALIDATION_ICON)

/* How many swatches one palette row may declare.  Sixteen fits across the
   drawer at the dark end of the game's window sizes, and a palette is a
   shortcut rather than a colour picker - a Mod with more than sixteen presets
   is really asking for a custom row. */
#define TC_COMPONENT_EDITOR_MAX_PALETTE_SWATCHES 16u

/* Table-level flags, passed to set_fields_v3().  Stage the table's edits in the
   host's working copy and draw Apply / Cancel at the bottom of the rows: no
   configuration is written until Apply, Cancel re-reads the instance (throwing
   the pending edits away), and Apply is one configuration write - one undo step
   and one on_config_changed - no matter how many fields were touched.

   A table that stages its edits may not also declare a CUSTOM row: such a row's
   callback writes the configuration itself through tc.component.storage, which
   would race the host's staged copy.  The table is refused rather than guessed
   at. */
#define TC_COMPONENT_EDITOR_TABLE_CONFIRM (1u<<0)
#define TC_COMPONENT_EDITOR_TABLE_KNOWN_FLAGS TC_COMPONENT_EDITOR_TABLE_CONFIRM

typedef struct TCComponentEditorApiV4 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_fields)(void* context,uint64_t custom_id,
                      const TCComponentFieldV1* fields,uint32_t count);
    int (*clear_fields)(void* context,uint64_t custom_id);
    int (*set_fields_v2)(void* context,uint64_t custom_id,
                         const TCComponentFieldV2* fields,uint32_t count);
    int (*begin_edit)(void* context,const TCComponentInstanceHandle* instance);
    int (*commit_edit)(void* context,const TCComponentInstanceHandle* instance);
    int (*abort_edit)(void* context,const TCComponentInstanceHandle* instance);
    /* The V3 field table plus the table-level flags above.  Unknown bits are
       refused with ERR_FIELD; the fields themselves are checked exactly as
       set_fields_v2() checks them. */
    int (*set_fields_v3)(void* context,uint64_t custom_id,
                         const TCComponentFieldV2* fields,uint32_t count,
                         uint32_t table_flags);
} TCComponentEditorApiV4;

/* Loader-owned storage for one Mod.  The host derives ownership from the
   service context: callers never pass a Mod id or a path and therefore cannot
   address another Mod's values.

   GLOBAL survives save-profile switches.  PROFILE lives inside the active
   isolated save profile, so duplicating/importing/removing that profile carries
   the data with it.  Keys are 1..64 lowercase ASCII bytes from [a-z0-9._-]. */
#define TC_SERVICE_MOD_STORAGE "tc.mod.storage"
#define TC_MOD_STORAGE_API_VERSION_1 1u
#define TC_MOD_STORAGE_API_VERSION_2 2u

#define TC_MOD_STORAGE_SCOPE_GLOBAL 1u
#define TC_MOD_STORAGE_SCOPE_PROFILE 2u

#define TC_MOD_STORAGE_OK 0
#define TC_MOD_STORAGE_ERR_UNAVAILABLE (-1)
#define TC_MOD_STORAGE_ERR_ARGUMENT (-2)
#define TC_MOD_STORAGE_ERR_NOT_FOUND (-3)
#define TC_MOD_STORAGE_ERR_SIZE (-4)
#define TC_MOD_STORAGE_ERR_IO (-5)
#define TC_MOD_STORAGE_ERR_QUOTA (-6)
#define TC_MOD_STORAGE_ERR_VERSION (-7)
#define TC_MOD_STORAGE_ERR_MIGRATION (-8)

#define TC_MOD_STORAGE_MAX_KEY_BYTES 64u
#define TC_MOD_STORAGE_MAX_VALUE_BYTES (1024u * 1024u)
#define TC_MOD_STORAGE_MAX_KEYS 256u
#define TC_MOD_STORAGE_MAX_SCOPE_BYTES (8u * 1024u * 1024u)

typedef struct TCModStorageApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    /* Count-only read: out == NULL and capacity == 0.  `bytes` receives the
       required size both for a count-only read and for ERR_SIZE. */
    int (*read)(void* context,uint32_t scope,const char* key,
                void* out,uint32_t capacity,uint32_t* bytes);
    /* Atomic whole-value replacement.  A zero-byte value is distinct from a
       missing key. */
    int (*write)(void* context,uint32_t scope,const char* key,
                 const void* data,uint32_t size);
    int (*remove)(void* context,uint32_t scope,const char* key);
    /* Returns 1 when present, 0 when absent, or a negative error. */
    int (*has)(void* context,uint32_t scope,const char* key);
} TCModStorageApiV1;

/* V2 adds schema metadata without changing what V1 readers see.  An existing
   V1/raw value has schema version 0.  The host calls one migration callback
   for the complete old -> target transition and commits its result with one
   atomic replacement; a rejected/throwing/oversized migration leaves both the
   old payload and its old version untouched. */
#define TC_MOD_STORAGE_MIGRATE_OK 0
#define TC_MOD_STORAGE_MIGRATE_REJECT 1
typedef int (*TCModStorageMigrateV1)(void* user,uint32_t from_version,
                                     uint32_t to_version,const void* old_data,
                                     uint32_t old_size,void* new_data,
                                     uint32_t capacity,uint32_t* new_size);

typedef struct TCModStorageApiV2 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*read)(void* context,uint32_t scope,const char* key,
                void* out,uint32_t capacity,uint32_t* bytes);
    int (*write)(void* context,uint32_t scope,const char* key,
                 const void* data,uint32_t size);
    int (*remove)(void* context,uint32_t scope,const char* key);
    int (*has)(void* context,uint32_t scope,const char* key);
    int (*get_version)(void* context,uint32_t scope,const char* key,
                       uint32_t* schema_version);
    int (*migrate)(void* context,uint32_t scope,const char* key,
                   uint32_t target_version,const void* default_data,
                   uint32_t default_size,TCModStorageMigrateV1 callback,
                   void* user,uint32_t* resulting_version);
} TCModStorageApiV2;

/* Declarative, loader-owned Mod settings.  Registration is allowed during
   tc_mod_load only.  Strings and choice labels are copied by the host.  Every
   field is persisted automatically in tc.mod.storage, under `storage_key` or
   `settings.<id>` when storage_key is null/empty. */
#define TC_SERVICE_MOD_SETTINGS "tc.mod.settings"
#define TC_MOD_SETTINGS_API_VERSION_1 1u
#define TC_MOD_SETTING_VALUE_VERSION_1 1u

#define TC_MOD_SETTING_BOOL 1u
#define TC_MOD_SETTING_INT 2u
#define TC_MOD_SETTING_FLOAT 3u
#define TC_MOD_SETTING_ENUM 4u
#define TC_MOD_SETTING_TEXT 5u

#define TC_MOD_SETTINGS_OK 0
#define TC_MOD_SETTINGS_ERR_UNAVAILABLE (-1)
#define TC_MOD_SETTINGS_ERR_ARGUMENT (-2)
#define TC_MOD_SETTINGS_ERR_DUPLICATE (-3)
#define TC_MOD_SETTINGS_ERR_CAPACITY (-4)
#define TC_MOD_SETTINGS_ERR_STORAGE (-5)
#define TC_MOD_SETTINGS_ERR_NOT_FOUND (-6)
#define TC_MOD_SETTINGS_ERR_TYPE (-7)
#define TC_MOD_SETTINGS_ERR_THREAD (-8)

#define TC_MOD_SETTINGS_MAX_FIELDS 64u
#define TC_MOD_SETTING_MAX_ID_BYTES 48u
#define TC_MOD_SETTING_MAX_TEXT_BYTES 255u
#define TC_MOD_SETTING_MAX_CHOICES 16u

typedef struct TCModSettingValueV1 {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    uint32_t reserved;
    int64_t integer;
    double number;
    uint32_t enum_index;
    uint32_t text_size;
    char text[TC_MOD_SETTING_MAX_TEXT_BYTES + 1u];
} TCModSettingValueV1;

typedef struct TCModSettingFieldV1 {
    uint32_t size;
    uint32_t type;
    uint32_t scope;
    uint32_t flags; /* V1 requires zero. */
    const char* id;
    const char* label;
    const char* description;
    const char* storage_key; /* optional; otherwise settings.<id> */
    int64_t minimum_integer;
    int64_t maximum_integer;
    int64_t step_integer;
    int64_t default_integer;
    double minimum_number;
    double maximum_number;
    double step_number;
    double default_number;
    const char* default_text;
    const char* const* choices;
    uint32_t choice_count;
    uint32_t reserved;
} TCModSettingFieldV1;

/* A return other than OK rejects a proposed UI/reset change before it is
   persisted.  The callback is a validator/notification, not an owner of value. */
typedef int (*TCModSettingChangedV1)(void* user,const char* setting_id,
                                     const TCModSettingValueV1* value);

typedef struct TCModSettingsApiV1 {
    uint32_t size;
    uint32_t version;
    void* context;
    int (*set_fields)(void* context,const TCModSettingFieldV1* fields,
                      uint32_t count,TCModSettingChangedV1 changed,void* user);
    int (*clear_fields)(void* context);
    int (*get)(void* context,const char* setting_id,
               TCModSettingValueV1* out,uint32_t out_size);
    int (*reset)(void* context,const char* setting_id);
} TCModSettingsApiV1;

#endif
