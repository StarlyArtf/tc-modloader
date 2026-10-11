#pragma once
/* S2's first half (docs/PLAN-sandbox-simulator.md section 4.6): the game's board
   records turned into a simcore netlist.

   The records are the authority, not a guess:

     component  kind +0x00, x +0x02, y +0x04, rotation +0x06, id +0x08,
                custom prototype id +0x188, input pin count +0x30,
                output pin count +0x40, and a pointer at +0x38
                (src/board_objects.hpp plus a real session measured on
                2026-09-25 with tests/sim-state-probe.cpp);
     wire       both endpoints +0x18..+0x1e, width +0x30, and the state byte
                offset the game's own reader uses at +0x38.

   Pin geometry comes from the game's prototype table (the same table
   tools/generate-gate-pins.py dumps).  A cell two component pins share is
   reported, never picked at random, and a kind the device library does not
   implement is reported as unsupported - red line 6. */

#include "devices.hpp"
#include "engine.hpp"
#include "memory.hpp"
#include "netlist.hpp"
#include "screen.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tcsim {

/* One prototype's pins, relative to the component's own cell.  `bits` is the
   pin's word size: 1 for a gate, up to 64 for a bus pin.  Eight pins per side
   because the game's widest built-in is an eight-leaf Splitter/Maker/Output. */
struct KindPins {
    uint8_t kind = 0;
    /* `input_count` is the *total* pin count, which is what the component's own
       record reports - the record counts the storage family's hidden pins, and
       the loader found that out the direct way (a sandbox run refused a board
       with `component 3 (Counter) has 2/1 pins in its record but the prototype
       has 0/1` until the hidden list was added to the count).
       `declared_inputs` is how many of the entries below came from the
       prototype's declared list; the rest are its hidden list. */
    uint8_t input_count = 0;
    uint8_t declared_inputs = 0;
    uint8_t output_count = 0;
    /* Up to 64 pins per side.  The widest *built-in* is an eight-leaf
       Splitter/Maker/Output, but a custom component's interface is whatever its
       author drew: the player's own `Symphony/DEC` has one input and seven
       outputs, and a 5-to-32 decoder has thirty-two.  The table used to stop at
       eight, which made the sandbox refuse those boards outright. */
    static constexpr uint8_t kMaxPins = 64;
    int8_t in_x[kMaxPins] = {};
    int8_t in_y[kMaxPins] = {};
    uint16_t in_bits[kMaxPins] = {};
    int8_t out_x[kMaxPins] = {};
    int8_t out_y[kMaxPins] = {};
    uint16_t out_bits[kMaxPins] = {};
    /* Pins the prototype table keeps out of its declared input list but the
       game still wires: the storage kinds' data/enable/reset pins live in the
       second list at `+0x70`/`+0x78` (tests/kind-list-probe.cpp prints them as
       `hidden0=…`).  They are numbered after the declared ones. */
    uint8_t hidden_count = 0;
    int8_t hidden_x[kMaxPins] = {};
    int8_t hidden_y[kMaxPins] = {};
    uint16_t hidden_bits[kMaxPins] = {};
    const char* name = "";
};

struct BoardComponent {
    uint8_t kind = 0;
    int16_t x = 0;
    int16_t y = 0;
    uint8_t rotation = 0;
    uint64_t id = 0;
    uint64_t custom_id = 0;
    uint64_t setting_value = 0;
    bool has_setting_value = false;
    /* The field width the record keeps at `+0x70`.  It is 0 for every kind
       measured so far except the Static Indexer (0x65), whose configured output
       width sits there (the prototype table's own `raw[0x58..0x80]` block prints
       it as `112:<n>`), and the two counters and delay lines, whose meaning
       there is still unmeasured. */
    uint16_t field_bits = 0;
    uint8_t input_count = 0;
    uint8_t output_count = 0;
    uint64_t private_pointer = 0;
    /* Memory fields measured from the game's 0x238-byte record.  For a RAM,
       `memory_bits + memory_extra_address_bits` is its address width (the
       stock default is 8 and Main Memory adds 16, producing 2^24 bytes).  For
       a Load/Store port, `memory_bits` is the 8/16/32-bit access width. */
    uint16_t memory_bits = 0;
    uint16_t memory_extra_address_bits = 0;
    bool memory_little_endian = false;
};

struct BoardWire {
    int16_t x1 = 0;
    int16_t y1 = 0;
    int16_t x2 = 0;
    int16_t y2 = 0;
    uint16_t width = 0;
    uint64_t slot = 0;
};

/* A wire with no state byte of its own.  Flattening a custom component into the
   board (see the sandbox plugin) synthesizes wires that exist only inside it:
   the game has no byte for those nets, so the builder must not publish into
   whatever offset the definition happened to carry. */
inline constexpr uint64_t kNoStateSlot = ~uint64_t(0);

struct BoardView {
    std::vector<BoardComponent> components;
    std::vector<BoardWire> wires;
};

/* Quarter turns clockwise, the convention the editor's rotation byte uses. */
inline std::pair<int, int> rotateOffset(int x, int y, uint8_t rotation) {
    switch (rotation & 3u) {
        case 1: return {-y, x};
        case 2: return {-x, -y};
        case 3: return {y, -x};
        default: break;
    }
    return {x, y};
}

/* The kinds the sandbox's gate levels are built from.  Everything else is
   reported as unsupported until S3 covers it: a wrong device is worse than a
   missing one. */
inline std::string deviceNameForKind(uint8_t kind) {
    switch (kind) {
        case 0x03: case 0x12: return "not";
        case 0x04: case 0x14: return "and2";
        case 0x05: return "and3";
        case 0x06: case 0x15: return "nand2";
        case 0x07: case 0x13: return "or2";
        case 0x08: return "or3";
        case 0x09: case 0x16: return "nor2";
        case 0x0a: case 0x17: return "xor2";
        case 0x0b: case 0x18: return "xnor2";
        default: break;
    }
    return {};
}

/* The game's word operations, from its own prototype table
   (`build/kinds.txt`, re-dumped 2026-09-29): every one of them is a two-in
   one-out component whose data pins are the same width, so the kind decides the
   operation and the record decides the width.

   `kDataAwait` is the one shape where the game's automatic width cannot be
   recovered from the wires: kind 0x6a is an "Input" whose single output is
   resolved by the game and then stored in a dedicated field of the record
   (the same situation the Word Switch is in).  Until that field is measured the
   builder reports it rather than assuming 1 bit - see
   docs/research/simcore-kind-coverage.md. */
enum class WordKind : uint8_t { kData, kControl, kUnknown };

struct WordKindSpec {
    uint8_t kind;
    const char* device;
    WordKind right; /* what the second pin is: peer data, a control word, or unmeasured */
    bool carry;     /* the Add family's carry in and out */
};

inline const WordKindSpec* wordKindSpec(uint8_t kind) {
    static const WordKindSpec table[] = {
        {0x1A, "equalW", WordKind::kData, false},   /* Equal */
        {0x1B, "lessU", WordKind::kData, false},    /* Low < (unsigned) */
        {0x1C, "lessS", WordKind::kData, false},    /* Less < (signed) */
        {0x1E, "addW", WordKind::kData, true},      /* Add, with carry in/out */
        {0x1F, "mulW", WordKind::kData, false},     /* Multiply */
        {0x20, "divW", WordKind::kData, false},     /* Divide */
        {0x21, "shlW", WordKind::kControl, false},  /* Shift Left */
        {0x22, "shrW", WordKind::kControl, false},  /* Shift Right */
        {0x23, "rolW", WordKind::kControl, false},  /* Rotate Left */
        {0x24, "rorW", WordKind::kControl, false},  /* Rotate Right */
        {0x25, "asrW", WordKind::kControl, false},  /* Arithmetic Shift Right */
        {0x6C, "modW", WordKind::kData, false},     /* Modulo */
        {0x14, "andW", WordKind::kData, false},     /* AND (word) */
        {0x13, "orW", WordKind::kData, false},      /* OR (word) */
        {0x17, "xorW", WordKind::kData, false},     /* XOR (word) */
        {0x15, "nandW", WordKind::kData, false},    /* NAND (word) */
        {0x16, "norW", WordKind::kData, false},     /* NOR (word) */
        {0x18, "xnorW", WordKind::kData, false},    /* XNOR (word) */
    };
    for (const WordKindSpec& spec : table) {
        if (spec.kind == kind) return &spec;
    }
    return nullptr;
}

/* The single-operand word operations. */
struct WordUnarySpec {
    uint8_t kind;
    const char* device;
};

inline const WordUnarySpec* wordUnarySpec(uint8_t kind) {
    static const WordUnarySpec table[] = {
        {0x12, "notW"},   /* NOT (word) */
        {0x1D, "negateW"},/* Negate */
        {0x31, "clzW"},   /* Count leading zero */
        {0x39, "ctzW"},   /* Count trailing zero */
        {0x68, "incW"},   /* Increment */
    };
    for (const WordUnarySpec& spec : table) {
        if (spec.kind == kind) return &spec;
    }
    return nullptr;
}

