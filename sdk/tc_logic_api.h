#ifndef TC_LOGIC_API_H
#define TC_LOGIC_API_H
#include <stdint.h>

/* Native logic ABI, version 2. Callbacks run on the game's simulation thread.
   Do not call UI/model APIs, throw, or retain the IO pointer.

   Shape comes from the component definition the plugin imports: up to eight
   input pins and eight output pins per direction, each pin 1..64 bits wide
   (total input width up to 512 bits).  The loader replaces the internal gates
   of that component, so the callback sees the pin values and returns the pin
   outputs.  A definition is bridgeable while every output pin is driven by one
   logic gate and the first of those gates exposes an operand list matching the
   declared inputs; that list becomes the callback's input tuple. */
/* RESET / REFRESH / CYCLE are the simulation phases every V1 callback handles.
   CREATE / DESTROY belong to the V2 lifecycle callbacks
   (TCComponentLifecycleV1): they run where the instance appeared or went away,
   not on a simulation step, and state written there is committed immediately. */
enum TCLogicPhase {
    TC_LOGIC_RESET=0, TC_LOGIC_REFRESH=1, TC_LOGIC_CYCLE=2,
    TC_LOGIC_CREATE=3, TC_LOGIC_DESTROY=4,
    /* The instance's configuration was replaced: a write_config commit, or an
       undo/redo that restored an earlier one.  The callback sees the new bytes in
       its config view.  Loading a stored configuration (or migrating an old one)
       is part of binding, not a change, so it arrives through TC_LOGIC_CREATE
       with the bytes already in place. */
    TC_LOGIC_CONFIG_CHANGED=5,
    /* The instance was created by a duplication, so its configuration is the
       source's copy rather than the registered default.  It runs after
       TC_LOGIC_CREATE, and the callback may replace the configuration (that
       replacement is an ordinary write_config, with its own undo step). */
    TC_LOGIC_CLONE=6,
    /* The instance's configuration was installed from the circuit's own record, so
       it is what the archive carried rather than the registered default.  Fired
       after TC_LOGIC_CREATE (and instead of TC_LOGIC_CLONE for a copy).  A record
       the host had to refuse does not fire it: that instance runs on the default. */
    TC_LOGIC_LOAD=7,
    /* The game is about to write a circuit out.  A project that keeps something in
       derived form - or that wants to repair a configuration it could not read
       earlier - can commit it now with an ordinary write_config; the file that is
       being written already sees it. */
    TC_LOGIC_SAVE=8,
    /* The instance moved to another board cell (MOVE), or its direction byte
       changed (ROTATE).  Both are board edits the player made with the game's own
       tools; the callback sees the new placement in the IO's geometry tail. */
    TC_LOGIC_MOVE=9,
    TC_LOGIC_ROTATE=10,
    /* The instance's footprint changed: the Mod resized it through
       tc.component.geometry, or the host re-derived it. */
    TC_LOGIC_RESIZE=11,
    /* The board was compiled and this instance was part of it: the callback runs
       once per compile, after the bindings for that compile exist. */
    TC_LOGIC_COMPILE=12,
    /* The host dropped the binding although the instance itself was not deleted -
       the board went away (leaving the level) or the loader is shutting down.
       A component the player *deleted* arrives as TC_LOGIC_DESTROY instead. */
    TC_LOGIC_UNBIND=13
};
typedef struct TCLogicIO {
    uint32_t size, phase;
    uint64_t instance_id;
    int64_t cycle;
    uint32_t input_count, output_count;
    /* One full word per pin, already masked to that pin's width. */
    uint64_t inputs[8];
    /* Also the return value of the generated invoke call: outputs[0]. */
    uint64_t outputs[8];
    /* Eight persistent words, isolated per compiled instance. Refresh runs
       against a copy; only CYCLE writes are committed. RESET starts at zero. */
    uint64_t state[8];
    void* user;
} TCLogicIO;
typedef void (*TCLogicCallback)(TCLogicIO*);

/* ---- V2 IO: more than eight pins ------------------------------------------

   `TCLogicIO` carries fixed `inputs[8]` / `outputs[8]` / `state[8]`, so a shape
   with more pins needs a different struct.  V2 keeps the same phase contract and
   the same per-pin value (one uint64_t, already masked to the pin's width) but
   takes the counts and the arrays from the host, so the pin count is bounded by
   the loader's shape limits instead of by this structure.

   The arrays are borrowed: they are valid only for the duration of the call.
   Writes to `outputs` and `state` are committed after the CYCLE callback (and
   discarded for REFRESH, exactly like V1).  A pin is 1..64 bits wide; V2/V3
   accept up to 32 pins per direction and 2048 input bits in total.  Inputs above
   128 bits are staged by multiple four-argument JIT calls before the callback. */
