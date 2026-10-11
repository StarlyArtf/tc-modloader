#pragma once
#include "../sdk/tc_service_api.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* tc.component.editor V1 (plan §14): the declarative half of a component's
   property editor.

   A Mod already can draw its own rows into the game's component drawer through
   TC_UI_SLOT_BOARD_COMPONENT_PANEL, but measured on this build the drawer itself
   never becomes the window under the mouse (`IsWindowHovered` is false for it
   while the pointer sits on a row the plugin drew inside it), so every Mod that
   wants *working* widgets has to open its own window layered over the panel and
   repeat that recipe - see docs/sdk/ui.md and examples/float-ops/components_ui.cpp.

   This service takes that recipe into the host: a type declares its editable
   fields once, the host draws the rows over the drawer and commits each edit
   through the same configuration path every other write uses (so one committed
   field is one undo step and the type's `on_config_changed` fires).

   Everything in this header is pure: no ImGui, no game state, no ABI.  The
   native side copies the fields, calls validate(), and uses read()/apply() as
   the model behind the host's widgets; the offline tests drive the same paths. */

namespace tc::component_editor {

inline constexpr uint32_t kMaxFields = TC_COMPONENT_EDITOR_MAX_FIELDS;
inline constexpr uint32_t kMaxTextBytes = TC_COMPONENT_EDITOR_MAX_TEXT_BYTES;
/* The V1 integer widget is a signed 32-bit box, so a UINT field may declare at
   most 31 meaningful bits unless it asks for the hexadecimal widget, which
   parses up to 64 bits of text (V2). */
inline constexpr uint32_t kMaxUintBits = 31;
inline constexpr uint32_t kMaxHexBits = 64;

enum class Kind : uint32_t {
    Bool = TC_COMPONENT_FIELD_BOOL,
    Int = TC_COMPONENT_FIELD_INT,
    Uint = TC_COMPONENT_FIELD_UINT,
    Enum = TC_COMPONENT_FIELD_ENUM,
    Text = TC_COMPONENT_FIELD_TEXT,
    Float = TC_COMPONENT_FIELD_FLOAT,
    FloatWidth = TC_COMPONENT_FIELD_FLOAT_WIDTH,
    ColorRgba8 = TC_COMPONENT_FIELD_COLOR_RGBA8,
    Custom = TC_COMPONENT_FIELD_CUSTOM,
};

/* One declared field, owned by the host: the Mod's strings are copied at
   registration time and never read again. */
struct Field {
    std::string id;
    std::string label;
    std::string tooltip;
    uint32_t kind = 0;
    uint32_t flags = 0;
    uint32_t offset = 0;
    uint32_t bytes = 0;
    uint32_t bits = 0;
    std::vector<std::string> choices;
    uint64_t maxUnsigned = 0;   /* the ceiling `bits` implies, before clamping */
    int64_t minValue = 0;
    int64_t maxValue = 0;
    double minFloat = 0.0;
    double maxFloat = 0.0;
    bool hasIntRange = false;
    bool hasFloatRange = false;
    bool readOnly = false;
    bool hex = false;
    /* V2 layout: where the row goes, not what it edits. */
    bool fullRow = false;
    bool inlineWithPrevious = false;
    bool separatorBefore = false;
    bool radio = false;
    bool dropdown = false;
    bool resizeHandle = false;
    /* V4 shapes: how a colour is picked, and how a validation message is shown. */
    bool colorPalette = false;
    bool colorHex = false;
    bool validationIcon = false;
    /* A palette row's swatches, parsed from its colour literals by validate(). */
    std::vector<uint32_t> swatches;
    TCComponentEditorValidateV1 validateValue = nullptr;
    TCComponentEditorCustomRowCallbackV1 drawCustom = nullptr;
    TCComponentEditorActionCallbackV1 action = nullptr;
    void* user = nullptr;
    float customHeight = 0.f;
    std::vector<uint32_t> contextActionIds;
    std::vector<std::string> contextActionLabels;
};

/* One decoded value: which member matters depends on the field's kind. */
struct Value {
    int64_t integer = 0;
    double real = 0.0;
    std::string text;
    bool boolean = false;
};

inline const char* kindName(uint32_t kind) {
    switch (kind) {
        case TC_COMPONENT_FIELD_BOOL: return "bool";
        case TC_COMPONENT_FIELD_INT: return "int";
        case TC_COMPONENT_FIELD_UINT: return "uint";
        case TC_COMPONENT_FIELD_ENUM: return "enum";
        case TC_COMPONENT_FIELD_TEXT: return "text";
        case TC_COMPONENT_FIELD_FLOAT: return "float";
        case TC_COMPONENT_FIELD_FLOAT_WIDTH: return "float-width";
        case TC_COMPONENT_FIELD_COLOR_RGBA8: return "color-rgba8";
        case TC_COMPONENT_FIELD_CUSTOM: return "custom";
        default: return "?";
    }
}

/* The name a log line uses, which for the width-aware float kind also says which
   width the row edits (a Mod's FP16 constant and its FP64 constant declare the
   same kind and must not read alike in the log). */
inline const char* kindName(const Field& field) {
    if (field.kind != TC_COMPONENT_FIELD_FLOAT_WIDTH) return kindName(field.kind);
    return field.bytes == 2 ? "float16" : (field.bytes == 8 ? "float64" : "float32");
}

/* Copy one field out of the Mod's array.  Strings are truncated the way the
   registry does it, so a field is either fully usable or refused. */
inline Field fromDeclared(const TCComponentFieldV1& declared) {
    Field field;
    auto text = [](const char* value, size_t cap) {
        std::string result = value ? value : "";
        if (result.size() > cap) result.resize(cap);
        return result;
    };
    field.id = text(declared.field_id, 63);
    field.label = text(declared.label, 63);
    field.tooltip = text(declared.tooltip, 255);
    field.kind = declared.kind;
    field.flags = declared.flags;
    field.offset = declared.offset;
    field.bytes = declared.bytes;
    field.bits = declared.bits;
    field.minValue = declared.min_value;
    field.maxValue = declared.max_value;
    field.minFloat = declared.min_float;
    field.maxFloat = declared.max_float;
    field.readOnly = (declared.flags & TC_COMPONENT_FIELD_READ_ONLY) != 0;
    field.hex = (declared.flags & TC_COMPONENT_FIELD_HEX) != 0;
    field.fullRow = (declared.flags & TC_COMPONENT_FIELD_FULL_ROW) != 0;
    field.inlineWithPrevious = (declared.flags & TC_COMPONENT_FIELD_INLINE) != 0;
    field.separatorBefore = (declared.flags & TC_COMPONENT_FIELD_SEPARATOR) != 0;
    field.radio = (declared.flags & TC_COMPONENT_FIELD_RADIO) != 0;
    field.dropdown = (declared.flags & TC_COMPONENT_FIELD_DROPDOWN) != 0;
    field.resizeHandle = (declared.flags & TC_COMPONENT_FIELD_RESIZE_HANDLE) != 0;
    field.colorPalette = (declared.flags & TC_COMPONENT_FIELD_COLOR_PALETTE) != 0;
    field.colorHex = (declared.flags & TC_COMPONENT_FIELD_COLOR_HEX) != 0;
    field.validationIcon = (declared.flags & TC_COMPONENT_FIELD_VALIDATION_ICON) != 0;
    /* An enum's choice labels and a palette's colour literals live in the same
       two members - both are "one string per index", and reusing them is what
       keeps the frozen V1 field layout from growing a pointer for V4. */
    const bool wantsLabels =
        declared.kind == TC_COMPONENT_FIELD_ENUM ||
        (declared.kind == TC_COMPONENT_FIELD_COLOR_RGBA8 && field.colorPalette);
    const uint32_t choices = wantsLabels ? declared.choices : 0;
    for (uint32_t index = 0; index < choices; ++index) {
        const char* label = declared.choice_labels ? declared.choice_labels[index] : nullptr;
        field.choices.push_back(text(label, 63));
    }
    return field;
}

/* A colour literal, the way a palette row declares its swatches: "#RRGGBB",
   "#RRGGBBAA" or the three-digit "#RGB" shorthand, with the '#' - or a "0x"
   prefix in its place - optional.  The result is the packed 0xAABBGGRR the
   configuration stores and tc.component.render draws with, so a swatch's colour
   is the same four bytes everything else sees. */
inline bool parseColorLiteral(const std::string& text, uint32_t* out) {
    if (!out) return false;
    size_t at = 0;
    size_t end = text.size();
    while (at < end && (text[at] == ' ' || text[at] == '\t')) ++at;
    while (end > at && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    if (at + 1 < end && text[at] == '0' && (text[at + 1] == 'x' || text[at + 1] == 'X')) at += 2;
    else if (at < end && text[at] == '#') ++at;
    uint32_t digits = 0;
    uint64_t value = 0;
    for (size_t index = at; index < end; ++index) {
        const char c = text[index];
        uint32_t nibble = 0;
        if (c >= '0' && c <= '9') nibble = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') nibble = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nibble = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        if (++digits > 8) return false;
        value = (value << 4) | nibble;
    }
    uint32_t red = 0, green = 0, blue = 0, alpha = 0xff;
    if (digits == 3) {
        red = static_cast<uint32_t>((value >> 8) & 0xfu) * 17u;
        green = static_cast<uint32_t>((value >> 4) & 0xfu) * 17u;
        blue = static_cast<uint32_t>(value & 0xfu) * 17u;
    } else if (digits == 6) {
        red = static_cast<uint32_t>((value >> 16) & 0xffu);
        green = static_cast<uint32_t>((value >> 8) & 0xffu);
        blue = static_cast<uint32_t>(value & 0xffu);
    } else if (digits == 8) {
        red = static_cast<uint32_t>((value >> 24) & 0xffu);
        green = static_cast<uint32_t>((value >> 16) & 0xffu);
        blue = static_cast<uint32_t>((value >> 8) & 0xffu);
        alpha = static_cast<uint32_t>(value & 0xffu);
    } else {
        return false;
    }
    *out = red | (green << 8) | (blue << 16) | (alpha << 24);
    return true;
}

/* The literal form of a packed colour, for a tooltip or a log line.  The alpha
   byte is dropped when it is opaque, so the common case reads "#ff8040". */
inline std::string formatColorLiteral(uint32_t packed) {
    char buffer[16] = {};
    const unsigned alpha = (packed >> 24) & 0xffu;
    if (alpha == 0xffu)
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", packed & 0xffu,
                      (packed >> 8) & 0xffu, (packed >> 16) & 0xffu);
    else
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", packed & 0xffu,
                      (packed >> 8) & 0xffu, (packed >> 16) & 0xffu, alpha);
    return buffer;
}

/* The packed text a COLOR_HEX row shows: the same 0xAABBGGRR the configuration
   holds, padded to eight digits so the bytes do not jump around. */
inline std::string formatColorHex(uint32_t packed) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08x", packed);
    return buffer;
}