/* The 2-in Concatenator (0x72): two vectors joined, the first pin the high
   half.  It is the one converter whose bit order the prototype table itself
   fixes (`in0=(-1,-1,w2) in1=(-1,0,w2) out0=(1,0,w4)`), so it can be built
   without a measurement.  The wider Maker/Concatenator/Splitter shapes wait for
   theirs - see docs/research/simcore-kind-coverage.md. */
inline bool isConcatKind(uint8_t kind) { return kind == 0x72; }

/* The two families whose data path the prototype table writes as a sentinel and
   which the board - not the table - sizes.  Both were measured on the player's
   own board (tools/kind-wire-widths.py, 2026-09-30):

     * a Splitter over words splits into equal leaves, so its input is
       `leaves * n` and each output carries n: `0x2F … in0 64 -> out0 32, out1
       32`, with the record's `+0xe0` saying 32 - the leaf width;
     * a Word Maker concatenates its inputs, so its output is their sum:
       `0x30 … in0 8, in1 8 -> out0 16`, with `+0xe0` saying 16 - the output.

   The table's sentinel used to make the builder refuse both ("has no width its
   prototype pins down"), which is why a player's board with a Word Maker on it
   carried a note nobody could clear. */
inline uint8_t wordSplitterLeaves(uint8_t kind) {
    switch (kind) {
        case 0x2f: return 2; /* "Splits a (2n)-bit value into two n-bit values" */
        case 0x63: return 4; /* "Splits a (4n)-bit value into four n-bit values" */
        case 0x64: return 8; /* "Splits an (8n)-bit value into eight n-bit values" */
        default: return 0;
    }
}

/* The Word Makers (2/4/8 inputs) and the wide Concatenators (4/8), whose inputs
   and output are all `wauto` in the table. */
inline bool kindIsWordMaker(uint8_t kind) {
    switch (kind) {
        case 0x30: /* Word Maker, 2 in */
        case 0x61: /* Word Maker, 4 in */
        case 0x62: /* Word Maker, 8 in */
        case 0x73: /* Concatenator, 4 in */
        case 0x74: /* Concatenator, 8 in */
            return true;
        default:
            return false;
    }
}

/* The record's own width field (`+0xe0`) means one number per data pin for the
   word operations and the storage family, but not for these two: a Splitter
   stores its *leaf* width there and a Word Maker its *output* width.  Seeding
   the per-pin table with those numbers would hide the wires' own widths - which
   are the measurement - so they are excluded. */
inline bool kindRecordWidthIsPerPin(uint8_t kind) {
    return wordSplitterLeaves(kind) == 0 && !kindIsWordMaker(kind);
}

/* The Static Indexer (0x65): field extraction whose configuration is the
   component's own record (signed index + field width), so the board builder
   needs no width resolution for it. */
inline bool isIndexerKind(uint8_t kind) { return kind == 0x65; }

/* The Decoder family: every width is declared by the prototype table itself
   (`0x2b` one address bit, `0x2c` two, `0x2d` three plus an enable), so the
   builder takes the address width from the pin rather than from any wire. */
struct DecoderKindSpec {
    uint8_t kind;
    uint8_t address_bits;
    bool has_enable;
};

inline const DecoderKindSpec* decoderKindSpec(uint8_t kind) {
    static const DecoderKindSpec table[] = {
        {0x2b, 1, false}, /* 1 Bit Decoder: 1 in (w1) / 2 out */
        {0x2c, 2, false}, /* 2 Bit Decoder: 1 in (w2) / 4 out */
        {0x2d, 3, true},  /* 3 Bit Decoder: in0 (w3), in1 (w1) / 8 out */
    };
    for (const DecoderKindSpec& spec : table) {
        if (spec.kind == kind) return &spec;
    }
    return nullptr;
}

/* The two custom-component interface pins, 0x4f (Input Pin) and 0x51 (Output
   Pin).  Both pass one signal through; the board builder only has to give them
   the width the rest of the netlist uses, which is why they join the
   automatic-width family below.  See `devicePin`. */
inline bool isPinKind(uint8_t kind) { return kind == 0x4f || kind == 0x51; }

/* The bit <-> vector converters, from the game's own prototype table.  Every
   one of them has a declared width: either the data pin carries the width (a
   Maker has `w1` inputs and a `w<n>` output) or nothing does.  `0x2f` is the
   second case - its input pin is one of the two Splitter sentinels whose
   meaning is unmeasured - so it is recognised here and then refused by
   `leafWidth`, which reports "not guessed" instead of picking a width.  The
   same goes for any row whose numbers do not agree. */
struct LeafKindSpec {
    uint8_t kind;
    WordOp op;
    const char* name;
};

inline const LeafKindSpec* leafKindSpec(uint8_t kind) {
    static const LeafKindSpec table[] = {
        {0x11, WordOp::kSplit, "splitter8"},  /* Splitter, 1 in (w8) / 8 out */
        {0x6e, WordOp::kSplit, "splitter4"},  /* Splitter, w4 / 4 out */
        {0x6d, WordOp::kSplit, "splitter2"},  /* Splitter, w2 / 2 out */
        {0x10, WordOp::kMake, "maker8"},      /* Maker, 8 x w1 / w8 */
        {0x70, WordOp::kMake, "maker4"},      /* Maker, 4 x w1 / w4 */
        {0x6f, WordOp::kMake, "maker2"},      /* Maker, 2 x w1 / w2 */
    };
    for (const LeafKindSpec& spec : table) {
        if (spec.kind == kind) return &spec;
    }
    return nullptr;
}

/* The width a leaf device can actually be built at, or 0 when the prototype's
   own numbers do not pin one down.  All of a leaf's declared widths have to be
   one concrete number that also matches the pin count: the Splitter's single
   input spans every leaf, the Maker's leaves are one bit each. */
inline uint16_t leafWidth(const LeafKindSpec& spec, const KindPins& pins) {
    const auto concrete = [](uint16_t bits) { return bits != 0 && bits < 0x7fffu; };
    if (spec.op == WordOp::kSplit) {
        if (pins.input_count != 1 || pins.output_count == 0) return 0;
        if (!concrete(pins.in_bits[0])) return 0;
        const uint16_t width = pins.in_bits[0];
        if (width != pins.output_count) return 0;
        for (uint8_t pin = 0; pin < pins.output_count; ++pin) {
            if (!concrete(pins.out_bits[pin]) || pins.out_bits[pin] != 1) return 0;
        }
        return width;
    }
    if (spec.op != WordOp::kMake) return 0;
    /* A Maker: every input leaf is one bit and the result is as wide as the
       number of leaves (the declared output width is `wauto` on the Word Maker
       and Concatenator shapes, and has to agree where it is concrete). */
    if (pins.input_count == 0 || pins.input_count > 8 || pins.output_count != 1) return 0;
    for (uint8_t pin = 0; pin < pins.input_count; ++pin) {
        if (pins.in_bits[pin] != 1) return 0;
    }
    const uint16_t declared = pins.out_bits[0];
    if (concrete(declared) && declared != pins.input_count) return 0;
    return pins.input_count;
}

/* Kinds that feed a net from outside the board: the level's inputs, the player's
   switches, and the two host peripherals whose values only the game can know -
   the Time component (nanoseconds since 1970, per its manual page) and the
   Keyboard (the key events it has recorded, per `Circuits/The keyboard`).  Their
   value lives in the game's state array, so the netlist only marks the net as
   externally driven; simcore stays a shadow and never invents a time or a key.
   The Keyboard also has an input pin; a source only drives its outputs, so that
   pin stays somebody else's business. */
inline bool kindIsSource(uint8_t kind) {
    switch (kind) {
        case 0x34: /* Push Button */
        case 0x3c: case 0x3d: case 0x3f: case 0x40: case 0x41: /* level inputs */
        case 0x6a: /* Input: "An input that will be converted to custom input, if
                      the solution is saved to the foundry" - the word-sized
                      level input.  The game emits no code for it; the value is
                      the level's, so the sandbox reads it from the same state
                      array as the bit inputs. */
        case 0x3e: /* Switched Input: "Loads the input from levels" */
        case 0x5f: /* Verilog Input */
        case 0x5c: /* Time */
        case 0x5d: /* Keyboard */
            return true;
        default: break;
    }
    return false;
}

/* Kinds that only observe: an Output, a probe. */
inline bool kindIsSink(uint8_t kind) {
    switch (kind) {
        case 0x28: case 0x3a: case 0x44: case 0x45: case 0x49: case 0x4a: case 0x4b: /* Outputs */
        case 0x52: case 0x54: case 0x55: /* probes */
        case 0x53: /* Memory Probe: "Displays a value on your custom component or
                      in the program editor" - it observes, it drives nothing. */
        case 0x33: /* Disassembling probe: emits no code at all. */
        case 0x59: /* 7 Segment Display: "Good for displaying numbers" */
        case 0x57: /* Halt: the game emits `if (in) { set_error(""); halt() }`.
                      The sandbox has no simulation to stop - the board's values
                      simply stop changing - so it is an observer here, and that
                      difference is written down rather than hidden. */
        case 0x4d: /* Counter (three 1-bit inputs, no board output): the level's
                      own output counter, "Three-bit output". */
        case 0x75: /* Auto Static Indexer: "Advanced version of static indexer.
                      Shift and output size is inferred from the size of wires
                      connected to input pins" - a configuration block.  The
                      game emits no code for it at all (com_static_indexer_config
                      with an empty body), so it observes its inputs and drives
                      nothing. */
        case 0x60: /* Verilog Output */
        case 0x46: /* Switched Output: value + switch, no outputs */
            return true;
        default: break;
    }
    return false;
}

