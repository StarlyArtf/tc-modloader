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
enum TCLogicPhase { TC_LOGIC_RESET=0, TC_LOGIC_REFRESH=1, TC_LOGIC_CYCLE=2 };
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
typedef struct TCLogicDefinition {
    /* version=2 for authored circuits; version=3 is loader-owned generated
       scaffolding and must be created through register_component. */
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