inline Field fromDeclared(const TCComponentFieldV2& declared) {
    Field field = fromDeclared(declared.field);
    field.validateValue = declared.validate;
    field.drawCustom = declared.draw_custom;
    field.action = declared.action;
    field.user = declared.user;
    field.customHeight = declared.custom_height;
    const uint32_t count = std::min<uint32_t>(declared.context_actions,
                                              TC_COMPONENT_EDITOR_MAX_CONTEXT_ACTIONS);
    for (uint32_t index = 0; index < count; ++index) {
        if (!declared.context_action_ids || !declared.context_action_labels) break;
        field.contextActionIds.push_back(declared.context_action_ids[index]);
        const char* label = declared.context_action_labels[index];
        std::string copied = label ? label : "";
        if (copied.size() > 63) copied.resize(63);
        field.contextActionLabels.push_back(std::move(copied));
    }
    return field;
}

inline uint64_t widthMask(uint32_t bytes) {
    if (bytes >= 8) return ~uint64_t(0);
    return (uint64_t(1) << (bytes * 8)) - 1;
}

/* The hexadecimal text a HEX field shows, padded to its own width so the digits
   do not jump around while the value changes. */
inline std::string formatHex(const Field& field, int64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%0*llx", static_cast<int>(field.bytes * 2),
                  static_cast<unsigned long long>(static_cast<uint64_t>(value)));
    return buffer;
}