/* ---- the storage family and the level-map parts ---------------------------

   `kRegister`, `kDelayLine` and `kCounter` are the game's own stateful parts.
   Their contract, pins and clocking are documented on `deviceRegister` and
   friends in devices.hpp; the prototype table's *hidden* pin list above is
   where their data/control pins come from.

   `kLevelGate` is the Level Component / Level And Delay pair, which the game
   emits as `value := <level value> & <pin>`; a sandbox board has no level value
   behind it (the sandbox level's test is empty), so its outputs are a constant
   0 and its input is an observer.  `kLevelMap`'s second output is the level's
   "best delay", which is authoring information rather than board state - the
   sandbox drives 0 there too. */
enum class StorageRole : uint8_t { kRegister, kDelayLine, kCounter, kConstant, kLevelGate };

struct StorageKindSpec {
    uint8_t kind;
    StorageRole role;
    const char* name;
};

inline const StorageKindSpec* storageKindSpec(uint8_t kind) {
    static const StorageKindSpec table[] = {
        {0x27, StorageRole::kRegister, "Register"},          /* "Small memory component" */
        {0x0e, StorageRole::kRegister, "Register (bit)"},
        {0x26, StorageRole::kCounter, "Counter"},            /* "Increments each cycle, unless overwritten" */
        {0x37, StorageRole::kDelayLine, "Delay Line"},       /* "Stores the input for 1 cycle" */
        {0x0d, StorageRole::kDelayLine, "Delay Line (bit)"},
        {0x77, StorageRole::kDelayLine, "Auto Delay Line"},
        {0x32, StorageRole::kConstant, "Auto Register"},     /* the auto-size config word */
        {0x5a, StorageRole::kConstant, "Static Value"},      /* "sets values or width" */
        {0x5e, StorageRole::kConstant, "Static Evaluator"},  /* "becomes a constant during simulation" */
        {0x3b, StorageRole::kLevelGate, "Level Component"},
        {0x29, StorageRole::kLevelGate, "Level And Delay Component"},
    };
    for (const StorageKindSpec& spec : table) {
        if (spec.kind == kind) return &spec;
    }
    return nullptr;
}

/* The width the Auto/copy family's configuration word is published at (the game
   emitted `U2 0x0` for a placed Auto Register). */
inline constexpr uint16_t kAutoConfigBits = 2;

inline bool kindIsConstant(uint8_t kind) { return kind == 0x01 || kind == 0x02 || kind == 0x2e; }

inline bool kindIsTriStateSwitch(uint8_t kind) {
    return kind == 0x0c || kind == 0x19; /* Bit Switch / Word Switch */
}

/* Kinds whose data pins the game sizes automatically and whose resolved width
   it then writes into the record (`+0xe0`, the same field a RAM or a Load port
   keeps its width in).  The word operations take their width from the pins the
   same way (`build/kinds.txt`: `0x1e name="Add" in0=(-1,-1,wauto) in1=(-1,1,
   wauto)`, the same for the comparisons, the shifts, the word gates and the
   width conversions), so they belong in this family too.  The Bit Switch (0x0c)
   is the one-bit member and needs no resolution. */
inline bool kindHasAutoDataWidth(uint8_t kind) {
    if (kind == 0x19 || /* Word Switch */
        kind == 0x2a || /* Mux */
        kind == 0x2f || /* Splitter (2n -> two n-bit halves) */
        kind == 0x63 || /* Splitter (4n -> four n-bit leaves) */
        kind == 0x64 || /* Splitter (8n -> eight n-bit leaves) */
        kind == 0x30 || /* Word Maker, 2 in */
        kind == 0x61 || /* Word Maker, 4 in */
        kind == 0x62 || /* Word Maker, 8 in */
        kind == 0x73 || /* Concatenator, 4 in */
        kind == 0x74 || /* Concatenator, 8 in */
        kind == 0x12 || /* NOT (word) */
        kind == 0x1d || /* Negate */
        kind == 0x31 || /* Count leading zero */
        kind == 0x39 || /* Count trailing zero */
        kind == 0x68 || /* Increment */
        kind == 0x72 || /* the 2-in Concatenator, whose widths the table declares */
        kind == 0x4f || /* Input Pin: one signal in, the same one out */
        kind == 0x51)   /* Output Pin */
        return true;
    /* The storage family's data pin carries the width the game resolved into the
       record (`+0xe0`, the same field a RAM uses); the Auto Register / Auto
       Delay Line pair has an extra size pin in front of its hidden ones. */
    if (storageKindSpec(kind) != nullptr) return true;
    /* Every other word operation too: they all declare `wauto` on their data
       pins and keep the width the game resolved in `memory_bits`. */
    return wordKindSpec(kind) != nullptr || wordUnarySpec(kind) != nullptr;
}

/* Handles with an automatic data width *and* a second pin that is a parameter
   rather than a peer: the shift/rotate amount, the word gates' second operand.
   Both pins go into the width resolution, and the record's own field still wins
   when the game filled it in. */
inline bool kindHasAutoRightWidth(uint8_t kind) {
    const WordKindSpec* spec = wordKindSpec(kind);
    return spec != nullptr && spec->right != WordKind::kUnknown;
}

/* The pins both families keep narrow whatever the data width is: the switch's
   enable and the mux's select are `w1` in the game's own prototype table, and
   the shift/rotate amount is a parameter whose width is whatever the wire
   carrying it happens to be.  A data width must never be written into these. */
inline bool isAutoWidthControlPin(uint8_t kind, uint8_t pin) {
    if (kind == 0x2a) return pin == 0; /* Mux: in0 is the select */
    if (kind == 0x19) return pin == 0; /* Switch: in0 is the enable */
    /* The storage family's narrow controls.  A Register's enable and a
       Counter's reset are one bit whatever the data width is, and the Auto
       variants' leading pin is the blue size pin - none of them may be widened
       by the wire that reaches them. */
    if (kind == 0x27 || kind == 0x0e) return pin == 0;  /* Register: EN, then D */
    if (kind == 0x26) return pin == 0;                  /* Counter: RST, then D */
    if (kind == 0x32) return pin != 2;                  /* Auto Register: blue size pin, EN, then D */
    if (kind == 0x37 || kind == 0x0d) return false;     /* Delay Line: D only */
    if (kind == 0x77) return pin == 0;                  /* Auto Delay Line: blue size pin, then D */
    if (storageKindSpec(kind) != nullptr) return false; /* the constant family has no data pin */
    return kindHasAutoRightWidth(kind) && pin == 1; /* the shift/rotate amount */
}

/* Host components registered by examples/clock.  The clock is simulator-owned;
   the switch and button are external sources whose live value is published by
   their owning Mod into the same wire slots that ordinary inputs use. */
inline bool customIsClock(uint64_t custom_id) {
    return custom_id == 0x434C4F4B5F303031ULL; /* CLOK_001 */
}
inline bool customIsInteractiveSource(uint64_t custom_id) {
    return custom_id == 0x535749545F303031ULL || /* SWIT_001 */
           custom_id == 0x4255544E5F303031ULL;   /* BUTN_001 */
}

inline Logic constantValue(const BoardComponent& component) {
    if (component.kind == 0x01) return Logic::kZero; /* OFF */
    if (component.kind == 0x02) return Logic::kOne;  /* ON */
    return component.has_setting_value && (component.setting_value & 1u) ? Logic::kOne : Logic::kZero;
}

struct BuildNote {
    std::string text;
    bool fatal = false; /* a fatal note means the netlist must not be run */
};

struct BuildResult {
    /* Where a source net's value comes from.  A host component (the interactive
       switch/button) keeps its value in its own instance configuration, not in a
       wire slot: the game's generated program is what copies it into the state
       array, and the sandbox suppresses that program.  `custom_id == 0` means
       "read the state array" (level inputs and the builtin sources). */
    struct SourceOrigin {
        uint64_t custom_id = 0;
        uint64_t instance_id = 0;
    };
    struct MemoryBinding {
        size_t component = 0;
        std::shared_ptr<MemoryImage> image;
        std::vector<size_t> ports; /* board component indices, execution order */
        /* Driven once per game cycle by SandboxRuntime.  A changing private
           token makes every port execute even when its visible pins are held. */
        NetId order_root = kNoNet;
    };
    /* A screen keeps a settings table and displays the RAM it is linked to, so
       the binding carries both the table (which the three pins write) and the
       nets those pins came from - plenty for a report or a viewer to say what
       the screen would be showing (screen.hpp renders it from a RAM image). */
    struct ScreenBinding {
        size_t component = 0;
        std::shared_ptr<ScreenSettings> settings;
        NetId enable = kNoNet;
        NetId setting_index = kNoNet;
        NetId value = kNoNet;
    };
    NetList netlist;
    std::vector<BuildNote> notes;
    std::vector<NetId> net_of_wire;
    std::vector<std::vector<uint64_t>> slots_of_net; /* state bytes to write per net */
    std::vector<NetId> source_nets;                  /* driven by the game's array */
    std::vector<SourceOrigin> source_origins;        /* parallel to source_nets */
    std::vector<NetId> observed_nets;                /* read back by the game */
    std::vector<NetId> clock_nets;                   /* host clock components */
    std::vector<DeviceId> devices_by_component;
    /* A secondary device for a component whose outputs do not all come out of
       one spec - the Level And Delay's 16-bit best-delay pin.  Keyed by board
       component index, connected to the same pin numbers as the primary. */
    std::map<size_t, DeviceId> extra_device_of_component;
    /* The storage family latches once per game cycle, and the engine's clocks
       are edges.  This net is toggled by SandboxRuntime at every cycle boundary;
       every Register / Counter / Delay Line takes its clock from it, the same
       trick the RAM's port chain already uses. */
    NetId cycle_token = kNoNet;
    std::vector<MemoryBinding> memories;
    std::vector<ScreenBinding> screens;
    /* Port specs capture their RAM image and therefore belong to this build,
       rather than to the process-wide stateless device catalogue. */
    std::vector<std::unique_ptr<DeviceSpec>> owned_specs;
    size_t unconnected_pins = 0;
    bool fatal() const {
        for (const BuildNote& note : notes) {
            if (note.fatal) return true;
        }
        return false;
    }
};

