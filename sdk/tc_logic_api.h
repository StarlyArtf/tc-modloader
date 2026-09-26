#ifndef TC_LOGIC_API_H
#define TC_LOGIC_API_H
#include <stdint.h>

/* Native logic ABI, version 2. Callbacks run on the game's simulation thread.
   Do not call UI/model APIs, throw, or retain the IO pointer.

   Shape comes from the component definition the plugin imports: up to eight
   input pins and eight output pins per direction, each pin 1..64 bits wide
   (total input width up to 128 bits).  The loader replaces the internal gates
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
    TC_LOGIC_SAVE=8
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
   discarded for REFRESH, exactly like V1).  A pin is 1..64 bits wide and the
   total input width is limited by the loader (128 bits: the generated call can
   carry two 64-bit payload words and no more). */
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
} TCLogicIOV2;
typedef void (*TCLogicCallbackV2)(TCLogicIOV2*);
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
