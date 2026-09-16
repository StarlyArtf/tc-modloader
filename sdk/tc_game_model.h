#ifndef TC_GAME_MODEL_H
#define TC_GAME_MODEL_H

// Minimal game object model for the pinned Turing Complete 2.1.334 build.
//
// This header wraps a few verified model/prototype symbols without pretending
// to know the whole Nim object graph.  It intentionally exposes only the
// portions whose ABI and memory layout were verified against the build:
//
//   get_prototype__modelZboardZcustom95prototype95list_u502
//   get_custom_prototype__modelZboardZcustom95prototype95list_u451
//   get_input_word_size__modelZboardZprototype95list_u4196
//   get_output_word_size__modelZboardZprototype95list_u4353
//   AUTO_SIZE__modelZmodel95types_u54
//   custom_prototypes_set__modelZboardZcustom95prototype95list_u192
//   custom_prototypes_del__modelZboardZcustom95prototype95list_u291
//   in_custom_prototypes__modelZboardZcustom95prototype95list_u9
//   notin_custom_prototypes__modelZboardZcustom95prototype95list_u189
//   cc_length__modelZboardZcustom95prototype95list_u8
//   cc_live_values__modelZboardZcustom95prototype95list_u7
//
// The remaining fields are left as opaque raw bytes.  Do not pass unknown
// built-in kind bytes to getPrototype(); the pinned build raises a Nim index
// or key error for unknown keys, and a native plugin cannot safely recover
// from that.  Custom prototype IDs are safe to query and return an empty
// prototype when absent.

#include "tc_mod_api.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace tc {

// get_prototype() checks this discriminator first.  0x4e ('N') selects the
// custom-prototype branch and causes the uint16_t at +0x188 to be used as the
// custom prototype ID.
enum : uint8_t { kPrototypeKindCustom = 0x4e };

// The only two fields read by get_prototype() in the pinned build.  Keep this
// struct as raw storage rather than a meaningful object model.
struct TCPrototypeKind {
    uint8_t tag;
    uint8_t reserved[0x187];
    uint16_t custom_id;  // offset 0x188
};

// Verified output size of get_prototype()/get_custom_prototype().  The actual
// object is a Nim Prototype; only the offsets below are currently exposed.
struct TCPrototype {
    alignas(8) unsigned char bytes[0x5a8];
};

// Input/output pin entries in a Prototype are each 0x38 bytes.  The raw word
// size value is the 8 bytes at +0x10 (verified in get_input_word_size and
// get_output_word_size).
struct TCPin {
    alignas(8) unsigned char bytes[0x38];
};

using TCGetPrototypeFn = void (*)(const void* kind, void* out);
using TCGetCustomPrototypeFn = void (*)(uint64_t custom_id, void* out);
using TCWordSizeFn = uint64_t (*)(uint8_t kind, uint16_t pin_index,
                                  const void* expected_word_size);
using TCCustomPrototypeSetFn = void (*)(uint64_t custom_id,
                                        const void* prototype);
using TCCustomPrototypeDelFn = void (*)(uint64_t custom_id);
using TCCustomPrototypeContainsFn = uint8_t (*)(uint64_t custom_id);

struct TCGameModel {
    TCGetPrototypeFn get_prototype = nullptr;
    TCGetCustomPrototypeFn get_custom_prototype = nullptr;
    TCWordSizeFn get_input_word_size = nullptr;
    TCWordSizeFn get_output_word_size = nullptr;
    const void* auto_size = nullptr;
    const void* prototypes_table = nullptr;
    const void* category_order = nullptr;
    TCCustomPrototypeSetFn custom_prototypes_set = nullptr;
    TCCustomPrototypeDelFn custom_prototypes_del = nullptr;
    TCCustomPrototypeContainsFn in_custom_prototypes = nullptr;
    TCCustomPrototypeContainsFn notin_custom_prototypes = nullptr;
    const uint64_t* custom_prototype_count = nullptr;
    const void* custom_prototype_live_values = nullptr;