/* Parses what the player typed into a HEX field: an optional 0x/0X prefix and up
   to sixteen hexadecimal digits.  Anything else - an empty box, a stray letter,
   a value wider than 64 bits - is refused, and the caller keeps the old bytes. */
inline bool parseHex(const std::string& text, uint64_t* out) {
    if (!out) return false;
    size_t at = 0;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
    if (at + 1 < text.size() && text[at] == '0' && (text[at + 1] == 'x' || text[at + 1] == 'X'))
        at += 2;
    uint64_t value = 0;
    uint32_t digits = 0;
    for (; at < text.size(); ++at) {
        const char c = text[at];
        uint32_t nibble = 0;
        if (c >= '0' && c <= '9') nibble = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') nibble = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nibble = static_cast<uint32_t>(c - 'A' + 10);
        else if (c == ' ' || c == '\t') break;
        else return false;
        if (++digits > 16) return false;
        value = (value << 4) | nibble;
    }
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
    if (!digits || at != text.size()) return false;
    *out = value;
    return true;
}

inline bool intLimits(uint32_t bytes, int64_t& low, int64_t& high) {
    switch (bytes) {
        case 1: low = -128; high = 127; return true;
        case 2: low = -32768; high = 32767; return true;
        case 4: low = -2147483647LL - 1; high = 2147483647LL; return true;
        default: return false;
    }
}

/* What a COLOR_HEX box accepts: an ordinary hexadecimal word (0xAABBGGRR, the
   way the value reads in the log) or a colour literal (#RRGGBB[AA]).  Anything
   else is refused and the caller keeps the old bytes. */
inline bool parseColorHex(const std::string& text, uint32_t* out) {
    if (!out) return false;
    if (text.find('#') != std::string::npos) return parseColorLiteral(text, out);
    uint64_t parsed = 0;
    if (!parseHex(text, &parsed) || parsed > 0xffffffffull) return false;
    *out = static_cast<uint32_t>(parsed);
    return true;
}

inline bool uintLimit(uint32_t bytes, uint32_t bits, uint64_t& high) {
    if (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) return false;
    const uint32_t width = bits ? bits : bytes * 8;
    if (width == 0 || width > bytes * 8) return false;
    high = width >= 64 ? ~uint64_t(0) : (uint64_t(1) << width) - 1;
    return true;
}

/* ---- the IEEE-754 widths, typed in decimal -------------------------------

   TC_COMPONENT_FIELD_FLOAT_WIDTH edits 2, 4 or 8 bytes of IEEE-754
   binary16/binary32/binary64 in a decimal box.  The host already had a box for
   the four-byte case - TC_COMPONENT_FIELD_FLOAT, ImGui's own float widget - but
   nothing that could *type* a value whose width is not four bytes, which is what
   a Mod's FP16/FP64 constants need.  (A UINT field with the HEX flag was the
   only escape hatch, and nobody wants to type 0x2E66 for 0.1.)

   Two rules keep the row honest:

     * it shows the shortest decimal that parses back to the stored pattern, so a
       value reads the way it reads on the board (binary16's 0.1 shows "0.1", not
       "0.099975586") and a pattern nobody typed - a NaN, a denormal, what a
       migration wrote - still shows something parseable;
     * the pattern is what is stored, and the parse is correctly rounded, so
       "0.1" typed into a binary16 row stores 0x2E66 and shows "0.1" again.

   binary16 has no hardware conversion here, so both directions are written out.
   `halfToDouble` is exact; `doubleToHalf` rounds to nearest, ties to even, and
   overflows to infinity exactly where the format does.  Because double carries
   far more precision than binary16 needs (53 bits against the 2p+2 = 24 the
   double-rounding rule asks for), "decimal -> double -> binary16" rounds exactly
   like "decimal -> binary16". */

