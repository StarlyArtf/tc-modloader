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
//   PROTOTYPES table layout (length, bucket pointer, 0x5b8-byte buckets,
//     key byte at +0x10, nonzero hash at +0x08)
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
enum : size_t {
    kPrototypeNameOffset = 0x10,
    kPrototypeDescriptionOffset = 0x28,
    kPrototypeCategoryRawOffset = 0x40,
    kPrototypeFlagsRawOffset = 0x48,
    kPrototypeShapeSvgOffset = 0xb0
};
enum : uint64_t {
    kPrototypeBucketStride = 0x5b8,
    kPrototypeBucketHashOffset = 0x08,
    kPrototypeBucketKeyOffset = 0x10
};

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

// Nim string object as used by the pinned build: length followed by a
// pointer to the payload.  rawNewString() creates such an object.
struct TCNimString {
    uint64_t length;
    void* data;
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
using TCRawNewStringFn = void (*)(void* out, int64_t length);
using TCCustomPrototypeSetFn = void (*)(uint64_t custom_id,
                                        const void* prototype);
using TCCustomPrototypeDelFn = void (*)(uint64_t custom_id);
using TCCustomPrototypeContainsFn = uint8_t (*)(uint64_t custom_id);

struct TCGameModel {
    TCGetPrototypeFn get_prototype = nullptr;
    TCGetCustomPrototypeFn get_custom_prototype = nullptr;
    TCWordSizeFn get_input_word_size = nullptr;
    TCWordSizeFn get_output_word_size = nullptr;
    TCRawNewStringFn raw_new_string = nullptr;
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
        raw_new_string = reinterpret_cast<TCRawNewStringFn>(
            resolve("rawNewString"));
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