    // Returns true when the essential prototype lookup functions resolved.
    bool valid() const {
        return get_prototype != nullptr && get_custom_prototype != nullptr;
    }

    // Registration/list primitives.  set/del are the low-level verified
    // mutation APIs; add_custom_prototype (file/parse based) is intentionally
    // not wrapped yet because its full stack-argument layout is still being
    // verified.
    bool mutationValid() const {
        return custom_prototypes_set != nullptr &&
               custom_prototypes_del != nullptr &&
               in_custom_prototypes != nullptr &&
               notin_custom_prototypes != nullptr;
    }

    // Resolve the pinned COFF symbols.  Returns false only if the host is
    // unavailable; individual symbols may still be null and are checked by
    // valid()/wordSizeValid().
    bool load(const TCHost* host) {
        if (host == nullptr) return false;

        auto resolve = [&](const char* name) {
            return host->resolve_symbol(host->context, name);
        };

        get_prototype = reinterpret_cast<TCGetPrototypeFn>(
            resolve("get_prototype__modelZboardZcustom95prototype95list_u502"));
        get_custom_prototype = reinterpret_cast<TCGetCustomPrototypeFn>(
            resolve("get_custom_prototype__modelZboardZcustom95prototype95list_u451"));
        get_input_word_size = reinterpret_cast<TCWordSizeFn>(
            resolve("get_input_word_size__modelZboardZprototype95list_u4196"));
        get_output_word_size = reinterpret_cast<TCWordSizeFn>(
            resolve("get_output_word_size__modelZboardZprototype95list_u4353"));
        auto_size = resolve("AUTO_SIZE__modelZmodel95types_u54");
        prototypes_table = resolve("PROTOTYPES__modelZboardZprototype95list_u3772");
        category_order = resolve("CATEGORY_ORDER__modelZboardZprototype95list_u21");
        custom_prototypes_set = reinterpret_cast<TCCustomPrototypeSetFn>(
            resolve("custom_prototypes_set__modelZboardZcustom95prototype95list_u192"));
        custom_prototypes_del = reinterpret_cast<TCCustomPrototypeDelFn>(
            resolve("custom_prototypes_del__modelZboardZcustom95prototype95list_u291"));
        in_custom_prototypes = reinterpret_cast<TCCustomPrototypeContainsFn>(
            resolve("in_custom_prototypes__modelZboardZcustom95prototype95list_u9"));
        notin_custom_prototypes = reinterpret_cast<TCCustomPrototypeContainsFn>(
            resolve("notin_custom_prototypes__modelZboardZcustom95prototype95list_u189"));
        custom_prototype_count = reinterpret_cast<const uint64_t*>(
            resolve("cc_length__modelZboardZcustom95prototype95list_u8"));
        custom_prototype_live_values =
            resolve("cc_live_values__modelZboardZcustom95prototype95list_u7");
        return true;
    }

    bool wordSizeValid() const {
        return get_input_word_size != nullptr &&
               get_output_word_size != nullptr && auto_size != nullptr;
    }

    // Fetch a prototype into out.  For custom prototypes use
    // kPrototypeKindCustom; for built-ins pass the known single-byte kind.
    // Unknown built-in kinds are not safe and are the caller's responsibility.
    bool getPrototype(uint8_t kind, uint64_t custom_id, TCPrototype& out) const {
        if (!valid()) return false;
        memset(out.bytes, 0, sizeof(out.bytes));
        if (kind == kPrototypeKindCustom) {
            get_custom_prototype(custom_id, &out);
        } else {
            TCPrototypeKind key{};
            key.tag = kind;
            key.custom_id = custom_id;
            get_prototype(&key, &out);
        }
        return true;
    }

    bool getCustomPrototype(uint64_t custom_id, TCPrototype& out) const {
        if (!get_custom_prototype) return false;
        memset(out.bytes, 0, sizeof(out.bytes));
        get_custom_prototype(custom_id, &out);
        return true;
    }