inline double halfToDouble(uint16_t bits) {
    const uint32_t sign = static_cast<uint32_t>(bits >> 15) << 31;
    const uint32_t exponent = (bits >> 10) & 0x1Fu;
    uint32_t fraction = bits & 0x3FFu;
    uint32_t single = 0;
    if (exponent == 0) {
        if (fraction == 0) {
            single = sign;                       /* +-0 */
        } else {
            /* A subnormal: shift the fraction up until the implicit one reaches
               bit 10, which costs one exponent step per shift (`-14 - shift` in
               half terms, so `113 - shift` in single terms). */
            int shift = 0;
            while ((fraction & 0x400u) == 0) {
                fraction <<= 1;
                ++shift;
            }
            fraction &= 0x3FFu;
            single = sign | ((113u - static_cast<uint32_t>(shift)) << 23) |
                     (fraction << 13);
        }
    } else if (exponent == 0x1Fu) {
        single = sign | 0x7F800000u | (fraction << 13);   /* infinity or NaN */
    } else {
        single = sign | ((exponent - 15u + 127u) << 23) | (fraction << 13);
    }
    float value = 0.f;
    std::memcpy(&value, &single, sizeof(value));
    return static_cast<double>(value);
}

/* Whether the dropped bits are more than half of the step, or exactly half with
   an odd kept value (round to nearest, ties to even). */
inline bool roundsUp(uint64_t remainder, int dropped, uint64_t kept) {
    if (remainder == 0) return false;
    const uint64_t half = UINT64_C(1) << (dropped - 1);
    if (remainder > half) return true;
    if (remainder < half) return false;
    return (kept & 1u) != 0;
}

inline uint16_t doubleToHalf(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint16_t sign = static_cast<uint16_t>((bits >> 48) & 0x8000u);
    const uint64_t exponent = (bits >> 52) & 0x7FFu;
    const uint64_t fraction = bits & UINT64_C(0xFFFFFFFFFFFFF);
    if (exponent == 0x7FFu) {
        /* Infinity stays infinity; a NaN collapses to the quiet NaN (a payload
           is a bit pattern, and this row's job is a value - the hex row next to
           it is where a payload is typed). */
        return static_cast<uint16_t>(sign | (fraction ? 0x7E00u : 0x7C00u));
    }
    /* A double subnormal is far below binary16's smallest subnormal, so it
       rounds to zero. */
    if (exponent == 0) return sign;
    const int binary_exponent = static_cast<int>(exponent) - 1023;
    const uint64_t significand = fraction | (UINT64_C(1) << 52);
    if (binary_exponent > 15) return static_cast<uint16_t>(sign | 0x7C00u);
    if (binary_exponent >= -14) {
        /* Normal: eleven bits are kept (the implicit one plus ten). */
        constexpr int kDropped = 52 - 10;
        const uint64_t kept = significand >> kDropped;
        const uint64_t remainder = significand & ((UINT64_C(1) << kDropped) - 1);
        uint32_t field = static_cast<uint32_t>(kept - (UINT64_C(1) << 10));
        uint32_t half_exponent = static_cast<uint32_t>(binary_exponent + 15);
        if (roundsUp(remainder, kDropped, kept)) {
            if (++field == 0x400u) {
                field = 0;
                if (++half_exponent >= 0x1Fu) return static_cast<uint16_t>(sign | 0x7C00u);
            }
        }
        return static_cast<uint16_t>(sign | (half_exponent << 10) | field);
    }
    /* Subnormal: the value is shifted right further, and a carry out of the top
       lands on the smallest normal (0x400 = exponent 1, fraction 0). */
    const int dropped = (52 - 10) + (-14 - binary_exponent);
    if (dropped >= 64) return sign;
    const uint64_t kept = significand >> dropped;
    const uint64_t remainder = significand & ((UINT64_C(1) << dropped) - 1);
    uint32_t field = static_cast<uint32_t>(kept);
    if (roundsUp(remainder, dropped, kept)) ++field;
    return static_cast<uint16_t>(sign | field);
}

inline uint64_t floatPatternOf(double value, uint32_t bytes) {
    if (bytes == 2) return doubleToHalf(value);
    if (bytes == 4) {
        const float narrowed = static_cast<float>(value);
        uint32_t bits = 0;
        std::memcpy(&bits, &narrowed, sizeof(bits));
        return bits;
    }
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline double floatValueOf(uint64_t pattern, uint32_t bytes) {
    if (bytes == 2) return halfToDouble(static_cast<uint16_t>(pattern));
    if (bytes == 4) {
        const uint32_t bits = static_cast<uint32_t>(pattern);
        float value = 0.f;
        std::memcpy(&value, &bits, sizeof(value));
        return static_cast<double>(value);
    }
    double value = 0.0;
    std::memcpy(&value, &pattern, sizeof(value));
    return value;
}

/* The shortest `%g` text that parses back to this pattern.  The precision is
   raised one digit at a time - binary16 never needs more than five, binary32
   nine, binary64 seventeen - and the first text that round-trips wins, so a
   value reads as short as it can without lying about which pattern is stored. */
inline std::string formatFloatText(const Field& field, int64_t pattern) {
    const uint64_t bits = static_cast<uint64_t>(pattern) & widthMask(field.bytes);
    const double value = floatValueOf(bits, field.bytes);
    const int max_digits = field.bytes == 2 ? 5 : (field.bytes == 4 ? 9 : 17);
    char buffer[64] = {};
    for (int digits = 1; digits <= max_digits; ++digits) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", digits, value);
        double parsed = 0.0;
        char* end = nullptr;
        parsed = std::strtod(buffer, &end);
        if (end && *end == '\0' && floatPatternOf(parsed, field.bytes) == bits) return buffer;
    }
    /* Unreachable for a finite pattern of any of the three widths (the
       max_digits print always round-trips); kept so a caller can never receive
       an empty string. */
    std::snprintf(buffer, sizeof(buffer), "%.*g", max_digits, value);
    return buffer;
}