    bool stringAllocValid() const {
        return raw_new_string != nullptr;
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

    // Copy a verified built-in prototype into out.  This is the safe base for
    // a programmatic custom component: copy it, adjust input/output pins and
    // any other raw fields, then call setCustomPrototype().
    bool cloneBuiltinPrototype(uint8_t kind, TCPrototype& out) const {
        if (!isBuiltinPrototypeKind(kind)) return false;
        return getPrototype(kind, 0, out);
    }

    // Convenience helper for the common template workflow.  The copied
    // prototype is registered under custom_id; the caller should keep the
    // returned out alive only as a working copy, since setCustomPrototype
    // deep-copies it into the game's custom prototype table.
    bool registerBuiltinAsCustom(uint8_t kind, uint64_t custom_id,
                                 TCPrototype& out) const {
        if (!cloneBuiltinPrototype(kind, out)) return false;
        return setCustomPrototype(custom_id, out);
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

    // Enumerate the built-in PROTOTYPES hash table without ever passing an
    // unknown key to get_prototype().  The table object is two qwords:
    // length, bucket pointer.  Buckets are 0x5b8 bytes; an occupied bucket
    // has a nonzero hash at +0x08 and its single-byte key at +0x10.
    uint64_t builtinPrototypeCount() const {
        if (!prototypes_table) return 0;
        uint64_t length = 0;
        void* buckets = nullptr;
        memcpy(&length, prototypes_table, sizeof(length));
        memcpy(&buckets, static_cast<const unsigned char*>(prototypes_table) + 8,
               sizeof(buckets));
        if (!buckets || length > 4096) return 0;

        uint64_t count = 0;
        for (uint64_t i = 0; i < length; ++i) {
            uint64_t hash = 0;
            const auto* bucket =
                static_cast<const unsigned char*>(buckets) +
                i * kPrototypeBucketStride;
            memcpy(&hash, bucket + kPrototypeBucketHashOffset, sizeof(hash));
            if (hash != 0) ++count;
        }
        return count;
    }

    uint8_t builtinPrototypeKindAt(uint64_t occupied_index) const {
        if (!prototypes_table || occupied_index >= builtinPrototypeCount()) {
            return 0;
        }
        uint64_t length = 0;
        void* buckets = nullptr;
        memcpy(&length, prototypes_table, sizeof(length));
        memcpy(&buckets, static_cast<const unsigned char*>(prototypes_table) + 8,
               sizeof(buckets));
        if (!buckets || length > 4096) return 0;

        uint64_t seen = 0;
        for (uint64_t i = 0; i < length; ++i) {
            uint64_t hash = 0;
            const auto* bucket =
                static_cast<const unsigned char*>(buckets) +
                i * kPrototypeBucketStride;
            memcpy(&hash, bucket + kPrototypeBucketHashOffset, sizeof(hash));
            if (hash == 0) continue;
            if (seen == occupied_index) {
                uint8_t key = 0;
                memcpy(&key, bucket + kPrototypeBucketKeyOffset, sizeof(key));
                return key;
            }
            ++seen;
        }
        return 0;
    }

    bool isBuiltinPrototypeKind(uint8_t kind) const {
        return builtinPrototypeIndexForKind(kind) != ~uint64_t{0};
    }

    uint64_t builtinPrototypeIndexForKind(uint8_t kind) const {
        if (!prototypes_table) return ~uint64_t{0};
        uint64_t length = 0;
        void* buckets = nullptr;
        memcpy(&length, prototypes_table, sizeof(length));
        memcpy(&buckets, static_cast<const unsigned char*>(prototypes_table) + 8,
               sizeof(buckets));
        if (!buckets || length > 4096) return ~uint64_t{0};

        uint64_t occupied = 0;
        for (uint64_t i = 0; i < length; ++i) {
            uint64_t hash = 0;
            const auto* bucket =
                static_cast<const unsigned char*>(buckets) +
                i * kPrototypeBucketStride;
            memcpy(&hash, bucket + kPrototypeBucketHashOffset, sizeof(hash));
            if (hash == 0) continue;
            uint8_t key = 0;
            memcpy(&key, bucket + kPrototypeBucketKeyOffset, sizeof(key));
            if (key == kind) return occupied;
            ++occupied;
        }
        return ~uint64_t{0};
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

    // Replace the verified name/description fields with a newly allocated
    // Nim string.  The old template string is not freed; use this only on a
    // working copy before registering it, never on a live table entry.
    bool setPrototypeName(TCPrototype& prototype, const char* utf8) const {
        return setPrototypeString(prototype, utf8, 0x10);
    }

    bool setPrototypeDescription(TCPrototype& prototype,
                                 const char* utf8) const {
        return setPrototypeString(prototype, utf8, kPrototypeDescriptionOffset);
    }

    bool setPrototypeShapeSvg(TCPrototype& prototype,
                              const char* utf8) const {
        return setPrototypeString(prototype, utf8, kPrototypeShapeSvgOffset);
    }

    bool setPrototypeString(TCPrototype& prototype, const char* utf8,
                            size_t field_offset) const {
        if (!raw_new_string || utf8 == nullptr ||
            field_offset > sizeof(prototype.bytes) ||
            sizeof(TCNimString) > sizeof(prototype.bytes) - field_offset) {
            return false;
        }
        const size_t length = strlen(utf8);
        if (length > INT64_MAX) return false;

        TCNimString value{};
        raw_new_string(&value, static_cast<int64_t>(length));
        if (!value.data) return false;

        // Nim payload: length/capacity at +0, UTF-8 bytes at +8.
        memset(static_cast<unsigned char*>(value.data) + 8, 0, length + 1);
        memcpy(static_cast<unsigned char*>(value.data) + 8, utf8, length);

        memcpy(prototype.bytes + field_offset, &value, sizeof(value));
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

inline TCNimString prototypeName(const TCPrototype& p) {
    TCNimString value{};
    memcpy(&value.length, p.bytes + kPrototypeNameOffset, sizeof(value.length));
    memcpy(&value.data, p.bytes + kPrototypeNameOffset + 8, sizeof(value.data));
    return value;
}

inline TCNimString prototypeDescription(const TCPrototype& p) {
    TCNimString value{};
    memcpy(&value.length, p.bytes + kPrototypeDescriptionOffset, sizeof(value.length));
    memcpy(&value.data, p.bytes + kPrototypeDescriptionOffset + 8, sizeof(value.data));
    return value;
}

inline TCNimString prototypeShapeSvg(const TCPrototype& p) {
    TCNimString value{};
    memcpy(&value.length, p.bytes + kPrototypeShapeSvgOffset, sizeof(value.length));
    memcpy(&value.data, p.bytes + kPrototypeShapeSvgOffset + 8, sizeof(value.data));
    return value;
}

inline uint64_t prototypeCategoryRaw(const TCPrototype& p) {
    uint64_t value = 0;
    memcpy(&value, p.bytes + kPrototypeCategoryRawOffset, sizeof(value));
    return value;
}

inline uint64_t prototypeFlagsRaw(const TCPrototype& p) {
    uint64_t value = 0;
    memcpy(&value, p.bytes + kPrototypeFlagsRawOffset, sizeof(value));
    return value;
}

inline const char* prototypeNameCStr(const TCPrototype& p) {
    TCNimString value = prototypeName(p);
    return value.data ? static_cast<const char*>(value.data) + 8 : nullptr;
}

inline const char* prototypeDescriptionCStr(const TCPrototype& p) {
    TCNimString value = prototypeDescription(p);
    return value.data ? static_cast<const char*>(value.data) + 8 : nullptr;
}

inline const char* prototypeShapeSvgCStr(const TCPrototype& p) {
    TCNimString value = prototypeShapeSvg(p);
    return value.data ? static_cast<const char*>(value.data) + 8 : nullptr;
}

// Small convenience wrapper for the verified template workflow.  It owns a
// working TCPrototype copy and keeps the model reference used to register it.
// Identity/visual fields not yet reverse-engineered remain raw bytes; they can
// be modified through setRawField() when their offsets are known.
class TCPrototypeBuilder {
 public:
    TCPrototypeBuilder(const TCGameModel& model, uint8_t builtin_kind)
        : model_(&model), kind_(builtin_kind), ready_(false) {
        ready_ = model.cloneBuiltinPrototype(builtin_kind, prototype_);
    }

    bool ready() const { return ready_; }
    uint8_t kind() const { return kind_; }
    TCPrototype& prototype() { return prototype_; }
    const TCPrototype& prototype() const { return prototype_; }

    void setInputCount(uint64_t count) {
        prototypeSetInputCount(prototype_, count);
    }

    void setInputPins(TCPin* pins) {
        prototypeSetInputPins(prototype_, pins);
    }

    void setOutputCount(uint64_t count) {
        prototypeSetOutputCount(prototype_, count);
    }

    void setOutputPins(TCPin* pins) {
        prototypeSetOutputPins(prototype_, pins);
    }

    void setCategoryRaw(uint64_t value) {
        setRawField(kPrototypeCategoryRawOffset, &value, sizeof(value));
    }

    void setFlagsRaw(uint64_t value) {
        setRawField(kPrototypeFlagsRawOffset, &value, sizeof(value));
    }

    bool setName(const char* utf8) {
        return model_->setPrototypeName(prototype_, utf8);
    }

    bool setDescription(const char* utf8) {
        return model_->setPrototypeDescription(prototype_, utf8);
    }

    bool setShapeSvg(const char* utf8) {
        return model_->setPrototypeShapeSvg(prototype_, utf8);
    }

    void setRawField(size_t offset, const void* data, size_t size) {
        if (offset <= sizeof(prototype_.bytes) &&
            size <= sizeof(prototype_.bytes) - offset) {
            memcpy(prototype_.bytes + offset, data, size);
        }
    }

    bool registerAsCustom(uint64_t custom_id) const {
        return ready_ && model_->setCustomPrototype(custom_id, prototype_);
    }

 private:
    const TCGameModel* model_;
    uint8_t kind_;
    bool ready_;
    TCPrototype prototype_{};
};

}  // namespace tc

#endif  // TC_GAME_MODEL_H