#define TC_LOGIC_IO_V2_VERSION_1 1u
typedef struct TCLogicIOV2 {
    uint32_t size, version;
    uint32_t phase;                 /* one of TCLogicPhase */
    uint64_t instance_id;
    int64_t cycle;
    uint32_t input_count, output_count;
    const uint64_t* inputs;         /* one full word per pin, masked to its width */
    uint64_t* outputs;              /* outputs[0] is also the generated call's result */
    uint64_t* state;                /* state_words simulation-state words, per instance */
    uint32_t state_words;
    uint32_t reserved;
    void* user;
    /* Appended in M3.  Configuration is host-owned, read-only during every
       callback, and survives a simulation RESET.  Older callbacks whose
       compiled struct stops at `user` keep working; newer callbacks must check
       `size` before reading these fields. */
    const uint8_t* config;
    uint32_t config_size;
    uint32_t config_schema;
    /* Appended with the placement cut (2026-09-28): where the instance sits on
       the board and how big it is.  Filled on every lifecycle phase, so a
       callback may always read its own placement; the phases that *change* it are
       TC_LOGIC_MOVE / TC_LOGIC_ROTATE / TC_LOGIC_RESIZE.  `rotation` counts
       quarter turns clockwise (0..3), the same byte the board record carries;
       the footprint is in board cells, half extents like tc.component.geometry.
       A callback whose compiled struct stops before these fields must not read
       them (the host always writes them, the caller's `size` decides what it
       may look at). */
    int32_t cell_x, cell_y;
    uint32_t rotation;
    float half_width, half_height;
} TCLogicIOV2;
typedef void (*TCLogicCallbackV2)(TCLogicIOV2*);

/* ---- V3 IO: three-state pin values ---------------------------------------

   V1 and V2 expose each pin as a uint64_t and therefore cannot distinguish a
   driven zero from a high-impedance bit.  V3 gives every pin a value plane and
   a drive plane:

       drive_mask bit = 1, value bit = 0  -> 0
       drive_mask bit = 1, value bit = 1  -> 1
       drive_mask bit = 0                 -> Z (value is canonicalised to 0)

   On input, the host zeroes bits above the pin's declared width and value bits
   whose drive bit is zero.  Before each callback it zero-initialises every
   output (Z); after the callback it applies the same width/canonicalisation
   rule.  A callback must therefore write both output planes on CYCLE and
   REFRESH.  RESET/state commit rules are otherwise the V2 rules.

   The pinned game represents a word Switch with one enable bit for the whole
   word.  Consequently a registered V3 pin currently arrives either with every
   in-range drive bit set or with none set, and an output must likewise return
   either the full width mask or zero.  A partial output mask is conservatively
   emitted as Z for the whole pin.  Keeping a mask in the ABI preserves the
   natural Verilog representation and leaves room for a future per-bit bridge
   without another callback type.

   There is no X value: conflicting 0/1 drivers remain the game's
   short-circuit/error state, not a fourth logic value delivered to a callback.
   A Verilog front-end may keep X internally for diagnostics, but it must not
   put X in this ABI.

   TCLogicIOV3 deliberately repeats the complete V2 context instead of being a
   tail that a V2 callback may inspect.  The pin pointer element types differ,
   so callers must never cast between TCLogicIOV2 and TCLogicIOV3 even where
   their current field offsets happen to match.

   This header defines the value model first.  A host must not advertise or
   invoke TCLogicCallbackV3 until its generated-code bridge can preserve both
   planes on input and consume both planes on output; treating drive_mask=0 as
   an ordinary zero would violate the contract. */
#define TC_LOGIC_IO_V3_VERSION_1 1u

typedef struct TCLogicValueV3 {
    uint64_t value;
    uint64_t drive_mask;
} TCLogicValueV3;

typedef struct TCLogicIOV3 {
    uint32_t size, version;
    uint32_t phase;                 /* one of TCLogicPhase */
    uint64_t instance_id;
    int64_t cycle;
    uint32_t input_count, output_count;
    const TCLogicValueV3* inputs;   /* borrowed, resolved 0/1/Z values */
    TCLogicValueV3* outputs;        /* callback writes value and drive_mask */
    uint64_t* state;                /* callback-defined storage, as in V2 */
    uint32_t state_words;
    uint32_t reserved;
    void* user;
    const uint8_t* config;
    uint32_t config_size;
    uint32_t config_schema;
    int32_t cell_x, cell_y;
    uint32_t rotation;
    float half_width, half_height;
} TCLogicIOV3;

typedef void (*TCLogicCallbackV3)(TCLogicIOV3*);

typedef struct TCLogicDefinition {
    /* version=2 for authored circuits; version=3 is loader-owned generated
       scaffolding and must be created through register_component; version=4 is
       a V2 definition registered through tc.component.types. */
    uint32_t size, version;
    uint64_t custom_id;
    TCLogicCallback callback;
    void* user;
} TCLogicDefinition;

/* Declarative components: pin array order is callback IO order. Strings and
   arrays are borrowed only for registration; callback/user live with the mod. */
typedef struct TCComponentPin {
    const char* name;
    uint32_t bits;
} TCComponentPin;
typedef struct TCNativeComponentDefinition {
    uint32_t size;
    uint64_t custom_id;
    const char* name;
    const char* description;
    const char* shape_svg; /* NULL uses the game's default shape. */
    uint32_t input_count, output_count;
    const TCComponentPin* inputs;
    const TCComponentPin* outputs;
    uint64_t gate_cost, delay;
    TCLogicCallback callback;
    void* user;
} TCNativeComponentDefinition;
#endif