/* Parses what the player typed.  Anything strtod does not consume entirely - an
   empty box, a stray letter, "1.2.3" - is refused and the caller keeps the old
   bytes.  `inf`/`infinity`/`nan` (any case, with a sign) are ordinary input to
   strtod and are accepted, and "-0" stores the negative zero.  A value outside
   the field's declared range is clamped, and `note` says so. */
inline bool parseFloatText(const Field& field, const std::string& text, uint64_t* out,
                           std::string* note) {
    if (!out) return false;
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    if (begin == end) return false;
    const std::string trimmed = text.substr(begin, end - begin);
    char* consumed = nullptr;
    double value = std::strtod(trimmed.c_str(), &consumed);
    if (!consumed || consumed == trimmed.c_str() || *consumed != '\0') return false;
    if (field.hasFloatRange) {
        if (value < field.minFloat) {
            if (note)
                *note = "field \"" + field.id + "\" clamped " + trimmed + " to " +
                        formatFloatText(field, static_cast<int64_t>(
                                                    floatPatternOf(field.minFloat, field.bytes)));
            value = field.minFloat;
        } else if (value > field.maxFloat) {
            if (note)
                *note = "field \"" + field.id + "\" clamped " + trimmed + " to " +
                        formatFloatText(field, static_cast<int64_t>(
                                                    floatPatternOf(field.maxFloat, field.bytes)));
            value = field.maxFloat;
        }
    }
    *out = floatPatternOf(value, field.bytes);
    return true;
}

/* Fills in the derived parts (the unsigned ceiling) and answers whether the
   field can be edited at all.  `why` receives a one-line reason for the log. */