struct BuildOptions {
    /* Include the wire's neighbouring byte in the write-back set: a real board
       writes the same value into the wire's slot and into the byte after it
       (measured 2026-09-25: slots 256/257, 258/259, 260/261 move together). */
    bool include_neighbour_slot = true;
    bool rotate_pins = true;
    /* Delay a gate gets when no timing library covers it ("每器件一个常数" in
       the plan's section 4.4).  Zero means "settle through delta rounds". */
    ArcDelay gate_delay{};
    /* The bridge may seed the image from the game's reset-data manager.  The
       standalone simulator leaves it empty, which is the game's zero-filled
       RAM behavior when no initial data is configured. */
    std::function<void(const BoardComponent&, MemoryImage&)> initialize_memory;
};

namespace board_detail {

/* A kind is a byte, and every message about one writes it in hex behind a `0x`
   prefix.  `std::to_string` promotes the byte to int and prints decimal, so
   the prefix and the digits used to disagree - "kind 0x153" for kind 0x99, in
   the very message red line 6 depends on. */
inline std::string kindText(uint8_t kind) {
    char text[8] = {};
    std::snprintf(text, sizeof(text), "0x%02X", static_cast<unsigned>(kind));
    return text;
}

inline uint64_t readU64(const unsigned char* bytes, size_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

using Coordinate = std::pair<int, int>;

struct PinSite {
    size_t component = 0;
    bool input = false;
    uint8_t pin = 0;
};

inline bool isMemoryPort(uint8_t kind) { return kind == 0x36 || kind == 0x38; }

/* Coordinates in a RAM's unrotated local space. */
inline Coordinate memoryLocalOffset(const BoardComponent& ram, const BoardComponent& port) {
    return rotateOffset(port.x - ram.x, port.y - ram.y, static_cast<uint8_t>((4u - ram.rotation) & 3u));
}

}  // namespace board_detail

/* Builds the netlist.  `geometry` answers with a prototype's pins for a kind;
   the plugin wires it to the game's own table and the offline test wires it to
   the generated table, so both run the same builder. */
inline BuildResult buildNetList(const BoardView& board,
                                const std::function<bool(const BoardComponent&, KindPins&)>& geometry,
                                const BuildOptions& options = BuildOptions()) {
    BuildResult result;
    result.net_of_wire.assign(board.wires.size(), kNoNet);
    result.devices_by_component.assign(board.components.size(), kNoDevice);

    /* Every pin the board has, placed where the wires are drawn. */
    std::map<board_detail::Coordinate, std::vector<board_detail::PinSite>> sites;
    std::vector<KindPins> pins_of(board.components.size());
    for (size_t index = 0; index < board.components.size(); ++index) {
        const BoardComponent& component = board.components[index];
        if (component.kind == 0x00) continue; /* the board's own record */
        KindPins pins;
        if (!geometry(component, pins)) {
            result.notes.push_back(BuildNote{"component " + std::to_string(index) + " has kind " +
                                                 board_detail::kindText(component.kind) + " with no pin geometry",
                                             true});
            continue;
        }
        pins_of[index] = pins;
        if (pins.input_count != component.input_count || pins.output_count != component.output_count) {
            result.notes.push_back(BuildNote{"component " + std::to_string(index) + " (" + pins.name +
                                                 ") has " + std::to_string(component.input_count) + "/" +
                                                 std::to_string(component.output_count) +
                                                 " pins in its record but the prototype has " +
                                                 std::to_string(pins.input_count) + "/" +
                                                 std::to_string(pins.output_count),
                                             true});
            continue;
        }
        /* The geometry table is bounded (see KindPins::kMaxPins); a kind that
           claims more pins than that is reported by the caller's geometry
           callback, which is where "the prototype has more pins than this
           builder reads" belongs. */
        if (pins.input_count > KindPins::kMaxPins || pins.output_count > KindPins::kMaxPins) continue;
        const uint8_t rotation = options.rotate_pins ? component.rotation : 0;
        for (uint8_t pin = 0; pin < pins.input_count; ++pin) {
            /* The remaining memory-port pins are the game's private connection
               into the RAM/socket dependency chain, not board wires. */
            if ((component.kind == 0x36 && pin >= 2) || (component.kind == 0x38 && pin >= 3)) continue;
            const auto offset = rotateOffset(pins.in_x[pin], pins.in_y[pin], rotation);
            sites[{component.x + offset.first, component.y + offset.second}].push_back(
                board_detail::PinSite{index, true, pin});
        }
        for (uint8_t pin = 0; pin < pins.output_count; ++pin) {
            if ((component.kind == 0x36 && pin >= 1) || component.kind == 0x38) continue;
            const auto offset = rotateOffset(pins.out_x[pin], pins.out_y[pin], rotation);
            sites[{component.x + offset.first, component.y + offset.second}].push_back(
                board_detail::PinSite{index, false, pin});
        }
        /* The prototype's hidden pins are wired by the game just like declared
           ones; they are numbered after the declared inputs so a device's pin
           order is "declared pins, then hidden pins". */
        for (uint8_t pin = 0; pin < pins.hidden_count; ++pin) {
            const auto offset = rotateOffset(pins.hidden_x[pin], pins.hidden_y[pin], rotation);
            sites[{component.x + offset.first, component.y + offset.second}].push_back(
                board_detail::PinSite{index, true,
                                      static_cast<uint8_t>(pins.declared_inputs + pin)});
        }
    }

    /* Wires join the cells they are drawn between; a cell lands on exactly one
       net, so a net is a connected group of wires. */
    std::vector<int> parent;
    std::map<board_detail::Coordinate, int> coordinate_id;
    const auto find = [&parent](int value) {
        while (parent[static_cast<size_t>(value)] != value) {
            parent[static_cast<size_t>(value)] = parent[static_cast<size_t>(parent[static_cast<size_t>(value)])];
            value = parent[static_cast<size_t>(value)];
        }
        return value;
    };
    const auto id_of = [&coordinate_id, &parent](const board_detail::Coordinate& coordinate) {
        const auto found = coordinate_id.find(coordinate);
        if (found != coordinate_id.end()) return found->second;
        const int id = static_cast<int>(parent.size());
        parent.push_back(id);
        coordinate_id[coordinate] = id;
        return id;
    };
    for (const BoardWire& wire : board.wires) {
        const int first = id_of({wire.x1, wire.y1});
        const int second = id_of({wire.x2, wire.y2});
        const int a = find(first);
        const int b = find(second);
        if (a != b) parent[static_cast<size_t>(a)] = b;
    }

    std::map<int, NetId> net_of_group;
    /* What width the pins at a net's cells ask for.  A board's own wires carry
       their width, but a *definition's* wires do not - measured on the player's
       foundry definitions: every internal wire of every one of them reads
       `width` 0.  Without this a 32-bit path inside a custom component would be
       built as one-bit nets and every value inside it would be truncated.  A
       pin whose own width is one of the "auto" sentinels takes the width the
       game resolved for its component (`+0xe0`). */
    /* A board's own wires state their width, and that stays the authority.  A
       *bridge* or a definition-internal wire carries `kNoStateSlot` - it is not
       one of the game's wires - and its width comes from the pins it was built
       from, which is exactly the number that must *not* widen a net the game
       sized itself: publishing a 32-bit word into a wire the game allocated two
       bytes for would run over its neighbours.  So: the game's own wires decide
       the width when they have one, and the pins only fill in when they do
       not. */
    std::map<int, uint16_t> wire_width_of_group;
    for (const BoardWire& wire : board.wires) {
        if (wire.slot == kNoStateSlot || !wire.width) continue;
        const int group = find(id_of({wire.x1, wire.y1}));
        const auto found = wire_width_of_group.find(group);
        if (found == wire_width_of_group.end() || wire.width > found->second) {
            wire_width_of_group[group] = wire.width;
        }
    }
    std::map<int, uint16_t> pin_width_of_group;
    for (const auto& entry : sites) {
        const auto cell = coordinate_id.find(entry.first);
        if (cell == coordinate_id.end()) continue; /* a pin no wire reaches */
        uint16_t want = 0;
        for (const board_detail::PinSite& site : entry.second) {
            const BoardComponent& component = board.components[site.component];
            if (site.component >= pins_of.size()) continue;
            const KindPins& pins = pins_of[site.component];
            const uint8_t pin = site.pin;
            uint16_t bits = 0;
            if (site.input && pin < pins.input_count) bits = pins.in_bits[pin];
            else if (!site.input && pin < pins.output_count) bits = pins.out_bits[pin];
            if (!bits || bits >= 0x7ff0u) bits = component.memory_bits;
            if (bits && bits < 0x7ff0u && bits > want) want = bits;
        }
        if (!want) continue;
        const int group = find(cell->second);
        const auto found = pin_width_of_group.find(group);
        if (found == pin_width_of_group.end() || want > found->second) pin_width_of_group[group] = want;
    }
    for (size_t index = 0; index < board.wires.size(); ++index) {
        const BoardWire& wire = board.wires[index];
        const int group = find(id_of({wire.x1, wire.y1}));
        auto found = net_of_group.find(group);
        if (found == net_of_group.end()) {
            uint16_t width = wire.width;
            const auto group_wire = wire_width_of_group.find(group);
            if (group_wire != wire_width_of_group.end() && group_wire->second > width) {
                width = group_wire->second;
            }
            if (!width) {
                const auto pins_at = pin_width_of_group.find(group);
                if (pins_at != pin_width_of_group.end()) width = pins_at->second;
            }
            const NetId net = result.netlist.addNet("net" + std::to_string(group),
                                                    static_cast<uint16_t>(width ? width : 1));
            result.slots_of_net.resize(result.netlist.netCount());
            found = net_of_group.emplace(group, net).first;
        }
        result.net_of_wire[index] = found->second;
        /* The wire's own state byte is where the game reads this net's value. */
        if (wire.slot != kNoStateSlot) {
            result.slots_of_net[found->second].push_back(wire.slot);
            if (options.include_neighbour_slot && wire.width <= 1)
                result.slots_of_net[found->second].push_back(wire.slot + 1);
        }
    }

    /* RAMs have no visible pins.  Ports placed in their socket form an
       implicit, ordered chain: the farthest port runs first and the closest
       runs last.  That is the game's documented/compiled "top to bottom"
       rule, including read-before-write versus write-before-read at one
       address. */
    for (size_t component_index = 0; component_index < board.components.size(); ++component_index) {
        const BoardComponent& component = board.components[component_index];
        if (component.kind != 0x76) continue;
        BuildResult::MemoryBinding binding;
        binding.component = component_index;
        binding.image = std::make_shared<MemoryImage>();
        binding.image->component_id = component.id;
        const uint32_t address_bits = static_cast<uint32_t>(component.memory_bits ? component.memory_bits : 8) +
                                      component.memory_extra_address_bits;
        binding.image->address_bits = static_cast<uint8_t>(address_bits > 63 ? 63 : address_bits);
        binding.image->little_endian = component.memory_little_endian;
        if (options.initialize_memory) options.initialize_memory(component, *binding.image);
        result.memories.push_back(std::move(binding));
    }

    for (size_t port_index = 0; port_index < board.components.size(); ++port_index) {
        const BoardComponent& port = board.components[port_index];
        if (!board_detail::isMemoryPort(port.kind)) continue;
        size_t best = result.memories.size();
        int best_distance = 0x7fffffff;
        for (size_t memory_index = 0; memory_index < result.memories.size(); ++memory_index) {
            const BoardComponent& ram = board.components[result.memories[memory_index].component];
            if ((ram.rotation & 3u) != (port.rotation & 3u)) continue;
            const board_detail::Coordinate local = board_detail::memoryLocalOffset(ram, port);
            /* The measured socket starts ten cells above the RAM anchor.  A
               tolerance of two keeps the association stable across the two
               recorded display/model anchors without accepting a loose port. */
            if (local.first != 0 || local.second > -8) continue;
            const int distance = -local.second;
            if (distance < best_distance) {
                best = memory_index;
                best_distance = distance;
            }
        }
        if (best == result.memories.size()) {
            result.notes.push_back(BuildNote{"memory port component " + std::to_string(port_index) +
                                                 " is not attached to a RAM socket",
                                             true});
            continue;
        }
        result.memories[best].ports.push_back(port_index);
    }

    for (size_t memory_index = 0; memory_index < result.memories.size(); ++memory_index) {
        BuildResult::MemoryBinding& binding = result.memories[memory_index];
        const BoardComponent& ram = board.components[binding.component];
        std::sort(binding.ports.begin(), binding.ports.end(), [&](size_t left, size_t right) {
            const auto a = board_detail::memoryLocalOffset(ram, board.components[left]);
            const auto b = board_detail::memoryLocalOffset(ram, board.components[right]);
            if (a.second != b.second) return a.second < b.second;
            return left < right;
        });

        if (binding.ports.empty()) continue;
        NetId order = result.netlist.addNet("ram" + std::to_string(memory_index) + "_order0", 8);
        binding.order_root = order;
        result.slots_of_net.resize(result.netlist.netCount());

        for (size_t sequence = 0; sequence < binding.ports.size(); ++sequence) {
            const size_t port_index = binding.ports[sequence];
            const BoardComponent& port = board.components[port_index];
            const uint16_t access_bits = port.memory_bits ? port.memory_bits : 8;
            std::unique_ptr<DeviceSpec> spec = port.kind == 0x36
                                                   ? memoryLoadPortSpec(binding.image, access_bits, options.gate_delay)
                                                   : memoryStorePortSpec(binding.image, access_bits, options.gate_delay);
            DeviceSpec* owned = spec.get();
            result.owned_specs.push_back(std::move(spec));
            const std::string role = port.kind == 0x36 ? "load" : "store";
            const DeviceId device = result.netlist.addDevice(
                owned, "u" + std::to_string(port_index) + "_" + role + std::to_string(access_bits));
            result.devices_by_component[port_index] = device;
            result.netlist.connectInput(device, static_cast<uint16_t>(owned->inputs.size() - 1), order);
            const NetId next = result.netlist.addNet("ram" + std::to_string(memory_index) + "_order" +
                                                         std::to_string(sequence + 1),
                                                     8);
            result.slots_of_net.resize(result.netlist.netCount());
            result.netlist.connectOutput(device, static_cast<uint16_t>(owned->outputs.size() - 1), next);
            order = next;
        }
    }

    /* A Word Switch's and a word Mux's prototype declare an automatic data
       width, and so does every word operation.  Three authorities, in this
       order: the width the game itself resolved into the record
       (`memory_bits`, `+0xe0`), then the width of the wire that reaches the
       data pin, then - only for a kind whose prototype declares a real width -
       that declared width.  A kind that declares `wauto` and has neither is
       genuinely unknown, and the builder says so instead of picking 1 bit.

       The width is kept per pin, not per component: the shift and rotate
       amount is a parameter pin whose width is whatever the wire carrying it
       happens to be, while the data pin next to it takes the full width. */
    std::vector<std::array<uint16_t, 8>> auto_width(board.components.size());
    for (std::array<uint16_t, 8>& pins : auto_width) pins.fill(0);
    /* The widths arriving at *output* pins, kept apart for the two families
       whose record widths are not per pin (`kindRecordWidthIsPerPin`): a
       Splitter's outputs are the only place its leaf width shows when the input
       is unwired, while a Word Maker's output is its input *sum* and must never
       be read back as an input width. */
    std::vector<std::array<uint16_t, 8>> auto_out_width(board.components.size());
    for (std::array<uint16_t, 8>& pins : auto_out_width) pins.fill(0);
    for (size_t index = 0; index < board.components.size(); ++index) {
        const BoardComponent& component = board.components[index];
        if (component.kind == 0x0c) {
            auto_width[index][0] = 1;
        } else if (kindHasAutoDataWidth(component.kind)) {
            for (size_t pin = 0; pin < auto_width[index].size(); ++pin) {
                /* The record's number describes the data path; a control or
                   parameter pin keeps its own width (the game's prototype says
                   `w1` for the Mux's select, and the shift amount is whatever
                   wire reaches it). */
                if (isAutoWidthControlPin(component.kind, static_cast<uint8_t>(pin))) continue;
                /* A Splitter's `+0xe0` is its leaf width and a Word Maker's is
                   its output width: neither is a per-pin number. */
                if (!kindRecordWidthIsPerPin(component.kind)) continue;
                auto_width[index][pin] = component.memory_bits;
            }
        }
    }
    for (const auto& entry : sites) {
        const auto found = coordinate_id.find(entry.first);
        if (found == coordinate_id.end()) continue;
        const NetId net = net_of_group[find(found->second)];
        for (const board_detail::PinSite& site : entry.second) {
            const BoardComponent& component = board.components[site.component];
            if (!kindHasAutoDataWidth(component.kind)) continue;
            if (site.input && isAutoWidthControlPin(component.kind, site.pin)) continue;
            const uint16_t width = result.netlist.net(net).bits;
            if (!site.input && !kindRecordWidthIsPerPin(component.kind)) {
                if (site.pin < auto_out_width[site.component].size() &&
                    width > auto_out_width[site.component][site.pin]) {
                    auto_out_width[site.component][site.pin] = width;
                }
                continue;
            }
            /* An output site is not an input width.  For most kinds the two
               agree, but a Word Maker's output is the *sum* of its inputs:
               letting that wire into the input table sized a device 16 + 8
               where the board says 8 + 8. */
            if (!site.input && (kindIsWordMaker(component.kind) || wordSplitterLeaves(component.kind) != 0))
                continue;
            if (site.pin < auto_width[site.component].size() &&
                width > auto_width[site.component][site.pin]) {
                auto_width[site.component][site.pin] = width;
            }
        }
    }
    const auto kind_bit_width = [&pins_of](size_t component_index, uint8_t pin) -> uint16_t {
        if (component_index >= pins_of.size()) return 0;
        const KindPins& pins = pins_of[component_index];
        if (pin >= pins.input_count) return 0;
        const uint16_t bits = pins.in_bits[pin];
        /* The three wide sentinels the prototype table uses for "auto" (and the
           two Splitter variants whose meaning is unmeasured) are not widths this
           builder may invent. */
        return bits < 0x7ff0u ? bits : 0;
    };
    /* One resolved data width per component: the game's own number, else the
       widest wire on a data pin, else the prototype's declared width. */
    std::vector<uint16_t> data_width(board.components.size(), 0);
    for (size_t index = 0; index < board.components.size(); ++index) {
        for (uint16_t width : auto_width[index]) {
            if (width > data_width[index]) data_width[index] = width;
        }
        if (data_width[index]) continue;
        const uint8_t pin_count = kindHasAutoRightWidth(board.components[index].kind) ? 2 : 1;
        for (uint8_t pin = 0; pin < pin_count && !data_width[index]; ++pin) {
            data_width[index] = kind_bit_width(index, pin);
        }
    }
    /* A width the prototype table does not pin down is reported here, once per
       component, whether or not a wire ever reaches it: the report has to
       happen at build time, not at "this component happens to be wired". */
    std::vector<uint16_t> leaf_width(board.components.size(), 0);
    /* The two `wauto` families, resolved here so an unwired member is reported
       even though it never reaches the device pass below. */
    std::vector<uint16_t> split_leaf_width(board.components.size(), 0);
    std::vector<std::vector<uint16_t>> maker_widths(board.components.size());
    const auto splitter_leaf_width = [&](size_t index, uint8_t leaves) -> uint16_t {
        uint16_t half = 0;
        /* A wired output already carries one leaf, so its width *is* n. */
        if (index < auto_out_width.size() && leaves) {
            for (uint8_t pin = 0; pin < leaves && pin < auto_out_width[index].size(); ++pin) {
                if (auto_out_width[index][pin] > half) half = auto_out_width[index][pin];
            }
        }
        if (index < auto_width.size() && !auto_width[index].empty() && leaves) {
            /* The Splitter's single input is `leaves * n`, which is the relation
               the board showed: 64 bits in, two 32-bit halves out. */
            const uint16_t input_width = auto_width[index][0];
            if (!half && input_width) half = static_cast<uint16_t>(input_width / leaves);
        }
        /* The record's own `+0xe0` is the leaf width for these kinds
           (measured: 0x2F … +0xe0 = 32 next to a 64-bit input). */
        if (!half && index < board.components.size() && board.components[index].memory_bits) {
            half = board.components[index].memory_bits;
        }
        if (half && static_cast<uint32_t>(half) * leaves > 64) half = 0;
        return half;
    };
    for (size_t index = 0; index < board.components.size(); ++index) {
        const BoardComponent& component = board.components[index];
        const bool word_kind = wordKindSpec(component.kind) != nullptr ||
                               wordUnarySpec(component.kind) != nullptr;
        if (word_kind && !data_width[index]) {
            result.notes.push_back(BuildNote{"component " + std::to_string(index) + " (kind " +
                                                 board_detail::kindText(component.kind) +
                                                 ") has no resolved word width; not guessed",
                                             false});
        }
        /* The word Splitter's leaves are a width too, and an unwired one has
           nothing to take them from - so it is reported here, where every
           component passes, rather than only when a wire happens to reach it. */
        if (const uint8_t leaves = wordSplitterLeaves(component.kind)) {
            split_leaf_width[index] = splitter_leaf_width(index, leaves);
            if (!split_leaf_width[index]) {
                result.notes.push_back(BuildNote{"component " + std::to_string(index) +
                                                     " (Splitter) has no resolved leaf width; not guessed",
                                                 false});
            }
        } else if (kindIsWordMaker(component.kind)) {
            bool complete = component.input_count > 0;
            for (uint8_t pin = 0; pin < component.input_count; ++pin) {
                const uint16_t width =
                    pin < auto_width[index].size() ? auto_width[index][pin] : 0;
                maker_widths[index].push_back(width);
                if (!width) complete = false;
            }
            if (!complete) {
                result.notes.push_back(BuildNote{"component " + std::to_string(index) +
                                                     " (Word Maker) has no resolved input width; not guessed",
                                                 false});
            }
        }
        if (const StorageKindSpec* storage = storageKindSpec(component.kind)) {
            const bool needs_width = storage->role == StorageRole::kRegister ||
                                     storage->role == StorageRole::kDelayLine ||
                                     storage->role == StorageRole::kCounter;
            if (needs_width && !data_width[index]) {
                result.notes.push_back(BuildNote{"component " + std::to_string(index) + " (" +
                                                     std::string(storage->name) +
                                                     ") has no resolved word width; not guessed",
                                                 false});
            }
        }
        if (const LeafKindSpec* leaf = leafKindSpec(component.kind)) {
            /* The word Splitter (0x2f) is handled by its own branch below: its
               halves come from the wires, not from the prototype's sentinel
               input width, so it must not be reported here. */
            if (component.kind == 0x2f) continue;
            leaf_width[index] = index < pins_of.size() ? leafWidth(*leaf, pins_of[index]) : 0;
            if (!leaf_width[index]) {
                result.notes.push_back(BuildNote{"component " + std::to_string(index) + " (" + leaf->name +
                                                     ") has no width its prototype pins down; not guessed",
                                                 false});
            }
        }
    }

    /* Every storage component gets its device up front, wired or not: an
       unwired register still drives its own stored value (the game reads 0 from
       a register whose enable is never asserted), so its output net must have a
       driver rather than sit at Z.  The site loop below only connects pins. */
    for (size_t index = 0; index < board.components.size(); ++index) {
        const BoardComponent& component = board.components[index];
        const StorageKindSpec* storage = storageKindSpec(component.kind);
        if (!storage) continue;
        const uint16_t width = data_width[index] ? data_width[index] : 1;
        const std::string tag = "u" + std::to_string(index) + "_" +
                                std::to_string(static_cast<int>(storage->role));
        const auto addClock = [&](const DeviceSpec& spec, DeviceId device) {
            if (result.cycle_token == kNoNet) {
                result.cycle_token = result.netlist.addNet("#cycle", 1);
                /* Every net has a slot list, and `publish()` walks them by net
                   index.  Adding a net without growing this table leaves the
                   last entry out of range, and the entry it then reads is
                   whatever the allocator left there - which is how the storage
                   family's cycle token turned into an intermittent null
                   dereference in the running game. */
                result.slots_of_net.resize(result.netlist.netCount());
            }
            result.netlist.connectInput(device, static_cast<uint16_t>(spec.clock_pin), result.cycle_token);
        };
        if (storage->role == StorageRole::kRegister) {
            const DeviceSpec& spec = deviceRegister(width, options.gate_delay);
            const DeviceId device = result.netlist.addDevice(&spec, tag + "_register");
            result.devices_by_component[index] = device;
            addClock(spec, device);
        } else if (storage->role == StorageRole::kDelayLine) {
            const DeviceSpec& spec = deviceDelayLine(width, options.gate_delay);
            const DeviceId device = result.netlist.addDevice(&spec, tag + "_delay");
            result.devices_by_component[index] = device;
            addClock(spec, device);
        } else if (storage->role == StorageRole::kCounter) {
            const DeviceSpec& spec = deviceCounter(width, options.gate_delay);
            const DeviceId device = result.netlist.addDevice(&spec, tag + "_counter");
            result.devices_by_component[index] = device;
            addClock(spec, device);
        } else if (storage->role == StorageRole::kLevelGate) {
            result.devices_by_component[index] =
                result.netlist.addDevice(&deviceWordConstant(1, 0), tag + "_levelgate");
            if (component.kind == 0x29)
                result.extra_device_of_component[index] =
                    result.netlist.addDevice(&deviceWordConstant(16, 0), tag + "_bestdelay");
        } else {
            uint16_t bits = kAutoConfigBits;
            uint64_t value = 0;
            if (component.kind == 0x5a) {
                bits = 64;
                value = component.memory_bits;
            } else if (component.kind == 0x5e) {
                bits = 64;
            }
            result.devices_by_component[index] =
                result.netlist.addDevice(&deviceWordConstant(bits, value), tag + "_constant");
        }
    }

    /* Pins attach to the net that already owns their cell. */
    for (const auto& entry : sites) {
        const std::vector<board_detail::PinSite>& pins = entry.second;
        const auto found = coordinate_id.find(entry.first);
        if (found == coordinate_id.end()) continue; /* a pin no wire reaches */
        const NetId net = net_of_group[find(found->second)];
        if (pins.size() > 1) {
            /* Name the pins, not just the cell: the cell's coordinates say
               nothing about which components met there, and a reader asking
               "is this a real connection or a geometry mistake?" needs to know
               that before they can answer.  Two pins in one cell are connected
               by the game (that is how touching components are wired), and this
               builder does the same - it attaches every pin site at the cell to
               the one net - so the note is information, not a refusal. */
            std::string detail;
            for (const board_detail::PinSite& site : pins) {
                if (!detail.empty()) detail += ", ";
                const BoardComponent& component = board.components[site.component];
                detail += "component " + std::to_string(site.component) + " (kind " +
                          board_detail::kindText(component.kind) + ") " + (site.input ? "in" : "out") +
                          std::to_string(site.pin) + " at " + std::to_string(component.x) + "," +
                          std::to_string(component.y) + " r" + std::to_string(component.rotation);
            }
            result.notes.push_back(BuildNote{"component pins " + std::to_string(pins.size()) +
                                                 " share the cell " + std::to_string(entry.first.first) + "," +
                                                 std::to_string(entry.first.second) + ": " + detail +
                                                 "; joined into one net",
                                             false});
        }
        for (const board_detail::PinSite& site : pins) {
            const BoardComponent& component = board.components[site.component];
            if (component.kind == 0x4e) {
                /* A host component (the clock, the interactive switch/button) is
                   recognized by its id, not by which side of the pin list the
                   cell came from.  Registered 0-input components can still carry
                   a scaffolding input pin in the prototype table; if a wire
                   happens to touch that cell, a pin-site-only test would call
                   the switch unknown and refuse the whole board (measured on a
                   player board: `custom component 6005349254844919857 has no
                   sandbox simulator behavior` while the switch itself was fine). */
                const bool is_clock = customIsClock(component.custom_id);
                const bool is_interactive = customIsInteractiveSource(component.custom_id);
                if (is_clock || is_interactive) {
                    if (site.input) continue; /* scaffolding pin: nothing is driven from it */
                    std::vector<NetId>& driven = is_clock ? result.clock_nets : result.source_nets;
                    if (std::find(driven.begin(), driven.end(), net) == driven.end()) {
                        driven.push_back(net);
                        if (!is_clock) {
                            result.source_origins.push_back(
                                BuildResult::SourceOrigin{component.custom_id, component.id});
                        }
                    }
                    continue;
                }
                /* An unknown custom component only matters where it drives a
                   net; its input pins are somebody else's output. */
                if (site.input) continue;
                result.notes.push_back(BuildNote{"custom component " + std::to_string(component.custom_id) +
                                                     " has no sandbox simulator behavior",
                                                 true});
                continue;
            }
            if (board_detail::isMemoryPort(component.kind)) {
                const DeviceId device = result.devices_by_component[site.component];
                if (device == kNoDevice) continue; /* the unattached-port note is already fatal */
                /* Only the visible pins were inserted into `sites`, and their
                   indices match the leading visible pins in the owned spec. */
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            if (component.kind == 0x76) continue; /* state holder; its ports are the interface */
            if (component.kind == 0x5b) {
                /* The game's Screen: Enable (1 bit), Setting index (8), Value
                   (64) and no outputs, in the catalogue's own order.  It owns
                   the settings table its pins latch into; what it *shows* is the
                   RAM it is linked to, which the game keeps on the component
                   itself (the little red button), so the picture needs that link
                   before it can be drawn - see the note in board.hpp's header. */
                if (result.devices_by_component[site.component] == kNoDevice) {
                    BuildResult::ScreenBinding binding;
                    binding.component = site.component;
                    binding.settings = std::make_shared<ScreenSettings>();
                    std::unique_ptr<DeviceSpec> spec = screenSpec(binding.settings, options.gate_delay);
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        spec.get(), "u" + std::to_string(site.component) + "_screen");
                    result.owned_specs.push_back(std::move(spec));
                    result.screens.push_back(binding);
                }
                for (BuildResult::ScreenBinding& binding : result.screens) {
                    if (binding.component != site.component) continue;
                    if (site.pin == 0) binding.enable = net;
                    else if (site.pin == 1) binding.setting_index = net;
                    else if (site.pin == 2) binding.value = net;
                }
                /* The game's own screen logic reads these three nets back, so
                   they are what a viewer has to keep in step. */
                if (std::find(result.observed_nets.begin(), result.observed_nets.end(), net) ==
                    result.observed_nets.end()) {
                    result.observed_nets.push_back(net);
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            if (component.kind == 0x2a) {
                /* The game's word Mux, built with the width the board resolved
                   for it.  Pins are the catalogue's own order, which is the one
                   the verified `byte_mux` level uses: in0 = Select, in1 = A,
                   in2 = B, out0 = Y. */
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const uint16_t width = data_width[site.component] ? data_width[site.component] : 1;
                    const DeviceSpec& spec = deviceMux(width, options.gate_delay);
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        &spec, "u" + std::to_string(site.component) + "_mux" + std::to_string(width));
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            if (kindIsTriStateSwitch(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const uint16_t width = data_width[site.component] ? data_width[site.component] : 1;
                    const DeviceSpec& spec = deviceTriState(width, options.gate_delay);
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        &spec, "u" + std::to_string(site.component) + "_switch" + std::to_string(width));
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            /* The Decoder family: one device per component, its shape fixed by
               the prototype table (`decoderKindSpec`). */
            if (const DecoderKindSpec* decoder = decoderKindSpec(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const DeviceSpec& spec = deviceDecoder(decoder->address_bits, decoder->has_enable,
                                                           options.gate_delay);
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        &spec, "u" + std::to_string(site.component) + "_decoder" +
                                   std::to_string(decoder->address_bits));
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            /* The two custom-component interface pins: one signal through, at
               the width the netlist resolved for them. */
            if (isPinKind(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const uint16_t width = data_width[site.component] ? data_width[site.component] : 1;
                    const DeviceSpec& spec = devicePin(width, options.gate_delay);
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        &spec, "u" + std::to_string(site.component) + "_pin" + std::to_string(width));
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            /* The Static Indexer (0x65): its configuration is the component's
               own record - `values[0]` is the signed index and `bits` is the
               output width - so nothing about it has to be recovered from the
               wires.  See `deviceIndexer` for the rule and its evidence. */
            if (isIndexerKind(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const int32_t index = static_cast<int32_t>(component.setting_value);
                    const int32_t width = static_cast<int32_t>(component.field_bits);
                    if (width <= 0 || width > 64) {
                        result.notes.push_back(BuildNote{"component " + std::to_string(site.component) +
                                                             " (Static Indexer) declares a "
                                                             + std::to_string(width) +
                                                             "-bit field; not guessed",
                                                         false});
                        continue;
                    }
                    const DeviceSpec& spec = deviceIndexer(index, static_cast<uint16_t>(width),
                                                           options.gate_delay);
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        &spec, "u" + std::to_string(site.component) + "_indexer" +
                                   std::to_string(index) + "_" + std::to_string(width));
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            /* A Splitter over words: `leaves` equal slices of one input, the
               topmost slice most significant.  Its prototype widths are
               sentinels, so the leaf width was resolved above from the wires or
               from the record's own number. */
            if (wordSplitterLeaves(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const uint16_t half = split_leaf_width[site.component];
                    if (!half) continue; /* already reported at build time */
                    const DeviceSpec& spec = deviceWordSplitter(wordSplitterLeaves(component.kind), half,
                                                                options.gate_delay);
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        &spec, "u" + std::to_string(site.component) + "_wordsplit" +
                                   std::to_string(wordSplitterLeaves(component.kind)) + "x" +
                                   std::to_string(half));
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            /* A Word Maker (or a wide Concatenator): every input keeps its own
               width and the output is their concatenation.  Measured on the
               player's board: `kind 0x30 … in0 8 bits, in1 8 bits -> out0 16
               bits`. */
            if (kindIsWordMaker(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const std::vector<uint16_t>& widths = maker_widths[site.component];
                    if (widths.empty() ||
                        std::any_of(widths.begin(), widths.end(), [](uint16_t width) { return width == 0; })) {
                        continue; /* already reported at build time */
                    }
                    const DeviceSpec& spec = deviceWordMaker(widths, options.gate_delay);
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        &spec, "u" + std::to_string(site.component) + "_wordmaker");
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            if (const LeafKindSpec* leaf = leafKindSpec(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const uint16_t width = leaf_width[site.component];
                    if (!width) continue; /* already reported at build time */
                    const DeviceSpec* spec = leaf->op == WordOp::kSplit
                                                 ? &deviceSplitter(width)
                                                 : &deviceMaker(width);
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        spec, "u" + std::to_string(site.component) + "_" + leaf->name);
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            /* The word operations: one device per component, its width taken
               from the record (or from the wires when the record carries 0). */
            if (isConcatKind(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const uint16_t high = kind_bit_width(site.component, 0);
                    const uint16_t low = kind_bit_width(site.component, 1);
                    if (!high || !low) {
                        result.notes.push_back(BuildNote{"component " + std::to_string(site.component) +
                                                             " (Concatenator) has no declared pin widths",
                                                         false});
                        continue;
                    }
                    const DeviceSpec& spec = deviceConcat(high, low, options.gate_delay);
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        &spec, "u" + std::to_string(site.component) + "_concat");
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            if (const WordKindSpec* word = wordKindSpec(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    /* No width anywhere is not a one-bit device: the width check
                       above already reported it for this component. */
                    if (!data_width[site.component]) continue;
                    const uint16_t width = data_width[site.component];
                    /* A parameter pin keeps the width of the wire that reaches
                       it: the shift amount is not a same-width operand. */
                    uint16_t right = width;
                    if (word->right == WordKind::kControl) {
                        const size_t pin = 1;
                        right = pin < auto_width[site.component].size() && auto_width[site.component][pin]
                                    ? auto_width[site.component][pin]
                                    : 1;
                    }                    const DeviceSpec* spec = builtinWordDevice(word->device, width, right, options.gate_delay);
                    if (!spec) {
                        result.notes.push_back(BuildNote{"word device " + std::string(word->device) +
                                                             " is missing from the builtin library",
                                                         false});
                        continue;
                    }
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        spec, "u" + std::to_string(site.component) + "_" + word->device);
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            if (const WordUnarySpec* unary = wordUnarySpec(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const uint16_t width = data_width[site.component] ? data_width[site.component] : 1;
                    const DeviceSpec* spec = builtinWordDevice(unary->device, width, width, options.gate_delay);
                    if (!spec) {
                        result.notes.push_back(BuildNote{"word device " + std::string(unary->device) +
                                                             " is missing from the builtin library",
                                                         false});
                        continue;
                    }
                    result.devices_by_component[site.component] = result.netlist.addDevice(
                        spec, "u" + std::to_string(site.component) + "_" + unary->device);
                }
                const DeviceId device = result.devices_by_component[site.component];
                if (site.input) result.netlist.connectInput(device, site.pin, net);
                else result.netlist.connectOutput(device, site.pin, net);
                continue;
            }
            if (kindIsConstant(component.kind)) {
                if (result.devices_by_component[site.component] == kNoDevice) {
                    const DeviceSpec& spec = deviceConstant(constantValue(component), options.gate_delay.tplh);
                    result.devices_by_component[site.component] =
                        result.netlist.addDevice(&spec, "u" + std::to_string(site.component) + "_constant");
                }
                if (!site.input) {
                    result.netlist.connectOutput(result.devices_by_component[site.component], site.pin, net);
                }
                continue;
            }
            /* The game's own storage parts, the Auto-size pair, and the two
               level-map components.  See storageKindSpec / deviceRegister. */
            if (const StorageKindSpec* storage = storageKindSpec(component.kind)) {
                const DeviceId device = result.devices_by_component[site.component];
                if (device == kNoDevice) continue;
                if (site.input) {
                    /* A level-map component's input is still worth observing -
                       it is the level's own signal. */
                    if (storage->role == StorageRole::kLevelGate) {
                        if (std::find(result.observed_nets.begin(), result.observed_nets.end(), net) ==
                            result.observed_nets.end()) {
                            result.observed_nets.push_back(net);
                        }
                        continue;
                    }
                    result.netlist.connectInput(device, site.pin, net);
                } else if (site.pin == 0) {
                    result.netlist.connectOutput(device, 0, net);
                } else if (component.kind == 0x29 && site.pin == 1) {
                    const auto extra = result.extra_device_of_component.find(site.component);
                    if (extra != result.extra_device_of_component.end())
                        result.netlist.connectOutput(extra->second, 0, net);
                }
                continue;
            }
            if (kindIsSource(component.kind)) {
                /* A source drives its outputs; its own input pins (the Keyboard
                   has one) are somebody else's output. */
                if (site.input) continue;
                if (std::find(result.source_nets.begin(), result.source_nets.end(), net) == result.source_nets.end()) {
                    result.source_nets.push_back(net);
                    /* A builtin source (a level input, a Verilog input) is read
                       from the game's state array. */
                    result.source_origins.push_back(BuildResult::SourceOrigin{});
                }
                continue;
            }
            if (kindIsSink(component.kind)) {
                if (std::find(result.observed_nets.begin(), result.observed_nets.end(), net) ==
                    result.observed_nets.end()) {
                    result.observed_nets.push_back(net);
                }
                continue;
            }
            const std::string name = deviceNameForKind(component.kind);
            if (name.empty()) {
                result.notes.push_back(BuildNote{"component " + std::to_string(site.component) + " (kind " +
                                                     board_detail::kindText(component.kind) +
                                                     ") has no device in the library",
                                                 true});
                continue;
            }
            if (result.devices_by_component[site.component] == kNoDevice) {
                const DeviceSpec* spec = builtinDevice(name, options.gate_delay);
                if (!spec) {
                    result.notes.push_back(BuildNote{"device " + name + " is missing from the builtin library", false});
                    continue;
                }
                result.devices_by_component[site.component] =
                    result.netlist.addDevice(spec, "u" + std::to_string(site.component) + "_" + name);
            }
            const DeviceId device = result.devices_by_component[site.component];
            if (site.input) {
                result.netlist.connectInput(device, site.pin, net);
            } else {
                result.netlist.connectOutput(device, site.pin, net);
            }
        }
    }

    for (const auto& entry : sites) {
        if (coordinate_id.find(entry.first) == coordinate_id.end()) ++result.unconnected_pins;
    }
    for (const std::string& problem : result.netlist.problems) {
        result.notes.push_back(BuildNote{problem, true});
    }
    return result;
}

/* The record decoding, kept next to the builder so the plugin and the offline
   test read the bytes the same way. */
/* One component record (0x238 bytes).  The same decoder serves the board and a
   custom definition's own component array: the game stores a definition's
   internals in the identical record shape (see docs/research/custom-components.md),
   and a reader that decoded them separately would drift the moment one side
   gained a field. */
inline BoardComponent readComponentRecord(const unsigned char* record) {
    BoardComponent component;
    if (!record) return component;
    component.kind = record[0];
    std::memcpy(&component.x, record + 2, 2);
    std::memcpy(&component.y, record + 4, 2);
    component.rotation = record[6];
    component.id = board_detail::readU64(record, 8);
    component.input_count = static_cast<uint8_t>(board_detail::readU64(record, 0x30));
    component.output_count = static_cast<uint8_t>(board_detail::readU64(record, 0x40));
    component.private_pointer = board_detail::readU64(record, 0x38);
    component.custom_id = board_detail::readU64(record, 0x188);
    component.memory_bits = static_cast<uint16_t>(board_detail::readU64(record, 0xe0));
    component.memory_extra_address_bits = static_cast<uint16_t>(board_detail::readU64(record, 0xf0));
    component.field_bits = static_cast<uint16_t>(board_detail::readU64(record, 0x70));
    component.memory_little_endian = record[0x178] != 0;
    const uint64_t setting_count = board_detail::readU64(record, 0xa8);
    const unsigned char* settings = nullptr;
    std::memcpy(&settings, record + 0xb0, sizeof(settings));
    if (setting_count > 0 && setting_count < 1024 &&
        reinterpret_cast<uintptr_t>(settings) >= 0x10000) {
        component.setting_value = board_detail::readU64(settings, 8);
        component.has_setting_value = true;
    }
    return component;
}

/* One wire record - the same shape for a board and for a definition's own
   wires (stride 0x68; the earlier "0x40 for a definition" was the probe's own
   guess and made most internal wires read as garbage). */
inline BoardWire readWireRecord(const unsigned char* record) {
    BoardWire wire;
    if (!record) return wire;
    std::memcpy(&wire.x1, record + 0x18, 2);
    std::memcpy(&wire.y1, record + 0x1a, 2);
    std::memcpy(&wire.x2, record + 0x1c, 2);
    std::memcpy(&wire.y2, record + 0x1e, 2);
    uint32_t width = 0;
    std::memcpy(&width, record + 0x30, 4);
    wire.width = static_cast<uint16_t>(width);
    wire.slot = board_detail::readU64(record, 0x38);
    return wire;
}

inline BoardView readBoard(const unsigned char* board, uint64_t component_stride = 0x238,
                           uint64_t wire_stride = 0x68) {
    BoardView view;
    if (!board) return view;
    uint64_t components = 0;
    const unsigned char* component_data = nullptr;
    uint64_t wires = 0;
    const unsigned char* wire_data = nullptr;
    components = board_detail::readU64(board, 0x78);
    std::memcpy(&component_data, board + 0x80, sizeof(component_data));
    wires = board_detail::readU64(board, 0x98);
    std::memcpy(&wire_data, board + 0xa0, sizeof(wire_data));
    if (component_data && components <= 4096) {
        for (uint64_t index = 0; index < components; ++index) {
            const unsigned char* record = component_data + 8 + index * component_stride;
            view.components.push_back(readComponentRecord(record));
        }
    }
    if (wire_data && wires <= 65536) {
        for (uint64_t index = 0; index < wires; ++index) {
            const unsigned char* record = wire_data + 8 + index * wire_stride;
            view.wires.push_back(readWireRecord(record));
        }
    }
    return view;
}

}  // namespace tcsim