    uint64_t inputWordSize(uint8_t kind, uint16_t pin_index) const {
        if (!wordSizeValid()) return 0;
        return get_input_word_size(kind, pin_index, auto_size);
    }

    uint64_t outputWordSize(uint8_t kind, uint16_t pin_index) const {
        if (!wordSizeValid()) return 0;
        return get_output_word_size(kind, pin_index, auto_size);
    }

    uint64_t customPrototypeCount() const {
        if (!custom_prototype_count) return 0;
        uint64_t value = 0;
        memcpy(&value, custom_prototype_count, sizeof(value));
        return value;
    }

    // cc_live_values entries are 16 bytes: {uint64_t id, uint16_t id_low,
    // reserved[6]}.  Return the full id at live_index.
    uint64_t customPrototypeIdAt(uint64_t live_index) const {
        if (!custom_prototype_live_values ||
            live_index >= customPrototypeCount()) {
            return 0;
        }
        const auto* entry = static_cast<const unsigned char*>(
                                custom_prototype_live_values) +
                            live_index * 16;
        uint64_t value = 0;
        memcpy(&value, entry, sizeof(value));
        return value;
    }

    bool hasCustomPrototype(uint64_t custom_id) const {
        return in_custom_prototypes != nullptr &&
               in_custom_prototypes(custom_id) != 0;
    }

    bool setCustomPrototype(uint64_t custom_id,
                            const TCPrototype& prototype) const {
        if (!custom_prototypes_set) return false;
        custom_prototypes_set(custom_id, &prototype);
        return true;
    }

    bool removeCustomPrototype(uint64_t custom_id) const {
        if (!custom_prototypes_del) return false;
        custom_prototypes_del(custom_id);
        return true;
    }
};

// Verified Prototype offsets:
//   +0x60  input pin sequence length (uint64_t)
//   +0x68  input pin sequence data pointer (TCPin*)
//   +0x80  output pin sequence length (uint64_t)
//   +0x88  output pin sequence data pointer (TCPin*)
inline uint64_t prototypeInputCount(const TCPrototype& p) {
    uint64_t value = 0;
    memcpy(&value, p.bytes + 0x60, sizeof(value));
    return value;
}

inline uint64_t prototypeOutputCount(const TCPrototype& p) {
    uint64_t value = 0;
    memcpy(&value, p.bytes + 0x80, sizeof(value));
    return value;
}

inline void prototypeSetInputCount(TCPrototype& p, uint64_t count) {
    memcpy(p.bytes + 0x60, &count, sizeof(count));
}

inline void prototypeSetInputPins(TCPrototype& p, TCPin* pins) {
    memcpy(p.bytes + 0x68, &pins, sizeof(pins));
}

inline void prototypeSetOutputCount(TCPrototype& p, uint64_t count) {
    memcpy(p.bytes + 0x80, &count, sizeof(count));
}

inline void prototypeSetOutputPins(TCPrototype& p, TCPin* pins) {
    memcpy(p.bytes + 0x88, &pins, sizeof(pins));
}

inline TCPin* prototypeInputPin(const TCPrototype& p, uint64_t index) {
    if (index >= prototypeInputCount(p)) return nullptr;
    void* data = nullptr;
    memcpy(&data, p.bytes + 0x68, sizeof(data));
    return static_cast<TCPin*>(data) + index;
}

inline TCPin* prototypeOutputPin(const TCPrototype& p, uint64_t index) {
    if (index >= prototypeOutputCount(p)) return nullptr;
    void* data = nullptr;
    memcpy(&data, p.bytes + 0x88, sizeof(data));
    return static_cast<TCPin*>(data) + index;
}

inline uint64_t pinWordSizeRaw(const TCPin& pin) {
    uint64_t value = 0;
    memcpy(&value, pin.bytes + 0x10, sizeof(value));
    return value;
}

}  // namespace tc

#endif  // TC_GAME_MODEL_H