inline bool validate(Field& field, uint32_t configBytes, std::string& why,
                     uint32_t knownFlags) {
    auto refuse = [&why](const std::string& reason) {
        why = reason;
        return false;
    };
    why.clear();
    if (field.id.empty()) return refuse("the field has no id");
    if (field.kind != TC_COMPONENT_FIELD_CUSTOM && field.bytes == 0)
        return refuse("field \"" + field.id + "\" declares no width");
    if (field.flags & ~knownFlags)
        return refuse("field \"" + field.id + "\" carries unknown flags");
    if (field.radio && field.kind != TC_COMPONENT_FIELD_ENUM)
        return refuse("field \"" + field.id + "\" asks for radio buttons but is not an enum");
    if (field.dropdown && field.kind != TC_COMPONENT_FIELD_ENUM)
        return refuse("field \"" + field.id + "\" asks for a dropdown but is not an enum");
    if (field.dropdown && field.radio)
        return refuse("field \"" + field.id + "\" asks for both radio and dropdown enum shapes");
    if (field.resizeHandle && field.kind != TC_COMPONENT_FIELD_CUSTOM)
        return refuse("field \"" + field.id + "\" asks for a resize handle but is not custom");
    if (field.colorPalette && field.kind != TC_COMPONENT_FIELD_COLOR_RGBA8)
        return refuse("field \"" + field.id + "\" declares a swatch palette but is not a colour");
    if (field.colorHex && field.kind != TC_COMPONENT_FIELD_COLOR_RGBA8)
        return refuse("field \"" + field.id + "\" asks for a colour hex box but is not a colour");
    if ((!field.contextActionIds.empty() || field.drawCustom || field.resizeHandle) &&
        field.kind != TC_COMPONENT_FIELD_CUSTOM)
        return refuse("field \"" + field.id + "\" attaches custom-row behavior to a value field");
    if (field.contextActionIds.size() != field.contextActionLabels.size())
        return refuse("field \"" + field.id + "\" has an incomplete context action table");
    for (size_t index = 0; index < field.contextActionIds.size(); ++index) {
        if (!field.contextActionIds[index] || field.contextActionLabels[index].empty())
            return refuse("field \"" + field.id + "\" has an empty context action");
        for (size_t earlier = 0; earlier < index; ++earlier)
            if (field.contextActionIds[earlier] == field.contextActionIds[index])
                return refuse("field \"" + field.id + "\" repeats a context action id");
    }
    if (field.kind == TC_COMPONENT_FIELD_CUSTOM &&
        (!std::isfinite(field.customHeight) || field.customHeight < 0.f ||
         field.customHeight > 4096.f))
        return refuse("field \"" + field.id + "\" has an invalid custom height");
    if (static_cast<uint64_t>(field.offset) + field.bytes > configBytes)
        return refuse("field \"" + field.id + "\" reaches past the configuration");
    switch (field.kind) {
        case TC_COMPONENT_FIELD_BOOL:
            if (field.bytes != 1) return refuse("field \"" + field.id + "\" is a bool that is not one byte");
            return true;
        case TC_COMPONENT_FIELD_INT: {
            int64_t low = 0, high = 0;
            if (!intLimits(field.bytes, low, high))
                return refuse("field \"" + field.id + "\" is an integer of " +
                              std::to_string(field.bytes) + " bytes");
            field.hasIntRange = field.minValue < field.maxValue;
            if (field.hasIntRange &&
                (field.minValue < low || field.maxValue > high))
                return refuse("field \"" + field.id + "\" has a range outside its width");
            return true;
        }
        case TC_COMPONENT_FIELD_UINT: {
            if (field.hex ? (field.bytes != 1 && field.bytes != 2 && field.bytes != 4 &&
                             field.bytes != 8)
                          : !uintLimit(field.bytes, field.bits, field.maxUnsigned))
                return refuse("field \"" + field.id + "\" is an unsigned integer of " +
                              std::to_string(field.bytes) + " bytes / " +
                              std::to_string(field.bits) + " bits");
            if (field.hex) {
                if (!uintLimit(field.bytes, field.bits, field.maxUnsigned) ||
                    (field.bits ? field.bits : field.bytes * 8) > kMaxHexBits)
                    return refuse("field \"" + field.id + "\" declares more than " +
                                  std::to_string(kMaxHexBits) + " bits");
            } else if ((field.bits ? field.bits : field.bytes * 8) > kMaxUintBits) {
                return refuse("field \"" + field.id + "\" declares more than " +
                              std::to_string(kMaxUintBits) + " bits");
            }
            field.hasIntRange = field.minValue != 0 || field.maxValue != 0;
            if (field.hasIntRange) {
                if (field.minValue < 0 || field.maxValue < field.minValue ||
                    static_cast<uint64_t>(field.maxValue) > field.maxUnsigned)
                    return refuse("field \"" + field.id + "\" has a range outside its bits");
            }
            return true;
        }
        case TC_COMPONENT_FIELD_ENUM: {
            if (field.bytes != 1 && field.bytes != 2 && field.bytes != 4)
                return refuse("field \"" + field.id + "\" is an enum of " +
                              std::to_string(field.bytes) + " bytes");
            if (field.choices.empty())
                return refuse("field \"" + field.id + "\" is an enum with no choices");
            const uint64_t ceiling = widthMask(field.bytes);
            if (field.choices.size() > ceiling)
                return refuse("field \"" + field.id + "\" has more choices than its width holds");
            return true;
        }
        case TC_COMPONENT_FIELD_TEXT:
            if (field.bytes < 2 || field.bytes > kMaxTextBytes)
                return refuse("field \"" + field.id + "\" declares " +
                              std::to_string(field.bytes) + " text bytes");
            return true;
        case TC_COMPONENT_FIELD_FLOAT:
            if (field.bytes != 4)
                return refuse("field \"" + field.id + "\" is a float that is not four bytes");
            field.hasFloatRange = field.minFloat < field.maxFloat;
            return true;
        case TC_COMPONENT_FIELD_FLOAT_WIDTH:
            if (field.bytes != 2 && field.bytes != 4 && field.bytes != 8)
                return refuse("field \"" + field.id + "\" is a float of " +
                              std::to_string(field.bytes) + " bytes");
            field.hasFloatRange = field.minFloat < field.maxFloat;
            return true;
        case TC_COMPONENT_FIELD_COLOR_RGBA8:
            if (field.bytes != 4)
                return refuse("field \"" + field.id + "\" is a color that is not four bytes");
            /* A palette is a list of swatches, so its colour literals are parsed
               here rather than at draw time: a typo refuses the whole table
               instead of painting a black square nobody can explain. */
            field.swatches.clear();
            if (field.colorPalette) {
                if (field.choices.empty())
                    return refuse("field \"" + field.id + "\" declares a palette with no swatches");
                if (field.choices.size() > TC_COMPONENT_EDITOR_MAX_PALETTE_SWATCHES)
                    return refuse("field \"" + field.id + "\" declares " +
                                  std::to_string(field.choices.size()) +
                                  " swatches, more than the host draws");
                for (const std::string& label : field.choices) {
                    uint32_t packed = 0;
                    if (!parseColorLiteral(label, &packed))
                        return refuse("field \"" + field.id +
                                      "\" has a swatch that is not a colour (\"" + label + "\")");
                    field.swatches.push_back(packed);
                }
            }
            return true;
        case TC_COMPONENT_FIELD_CUSTOM:
            return true;
        default:
            return refuse("field \"" + field.id + "\" has an unknown kind " +
                          std::to_string(field.kind));
    }
}

/* Little-endian on purpose: the configuration blob is a byte layout, not a
   C struct, so the offline tests do not depend on the host's byte order. */
inline uint64_t readLittle(const uint8_t* bytes, uint32_t width) {
    uint64_t value = 0;
    for (uint32_t index = 0; index < width && index < 8; ++index)
        value |= uint64_t(bytes[index]) << (index * 8);
    return value;
}

inline void writeLittle(uint8_t* bytes, uint32_t width, uint64_t value) {
    for (uint32_t index = 0; index < width && index < 8; ++index)
        bytes[index] = static_cast<uint8_t>((value >> (index * 8)) & 0xffu);
}

inline int64_t signExtend(uint64_t value, uint32_t width) {
    if (width == 0 || width >= 8) return static_cast<int64_t>(value);
    const uint64_t sign = uint64_t(1) << (width * 8 - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

/* Table-level rules: the ones a field cannot check on its own.  A row that
   shares a line needs a row to share it with, and the rows that take the whole
   line (a full-width control, a radio group) end it - so nothing may be declared
   inline after them, and at most one row may share a line (three columns would
   run past the window). */
inline bool validateTable(std::vector<Field>& fields, uint32_t configBytes, std::string& why,
                          uint32_t knownFlags) {
    for (size_t index = 0; index < fields.size(); ++index) {
        if (!validate(fields[index], configBytes, why, knownFlags)) return false;
        if (!fields[index].inlineWithPrevious) continue;
        if (index == 0) {
            why = "field \"" + fields[index].id + "\" shares a line but is the first row";
            return false;
        }
        const Field& previous = fields[index - 1];
        if (previous.fullRow || previous.radio || previous.kind == TC_COMPONENT_FIELD_CUSTOM) {
            why = "field \"" + fields[index].id + "\" shares a line with \"" + previous.id +
                  "\", which takes the whole line";
            return false;
        }
        if (previous.inlineWithPrevious) {
            why = "field \"" + fields[index].id + "\" shares a line with \"" + previous.id +
                  "\", which already shares one";
            return false;
        }
    }
    return true;
}

/* The cheap overload: everything the current ABI knows.  A caller that has to
   keep an older flag set (the V1/V2 entry point) passes its own. */
inline bool validate(Field& field, uint32_t configBytes, std::string& why) {
    return validate(field, configBytes, why, TC_COMPONENT_FIELD_KNOWN_FLAGS);
}

inline bool validateTable(std::vector<Field>& fields, uint32_t configBytes, std::string& why) {
    return validateTable(fields, configBytes, why, TC_COMPONENT_FIELD_KNOWN_FLAGS);
}

/* Table-level flags, checked the same way field flags are: unknown bits refuse
   the table, and so does a combination the host cannot honour.  CONFIRM stages
   the value rows in the host's working copy and writes them on Apply, while a
   CUSTOM row's callback writes the configuration itself through
   tc.component.storage - two owners for one blob, so the pair is refused. */
inline bool validateTableFlags(uint32_t tableFlags, const std::vector<Field>& fields,
                               std::string& why) {
    why.clear();
    if (tableFlags & ~TC_COMPONENT_EDITOR_TABLE_KNOWN_FLAGS) {
        why = "the table carries unknown flags";
        return false;
    }
    if (tableFlags & TC_COMPONENT_EDITOR_TABLE_CONFIRM) {
        for (const Field& field : fields) {
            if (field.kind != TC_COMPONENT_FIELD_CUSTOM) continue;
            why = "the table stages its edits (CONFIRM) and also declares the custom row \"" +
                  field.id + "\", which writes the configuration itself";
            return false;
        }
    }
    return true;
}

/* Decodes the field's current value.  The caller guarantees the field was
   validated against a configuration of `configBytes` bytes. */
inline bool read(const Field& field, const uint8_t* config, uint32_t configBytes, Value& out) {
    if (!config || static_cast<uint64_t>(field.offset) + field.bytes > configBytes) return false;
    const uint8_t* at = config + field.offset;
    switch (field.kind) {
        case TC_COMPONENT_FIELD_BOOL:
            out.boolean = at[0] != 0;
            out.integer = out.boolean ? 1 : 0;
            return true;
        case TC_COMPONENT_FIELD_INT:
            out.integer = signExtend(readLittle(at, field.bytes), field.bytes);
            return true;
        case TC_COMPONENT_FIELD_UINT:
        case TC_COMPONENT_FIELD_ENUM:
        case TC_COMPONENT_FIELD_COLOR_RGBA8:
            out.integer = static_cast<int64_t>(readLittle(at, field.bytes));
            if (field.kind == TC_COMPONENT_FIELD_ENUM && out.integer >= 0 &&
                static_cast<size_t>(out.integer) < field.choices.size())
                out.text = field.choices[static_cast<size_t>(out.integer)];
            return true;
        case TC_COMPONENT_FIELD_FLOAT: {
            const uint32_t bits = static_cast<uint32_t>(readLittle(at, 4));
            float value = 0.f;
            std::memcpy(&value, &bits, sizeof(value));
            out.real = value;
            return true;
        }
        case TC_COMPONENT_FIELD_FLOAT_WIDTH: {
            /* The row's text carries the decimal form; `integer` carries the raw
               pattern, the same way a hexadecimal UINT field does, so the caller
               can commit it back without a second conversion. */
            const uint64_t bits = readLittle(at, field.bytes) & widthMask(field.bytes);
            out.integer = static_cast<int64_t>(bits);
            out.real = floatValueOf(bits, field.bytes);
            out.text = formatFloatText(field, out.integer);
            return true;
        }
        case TC_COMPONENT_FIELD_TEXT: {
            const char* text = reinterpret_cast<const char*>(at);
            size_t length = 0;
            while (length < field.bytes && text[length]) ++length;
            out.text.assign(text, length);
            return true;
        }
        case TC_COMPONENT_FIELD_CUSTOM:
            return true;
        default:
            return false;
    }
}

/* Encodes `in` into the field's bytes, clamping to the declared range first.
   `note` (optional) receives a line when a value had to be clamped. */
inline bool apply(const Field& field, uint8_t* config, uint32_t configBytes, const Value& in,
                  std::string* note) {
    if (!config || static_cast<uint64_t>(field.offset) + field.bytes > configBytes) return false;
    uint8_t* at = config + field.offset;
    switch (field.kind) {
        case TC_COMPONENT_FIELD_BOOL:
            at[0] = in.boolean ? 1 : 0;
            return true;
        case TC_COMPONENT_FIELD_INT: {
            int64_t low = 0, high = 0;
            intLimits(field.bytes, low, high);
            if (field.hasIntRange) {
                if (field.minValue > low) low = field.minValue;
                if (field.maxValue < high) high = field.maxValue;
            }
            int64_t value = in.integer;
            if (value < low || value > high) {
                const int64_t clamped = value < low ? low : high;
                if (note)
                    *note = "field \"" + field.id + "\" clamped " + std::to_string(value) +
                            " to " + std::to_string(clamped);
                value = clamped;
            }
            writeLittle(at, field.bytes, static_cast<uint64_t>(value));
            return true;
        }
        case TC_COMPONENT_FIELD_UINT: {
            uint64_t high = widthMask(field.bytes);
            if (field.bits) high = (uint64_t(1) << field.bits) - 1;
            uint64_t low = 0;
            if (field.hasIntRange) {
                low = static_cast<uint64_t>(field.minValue);
                if (static_cast<uint64_t>(field.maxValue) < high)
                    high = static_cast<uint64_t>(field.maxValue);
            }
            uint64_t value = static_cast<uint64_t>(in.integer);
            if (value < low || value > high) {
                const uint64_t clamped = value < low ? low : high;
                if (note)
                    *note = "field \"" + field.id + "\" clamped " + std::to_string(value) +
                            " to " + std::to_string(clamped);
                value = clamped;
            }
            writeLittle(at, field.bytes, value);
            return true;
        }
        case TC_COMPONENT_FIELD_ENUM: {
            uint64_t index = static_cast<uint64_t>(in.integer);
            if (index >= field.choices.size()) {
                const uint64_t clamped = field.choices.empty() ? 0 : field.choices.size() - 1;
                if (note)
                    *note = "field \"" + field.id + "\" clamped an enum index to " +
                            std::to_string(clamped);
                index = clamped;
            }
            writeLittle(at, field.bytes, index);
            return true;
        }
        case TC_COMPONENT_FIELD_COLOR_RGBA8:
            writeLittle(at, 4, static_cast<uint64_t>(in.integer));
            return true;
        case TC_COMPONENT_FIELD_FLOAT: {
            double value = in.real;
            if (field.hasFloatRange) {
                if (value < field.minFloat) {
                    if (note) *note = "field \"" + field.id + "\" clamped " + std::to_string(value) +
                                      " to " + std::to_string(field.minFloat);
                    value = field.minFloat;
                } else if (value > field.maxFloat) {
                    if (note) *note = "field \"" + field.id + "\" clamped " + std::to_string(value) +
                                      " to " + std::to_string(field.maxFloat);
                    value = field.maxFloat;
                }
            }
            const float narrowed = static_cast<float>(value);
            uint32_t bits = 0;
            std::memcpy(&bits, &narrowed, sizeof(bits));
            writeLittle(at, 4, bits);
            return true;
        }
        case TC_COMPONENT_FIELD_TEXT: {
            std::string value = in.text;
            if (value.size() >= field.bytes) {
                value.resize(field.bytes - 1);
                if (note) *note = "field \"" + field.id + "\" text truncated to " +
                                  std::to_string(field.bytes - 1) + " bytes";
            }
            std::memset(at, 0, field.bytes);
            std::memcpy(at, value.data(), value.size());
            return true;
        }
        case TC_COMPONENT_FIELD_FLOAT_WIDTH:
            /* The value arrived as a pattern (parseFloatText produced it), so
               this is a write, not a conversion - a caller that skipped the
               parse cannot silently store a different number. */
            writeLittle(at, field.bytes,
                        static_cast<uint64_t>(in.integer) & widthMask(field.bytes));
            return true;
        default:
            return false;
    }
}

/* The text a read-only row (or a log line) shows for a value. */
inline std::string display(const Field& field, const Value& value) {
    char buffer[96];
    switch (field.kind) {
        case TC_COMPONENT_FIELD_BOOL:
            return value.boolean ? "on" : "off";
        case TC_COMPONENT_FIELD_INT:
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value.integer));
            return buffer;
        case TC_COMPONENT_FIELD_UINT:
            if (field.hex)
                std::snprintf(buffer, sizeof(buffer), "0x%llx",
                              static_cast<unsigned long long>(value.integer));
            else
                std::snprintf(buffer, sizeof(buffer), "%llu",
                              static_cast<unsigned long long>(value.integer));
            return buffer;
        case TC_COMPONENT_FIELD_ENUM:
            if (value.integer >= 0 && static_cast<size_t>(value.integer) < field.choices.size())
                return field.choices[static_cast<size_t>(value.integer)];
            std::snprintf(buffer, sizeof(buffer), "#%lld", static_cast<long long>(value.integer));
            return buffer;
        case TC_COMPONENT_FIELD_FLOAT:
            std::snprintf(buffer, sizeof(buffer), "%.7g", value.real);
            return buffer;
        case TC_COMPONENT_FIELD_FLOAT_WIDTH:
            return formatFloatText(field, value.integer);
        case TC_COMPONENT_FIELD_TEXT:
            return value.text;
        case TC_COMPONENT_FIELD_COLOR_RGBA8:
            /* A literal, not the raw word: "#ff8040" reads as a colour to the
               player and to the log, while the row's hex box is where the exact
               four bytes are typed. */
            return formatColorLiteral(static_cast<uint32_t>(value.integer));
        case TC_COMPONENT_FIELD_CUSTOM:
            return "";
        default:
            return "";
    }
}

/* The next enum choice, wrapping - what clicking the row's value does. */
inline int64_t nextChoice(const Field& field, int64_t current) {
    if (field.choices.empty()) return 0;
    const int64_t size = static_cast<int64_t>(field.choices.size());
    if (current < 0 || current >= size) return 0;
    return (current + 1) % size;
}

}  // namespace tc::component_editor
